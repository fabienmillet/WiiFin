#include "JellyfinClient.h"
#include <algorithm>
#include "../core/ExitZone.h"
#include "../core/Log.h"
#include "../core/NetConnect.h"
#include "../player/WiiPlayer.h"   /* the volume, for the reports */
#include <network.h>
#include <ogc/if_config.h>
#include <ogc/lwp.h>
#include <ogc/mutex.h>
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
/* Sent with every request; one per console, see setDeviceId() */
static std::string s_deviceId = "wiifin-wii";

void JellyfinClient::setDeviceId(const std::string& id) { if (!id.empty()) s_deviceId = id; }
const std::string& JellyfinClient::deviceId() { return s_deviceId; }

#include <ogcsys.h>

// ---------------------------------------------------------------------------
// mbedTLS BIO callbacks: wrap libogc net_read / net_write
// ---------------------------------------------------------------------------
namespace {

void connLockInit();   /* below, with the kept-alive connection */

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

void JellyfinClient::bringUpNetwork() {
    /* The Wii's network stack sometimes needs several tries (slow DHCP,
     * NWC24 still starting, a stale IOS state from the previous app).  The
     * gateway is not asked for: libogc fails the whole call when the default
     * route is not in the table yet, although the network works. */
    s32 ret = -1;
    for (int attempt = 0; attempt < 4; ++attempt) {
        char ip[16] = "", mask[16] = "";
        ret = if_config(ip, mask, nullptr, true, 20);
        SYS_Report("[Net] attempt %d: if_config=%d ip=%s\n", attempt + 1, (int)ret, ip);
        if (ret >= 0 && ip[0] && strcmp(ip, "0.0.0.0") != 0) {
            localIp_   = ip;
            localMask_ = mask;
            networkReady = true;
            return;
        }
        net_deinit();                 /* start over from a clean state */
        usleep(1000 * 1000);
    }
    char buf[96];
    if (ret >= 0) snprintf(buf, sizeof(buf), "No IP address from the router (DHCP)");
    else          snprintf(buf, sizeof(buf), "Network start-up failed (error %d)", (int)ret);
    errMsg = buf;
}

void* JellyfinClient::netThreadMain(void* self) {
    JellyfinClient* c = static_cast<JellyfinClient*>(self);
    c->bringUpNetwork();
    c->netBusy = false;
    return nullptr;
}

void JellyfinClient::startNetwork() {
    connLockInit();   /* at boot, before any other thread makes a request */
    if (networkReady || netBusy) return;
    if (netThread) {                  /* previous attempt finished: reap it */
        LWP_JoinThread(netThread, nullptr);
        netThread = 0;
    }
    static u8 stack[32 * 1024] __attribute__((aligned(32)));
    lwp_t t = LWP_THREAD_NULL;
    netBusy = true;
    if (LWP_CreateThread(&t, netThreadMain, this, stack, sizeof(stack), 40) < 0) {
        netBusy = false;
        bringUpNetwork();             /* no thread: do it here */
        return;
    }
    netThread = t;
}

bool JellyfinClient::takeNetworkResult() {
    if (netThread && !netBusy) {
        LWP_JoinThread(netThread, nullptr);
        netThread = 0;
    }
    return networkReady;
}

bool JellyfinClient::initNetwork() {
    if (takeNetworkResult()) return true;
    startNetwork();                   /* a new attempt, unless one is running */
    if (netThread) {
        LWP_JoinThread(netThread, nullptr);
        netThread = 0;
    }
    return networkReady;
}

bool JellyfinClient::discoverServers(std::vector<DiscoveredServer>& out) {
    out.clear();

    s32 sock = net_socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        errMsg = "UDP socket failed";
        SYS_Report("[Discover] socket: %d\n", (int)sock);
        return false;
    }

    // Enable broadcast
    u32 broadcastOn = 1;
    s32 r = net_setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcastOn, sizeof(broadcastOn));
    if (r < 0) SYS_Report("[Discover] SO_BROADCAST: %d\n", (int)r);

    /* Bound to a port of its own: the servers answer to the port the question
     * came from, and the Wii's IOS does not always pick one on sendto.  A
     * port given: the console's IOS refuses port 0 (EINVAL; Dolphin takes
     * it).  One from the dynamic range, another if it is taken. */
    for (int tries = 0; tries < 4; ++tries) {
        struct sockaddr_in local;
        memset(&local, 0, sizeof(local));
        local.sin_family      = AF_INET;
        local.sin_port        = htons((u16)(49152 + (gettime() + tries * 4099) % 16384));
        local.sin_addr.s_addr = INADDR_ANY;
        r = net_bind(sock, (struct sockaddr*)&local, (socklen_t)sizeof(local));
        if (r >= 0) break;
        SYS_Report("[Discover] bind: %d\n", (int)r);
    }
    /* Non-blocking: the answers are read by polling as well as through
     * net_select, which IOS does not always wake up for UDP. */
    u32 nb = 1;
    net_ioctl(sock, FIONBIO, &nb);

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
    bool haveSubnet = false;
    if (!localIp_.empty() && !localMask_.empty()) {
        u32 ip4   = ntohl(inet_addr(localIp_.c_str()));
        u32 mask4 = ntohl(inet_addr(localMask_.c_str()));
        if (ip4 != 0xFFFFFFFFu && mask4 != 0xFFFFFFFFu && mask4 != 0) {
            destSubnet.sin_addr.s_addr = htonl((ip4 & mask4) | (~mask4));
            haveSubnet = true;
        }
    }

    SYS_Report("[Discover] from %s, mask %s\n", localIp_.c_str(), localMask_.c_str());

    /* The address length is 8, not sizeof: libogc's net_sendto writes it
     * into the address as its sa_len, and the console's IOS takes 8 only
     * (EINVAL on each question otherwise; Dolphin takes 16). */
    const socklen_t IOS_ADDR_LEN = 8;
    const char* msg = "Who is JellyfinServer?";
    auto sendBroadcast = [&](int round) {
        s32 a = net_sendto(sock, msg, (s32)strlen(msg), 0,
                           (struct sockaddr*)&dest255, IOS_ADDR_LEN);
        s32 b = haveSubnet ? net_sendto(sock, msg, (s32)strlen(msg), 0,
                                        (struct sockaddr*)&destSubnet, IOS_ADDR_LEN)
                           : 0;
        SYS_Report("[Discover] question %d: to all %d, to the subnet %d\n", round, (int)a, (int)b);
    };
    sendBroadcast(1);

    // Time-based collection using gettime() — independent of net_select resolution.
    // • Exit 300 ms after the first server found (other servers answer too).
    // • Hard cap: 3000 ms if nothing found, asking again at 800 and 1600 ms
    //   (a Wi-Fi link drops a broadcast now and then).
    const u32 MAX_MS   = 3000;
    const u32 FOUND_MS = 300;
    u64 startTicks = gettime();
    u64 foundTicks = 0;
    int round = 1;
    char buf[1024];

    while (true) {
        u32 elapsedMs = (u32)ticks_to_millisecs(gettime() - startTicks);
        if (elapsedMs >= MAX_MS) break;
        if (foundTicks > 0 && (u32)ticks_to_millisecs(gettime() - foundTicks) >= FOUND_MS)
            break;

        if (out.empty() && round < 3 && elapsedMs >= (u32)round * 800) sendBroadcast(++round);

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);
        struct timeval tv = {0, 50000};
        net_select(sock + 1, &rfds, nullptr, nullptr, &tv);   /* a wait, at most 50 ms */

        /* every answer waiting, whatever select said */
        for (;;) {
            struct sockaddr_in from;
            socklen_t fromLen = IOS_ADDR_LEN;   /* as for sendto */
            int n = (int)net_recvfrom(sock, buf, (s32)(sizeof(buf) - 1), 0,
                                      (struct sockaddr*)&from, &fromLen);
            if (n <= 0) break;
            buf[n] = '\0';

            std::string json(buf, n);
            std::string address = jsonGetString(json, "Address");
            std::string name    = jsonGetString(json, "Name");
            SYS_Report("[Discover] answer of %d bytes: %s\n", n,
                       address.empty() ? "no address in it" : "a server");
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

/* Ensure a server URL has an http:// or https:// scheme prefix.
 * If the URL already contains "://" but is not http/https, return it unchanged
 * (parseUrl will then reject it with an unsupported-scheme error). */
static std::string addScheme(const std::string& url) {
    const std::string n = normScheme(url);
    if (n.size() >= 7 && n.compare(0, 7, "http://")  == 0) return n;
    if (n.size() >= 8 && n.compare(0, 8, "https://") == 0) return n;
    // If there's already a "://" it's an unsupported scheme — don't prepend
    if (n.find("://") != std::string::npos) return n;
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
        // Reject any other scheme (ftp://, etc.) — only truly scheme-less URLs
        // (no "://" present) are accepted here for auto-detection.
        if (u.find("://") != std::string::npos) {
            errMsg = "Unsupported URL scheme";
            return false;
        }
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
static std::string decodeJsonString(const std::string& s, bool newlines = false) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char nc = s[++i];
            switch (nc) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'n':  if (newlines) out += '\n'; break; // names: none
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

std::string JellyfinClient::jsonGetString(const std::string& json, const std::string& key, bool newlines) {
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
    return decodeJsonString(raw, newlines);
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
void forEachArrayObject(const std::string& json, size_t pos,
                        void (*cb)(const std::string&, void*), void* ctx);

void forEachItemObject(const std::string& json,
                       void (*cb)(const std::string&, void*), void* ctx) {
    size_t pos = json.find("\"Items\":");
    if (pos == std::string::npos) return;
    pos = json.find('[', pos);
    if (pos == std::string::npos) return;
    forEachArrayObject(json, pos, cb, ctx);
}

// Calls cb(objectString) for each top-level object of the array whose '['
// is at pos.
void forEachArrayObject(const std::string& json, size_t pos,
                        void (*cb)(const std::string&, void*), void* ctx) {
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
                                 std::string& responseBody,
                                 size_t maxResponse) {
    std::string host, basePath;
    int port;
    bool isHttps;
    if (!parseUrl(url, host, port, basePath, isHttps)) return -1;
    return request(isHttps, host, port, basePath, method, contentType, body,
                   authToken, responseBody, maxResponse);
}

// Wii RTC is unreliable and many home Jellyfin servers use self-signed certificates
// or private CAs that are not in the embedded CA bundle.
// We clear date-related flags (clock not trusted) and NOT_TRUSTED (CA chain not trusted)
// so that self-signed / private-CA certs work. CN/SAN hostname verification is still
// enforced by mbedtls_ssl_set_hostname() above, which prevents MITM: an attacker on
// the local network would need a certificate valid for the exact server hostname.
// REVOKED and CN_MISMATCH are intentionally kept.
static int wii_cert_verify(void*, mbedtls_x509_crt*, int, uint32_t* flags);

/* Shared with the MPlayer stream module (stream_wiifin.cpp) */
int wiifin_cert_verify(void* ctx, mbedtls_x509_crt* crt, int depth, uint32_t* flags) {
    return wii_cert_verify(ctx, crt, depth, flags);
}

static int wii_cert_verify(void*, mbedtls_x509_crt*, int, uint32_t* flags) {
    *flags &= ~(uint32_t)(MBEDTLS_X509_BADCERT_EXPIRED    | MBEDTLS_X509_BADCERT_FUTURE    |
                          MBEDTLS_X509_BADCRL_EXPIRED      | MBEDTLS_X509_BADCRL_FUTURE     |
                          MBEDTLS_X509_BADCERT_NOT_TRUSTED | MBEDTLS_X509_BADCRL_NOT_TRUSTED);
    return 0;
}

// ---------------------------------------------------------------------------
// HTTP/HTTPS transport
//
// Setting up a connection is expensive on the Wii (TCP connect through IOS,
// plus a TLS handshake and CA-bundle parse for HTTPS), so:
//   - entropy, DRBG, SSL config and CA chain are initialised once;
//   - the connection is kept alive (HTTP/1.1) and reused by the next request
//     to the same server, which skips both the connect and the handshake.
// ---------------------------------------------------------------------------
namespace {

struct TlsShared {
    bool                     seeded     = false;
    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context drbg;
    bool                     caLoaded   = false;
    mbedtls_x509_crt         ca;
    bool                     confReady  = false;
    bool                     confVerify = false;
    mbedtls_ssl_config       conf;
};

struct Conn {
    bool                open   = false;
    bool                tls    = false;
    std::string         host;
    int                 port   = 0;
    bool                verify = false;
    s32                 sock   = -1;   /* TLS BIO context points here */
    mbedtls_ssl_context ssl;
    u64                 lastUseMs = 0;
};

TlsShared s_tls;
Conn      s_conn;

/* Reconnect instead of reusing a connection idle for longer than this:
 * servers drop idle keep-alive connections (nginx 75 s, Kestrel 130 s). */
const u64 IDLE_REUSE_MS = 30000;

u64 nowMs() { return ticks_to_millisecs(gettime()); }

void closeConn() {
    if (!s_conn.open) return;
    /* No close_notify: the server may have dropped it already (an idle
     * keep-alive connection); wii_player_abort_io leaves it alone. */
    if (s_conn.tls) mbedtls_ssl_free(&s_conn.ssl);
    wii_player_keep_socket(1, -1);
    net_close(s_conn.sock);
    s_conn.open = false;
    s_conn.sock = -1;
}

/* One request at a time on s_conn: during playback the trickplay loader and
 * MPlayer's "stream opened" report run beside the main thread.  Recursive:
 * a request may recover the network, which closes the connection. */
mutex_t s_connLock = LWP_MUTEX_NULL;

void connLockInit() {
    if (s_connLock == LWP_MUTEX_NULL) LWP_MutexInit(&s_connLock, true);
}

struct ConnGuard {
    ConnGuard()  { connLockInit(); LWP_MutexLock(s_connLock); }
    ~ConnGuard() { LWP_MutexUnlock(s_connLock); }
};

bool loadCa(std::string& err) {
    if (s_tls.caLoaded) return true;
    mbedtls_x509_crt_init(&s_tls.ca);
    // data_cacert_pem has a null terminator embedded (len includes it)
    int parseRet = mbedtls_x509_crt_parse(&s_tls.ca, data_cacert_pem, data_cacert_pem_len);
    if (parseRet < 0) {
        mbedtls_x509_crt_free(&s_tls.ca);
        err = "TLS: CA bundle parse failed (" + std::to_string(parseRet) + ")";
        return false;
    }
    s_tls.caLoaded = true;
    return true;
}

bool tlsSharedInit(bool verify, std::string& err) {
    if (!s_tls.seeded) {
        mbedtls_entropy_init(&s_tls.entropy);
        mbedtls_ctr_drbg_init(&s_tls.drbg);
        const char* pers = "wiifin_jellyfin";
        if (mbedtls_ctr_drbg_seed(&s_tls.drbg, mbedtls_entropy_func, &s_tls.entropy,
                                  (const unsigned char*)pers, strlen(pers)) != 0) {
            mbedtls_ctr_drbg_free(&s_tls.drbg);
            mbedtls_entropy_free(&s_tls.entropy);
            err = "TLS: DRBG seed failed";
            return false;
        }
        s_tls.seeded = true;
    }
    if (verify && !loadCa(err)) return false;
    if (!s_tls.confReady || s_tls.confVerify != verify) {
        if (s_tls.confReady) {
            if (s_conn.tls) closeConn();   /* the SSL context references the old config */
            mbedtls_ssl_config_free(&s_tls.conf);
            s_tls.confReady = false;
        }
        mbedtls_ssl_config_init(&s_tls.conf);
        if (mbedtls_ssl_config_defaults(&s_tls.conf, MBEDTLS_SSL_IS_CLIENT,
                                        MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
            mbedtls_ssl_config_free(&s_tls.conf);
            err = "TLS: ssl_config_defaults failed";
            return false;
        }
        if (verify) {
            mbedtls_ssl_conf_ca_chain(&s_tls.conf, &s_tls.ca, nullptr);
            mbedtls_ssl_conf_authmode(&s_tls.conf, MBEDTLS_SSL_VERIFY_REQUIRED);
            mbedtls_ssl_conf_verify(&s_tls.conf, wii_cert_verify, nullptr);
        } else {
            mbedtls_ssl_conf_authmode(&s_tls.conf, MBEDTLS_SSL_VERIFY_NONE);
        }
        mbedtls_ssl_conf_rng(&s_tls.conf, mbedtls_ctr_drbg_random, &s_tls.drbg);
        s_tls.confReady  = true;
        s_tls.confVerify = verify;
    }
    return true;
}

/* Resolve host (IPv4 literal or DNS) into addr. */
bool resolve(const std::string& host, int port, struct sockaddr_in& addr, std::string& err) {
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_aton(host.c_str(), &addr.sin_addr) != 0) return true;
    struct hostent* he = net_gethostbyname(host.c_str());
    if (!he) { err = "DNS failed: " + host; return false; }
    if (he->h_length > (int)sizeof(addr.sin_addr)) { err = "DNS: unexpected address length"; return false; }
    memcpy(&addr.sin_addr, he->h_addr, he->h_length);
    return true;
}

/* net_socket()'s error when the last openConn failed there: ENETRESET /
 * ENETDOWN mean IOS dropped the network (Wi-Fi link lost) and every new
 * socket fails until the network is brought up again. */
s32 s_lastSocketErr = 0;

/* Open a TCP connection (and run the TLS handshake if tls) into s_conn.
 * transient = true for IOS I/O errors worth retrying. */
bool openConn(bool tls, const struct sockaddr_in& addr, const std::string& host, int port,
              bool verify, std::string& err, bool& transient) {
    transient = false;
    s32 sock = net_socket(AF_INET, SOCK_STREAM, 0);
    s_lastSocketErr = sock < 0 ? sock : 0;
    if (sock < 0) { err = "socket() failed (" + std::to_string(sock) + ")"; return false; }

    /* 8 s: a home server answers in milliseconds; past that it is down or
     * the address is wrong, and the user should hear about it */
    int cr = connectWithTimeout(sock, const_cast<struct sockaddr_in*>(&addr), 8);
    if (cr < 0) {
        err = cr == -ETIMEDOUT ? "Server not reachable (connection timed out)"
                               : "connect() failed (" + std::to_string(cr) + ")";
        net_close(sock);
        return false;
    }

    s_conn.sock = sock;
    if (tls) {
        u64 t0 = nowMs();
        mbedtls_ssl_init(&s_conn.ssl);
        int ret = mbedtls_ssl_setup(&s_conn.ssl, &s_tls.conf);
        if (ret == 0) {
            mbedtls_ssl_set_hostname(&s_conn.ssl, host.c_str());
            mbedtls_ssl_set_bio(&s_conn.ssl, &s_conn.sock, wii_tls_send, wii_tls_recv, nullptr);
            do {
                ret = mbedtls_ssl_handshake(&s_conn.ssl);
            } while (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE);
        }
        if (ret != 0) {
            /* NET_SEND_FAILED (0x4C) / NET_RECV_FAILED (0x4E): transient IOS I/O error
             * from MPlayer CE's orphaned socket/cache thread after longjmp.  Retry. */
            if (ret == MBEDTLS_ERR_NET_SEND_FAILED || ret == MBEDTLS_ERR_NET_RECV_FAILED) {
                SYS_Report("[TLS] transient handshake err -0x%04X\n", (unsigned)(-ret));
                transient = true;
                err = "TLS handshake failed (network error)";
            } else {
                uint32_t vflags = mbedtls_ssl_get_verify_result(&s_conn.ssl);
                char ebuf[80];
                mbedtls_strerror(ret, ebuf, sizeof(ebuf));
                char vbuf[24];
                snprintf(vbuf, sizeof(vbuf), " [f=%08X]", (unsigned)vflags);
                err = std::string("TLS handshake failed: ") + ebuf + vbuf;
            }
            mbedtls_ssl_free(&s_conn.ssl);
            net_close(sock);
            s_conn.sock = -1;
            return false;
        }
        SYS_Report("[TLS] handshake with %s:%d in %llu ms (%s)\n", host.c_str(), port,
                   nowMs() - t0, mbedtls_ssl_get_ciphersuite(&s_conn.ssl));
    }
    wii_player_keep_socket(1, sock);   /* MPlayer's clean-up leaves it */
    s_conn.open      = true;
    s_conn.tls       = tls;
    s_conn.host      = host;
    s_conn.port      = port;
    s_conn.verify    = verify;
    s_conn.lastUseMs = nowMs();
    return true;
}

/* Write everything; false on error. */
bool connWrite(const unsigned char* p, int n) {
    while (n > 0) {
        int w;
        if (s_conn.tls) {
            w = mbedtls_ssl_write(&s_conn.ssl, p, (size_t)n);
            if (w == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        } else {
            w = wii_tls_send(&s_conn.sock, p, (size_t)n);
            if (w == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        }
        if (w <= 0) return false;
        p += w; n -= w;
    }
    return true;
}

/* Read some bytes: > 0 bytes read, <= 0 connection closed / error / timeout. */
int connRead(unsigned char* buf, int n) {
    for (;;) {
        int r = s_conn.tls ? mbedtls_ssl_read(&s_conn.ssl, buf, (size_t)n)
                           : wii_tls_recv(&s_conn.sock, buf, (size_t)n);
        if (r == MBEDTLS_ERR_SSL_WANT_READ) continue;
        return r;
    }
}

/* Case-insensitive lookup of a header value ("" if absent). */
std::string headerValue(const char* headers, int len, const char* name) {
    int nlen = (int)strlen(name);
    for (int i = 0; i + nlen < len; ++i) {
        if (i > 0 && headers[i - 1] != '\n') continue;
        bool match = true;
        for (int j = 0; j < nlen && match; ++j) {
            char c = headers[i + j];
            if (c >= 'A' && c <= 'Z') c += 32;
            if (c != name[j]) match = false;
        }
        if (!match || headers[i + nlen] != ':') continue;
        int v = i + nlen + 1;
        while (v < len && headers[v] == ' ') ++v;
        int e = v;
        while (e < len && headers[e] != '\r' && headers[e] != '\n') ++e;
        std::string out(headers + v, (size_t)(e - v));
        for (char& c : out) if (c >= 'A' && c <= 'Z') c += 32;
        return out;
    }
    return "";
}

/* True once a chunked body has been received up to its final 0-size chunk. */
bool chunkedComplete(const char* body, int len) {
    int pos = 0;
    while (pos < len) {
        int crlf = pos;
        while (crlf + 1 < len && !(body[crlf] == '\r' && body[crlf + 1] == '\n')) crlf++;
        if (crlf + 1 >= len) return false;
        long size = strtol(body + pos, nullptr, 16);
        pos = crlf + 2;
        if (size == 0) {
            /* Optional trailers end with an empty line */
            if (pos + 2 <= len && body[pos] == '\r' && body[pos + 1] == '\n') return true;
            for (int i = pos; i + 3 < len; ++i)
                if (body[i] == '\r' && body[i + 1] == '\n' && body[i + 2] == '\r' && body[i + 3] == '\n')
                    return true;
            return false;
        }
        pos += (int)size + 2;
    }
    return false;
}

enum class ExResult { Ok, Stale, Failed };

/* Send one request on s_conn and read the full response.
 * Stale = the reused connection was already closed by the server (nothing
 * received): the caller reconnects and sends the request again. */
ExResult exchange(const char* req, int reqLen, const std::string& body, bool reused,
                  int& status, std::string& responseBody, bool& keepAlive,
                  std::string& err, size_t maxResponse) {
    if (!connWrite((const unsigned char*)req, reqLen) ||
        (!body.empty() && !connWrite((const unsigned char*)body.data(), (int)body.size()))) {
        if (reused) return ExResult::Stale;
        err = "Failed to send request";
        return ExResult::Failed;
    }

    /* The response goes to a static buffer: no heap for the usual small
     * ones.  A request allowing more (maxResponse) moves on to the heap
     * once it is full, doubling the room up to that size. */
    static char rawBuf[256 * 1024] DEAD_AT_EXIT;
    char*  raw  = rawBuf;
    size_t cap  = sizeof(rawBuf);
    char*  heap = nullptr;
    struct Free { char*& p; ~Free() { free(p); } } freeHeap{heap};
    int  rawLen     = 0;
    int  bodyStart  = -1;     /* offset of the body once headers are parsed */
    long contentLen = -1;
    bool chunked    = false;
    bool noBody     = false;
    bool complete   = false;
    keepAlive = true;

    unsigned char buf[4096];
    while (!complete) {
        int ret = connRead(buf, sizeof(buf));
        if (ret <= 0) {
            /* Connection ended (close_notify, EOF, error or timeout) */
            keepAlive = false;
            if (rawLen == 0 && reused) return ExResult::Stale;
            break;
        }
        if ((size_t)(rawLen + ret) >= cap && cap < maxResponse) {
            size_t ncap = cap * 2 > maxResponse ? maxResponse : cap * 2;
            char* grown = (char*)(heap ? realloc(heap, ncap) : malloc(ncap));
            if (grown) {
                if (!heap) memcpy(grown, rawBuf, (size_t)rawLen);
                heap = raw = grown;
                cap  = ncap;
            }
        }
        int canCopy = (int)(cap - 1) - rawLen;
        if (ret < canCopy) canCopy = ret;
        if (canCopy > 0) {
            memcpy(raw + rawLen, buf, (size_t)canCopy);
            rawLen += canCopy;
        }
        if (canCopy < ret) {
            /* response too large: cut, and the connection with it (the rest
             * of this read is lost: waiting for more would only time out) */
            SYS_Report("[HTTP] response over %u KB, cut\n", (unsigned)(cap / 1024));
            keepAlive = false;
            break;
        }

        if (bodyStart < 0) {
            raw[rawLen] = '\0';
            const char* sep = strstr(raw, "\r\n\r\n");
            if (!sep) continue;
            bodyStart = (int)(sep - raw) + 4;
            if (strncmp(raw, "HTTP/", 5) == 0) {
                const char* sp = strchr(raw, ' ');
                if (sp) status = atoi(sp + 1);
            }
            std::string te = headerValue(raw, bodyStart, "transfer-encoding");
            std::string cl = headerValue(raw, bodyStart, "content-length");
            std::string cn = headerValue(raw, bodyStart, "connection");
            chunked    = te.find("chunked") != std::string::npos;
            contentLen = cl.empty() ? -1 : atol(cl.c_str());
            noBody     = status == 204 || status == 304 || (status >= 100 && status < 200);
            if (cn.find("close") != std::string::npos ||
                strncmp(raw, "HTTP/1.0", 8) == 0) keepAlive = false;
            /* Without a length the body ends when the server closes */
            if (!chunked && contentLen < 0 && !noBody) keepAlive = false;
        }
        int have = rawLen - bodyStart;
        if (noBody)                complete = true;
        else if (chunked)          complete = chunkedComplete(raw + bodyStart, have);
        else if (contentLen >= 0)  complete = have >= contentLen;
    }
    raw[rawLen] = '\0';
    if (!complete) keepAlive = false;

    if (status == 0) {
        err = rawLen == 0 ? "No response from server (check server URL / port)"
                          : "Invalid HTTP response (server may require HTTPS)";
        return ExResult::Failed;
    }
    if (bodyStart >= 0)
        responseBody = decodeChunked(raw, bodyStart - 4, raw + bodyStart, rawLen - bodyStart);
    else
        responseBody = "";
    return ExResult::Ok;
}

} // namespace

/* Parsed CA bundle (lazily), shared with the MPlayer stream module. */
mbedtls_x509_crt* wiifin_ca_chain() {
    std::string err;
    return loadCa(err) ? &s_tls.ca : nullptr;
}

void JellyfinClient::dropConnection() {
    ConnGuard g;
    closeConn();
}

bool JellyfinClient::recoverNetwork() {
    if (netBusy) return false;          /* the start-up thread owns it */
    takeNetworkResult();
    closeConn();
    networkReady = false;
    net_deinit();
    bringUpNetwork();
    return networkReady;
}

int JellyfinClient::request(bool tls, const std::string& host, int port,
                            const std::string& path,
                            const std::string& method,
                            const std::string& contentType,
                            const std::string& body,
                            const std::string& authToken,
                            std::string& responseBody,
                            size_t maxResponse) {
    ConnGuard g;
    std::string err;
    Log::addPrivate(host);   /* never write the server's name to the log */
    /* failures go to the log (path only: queries may hold search terms) */
    auto fail = [&](const std::string& why) {
        errMsg = why;
        SYS_Report("[HTTP] %s %s %s -> %s\n", method.c_str(), tls ? "https" : "http",
                   path.substr(0, path.find('?')).c_str(), why.c_str());
        return -1;
    };
    if (tls && !tlsSharedInit(sslVerify, err)) return fail(err);

    std::string authHdr = authToken.empty() ? "" : (", Token=\"" + authToken + "\"");
    std::string ctHdr   = contentType.empty() ? "" : ("Content-Type: " + contentType + "\r\n");
    // RFC 7230 §5.4: omit port from Host when it is the default for the scheme.
    std::string hostHdr = host + (port == (tls ? 443 : 80) ? "" : (":" + std::to_string(port)));
    char req[4096];
    int reqLen = snprintf(req, sizeof(req),
        "%s %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Connection: keep-alive\r\n"
        "Authorization: MediaBrowser Client=\"WiiFin\", Device=\"Nintendo Wii\", "
        "DeviceId=\"%s\", Version=\"" WIIFIN_VERSION "\"%s\r\n"
        "%s"
        "Content-Length: %zu\r\n"
        "\r\n",
        method.c_str(),
        path.empty() ? "/" : path.c_str(),
        hostHdr.c_str(),
        s_deviceId.c_str(),
        authHdr.c_str(),
        ctHdr.c_str(),
        body.size()
    );
    if (reqLen >= (int)sizeof(req)) return fail("Request headers too large");

    /* Retry loop: a reused connection the server already closed is replaced
     * by a fresh one; transient IOS send/recv failures during the handshake
     * (left behind by MPlayer's orphaned sockets) get a short cool-down. */
    bool recovered = false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        bool reused = s_conn.open && s_conn.tls == tls && s_conn.host == host &&
                      s_conn.port == port && (!tls || s_conn.verify == sslVerify) &&
                      nowMs() - s_conn.lastUseMs < IDLE_REUSE_MS;
        if (!reused) {
            closeConn();
            struct sockaddr_in addr;
            if (!resolve(host, port, addr, err)) return fail(err);
            bool transient = false;
            if (!openConn(tls, addr, host, port, sslVerify, err, transient)) {
                if ((s_lastSocketErr == -ENETRESET || s_lastSocketErr == -ENETDOWN ||
                     s_lastSocketErr == -ENXIO) && !recovered) {
                    SYS_Report("[Net] network lost (%d), restarting it\n", (int)s_lastSocketErr);
                    recovered = true;
                    if (recoverNetwork()) continue;
                    return fail("Network lost: " + errMsg);
                }
                if (!transient || attempt == 2) return fail(err);
                errMsg = err;
                usleep(500000);   // 500 ms cool-down before retry
                continue;
            }
        }

        int  status    = 0;
        bool keepAlive = false;
        ExResult r = exchange(req, reqLen, body, reused, status, responseBody,
                              keepAlive, err, maxResponse);
        if (r == ExResult::Stale) { closeConn(); continue; }
        if (r == ExResult::Failed) { closeConn(); return fail(err); }
        if (keepAlive) s_conn.lastUseMs = nowMs();
        else           closeConn();
        if (status >= 400 && !(status == 404 && path.find("/Images/") != std::string::npos))
            SYS_Report("[HTTP] %s %s -> HTTP %d\n", method.c_str(),
                       path.substr(0, path.find('?')).c_str(), status);
        return status;
    }
    return fail("Connection lost");
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
        // status == -1 : errMsg already set by request()
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

void JellyfinClient::logServerInfo(const std::string& serverUrl) {
    static std::string logged;
    if (logged == serverUrl) return;
    std::string resp;
    if (httpRequest(serverUrl + "/System/Info/Public", "GET", "", "", "", resp) != 200) return;
    logged = serverUrl;
    SYS_Report("[Server] %s %s\n", jsonGetString(resp, "ProductName").c_str(),
               jsonGetString(resp, "Version").c_str());
}

/* Hides what a server log line says about the user's files: quoted strings
 * and tokens holding a path ("/media/...", "C:\\..."), which carry titles. */
static std::string maskPaths(const std::string& in) {
    std::string out;
    for (size_t i = 0; i < in.size(); ) {
        char c = in[i];
        if (c == '"' || c == '\'') {
            size_t e = in.find(c, i + 1);
            std::string q = in.substr(i, e == std::string::npos ? std::string::npos : e - i + 1);
            bool path = q.find('/') != std::string::npos || q.find('\\') != std::string::npos;
            out += path ? std::string("<path>") : q;
            i = e == std::string::npos ? in.size() : e + 1;
            continue;
        }
        bool start = i == 0 || in[i - 1] == ' ' || in[i - 1] == '=' || in[i - 1] == ':';
        bool winPath = i + 2 < in.size() && isalpha((unsigned char)c) && in[i + 1] == ':' && in[i + 2] == '\\';
        if (start && (c == '/' || winPath)) {
            /* unquoted paths may hold spaces: up to the next option or the
             * end of the line, keeping a final '.', ':' or ',' */
            size_t e = in.find(" -", i);
            if (e == std::string::npos) e = in.size();
            while (e > i && (in[e - 1] == '.' || in[e - 1] == ':' || in[e - 1] == ',')) --e;
            out += "<path>";
            i = e;
            continue;
        }
        out += c;
        ++i;
    }
    return out;
}

void JellyfinClient::logTranscodeFailure(const std::string& serverUrl, const JellyfinAuth& auth) {
    std::string resp;
    int status = httpRequest(serverUrl + "/System/Logs", "GET", "", "", auth.accessToken, resp);
    if (status != 200) {
        SYS_Report("[Server] FFmpeg log not readable (HTTP %d: the account is not an administrator)\n", status);
        return;
    }
    /* newest FFmpeg.Transcode-*.log (ISO dates compare as strings) */
    struct Pick { JellyfinClient* self; std::string name, date; };
    Pick pick{ this, "", "" };
    size_t pos = resp.find('[');
    if (pos != std::string::npos)
        forEachArrayObject(resp, pos, [](const std::string& obj, void* v) {
            Pick* p = static_cast<Pick*>(v);
            std::string name = p->self->jsonGetString(obj, "Name");
            std::string date = p->self->jsonGetString(obj, "DateModified");
            if (name.compare(0, 16, "FFmpeg.Transcode") == 0 && date > p->date) {
                p->name = name;
                p->date = date;
            }
        }, &pick);
    if (pick.name.empty()) { SYS_Report("[Server] no FFmpeg transcode log found\n"); return; }
    std::string text;
    if (httpRequest(serverUrl + "/System/Logs/Log?name=" + pick.name, "GET", "", "",
                    auth.accessToken, text) != 200) return;
    /* the end holds the error; keep the last 25 lines, 200 chars each */
    std::vector<std::string> lines;
    size_t b = 0;
    while (b < text.size()) {
        size_t e = text.find('\n', b);
        if (e == std::string::npos) e = text.size();
        std::string ln = text.substr(b, e - b);
        if (!ln.empty() && ln.back() == '\r') ln.pop_back();
        if (!ln.empty()) lines.push_back(ln);
        b = e + 1;
    }
    size_t from = lines.size() > 25 ? lines.size() - 25 : 0;
    SYS_Report("[Server] end of the FFmpeg log of %s:\n", pick.date.c_str());
    for (size_t i = from; i < lines.size(); ++i)
        SYS_Report("[Server FFmpeg] %.200s\n", maskPaths(lines[i]).c_str());
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
                               int& totalCount,
                               const std::string& extra) {
    char qs[768];
    snprintf(qs, sizeof(qs),
        "/Users/%s/Items?ParentId=%s%s%s"
        "&Fields=ProductionYear,UserData,RunTimeTicks"
        "&EnableImages=false"
        "&Limit=%d&StartIndex=%d",
        auth.userId.c_str(), parentId.c_str(),
        extra.find("SortBy=") == std::string::npos ? "&SortBy=SortName&SortOrder=Ascending" : "",
        extra.c_str(), limit, startIndex);
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
    /* format=Jpg: JPEG is the only format WiiFin decodes, and Jellyfin
     * sends some pictures as PNG (the ones it draws for libraries) */
    char qs[256];
    snprintf(qs, sizeof(qs),
        "/Items/%s/Images/Primary?fillWidth=%d&fillHeight=%d&maxWidth=%d&maxHeight=%d&quality=82&format=Jpg",
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
             "?maxWidth=%d&maxHeight=%d&quality=82&format=Jpg", maxWidth, maxHeight);

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
        + "?Fields=Overview,Genres,OfficialRating,RunTimeTicks,MediaStreams,MediaSources,UserData";
    std::string resp;
    int status = httpRequest(url, "GET", "", "", auth.accessToken, resp);
    if (status != 200) {
        if (status >= 0) errMsg = "getItemDetail failed (HTTP " + std::to_string(status) + ")";
        return false;
    }

    out.id                    = jsonGetString(resp, "Id");
    out.name                  = jsonGetString(resp, "Name");
    out.overview              = jsonGetString(resp, "Overview", true);   /* its paragraphs */
    out.officialRating        = jsonGetString(resp, "OfficialRating");
    out.year                  = jsonGetInt(resp,    "ProductionYear");
    out.runtimeTicks          = jsonGetLongLong(resp, "RunTimeTicks");
    out.playbackPositionTicks = jsonGetLongLong(resp, "PlaybackPositionTicks");
    out.isFavorite            = jsonGetBool(resp, "IsFavorite");
    out.played                = jsonGetBool(resp, "Played");
    out.specialFeatures       = jsonGetInt(resp, "SpecialFeatureCount") + jsonGetInt(resp, "LocalTrailerCount");
    out.seriesId              = jsonGetString(resp, "SeriesId");
    out.seasonId              = jsonGetString(resp, "SeasonId");

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

    // MediaStreams: audio and subtitle tracks (of the default file)
    parseStreams(resp, out.audioStreams, out.subtitleStreams);

    // MediaSources: when the title has several files, each one's tracks
    {
        size_t mp = resp.find("\"MediaSources\":");
        size_t pos = mp == std::string::npos ? std::string::npos : resp.find('[', mp);
        std::vector<MediaVersion> vs;
        if (pos != std::string::npos) {
            pos++;
            std::string obj;
            while (vs.size() < 8 && !(obj = nextJsonObject(resp, pos)).empty()) {
                MediaVersion v;
                v.id   = jsonGetString(obj, "Id");
                v.name = jsonGetString(obj, "Name");
                if (v.name.empty()) v.name = "Version " + std::to_string(vs.size() + 1);
                parseStreams(obj, v.audio, v.subs);
                if (obj.find("\"DefaultAudioStreamIndex\":") != std::string::npos)
                    v.defaultAudio = jsonGetInt(obj, "DefaultAudioStreamIndex");
                if (obj.find("\"DefaultSubtitleStreamIndex\":") != std::string::npos)
                    v.defaultSub = jsonGetInt(obj, "DefaultSubtitleStreamIndex");
                if (!v.id.empty()) vs.push_back(v);
            }
        }
        /* the item's own file (else the first): its tracks to start with */
        for (size_t i = 0; i < vs.size(); i++)
            if (i == 0 || vs[i].id == itemId) {
                out.defaultAudioIndex = vs[i].defaultAudio;
                out.defaultSubIndex   = vs[i].defaultSub;
            }
        if (vs.size() > 1) out.versions = vs;
    }

    return true;
}

/* The next {...} of a JSON array from pos (just after '[' or a previous
 * object), pos moved past it; empty at the array's end */
std::string JellyfinClient::nextJsonObject(const std::string& s, size_t& pos) {
    while (pos < s.size() && s[pos] != '{' && s[pos] != ']') pos++;
    if (pos >= s.size() || s[pos] == ']') return "";
    int depth = 0; bool inStr = false;
    size_t objStart = pos;
    for (; pos < s.size(); pos++) {
        char c = s[pos];
        if (inStr) {
            if (c == '\\') pos++;
            else if (c == '"') inStr = false;
        } else {
            if (c == '"') inStr = true;
            else if (c == '{') depth++;
            else if (c == '}') { if (--depth == 0) { pos++; break; } }
        }
    }
    return s.substr(objStart, pos - objStart);
}

/* The audio and subtitle tracks of the first "MediaStreams" array in json */
void JellyfinClient::parseStreams(const std::string& json, std::vector<MediaStream>& audio,
                                  std::vector<MediaStream>& subs) {
    size_t mp = json.find("\"MediaStreams\":");
    if (mp == std::string::npos) return;
    size_t pos = json.find('[', mp);
    if (pos == std::string::npos) return;
    pos++;
    std::string obj;
    while (!(obj = nextJsonObject(json, pos)).empty()) {
        std::string t = jsonGetString(obj, "Type");
        MediaStream ms;
        ms.index        = jsonGetInt(obj, "Index");
        ms.type         = t;
        ms.displayTitle = jsonGetString(obj, "DisplayTitle");
        ms.language     = jsonGetString(obj, "Language");
        ms.codec        = jsonGetString(obj, "Codec");
        ms.isText       = jsonGetBool(obj, "IsTextSubtitleStream");
        if      (t == "Audio"    && audio.size() < 8)  audio.push_back(ms);
        else if (t == "Subtitle" && subs.size()  < 16) subs.push_back(ms);
    }
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
    return fetchEpisodes(serverUrl + qs, auth, out);
}

/* a long series is more than the 256 KB of a response (about 300 episodes):
 * up to 4 MB, on the heap */
bool JellyfinClient::getSeriesEpisodes(const std::string& serverUrl,
                                        const JellyfinAuth& auth,
                                        const std::string& seriesId,
                                        std::vector<JellyfinEpisode>& out) {
    char qs[512];
    snprintf(qs, sizeof(qs),
        "/Shows/%s/Episodes?UserId=%s&IsMissing=false"
        "&Fields=IndexNumber,ParentIndexNumber,UserData&EnableImages=false",
        seriesId.c_str(), auth.userId.c_str());
    return fetchEpisodes(serverUrl + qs, auth, out, 4u << 20);
}

bool JellyfinClient::getShuffledEpisodes(const std::string& serverUrl,
                                          const JellyfinAuth& auth,
                                          const std::string& seriesId,
                                          const std::string& seasonId,
                                          int limit,
                                          std::vector<JellyfinEpisode>& out) {
    char qs[512];
    snprintf(qs, sizeof(qs),
        "/Shows/%s/Episodes?UserId=%s%s%s&SortBy=Random&Limit=%d"
        "&Fields=IndexNumber,ParentIndexNumber,UserData&EnableImages=false",
        seriesId.c_str(), auth.userId.c_str(),
        seasonId.empty() ? "" : "&SeasonId=", seasonId.c_str(), limit);
    return fetchEpisodes(serverUrl + qs, auth, out);
}

bool JellyfinClient::fetchEpisodes(const std::string& url, const JellyfinAuth& auth,
                                   std::vector<JellyfinEpisode>& out, size_t maxResponse) {
    std::string resp;
    int status = httpRequest(url, "GET", "", "", auth.accessToken, resp, maxResponse);
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
        e.played                = c->self->jsonGetBool(obj, "Played");
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
        "/Users/%s/Items?IncludeItemTypes=Movie,Series,Episode,MusicAlbum"
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

const char* JellyfinClient::videoQualityName(int q) {
    static const char* names[VIDEO_QUALITY_COUNT] = { "Low", "Normal", "High", "Max" };
    return (q >= 0 && q < VIDEO_QUALITY_COUNT) ? names[q] : names[1];
}

int JellyfinClient::videoBitrate() const {
    /* MPEG-4 ASP at 640 px.  Decoding cost grows with the bitrate (Dolphin,
     * grainy 1080p source: 15% of real time at 1.5 Mb/s, 25% at 2.5, 39% at
     * 4), so High stops at 3.5 to leave headroom on real hardware; Max (5)
     * is for wired adapters and sharp sources, at the risk of stutter.
     * Playback steps down on its own when the link can't keep up. */
    return qualityBitrate(effectiveQuality());
}

int JellyfinClient::qualityBitrate(int q) {
    static const int bitrates[VIDEO_QUALITY_COUNT] = { 1500000, 2500000, 3500000, 5000000 };
    return bitrates[(q >= 0 && q < VIDEO_QUALITY_COUNT) ? q : 1];
}

/* Start at a quality the link can carry instead of finding out through two
 * rebuffers: time a download from Jellyfin's bitrate test (the same probe
 * its own clients use), then cap the quality to the highest level whose
 * stream fits with 30 % to spare for the cache to fill.  The setting itself
 * is left alone. */
void JellyfinClient::measureLink(const std::string& serverUrl, const JellyfinAuth& auth) {
    if (linkMeasuredFor == serverUrl) return;
    /* Two downloads, 64 then 128 KB (Jellyfin rounds sizes up to a power of
     * two; 256 KB would not fit in the response buffer with the headers),
     * and the speed from the difference: the request's own time (round
     * trip, server, TLS) cancels out.  One download of 128 KB took it for
     * slow transfer and capped the quality to Low on links that carry High. */
    std::string small, big;
    u64 t0 = nowMs();
    int s1 = httpRequest(serverUrl + "/Playback/BitrateTest?size=65536", "GET", "", "",
                         auth.accessToken, small);
    u64 t1 = nowMs();
    int s2 = httpRequest(serverUrl + "/Playback/BitrateTest?size=131072", "GET", "", "",
                         auth.accessToken, big);
    u64 t2 = nowMs();
    if (s1 != 200 || s2 != 200 || big.size() <= small.size()) {
        SYS_Report("[Net] link speed not measured (HTTP %d, %d)\n", s1, s2);
        return;                                  /* try again next playback */
    }
    linkMeasuredFor = serverUrl;
    long long msSmall = (long long)(t1 - t0), msBig = (long long)(t2 - t1);
    long long ms = msBig - msSmall;
    double bytes = (double)(big.size() - small.size());
    if (ms < 5) { ms = msBig; bytes = (double)big.size(); }   /* too close to tell: the plain rate */
    double kbps = bytes * 8.0 / (double)(ms > 0 ? ms : 1);
    linkKbps = (int)kbps;
    int cap = VIDEO_QUALITY_COUNT - 1;
    while (cap > 0 && (qualityBitrate(cap) + 128000) / 1000.0 * 1.3 > kbps) --cap;
    linkCap = cap;
    SYS_Report("[Net] link %.1f Mb/s (%u bytes in %lld ms, %u in %lld ms): quality up to %s\n",
               kbps / 1000.0, (unsigned)small.size(), msSmall, (unsigned)big.size(), msBig,
               videoQualityName(cap));
}

// ---------------------------------------------------------------------------
// Direct play
// ---------------------------------------------------------------------------
namespace {
/* What MPlayer CE decodes in real time on the Wii's 729 MHz CPU, all in
 * software: cautious limits, to widen with measurements on a console.
 * Not in the build at all: HEVC, VP9, AV1, MS-MPEG4 (DivX 3), WMV in ASF,
 * DTS (in it, but too heavy next to a picture).  The device profile sent to
 * Jellyfin and the reasons WiiFin gives for a transcode both come from
 * these tables. */
struct DirectFormat {
    const char* containers;   /* as Jellyfin names them */
    const char* demuxer;      /* MPlayer's own, or FFmpeg's (lavf:<format>) */
    const char* video;
    const char* audio;
};
const DirectFormat FORMATS[] = {
    { "avi",            "avi",    "mpeg4,h264,mpeg1video,mpeg2video",     "mp3,mp2,ac3,aac" },
    { "mkv,webm",       "mkv",    "mpeg4,h264,mpeg1video,mpeg2video,vp8", "mp3,mp2,ac3,aac,vorbis,flac" },
    /* FFmpeg's MP4 demuxer: MPlayer's own lost H.264 reference frames
     * ("Frame num gap": a picture falling apart into grey blocks) */
    { "mp4,m4v,mov",    "lavf:mov",    "mpeg4,h264",                      "aac,mp3,ac3" },
    { "ts,mpegts,m2ts", "lavf:mpegts", "mpeg2video,h264,mpeg4",           "mp2,mp3,ac3,aac" },
    { "mpeg,mpg,vob",   "mpegps", "mpeg1video,mpeg2video",                "mp2,mp3,ac3" },
};
struct VideoLimit {
    const char* codecs;
    int width, height;
    int level;      /* 0: none */
    int bitrate;    /* 0: none */
};
const VideoLimit VIDEO_LIMITS[] = {
    { "mpeg4,mpeg1video,mpeg2video", 720, 576, 0,  0 },
    { "h264",                        720, 480, 31, 2500000 },
    { "vp8",                         640, 480, 0,  0 },
};
const int MAX_FPS = 30;
/* audio channels: AC3 5.1 is mixed down cheaply, the others are stereo.
 * Not in AVI: FFmpeg-made ones describe 5.1 audio with a WAVEFORMATEXTENSIBLE
 * header that MPlayer CE's AVI demuxer misreads (the AC3 went to the PCM
 * decoder: noise, and the picture fell to 2 fps). */
int maxChannels(const std::string& codec, const DirectFormat* f) {
    return codec == "ac3" && !(f && strcmp(f->demuxer, "avi") == 0) ? 6 : 2;
}

/* name is one of the comma-separated list */
bool inList(const char* list, const std::string& name) {
    if (name.empty()) return false;
    for (const char* p = list; *p; ) {
        const char* e = strchr(p, ',');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n == name.size() && strncmp(p, name.c_str(), n) == 0) return true;
        if (!e) break;
        p = e + 1;
    }
    return false;
}
/* any name of a comma-separated list (Jellyfin's "mov,mp4,m4a" containers) */
const DirectFormat* formatOf(const std::string& containers) {
    size_t p = 0;
    while (p <= containers.size()) {
        size_t e = containers.find(',', p);
        std::string c = containers.substr(p, e == std::string::npos ? std::string::npos : e - p);
        for (const DirectFormat& f : FORMATS) if (inList(f.containers, c)) return &f;
        if (e == std::string::npos) break;
        p = e + 1;
    }
    return nullptr;
}

std::string condition(const char* prop, int value, bool required) {
    return std::string("{\"Condition\":\"LessThanEqual\",\"Property\":\"") + prop +
           "\",\"Value\":\"" + std::to_string(value) + "\",\"IsRequired\":" +
           (required ? "true" : "false") + "}";
}

/* "DirectPlayProfiles", "CodecProfiles"... of the device profile */
std::string directProfile() {
    std::string s = "\"DirectPlayProfiles\":[";
    for (size_t i = 0; i < sizeof(FORMATS) / sizeof(FORMATS[0]); ++i)
        s += std::string(i ? "," : "") + "{\"Type\":\"Video\",\"Container\":\"" + FORMATS[i].containers +
             "\",\"VideoCodec\":\"" + FORMATS[i].video + "\",\"AudioCodec\":\"" + FORMATS[i].audio + "\"}";
    s += "],\"TranscodingProfiles\":[],\"CodecProfiles\":[";
    for (const VideoLimit& v : VIDEO_LIMITS) {
        s += std::string("{\"Type\":\"Video\",\"Codec\":\"") + v.codecs + "\",\"Conditions\":[" +
             condition("Width", v.width, true) + "," + condition("Height", v.height, true) + "," +
             condition("VideoFramerate", MAX_FPS, false);
        if (v.level)   s += "," + condition("VideoLevel", v.level, false) + "," +
                             condition("VideoBitDepth", 8, false);
        if (v.bitrate) s += "," + condition("VideoBitrate", v.bitrate, false);
        s += "]},";
    }
    s += "{\"Type\":\"VideoAudio\",\"Codec\":\"mp3,mp2,aac,vorbis,flac\",\"Conditions\":[" +
         condition("AudioChannels", 2, false) + "]},"
         "{\"Type\":\"VideoAudio\",\"Codec\":\"ac3\",\"Conditions\":[" +
         condition("AudioChannels", 6, false) + "]},"
         "{\"Type\":\"VideoAudio\",\"Codec\":\"ac3\",\"Container\":\"avi\",\"Conditions\":[" +
         condition("AudioChannels", 2, false) + "]}"
         "],\"SubtitleProfiles\":[]";
    return s;
}

/* The object of the first stream of that type ("Video", "Audio") */
std::string streamOfType(const std::string& json, const char* type) {
    size_t t = json.find(std::string("\"Type\":\"") + type + "\"");
    if (t == std::string::npos) return "";
    size_t o = json.rfind('{', t), e = json.find('}', t);
    return (o == std::string::npos || e == std::string::npos) ? "" : json.substr(o, e - o + 1);
}

void addReason(std::string& r, const char* why) {
    if (r.find(why) == std::string::npos) r += (r.empty() ? "" : ",") + std::string(why);
}
} // namespace

bool JellyfinClient::getDirectPlayUrl(const std::string& serverUrl,
                                      const JellyfinAuth& auth,
                                      const std::string& itemId,
                                      const std::string& mediaSourceId,
                                      int audioStreamIndex,
                                      int subtitleStreamIndex,
                                      DirectPlay& out)
{
    out = DirectPlay();
    /* the reasons Jellyfin's dashboard shows for the transcode */
    if (!directPlay || forceReencode) { out.reasons = "DirectPlayError"; return false; }
    if (subtitleStreamIndex >= 0)     { out.reasons = "SubtitleCodecNotSupported"; return false; }
    measureLink(serverUrl, auth);
    /* what the link carries, with a margin; the Wii's Wi-Fi and its CPU
     * give out above 8 Mb/s anyway */
    int maxBps = linkKbps > 0 ? (int)(linkKbps * 1000LL * 3 / 4) : 4000000;
    if (maxBps > 8000000) maxBps = 8000000;

    char url[512];
    snprintf(url, sizeof(url), "%s/Items/%s/PlaybackInfo?UserId=%s&DeviceId=%s",
             serverUrl.c_str(), itemId.c_str(), auth.userId.c_str(), s_deviceId.c_str());
    std::string body =
        "{\"UserId\":\"" + auth.userId + "\","
        "\"MediaSourceId\":\"" + mediaSourceId + "\","
        "\"AudioStreamIndex\":" + std::to_string(audioStreamIndex) + ","
        "\"SubtitleStreamIndex\":-1,"   /* none: else the default track had to be burned in */
        "\"MaxStreamingBitrate\":" + std::to_string(maxBps) + ","
        "\"IsPlayback\":true,\"AutoOpenLiveStream\":true,"
        "\"EnableDirectPlay\":true,\"EnableDirectStream\":false,\"EnableTranscoding\":false,"
        "\"DeviceProfile\":{\"MaxStreamingBitrate\":" + std::to_string(maxBps) + "," +
        directProfile() + "}}";
    std::string resp;
    int status = httpRequest(url, "POST", "application/json", body, auth.accessToken, resp);
    if (status != 200) {
        SYS_Report("[DirectPlay] PlaybackInfo HTTP %d\n", status);
        out.reasons = "DirectPlayError";
        return false;
    }
    std::string video = streamOfType(resp, "Video"), audio = streamOfType(resp, "Audio");
    /* the audio tracks, in file order: the chosen one is checked, and
     * MPlayer is told which one to play (-aid) */
    std::vector<std::string> tracks;
    for (size_t p = resp.find("\"Type\":\"Audio\""); p != std::string::npos;
         p = resp.find("\"Type\":\"Audio\"", p + 1)) {
        size_t o = resp.rfind('{', p), e = resp.find('}', p);
        if (o != std::string::npos && e != std::string::npos) tracks.push_back(resp.substr(o, e - o + 1));
    }
    int chosen = -1;
    for (size_t i = 0; i < tracks.size(); ++i)
        if (jsonGetInt(tracks[i], "Index") == audioStreamIndex) chosen = (int)i;
    if (chosen > 0) audio = tracks[chosen];
    out.container  = jsonGetString(resp, "Container");
    out.videoCodec = jsonGetString(video, "Codec");
    out.audioCodec = jsonGetString(audio, "Codec");
    out.width      = jsonGetInt(video, "Width");
    out.height     = jsonGetInt(video, "Height");
    {
        size_t p = video.find("\"RealFrameRate\":");
        out.fps = p != std::string::npos ? (float)atof(video.c_str() + p + 16) : 0.0f;
    }
    const DirectFormat* fmt = formatOf(out.container);
    if (fmt) out.demuxer = fmt->demuxer;

    /* why not, in Jellyfin's words, for the dashboard */
    std::string why;
    if (!fmt) addReason(why, "ContainerNotSupported");
    else {
        if (!inList(fmt->video, out.videoCodec)) addReason(why, "VideoCodecNotSupported");
        if (!audio.empty() && !inList(fmt->audio, out.audioCodec)) addReason(why, "AudioCodecNotSupported");
    }
    for (const VideoLimit& v : VIDEO_LIMITS) {
        if (!inList(v.codecs, out.videoCodec)) continue;
        if (out.width > v.width || out.height > v.height) addReason(why, "VideoResolutionNotSupported");
        if (v.level && jsonGetInt(video, "Level") > v.level) addReason(why, "VideoLevelNotSupported");
        if (v.level && jsonGetInt(video, "BitDepth") > 8)   addReason(why, "VideoBitDepthNotSupported");
        if (v.bitrate && jsonGetInt(video, "BitRate") > v.bitrate) addReason(why, "VideoBitrateNotSupported");
    }
    if (out.fps > MAX_FPS + 0.5f) addReason(why, "VideoFramerateNotSupported");
    if (!audio.empty() && jsonGetInt(audio, "Channels") > maxChannels(out.audioCodec, fmt))
        addReason(why, "AudioChannelsNotSupported");
    if (jsonGetInt(resp, "Bitrate") > maxBps) addReason(why, "ContainerBitrateExceedsLimit");

    bool can = jsonGetBool(resp, "SupportsDirectPlay");
    /* Another track than the first: MPlayer's -aid counts the audio tracks
     * from 0 in MKV and FFmpeg's demuxers (MP4, TS), and is the stream
     * number in an AVI (Jellyfin's index).  MPEG-PS numbers them its own way:
     * transcoded. */
    if (chosen > 0 && fmt) {
        if (strcmp(fmt->demuxer, "avi") == 0) out.aid = audioStreamIndex;
        else if (strcmp(fmt->demuxer, "mpegps") != 0) out.aid = chosen;
        else { can = false; addReason(why, "SecondaryAudioNotSupported"); }
    }
    if (!can && why.empty()) why = "DirectPlayError";
    SYS_Report("[DirectPlay] %s %s %dx%d %.2f fps, %s (track %d of %u), link cap %d kb/s: %s%s\n",
               out.container.c_str(), out.videoCodec.c_str(), out.width, out.height,
               (double)out.fps, out.audioCodec.c_str(), chosen < 0 ? 1 : chosen + 1,
               (unsigned)tracks.size(), maxBps / 1000,
               can ? "direct" : "transcode, ", can ? "" : why.c_str());
    if (!can) { out.reasons = why; return false; }
    out.playSessionId = jsonGetString(resp, "PlaySessionId");
    out.url = addScheme(serverUrl) + "/Videos/" + itemId + "/stream?static=true&MediaSourceId=" +
              mediaSourceId + "&DeviceId=" + s_deviceId + "&PlaySessionId=" + out.playSessionId +
              "&ApiKey=" + auth.accessToken;
    return true;
}

bool JellyfinClient::getPlaybackUrl(const std::string& serverUrl,
                                    const JellyfinAuth& auth,
                                    const std::string& itemId,
                                    const std::string& mediaSourceId,
                                    int audioStreamIndex,
                                    int subtitleStreamIndex,
                                    long long startTimeTicks,
                                    std::string& outUrl,
                                    std::string& outPlaySessionId,
                                    PlaybackChoice& how,
                                    bool allowDirect)
{
    how = PlaybackChoice();
    DirectPlay dp;
    if (allowDirect && getDirectPlayUrl(serverUrl, auth, itemId, mediaSourceId,
                                        audioStreamIndex, subtitleStreamIndex, dp)) {
        outUrl           = dp.url;
        outPlaySessionId = dp.playSessionId;
        how.direct  = true;
        how.demuxer = dp.demuxer;
        how.fps     = dp.fps;
        how.aid     = dp.aid;
        return true;
    }
    if (!getTranscodingUrl(serverUrl, auth, itemId, mediaSourceId, audioStreamIndex,
                           subtitleStreamIndex, startTimeTicks, outUrl, outPlaySessionId,
                           allowDirect ? dp.reasons : std::string("DirectPlayError")))
        return false;
    /* the transcode's frame rate: the source's, held to the URL's
     * MaxFramerate (the player is told it: see g_wiifin_fps) */
    how.fps = dp.fps;
    size_t m = outUrl.find("MaxFramerate=");
    if (m != std::string::npos) {
        float cap = (float)atof(outUrl.c_str() + m + 13);
        if (cap > 1.0f && (how.fps <= 1.0f || cap < how.fps)) how.fps = cap;
    }
    return true;
}

bool JellyfinClient::getTranscodingUrl(const std::string& serverUrl,
                                        const JellyfinAuth& auth,
                                        const std::string& itemId,
                                        const std::string& mediaSourceId,
                                        int audioStreamIndex,
                                        int subtitleStreamIndex,
                                        long long startTimeTicks,
                                        std::string& outUrl,
                                        std::string& outPlaySessionId,
                                        const std::string& transcodeReasons)
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
        "%s/Items/%s/PlaybackInfo?UserId=%s&DeviceId=%s",
        serverUrl.c_str(), itemId.c_str(), auth.userId.c_str(), s_deviceId.c_str());

    // EnableDirectPlay and EnableDirectStream MUST be false in the JSON body.
    // Jellyfin reads these from the body; the same-named query params are ignored
    // by the server-side session manager when selecting the play method.
    //
    // Output: MPEG-4 ASP (DivX/Xvid class) + MP3 stereo in MPEG-TS.  On the Wii
    // it decodes about as cheaply as MPEG-2 but gives a much better picture per
    // bit (SSIM 0.959 at 1.2 Mb/s vs 0.939 for MPEG-2 at 2 Mb/s on grainy
    // 1080p HEVC), which matters on the Wii's slow Wi-Fi.  640 px is the width
    // of the Wii framebuffer: decoding wider pictures only wastes CPU.
    //
    // CodecProfiles with LessThanEqual Width/Height Conditions are the only way to
    // enforce output resolution in Jellyfin — MaxWidth/MaxHeight in the top-level
    // body are hints; the internal scale filter uses its own 1280px default unless
    // the device profile declares explicit width/height constraints.
    measureLink(serverUrl, auth);
    const int videoBps = videoBitrate();
    static const int AUDIO_BPS = 128000;
    static const char* codecProfiles =
        "[{\"Type\":\"Video\",\"Codec\":\"mpeg4\",\"Conditions\":["
        "{\"Condition\":\"LessThanEqual\",\"Property\":\"Width\","
            "\"Value\":\"640\",\"IsRequired\":true},"
        "{\"Condition\":\"LessThanEqual\",\"Property\":\"Height\","
            "\"Value\":\"480\",\"IsRequired\":true}"
        "]}]";

    char subField[48] = "";
    if (subtitleStreamIndex >= 0)
        snprintf(subField, sizeof(subField), "\"SubtitleStreamIndex\":%d,", subtitleStreamIndex);

    char bodyBuf[2048];
    snprintf(bodyBuf, sizeof(bodyBuf),
        "{\"UserId\":\"%s\","
        "\"MediaSourceId\":\"%s\","
        "\"AudioStreamIndex\":%d,"
        "%s"
        "\"MaxStreamingBitrate\":%d,"
        "\"MaxWidth\":640,\"MaxHeight\":480,"
        "\"StartTimeTicks\":%lld,"
        "\"IsPlayback\":true,"
        "\"AutoOpenLiveStream\":true,"
        "\"EnableDirectPlay\":false,"
        "\"EnableDirectStream\":false,"
        "\"DeviceProfile\":{"
        "\"DirectPlayProfiles\":[],"
        "\"TranscodingProfiles\":[{\"Container\":\"ts\",\"Type\":\"Video\","
        "\"VideoCodec\":\"mpeg4\",\"AudioCodec\":\"mp3\","
        "\"Protocol\":\"http\",\"Context\":\"Streaming\","
        "\"MaxAudioChannels\":\"2\",\"MaxFramerate\":30}],"
        "\"CodecProfiles\":%s,\"SubtitleProfiles\":[]}}",
        auth.userId.c_str(), mediaSourceId.c_str(), audioStreamIndex, subField,
        videoBps + AUDIO_BPS, (long long)startTimeTicks, codecProfiles);

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

    // Jellyfin derives the bitrates from the source (e.g. keeps a 384 kb/s
    // audio budget for 5.1 sources): pin them to what the Wii can stream.
    char vbBuf[16], abBuf[16];
    snprintf(vbBuf, sizeof(vbBuf), "%d", videoBps);
    snprintf(abBuf, sizeof(abBuf), "%d", AUDIO_BPS);
    urlReplaceParam(relUrl, "VideoCodec", "mpeg4");
    urlReplaceParam(relUrl, "AudioCodec", "mp3");
    urlReplaceParam(relUrl, "AudioBitrate", abBuf);
    urlReplaceParam(relUrl, "VideoBitrate", vbBuf);
    urlReplaceParam(relUrl, "MaxVideoBitDepth", "8");
    urlReplaceParam(relUrl, "MaxWidth", "640");
    urlReplaceParam(relUrl, "MaxHeight", "480");
    /* why, for the server's dashboard: what kept the file from playing as
     * it is (else Jellyfin says DirectPlayError, direct play being off here) */
    if (!transcodeReasons.empty()) urlReplaceParam(relUrl, "TranscodeReasons", transcodeReasons.c_str());

    // When subtitles are off (subtitleStreamIndex < 0), Jellyfin may still
    // auto-select the media's default subtitle track and embed
    // SubtitleStreamIndex=N&SubtitleMethod=Encode in the returned URL.
    // Strip both parameters so the transcoder does not burn in any subtitles.
    if (subtitleStreamIndex < 0) {
        urlRemoveParam(relUrl, "SubtitleStreamIndex");
        urlRemoveParam(relUrl, "SubtitleMethod");
    } else {
        // Burning subtitles from a start position, Jellyfin shifts the
        // timestamps back (setpts=PTS-start) as if -copyts were on, but only
        // adds -copyts when asked: without it the subtitles came ~start
        // seconds late, and the audio that much ahead of the first frame.
        urlReplaceParam(relUrl, "CopyTimestamps", "true");
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
    if (forceReencode) {
        urlReplaceParam(relUrl, "AllowVideoStreamCopy", "false");
        urlReplaceParam(relUrl, "AllowAudioStreamCopy", "false");
    } else if (startTimeTicks > 0) {
        // A DivX/Xvid source is copied as is, keyframes included (often 10 s
        // apart).  Cut mid-file, the stream opens on B-frames referring to
        // the previous GOP: MPlayer's decoder then fails every picture up to
        // the next keyframe, the sound playing over a black screen.  A
        // re-encode starts clean, with a keyframe every half second.
        urlReplaceParam(relUrl, "AllowVideoStreamCopy", "false");
    }
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

namespace {
/* "PlayMethod", stream indexes and the rest of a playback report body */
std::string reportFields(const char* playMethod, int audio, int sub)
{
    std::string f = std::string("\"PlayMethod\":\"") + (playMethod ? playMethod : "Transcode") + "\",";
    /* the dashboard's volume slider follows WiiFin's */
    f += "\"VolumeLevel\":" + std::to_string(wii_player_volume()) + ",";
    if (audio >= 0) f += "\"AudioStreamIndex\":" + std::to_string(audio) + ",";
    if (sub >= 0)   f += "\"SubtitleStreamIndex\":" + std::to_string(sub) + ",";
    return f;
}
} // namespace

bool JellyfinClient::postCapabilities(const std::string& serverUrl, const JellyfinAuth& auth)
{
    const std::string body =
        "{\"PlayableMediaTypes\":[\"Video\",\"Audio\"],"
        "\"SupportedCommands\":[\"DisplayMessage\",\"VolumeUp\",\"VolumeDown\",\"SetVolume\","
        "\"Mute\",\"Unmute\",\"ToggleMute\","
        "\"SetAudioStreamIndex\",\"SetSubtitleStreamIndex\"],"
        "\"SupportsMediaControl\":true,"
        "\"SupportsPersistentIdentifier\":true}";
    std::string resp;
    int status = httpRequest(serverUrl + "/Sessions/Capabilities/Full", "POST", "application/json",
                             body, auth.accessToken, resp);
    SYS_Report("[Remote] capabilities: HTTP %d\n", status);
    return status == 204 || status == 200;
}

bool JellyfinClient::reportPlaybackStart(const std::string& serverUrl,
                                          const JellyfinAuth& auth,
                                          const std::string& itemId,
                                          const std::string& mediaSourceId,
                                          const std::string& playSessionId,
                                          const char* playMethod,
                                          int audioStreamIndex,
                                          int subtitleStreamIndex)
{
    std::string body =
        "{\"ItemId\":\"" + itemId + "\","
        "\"MediaSourceId\":\"" + mediaSourceId + "\","
        "\"PlaySessionId\":\"" + playSessionId + "\"," +
        reportFields(playMethod, audioStreamIndex, subtitleStreamIndex) +
        "\"CanSeek\":true,"
        "\"IsPaused\":false,"
        "\"IsMuted\":" + std::string(wii_player_muted() ? "true" : "false") + "}";

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
                                             bool isPaused,
                                             const char* playMethod,
                                             int audioStreamIndex,
                                             int subtitleStreamIndex)
{
    std::string body =
        "{\"ItemId\":\"" + itemId + "\","
        "\"MediaSourceId\":\"" + mediaSourceId + "\","
        "\"PlaySessionId\":\"" + playSessionId + "\"," +
        reportFields(playMethod, audioStreamIndex, subtitleStreamIndex) +
        "\"PositionTicks\":" + std::to_string(positionTicks) + ","
        "\"CanSeek\":true,"
        "\"IsPaused\":" + (isPaused ? "true" : "false") + ","
        "\"IsMuted\":" + std::string(wii_player_muted() ? "true" : "false") + "}";

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
                      "?DeviceId=" + s_deviceId + "&PlaySessionId=" + playSessionId;
    std::string resp;
    int status = httpRequest(url, "DELETE", "", "", auth.accessToken, resp);
    SYS_Report("[WiiPlayer] deleteActiveEncoding status=%d\n", status);
    return status == 204 || status == 200;
}

// ---------------------------------------------------------------------------
// Segments to skip (intro, recap, credits...)
//   GET /MediaSegments/{id}  (10.10+)
//     {"Items":[{"Type":"Intro","StartTicks":600000000,"EndTicks":1200000000},...]}
//   GET /Episode/{id}/IntroSkipperSegments  (Intro Skipper on older servers)
//     {"Introduction":{"Valid":true,"Start":60.0,"End":120.0},"Credits":{...}}
//   GET /Episode/{id}/IntroTimestamps  (its first versions)
//     {"Valid":true,"IntroStart":60.0,"IntroEnd":120.0}
// None of them (404: no plugin, an old server) is no segment, not an error.
// ---------------------------------------------------------------------------
bool JellyfinClient::getIntroTimestamps(const std::string& serverUrl,
                                         const JellyfinAuth& auth,
                                         const std::string& episodeId,
                                         IntroInfo& out)
{
    out = IntroInfo{};
    auto add = [&](MediaSegment::Kind k, float start, float end) {
        if (end - start < 1.0f || start < 0.0f) return;
        MediaSegment m; m.kind = k; m.start = start; m.end = end;
        out.segments.push_back(m);
    };
    auto number = [](const std::string& obj, const char* key) -> double {
        std::string search = std::string("\"") + key + "\":";
        size_t p = obj.find(search);
        return p == std::string::npos ? -1.0 : atof(obj.c_str() + p + search.size());
    };
    std::string resp;

    if (httpRequest(serverUrl + "/MediaSegments/" + episodeId, "GET", "", "", auth.accessToken, resp) == 200) {
        struct Ctx { std::vector<std::string> objs; };
        Ctx ctx;
        forEachItemObject(resp, [](const std::string& obj, void* c) {
            static_cast<Ctx*>(c)->objs.push_back(obj);
        }, &ctx);
        for (const std::string& o : ctx.objs) {
            std::string type = jsonGetString(o, "Type");
            static const struct { const char* name; MediaSegment::Kind kind; } KINDS[] = {
                { "Intro", MediaSegment::Intro }, { "Recap", MediaSegment::Recap },
                { "Preview", MediaSegment::Preview }, { "Commercial", MediaSegment::Commercial },
                { "Outro", MediaSegment::Outro },
            };
            for (const auto& k : KINDS)
                if (type == k.name)
                    add(k.kind, (float)(number(o, "StartTicks") / 1e7), (float)(number(o, "EndTicks") / 1e7));
        }
    }
    if (out.segments.empty() &&
        httpRequest(serverUrl + "/Episode/" + episodeId + "/IntroSkipperSegments", "GET", "", "",
                    auth.accessToken, resp) == 200) {
        static const struct { const char* key; MediaSegment::Kind kind; } KEYS[] = {
            { "\"Introduction\":", MediaSegment::Intro }, { "\"Recap\":", MediaSegment::Recap },
            { "\"Preview\":", MediaSegment::Preview }, { "\"Credits\":", MediaSegment::Outro },
        };
        for (const auto& k : KEYS) {
            size_t p = resp.find(k.key);
            if (p == std::string::npos) continue;
            size_t e = resp.find('}', p);
            std::string obj = resp.substr(p, e == std::string::npos ? std::string::npos : e - p);
            if (obj.find("\"Valid\":true") == std::string::npos) continue;
            add(k.kind, (float)number(obj, "Start"), (float)number(obj, "End"));
        }
    }
    if (out.segments.empty() &&
        httpRequest(serverUrl + "/Episode/" + episodeId + "/IntroTimestamps", "GET", "", "",
                    auth.accessToken, resp) == 200 && jsonGetBool(resp, "Valid"))
        add(MediaSegment::Intro, (float)number(resp, "IntroStart"), (float)number(resp, "IntroEnd"));

    std::sort(out.segments.begin(), out.segments.end(),
              [](const MediaSegment& a, const MediaSegment& b) { return a.start < b.start; });
    if (!out.segments.empty())
        SYS_Report("[Segments] %u to skip\n", (unsigned)out.segments.size());
    return true;
}

// ---------------------------------------------------------------------------
// Trickplay thumbnails (Jellyfin 10.9+)
// GET /Users/{u}/Items/{id}?Fields=Trickplay answers, among the rest:
//   "Trickplay":{"<mediaSourceId>":{"320":{"Width":320,"Height":180,
//     "TileWidth":10,"TileHeight":10,"ThumbnailCount":15,"Interval":10000,...}}}
// ---------------------------------------------------------------------------
bool JellyfinClient::getTrickplayInfo(const std::string& serverUrl,
                                      const JellyfinAuth& auth,
                                      const std::string& itemId,
                                      const std::string& mediaSourceId,
                                      TrickplayInfo& out)
{
    out = TrickplayInfo{};
    std::string url = serverUrl + "/Users/" + auth.userId + "/Items/" + itemId + "?Fields=Trickplay";
    std::string resp;
    if (httpRequest(url, "GET", "", "", auth.accessToken, resp) != 200) return false;

    size_t p = resp.find("\"Trickplay\":{");
    if (p == std::string::npos) return false;
    p += 12;                                     /* the map's '{' */
    /* this media source's sizes, or the first source's */
    size_t src = mediaSourceId.empty() ? std::string::npos
                                       : resp.find("\"" + mediaSourceId + "\":{", p);
    src = src != std::string::npos ? resp.find('{', src) : resp.find('{', p + 1);
    if (src == std::string::npos) return false;

    /* each size is an object one level down: keep the narrowest */
    int depth = 0;
    size_t start = 0;
    for (size_t i = src; i < resp.size(); ++i) {
        if (resp[i] == '{') {
            if (++depth == 2) start = i;
        } else if (resp[i] == '}') {
            if (depth == 2) {
                std::string o = resp.substr(start, i + 1 - start);
                TrickplayInfo t;
                t.width      = jsonGetInt(o, "Width");
                t.height     = jsonGetInt(o, "Height");
                t.tileW      = jsonGetInt(o, "TileWidth");
                t.tileH      = jsonGetInt(o, "TileHeight");
                t.count      = jsonGetInt(o, "ThumbnailCount");
                t.intervalMs = jsonGetInt(o, "Interval");
                if (t.ok() && (!out.ok() || t.width < out.width)) out = t;
            }
            if (--depth == 0) break;
        }
    }
    SYS_Report("[Trickplay] %s: %dx%d, %d thumbnails every %d ms\n",
               out.ok() ? "found" : "none", out.width, out.height, out.count, out.intervalMs);
    return out.ok();
}

bool JellyfinClient::getTrickplayTile(const std::string& serverUrl,
                                      const JellyfinAuth& auth,
                                      const std::string& itemId,
                                      const std::string& mediaSourceId,
                                      int width, int index,
                                      std::string& outBytes)
{
    char path[160];
    snprintf(path, sizeof(path), "/Videos/%s/Trickplay/%d/%d.jpg?MediaSourceId=%s",
             itemId.c_str(), width, index, mediaSourceId.c_str());
    outBytes.clear();
    return httpRequest(serverUrl + path, "GET", "", "", auth.accessToken, outBytes) == 200;
}

bool JellyfinClient::getSubtitleSrt(const std::string& serverUrl,
                                    const JellyfinAuth& auth,
                                    const std::string& itemId,
                                    const std::string& mediaSourceId,
                                    int streamIndex,
                                    std::string& outSrt)
{
    char path[200];
    snprintf(path, sizeof(path), "/Videos/%s/%s/Subtitles/%d/0/Stream.srt",
             itemId.c_str(), mediaSourceId.c_str(), streamIndex);
    outSrt.clear();
    /* an anime's ASS track (karaoke, effects) can be megabytes as SRT */
    int st = httpRequest(serverUrl + path, "GET", "", "", auth.accessToken, outSrt, 4 * 1024 * 1024);
    SYS_Report("[Subtitles] track %d: HTTP %d, %u bytes\n", streamIndex, st, (unsigned)outSrt.size());
    return st == 200 && !outSrt.empty();
}

// ---------------------------------------------------------------------------
// Fetch Audio tracks for a MusicAlbum
// GET /Users/{userId}/Items?ParentId={albumId}&IncludeItemTypes=Audio
// ---------------------------------------------------------------------------
bool JellyfinClient::getIsFavorite(const std::string& serverUrl, const JellyfinAuth& auth,
                                    const std::string& itemId, bool& favorite, int* specials)
{
    std::string resp;
    int status = httpRequest(serverUrl + "/Users/" + auth.userId + "/Items/" + itemId,
                             "GET", "", "", auth.accessToken, resp);
    if (status != 200) return false;
    favorite = jsonGetBool(resp, "IsFavorite");
    if (specials) *specials = jsonGetInt(resp, "SpecialFeatureCount") + jsonGetInt(resp, "LocalTrailerCount");
    return true;
}

bool JellyfinClient::getSpecialFeatures(const std::string& serverUrl, const JellyfinAuth& auth,
                                         const std::string& itemId, std::vector<JellyfinItem>& out)
{
    out.clear();
    struct Ctx { JellyfinClient* self; std::vector<JellyfinItem>* out; };
    Ctx ctx{ this, &out };
    bool any = false;
    /* both answer a bare array: read as the "Items" of a list */
    for (const char* route : { "/LocalTrailers", "/SpecialFeatures" }) {
        std::string resp;
        if (httpRequest(serverUrl + "/Items/" + itemId + route + "?userId=" + auth.userId,
                        "GET", "", "", auth.accessToken, resp) != 200) continue;
        any = true;
        forEachItemObject("{\"Items\":" + resp + "}", [](const std::string& obj, void* vctx) {
            Ctx* c = static_cast<Ctx*>(vctx);
            JellyfinItem it;
            it.id           = c->self->jsonGetString(obj, "Id");
            it.name         = c->self->jsonGetString(obj, "Name");
            it.type         = c->self->jsonGetString(obj, "Type");
            it.extraType    = c->self->jsonGetString(obj, "ExtraType");
            it.runtimeTicks = c->self->jsonGetLongLong(obj, "RunTimeTicks");
            it.playbackPositionTicks = c->self->jsonGetLongLong(obj, "PlaybackPositionTicks");
            if (it.extraType.empty()) it.extraType = it.type == "Trailer" ? "Trailer" : "Extra";
            if (!it.id.empty()) c->out->push_back(it);
        }, &ctx);
    }
    if (!any) errMsg = "No special features";
    return any;
}

bool JellyfinClient::getItemsByIds(const std::string& serverUrl,
                                    const JellyfinAuth& auth,
                                    const std::vector<std::string>& ids,
                                    std::vector<JellyfinItem>& outItems,
                                    std::vector<JellyfinAudioItem>& outAudio)
{
    outItems.clear();
    outAudio.clear();
    if (ids.empty()) return false;
    std::string list;
    for (size_t i = 0; i < ids.size() && i < 200; ++i) list += (i ? "," : "") + ids[i];
    std::string url = serverUrl + "/Users/" + auth.userId + "/Items?Ids=" + list +
                      "&Fields=RunTimeTicks,AlbumArtist,Album,SeriesId&EnableImages=false";
    std::string resp;
    int status = httpRequest(url, "GET", "", "", auth.accessToken, resp, 2u << 20);
    if (status != 200) {
        if (status >= 0) errMsg = "getItemsByIds failed (HTTP " + std::to_string(status) + ")";
        return false;
    }
    struct Ctx { JellyfinClient* self; std::vector<JellyfinItem> items; std::vector<JellyfinAudioItem> audio; };
    Ctx ctx{ this, {}, {} };
    forEachItemObject(resp, [](const std::string& obj, void* vctx) {
        Ctx* c = static_cast<Ctx*>(vctx);
        JellyfinItem it;
        it.id           = c->self->jsonGetString(obj, "Id");
        it.name         = c->self->jsonGetString(obj, "Name");
        it.type         = c->self->jsonGetString(obj, "Type");
        it.seriesId     = c->self->jsonGetString(obj, "SeriesId");
        it.runtimeTicks = c->self->jsonGetLongLong(obj, "RunTimeTicks");
        JellyfinAudioItem a;
        a.id           = it.id;
        a.name         = it.name;
        a.artist       = c->self->jsonGetString(obj, "AlbumArtist");
        a.album        = c->self->jsonGetString(obj, "Album");
        a.runtimeTicks = it.runtimeTicks;
        if (!it.id.empty()) { c->items.push_back(it); c->audio.push_back(a); }
    }, &ctx);
    /* the server's order is its own: back to the one asked for */
    for (const std::string& id : ids)
        for (size_t i = 0; i < ctx.items.size(); ++i)
            if (ctx.items[i].id == id) { outItems.push_back(ctx.items[i]); outAudio.push_back(ctx.audio[i]); break; }
    return !outItems.empty();
}

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
// Music player: one track's details, favourites, autoplay
// ---------------------------------------------------------------------------
namespace {
/* The strings of "key":["a","b"] */
std::vector<std::string> jsonStringArray(const std::string& json, const std::string& key) {
    std::vector<std::string> out;
    size_t p = json.find("\"" + key + "\":[");
    if (p == std::string::npos) return out;
    p += key.size() + 4;
    while (p < json.size() && json[p] != ']') {
        if (json[p] != '"') { ++p; continue; }
        std::string raw;
        for (++p; p < json.size() && json[p] != '"'; ++p) {
            if (json[p] == '\\' && p + 1 < json.size()) raw += json[p++];
            raw += json[p];
        }
        ++p;
        out.push_back(decodeJsonString(raw));
    }
    return out;
}
} // namespace

bool JellyfinClient::getAudioTrackInfo(const std::string& serverUrl,
                                       const JellyfinAuth& auth,
                                       const std::string& itemId,
                                       AudioTrackInfo& out)
{
    out = AudioTrackInfo();
    std::string resp;
    if (httpRequest(serverUrl + "/Users/" + auth.userId + "/Items/" + itemId, "GET", "", "",
                    auth.accessToken, resp) != 200)
        return false;
    for (const std::string& a : jsonStringArray(resp, "Artists"))
        out.artists += (out.artists.empty() ? "" : ", ") + a;
    if (out.artists.empty()) out.artists = jsonGetString(resp, "AlbumArtist");
    out.album         = jsonGetString(resp, "Album");
    out.albumId       = jsonGetString(resp, "AlbumId");
    out.parentId      = jsonGetString(resp, "ParentId");
    out.year          = jsonGetInt(resp, "ProductionYear");
    out.isFavorite    = jsonGetBool(resp, "IsFavorite");
    out.hasImage      = resp.find("\"ImageTags\":{\"Primary\"") != std::string::npos;
    out.albumHasImage = !jsonGetString(resp, "AlbumPrimaryImageTag").empty();
    return true;
}

bool JellyfinClient::setPlayed(const std::string& serverUrl, const JellyfinAuth& auth,
                               const std::string& itemId, bool played)
{
    /* /UserPlayedItems since 10.9; the older route for older servers */
    std::string resp;
    const char* method = played ? "POST" : "DELETE";
    int st = httpRequest(serverUrl + "/UserPlayedItems/" + itemId + "?userId=" + auth.userId,
                         method, "", "", auth.accessToken, resp);
    if (st == 404)
        st = httpRequest(serverUrl + "/Users/" + auth.userId + "/PlayedItems/" + itemId,
                         method, "", "", auth.accessToken, resp);
    SYS_Report("[Played] %s: HTTP %d\n", played ? "on" : "off", st);
    return st == 200 || st == 204;
}

bool JellyfinClient::setFavorite(const std::string& serverUrl,
                                 const JellyfinAuth& auth,
                                 const std::string& itemId,
                                 bool favorite)
{
    /* /UserFavoriteItems since 10.9; the older route for older servers */
    std::string resp;
    const char* method = favorite ? "POST" : "DELETE";
    int st = httpRequest(serverUrl + "/UserFavoriteItems/" + itemId + "?userId=" + auth.userId,
                         method, "", "", auth.accessToken, resp);
    if (st == 404)
        st = httpRequest(serverUrl + "/Users/" + auth.userId + "/FavoriteItems/" + itemId,
                         method, "", "", auth.accessToken, resp);
    SYS_Report("[Favorite] %s: HTTP %d\n", favorite ? "on" : "off", st);
    return st == 200 || st == 204;
}

bool JellyfinClient::getAutoplayTracks(const std::string& serverUrl,
                                       const JellyfinAuth& auth,
                                       const std::string& itemId,
                                       const std::string& parentId,
                                       int limit,
                                       std::vector<JellyfinAudioItem>& out)
{
    out.clear();
    auto fetch = [&](const std::string& pathAndQuery) {
        std::string resp;
        if (httpRequest(serverUrl + pathAndQuery, "GET", "", "", auth.accessToken, resp) != 200)
            return;
        struct Ctx { JellyfinClient* self; std::vector<JellyfinAudioItem>* out; const std::string* skip; };
        Ctx ctx{ this, &out, &itemId };
        forEachItemObject(resp, [](const std::string& obj, void* vctx) {
            Ctx* c = static_cast<Ctx*>(vctx);
            JellyfinAudioItem it;
            it.id           = c->self->jsonGetString(obj, "Id");
            it.name         = c->self->jsonGetString(obj, "Name");
            it.artist       = c->self->jsonGetString(obj, "AlbumArtist");
            it.album        = c->self->jsonGetString(obj, "Album");
            it.runtimeTicks = c->self->jsonGetLongLong(obj, "RunTimeTicks");
            if (!it.id.empty() && it.id != *c->skip && c->self->jsonGetString(obj, "Type") == "Audio")
                c->out->push_back(it);
        }, &ctx);
    };
    char q[256];
    snprintf(q, sizeof(q), "/Items/%s/InstantMix?UserId=%s&Limit=%d&Fields=RunTimeTicks&EnableImages=false",
             itemId.c_str(), auth.userId.c_str(), limit);
    fetch(q);
    if (out.empty() && !parentId.empty()) {
        snprintf(q, sizeof(q), "/Users/%s/Items?ParentId=%s&IncludeItemTypes=Audio&Recursive=true"
                 "&SortBy=Random&Limit=%d&Fields=RunTimeTicks&EnableImages=false",
                 auth.userId.c_str(), parentId.c_str(), limit);
        fetch(q);
    }
    SYS_Report("[Music] autoplay: %u tracks\n", (unsigned)out.size());
    return !out.empty();
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
                                        std::string& outPlaySessionId,
                                        std::string* outPlayMethod,
                                        int* outAudioIndex)
{
    /* Step 1: POST PlaybackInfo with AutoOpenLiveStream:false to obtain a
     * PlaySessionId only, without opening any server-side transcode session.
     * We do NOT use the TranscodingUrl from this response — for audio items
     * Jellyfin often returns a video container URL (TS MPEG2VIDEO) which is
     * wrong.  Instead we build the /Audio/universal URL ourselves (step 2). */
    char fullUrl[512];
    snprintf(fullUrl, sizeof(fullUrl),
        "%s/Items/%s/PlaybackInfo"
        "?UserId=%s&DeviceId=%s",
        serverUrl.c_str(), itemId.c_str(), auth.userId.c_str(), s_deviceId.c_str());

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

    /* Sent as it is when MPlayer CE plays it (its audio demuxer reads MP3,
     * FLAC and WAV): MP3 up to 320 kb/s, FLAC and WAV in stereo up to 48 kHz
     * (the Wii outputs 16-bit stereo at 48 kHz: more is converted on its
     * CPU for nothing).  Anything else is transcoded to MP3.
     * Not /Audio/{id}/universal: it opens a PlaybackInfo of its own and
     * runs the transcode under that PlaySessionId, so the server took our
     * reports for direct play and the transcode was never stopped. */
    std::string container = jsonGetString(resp, "Container");
    std::string codec;
    int bitrate = 0, index = 0, rate = 0, channels = 0, bits = 0;
    {
        size_t t = resp.find("\"Type\":\"Audio\"");
        if (t != std::string::npos) {
            size_t o = resp.rfind('{', t), e = resp.find('}', t);
            if (o != std::string::npos && e != std::string::npos) {
                std::string stream = resp.substr(o, e - o + 1);
                codec    = jsonGetString(stream, "Codec");
                bitrate  = jsonGetInt(stream, "BitRate");
                index    = jsonGetInt(stream, "Index");
                rate     = jsonGetInt(stream, "SampleRate");
                channels = jsonGetInt(stream, "Channels");
                bits     = jsonGetInt(stream, "BitDepth");
            }
        }
    }
    const bool mp3  = container == "mp3" && codec == "mp3";
    const bool flac = container == "flac" && codec == "flac";
    const bool wav  = container == "wav" && (codec == "pcm_s16le" || codec == "pcm_s24le");
    /* why not, in Jellyfin's words, for the dashboard */
    std::string why;
    auto add = [&](const char* r) { why += (why.empty() ? "" : ",") + std::string(r); };
    if (!mp3 && !flac && !wav) {
        if (container != "mp3" && container != "flac" && container != "wav") add("ContainerNotSupported");
        else add("AudioCodecNotSupported");
    }
    if (mp3 && bitrate > 320000) add("AudioBitrateNotSupported");
    if ((flac || wav) && rate > 48000) add("AudioSampleRateNotSupported");
    if ((flac || wav) && bits > 24) add("AudioBitDepthNotSupported");
    if (channels > 2) add("AudioChannelsNotSupported");
    const bool direct = why.empty();
    if (outPlayMethod) *outPlayMethod = direct ? "DirectPlay" : "Transcode";
    if (outAudioIndex) *outAudioIndex = index;
    SYS_Report("[AudioPlaybackInfo] %s %s %d kb/s %d Hz %d ch: %s%s\n", container.c_str(), codec.c_str(),
               bitrate / 1000, rate, channels, direct ? "sent as it is" : "transcoded to MP3, ",
               why.c_str());

    char audioUrl[1024];
    std::string schemedSvr = addScheme(serverUrl);
    if (direct)
        snprintf(audioUrl, sizeof(audioUrl),
            "%s/Audio/%s/stream?static=true&MediaSourceId=%s&DeviceId=%s&PlaySessionId=%s&ApiKey=%s",
            schemedSvr.c_str(), itemId.c_str(), itemId.c_str(), s_deviceId.c_str(),
            outPlaySessionId.c_str(), auth.accessToken.c_str());
    else
        snprintf(audioUrl, sizeof(audioUrl),
            "%s/Audio/%s/stream.mp3?MediaSourceId=%s&DeviceId=%s&PlaySessionId=%s"
            "&AudioCodec=mp3&AudioBitRate=320000&MaxAudioChannels=2&StartTimeTicks=%lld"
            "&TranscodeReasons=%s&ApiKey=%s",
            schemedSvr.c_str(), itemId.c_str(), itemId.c_str(), s_deviceId.c_str(),
            outPlaySessionId.c_str(), (long long)startTimeTicks, why.c_str(), auth.accessToken.c_str());
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

// ---------------------------------------------------------------------------
// Generic query used by the carousel home (Flix theme)
// ---------------------------------------------------------------------------
static float jsonGetFloat(const std::string& json, const char* key) {
    std::string search = std::string("\"") + key + "\":";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return 0.0f;
    return (float)strtod(json.c_str() + pos + search.size(), nullptr);
}

bool JellyfinClient::getItemsByQuery(const std::string& serverUrl,
                                     const JellyfinAuth& auth,
                                     const std::string& pathAndQuery,
                                     std::vector<JellyfinItem>& out,
                                     int* totalCount) {
    std::string url = serverUrl + pathAndQuery +
        (pathAndQuery.find('?') == std::string::npos ? "?" : "&") +
        "Fields=ProductionYear,UserData,RunTimeTicks,SeriesId,ChildCount,RecursiveItemCount,SortName"
        "&EnableImages=false";
    std::string resp;
    int status = httpRequest(url, "GET", "", "", auth.accessToken, resp);
    if (status != 200) {
        if (status >= 0) errMsg = "Query failed (HTTP " + std::to_string(status) + ")";
        return false;
    }

    if (totalCount) *totalCount = jsonGetInt(resp, "TotalRecordCount");
    size_t pos = resp.find("\"Items\":");
    pos = resp.find('[', pos == std::string::npos ? 0 : pos);
    if (pos == std::string::npos) return true;

    struct Ctx { JellyfinClient* self; std::vector<JellyfinItem>* out; };
    Ctx ctx{ this, &out };
    forEachArrayObject(resp, pos, [](const std::string& obj, void* vctx) {
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
        item.communityRating       = jsonGetFloat(obj, "CommunityRating");
        item.officialRating        = c->self->jsonGetString(obj, "OfficialRating");
        item.childCount            = c->self->jsonGetInt(obj,    "ChildCount");
        item.recursiveItemCount    = c->self->jsonGetInt(obj,    "RecursiveItemCount");
        item.sortName              = c->self->jsonGetString(obj, "SortName");
        if (!item.id.empty() && !item.name.empty())
            c->out->push_back(item);
    }, &ctx);
    return true;
}
