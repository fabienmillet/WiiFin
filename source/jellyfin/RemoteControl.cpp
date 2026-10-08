/* Remote control: the server's WebSocket (see RemoteControl.h).
 *
 *   GET /socket?deviceId=<id>  (HTTP/1.1 Upgrade: websocket, the usual Authorization)
 *   <- {"MessageType":"ForceKeepAlive","Data":60}   send KeepAlive every Data/2 s
 *   <- {"MessageType":"Playstate","Data":{"Command":"Pause",...}}
 *   <- {"MessageType":"GeneralCommand","Data":{"Name":"DisplayMessage",
 *                                              "Arguments":{"Header":..,"Text":..,"TimeoutMs":..}}}
 */
#include "RemoteControl.h"
#include "JellyfinClient.h"
#include "../core/NetConnect.h"
#include "../ui/Ui.h"
#include <ogc/lwp.h>
#include <ogc/mutex.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/system.h>
#include <network.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <deque>
#include <string>
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/x509_crt.h>

#include "../player/WiiPlayer.h"   /* wii_player_keep_socket */
#include "../version.h"

mbedtls_x509_crt* wiifin_ca_chain();
int wiifin_cert_verify(void*, mbedtls_x509_crt*, int, uint32_t* flags);

namespace {

/* ---- State shared with the main thread ---------------------------------- */

struct Message { std::string header, text; int ms; };

mutex_t                     s_lock = LWP_MUTEX_NULL;
std::deque<Remote::Command> s_queue;
std::deque<Message>         s_messages;
Remote::PlayRequest         s_play;
u64                         s_playAt = 0;   /* when it came, 0 = none */
volatile bool               s_playerActive = false;
volatile bool               s_connected    = false;

struct Lock {
    Lock()  { LWP_MutexLock(s_lock); }
    ~Lock() { LWP_MutexUnlock(s_lock); }
};

/* ---- The thread ---------------------------------------------------------- */

lwp_t         s_thread = LWP_THREAD_NULL;
volatile bool s_run    = false;
u8            s_stack[64 * 1024] ATTRIBUTE_ALIGN(32);   /* a TLS handshake needs room */
std::string   s_host, s_path, s_token;
int           s_port   = 80;
bool          s_https  = false;
bool          s_verify = true;

u64 nowMs() { return ticks_to_millisecs(gettime()); }

/* sleep up to ms, waking early when stopped */
void nap(int ms) {
    for (int t = 0; t < ms && s_run; t += 100) usleep(100 * 1000);
}

struct Ws {
    s32  sock = -1;
    bool tls = false, tlsReady = false;
    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_ssl_config       conf;
    mbedtls_ssl_context      ssl;
    std::string              in;   /* bytes read past what was asked for */
};

/* readable within ms (TLS: bytes already decrypted count) */
bool readable(Ws& w, int ms) {
    if (!w.in.empty()) return true;
    if (w.tls && mbedtls_ssl_get_bytes_avail(&w.ssl) > 0) return true;
    fd_set fds; FD_ZERO(&fds); FD_SET(w.sock, &fds);
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    return net_select(w.sock + 1, &fds, nullptr, nullptr, &tv) > 0;
}

int bioSend(void* ctx, const unsigned char* buf, size_t len) {
    s32 sock = *(s32*)ctx;
    int r = net_write(sock, (void*)buf, (u32)len);
    if (r == -EAGAIN || r == -EWOULDBLOCK) { usleep(5000); return MBEDTLS_ERR_SSL_WANT_WRITE; }
    return r < 0 ? MBEDTLS_ERR_NET_SEND_FAILED : r;
}

int bioRecv(void* ctx, unsigned char* buf, size_t len) {
    s32 sock = *(s32*)ctx;
    fd_set fds; FD_ZERO(&fds); FD_SET(sock, &fds);
    struct timeval tv = { 10, 0 };
    if (net_select(sock + 1, &fds, nullptr, nullptr, &tv) <= 0) return MBEDTLS_ERR_NET_RECV_FAILED;
    int r = net_read(sock, buf, (u32)len);
    if (r == -EAGAIN || r == -EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_READ;
    return r < 0 ? MBEDTLS_ERR_NET_RECV_FAILED : r;
}

void wsClose(Ws& w) {
    wii_player_keep_socket(0, -1);
    if (w.tlsReady) {
        mbedtls_ssl_free(&w.ssl);
        mbedtls_ssl_config_free(&w.conf);
        mbedtls_ctr_drbg_free(&w.drbg);
        mbedtls_entropy_free(&w.entropy);
        w.tlsReady = false;
    }
    if (w.sock >= 0) net_close(w.sock);
    w.sock = -1;
    w.in.clear();
}

bool sendAll(Ws& w, const void* data, int n) {
    const unsigned char* p = (const unsigned char*)data;
    while (n > 0) {
        int r = w.tls ? mbedtls_ssl_write(&w.ssl, p, (size_t)n) : bioSend(&w.sock, p, (size_t)n);
        if (r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (r <= 0) return false;
        p += r; n -= r;
    }
    return true;
}

/* > 0 bytes, <= 0 closed / error / 10 s without data */
int recvSome(Ws& w, unsigned char* buf, int n) {
    if (!w.in.empty()) {
        int k = (int)w.in.size() < n ? (int)w.in.size() : n;
        memcpy(buf, w.in.data(), k);
        w.in.erase(0, k);
        return k;
    }
    for (;;) {
        int r = w.tls ? mbedtls_ssl_read(&w.ssl, buf, (size_t)n) : bioRecv(&w.sock, buf, (size_t)n);
        if (r == MBEDTLS_ERR_SSL_WANT_READ) continue;
        return r;
    }
}

bool recvExact(Ws& w, unsigned char* buf, int n) {
    while (n > 0) {
        int r = recvSome(w, buf, n);
        if (r <= 0) return false;
        buf += r; n -= r;
    }
    return true;
}

std::string base64(const unsigned char* p, int n) {
    static const char* A = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    for (int i = 0; i < n; i += 3) {
        unsigned v = p[i] << 16 | (i + 1 < n ? p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        o += A[v >> 18 & 63]; o += A[v >> 12 & 63];
        o += i + 1 < n ? A[v >> 6 & 63] : '=';
        o += i + 2 < n ? A[v & 63] : '=';
    }
    return o;
}

u32 s_rand = 0;
u32 rnd() { s_rand = s_rand * 1664525u + 1013904223u + (u32)gettime(); return s_rand; }

/* TCP (+ TLS), then the WebSocket handshake.  err: why not, for the log. */
bool wsOpen(Ws& w, std::string& err) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(s_port);
    if (inet_aton(s_host.c_str(), &addr.sin_addr) == 0) {
        struct hostent* he = net_gethostbyname(s_host.c_str());
        if (!he || he->h_length > (int)sizeof(addr.sin_addr)) { err = "DNS failed"; return false; }
        memcpy(&addr.sin_addr, he->h_addr, he->h_length);
    }
    w.sock = net_socket(AF_INET, SOCK_STREAM, 0);
    if (w.sock < 0) { err = "no socket (" + std::to_string(w.sock) + ")"; return false; }
    wii_player_keep_socket(0, w.sock);
    int cr = connectWithTimeout(w.sock, &addr, 8);
    if (cr < 0) { err = "connect " + std::to_string(cr); wsClose(w); return false; }

    w.tls = s_https;
    if (w.tls) {
        mbedtls_entropy_init(&w.entropy);
        mbedtls_ctr_drbg_init(&w.drbg);
        mbedtls_ssl_config_init(&w.conf);
        mbedtls_ssl_init(&w.ssl);
        w.tlsReady = true;
        const char* pers = "wiifin_remote";
        int ret = mbedtls_ctr_drbg_seed(&w.drbg, mbedtls_entropy_func, &w.entropy,
                                        (const unsigned char*)pers, strlen(pers));
        if (ret == 0)
            ret = mbedtls_ssl_config_defaults(&w.conf, MBEDTLS_SSL_IS_CLIENT,
                                              MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
        if (ret == 0) {
            mbedtls_x509_crt* ca = s_verify ? wiifin_ca_chain() : nullptr;
            if (ca) {
                mbedtls_ssl_conf_ca_chain(&w.conf, ca, nullptr);
                mbedtls_ssl_conf_authmode(&w.conf, MBEDTLS_SSL_VERIFY_REQUIRED);
                mbedtls_ssl_conf_verify(&w.conf, wiifin_cert_verify, nullptr);
            } else {
                mbedtls_ssl_conf_authmode(&w.conf, MBEDTLS_SSL_VERIFY_NONE);
            }
            mbedtls_ssl_conf_rng(&w.conf, mbedtls_ctr_drbg_random, &w.drbg);
            ret = mbedtls_ssl_setup(&w.ssl, &w.conf);
        }
        if (ret == 0) {
            mbedtls_ssl_set_hostname(&w.ssl, s_host.c_str());
            mbedtls_ssl_set_bio(&w.ssl, &w.sock, bioSend, bioRecv, nullptr);
            do { ret = mbedtls_ssl_handshake(&w.ssl); }
            while (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE);
        }
        if (ret != 0) {
            char b[16]; snprintf(b, sizeof(b), "-0x%04X", (unsigned)-ret);
            err = std::string("TLS ") + b;
            wsClose(w);
            return false;
        }
    }

    unsigned char key[16];
    for (int i = 0; i < 16; i += 4) { u32 r = rnd(); memcpy(key + i, &r, 4); }
    std::string host = s_host;
    if (!(s_port == (s_https ? 443 : 80))) host += ":" + std::to_string(s_port);
    /* the same Authorization as every other request: the same session on
     * the server (newer ones refuse a token in the URL) */
    std::string req = "GET " + s_path + "/socket?deviceId=" + JellyfinClient::deviceId() + " HTTP/1.1\r\n"
                      "Host: " + host + "\r\n"
                      "Authorization: MediaBrowser Client=\"WiiFin\", Device=\"Nintendo Wii\", "
                      "DeviceId=\"" + JellyfinClient::deviceId() + "\", Version=\"" WIIFIN_VERSION "\", "
                      "Token=\"" + s_token + "\"\r\n"
                      "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                      "Sec-WebSocket-Key: " + base64(key, 16) + "\r\n"
                      "Sec-WebSocket-Version: 13\r\n\r\n";
    if (!sendAll(w, req.data(), (int)req.size())) { err = "send failed"; wsClose(w); return false; }

    /* the answer's headers; what follows them is the first frames */
    std::string head;
    unsigned char buf[512];
    while (head.find("\r\n\r\n") == std::string::npos) {
        if (head.size() > 8192) { err = "headers too long"; wsClose(w); return false; }
        int r = recvSome(w, buf, sizeof(buf));
        if (r <= 0) { err = "no answer"; wsClose(w); return false; }
        head.append((const char*)buf, r);
    }
    size_t end = head.find("\r\n\r\n") + 4;
    w.in = head.substr(end);
    head.resize(end);
    if (head.compare(0, 12, "HTTP/1.1 101") != 0) {
        err = "HTTP " + head.substr(9, 3);
        wsClose(w);
        return false;
    }
    return true;
}

/* one frame from WiiFin: masked, as every client frame must be */
bool sendFrame(Ws& w, int opcode, const std::string& payload) {
    std::string f;
    f += (char)(0x80 | opcode);
    size_t n = payload.size();
    if (n < 126) { f += (char)(0x80 | n); }
    else         { f += (char)(0x80 | 126); f += (char)(n >> 8); f += (char)(n & 0xFF); }
    u32 m = rnd();
    unsigned char mask[4];
    memcpy(mask, &m, 4);
    f.append((const char*)mask, 4);
    for (size_t i = 0; i < n; ++i) f += (char)(payload[i] ^ mask[i & 3]);
    return sendAll(w, f.data(), (int)f.size());
}

/* ---- JSON: just what the messages need ----------------------------------- */

void putUtf8(std::string& o, unsigned cp) {
    if (cp < 0x80) o += (char)cp;
    else if (cp < 0x800) { o += (char)(0xC0 | cp >> 6); o += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { o += (char)(0xE0 | cp >> 12); o += (char)(0x80 | (cp >> 6 & 0x3F));
                             o += (char)(0x80 | (cp & 0x3F)); }
    else { o += (char)(0xF0 | cp >> 18); o += (char)(0x80 | (cp >> 12 & 0x3F));
           o += (char)(0x80 | (cp >> 6 & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
}

/* the value of "key" (first one): a string decoded, or the number / true /
 * null as written; "" when absent */
std::string jsonValue(const std::string& j, const char* key) {
    std::string k = std::string("\"") + key + "\":";
    size_t p = j.find(k);
    if (p == std::string::npos) return "";
    p += k.size();
    while (p < j.size() && j[p] == ' ') ++p;
    std::string o;
    if (p < j.size() && j[p] == '"') {
        for (++p; p < j.size() && j[p] != '"'; ++p) {
            if (j[p] != '\\' || p + 1 >= j.size()) { o += j[p]; continue; }
            char e = j[++p];
            if (e == 'n') o += '\n';
            else if (e == 't') o += ' ';
            else if (e == 'u' && p + 4 < j.size()) {
                unsigned cp = (unsigned)strtoul(j.substr(p + 1, 4).c_str(), nullptr, 16);
                p += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && p + 6 < j.size() && j[p + 1] == '\\' && j[p + 2] == 'u') {
                    unsigned lo = (unsigned)strtoul(j.substr(p + 3, 4).c_str(), nullptr, 16);
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    p += 6;
                }
                putUtf8(o, cp);
            } else o += e;   /* \" \\ \/ */
        }
        return o;
    }
    while (p < j.size() && j[p] != ',' && j[p] != '}' && j[p] != ']') o += j[p++];
    return o;
}

void push(Remote::Cmd c, long long ticks = 0, int value = 0) {
    Lock l;
    Remote::Command cmd;
    cmd.cmd = c; cmd.ticks = ticks; cmd.value = value;
    s_queue.push_back(cmd);
}

/* one text message; keepAliveMs: the interval the server asks for */
void handle(const std::string& m, u64& keepAliveMs) {
    std::string type = jsonValue(m, "MessageType");
    if (type == "ForceKeepAlive") {
        long secs = atol(jsonValue(m, "Data").c_str());
        keepAliveMs = (u64)(secs > 10 ? secs : 10) * 500;   /* half of it */
    } else if (type == "Playstate") {
        std::string c = jsonValue(m, "Command");
        static const struct { const char* name; Remote::Cmd cmd; } MAP[] = {
            { "Pause", Remote::Cmd::Pause }, { "Unpause", Remote::Cmd::Unpause },
            { "PlayPause", Remote::Cmd::PlayPause }, { "Stop", Remote::Cmd::Stop },
            { "Seek", Remote::Cmd::Seek }, { "NextTrack", Remote::Cmd::Next },
            { "PreviousTrack", Remote::Cmd::Prev }, { "FastForward", Remote::Cmd::FastForward },
            { "Rewind", Remote::Cmd::Rewind },
        };
        for (const auto& e : MAP)
            if (c == e.name) push(e.cmd, atoll(jsonValue(m, "SeekPositionTicks").c_str()));
    } else if (type == "Play") {
        std::string cmd = jsonValue(m, "PlayCommand");
        Remote::PlayRequest r;
        r.queue = cmd == "PlayNext" || cmd == "PlayLast";
        r.next  = cmd == "PlayNext";
        if (!r.queue && cmd != "PlayNow" && !cmd.empty()) {
            SYS_Report("[Remote] play (%s): not supported\n", cmd.c_str());
            return;
        }
        size_t a = m.find("\"ItemIds\":[");
        size_t e = a == std::string::npos ? a : m.find(']', a);
        for (size_t p = a; p != std::string::npos && p < e; ) {
            size_t q1 = m.find('"', p + 11);
            if (q1 == std::string::npos || q1 > e) break;
            size_t q2 = m.find('"', q1 + 1);
            if (q2 == std::string::npos || q2 > e) break;
            r.ids.push_back(m.substr(q1 + 1, q2 - q1 - 1));
            p = q2 - 10;   /* the next search starts after this id */
        }
        r.startIndex = atoi(jsonValue(m, "StartIndex").c_str());
        r.startTicks = atoll(jsonValue(m, "StartPositionTicks").c_str());
        if (r.startIndex < 0 || r.startIndex >= (int)r.ids.size()) r.startIndex = 0;
        SYS_Report("[Remote] %s: %u item(s), from %lld s\n",
                   r.queue ? (r.next ? "play next" : "add to queue") : "play",
                   (unsigned)r.ids.size(), r.startTicks / 10000000LL);
        if (r.ids.empty()) return;
        Lock l;
        s_play   = r;
        s_playAt = nowMs();
    } else if (type == "GeneralCommand") {
        std::string name = jsonValue(m, "Name");
        if (name == "DisplayMessage") {
            Message msg;
            msg.header = jsonValue(m, "Header");
            msg.text   = jsonValue(m, "Text");
            int ms = atoi(jsonValue(m, "TimeoutMs").c_str());
            msg.ms = ms > 0 ? (ms > 60000 ? 60000 : ms) : 5000;
            SYS_Report("[Remote] message (%u characters, %d ms)\n",
                       (unsigned)(msg.header.size() + msg.text.size()), msg.ms);
            Lock l;
            s_messages.push_back(msg);
        } else if (name == "VolumeUp") {
            push(Remote::Cmd::VolumeUp);
        } else if (name == "VolumeDown") {
            push(Remote::Cmd::VolumeDown);
        } else if (name == "SetVolume") {
            push(Remote::Cmd::SetVolume, 0, atoi(jsonValue(m, "Volume").c_str()));
        } else if (name == "Mute") {
            push(Remote::Cmd::Mute);
        } else if (name == "Unmute") {
            push(Remote::Cmd::Unmute);
        } else if (name == "ToggleMute") {
            push(Remote::Cmd::ToggleMute);
        } else if (name == "SetAudioStreamIndex") {
            push(Remote::Cmd::AudioTrack, 0, atoi(jsonValue(m, "Index").c_str()));
        } else if (name == "SetSubtitleStreamIndex") {
            push(Remote::Cmd::SubtitleTrack, 0, atoi(jsonValue(m, "Index").c_str()));
        } else {
            SYS_Report("[Remote] %s: not supported\n", name.c_str());
        }
    }
}

/* connected: frames until the connection drops or stop() */
void session(Ws& w) {
    u64 keepAliveMs = 30000, lastSent = nowMs();
    std::string partial;   /* a fragmented message */
    while (s_run) {
        if (nowMs() - lastSent >= keepAliveMs) {
            if (!sendFrame(w, 1, "{\"MessageType\":\"KeepAlive\"}")) return;
            lastSent = nowMs();
        }
        if (!readable(w, 500)) continue;

        unsigned char h[2];
        if (!recvExact(w, h, 2)) return;
        int opcode = h[0] & 0x0F;
        bool fin = h[0] & 0x80, masked = h[1] & 0x80;
        unsigned long long n = h[1] & 0x7F;
        if (n == 126) {
            unsigned char e[2];
            if (!recvExact(w, e, 2)) return;
            n = (unsigned)e[0] << 8 | e[1];
        } else if (n == 127) {
            unsigned char e[8];
            if (!recvExact(w, e, 8)) return;
            n = 0;
            for (int i = 0; i < 8; ++i) n = n << 8 | e[i];
        }
        unsigned char mask[4] = { 0, 0, 0, 0 };
        if (masked && !recvExact(w, mask, 4)) return;
        if (n > 256 * 1024) return;   /* not a message of ours: start again */
        std::string payload((size_t)n, '\0');
        if (n && !recvExact(w, (unsigned char*)&payload[0], (int)n)) return;
        if (masked) for (size_t i = 0; i < payload.size(); ++i) payload[i] ^= mask[i & 3];

        if (opcode == 8) { sendFrame(w, 8, ""); return; }          /* close */
        if (opcode == 9) { if (!sendFrame(w, 10, payload)) return; continue; }   /* ping */
        if (opcode == 10) continue;                                 /* pong */
        if (opcode == 1 || opcode == 0) {
            partial += payload;
            if (fin) { handle(partial, keepAliveMs); partial.clear(); }
        }
    }
    sendFrame(w, 8, "");
}

void* threadMain(void*) {
    int backoff = 2;
    std::string lastErr;
    while (s_run) {
        Ws w;
        std::string err;
        if (!wsOpen(w, err)) {
            if (err != lastErr) SYS_Report("[Remote] cannot connect: %s (again in %d s)\n", err.c_str(), backoff);
            lastErr = err;
            nap(backoff * 1000);
            backoff = backoff * 2 > 30 ? 30 : backoff * 2;
            continue;
        }
        SYS_Report("[Remote] connected\n");
        lastErr.clear();
        backoff = 2;
        s_connected = true;
        session(w);
        s_connected = false;
        wsClose(w);
        if (s_run) { SYS_Report("[Remote] connection lost\n"); nap(1000); }
    }
    return nullptr;
}

} // namespace

/* ---- API ----------------------------------------------------------------- */

void Remote::start(const std::string& serverUrl, const std::string& token, bool verifyTls) {
    stop();
    if (s_lock == LWP_MUTEX_NULL) LWP_MutexInit(&s_lock, false);
    /* http(s)://host[:port][/path] */
    size_t p = serverUrl.find("://");
    if (p == std::string::npos) return;
    s_https = serverUrl.compare(0, p, "https") == 0;
    std::string rest = serverUrl.substr(p + 3);
    size_t slash = rest.find('/');
    std::string hostPort = rest.substr(0, slash);
    s_path = slash == std::string::npos ? "" : rest.substr(slash);
    while (!s_path.empty() && s_path.back() == '/') s_path.pop_back();
    size_t colon = hostPort.rfind(':');
    s_host = hostPort.substr(0, colon);
    s_port = colon == std::string::npos ? (s_https ? 443 : 80) : atoi(hostPort.c_str() + colon + 1);
    s_token  = token;
    s_verify = verifyTls;
    s_rand   = (u32)gettime();
    s_run    = true;
    LWP_CreateThread(&s_thread, threadMain, nullptr, s_stack, sizeof(s_stack), 30);
}

void Remote::stop() {
    if (s_thread == LWP_THREAD_NULL) return;
    s_run = false;
    LWP_JoinThread(s_thread, nullptr);
    s_thread = LWP_THREAD_NULL;
    s_connected = false;
    Lock l;
    s_queue.clear();
    s_messages.clear();
    s_playAt = 0;
}

bool Remote::connected() { return s_connected; }

/* a request still waiting (30 s at most) */
static bool pending() {
    if (s_playAt && nowMs() - s_playAt > 30000) { s_playAt = 0; SYS_Report("[Remote] play: expired\n"); }
    return s_playAt != 0;
}

/* to play now: PlayNow, or a queue request with nothing playing */
bool Remote::hasPlay() {
    if (s_lock == LWP_MUTEX_NULL) return false;
    Lock l;
    return pending() && (!s_play.queue || !s_playerActive);
}

bool Remote::takePlay(PlayRequest& r) {
    if (s_lock == LWP_MUTEX_NULL) return false;
    Lock l;
    if (!pending() || (s_play.queue && s_playerActive)) return false;
    r = s_play;
    s_playAt = 0;
    return true;
}

bool Remote::takeQueue(PlayRequest& r) {
    if (s_lock == LWP_MUTEX_NULL) return false;
    Lock l;
    if (!pending() || !s_play.queue || !s_playerActive) return false;
    r = s_play;
    s_playAt = 0;
    return true;
}

void Remote::setPlayerActive(bool on) {
    s_playerActive = on;
    if (s_lock == LWP_MUTEX_NULL) return;
    Lock l;
    s_queue.clear();   /* nothing left over from before */
}

bool Remote::poll(Command& c) {
    if (s_lock == LWP_MUTEX_NULL) return false;
    Lock l;
    if (s_queue.empty()) return false;
    c = s_queue.front();
    s_queue.pop_front();
    SYS_Report("[Remote] %s\n", name(c.cmd));
    return true;
}

const char* Remote::name(Cmd c) {
    switch (c) {
    case Cmd::Pause:         return "pause";
    case Cmd::Unpause:       return "unpause";
    case Cmd::PlayPause:     return "play/pause";
    case Cmd::Stop:          return "stop";
    case Cmd::Seek:          return "seek";
    case Cmd::Next:          return "next";
    case Cmd::Prev:          return "previous";
    case Cmd::FastForward:   return "fast forward";
    case Cmd::Rewind:        return "rewind";
    case Cmd::VolumeUp:      return "volume up";
    case Cmd::VolumeDown:    return "volume down";
    case Cmd::SetVolume:     return "set volume";
    case Cmd::Mute:          return "mute";
    case Cmd::Unmute:        return "unmute";
    case Cmd::ToggleMute:    return "toggle mute";
    case Cmd::AudioTrack:    return "audio track";
    case Cmd::SubtitleTrack: return "subtitle track";
    default:                 return "none";
    }
}

void Remote::pump() {
    if (s_lock == LWP_MUTEX_NULL) return;
    Message m;
    bool have = false;
    {
        Lock l;
        if (!s_playerActive && !s_queue.empty()) {
            SYS_Report("[Remote] %s: nothing playing\n", name(s_queue.front().cmd));
            s_queue.clear();
        }
        if (!s_messages.empty()) { m = s_messages.front(); s_messages.pop_front(); have = true; }
    }
    if (have) Ui::showNotice(m.header, m.text, m.ms);
}
