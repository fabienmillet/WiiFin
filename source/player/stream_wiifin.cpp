/*
 * stream_wiifin.cpp — HTTP/HTTPS stream module for MPlayer CE.
 *
 * Defines `stream_info_http_wii` and `stream_info_https_wii`, the two entries
 * libmplayer's stream.o registers for http:// and https://, so the linker
 * never pulls the archive's own stream_http_wii.o / stream_https_wii.o.
 * Their chunked-transfer parser lost sync whenever a chunk-size line was
 * split across two network reads (typical on Wi-Fi or behind a reverse
 * proxy): the stream then looked finished after a few KB.  This module
 * decodes chunked bodies with a byte-wise state machine, reports the end of
 * the stream properly, and gives up quickly when MPlayer is asked to quit.
 */

#include "stream_wiifin.h"
#include "../core/Log.h"
#include "../core/NetConnect.h"
#include <ogc/lwp.h>

#include <network.h>
#include <ogc/lwp_watchdog.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stddef.h>
#include <string>

#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/x509_crt.h>

/* Shared with JellyfinClient.cpp: parsed CA bundle and the certificate check
 * that tolerates the Wii's unreliable clock and self-signed servers. */
mbedtls_x509_crt* wiifin_ca_chain();
int wiifin_cert_verify(void*, mbedtls_x509_crt*, int, uint32_t* flags);

extern "C" volatile int async_quit_request;   /* MPlayer input.c */
extern "C" float cache_fill_status;           /* MPlayer cache2.c, % ahead */

bool g_wiifin_stream_tls_verify = true;
volatile unsigned long long g_wiifin_stream_bytes = 0;

/* ---- MPlayer ABI — must match libmplayer.a (stream/stream.h) -------------
 * Offsets verified against stream_https_wii.o: fill_buffer @0, seek @8,
 * close @16, fd @20, type @24, end_pos (64-bit off_t) @64, priv @92. */
namespace mpabi {

struct mp_stream {
    int  (*fill_buffer)(mp_stream* s, char* buffer, int max_len);
    int  (*write_buffer)(mp_stream* s, char* buffer, int len);
    int  (*seek)(mp_stream* s, long long pos);
    int  (*control)(mp_stream* s, int cmd, void* arg);
    void (*close)(mp_stream* s);
    int  fd;
    int  type;
    int  flags;
    int  sector_size;
    int  read_chunk;
    unsigned int buf_pos, buf_len;
    long long pos, start_pos, end_pos;
    int  eof;
    int  error;
    int  mode;
    unsigned int cache_pid;
    void* cache_data;
    void* priv;
    char* url;
    /* ... buffer and other fields are not used here */
};
static_assert(offsetof(mp_stream, close)   == 16, "stream_t layout");
static_assert(offsetof(mp_stream, type)    == 24, "stream_t layout");
static_assert(offsetof(mp_stream, end_pos) == 64, "stream_t layout");
static_assert(offsetof(mp_stream, priv)    == 92, "stream_t layout");

struct mp_stream_info {
    const char* info;
    const char* name;
    const char* author;
    const char* comment;
    int (*open)(mp_stream* st, int mode, void* opts, int* file_format);
    const char* protocols[10];
    const void* opts;
    int opts_url;
};
static_assert(sizeof(mp_stream_info) == 68, "stream_info_t layout");

} // namespace mpabi

using mpabi::mp_stream;
using mpabi::mp_stream_info;

namespace {

const int STREAMTYPE_FILE    = 0;
const int STREAMTYPE_STREAM  = 2;
const int STREAM_READ        = 0;
const int STREAM_UNSUPPORTED = -1;
const int STREAM_ERROR       = 0;
const int STREAM_OK          = 1;

/* ---- Connection ----------------------------------------------------------- */

const int READ_TIMEOUT_S = 30;   /* no data at all for this long: give up */

struct Conn {
    s32  sock = -1;
    bool tls  = false;
    bool tlsReady = false;
    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_ssl_config       conf;
    mbedtls_ssl_context      ssl;
};

/* Wait until sock is readable/writable; false on timeout, error or quit. */
bool waitSock(s32 sock, bool forWrite, int timeoutS) {
    for (int i = 0; i < timeoutS; ++i) {
        if (async_quit_request) return false;
        fd_set fds; FD_ZERO(&fds); FD_SET(sock, &fds);
        struct timeval tv = {1, 0};
        int r = forWrite ? net_select(sock + 1, nullptr, &fds, nullptr, &tv)
                         : net_select(sock + 1, &fds, nullptr, nullptr, &tv);
        if (r > 0) return true;
        if (r < 0) return false;
    }
    return false;
}

int bioSend(void* ctx, const unsigned char* buf, size_t len) {
    s32 sock = *(s32*)ctx;
    int r = net_write(sock, (void*)buf, (u32)len);
    if (r == -EAGAIN || r == -EWOULDBLOCK) {
        if (!waitSock(sock, true, READ_TIMEOUT_S)) return MBEDTLS_ERR_NET_SEND_FAILED;
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    return r < 0 ? MBEDTLS_ERR_NET_SEND_FAILED : r;
}

int bioRecv(void* ctx, unsigned char* buf, size_t len) {
    s32 sock = *(s32*)ctx;
    if (!waitSock(sock, false, READ_TIMEOUT_S)) return MBEDTLS_ERR_NET_RECV_FAILED;
    int r = net_read(sock, buf, (u32)len);
    if (r == -EAGAIN || r == -EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_READ;
    return r < 0 ? MBEDTLS_ERR_NET_RECV_FAILED : r;
}

void connClose(Conn& c) {
    if (c.tlsReady) {
        mbedtls_ssl_free(&c.ssl);
        mbedtls_ssl_config_free(&c.conf);
        mbedtls_ctr_drbg_free(&c.drbg);
        mbedtls_entropy_free(&c.entropy);
        c.tlsReady = false;
    }
    if (c.sock >= 0) net_close(c.sock);
    c.sock = -1;
}

bool connOpen(Conn& c, bool tls, const std::string& host, int port) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_aton(host.c_str(), &addr.sin_addr) == 0) {
        struct hostent* he = net_gethostbyname(host.c_str());
        if (!he || he->h_length > (int)sizeof(addr.sin_addr)) return false;
        memcpy(&addr.sin_addr, he->h_addr, he->h_length);
    }
    c.sock = net_socket(AF_INET, SOCK_STREAM, 0);
    if (c.sock < 0) return false;
    if (connectWithTimeout(c.sock, &addr, 10, (volatile int*)&async_quit_request) < 0) {
        connClose(c);
        return false;
    }

    c.tls = tls;
    if (!tls) return true;

    mbedtls_entropy_init(&c.entropy);
    mbedtls_ctr_drbg_init(&c.drbg);
    mbedtls_ssl_config_init(&c.conf);
    mbedtls_ssl_init(&c.ssl);
    c.tlsReady = true;
    const char* pers = "wiifin_stream";
    int ret = mbedtls_ctr_drbg_seed(&c.drbg, mbedtls_entropy_func, &c.entropy,
                                    (const unsigned char*)pers, strlen(pers));
    if (ret == 0)
        ret = mbedtls_ssl_config_defaults(&c.conf, MBEDTLS_SSL_IS_CLIENT,
                                          MBEDTLS_SSL_TRANSPORT_STREAM,
                                          MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret == 0) {
        mbedtls_x509_crt* ca = g_wiifin_stream_tls_verify ? wiifin_ca_chain() : nullptr;
        if (ca) {
            mbedtls_ssl_conf_ca_chain(&c.conf, ca, nullptr);
            mbedtls_ssl_conf_authmode(&c.conf, MBEDTLS_SSL_VERIFY_REQUIRED);
            mbedtls_ssl_conf_verify(&c.conf, wiifin_cert_verify, nullptr);
        } else {
            mbedtls_ssl_conf_authmode(&c.conf, MBEDTLS_SSL_VERIFY_NONE);
        }
        mbedtls_ssl_conf_rng(&c.conf, mbedtls_ctr_drbg_random, &c.drbg);
        ret = mbedtls_ssl_setup(&c.ssl, &c.conf);
    }
    if (ret == 0) {
        mbedtls_ssl_set_hostname(&c.ssl, host.c_str());
        mbedtls_ssl_set_bio(&c.ssl, &c.sock, bioSend, bioRecv, nullptr);
        do {
            ret = mbedtls_ssl_handshake(&c.ssl);
        } while (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE);
    }
    if (ret != 0) {
        SYS_Report("[stream] TLS handshake failed -0x%04X\n", (unsigned)-ret);
        connClose(c);
        return false;
    }
    return true;
}

bool connWrite(Conn& c, const char* p, int n) {
    while (n > 0) {
        int w = c.tls ? mbedtls_ssl_write(&c.ssl, (const unsigned char*)p, (size_t)n)
                      : bioSend(&c.sock, (const unsigned char*)p, (size_t)n);
        if (w == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (w <= 0) return false;
        p += w; n -= w;
    }
    return true;
}

/* > 0 bytes, 0 = connection closed / timeout / quit / error */
int connRead(Conn& c, char* buf, int n) {
    for (;;) {
        int r = c.tls ? mbedtls_ssl_read(&c.ssl, (unsigned char*)buf, (size_t)n)
                      : bioRecv(&c.sock, (unsigned char*)buf, (size_t)n);
        if (r == MBEDTLS_ERR_SSL_WANT_READ) continue;
        return r > 0 ? r : 0;
    }
}

/* ---- Stream state -------------------------------------------------------- */

enum class Chunk { Size, SizeExt, SizeLF, Data, DataCR, DataLF, Trailer, TrailerLF, Done };

struct HttpStream {
    Conn  conn;
    bool  chunked   = false;
    long long remaining = -1;      /* Content-Length body bytes left, -1 = unknown */
    bool  done      = false;
    /* raw bytes received but not consumed yet */
    char  raw[16384];
    int   rawPos = 0, rawLen = 0;
    /* chunked decoder */
    Chunk state     = Chunk::Size;
    unsigned long chunkLeft = 0;
    bool  sawDigit  = false;
    bool  trailerEmpty = true;
    int   prio = 70;               /* cache thread priority, see pacePrefetch */
    lwp_t opener = LWP_THREAD_NULL; /* MPlayer's own thread: never re-prioritised */
    /* a whole file (direct play): reopened at any byte with a Range request */
    std::string url;               /* after redirects */
    long long total = -1;          /* file size, -1 = a live stream */
    int   resumes   = 0;           /* reconnections since data last came */
};

bool parseUrl(const std::string& url, bool& tls, std::string& host, int& port, std::string& path) {
    size_t p;
    if      (url.compare(0, 8, "https://") == 0) { tls = true;  p = 8; }
    else if (url.compare(0, 7, "http://")  == 0) { tls = false; p = 7; }
    else return false;
    size_t slash = url.find('/', p);
    std::string hostPort = url.substr(p, slash == std::string::npos ? std::string::npos : slash - p);
    path = slash == std::string::npos ? "/" : url.substr(slash);
    size_t colon = hostPort.rfind(':');
    if (colon != std::string::npos) {
        host = hostPort.substr(0, colon);
        port = atoi(hostPort.c_str() + colon + 1);
    } else {
        host = hostPort;
        port = tls ? 443 : 80;
    }
    return !host.empty() && port > 0;
}

std::string headerValue(const std::string& headers, const char* name) {
    std::string lower = headers;
    for (char& ch : lower) if (ch >= 'A' && ch <= 'Z') ch += 32;
    std::string key = std::string("\n") + name + ":";
    size_t p = lower.find(key);
    if (p == std::string::npos) return "";
    p += key.size();
    while (p < headers.size() && headers[p] == ' ') ++p;
    size_t e = headers.find("\r\n", p);
    return headers.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

/* Send the GET and read the response headers.  Returns the HTTP status (0 on
 * network error); leftover body bytes stay in st->raw. */
int request(HttpStream* st, const std::string& host, int port, bool tls,
            const std::string& path, std::string& headers, long long from = -1) {
    Log::addPrivate(host);
    std::string hostHdr = host + (port == (tls ? 443 : 80) ? "" : ":" + std::to_string(port));
    std::string req = "GET " + path + " HTTP/1.1\r\n"
                      "Host: " + hostHdr + "\r\n"
                      "User-Agent: WiiFin\r\n"
                      "Accept: */*\r\n" +
                      (from >= 0 ? "Range: bytes=" + std::to_string(from) + "-\r\n" : std::string()) +
                      "Connection: close\r\n\r\n";
    if (!connOpen(st->conn, tls, host, port)) return 0;
    if (!connWrite(st->conn, req.data(), (int)req.size())) return 0;

    st->rawPos = st->rawLen = 0;
    headers.clear();
    for (;;) {
        if (st->rawLen >= (int)sizeof(st->raw)) return 0;   /* headers too large */
        int n = connRead(st->conn, st->raw + st->rawLen, (int)sizeof(st->raw) - st->rawLen);
        if (n <= 0) return 0;
        st->rawLen += n;
        std::string seen(st->raw, (size_t)st->rawLen);
        size_t end = seen.find("\r\n\r\n");
        if (end == std::string::npos) continue;
        headers = seen.substr(0, end + 2);
        st->rawPos = (int)end + 4;
        break;
    }
    int status = 0;
    if (headers.compare(0, 5, "HTTP/") == 0) {
        size_t sp = headers.find(' ');
        if (sp != std::string::npos) status = atoi(headers.c_str() + sp + 1);
    }
    return status;
}

/* Decode buffered raw bytes into out (chunked or plain).  Returns bytes
 * produced; sets st->done when the body is complete. */
int decode(HttpStream* st, char* out, int max) {
    int produced = 0;
    if (!st->chunked) {
        int n = st->rawLen - st->rawPos;
        if (n > max) n = max;
        if (st->remaining >= 0 && n > st->remaining) n = (int)st->remaining;
        memcpy(out, st->raw + st->rawPos, (size_t)n);
        st->rawPos += n;
        if (st->remaining >= 0) {
            st->remaining -= n;
            if (st->remaining == 0) st->done = true;
        }
        return n;
    }
    while (st->rawPos < st->rawLen && produced < max && !st->done) {
        char ch = st->raw[st->rawPos];
        switch (st->state) {
        case Chunk::Size: {
            int d = (ch >= '0' && ch <= '9') ? ch - '0'
                  : (ch >= 'a' && ch <= 'f') ? ch - 'a' + 10
                  : (ch >= 'A' && ch <= 'F') ? ch - 'A' + 10 : -1;
            if (d >= 0) { st->chunkLeft = st->chunkLeft * 16 + d; st->sawDigit = true; }
            else if (ch == '\r') st->state = Chunk::SizeLF;
            else if (ch == ';' || ch == ' ' || ch == '\t') st->state = Chunk::SizeExt;
            /* tolerate stray LF / garbage before the digits */
            ++st->rawPos;
            break;
        }
        case Chunk::SizeExt:
            if (ch == '\r') st->state = Chunk::SizeLF;
            ++st->rawPos;
            break;
        case Chunk::SizeLF:
            ++st->rawPos;
            if (!st->sawDigit) { st->state = Chunk::Size; break; }   /* empty line */
            st->sawDigit = false;
            if (st->chunkLeft == 0) { st->state = Chunk::Trailer; st->trailerEmpty = true; }
            else                      st->state = Chunk::Data;
            break;
        case Chunk::Data: {
            int n = st->rawLen - st->rawPos;
            if ((unsigned long)n > st->chunkLeft) n = (int)st->chunkLeft;
            if (n > max - produced) n = max - produced;
            memcpy(out + produced, st->raw + st->rawPos, (size_t)n);
            produced += n;
            st->rawPos += n;
            st->chunkLeft -= (unsigned long)n;
            if (st->chunkLeft == 0) st->state = Chunk::DataCR;
            break;
        }
        case Chunk::DataCR:
            ++st->rawPos;
            st->state = (ch == '\r') ? Chunk::DataLF : Chunk::Size;
            if (ch != '\r' && ch != '\n') --st->rawPos;   /* missing CRLF: reparse */
            break;
        case Chunk::DataLF:
            ++st->rawPos;
            st->state = Chunk::Size;
            break;
        case Chunk::Trailer:
            ++st->rawPos;
            if (ch == '\r') st->state = Chunk::TrailerLF;
            else            st->trailerEmpty = false;
            break;
        case Chunk::TrailerLF:
            ++st->rawPos;
            if (st->trailerEmpty) { st->state = Chunk::Done; st->done = true; }
            else                  { st->state = Chunk::Trailer; st->trailerEmpty = true; }
            break;
        case Chunk::Done:
            st->done = true;
            break;
        }
    }
    return produced;
}

/* fillBuffer runs on MPlayer's cache thread, created at priority 70: above
 * the decoder (60) so prefetch is never starved.  But filling the 8 MB
 * cache at network speed, with TLS decryption, kept the CPU from the
 * decoder for seconds after every start: MPlayer then skipped frames
 * (109 in the first 6 s of an HTTPS stream).  So the thread only stays
 * above the decoder while the cache is short; once it is ahead, it fills
 * the cache with the time the decoder leaves. */
void pacePrefetch(HttpStream* st) {
    if (LWP_GetSelf() == st->opener) return;   /* a read before the cache started */
    float fill = cache_fill_status;           /* -1: whole stream received */
    int want = st->prio;
    if (fill >= 0.0f && fill < 8.0f)  want = 70;
    else if (fill < 0.0f || fill > 15.0f) want = 50;
    if (want != st->prio) {
        LWP_SetThreadPriority(LWP_GetSelf(), (u32)want);
        st->prio = want;
    }
}

int seekStream(mp_stream* s, long long pos);

int fillBuffer(mp_stream* s, char* buffer, int max_len) {
    HttpStream* st = (HttpStream*)s->priv;
    if (!st) return 0;
    pacePrefetch(st);
    while (!st->done) {
        int n = decode(st, buffer, max_len);
        if (n > 0) { g_wiifin_stream_bytes += (unsigned)n; st->resumes = 0; return n; }
        if (st->done) break;
        /* need more raw data */
        st->rawPos = st->rawLen = 0;
        n = connRead(st->conn, st->raw, (int)sizeof(st->raw));
        if (n <= 0) {
            /* closed or timed out before the end of a file (the server
             * dropping a connection read slowly, a seek's new request cut
             * short): MPlayer would take it for the end of the film, so go
             * on from that byte with a new request; not when quitting */
            if (st->total < 0 || st->chunked || st->remaining <= 0 || async_quit_request ||
                ++st->resumes > 3)
                break;
            const long long at = st->total - st->remaining;
            SYS_Report("[stream] cut at %lld of %lld bytes, going on from there\n", at, st->total);
            if (!seekStream(s, at)) break;
            continue;
        }
        st->rawLen = n;
    }
    return 0;
}

void closeStream(mp_stream* s) {
    HttpStream* st = (HttpStream*)s->priv;
    if (!st) return;
    connClose(st->conn);
    delete st;
    s->priv = nullptr;
}

/* Reads go on from byte pos: a new request for the rest of the file.  The
 * cache thread calls it, for a demuxer looking for an index (MP4 files
 * keep theirs at the end) or for a seek in the film. */
int seekStream(mp_stream* s, long long pos) {
    HttpStream* st = (HttpStream*)s->priv;
    if (!st || st->total < 0 || pos < 0 || pos > st->total) return 0;
    if (pos == st->total) {            /* the end: nothing left to read */
        connClose(st->conn);
        st->done = true;
        s->pos = pos;
        return 1;
    }
    connClose(st->conn);
    bool tls; std::string host, path; int port;
    if (!parseUrl(st->url, tls, host, port, path)) return 0;
    std::string headers;
    int status = 0;
    for (int attempt = 0; attempt < 3 && !async_quit_request; ++attempt) {
        status = request(st, host, port, tls, path, headers, pos);
        if (status == 206) break;
        connClose(st->conn);
        if (status != 0) break;        /* an answer other than a range: give up */
        usleep(300000);                /* a socket error: try again */
    }
    if (status != 206) {
        SYS_Report("[stream] seek to %lld failed (HTTP %d)\n", pos, status);
        return 0;
    }
    st->chunked   = false;
    st->remaining = st->total - pos;
    st->done      = false;
    s->pos        = pos;
    return 1;
}

int openStream(mp_stream* s, int mode, void* opts, int* file_format) {
    (void)opts; (void)file_format;
    if (mode != STREAM_READ) return STREAM_UNSUPPORTED;
    if (!s->url) return STREAM_ERROR;

    std::string url = s->url;
    HttpStream* st = new HttpStream;
    std::string headers;
    int status = 0;
    /* Follow redirects; while Jellyfin's transcoder spins up it may answer
     * 5xx for a few seconds, so retry those. */
    int errors500 = 0;
    for (int attempt = 0, redirects = 0; attempt < 8; ++attempt) {
        bool tls; std::string host, path; int port;
        if (!parseUrl(url, tls, host, port, path)) break;
        status = request(st, host, port, tls, path, headers);
        if (status == 0 && attempt < 3 && !async_quit_request) {
            /* transient IOS socket error (e.g. right after the previous session) */
            connClose(st->conn);
            usleep(500000);
            continue;
        }
        if (status >= 300 && status < 400 && redirects < 5) {
            std::string loc = headerValue(headers, "location");
            connClose(st->conn);
            if (loc.empty()) break;
            if (loc[0] == '/') loc = std::string(tls ? "https://" : "http://") + host + ":" +
                                     std::to_string(port) + loc;
            url = loc;
            ++redirects;
            continue;
        }
        /* 503/504 while the transcoder starts can last a while; a 500 is
         * usually FFmpeg failing on the server, which retrying won't fix */
        if (status >= 500 && !async_quit_request && (status != 500 || ++errors500 < 3)) {
            connClose(st->conn);
            SYS_Report("[stream] HTTP %d, retrying\n", status);
            usleep(2000000);
            continue;
        }
        break;
    }
    st->chunked = headerValue(headers, "transfer-encoding").find("chunked") != std::string::npos;
    std::string cl = headerValue(headers, "content-length");
    if (!st->chunked && !cl.empty()) st->remaining = atoll(cl.c_str());
    st->url = url;
    /* a file of known size that the server serves by byte ranges (Jellyfin's
     * static streams): seekable; a transcode is a live stream */
    if (status == 200 && st->remaining > 0 &&
        headerValue(headers, "accept-ranges").find("bytes") != std::string::npos)
        st->total = st->remaining;
    g_wiifin_stream_fail_status  = 0;
    g_wiifin_stream_fail_body[0] = 0;
    if (status != 200 && status != 206) {
        if (status > 0) {
            /* the server's explanation, for the log and the error screen */
            char body[160];
            int n = decode(st, body, (int)sizeof(body) - 1);
            if (n <= 0 && !st->done) {
                st->rawPos = st->rawLen = 0;
                int r = connRead(st->conn, st->raw, 1024);
                if (r > 0) { st->rawLen = r; n = decode(st, body, (int)sizeof(body) - 1); }
            }
            body[n > 0 ? n : 0] = 0;
            for (char* c = body; *c; ++c) if (*c == '\r' || *c == '\n') *c = ' ';
            g_wiifin_stream_fail_status = status;
            snprintf(g_wiifin_stream_fail_body, sizeof(g_wiifin_stream_fail_body), "%s", body);
        }
        SYS_Report("[stream] open failed (HTTP %d): %s\n", status, g_wiifin_stream_fail_body);
        connClose(st->conn);
        delete st;
        return STREAM_ERROR;
    }

    SYS_Report("[stream] HTTP %d, %s%s\n", status,
               st->chunked ? "chunked" : (st->remaining >= 0 ? "content-length" : "until close"),
               st->total >= 0 ? ", a file: seekable" : "");

    st->opener     = LWP_GetSelf();
    s->priv        = st;
    s->fill_buffer = fillBuffer;
    s->close       = closeStream;
    s->fd          = -1;
    if (st->total >= 0) {
        /* a file: MPlayer seeks through s->seek (MP_STREAM_SEEK follows) */
        s->seek    = seekStream;
        s->type    = STREAMTYPE_FILE;
        s->end_pos = st->total;
    } else {
        s->seek    = nullptr;
        s->type    = STREAMTYPE_STREAM;
        s->end_pos = 0;   /* unknown length: like the original module */
    }
    return STREAM_OK;
}

} // namespace

int  g_wiifin_stream_fail_status = 0;
char g_wiifin_stream_fail_body[160] = "";

extern "C" {

extern const mp_stream_info stream_info_http_wii;
extern const mp_stream_info stream_info_https_wii;

const mp_stream_info stream_info_http_wii = {
    "WiiFin HTTP stream", "http_wii", "WiiFin", "",
    openStream, { "http", nullptr }, nullptr, 0
};

const mp_stream_info stream_info_https_wii = {
    "WiiFin HTTPS stream", "https_wii", "WiiFin", "mbedTLS",
    openStream, { "https", nullptr }, nullptr, 0
};

}
