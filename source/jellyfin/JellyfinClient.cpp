#include "JellyfinClient.h"
#include "../core/ExitZone.h"
#include "../core/Log.h"
#include "../core/NetConnect.h"
#include <network.h>
#include <ogc/if_config.h>
#include <ogc/lwp.h>
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
                                 std::string& responseBody) {
    std::string host, basePath;
    int port;
    bool isHttps;
    if (!parseUrl(url, host, port, basePath, isHttps)) return -1;
    return request(isHttps, host, port, basePath, method, contentType, body,
                   authToken, responseBody);
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
    /* No close_notify: the socket may already have been closed behind our
     * back (MPlayer closes every IOS socket when a video session ends). */
    if (s_conn.tls) mbedtls_ssl_free(&s_conn.ssl);
    net_close(s_conn.sock);
    s_conn.open = false;
    s_conn.sock = -1;
}

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
                  std::string& err) {
    if (!connWrite((const unsigned char*)req, reqLen) ||
        (!body.empty() && !connWrite((const unsigned char*)body.data(), (int)body.size()))) {
        if (reused) return ExResult::Stale;
        err = "Failed to send request";
        return ExResult::Failed;
    }

    static char rawBuf[256 * 1024] DEAD_AT_EXIT;
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
        int canCopy = (int)(sizeof(rawBuf) - 1) - rawLen;
        if (canCopy <= 0) { keepAlive = false; break; }   /* response too large */
        if (ret < canCopy) canCopy = ret;
        memcpy(rawBuf + rawLen, buf, (size_t)canCopy);
        rawLen += canCopy;

        if (bodyStart < 0) {
            rawBuf[rawLen] = '\0';
            const char* sep = strstr(rawBuf, "\r\n\r\n");
            if (!sep) continue;
            bodyStart = (int)(sep - rawBuf) + 4;
            if (strncmp(rawBuf, "HTTP/", 5) == 0) {
                const char* sp = strchr(rawBuf, ' ');
                if (sp) status = atoi(sp + 1);
            }
            std::string te = headerValue(rawBuf, bodyStart, "transfer-encoding");
            std::string cl = headerValue(rawBuf, bodyStart, "content-length");
            std::string cn = headerValue(rawBuf, bodyStart, "connection");
            chunked    = te.find("chunked") != std::string::npos;
            contentLen = cl.empty() ? -1 : atol(cl.c_str());
            noBody     = status == 204 || status == 304 || (status >= 100 && status < 200);
            if (cn.find("close") != std::string::npos ||
                strncmp(rawBuf, "HTTP/1.0", 8) == 0) keepAlive = false;
            /* Without a length the body ends when the server closes */
            if (!chunked && contentLen < 0 && !noBody) keepAlive = false;
        }
        int have = rawLen - bodyStart;
        if (noBody)                complete = true;
        else if (chunked)          complete = chunkedComplete(rawBuf + bodyStart, have);
        else if (contentLen >= 0)  complete = have >= contentLen;
    }
    rawBuf[rawLen] = '\0';
    if (!complete) keepAlive = false;

    if (status == 0) {
        err = rawLen == 0 ? "No response from server (check server URL / port)"
                          : "Invalid HTTP response (server may require HTTPS)";
        return ExResult::Failed;
    }
    if (bodyStart >= 0)
        responseBody = decodeChunked(rawBuf, bodyStart - 4, rawBuf + bodyStart, rawLen - bodyStart);
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
                            std::string& responseBody) {
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
        "Authorization: " WIIFIN_CLIENT_HDR "%s\r\n"
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
                              keepAlive, err);
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
    return fetchEpisodes(serverUrl + qs, auth, out);
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
                                   std::vector<JellyfinEpisode>& out) {
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

const char* JellyfinClient::videoQualityName(int q) {
    static const char* names[VIDEO_QUALITY_COUNT] = { "Low", "Normal", "High" };
    return (q >= 0 && q < VIDEO_QUALITY_COUNT) ? names[q] : names[1];
}

int JellyfinClient::videoBitrate() const {
    /* MPEG-4 ASP at 640 px.  Decoding cost grows with the bitrate (Dolphin,
     * grainy 1080p source: 15% of real time at 1.5 Mb/s, 25% at 2.5, 39% at
     * 4), so High stops at 3.5 to leave headroom on real hardware.  Playback
     * steps down on its own when the link can't keep up. */
    static const int bitrates[VIDEO_QUALITY_COUNT] = { 1500000, 2500000, 3500000 };
    int q = effectiveQuality();
    return bitrates[(q >= 0 && q < VIDEO_QUALITY_COUNT) ? q : 1];
}

/* Start at a quality the link can carry instead of finding out through two
 * rebuffers: time a download from Jellyfin's bitrate test (the same probe
 * its own clients use), then cap the quality to the highest level whose
 * stream fits with 30 % to spare for the cache to fill.  The setting itself
 * is left alone. */
void JellyfinClient::measureLink(const std::string& serverUrl, const JellyfinAuth& auth) {
    if (linkMeasuredFor == serverUrl) return;
    std::string body;
    u64 t0 = nowMs();
    int status = httpRequest(serverUrl + "/Playback/BitrateTest?size=300000", "GET", "", "",
                             auth.accessToken, body);
    u64 ms = nowMs() - t0;
    if (status != 200 || body.size() < 100000) {
        SYS_Report("[Net] link speed not measured (HTTP %d)\n", status);
        return;                                  /* try again next playback */
    }
    linkMeasuredFor = serverUrl;
    double kbps = body.size() * 8.0 / (ms ? ms : 1);
    static const int bitrates[VIDEO_QUALITY_COUNT] = { 1500000, 2500000, 3500000 };
    int cap = VIDEO_QUALITY_COUNT - 1;
    while (cap > 0 && (bitrates[cap] + 128000) / 1000.0 * 1.3 > kbps) --cap;
    linkCap = cap;
    SYS_Report("[Net] link %.1f Mb/s (%u bytes in %llu ms): quality up to %s\n",
               kbps / 1000.0, (unsigned)body.size(), ms, videoQualityName(cap));
}

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
        "&ApiKey=%s",
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
