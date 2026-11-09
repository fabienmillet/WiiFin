#include "JellyfinClient.h"
#include <algorithm>
#include "../core/ExitZone.h"
#include "../core/Lang.h"
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
#include <map>
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
static std::string s_deviceId = "wiiplex-wii";

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
    if (ret >= 0) snprintf(buf, sizeof(buf), "%s", N_("No IP address from the router (DHCP)"));
    else          snprintf(buf, sizeof(buf), "Network start-up failed (error %d)", (int)ret);
    errMsg = buf;
}

/* the network restarting (RemoteControl waits for it, its sockets apart) */
volatile bool g_netRestarting = false;

void* JellyfinClient::netThreadMain(void* self) {
    JellyfinClient* c = static_cast<JellyfinClient*>(self);
    if (c->netRestart) {
        c->netRestart = false;
        g_netRestarting = true;
        net_deinit();
    }
    c->bringUpNetwork();
    g_netRestarting = false;
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
        errMsg = N_("UDP socket failed");
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
    dest255.sin_port        = htons(32414);   /* Plex GDM */
    dest255.sin_addr.s_addr = 0xFFFFFFFFu;

    destSubnet.sin_family = AF_INET;
    destSubnet.sin_port   = htons(32414);
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
    const char* msg = "M-SEARCH * HTTP/1.1\r\n\r\n";
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

            /* GDM's answer: "HTTP/1.0 200 OK\r\nName: ...\r\nPort: 32400\r\n..." */
            std::string txt(buf, n);
            auto field = [&](const char* k) {
                std::string key = std::string("\n") + k + ": ";
                size_t a = txt.find(key);
                if (a == std::string::npos) return std::string();
                a += key.size();
                size_t e = txt.find_first_of("\r\n", a);
                return txt.substr(a, e == std::string::npos ? std::string::npos : e - a);
            };
            std::string name = field("Name");
            std::string port = field("Port");
            std::string address;
            if (txt.find("plex/media-server") != std::string::npos && !port.empty())
                address = std::string("http://") + inet_ntoa(from.sin_addr) + ":" + port;
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
    /* no space has a place in a URL: one typed around it (the keyboard's
     * space bar) would make the host a name to look up */
    std::string bare;
    for (char ch : rawUrl) if (!isspace((unsigned char)ch)) bare += ch;
    std::string u = normScheme(bare);
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
            errMsg = N_("Unsupported URL scheme");
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
            err = N_("TLS: DRBG seed failed");
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
            err = N_("TLS: ssl_config_defaults failed");
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
    /* each name's last address: the Wii's DNS failing a while (it does, its
     * Wi-Fi weak), that one again; a network really gone then fails to
     * connect, and starts again */
    static std::map<std::string, struct in_addr> s_known;
    struct hostent* he = net_gethostbyname(host.c_str());
    if (!he || he->h_length > (int)sizeof(addr.sin_addr)) {
        auto k = s_known.find(host);
        if (k != s_known.end()) {
            SYS_Report("[Net] DNS failed: its last address again\n");
            addr.sin_addr = k->second;
            return true;
        }
    }
    if (!he) { err = "DNS failed: " + host; SYS_Report("[Net] DNS failed: \"%s\" (%u bytes)\n", host.c_str(), (unsigned)host.size()); return false; }
    if (he->h_length > (int)sizeof(addr.sin_addr)) { err = N_("DNS: unexpected address length"); return false; }
    memcpy(&addr.sin_addr, he->h_addr, he->h_length);
    s_known[host] = addr.sin_addr;
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
        err = cr == -ETIMEDOUT ? N_("Server not reachable (connection timed out)")
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
                err = N_("TLS handshake failed (network error)");
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
        err = N_("Failed to send request");
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
        err = rawLen == 0 ? N_("No response from server (check server URL / port)")
                          : N_("Invalid HTTP response (server may require HTTPS)");
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

/* The network lost (the Wi-Fi gone...): started again in the background.
 * It may take a long while (if_config's tries, the access point away): the
 * request fails at once meanwhile, the interface goes on (it froze 100 s on
 * a tester's Wii, its Wi-Fi gone); the next ones work once it is back */
bool JellyfinClient::recoverNetwork() {
    if (netBusy) return false;          /* already restarting */
    takeNetworkResult();
    closeConn();
    networkReady = false;
    netRestart = true;
    startNetwork();
    return false;
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
    /* the network starting again (after a loss): not now */
    if (netBusy) return fail(N_("Network lost: reconnecting..."));
    if (!networkReady && takeNetworkResult() == false && netThread == 0 && netRestart == false) {
        /* the last restart failed: another, in the background */
        recoverNetwork();
        return fail(N_("Network lost: reconnecting..."));
    }
    if (tls && !tlsSharedInit(sslVerify, err)) return fail(err);

    std::string authHdr = authToken.empty() ? "" : ("X-Plex-Token: " + authToken + "\r\n");
    std::string ctHdr   = contentType.empty() ? "" : ("Content-Type: " + contentType + "\r\n");
    // RFC 7230 §5.4: omit port from Host when it is the default for the scheme.
    std::string hostHdr = host + (port == (tls ? 443 : 80) ? "" : (":" + std::to_string(port)));
    char req[4096];
    int reqLen = snprintf(req, sizeof(req),
        "%s %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Connection: keep-alive\r\n"
        "Accept: application/json\r\n"
        "X-Plex-Client-Identifier: %s\r\n"
        "X-Plex-Product: WiiPlex\r\nX-Plex-Version: " WIIFIN_VERSION "\r\n"
        "X-Plex-Platform: Generic\r\nX-Plex-Device: Nintendo Wii\r\nX-Plex-Device-Name: Wii\r\n"
        "%s"
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
    if (reqLen >= (int)sizeof(req)) return fail(N_("Request headers too large"));

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
                    return fail(N_("Network lost: reconnecting..."));
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
    return fail(N_("Connection lost"));
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


#include "PlexApi.inc"
