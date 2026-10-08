#include "Log.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <vector>
#include <ogc/mutex.h>
#include <ogc/lwp.h>
#include <ogc/lwp_watchdog.h>

extern "C" void __real_SYS_Report(char const* const fmt, ...);

static mutex_t s_lock  = LWP_MUTEX_NULL;
static FILE*   s_file  = nullptr;
static size_t  s_bytes = 0;
static unsigned long long s_t0 = 0;               /* ms, at init */
static const size_t MAX_BYTES = 1024 * 1024;      /* keep SD writes bounded */

/* lines reported before the SD card is known */
static char   s_early[8 * 1024];
static size_t s_earlyLen = 0;

/* Server names registered with addPrivate(); the log is meant to be posted
 * publicly, so it must not say where the server is. */
static std::vector<std::string> s_private;

/* Hide what a line may carry about the user: credentials, user and device
 * ids (in
 * stream URLs and request bodies), IPv4 addresses (server, Wii, router) and
 * the server's name. */
static std::string mask(const char* in)
{
    std::string s(in);
    for (const std::string& name : s_private)
        for (size_t p = s.find(name); p != std::string::npos; p = s.find(name, p + 8))
            s.replace(p, name.size(), "<server>");

    static const char* const keys[] = {
        "ApiKey=", "api_key=", "Token=\"", "Pw\":\"", "UserId=", "UserId\":\"", "Users/",
        "DeviceId=",
    };
    for (const char* k : keys) {
        size_t kl = strlen(k);
        for (size_t p = s.find(k); p != std::string::npos; p = s.find(k, p)) {
            p += kl;
            size_t e = s.find_first_of("&\"/ ?\n", p);
            if (e == std::string::npos) e = s.size();
            if (e - p > 3) { s.replace(p, e - p, "***"); p += 3; }
            else p = e;
        }
    }

    /* dotted quads: d.d.d.d with 1-3 digits each */
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ) {
        size_t j = i;
        int parts = 0;
        bool prevDigit = i > 0 && ((s[i - 1] >= '0' && s[i - 1] <= '9') || s[i - 1] == '.');
        while (!prevDigit && parts < 4) {
            size_t d = j;
            while (d < s.size() && d - j < 4 && s[d] >= '0' && s[d] <= '9') ++d;
            if (d == j || d - j > 3) break;
            ++parts;
            j = d;
            if (parts < 4) { if (j < s.size() && s[j] == '.') ++j; else break; }
        }
        bool nextDigit = j < s.size() && s[j] >= '0' && s[j] <= '9';
        if (parts == 4 && !nextDigit) { out += "<ip>"; i = j; }
        else                          { out += s[i]; ++i; }
    }
    return out;
}

/* Lines waiting for the writer thread.  SD writes (and the fsync libfat
 * needs to update the file size) take milliseconds on a real Wii: done by
 * the caller, they stalled MPlayer's thread and broke the frame pacing. */
static std::string s_pending;
static lwp_t       s_writer = LWP_THREAD_NULL;
static volatile bool s_stop = false;

/* caller holds s_lock */
static void writeLine(const char* line)
{
    size_t n = strlen(line);
    if (s_file) {
        if (s_bytes + n > MAX_BYTES) return;
        s_pending += line;
        s_bytes += n;
    } else if (s_earlyLen + n < sizeof(s_early)) {
        memcpy(s_early + s_earlyLen, line, n);
        s_earlyLen += n;
    }
}

/* Takes what is pending and writes it to the card. */
static void flushPending()
{
    std::string chunk;
    LWP_MutexLock(s_lock);
    chunk.swap(s_pending);
    FILE* f = s_file;
    LWP_MutexUnlock(s_lock);
    if (!f || chunk.empty()) return;
    fwrite(chunk.data(), 1, chunk.size(), f);
    fflush(f);
    fsync(fileno(f));   /* libfat writes the file size on sync only */
}

/* Every 250 ms: a crash loses at most that much of the log. */
static void* writerMain(void*)
{
    while (!s_stop) {
        flushPending();
        usleep(250 * 1000);
    }
    flushPending();
    return nullptr;
}

/* MPlayer's status line ("A: 12.3 V: 12.3 A-V: ...") comes with every frame:
 * one every 5 s is enough to see the sync and the dropped-frame counters. */
static bool keepLine(const char* buf, unsigned long long ms)
{
    const char* p = buf;
    while (*p == ' ' || *p == '\r' || *p == '\n') ++p;
    if (p[0] != 'A' || p[1] != ':') return true;
    static unsigned long long lastStatus = 0;
    if (lastStatus && ms - lastStatus < 5000) return false;
    lastStatus = ms;
    return true;
}

extern "C" void __wrap_SYS_Report(char const* const fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    __real_SYS_Report("%s", buf);

    if (s_lock == LWP_MUTEX_NULL) return;
    unsigned long long ms = ticks_to_millisecs(gettime()) - s_t0;
    if (!keepLine(buf, ms)) return;
    char stamp[24];
    snprintf(stamp, sizeof(stamp), "%6llu.%03llu ", ms / 1000, ms % 1000);
    LWP_MutexLock(s_lock);
    std::string line = stamp + mask(buf);
    writeLine(line.c_str());
    LWP_MutexUnlock(s_lock);
}

void Log::init()
{
    if (s_lock == LWP_MUTEX_NULL) LWP_MutexInit(&s_lock, false);
    s_t0 = ticks_to_millisecs(gettime());
}

void Log::addPrivate(const std::string& name)
{
    if (name.size() < 3 || s_lock == LWP_MUTEX_NULL) return;
    LWP_MutexLock(s_lock);
    bool known = false;
    for (const std::string& n : s_private) known = known || n == name;
    if (!known) s_private.push_back(name);
    LWP_MutexUnlock(s_lock);
}

void Log::addPrivateUrl(const std::string& url)
{
    size_t hs = url.find("://");
    hs = hs == std::string::npos ? 0 : hs + 3;
    size_t he = url.find_first_of(":/?", hs);
    addPrivate(url.substr(hs, he == std::string::npos ? std::string::npos : he - hs));
}

void Log::open(const std::string& dir)
{
    if (s_lock == LWP_MUTEX_NULL || s_file) return;
    std::string path = dir + "wiifin.log", prev = dir + "wiifin.prev.log";
    remove(prev.c_str());
    rename(path.c_str(), prev.c_str());
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return;
    LWP_MutexLock(s_lock);
    s_file = f;
    if (s_earlyLen) { s_pending.assign(s_early, s_earlyLen); s_bytes = s_earlyLen; }
    LWP_MutexUnlock(s_lock);
    static u8 stack[16 * 1024] __attribute__((aligned(32)));
    s_stop = false;
    /* lowest priority: it only runs while everything else waits */
    if (LWP_CreateThread(&s_writer, writerMain, nullptr, stack, sizeof(stack), 10) < 0)
        s_writer = LWP_THREAD_NULL;
}

void Log::close()
{
    if (s_writer != LWP_THREAD_NULL) {
        s_stop = true;
        LWP_JoinThread(s_writer, nullptr);
        s_writer = LWP_THREAD_NULL;
    } else {
        flushPending();
    }
    LWP_MutexLock(s_lock);
    FILE* f = s_file;
    s_file = nullptr;
    LWP_MutexUnlock(s_lock);
    if (f) fclose(f);
}
