#include "JellyfinClient.h"
#include <network.h>
#include <ogc/if_config.h>
#include <ogc/lwp_watchdog.h>
#include <sys/filio.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <vector>

#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/x509_crt.h>

extern unsigned char data_cacert_pem[];
extern unsigned int  data_cacert_pem_len;

#include "../version.h"
#define WIIFIN_CLIENT_HDR "MediaBrowser Client=\"WiiFin\", Device=\"Nintendo Wii\", DeviceId=\"wiifin-wii\", Version=\"" WIIFIN_VERSION "\""

#include <ogcsys.h>

// ---------------------------------------------------------------------------
// mbedTLS BIO callbacks: wrap libogc net_read / net_write
// ---------------------------------------------------------------------------
namespace {

int wii_tls_send(void* ctx, const unsigned char* buf, size_t len) {
    s32 fd = *(s32*)ctx;
    int ret = net_write(fd, (void*)buf, (u32)len);
    if (ret < 0) {
        if (ret == -EAGAIN || ret == -EWOULDBLOCK) {
            // Wait for send buffer via net_select instead of usleep
            // (IOS truncates usleep to ~1ms regardless of requested value)
            fd_set wfds;
            FD_ZERO(&wfds); FD_SET(fd, &wfds);
            struct timeval tv = {0, 5000};
            net_select(fd + 1, nullptr, &wfds, nullptr, &tv);
            return MBEDTLS_ERR_SSL_WANT_WRITE;
        }
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    return ret;
}

int wii_tls_recv(void* ctx, unsigned char* buf, size_t len) {
    s32 fd = *(s32*)ctx;
    /* 15 s per-read timeout — prevents a hung server from blocking forever.
     * net_select returns 0 on timeout, <0 on error; both mean no data. */
    {
        fd_set rfds; FD_ZERO(&rfds); FD_SET(fd, &rfds);
        struct timeval stv = {15, 0};
        if (net_select(fd + 1, &rfds, nullptr, nullptr, &stv) <= 0)
            return MBEDTLS_ERR_NET_RECV_FAILED; /* timeout */
    }
    int ret = net_read(fd, buf, (u32)len);
    if (ret < 0) {
        if (ret == -EAGAIN || ret == -EWOULDBLOCK)
            return MBEDTLS_ERR_SSL_WANT_READ;
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
    return ret;
}

} // namespace

// ---------------------------------------------------------------------------

bool JellyfinClient::initNetwork() {
    if (networkReady) return true;
    char ip[16], mask[16], gw[16];
    s32 ret = if_config(ip, mask, gw, true, 20);
    if (ret < 0) {
        errMsg = "Network init failed";
        return false;
    }
    networkReady = true;
    localIp_   = ip;
    localMask_ = mask;
    return true;
}

bool JellyfinClient::discoverServers(std::vector<DiscoveredServer>& out) {
    out.clear();

    s32 sock = net_socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        errMsg = "UDP socket failed";
        return false;
    }

    // Enable broadcast
    u32 broadcastOn = 1;
    net_setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcastOn, sizeof(broadcastOn));

    // Build the two broadcast targets:
    //  1) 255.255.255.255 (limited broadcast)
    //  2) Subnet-directed broadcast — more reliable on some routers/IOS
    struct sockaddr_in dest255, destSubnet;
    memset(&dest255,   0, sizeof(dest255));
    memset(&destSubnet, 0, sizeof(destSubnet));

    dest255.sin_family      = AF_INET;
    dest255.sin_port        = htons(7359);
    dest255.sin_addr.s_addr = 0xFFFFFFFFu;

    destSubnet.sin_family = AF_INET;
    destSubnet.sin_port   = htons(7359);
    // Compute (ip & mask) | ~mask — fall back gracefully if IP unknown
    if (!localIp_.empty() && !localMask_.empty()) {
        u32 ip4   = ntohl(inet_addr(localIp_.c_str()));
        u32 mask4 = ntohl(inet_addr(localMask_.c_str()));
        if (ip4 != 0xFFFFFFFFu && mask4 != 0xFFFFFFFFu)
            destSubnet.sin_addr.s_addr = htonl((ip4 & mask4) | (~mask4));
        else
            destSubnet.sin_addr.s_addr = 0xFFFFFFFFu;
    } else {
        destSubnet.sin_addr.s_addr = 0xFFFFFFFFu;
    }

    SYS_Report("[Discover] localIp=%s\n", localIp_.c_str());

    const char* msg = "Who is JellyfinServer?";
    auto sendBroadcast = [&]() {
        net_sendto(sock, msg, (s32)strlen(msg), 0,
                   (struct sockaddr*)&dest255,   (socklen_t)sizeof(dest255));
        net_sendto(sock, msg, (s32)strlen(msg), 0,
                   (struct sockaddr*)&destSubnet, (socklen_t)sizeof(destSubnet));
    };
    sendBroadcast();

    // Time-based collection using gettime() — independent of net_select resolution.
    // • Exit 100 ms after first server found (LAN responses are nearly instantaneous).
    // • Hard cap: 2000 ms if nothing found.
    // • Re-broadcast at 800 ms to catch slow responders.
    const u32 MAX_MS   = 2000;
    const u32 FOUND_MS = 100;
    u64 startTicks = gettime();
    u64 foundTicks = 0;
    bool rebroadcasted = false;
    char buf[1024];

    while (true) {
        u32 elapsedMs = (u32)ticks_to_millisecs(gettime() - startTicks);
        if (elapsedMs >= MAX_MS) break;
        if (foundTicks > 0 && (u32)ticks_to_millisecs(gettime() - foundTicks) >= FOUND_MS)
            break;

        if (!rebroadcasted && elapsedMs >= 800) {
            sendBroadcast();
            rebroadcasted = true;
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);
        struct timeval tv = {0, 50000};
        int sel = net_select(sock + 1, &rfds, nullptr, nullptr, &tv);
        if (sel <= 0) {
            // Timeout with no data: if we already have results, stop now
            if (!out.empty()) break;
            continue;
        }

        struct sockaddr_in from;
        socklen_t fromLen = (socklen_t)sizeof(from);
        int n = (int)net_recvfrom(sock, buf, (s32)(sizeof(buf) - 1), 0,
                                  (struct sockaddr*)&from, &fromLen);
        if (n <= 0) continue;
        buf[n] = '\0';

        std::string json(buf, n);
        std::string address = jsonGetString(json, "Address");
        std::string name    = jsonGetString(json, "Name");
        if (address.empty()) continue;

        bool dup = false;
        for (const auto& s : out)
            if (s.address == address) { dup = true; break; }
        if (!dup) {
            DiscoveredServer ds;
            ds.name    = name.empty() ? address : name;
            ds.address = address;
            out.push_back(ds);
            if (foundTicks == 0) foundTicks = gettime();
        }
    }

    SYS_Report("[Discover] done: %d servers\n", (int)out.size());
    net_close(sock);
    return true;
}

/* Normalise a URL scheme to lowercase so all comparisons can be case-sensitive.
 * Handles HTTP://, HTTPS://, Http://, etc. typed on external keyboards. */
static std::string normScheme(const std::string& url) {
    // Scheme is everything before the first ':' — at most 8 chars for http/https.
    size_t i = 0;
    while (i < url.size() && i < 8 && url[i] != ':') ++i;
    if (i < url.size() && url[i] == ':') {
        std::string out = url;
        for (size_t j = 0; j < i; ++j)
            out[j] = (char)tolower((unsigned char)out[j]);
        return out;
    }
    return url; // no ':' found before 8 chars — no scheme present
}

/* Extract the explicit port from the host:port portion of a scheme-less URL.
 * Returns -1 if no colon is found (no explicit port in the URL). */
static int extractPort(const std::string& url) {
    size_t slash = url.find('/');
    const std::string hostport = (slash != std::string::npos) ? url.substr(0, slash) : url;
    size_t colon = hostport.rfind(':');
    if (colon == std::string::npos) return -1;
    return atoi(hostport.c_str() + colon + 1);
}

/* Returns true when a scheme-less URL should use HTTPS.
 * No explicit port  → HTTPS:443 (most public Jellyfin servers use HTTPS).
 * Port 443 or 8920  → HTTPS (8920 is Jellyfin's built-in HTTPS port).
 * Any other port    → HTTP (e.g. :8096 / :80 / custom HTTP port).
 * Users can always override by typing http:// or https:// explicitly. */
static bool schemeAutoHttps(const std::string& url) {
    int p = extractPort(url);
    if (p < 0)  return true;          // no explicit port → default HTTPS
    return p == 443 || p == 8920;
}

/* Ensure a server URL has an http:// or https:// scheme prefix. */
static std::string addScheme(const std::string& url) {
    const std::string n = normScheme(url);
    if (n.size() >= 7 && n.compare(0, 7, "http://")  == 0) return n;
    if (n.size() >= 8 && n.compare(0, 8, "https://") == 0) return n;
    return (schemeAutoHttps(n) ? "https://" : "http://") + n;
}

bool JellyfinClient::parseUrl(const std::string& rawUrl,
                               std::string& host, int& port,
                               std::string& basePath, bool& isHttps) {
    std::string u = normScheme(rawUrl);
    if (u.substr(0, 8) == "https://") {
        isHttps = true;
        u = u.substr(8);
        port = 443;
    } else if (u.substr(0, 7) == "http://") {
        isHttps = false;
        u = u.substr(7);
        port = 80;
    } else {
        // No scheme — auto-detect via exact port number (not substring match).
        // Port 443 = standard HTTPS; 8920 = Jellyfin default HTTPS port.
        isHttps = schemeAutoHttps(u);
        port = isHttps ? 443 : 80;
    }

    size_t slash = u.find('/');
    std::string hostport = (slash != std::string::npos) ? u.substr(0, slash) : u;
    basePath = (slash != std::string::npos) ? u.substr(slash) : "/";

    // Collapse multiple leading slashes (e.g. "//QuickConnect" → "/QuickConnect")
    // which happen when serverUrl has a trailing slash and caller appends "/Path"
    while (basePath.size() > 1 && basePath[0] == '/' && basePath[1] == '/')
        basePath.erase(1, 1);

    // Strip trailing slash so callers can safely append "/Path"
    while (basePath.size() > 1 && basePath.back() == '/')
        basePath.pop_back();

    size_t colon = hostport.find(':');
    if (colon != std::string::npos) {
        host = hostport.substr(0, colon);
        port = atoi(hostport.substr(colon + 1).c_str());
    } else {
        host = hostport;
        // port already set to default above
    }
    return true;
}

// Decode JSON string escape sequences (\uXXXX, \", \\, etc.) to UTF-8.
static std::string decodeJsonString(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char nc = s[++i];
            switch (nc) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'n':              break; // skip literal newlines in names
                case 'r':              break;
                case 't':  out += ' '; break;
                case 'u': {
                    if (i + 4 < s.size()) {
                        char hex[5] = { s[i+1], s[i+2], s[i+3], s[i+4], '\0' };
                        unsigned long cp = strtoul(hex, nullptr, 16);
                        i += 4;
                        if      (cp == 0x00A0)                    out += ' ';   // NBSP
                        else if (cp == 0x2013 || cp == 0x2014)    out += '-';   // en/em dash
                        else if (cp == 0x2018 || cp == 0x2019)    out += '\'';
                        else if (cp == 0x201C || cp == 0x201D)    out += '"';
                        else if (cp == 0x2026)                  { out += "..."; }
                        else if (cp < 0x80) {
                            out += (char)cp;
                        } else if (cp < 0x800) {
                            out += (char)(0xC0 | (cp >> 6));
                            out += (char)(0x80 | (cp & 0x3F));
                        } else if (cp < 0x10000) {
                            out += (char)(0xE0 | (cp >> 12));
                            out += (char)(0x80 | ((cp >> 6) & 0x3F));
                            out += (char)(0x80 | (cp & 0x3F));
                        }
                    }
                    break;
                }
                default: out += nc; break;
            }
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string JellyfinClient::jsonGetString(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos += search.size();
    // Scan for unescaped closing quote
    std::string raw;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            raw += '\\';           // keep escape prefix for decoder
            raw += json[pos + 1];
            pos += 2;
        } else {
            raw += json[pos++];
        }
    }
    return decodeJsonString(raw);
}

bool JellyfinClient::jsonGetBool(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return false;
    pos += search.size();
    while (pos < json.size() && json[pos] == ' ') pos++;
    return json.substr(pos, 4) == "true";
}

int JellyfinClient::jsonGetInt(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return 0;
    pos += search.size();
    while (pos < json.size() && json[pos] == ' ') pos++;
    if (pos >= json.size()) return 0;
    return atoi(json.c_str() + pos);
}

long long JellyfinClient::jsonGetLongLong(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return 0;
    pos += search.size();
    while (pos < json.size() && json[pos] == ' ') pos++;
    if (pos >= json.size()) return 0;
    return atoll(json.c_str() + pos);
}

// Walk "Items":[{...},{...}] inside a JSON string.
// Calls callback(objectString) for each top-level object.
namespace {
void forEachItemObject(const std::string& json,
                       void (*cb)(const std::string&, void*), void* ctx) {
    size_t pos = json.find("\"Items\":");
    if (pos == std::string::npos) return;
    pos = json.find('[', pos);
    if (pos == std::string::npos) return;
    pos++; // skip '['

    while (pos < json.size()) {
        // skip to next '{'
        while (pos < json.size() && json[pos] != '{' && json[pos] != ']') pos++;
        if (pos >= json.size() || json[pos] == ']') break;
        // find matching '}' — must skip string literals to avoid counting
        // '{'/'}' that appear inside BlurHash values or other string fields
        int depth = 0;
        bool inStr = false;
        size_t objStart = pos;
        for (; pos < json.size(); pos++) {
            char c = json[pos];
            if (inStr) {
                if      (c == '\\') { pos++; } // skip escaped character
                else if (c == '"')  { inStr = false; }
            } else {
                if      (c == '"') { inStr = true; }
                else if (c == '{') { depth++; }
                else if (c == '}') { if (--depth == 0) { pos++; break; } }
            }
        }
        cb(json.substr(objStart, pos - objStart), ctx);
    }
}
} // namespace

// Decode HTTP chunked transfer-encoded body in-place.
// Returns the decoded content, or the input unchanged if it is not chunked.
static std::string decodeChunked(const char* headers, int headersLen,
                                   const char* body, int bodyLen) {
    // Only apply if the response headers actually say chunked.
    {
        const char* needle = "transfer-encoding: chunked";
        int nlen = (int)strlen(needle);
        bool chunked = false;
        for (int i = 0; i + nlen <= headersLen && !chunked; i++) {
            bool match = true;
            for (int j = 0; j < nlen && match; j++) {
                char c = headers[i + j];
                if (c >= 'A' && c <= 'Z') c += 32;
                if (c != needle[j]) match = false;
            }
            if (match) chunked = true;
        }
        if (!chunked)
            return std::string(body, (size_t)bodyLen);
    }
    std::string out;
    out.reserve((size_t)bodyLen);
    int pos = 0;
    while (pos < bodyLen) {
        // Find end of chunk-size line
        int crlf = pos;
        while (crlf + 1 < bodyLen && !(body[crlf] == '\r' && body[crlf + 1] == '\n')) crlf++;
        if (crlf + 1 >= bodyLen) break;
        // Parse hex chunk size (ignore chunk extensions after ';')
        size_t chunkSize = 0;
        for (int i = pos; i < crlf; i++) {
            char c = body[i];
            if (c == ';') break; // chunk extension
            unsigned digit;
            if (c >= '0' && c <= '9')      digit = (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') digit = (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') digit = (unsigned)(c - 'A' + 10);
            else break;
            chunkSize = chunkSize * 16 + digit;
        }
        pos = crlf + 2; // skip \r\n after size
        if (chunkSize == 0) break; // last chunk
        if (pos + (int)chunkSize > bodyLen) break; // truncated
        out.append(body + pos, chunkSize);
        pos += (int)chunkSize + 2; // skip chunk data + trailing \r\n
    }
    return out;
}

// ---------------------------------------------------------------------------
// Plain HTTP over raw TCP
// ---------------------------------------------------------------------------
int JellyfinClient::httpRequest(const std::string& url,
                                 const std::string& method,
                                 const std::string& contentType,
                                 const std::string& body,
                                 const std::string& authToken,
                                 std::string& responseBody) {
    std::string host, basePath;
    int port;
    bool isHttps;
    if (!parseUrl(url, host, port, basePath, isHttps)) return -1;

    if (isHttps) {
        return httpsRequest(host, port, basePath, method, contentType, body, authToken, responseBody);
    }

    // Resolve host — try as a numeric IPv4 literal first; fall back to DNS.
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_aton(host.c_str(), &addr.sin_addr) == 0) {
        struct hostent* he = net_gethostbyname(host.c_str());
        if (!he) { errMsg = "DNS failed: " + host; return -1; }
        if (he->h_length > (int)sizeof(addr.sin_addr)) { errMsg = "DNS: unexpected address length"; return -1; }
        memcpy(&addr.sin_addr, he->h_addr, he->h_length);
    }

    s32 sock = net_socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { errMsg = "socket() failed"; return -1; }

    {
        int cr = net_connect(sock, (struct sockaddr*)&addr, sizeof(addr));
        if (cr < 0) {
            if (cr == -EINPROGRESS || cr == -EAGAIN) {
                fd_set wfds;
                FD_ZERO(&wfds); FD_SET(sock, &wfds);
                struct timeval tv = {10, 0};
                if (net_select(sock + 1, nullptr, &wfds, nullptr, &tv) <= 0) {
                    errMsg = "connect() timed out";
                    net_close(sock);
                    return -1;
                }
            } else {
                errMsg = "connect() failed";
                net_close(sock);
                return -1;
            }
        }
    }
    // libogc net_connect leaves the socket O_NONBLOCK after its internal polling loop — restore blocking.
    { u32 nb = 0; net_ioctl(sock, FIONBIO, &nb); }

    std::string authHdr = authToken.empty() ? "" : (", Token=\"" + authToken + "\"");
    std::string ctHdr   = contentType.empty() ? "" : ("Content-Type: " + contentType + "\r\n");
    // RFC 7230 §5.4: omit port from Host when it is the default for the scheme.
    std::string hostHdr = host + (port == 80 ? "" : (":" + std::to_string(port)));
    char req[4096];
    int reqLen = snprintf(req, sizeof(req),
        "%s %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Connection: close\r\n"
        "X-Emby-Authorization: " WIIFIN_CLIENT_HDR "%s\r\n"
        "%s"
        "Content-Length: %zu\r\n"
        "\r\n",
        method.c_str(),
        basePath.empty() ? "/" : basePath.c_str(),
        hostHdr.c_str(),
        authHdr.c_str(),
        ctHdr.c_str(),
        body.size()
    );
    if (reqLen >= (int)sizeof(req)) {
        errMsg = "Request headers too large";
        net_close(sock);
        return -1;
    }

    SYS_Report("[http] %s http://%s%s\n", method.c_str(), hostHdr.c_str(), basePath.c_str());

    // Send request headers + body, looping to handle partial writes and EAGAIN.
    {
        auto sendAll = [&](const void* data, int len) -> bool {
            const char* p = (const char*)data;
            int sent = 0;
            while (sent < len) {
                int w = net_write(sock, (void*)(p + sent), len - sent);
                if (w > 0) { sent += w; continue; }
                if (w == -EAGAIN || w == -EWOULDBLOCK) {
                    fd_set wfds; FD_ZERO(&wfds); FD_SET(sock, &wfds);
                    struct timeval tv = {10, 0};
                    if (net_select(sock + 1, nullptr, &wfds, nullptr, &tv) <= 0)
                        return false; // write timeout
                    continue;
                }
                return false; // write error
            }
            return true;
        };
        if (!sendAll(req, reqLen) ||
            (!body.empty() && !sendAll(body.c_str(), (int)body.size()))) {
            errMsg = "Failed to send request";
            net_close(sock);
            return -1;
        }
    }

    static char rawBuf[256 * 1024];
    int rawLen = 0;
    char buf[4096];
    while (true) {
        /* 15 s per-read timeout — prevents a hung server from blocking forever */
        {
            fd_set rfds; FD_ZERO(&rfds); FD_SET(sock, &rfds);
            struct timeval stv = {15, 0};
            if (net_select(sock + 1, &rfds, nullptr, nullptr, &stv) <= 0)
                break; /* timeout or select error */
        }
        int n = net_read(sock, buf, sizeof(buf) - 1);
        if (n > 0) {
            int canCopy = (int)(sizeof(rawBuf) - 1) - rawLen;
            if (canCopy > 0) {
                if (n < canCopy) canCopy = n;
                memcpy(rawBuf + rawLen, buf, (size_t)canCopy);
                rawLen += canCopy;
            }
        } else if (n == 0) {
            break; // connection closed
        } else if (n == -EAGAIN || n == -EWOULDBLOCK) {
            continue; // spurious EAGAIN — retry
        } else {
            break; // error
        }
    }
    net_close(sock);
    rawBuf[rawLen] = '\0';

    int status = 0;
    if (rawLen >= 12 && strncmp(rawBuf, "HTTP/", 5) == 0) {
        const char* sp = strchr(rawBuf, ' ');
        if (sp) status = atoi(sp + 1);
    }
    SYS_Report("[http] status=%d rawLen=%d\n", status, rawLen);
    if (status == 0) {
        errMsg = rawLen == 0 ? "No response from server (check server URL / port)"
                             : "Invalid HTTP response (server may require HTTPS)";
        return -1;
    }
    const char* sep = strstr(rawBuf, "\r\n\r\n");
    if (sep) {
        int headersLen = (int)(sep - rawBuf);
        const char* bodyPtr = sep + 4;
        int bodyLen = rawLen - headersLen - 4;
        if (bodyLen < 0) bodyLen = 0;
        responseBody = decodeChunked(rawBuf, headersLen, bodyPtr, bodyLen);
    } else {
        responseBody = "";
    }
    return status;
}

// Wii RTC is unreliable and many home Jellyfin servers use self-signed certificates
// or private CAs that are not in the embedded CA bundle.
// We clear date-related flags (clock not trusted) and NOT_TRUSTED (CA chain not trusted)
// so that self-signed / private-CA certs work. CN/SAN hostname verification is still
// enforced by mbedtls_ssl_set_hostname() above, which prevents MITM: an attacker on
// the local network would need a certificate valid for the exact server hostname.
// REVOKED and CN_MISMATCH are intentionally kept.
static int wii_cert_verify(void*, mbedtls_x509_crt*, int, uint32_t* flags) {
    *flags &= ~(uint32_t)(MBEDTLS_X509_BADCERT_EXPIRED    | MBEDTLS_X509_BADCERT_FUTURE    |
                          MBEDTLS_X509_BADCRL_EXPIRED      | MBEDTLS_X509_BADCRL_FUTURE     |
                          MBEDTLS_X509_BADCERT_NOT_TRUSTED | MBEDTLS_X509_BADCRL_NOT_TRUSTED);
    return 0;
}

// ---------------------------------------------------------------------------
// HTTPS over TLS using mbedTLS (MBEDTLS_SSL_VERIFY_NONE — no CA bundle)
// ---------------------------------------------------------------------------
int JellyfinClient::httpsRequest(const std::string& host, int port,
                                  const std::string& path,
                                  const std::string& method,
                                  const std::string& contentType,
                                  const std::string& body,
                                  const std::string& authToken,
                                  std::string& responseBody) {
    int httpStatus = -1;

    // Resolve address once — reused across retry attempts.
    // Try as a numeric IPv4 literal first; fall back to DNS.
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_aton(host.c_str(), &addr.sin_addr) == 0) {
        struct hostent* he = net_gethostbyname(host.c_str());
        if (!he) { errMsg = "DNS failed: " + host; return -1; }
        if (he->h_length > (int)sizeof(addr.sin_addr)) { errMsg = "DNS: unexpected address length"; return -1; }
        memcpy(&addr.sin_addr, he->h_addr, he->h_length);
    }

    /* Retry loop for transient IOS send/recv failures.
     *
     * After MPlayer CE exits via longjmp, its orphaned stream socket and cache2
     * thread can leave the IOS network IPC queue in a busy/inconsistent state.
     * The next TLS handshake attempt then fails because net_write() returns an
     * error (visible as MBEDTLS_ERR_NET_SEND_FAILED / UNKNOWN ERROR CODE 004C).
     * Closing the stale sockets in WiiPlayer.cpp resolves the root cause;
     * this retry is a secondary safety net in case IOS needs more time to settle. */
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (attempt > 0) usleep(500000); // 500 ms cool-down before retry

    s32 sock = net_socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { errMsg = "socket() failed (" + std::to_string(sock) + ")"; return -1; }

    {
        int cr = net_connect(sock, (struct sockaddr*)&addr, sizeof(addr));
        if (cr < 0) {
            if (cr == -EINPROGRESS || cr == -EAGAIN) {
                // Non-blocking connect in progress — wait for it to complete.
                fd_set wfds;
                FD_ZERO(&wfds); FD_SET(sock, &wfds);
                struct timeval tv = {10, 0}; // 10 s timeout
                if (net_select(sock + 1, nullptr, &wfds, nullptr, &tv) <= 0) {
                    errMsg = "connect() timed out";
                    net_close(sock);
                    return -1;
                }
            } else {
                errMsg = "connect() failed";
                net_close(sock);
                return -1;
            }
        }
    }
    // libogc net_connect leaves the socket O_NONBLOCK after its internal polling loop — restore blocking.
    { u32 nb = 0; net_ioctl(sock, FIONBIO, &nb); }

    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_ssl_context      ssl;
    mbedtls_ssl_config       conf;
    mbedtls_x509_crt         cacert;

    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    mbedtls_x509_crt_init(&cacert);

    bool failed    = false;
    bool transient = false;
    int  ret       = 0;

    do {
        const char* pers = "wiifin_jellyfin";
        if ((ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                          (const unsigned char*)pers, strlen(pers))) != 0) {
            errMsg = "TLS: DRBG seed failed";
            failed = true; break;
        }

        if ((ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
                                               MBEDTLS_SSL_TRANSPORT_STREAM,
                                               MBEDTLS_SSL_PRESET_DEFAULT)) != 0) {
            errMsg = "TLS: ssl_config_defaults failed";
            failed = true; break;
        }

        // Certificate verification (configurable)
        if (sslVerify) {
            // data_cacert_pem has a null terminator embedded (len includes it)
            int parseRet = mbedtls_x509_crt_parse(&cacert, data_cacert_pem, data_cacert_pem_len);
            if (parseRet < 0) {
                errMsg = "TLS: CA bundle parse failed (" + std::to_string(parseRet) + ")";
                failed = true; break;
            }
            mbedtls_ssl_conf_ca_chain(&conf, &cacert, nullptr);
            mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
            mbedtls_ssl_conf_verify(&conf, wii_cert_verify, nullptr);
        } else {
            mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
        }
        mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);

        if ((ret = mbedtls_ssl_setup(&ssl, &conf)) != 0) {
            errMsg = "TLS: ssl_setup failed";
            failed = true; break;
        }

        mbedtls_ssl_set_hostname(&ssl, host.c_str());
        mbedtls_ssl_set_bio(&ssl, &sock, wii_tls_send, wii_tls_recv, nullptr);

        // TLS handshake
        do {
            ret = mbedtls_ssl_handshake(&ssl);
        } while (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE);

        if (ret != 0) {
            /* NET_SEND_FAILED (0x4C) / NET_RECV_FAILED (0x4E): transient IOS I/O error
             * from MPlayer CE's orphaned socket/cache thread after longjmp.  Retry. */
            if ((ret == MBEDTLS_ERR_NET_SEND_FAILED ||
                 ret == MBEDTLS_ERR_NET_RECV_FAILED) && attempt < 2) {
                SYS_Report("[TLS] attempt %d: transient handshake err -0x%04X, retry\n",
                           attempt, (unsigned)(-ret));
                transient = true;
            } else {
                uint32_t vflags = mbedtls_ssl_get_verify_result(&ssl);
                char ebuf[80];
                mbedtls_strerror(ret, ebuf, sizeof(ebuf));
                char vbuf[24];
                snprintf(vbuf, sizeof(vbuf), " [f=%08X]", (unsigned)vflags);
                errMsg = std::string("TLS handshake failed: ") + ebuf + vbuf;
            }
            failed = true; break;
        }

        // Build HTTP/1.0 request
        std::string authHdr = authToken.empty() ? "" : (", Token=\"" + authToken + "\"");
        std::string ctHdr   = contentType.empty() ? "" : ("Content-Type: " + contentType + "\r\n");
        // RFC 7230 §5.4: omit port from Host when it is the default for the scheme.
        std::string hostHdr = host + (port == 443 ? "" : (":" + std::to_string(port)));
        char req[4096];
        int reqLen = snprintf(req, sizeof(req),
            "%s %s HTTP/1.0\r\n"
            "Host: %s\r\n"
            "Connection: close\r\n"
            "X-Emby-Authorization: " WIIFIN_CLIENT_HDR "%s\r\n"
            "%s"
            "Content-Length: %zu\r\n"
            "\r\n",
            method.c_str(),
            path.empty() ? "/" : path.c_str(),
            hostHdr.c_str(),
            authHdr.c_str(),
            ctHdr.c_str(),
            body.size()
        );
        if (reqLen >= (int)sizeof(req)) {
            errMsg = "Request headers too large";
            failed = true; break;
        }

        // Send headers
        const unsigned char* ptr = (const unsigned char*)req;
        int remaining = reqLen;
        while (remaining > 0) {
            ret = mbedtls_ssl_write(&ssl, ptr, (size_t)remaining);
            if (ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            if (ret < 0) { errMsg = "TLS: write failed"; failed = true; break; }
            ptr += ret; remaining -= ret;
        }
        if (failed) break;

        // Send body
        if (!body.empty()) {
            ptr = (const unsigned char*)body.c_str();
            remaining = (int)body.size();
            while (remaining > 0) {
                ret = mbedtls_ssl_write(&ssl, ptr, (size_t)remaining);
                if (ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
                if (ret < 0) { errMsg = "TLS: write body failed"; failed = true; break; }
                ptr += ret; remaining -= ret;
            }
        }
        if (failed) break;

        // Read response — g_frameCallback is called inside wii_tls_recv on EAGAIN
        static char rawBuf[256 * 1024];
        int rawLen = 0;
        unsigned char buf[2048];
        while (true) {
            ret = mbedtls_ssl_read(&ssl, buf, sizeof(buf));
            if (ret == MBEDTLS_ERR_SSL_WANT_READ) continue;
            if (ret <= 0) break;
            int canCopy = (int)(sizeof(rawBuf) - 1) - rawLen;
            if (canCopy > 0) {
                if (ret < canCopy) canCopy = ret;
                memcpy(rawBuf + rawLen, (char*)buf, (size_t)canCopy);
                rawLen += canCopy;
            }
        }
        rawBuf[rawLen] = '\0';

        mbedtls_ssl_close_notify(&ssl);

        if (rawLen >= 12 && strncmp(rawBuf, "HTTP/", 5) == 0) {
            const char* sp = strchr(rawBuf, ' ');
            if (sp) httpStatus = atoi(sp + 1);
        }
        const char* sep = strstr(rawBuf, "\r\n\r\n");
        if (sep) {
            int headersLen = (int)(sep - rawBuf);
            const char* bodyPtr = sep + 4;
            int bodyLen = rawLen - headersLen - 4;
            if (bodyLen < 0) bodyLen = 0;
            responseBody = decodeChunked(rawBuf, headersLen, bodyPtr, bodyLen);
        } else {
            responseBody = "";
        }

    } while (false);

    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    mbedtls_x509_crt_free(&cacert);
    net_close(sock);

        if (!failed)    return httpStatus; // success
        if (!transient) return -1;         // permanent failure — do not retry
        // transient failure — next loop iteration will wait + retry
    } // end retry loop

    return -1; // all attempts exhausted
}

// Escape a value for safe embedding inside a JSON string literal.
static std::string jsonEscapeString(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if      (c == '"')  { out += "\\\""; }
        else if (c == '\\') { out += "\\\\"; }
        else if (c == '\n') { out += "\\n"; }
        else if (c == '\r') { out += "\\r"; }
        else if (c == '\t') { out += "\\t"; }
        else if (c < 0x20)  { /* strip other control chars */ }
        else                { out += (char)c; }
    }
    return out;
}

bool JellyfinClient::authenticate(const std::string& serverUrl,
                                   const std::string& username,
                                   const std::string& password,
                                   JellyfinAuth& out) {
    std::string body = "{\"Username\":\"" + jsonEscapeString(username) + "\",\"Pw\":\"" + jsonEscapeString(password) + "\"}";
    std::string url  = serverUrl + "/Users/AuthenticateByName";
    std::string resp;
    int status = httpRequest(url, "POST", "application/json", body, "", resp);
    if (status != 200) {
        if (status >= 0)
            errMsg = "Auth failed (HTTP " + std::to_string(status) + ")";
        // status == -1 : errMsg already set by httpsRequest / httpRequest
        return false;
    }
    out.accessToken = jsonGetString(resp, "AccessToken");
    out.userId      = jsonGetString(resp, "Id");
    out.serverName  = jsonGetString(resp, "Name");
    if (out.accessToken.empty()) { errMsg = "No token in response"; return false; }
    return true;
}

bool JellyfinClient::quickConnectInitiate(const std::string& serverUrl, QuickConnectResult& out) {
    std::string url = serverUrl + "/QuickConnect/Initiate";
    std::string resp;
    int status = httpRequest(url, "POST", "application/json", "{}", "", resp);
    if (status != 200) {
        if (status >= 0)
            errMsg = "QuickConnect initiate failed (HTTP " + std::to_string(status) + ")";
        return false;
    }
    out.code   = jsonGetString(resp, "Code");
    out.secret = jsonGetString(resp, "Secret");
    out.authenticated = false;
    if (out.code.empty()) { errMsg = "No code in QC response"; return false; }
    return true;
}

bool JellyfinClient::quickConnectCheck(const std::string& serverUrl,
                                        const std::string& secret,
                                        QuickConnectResult& out) {
    std::string url = serverUrl + "/QuickConnect/Connect?Secret=" + secret;
    std::string resp;
    int status = httpRequest(url, "GET", "", "", "", resp);
    if (status != 200) return false;
    out.authenticated = jsonGetBool(resp, "Authenticated");
    out.secret = secret;
    return true;
}

bool JellyfinClient::quickConnectAuthenticate(const std::string& serverUrl,
                                               const std::string& secret,
                                               JellyfinAuth& out) {
    std::string body = "{\"Secret\":\"" + jsonEscapeString(secret) + "\"}";
    std::string url  = serverUrl + "/Users/AuthenticateWithQuickConnect";
    std::string resp;
    int status = httpRequest(url, "POST", "application/json", body, "", resp);
    if (status != 200) {
        if (status >= 0)
            errMsg = "QC auth failed (HTTP " + std::to_string(status) + ")";
        return false;
    }
    out.accessToken = jsonGetString(resp, "AccessToken");
    out.userId      = jsonGetString(resp, "Id");
    out.serverName  = jsonGetString(resp, "Name");
    if (out.accessToken.empty()) { errMsg = "No token in QC auth response"; return false; }
    return true;
}

bool JellyfinClient::getServerName(const std::string& serverUrl,
                                    const JellyfinAuth& auth,
                                    std::string& outName) {
    std::string resp;
    int status = httpRequest(serverUrl + "/System/Info",
                             "GET", "", "", auth.accessToken, resp);
    if (status != 200) return false;
    outName = jsonGetString(resp, "ServerName");
    return !outName.empty();
}

// ---------------------------------------------------------------------------
// Fetch user views (libraries)  GET /Users/{userId}/Views
// ---------------------------------------------------------------------------
bool JellyfinClient::getLibraries(const std::string& serverUrl,
                                   const JellyfinAuth& auth,
                                   std::vector<JellyfinLibrary>& out) {
    std::string url  = serverUrl + "/Users/" + auth.userId + "/Views";
    std::string resp;
    int status = httpRequest(url, "GET", "", "", auth.accessToken, resp);
    if (status != 200) {
        if (status >= 0) errMsg = "getLibraries failed (HTTP " + std::to_string(status) + ")";
        return false;
    }

    struct Ctx { JellyfinClient* self; std::vector<JellyfinLibrary>* out; };
    Ctx ctx{ this, &out };
    forEachItemObject(resp, [](const std::string& obj, void* vctx) {
        Ctx* c = static_cast<Ctx*>(vctx);
        JellyfinLibrary lib;
        lib.id             = c->self->jsonGetString(obj, "Id");
        lib.name           = c->self->jsonGetString(obj, "Name");
        lib.collectionType = c->self->jsonGetString(obj, "CollectionType");
        if (!lib.id.empty() && !lib.name.empty() && lib.collectionType != "books")
            c->out->push_back(lib);
    }, &ctx);

    return true;
}

// ---------------------------------------------------------------------------
// Fetch items inside a library  GET /Users/{userId}/Items?ParentId=...
// ---------------------------------------------------------------------------
bool JellyfinClient::getItems(const std::string& serverUrl,
                               const JellyfinAuth& auth,
                               const std::string& parentId,
                               int startIndex, int limit,
                               std::vector<JellyfinItem>& out,
                               int& totalCount) {
    char qs[512];
    snprintf(qs, sizeof(qs),
        "/Users/%s/Items?ParentId=%s"
        "&SortBy=SortName&SortOrder=Ascending"
        "&Fields=ProductionYear,UserData,RunTimeTicks"
        "&EnableImages=false"
        "&Limit=%d&StartIndex=%d",
        auth.userId.c_str(), parentId.c_str(), limit, startIndex);
    std::string url  = serverUrl + qs;
    std::string resp;
    int status = httpRequest(url, "GET", "", "", auth.accessToken, resp);
    if (status != 200) {
        if (status >= 0) errMsg = "getItems failed (HTTP " + std::to_string(status) + ")";
        return false;
    }

    totalCount = jsonGetInt(resp, "TotalRecordCount");

    struct Ctx { JellyfinClient* self; std::vector<JellyfinItem>* out; };
    Ctx ctx{ this, &out };
    forEachItemObject(resp, [](const std::string& obj, void* vctx) {
        Ctx* c = static_cast<Ctx*>(vctx);
        JellyfinItem item;
        item.id   = c->self->jsonGetString(obj, "Id");
        item.name = c->self->jsonGetString(obj, "Name");
        item.type = c->self->jsonGetString(obj, "Type");
        item.year = c->self->jsonGetInt(obj, "ProductionYear");
        if (!item.id.empty() && !item.name.empty())
            c->out->push_back(item);
    }, &ctx);

    return true;
}

// ---------------------------------------------------------------------------
// Fetch albums for an artist by AlbumArtistIds (more reliable than ParentId
// for artist items returned by /Search/Hints which may differ from the tree)
// ---------------------------------------------------------------------------
bool JellyfinClient::getAlbumsByArtist(const std::string& serverUrl,
                                        const JellyfinAuth& auth,
                                        const std::string& artistId,
                                        int startIndex, int limit,
                                        std::vector<JellyfinItem>& out,
                                        int& totalCount) {
    char qs[512];
    snprintf(qs, sizeof(qs),
        "/Users/%s/Items"
        "?AlbumArtistIds=%s"
        "&IncludeItemTypes=MusicAlbum"
        "&Recursive=true"
        "&SortBy=SortName&SortOrder=Ascending"
        "&Fields=ProductionYear,UserData,RunTimeTicks"
        "&EnableImages=false"
        "&Limit=%d&StartIndex=%d",
        auth.userId.c_str(), artistId.c_str(), limit, startIndex);
    std::string url  = serverUrl + qs;
    std::string resp;
    int status = httpRequest(url, "GET", "", "", auth.accessToken, resp);
    if (status != 200) {
        if (status >= 0) errMsg = "getAlbumsByArtist failed (HTTP " + std::to_string(status) + ")";
        return false;
    }

    totalCount = jsonGetInt(resp, "TotalRecordCount");

    struct Ctx { JellyfinClient* self; std::vector<JellyfinItem>* out; };
    Ctx ctx{ this, &out };
    forEachItemObject(resp, [](const std::string& obj, void* vctx) {
        Ctx* c = static_cast<Ctx*>(vctx);
        JellyfinItem item;
        item.id   = c->self->jsonGetString(obj, "Id");
        item.name = c->self->jsonGetString(obj, "Name");
        item.type = c->self->jsonGetString(obj, "Type");
        item.year = c->self->jsonGetInt(obj, "ProductionYear");
        if (!item.id.empty() && !item.name.empty())
            c->out->push_back(item);
    }, &ctx);

    return true;
}

// ---------------------------------------------------------------------------
// Fetch raw bytes for an item's primary image (JPEG)
// ---------------------------------------------------------------------------
bool JellyfinClient::getItemImageBytes(const std::string& serverUrl,
                                        const JellyfinAuth& auth,
                                        const std::string& itemId,
                                        int maxWidth, int maxHeight,
                                        std::string& outBytes) {
    char qs[256];
    snprintf(qs, sizeof(qs),
        "/Items/%s/Images/Primary?fillWidth=%d&fillHeight=%d&maxWidth=%d&maxHeight=%d&quality=82",
        itemId.c_str(), maxWidth, maxHeight, maxWidth, maxHeight);
    std::string url = serverUrl + qs;
    int status = httpRequest(url, "GET", "", "", auth.accessToken, outBytes);
    return status == 200;
}

// ---------------------------------------------------------------------------
// Fetch best landscape image for activity cards
//   Episode  → Images/Thumb  → series Images/Backdrop/0 → Primary fallback
//   Movie/…  → Images/Backdrop/0 → Primary fallback
// ---------------------------------------------------------------------------
bool JellyfinClient::getItemBackdropBytes(const std::string& serverUrl,
                                           const JellyfinAuth& auth,
                                           const JellyfinItem& item,
                                           int maxWidth, int maxHeight,
                                           std::string& outBytes) {
    char sizeqs[128];
    snprintf(sizeqs, sizeof(sizeqs),
             "?maxWidth=%d&maxHeight=%d&quality=82", maxWidth, maxHeight);

    if (item.type == "Episode") {
        // Episode still (Thumb)
        std::string url = serverUrl + "/Items/" + item.id + "/Images/Thumb" + sizeqs;
        int st = httpRequest(url, "GET", "", "", auth.accessToken, outBytes);
        if (st == 200 && !outBytes.empty()) return true;
        outBytes.clear();
        // Series backdrop
        if (!item.seriesId.empty()) {
            url = serverUrl + "/Items/" + item.seriesId + "/Images/Backdrop/0" + sizeqs;
            st  = httpRequest(url, "GET", "", "", auth.accessToken, outBytes);
            if (st == 200 && !outBytes.empty()) return true;
            outBytes.clear();
        }
    } else {
        // Movie / Series backdrop
        std::string url = serverUrl + "/Items/" + item.id + "/Images/Backdrop/0" + sizeqs;
        int st = httpRequest(url, "GET", "", "", auth.accessToken, outBytes);
        if (st == 200 && !outBytes.empty()) return true;
        outBytes.clear();
    }

    // Fallback: Primary portrait image
    return getItemImageBytes(serverUrl, auth, item.id, maxWidth, maxHeight, outBytes);
}

// ---------------------------------------------------------------------------
// Fetch full item metadata  GET /Items/{id}
// ---------------------------------------------------------------------------
bool JellyfinClient::getItemDetail(const std::string& serverUrl,
                                    const JellyfinAuth& auth,
                                    const std::string& itemId,
                                    JellyfinItemDetail& out) {
    std::string url = serverUrl + "/Items/" + itemId
        + "?Fields=Overview,Genres,OfficialRating,RunTimeTicks,MediaStreams,UserData";
    std::string resp;
    int status = httpRequest(url, "GET", "", "", auth.accessToken, resp);
    if (status != 200) {
        if (status >= 0) errMsg = "getItemDetail failed (HTTP " + std::to_string(status) + ")";
        return false;
    }

    out.id                    = jsonGetString(resp, "Id");
    out.name                  = jsonGetString(resp, "Name");
    out.overview              = jsonGetString(resp, "Overview");
    out.officialRating        = jsonGetString(resp, "OfficialRating");
    out.year                  = jsonGetInt(resp,    "ProductionYear");
    out.runtimeTicks          = jsonGetLongLong(resp, "RunTimeTicks");
    out.playbackPositionTicks = jsonGetLongLong(resp, "PlaybackPositionTicks");

    // Genres array: ["Action","Comedy",...]
    {
        size_t gp = resp.find("\"Genres\":");
        if (gp != std::string::npos) {
            size_t lb = resp.find('[', gp);
            size_t rb = resp.find(']', lb);
            if (lb != std::string::npos && rb != std::string::npos) {
                std::string arr = resp.substr(lb + 1, rb - lb - 1);
                size_t p = 0;
                while (out.genres.size() < 4 && p < arr.size()) {
                    size_t q1 = arr.find('"', p);
                    if (q1 == std::string::npos) break;
                    size_t q2 = arr.find('"', q1 + 1);
                    if (q2 == std::string::npos) break;
                    out.genres.push_back(decodeJsonString(arr.substr(q1 + 1, q2 - q1 - 1)));
                    p = q2 + 1;
                }
            }
        }
    }

    // People array: [{"Name":"...","Type":"Actor","Role":"..."}, ...]
    // We want directors first (max 1) then up to 5 actors = 6 total
    {
        size_t pp = resp.find("\"People\":");
        if (pp != std::string::npos) {
            size_t lb = resp.find('[', pp);
            if (lb != std::string::npos) {
                // Walk the array manually
                size_t pos = lb + 1;
                while (out.people.size() < 6 && pos < resp.size()) {
                    while (pos < resp.size() && resp[pos] != '{' && resp[pos] != ']') pos++;
                    if (pos >= resp.size() || resp[pos] == ']') break;
                    // find matching }
                    int depth = 0; bool inStr = false;
                    size_t objStart = pos;
                    for (; pos < resp.size(); pos++) {
                        char c = resp[pos];
                        if (inStr) {
                            if (c == '\\') pos++;
                            else if (c == '"') inStr = false;
                        } else {
                            if (c == '"') inStr = true;
                            else if (c == '{') depth++;
                            else if (c == '}') { if (--depth == 0) { pos++; break; } }
                        }
                    }
                    std::string obj = resp.substr(objStart, pos - objStart);
                    JellyfinPerson p;
                    p.name      = jsonGetString(obj, "Name");
                    p.role      = jsonGetString(obj, "Type");
                    p.character = jsonGetString(obj, "Role");
                    if (!p.name.empty()) out.people.push_back(p);
                }
            }
        }
    }

    // MediaStreams: audio and subtitle tracks
    {
        size_t mp = resp.find("\"MediaStreams\":");
        if (mp != std::string::npos) {
            size_t lb = resp.find('[', mp);
            if (lb != std::string::npos) {
                size_t pos = lb + 1;
                while (pos < resp.size()) {
                    while (pos < resp.size() && resp[pos] != '{' && resp[pos] != ']') pos++;
                    if (pos >= resp.size() || resp[pos] == ']') break;
                    int depth = 0; bool inStr = false;
                    size_t objStart = pos;
                    for (; pos < resp.size(); pos++) {
                        char c = resp[pos];
                        if (inStr) {
                            if (c == '\\') pos++;
                            else if (c == '"') inStr = false;
                        } else {
                            if (c == '"') inStr = true;
                            else if (c == '{') depth++;
                            else if (c == '}') { if (--depth == 0) { pos++; break; } }
                        }
                    }
                    std::string obj = resp.substr(objStart, pos - objStart);
                    std::string t = jsonGetString(obj, "Type");
                    MediaStream ms;
                    ms.index        = jsonGetInt(obj, "Index");
                    ms.type         = t;
                    ms.displayTitle = jsonGetString(obj, "DisplayTitle");
                    ms.language     = jsonGetString(obj, "Language");
                    ms.codec        = jsonGetString(obj, "Codec");
                    if      (t == "Audio"    && out.audioStreams.size()    < 8)  out.audioStreams.push_back(ms);
                    else if (t == "Subtitle" && out.subtitleStreams.size() < 16) out.subtitleStreams.push_back(ms);
                }
            }
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// Fetch seasons for a TV series  GET /Shows/{seriesId}/Seasons
// ---------------------------------------------------------------------------
bool JellyfinClient::getSeasons(const std::string& serverUrl,
                                 const JellyfinAuth& auth,
                                 const std::string& seriesId,
                                 std::vector<JellyfinSeason>& out) {
    char qs[512];
    snprintf(qs, sizeof(qs),
        "/Shows/%s/Seasons?UserId=%s&Fields=IndexNumber&EnableImages=false",
        seriesId.c_str(), auth.userId.c_str());
    std::string url  = serverUrl + qs;
    std::string resp;
    int status = httpRequest(url, "GET", "", "", auth.accessToken, resp);
    if (status != 200) {
        if (status >= 0) errMsg = "getSeasons failed (HTTP " + std::to_string(status) + ")";
        return false;
    }

    struct Ctx { JellyfinClient* self; std::vector<JellyfinSeason>* out; };
    Ctx ctx{ this, &out };
    forEachItemObject(resp, [](const std::string& obj, void* vctx) {
        Ctx* c = static_cast<Ctx*>(vctx);
        JellyfinSeason s;
        s.id          = c->self->jsonGetString(obj, "Id");
        s.name        = c->self->jsonGetString(obj, "Name");
        s.indexNumber = c->self->jsonGetInt(obj,    "IndexNumber");
        if (!s.id.empty()) c->out->push_back(s);
    }, &ctx);

    return true;
}

// ---------------------------------------------------------------------------
// Fetch episodes for a season  GET /Shows/{seriesId}/Episodes?SeasonId=...
// ---------------------------------------------------------------------------
bool JellyfinClient::getEpisodes(const std::string& serverUrl,
                                  const JellyfinAuth& auth,
                                  const std::string& seriesId,
                                  const std::string& seasonId,
                                  std::vector<JellyfinEpisode>& out) {
    char qs[512];
    snprintf(qs, sizeof(qs),
        "/Shows/%s/Episodes?SeasonId=%s&UserId=%s"
        "&Fields=IndexNumber,ParentIndexNumber,UserData&EnableImages=false",
        seriesId.c_str(), seasonId.c_str(), auth.userId.c_str());
    std::string url  = serverUrl + qs;
    std::string resp;
    int status = httpRequest(url, "GET", "", "", auth.accessToken, resp);
    if (status != 200) {
        if (status >= 0) errMsg = "getEpisodes failed (HTTP " + std::to_string(status) + ")";
        return false;
    }

    struct Ctx { JellyfinClient* self; std::vector<JellyfinEpisode>* out; };
    Ctx ctx{ this, &out };
    forEachItemObject(resp, [](const std::string& obj, void* vctx) {
        Ctx* c = static_cast<Ctx*>(vctx);
        JellyfinEpisode e;
        e.id                    = c->self->jsonGetString(obj, "Id");
        e.name                  = c->self->jsonGetString(obj, "Name");
        e.indexNumber           = c->self->jsonGetInt(obj,    "IndexNumber");
        e.seasonNumber          = c->self->jsonGetInt(obj,    "ParentIndexNumber");
        e.playbackPositionTicks = c->self->jsonGetLongLong(obj, "PlaybackPositionTicks");
        if (!e.id.empty()) c->out->push_back(e);
    }, &ctx);

    return true;
}

// ---------------------------------------------------------------------------
// Continue Watching  GET /Users/{userId}/Items/Resume
// ---------------------------------------------------------------------------
bool JellyfinClient::getContinueWatching(const std::string& serverUrl,
                                          const JellyfinAuth& auth,
                                          std::vector<JellyfinItem>& out) {
    char qs[256];
    snprintf(qs, sizeof(qs),
        "/Users/%s/Items/Resume?Limit=3&MediaTypes=Video"
        "&Fields=ProductionYear,UserData,RunTimeTicks,SeriesId&EnableImages=false",
        auth.userId.c_str());
    std::string url  = serverUrl + qs;
    std::string resp;
    int status = httpRequest(url, "GET", "", "", auth.accessToken, resp);
    if (status != 200) return false;

    struct Ctx { JellyfinClient* self; std::vector<JellyfinItem>* out; };
    Ctx ctx{ this, &out };
    forEachItemObject(resp, [](const std::string& obj, void* vctx) {
        Ctx* c = static_cast<Ctx*>(vctx);
        JellyfinItem item;
        item.id                    = c->self->jsonGetString(obj, "Id");
        item.name                  = c->self->jsonGetString(obj, "Name");
        item.type                  = c->self->jsonGetString(obj, "Type");
        item.year                  = c->self->jsonGetInt(obj,    "ProductionYear");
        item.seriesName            = c->self->jsonGetString(obj, "SeriesName");
        item.seriesId              = c->self->jsonGetString(obj, "SeriesId");
        item.seasonNumber          = c->self->jsonGetInt(obj,    "ParentIndexNumber");
        item.episodeNumber         = c->self->jsonGetInt(obj,    "IndexNumber");
        item.runtimeTicks          = c->self->jsonGetLongLong(obj, "RunTimeTicks");
        item.playbackPositionTicks = c->self->jsonGetLongLong(obj, "PlaybackPositionTicks");
        if (!item.id.empty())
            c->out->push_back(item);
    }, &ctx);
    return true;
}

// ---------------------------------------------------------------------------
// Next Up  GET /Shows/NextUp
// ---------------------------------------------------------------------------
bool JellyfinClient::getNextUp(const std::string& serverUrl,
                                const JellyfinAuth& auth,
                                std::vector<JellyfinItem>& out) {
    char qs[256];
    snprintf(qs, sizeof(qs),
        "/Shows/NextUp?UserId=%s&Limit=3&MediaTypes=Video"
        "&Fields=ProductionYear,UserData,RunTimeTicks,SeriesId&EnableImages=false",
        auth.userId.c_str());
    std::string url  = serverUrl + qs;
    std::string resp;
    int status = httpRequest(url, "GET", "", "", auth.accessToken, resp);
    if (status != 200) return false;

    struct Ctx { JellyfinClient* self; std::vector<JellyfinItem>* out; };
    Ctx ctx{ this, &out };
    forEachItemObject(resp, [](const std::string& obj, void* vctx) {
        Ctx* c = static_cast<Ctx*>(vctx);
        JellyfinItem item;
        item.id            = c->self->jsonGetString(obj, "Id");
        item.name          = c->self->jsonGetString(obj, "Name");
        item.type          = c->self->jsonGetString(obj, "Type");
        item.year          = c->self->jsonGetInt(obj,    "ProductionYear");
        item.seriesName    = c->self->jsonGetString(obj, "SeriesName");
        item.seriesId      = c->self->jsonGetString(obj, "SeriesId");
        item.seasonNumber  = c->self->jsonGetInt(obj,    "ParentIndexNumber");
        item.episodeNumber = c->self->jsonGetInt(obj,    "IndexNumber");
        item.runtimeTicks  = c->self->jsonGetLongLong(obj, "RunTimeTicks");
        if (!item.id.empty())
            c->out->push_back(item);
    }, &ctx);
    return true;
}

// ---------------------------------------------------------------------------
// BoxSet collections from a movies library
// ---------------------------------------------------------------------------
bool JellyfinClient::getMovieCollections(const std::string& serverUrl,
                                          const JellyfinAuth& auth,
                                          const std::string& /*parentId*/,
                                          int startIndex, int limit,
                                          std::vector<JellyfinItem>& out,
                                          int& totalCount) {
    // BoxSets live in a separate virtual "Collections" folder, not under the
    // movies library. Query globally with Recursive=true to find all of them.
    char qs[512];
    snprintf(qs, sizeof(qs),
        "/Users/%s/Items?IncludeItemTypes=BoxSet"
        "&SortBy=SortName&SortOrder=Ascending"
        "&Fields=ProductionYear,UserData,RunTimeTicks"
        "&EnableImages=false&Recursive=true"
        "&Limit=%d&StartIndex=%d",
        auth.userId.c_str(), limit, startIndex);
    std::string url  = serverUrl + qs;
    std::string resp;
    int status = httpRequest(url, "GET", "", "", auth.accessToken, resp);
    if (status != 200) {
        if (status >= 0) errMsg = "getMovieCollections failed (HTTP " + std::to_string(status) + ")";
        return false;
    }
    totalCount = jsonGetInt(resp, "TotalRecordCount");
    struct Ctx { JellyfinClient* self; std::vector<JellyfinItem>* out; };
    Ctx ctx{ this, &out };
    forEachItemObject(resp, [](const std::string& obj, void* vctx) {
        Ctx* c = static_cast<Ctx*>(vctx);
        JellyfinItem item;
        item.id   = c->self->jsonGetString(obj, "Id");
        item.name = c->self->jsonGetString(obj, "Name");
        item.type = c->self->jsonGetString(obj, "Type");
        item.year = c->self->jsonGetInt(obj, "ProductionYear");
        if (!item.id.empty() && !item.name.empty())
            c->out->push_back(item);
    }, &ctx);
    return true;
}

// ---------------------------------------------------------------------------
// Favourite movies from a library
// ---------------------------------------------------------------------------
bool JellyfinClient::getFavoriteMovies(const std::string& serverUrl,
                                        const JellyfinAuth& auth,
                                        const std::string& parentId,
                                        int startIndex, int limit,
                                        std::vector<JellyfinItem>& out,
                                        int& totalCount) {
    char qs[512];
    snprintf(qs, sizeof(qs),
        "/Users/%s/Items?ParentId=%s&IncludeItemTypes=Movie"
        "&Filters=IsFavorite"
        "&SortBy=SortName&SortOrder=Ascending"
        "&Fields=ProductionYear,UserData,RunTimeTicks"
        "&EnableImages=false&Recursive=true"
        "&Limit=%d&StartIndex=%d",
        auth.userId.c_str(), parentId.c_str(), limit, startIndex);
    std::string url  = serverUrl + qs;
    std::string resp;
    int status = httpRequest(url, "GET", "", "", auth.accessToken, resp);
    if (status != 200) {
        if (status >= 0) errMsg = "getFavoriteMovies failed (HTTP " + std::to_string(status) + ")";
        return false;
    }
    totalCount = jsonGetInt(resp, "TotalRecordCount");
    struct Ctx { JellyfinClient* self; std::vector<JellyfinItem>* out; };
    Ctx ctx{ this, &out };
    forEachItemObject(resp, [](const std::string& obj, void* vctx) {
        Ctx* c = static_cast<Ctx*>(vctx);
        JellyfinItem item;
        item.id   = c->self->jsonGetString(obj, "Id");
        item.name = c->self->jsonGetString(obj, "Name");
        item.type = c->self->jsonGetString(obj, "Type");
        item.year = c->self->jsonGetInt(obj, "ProductionYear");
        if (!item.id.empty() && !item.name.empty())
            c->out->push_back(item);
    }, &ctx);
    return true;
}

// ---------------------------------------------------------------------------
// Global favourites — all favourite items (Movie, Series, MusicAlbum) server-wide
// ---------------------------------------------------------------------------
bool JellyfinClient::getGlobalFavorites(const std::string& serverUrl,
                                         const JellyfinAuth& auth,
                                         int startIndex, int limit,
                                         std::vector<JellyfinItem>& out,
                                         int& totalCount) {
    char qs[512];
    snprintf(qs, sizeof(qs),
        "/Users/%s/Items?IncludeItemTypes=Movie,Series,MusicAlbum"
        "&Filters=IsFavorite"
        "&SortBy=SortName&SortOrder=Ascending"
        "&Fields=ProductionYear,UserData,RunTimeTicks"
        "&EnableImages=false&Recursive=true"
        "&Limit=%d&StartIndex=%d",
        auth.userId.c_str(), limit, startIndex);
    std::string url  = serverUrl + qs;
    std::string resp;
    int status = httpRequest(url, "GET", "", "", auth.accessToken, resp);
    if (status != 200) {
        if (status >= 0) errMsg = "getGlobalFavorites failed (HTTP " + std::to_string(status) + ")";
        return false;
    }
    totalCount = jsonGetInt(resp, "TotalRecordCount");
    struct Ctx { JellyfinClient* self; std::vector<JellyfinItem>* out; };
    Ctx ctx{ this, &out };
    forEachItemObject(resp, [](const std::string& obj, void* vctx) {
        Ctx* c = static_cast<Ctx*>(vctx);
        JellyfinItem item;
        item.id   = c->self->jsonGetString(obj, "Id");
        item.name = c->self->jsonGetString(obj, "Name");
        item.type = c->self->jsonGetString(obj, "Type");
        item.year = c->self->jsonGetInt(obj, "ProductionYear");
        if (!item.id.empty() && !item.name.empty())
            c->out->push_back(item);
    }, &ctx);
    return true;
}

// ---------------------------------------------------------------------------
// Continue-watching filtered to movies only
// ---------------------------------------------------------------------------
bool JellyfinClient::getMovieContinueWatching(const std::string& serverUrl,
                                               const JellyfinAuth& auth,
                                               std::vector<JellyfinItem>& out) {
    char qs[256];
    snprintf(qs, sizeof(qs),
        "/Users/%s/Items/Resume?Limit=4&IncludeItemTypes=Movie"
        "&Fields=ProductionYear,UserData,RunTimeTicks&EnableImages=false",
        auth.userId.c_str());
    std::string url  = serverUrl + qs;
    std::string resp;
    int status = httpRequest(url, "GET", "", "", auth.accessToken, resp);
    if (status != 200) return false;
    struct Ctx { JellyfinClient* self; std::vector<JellyfinItem>* out; };
    Ctx ctx{ this, &out };
    forEachItemObject(resp, [](const std::string& obj, void* vctx) {
        Ctx* c = static_cast<Ctx*>(vctx);
        JellyfinItem item;
        item.id                    = c->self->jsonGetString(obj, "Id");
        item.name                  = c->self->jsonGetString(obj, "Name");
        item.type                  = c->self->jsonGetString(obj, "Type");
        item.year                  = c->self->jsonGetInt(obj,    "ProductionYear");
        item.runtimeTicks          = c->self->jsonGetLongLong(obj, "RunTimeTicks");
        item.playbackPositionTicks = c->self->jsonGetLongLong(obj, "PlaybackPositionTicks");
        if (!item.id.empty())
            c->out->push_back(item);
    }, &ctx);
    return true;
}

// ---------------------------------------------------------------------------
// Recently added movies — /Users/{id}/Items/Latest returns a plain array
// ---------------------------------------------------------------------------
bool JellyfinClient::getMoviesLatest(const std::string& serverUrl,
                                      const JellyfinAuth& auth,
                                      const std::string& parentId,
                                      int limit,
                                      std::vector<JellyfinItem>& out) {
    char qs[256];
    snprintf(qs, sizeof(qs),
        "/Users/%s/Items/Latest?ParentId=%s&IncludeItemTypes=Movie"
        "&Limit=%d&Fields=ProductionYear,UserData,RunTimeTicks&EnableImages=false",
        auth.userId.c_str(), parentId.c_str(), limit);
    std::string url  = serverUrl + qs;
    std::string resp;
    int status = httpRequest(url, "GET", "", "", auth.accessToken, resp);
    if (status != 200) return false;

    // Response is a raw JSON array "[{...},{...}]", not the Items/TotalRecordCount envelope.
    size_t pos = 0;
    while (pos < resp.size() && resp[pos] != '[') pos++;
    if (pos >= resp.size()) return true;
    pos++; // skip '['
    while (pos < resp.size()) {
        while (pos < resp.size() && resp[pos] != '{' && resp[pos] != ']') pos++;
        if (pos >= resp.size() || resp[pos] == ']') break;
        int  depth = 0; bool inStr = false;
        size_t objStart = pos;
        for (; pos < resp.size(); pos++) {
            char c = resp[pos];
            if (inStr) {
                if      (c == '\\') { pos++; }
                else if (c == '"')  { inStr = false; }
            } else {
                if      (c == '"') { inStr = true; }
                else if (c == '{') { depth++; }
                else if (c == '}') { if (--depth == 0) { pos++; break; } }
            }
        }
        std::string obj = resp.substr(objStart, pos - objStart);
        JellyfinItem item;
        item.id   = jsonGetString(obj, "Id");
        item.name = jsonGetString(obj, "Name");
        item.type = jsonGetString(obj, "Type");
        item.year = jsonGetInt(obj, "ProductionYear");
        if (!item.id.empty() && !item.name.empty())
            out.push_back(item);
    }
    return true;
}

// ---------------------------------------------------------------------------
// URL query-param helper (shared by getTranscodingUrl / getAudioStreamUrl)
// ---------------------------------------------------------------------------
static void urlReplaceParam(std::string& url, const char* key, const char* val)
{
    std::string k = std::string(key) + "=";
    size_t p = url.find(k);
    if (p == std::string::npos) { url += "&" + k + val; return; }
    size_t vs = p + k.size();
    size_t ve = url.find('&', vs);
    url.replace(vs, (ve == std::string::npos ? url.size() : ve) - vs, val);
}

static void urlRemoveParam(std::string& url, const char* key)
{
    // Try "&key=val" first (non-first query param)
    {
        std::string srch = std::string("&") + key + "=";
        size_t p = url.find(srch);
        if (p != std::string::npos) {
            size_t ve = url.find('&', p + srch.size());
            url.erase(p, (ve == std::string::npos ? url.size() : ve) - p);
            return;
        }
    }
    // Try "?key=val" (first query param)
    {
        std::string srch = std::string("?") + key + "=";
        size_t p = url.find(srch);
        if (p != std::string::npos) {
            size_t ve = url.find('&', p + srch.size());
            if (ve == std::string::npos)
                url.erase(p);           // only param — remove "?key=val"
            else
                url.erase(p + 1, ve - p); // keep '?', remove "key=val&"
        }
    }
}

// ---------------------------------------------------------------------------
// Continue-watching TV episodes only
// ---------------------------------------------------------------------------
bool JellyfinClient::getTVContinueWatching(const std::string& serverUrl,
                                            const JellyfinAuth& auth,
                                            std::vector<JellyfinItem>& out) {
    char qs[256];
    snprintf(qs, sizeof(qs),
        "/Users/%s/Items/Resume?Limit=4&IncludeItemTypes=Episode"
        "&Fields=ProductionYear,UserData,RunTimeTicks,SeriesId&EnableImages=false",
        auth.userId.c_str());
    std::string url  = serverUrl + qs;
    std::string resp;
    int status = httpRequest(url, "GET", "", "", auth.accessToken, resp);
    if (status != 200) return false;
    struct Ctx { JellyfinClient* self; std::vector<JellyfinItem>* out; };
    Ctx ctx{ this, &out };
    forEachItemObject(resp, [](const std::string& obj, void* vctx) {
        Ctx* c = static_cast<Ctx*>(vctx);
        JellyfinItem item;
        item.id                    = c->self->jsonGetString(obj, "Id");
        item.name                  = c->self->jsonGetString(obj, "Name");
        item.type                  = c->self->jsonGetString(obj, "Type");
        item.year                  = c->self->jsonGetInt(obj,    "ProductionYear");
        item.seriesName            = c->self->jsonGetString(obj, "SeriesName");
        item.seriesId              = c->self->jsonGetString(obj, "SeriesId");
        item.seasonNumber          = c->self->jsonGetInt(obj,    "ParentIndexNumber");
        item.episodeNumber         = c->self->jsonGetInt(obj,    "IndexNumber");
        item.runtimeTicks          = c->self->jsonGetLongLong(obj, "RunTimeTicks");
        item.playbackPositionTicks = c->self->jsonGetLongLong(obj, "PlaybackPositionTicks");
        if (!item.id.empty())
            c->out->push_back(item);
    }, &ctx);
    return true;
}

// ---------------------------------------------------------------------------
// Recently added TV series — /Users/{id}/Items/Latest (raw array)
// ---------------------------------------------------------------------------
bool JellyfinClient::getTVSeriesLatest(const std::string& serverUrl,
                                        const JellyfinAuth& auth,
                                        const std::string& parentId,
                                        int limit,
                                        std::vector<JellyfinItem>& out) {
    char qs[256];
    snprintf(qs, sizeof(qs),
        "/Users/%s/Items/Latest?ParentId=%s&IncludeItemTypes=Series"
        "&Limit=%d&Fields=ProductionYear,UserData,RunTimeTicks&EnableImages=false",
        auth.userId.c_str(), parentId.c_str(), limit);
    std::string url  = serverUrl + qs;
    std::string resp;
    int status = httpRequest(url, "GET", "", "", auth.accessToken, resp);
    if (status != 200) return false;

    // Response is a raw JSON array "[{...},{...}]"
    size_t pos = 0;
    while (pos < resp.size() && resp[pos] != '[') pos++;
    if (pos >= resp.size()) return true;
    pos++;
    while (pos < resp.size()) {
        while (pos < resp.size() && resp[pos] != '{' && resp[pos] != ']') pos++;
        if (pos >= resp.size() || resp[pos] == ']') break;
        int  depth = 0; bool inStr = false;
        size_t objStart = pos;
        for (; pos < resp.size(); pos++) {
            char c = resp[pos];
            if (inStr) {
                if      (c == '\\') { pos++; }
                else if (c == '"')  { inStr = false; }
            } else {
                if      (c == '"') { inStr = true; }
                else if (c == '{') { depth++; }
                else if (c == '}') { if (--depth == 0) { pos++; break; } }
            }
        }
        std::string obj = resp.substr(objStart, pos - objStart);
        JellyfinItem item;
        item.id   = jsonGetString(obj, "Id");
        item.name = jsonGetString(obj, "Name");
        item.type = jsonGetString(obj, "Type");
        item.year = jsonGetInt(obj, "ProductionYear");
        if (!item.id.empty() && !item.name.empty())
            out.push_back(item);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Upcoming (unaired) episodes — /Shows/Upcoming
// ---------------------------------------------------------------------------
bool JellyfinClient::getTVUpcoming(const std::string& serverUrl,
                                    const JellyfinAuth& auth,
                                    int limit,
                                    std::vector<JellyfinItem>& out) {
    char qs[256];
    snprintf(qs, sizeof(qs),
        "/Shows/Upcoming?UserId=%s&Limit=%d"
        "&Fields=ProductionYear,UserData,RunTimeTicks,SeriesId&EnableImages=false",
        auth.userId.c_str(), limit);
    std::string url  = serverUrl + qs;
    std::string resp;
    int status = httpRequest(url, "GET", "", "", auth.accessToken, resp);
    if (status != 200) return false;
    struct Ctx { JellyfinClient* self; std::vector<JellyfinItem>* out; };
    Ctx ctx{ this, &out };
    forEachItemObject(resp, [](const std::string& obj, void* vctx) {
        Ctx* c = static_cast<Ctx*>(vctx);
        JellyfinItem item;
        item.id            = c->self->jsonGetString(obj, "Id");
        item.name          = c->self->jsonGetString(obj, "Name");
        item.type          = c->self->jsonGetString(obj, "Type");
        item.year          = c->self->jsonGetInt(obj,    "ProductionYear");
        item.seriesName    = c->self->jsonGetString(obj, "SeriesName");
        item.seriesId      = c->self->jsonGetString(obj, "SeriesId");
        item.seasonNumber  = c->self->jsonGetInt(obj,    "ParentIndexNumber");
        item.episodeNumber = c->self->jsonGetInt(obj,    "IndexNumber");
        item.runtimeTicks  = c->self->jsonGetLongLong(obj, "RunTimeTicks");
        if (!item.id.empty())
            c->out->push_back(item);
    }, &ctx);
    return true;
}

// ---------------------------------------------------------------------------
// Recently added music albums — /Users/{id}/Items/Latest (raw array)
// ---------------------------------------------------------------------------
bool JellyfinClient::getMusicLatest(const std::string& serverUrl,
                                     const JellyfinAuth& auth,
                                     const std::string& parentId,
                                     int limit,
                                     std::vector<JellyfinItem>& out) {
    // Use standard Items endpoint sorted by DateCreated desc so we always
    // return up to `limit` albums regardless of when they were added.
    char qs[512];
    snprintf(qs, sizeof(qs),
        "/Users/%s/Items?ParentId=%s&IncludeItemTypes=MusicAlbum"
        "&SortBy=DateCreated,SortName&SortOrder=Descending,Ascending"
        "&Recursive=true&Limit=%d"
        "&Fields=ProductionYear,UserData,RunTimeTicks&EnableImages=false",
        auth.userId.c_str(), parentId.c_str(), limit);
    std::string url  = serverUrl + qs;
    std::string resp;
    int status = httpRequest(url, "GET", "", "", auth.accessToken, resp);
    if (status != 200) return false;

    // Standard envelope: {"Items":[...], "TotalRecordCount":N}
    size_t arrPos = resp.find("\"Items\"");
    if (arrPos == std::string::npos) return true;
    arrPos = resp.find('[', arrPos);
    if (arrPos == std::string::npos) return true;
    resp = resp.substr(arrPos); // trim to the array

    // Response is a raw JSON array "[{...},{...}]"
    size_t pos = 0;
    while (pos < resp.size() && resp[pos] != '[') pos++;
    if (pos >= resp.size()) return true;
    pos++;
    while (pos < resp.size()) {
        while (pos < resp.size() && resp[pos] != '{' && resp[pos] != ']') pos++;
        if (pos >= resp.size() || resp[pos] == ']') break;
        int depth = 0; bool inStr = false;
        size_t objStart = pos;
        for (; pos < resp.size(); pos++) {
            char c = resp[pos];
            if (inStr) {
                if      (c == '\\') { pos++; }
                else if (c == '"')  { inStr = false; }
            } else {
                if      (c == '"') { inStr = true; }
                else if (c == '{') { depth++; }
                else if (c == '}') { if (--depth == 0) { pos++; break; } }
            }
        }
        std::string obj = resp.substr(objStart, pos - objStart);
        JellyfinItem item;
        item.id   = jsonGetString(obj, "Id");
        item.name = jsonGetString(obj, "Name");
        item.type = jsonGetString(obj, "Type");
        item.year = jsonGetInt(obj, "ProductionYear");
        if (!item.id.empty() && !item.name.empty())
            out.push_back(item);
    }
    return true;
}

// ---------------------------------------------------------------------------
// All playlists — paginated
// ---------------------------------------------------------------------------
bool JellyfinClient::getPlaylists(const std::string& serverUrl,
                                   const JellyfinAuth& auth,
                                   int startIndex, int limit,
                                   std::vector<JellyfinItem>& out,
                                   int& totalCount) {
    char qs[512];
    snprintf(qs, sizeof(qs),
        "/Users/%s/Items?IncludeItemTypes=Playlist"
        "&SortBy=SortName&SortOrder=Ascending"
        "&Fields=ProductionYear,UserData,RunTimeTicks"
        "&EnableImages=false&Recursive=true"
        "&Limit=%d&StartIndex=%d",
        auth.userId.c_str(), limit, startIndex);
    std::string url  = serverUrl + qs;
    std::string resp;
    int status = httpRequest(url, "GET", "", "", auth.accessToken, resp);
    if (status != 200) {
        if (status >= 0) errMsg = "getPlaylists failed (HTTP " + std::to_string(status) + ")";
        return false;
    }
    totalCount = jsonGetInt(resp, "TotalRecordCount");
    struct Ctx { JellyfinClient* self; std::vector<JellyfinItem>* out; };
    Ctx ctx{ this, &out };
    forEachItemObject(resp, [](const std::string& obj, void* vctx) {
        Ctx* c = static_cast<Ctx*>(vctx);
        JellyfinItem item;
        item.id   = c->self->jsonGetString(obj, "Id");
        item.name = c->self->jsonGetString(obj, "Name");
        item.type = c->self->jsonGetString(obj, "Type");
        if (!item.id.empty() && !item.name.empty())
            c->out->push_back(item);
    }, &ctx);
    return true;
}

// ---------------------------------------------------------------------------
// Audio items inside a playlist — /Playlists/{id}/Items
// ---------------------------------------------------------------------------
bool JellyfinClient::getPlaylistTracks(const std::string& serverUrl,
                                        const JellyfinAuth& auth,
                                        const std::string& playlistId,
                                        std::vector<JellyfinAudioItem>& out) {
    char qs[512];
    snprintf(qs, sizeof(qs),
        "/Playlists/%s/Items?UserId=%s"
        "&Fields=ProductionYear,UserData,RunTimeTicks"
        "&EnableImages=false",
        playlistId.c_str(), auth.userId.c_str());
    std::string url  = serverUrl + qs;
    std::string resp;
    int status = httpRequest(url, "GET", "", "", auth.accessToken, resp);
    if (status != 200) return false;
    struct Ctx { JellyfinClient* self; std::vector<JellyfinAudioItem>* out; };
    Ctx ctx{ this, &out };
    forEachItemObject(resp, [](const std::string& obj, void* vctx) {
        Ctx* c = static_cast<Ctx*>(vctx);
        // Only add audio-type items
        std::string type = c->self->jsonGetString(obj, "Type");
        if (type != "Audio") return;
        JellyfinAudioItem item;
        item.id                    = c->self->jsonGetString(obj, "Id");
        item.name                  = c->self->jsonGetString(obj, "Name");
        item.artist                = c->self->jsonGetString(obj, "AlbumArtist");
        item.album                 = c->self->jsonGetString(obj, "Album");
        item.trackNumber           = c->self->jsonGetInt(obj,    "IndexNumber");
        item.runtimeTicks          = c->self->jsonGetLongLong(obj, "RunTimeTicks");
        item.playbackPositionTicks = c->self->jsonGetLongLong(obj, "PlaybackPositionTicks");
        if (!item.id.empty())
            c->out->push_back(item);
    }, &ctx);
    return true;
}

// ---------------------------------------------------------------------------
// Transcoding URL via PlaybackInfo
// ---------------------------------------------------------------------------

bool JellyfinClient::getTranscodingUrl(const std::string& serverUrl,
                                        const JellyfinAuth& auth,
                                        const std::string& itemId,
                                        const std::string& mediaSourceId,
                                        int audioStreamIndex,
                                        int subtitleStreamIndex,
                                        long long startTimeTicks,
                                        std::string& outUrl,
                                        std::string& outPlaySessionId)
{
    // Snap startTimeTicks 3 seconds back so Jellyfin can output a full GOP before
    // the resume point — guarantees audio and video are aligned at stream start.
    static const long long RESUME_PAD_TICKS = 30000000LL; // 3s in 100ns ticks
    if (startTimeTicks > RESUME_PAD_TICKS)
        startTimeTicks -= RESUME_PAD_TICKS;
    else if (startTimeTicks > 0)
        startTimeTicks = 0;

    // Query string: only DeviceId — all playback control fields go in the body.
    char fullUrl[512];
    snprintf(fullUrl, sizeof(fullUrl),
        "%s/Items/%s/PlaybackInfo?UserId=%s&DeviceId=wiifin-wii",
        serverUrl.c_str(), itemId.c_str(), auth.userId.c_str());

    // EnableDirectPlay and EnableDirectStream MUST be false in the JSON body.
    // Jellyfin reads these from the body; the same-named query params are ignored
    // by the server-side session manager when selecting the play method.
    char bodyBuf[1024];
    // CodecProfiles with LessThanEqual Width/Height Conditions are the only way to
    // enforce output resolution in Jellyfin — MaxWidth/MaxHeight in the top-level
    // body are hints; the internal scale filter uses its own 1280px default unless
    // the device profile declares explicit width/height constraints.
    static const char* codecProfiles =
        "[{\"Type\":\"Video\",\"Codec\":\"mpeg2video\",\"Conditions\":["
        "{\"Condition\":\"LessThanEqual\",\"Property\":\"Width\","
            "\"Value\":\"848\",\"IsRequired\":true},"
        "{\"Condition\":\"LessThanEqual\",\"Property\":\"Height\","
            "\"Value\":\"480\",\"IsRequired\":true}"
        "]}]";

    if (subtitleStreamIndex >= 0) {
        snprintf(bodyBuf, sizeof(bodyBuf),
            "{\"UserId\":\"%s\","
            "\"MediaSourceId\":\"%s\","
            "\"AudioStreamIndex\":%d,"
            "\"SubtitleStreamIndex\":%d,"
            "\"MaxStreamingBitrate\":1000000,"
            "\"MaxWidth\":848,\"MaxHeight\":480,"
            "\"StartTimeTicks\":%lld,"
            "\"IsPlayback\":true,"
            "\"AutoOpenLiveStream\":true,"
            "\"EnableDirectPlay\":false,"
            "\"EnableDirectStream\":false,"
            "\"DeviceProfile\":{"
            "\"DirectPlayProfiles\":[],"
            "\"TranscodingProfiles\":[{\"Container\":\"ts\",\"Type\":\"Video\","
            "\"VideoCodec\":\"mpeg2video\",\"AudioCodec\":\"mp3\","
            "\"Protocol\":\"http\",\"Context\":\"Streaming\","
            "\"MaxAudioChannels\":\"2\",\"MaxFramerate\":24}],"
            "\"CodecProfiles\":%s,\"SubtitleProfiles\":[]}}",
            auth.userId.c_str(), mediaSourceId.c_str(),
            audioStreamIndex, subtitleStreamIndex, (long long)startTimeTicks, codecProfiles);
    } else {
        snprintf(bodyBuf, sizeof(bodyBuf),
            "{\"UserId\":\"%s\","
            "\"MediaSourceId\":\"%s\","
            "\"AudioStreamIndex\":%d,"
            "\"MaxStreamingBitrate\":1000000,"
            "\"MaxWidth\":848,\"MaxHeight\":480,"
            "\"StartTimeTicks\":%lld,"
            "\"IsPlayback\":true,"
            "\"AutoOpenLiveStream\":true,"
            "\"EnableDirectPlay\":false,"
            "\"EnableDirectStream\":false,"
            "\"DeviceProfile\":{"
            "\"DirectPlayProfiles\":[],"
            "\"TranscodingProfiles\":[{\"Container\":\"ts\",\"Type\":\"Video\","
            "\"VideoCodec\":\"mpeg2video\",\"AudioCodec\":\"mp3\","
            "\"Protocol\":\"http\",\"Context\":\"Streaming\","
            "\"MaxAudioChannels\":\"2\",\"MaxFramerate\":24}],"
            "\"CodecProfiles\":%s,\"SubtitleProfiles\":[]}}",
            auth.userId.c_str(), mediaSourceId.c_str(),
            audioStreamIndex, (long long)startTimeTicks, codecProfiles);
    }

    SYS_Report("[PlaybackInfo] POST %s\n", fullUrl);
    SYS_Report("[PlaybackInfo] body: %.256s\n", bodyBuf);

    std::string resp;
    int status = httpRequest(fullUrl, "POST", "application/json", bodyBuf,
                             auth.accessToken, resp);
    SYS_Report("[PlaybackInfo] HTTP status=%d respLen=%d\n", status, (int)resp.size());
    if (status != 200) {
        SYS_Report("[PlaybackInfo] error body: %.256s\n", resp.c_str());
        errMsg = "PlaybackInfo HTTP ";
        char tmp[16]; snprintf(tmp, sizeof(tmp), "%d", status);
        errMsg += tmp;
        return false;
    }

    std::string relUrl = jsonGetString(resp, "TranscodingUrl");
    SYS_Report("[PlaybackInfo] TranscodingUrl present: %s\n", relUrl.empty() ? "no" : "yes");
    if (relUrl.empty()) {
        size_t p = resp.find("TranscodingUrl");
        if (p != std::string::npos)
            SYS_Report("[PlaybackInfo] raw snippet: %.128s\n", resp.c_str() + p);
        else
            SYS_Report("[PlaybackInfo] key not found. First 256: %.256s\n", resp.c_str());
        errMsg = "No TranscodingUrl in PlaybackInfo response";
        return false;
    }

    // Extract PlaySessionId from the response (appears in TranscodingInfo).
    outPlaySessionId = jsonGetString(resp, "PlaySessionId");
    SYS_Report("[PlaybackInfo] PlaySessionId='%s'\n", outPlaySessionId.c_str());

    urlReplaceParam(relUrl, "VideoCodec", "mpeg2video");
    urlReplaceParam(relUrl, "AudioCodec", "mp3");
    urlReplaceParam(relUrl, "AudioBitrate", "192000");
    urlReplaceParam(relUrl, "VideoBitrate", "700000");
    urlReplaceParam(relUrl, "MaxVideoBitDepth", "8");
    urlReplaceParam(relUrl, "MaxWidth", "848");
    urlReplaceParam(relUrl, "MaxHeight", "480");
    urlReplaceParam(relUrl, "MaxFramerate", "24");

    // When subtitles are off (subtitleStreamIndex < 0), Jellyfin may still
    // auto-select the media's default subtitle track and embed
    // SubtitleStreamIndex=N&SubtitleMethod=Encode in the returned URL.
    // Strip both parameters so the transcoder does not burn in any subtitles.
    if (subtitleStreamIndex < 0) {
        urlRemoveParam(relUrl, "SubtitleStreamIndex");
        urlRemoveParam(relUrl, "SubtitleMethod");
    }

    // Always enforce StartTimeTicks in the final URL so Jellyfin's transcoder
    // starts at the correct position even if the PlaybackInfo response omits it.
    if (startTimeTicks > 0) {
        char stBuf[32];
        snprintf(stBuf, sizeof(stBuf), "%lld", startTimeTicks);
        urlReplaceParam(relUrl, "StartTimeTicks", stBuf);
    }

    // TranscodingUrl is a relative path starting with '/'.  Jellyfin sometimes
    // produces a query string starting with "?&" (empty first parameter); fix it.
    outUrl = addScheme(serverUrl) + relUrl;
    {
        auto q = outUrl.find("?&");
        if (q != std::string::npos) outUrl.replace(q, 2, "?");
    }
    SYS_Report("[PlaybackInfo] final URL length: %zu\n", outUrl.size());
    return true;
}

// Playback reporting
// ---------------------------------------------------------------------------

bool JellyfinClient::reportPlaybackStart(const std::string& serverUrl,
                                          const JellyfinAuth& auth,
                                          const std::string& itemId,
                                          const std::string& mediaSourceId,
                                          const std::string& playSessionId)
{
    std::string body =
        "{\"ItemId\":\"" + itemId + "\","
        "\"MediaSourceId\":\"" + mediaSourceId + "\","
        "\"PlaySessionId\":\"" + playSessionId + "\","
        "\"PlayMethod\":\"Transcode\","
        "\"AudioStreamIndex\":1,"
        "\"CanSeek\":false,"
        "\"IsPaused\":false,"
        "\"IsMuted\":false}";

    std::string resp;
    int status = httpRequest(serverUrl + "/Sessions/Playing",
                             "POST", "application/json", body,
                             auth.accessToken, resp);
    // Jellyfin returns 204 No Content on success
    return status == 204 || status == 200;
}

bool JellyfinClient::reportPlaybackProgress(const std::string& serverUrl,
                                             const JellyfinAuth& auth,
                                             const std::string& itemId,
                                             const std::string& mediaSourceId,
                                             const std::string& playSessionId,
                                             long long positionTicks,
                                             bool isPaused)
{
    char ticksBuf[32];
    snprintf(ticksBuf, sizeof(ticksBuf), "%lld", positionTicks);

    std::string body =
        "{\"ItemId\":\"" + itemId + "\","
        "\"MediaSourceId\":\"" + mediaSourceId + "\","
        "\"PlaySessionId\":\"" + playSessionId + "\","
        "\"PlayMethod\":\"Transcode\","
        "\"PositionTicks\":" + ticksBuf + ","
        "\"IsPaused\":" + (isPaused ? "true" : "false") + ","
        "\"IsMuted\":false}";

    std::string resp;
    int status = httpRequest(serverUrl + "/Sessions/Playing/Progress",
                             "POST", "application/json", body,
                             auth.accessToken, resp);
    return status == 204 || status == 200;
}

bool JellyfinClient::reportPlaybackStopped(const std::string& serverUrl,
                                            const JellyfinAuth& auth,
                                            const std::string& itemId,
                                            const std::string& mediaSourceId,
                                            const std::string& playSessionId,
                                            long long positionTicks)
{
    char ticksBuf[32];
    snprintf(ticksBuf, sizeof(ticksBuf), "%lld", positionTicks);

    std::string body =
        "{\"ItemId\":\"" + itemId + "\","
        "\"MediaSourceId\":\"" + mediaSourceId + "\","
        "\"PlaySessionId\":\"" + playSessionId + "\","
        "\"PositionTicks\":" + ticksBuf + "}";

    std::string resp;
    int status = httpRequest(serverUrl + "/Sessions/Playing/Stopped",
                             "POST", "application/json", body,
                             auth.accessToken, resp);
    return status == 204 || status == 200;
}

bool JellyfinClient::deleteActiveEncoding(const std::string& serverUrl,
                                           const JellyfinAuth& auth,
                                           const std::string& playSessionId)
{
    if (playSessionId.empty()) return true;
    std::string url = serverUrl + "/Videos/ActiveEncodings"
                      "?DeviceId=wiifin-wii&PlaySessionId=" + playSessionId;
    std::string resp;
    int status = httpRequest(url, "DELETE", "", "", auth.accessToken, resp);
    SYS_Report("[WiiPlayer] deleteActiveEncoding status=%d\n", status);
    return status == 204 || status == 200;
}

// ---------------------------------------------------------------------------
// Intro / credits timestamps — Jellyfin Intro Skipper plugin
// GET /Episode/{id}/IntroTimestamps
// ---------------------------------------------------------------------------
bool JellyfinClient::getIntroTimestamps(const std::string& serverUrl,
                                         const JellyfinAuth& auth,
                                         const std::string& episodeId,
                                         IntroInfo& out)
{
    out = IntroInfo{};  /* clear output */
    std::string url = serverUrl + "/Episode/" + episodeId + "/IntroTimestamps";
    std::string resp;
    int status = httpRequest(url, "GET", "", "", auth.accessToken, resp);
    if (status == 404) return true;   /* plugin not installed: not an error */
    if (status != 200) return true;   /* any other failure: silent, no intro */

    /* Expected JSON:
     * {
     *   "Valid": true,
     *   "IntroStart": 60.0,
     *   "IntroEnd":  120.0,
     *   "ShowSkipPromptAt": 55.0,
     *   "HideSkipPromptAt": 125.0
     * }
     * Use a simple string search avoiding full JSON parse.
     */
    auto getFloat = [&](const std::string& key) -> float {
        std::string search = "\"" + key + "\":";
        size_t p = resp.find(search);
        if (p == std::string::npos) return 0.0f;
        p += search.size();
        while (p < resp.size() && (resp[p] == ' ' || resp[p] == '\t')) ++p;
        return (float)atof(resp.c_str() + p);
    };

    bool valid = jsonGetBool(resp, "Valid");
    if (!valid) return true;

    out.hasIntro     = true;
    out.introStart   = getFloat("IntroStart");
    out.introEnd     = getFloat("IntroEnd");
    out.showPromptAt = getFloat("ShowSkipPromptAt");
    out.hidePromptAt = getFloat("HideSkipPromptAt");

    /* Clamp to sane range */
    if (out.introEnd <= out.introStart) { out = IntroInfo{}; }
    return true;
}

// ---------------------------------------------------------------------------
// Fetch Audio tracks for a MusicAlbum
// GET /Users/{userId}/Items?ParentId={albumId}&IncludeItemTypes=Audio
// ---------------------------------------------------------------------------
bool JellyfinClient::getAlbumTracks(const std::string& serverUrl,
                                     const JellyfinAuth& auth,
                                     const std::string& albumId,
                                     std::vector<JellyfinAudioItem>& out)
{
    char qs[512];
    snprintf(qs, sizeof(qs),
        "/Users/%s/Items?ParentId=%s"
        "&IncludeItemTypes=Audio"
        "&SortBy=SortName&SortOrder=Ascending"
        "&Fields=TrackNumber,RunTimeTicks,UserData,AlbumArtist,Album"
        "&EnableImages=false&Limit=500",
        auth.userId.c_str(), albumId.c_str());
    std::string url  = serverUrl + qs;
    std::string resp;
    int status = httpRequest(url, "GET", "", "", auth.accessToken, resp);
    if (status != 200) {
        if (status >= 0) errMsg = "getAlbumTracks failed (HTTP " + std::to_string(status) + ")";
        return false;
    }

    struct Ctx { JellyfinClient* self; std::vector<JellyfinAudioItem>* out; };
    Ctx ctx{ this, &out };
    forEachItemObject(resp, [](const std::string& obj, void* vctx) {
        Ctx* c = static_cast<Ctx*>(vctx);
        JellyfinAudioItem item;
        item.id                    = c->self->jsonGetString(obj, "Id");
        item.name                  = c->self->jsonGetString(obj, "Name");
        item.artist                = c->self->jsonGetString(obj, "AlbumArtist");
        item.album                 = c->self->jsonGetString(obj, "Album");
        item.trackNumber           = c->self->jsonGetInt(obj, "IndexNumber");
        item.runtimeTicks          = c->self->jsonGetLongLong(obj, "RunTimeTicks");
        item.playbackPositionTicks = c->self->jsonGetLongLong(obj, "PlaybackPositionTicks");
        if (!item.id.empty()) c->out->push_back(item);
    }, &ctx);

    return true;
}

// ---------------------------------------------------------------------------
// Build a direct audio stream URL via POST /Items/{id}/PlaybackInfo.
// Requests an audio-only MP3 transcode so MPlayer can play it without video.
// ---------------------------------------------------------------------------
bool JellyfinClient::getAudioStreamUrl(const std::string& serverUrl,
                                        const JellyfinAuth& auth,
                                        const std::string& itemId,
                                        long long startTimeTicks,
                                        std::string& outUrl,
                                        std::string& outPlaySessionId)
{
    /* Step 1: POST PlaybackInfo with AutoOpenLiveStream:false to obtain a
     * PlaySessionId only, without opening any server-side transcode session.
     * We do NOT use the TranscodingUrl from this response — for audio items
     * Jellyfin often returns a video container URL (TS MPEG2VIDEO) which is
     * wrong.  Instead we build the /Audio/universal URL ourselves (step 2). */
    char fullUrl[512];
    snprintf(fullUrl, sizeof(fullUrl),
        "%s/Items/%s/PlaybackInfo"
        "?UserId=%s&DeviceId=wiifin-wii",
        serverUrl.c_str(), itemId.c_str(), auth.userId.c_str());

    char bodyBuf[512];
    snprintf(bodyBuf, sizeof(bodyBuf),
        "{\"UserId\":\"%s\","
        "\"MediaSourceId\":\"%s\","
        "\"StartTimeTicks\":%lld,"
        "\"IsPlayback\":true,"
        "\"AutoOpenLiveStream\":false,"
        "\"EnableDirectPlay\":false,"
        "\"EnableDirectStream\":false}",
        auth.userId.c_str(), itemId.c_str(), (long long)startTimeTicks);

    SYS_Report("[AudioPlaybackInfo] POST %s\n", fullUrl);

    std::string resp;
    int status = httpRequest(fullUrl, "POST", "application/json", bodyBuf,
                             auth.accessToken, resp);
    if (status != 200) {
        errMsg = "AudioPlaybackInfo HTTP ";
        char tmp[16]; snprintf(tmp, sizeof(tmp), "%d", status);
        errMsg += tmp;
        return false;
    }

    outPlaySessionId = jsonGetString(resp, "PlaySessionId");
    if (outPlaySessionId.empty()) {
        SYS_Report("[AudioPlaybackInfo] No PlaySessionId in response (first 256): %.256s\n",
                   resp.c_str());
        errMsg = "No PlaySessionId in AudioPlaybackInfo response";
        return false;
    }

    /* Step 2: Build a direct /Audio/{id}/universal URL.
     * MPlayer CE decodes MP3 in software on the PowerPC, so any standard
     * MP3 bitrate (including 320 kbps) plays natively without transcoding.
     * MaxAudioBitRate=320000 lets Jellyfin direct-stream the source file
     * when it is already MP3 ≤320 kbps; it will only transcode to 320 kbps
     * when the source uses a different codec (e.g. FLAC, AAC, OGG). */
    char audioUrl[1024];
    std::string schemedSvr = addScheme(serverUrl);
    snprintf(audioUrl, sizeof(audioUrl),
        "%s/Audio/%s/universal"
        "?UserId=%s"
        "&DeviceId=wiifin-wii"
        "&PlaySessionId=%s"
        "&MediaSourceId=%s"
        "&Container=mp3"
        "&AudioCodec=mp3"
        "&MaxAudioBitRate=320000"
        "&MaxAudioChannels=2"
        "&TranscodingContainer=mp3"
        "&TranscodingProtocol=http"
        "&StartTimeTicks=%lld"
        "&api_key=%s",
        schemedSvr.c_str(), itemId.c_str(),
        auth.userId.c_str(),
        outPlaySessionId.c_str(),
        itemId.c_str(),
        (long long)startTimeTicks,
        auth.accessToken.c_str());
    outUrl = audioUrl;

    SYS_Report("[AudioPlaybackInfo] sessionId=%s\n", outPlaySessionId.c_str());
    return true;
}

// ---------------------------------------------------------------------------
// Search items across all libraries via /Search/Hints (covers MusicArtist too)
// ---------------------------------------------------------------------------
bool JellyfinClient::searchItems(const std::string& serverUrl,
                                  const JellyfinAuth& auth,
                                  const std::string& searchTerm,
                                  int limit,
                                  std::vector<JellyfinItem>& out) {
    // Percent-encode the search term (printable ASCII only)
    std::string encoded;
    encoded.reserve(searchTerm.size() * 3);
    for (unsigned char c : searchTerm) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += (char)c;
        } else {
            char hex[4];
            snprintf(hex, sizeof(hex), "%%%02X", c);
            encoded += hex;
        }
    }

    char qs[512];
    snprintf(qs, sizeof(qs),
        "/Search/Hints"
        "?searchTerm=%s"
        "&userId=%s"
        "&limit=%d"
        "&includeItemTypes=Movie,Series,Episode,MusicAlbum,Audio,MusicArtist,BoxSet,Playlist",
        encoded.c_str(), auth.userId.c_str(), limit);

    std::string url  = serverUrl + qs;
    std::string resp;
    int status = httpRequest(url, "GET", "", "", auth.accessToken, resp);
    if (status != 200) {
        if (status >= 0) errMsg = "searchItems failed (HTTP " + std::to_string(status) + ")";
        return false;
    }

    // /Search/Hints returns { "SearchHints": [...], "TotalRecordCount": N }
    // Each hint uses "ItemId" (not "Id"), and fields differ slightly from Items.
    size_t arrPos = resp.find("\"SearchHints\":");
    if (arrPos == std::string::npos) return true; // empty but valid
    arrPos = resp.find('[', arrPos);
    if (arrPos == std::string::npos) return true;
    arrPos++;

    while (arrPos < resp.size()) {
        while (arrPos < resp.size() && resp[arrPos] != '{' && resp[arrPos] != ']') arrPos++;
        if (arrPos >= resp.size() || resp[arrPos] == ']') break;

        // find matching '}'
        int depth = 0;
        bool inStr = false;
        size_t objStart = arrPos;
        for (; arrPos < resp.size(); arrPos++) {
            char ch = resp[arrPos];
            if (inStr) {
                if      (ch == '\\') { arrPos++; }
                else if (ch == '"')  { inStr = false; }
            } else {
                if      (ch == '"') { inStr = true; }
                else if (ch == '{') { depth++; }
                else if (ch == '}') { if (--depth == 0) { arrPos++; break; } }
            }
        }
        const std::string obj = resp.substr(objStart, arrPos - objStart);

        JellyfinItem item;
        item.id            = jsonGetString(obj, "ItemId");   // Search/Hints uses ItemId
        item.name          = jsonGetString(obj, "Name");
        item.type          = jsonGetString(obj, "Type");
        item.year          = jsonGetInt(obj, "ProductionYear");
        item.seriesName    = jsonGetString(obj, "Series");   // field name is "Series" in hints
        item.seriesId      = jsonGetString(obj, "SeriesId");
        item.seasonNumber  = jsonGetInt(obj, "ParentIndexNumber");
        item.episodeNumber = jsonGetInt(obj, "IndexNumber");
        item.runtimeTicks  = jsonGetLongLong(obj, "RunTimeTicks");
        if (!item.id.empty() && !item.name.empty())
            out.push_back(item);
    }
    return true;
}
