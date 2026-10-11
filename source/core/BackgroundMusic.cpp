#include "BackgroundMusic.h"
#include "MusicBGM.h"
#include "../player/stream_wiifin.h"
#include "../ui/Ui.h"

#include <mp3player.h>
#include <ogc/lwp.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/system.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern "C" volatile int async_quit_request;   /* MPlayer input.c: breaks off a read */

namespace {

/* the queue, as the music player left it */
JellyfinClient*         s_client = nullptr;   /* a copy: the main thread uses its own */
std::string             s_server;
JellyfinAuth            s_auth;
std::vector<MusicTrack> s_tracks;
std::vector<int>        s_order;
volatile int            s_pos    = 0;
int                     s_repeat = 0;
bool                    s_menuMusic = false;
float                   s_from   = 0.0f;      /* seconds into the first song */
volatile u64            s_started = 0;        /* when this song's sound began */
volatile float          s_base    = 0.0f;     /* where it began, in the song */

volatile bool s_stop    = false;
volatile bool s_running = false;
lwp_t         s_thread  = LWP_THREAD_NULL;
u8            s_stack[64 * 1024] __attribute__((aligned(32)));   /* Jellyfin's requests (TLS) */

/* the song's bytes: the download thread writes, MP3Player's reads */
const u32     RING = 512 * 1024;
u8*           s_ring = nullptr;
volatile u32  s_head = 0, s_tail = 0;         /* written, read (they only grow) */
volatile bool s_dlDone = false, s_dlStop = false;
std::string   s_url;
lwp_t         s_dl = LWP_THREAD_NULL;
u8            s_dlStack[64 * 1024] __attribute__((aligned(32)));   /* a TLS handshake needs room */

u64 nowMs() { return ticks_to_millisecs(gettime()); }

/* opens and reads in this thread (the stream's connection rules) */
void* download(void*)
{
    void* h = wiifin_http_open(s_url.c_str());
    if (!h) SYS_Report("[Background] the song did not open\n");
    char buf[8192];
    while (h && !s_dlStop) {
        while (!s_dlStop && s_head - s_tail > RING - sizeof(buf)) usleep(10000);   /* full */
        if (s_dlStop) break;
        int n = wiifin_http_read(h, buf, sizeof(buf));
        if (n <= 0) break;
        for (int i = 0; i < n; ++i) s_ring[(s_head + i) % RING] = (u8)buf[i];
        s_head += n;
    }
    wiifin_http_close(h);
    s_dlDone = true;
    return nullptr;
}

/* MP3Player's reader: what came, waiting for it in short steps (Stop joins
 * the thread that calls this) */
s32 readSong(void*, void* out, s32 len)
{
    for (;;) {
        u32 avail = s_head - s_tail;
        if (avail > 0) {
            u32 n = avail < (u32)len ? avail : (u32)len;
            for (u32 i = 0; i < n; ++i) ((u8*)out)[i] = s_ring[(s_tail + i) % RING];
            s_tail += n;
            return (s32)n;
        }
        if (s_dlDone || s_stop) return 0;
        usleep(5000);
    }
}

void stopDownload()
{
    if (s_dl == LWP_THREAD_NULL) return;
    s_dlStop = true;
    async_quit_request = 1;          /* a read waiting for the network */
    LWP_JoinThread(s_dl, nullptr);
    async_quit_request = 0;
    s_dl = LWP_THREAD_NULL;
}

float songSecs()
{
    return s_started ? s_base + (nowMs() - s_started) / 1000.0f : s_base;
}

void showSong()
{
    const MusicTrack& t = s_tracks[s_order[s_pos]];
    Ui::setNowPlaying(t.artist.empty() ? t.title : t.title + "  \xe2\x80\x94  " + t.artist);
}

void* runQueue(void*)
{
    JellyfinClient& c = *s_client;
    float from = s_from;
    int failures = 0;
    while (!s_stop && s_pos < (int)s_order.size()) {
        const MusicTrack t = s_tracks[s_order[s_pos]];
        std::string session, method;
        int index = 0;
        const long long fromTicks = (long long)(from * 10000000.0f);
        bool ok = c.getAudioStreamUrl(s_server, s_auth, t.id, fromTicks, s_url, session, &method, &index, true);
        if (ok) {
            s_head = s_tail = 0;
            s_dlDone = s_dlStop = false;
            async_quit_request = 0;
            LWP_CreateThread(&s_dl, download, nullptr, s_dlStack, sizeof(s_dlStack), 55);
            /* a little ahead before the sound starts */
            u64 t0 = nowMs();
            while (!s_stop && !s_dlDone && s_head < 64 * 1024 && nowMs() - t0 < 15000) usleep(20000);
            ok = s_head > 0;
        }
        if (!ok || s_stop) {
            stopDownload();
            if (s_stop) break;
            SYS_Report("[Background] %s did not play\n", t.title.c_str());
            if (++failures >= 3) break;
            s_pos = s_pos + 1;
            from = 0.0f;
            continue;
        }
        failures = 0;
        showSong();
        SYS_Report("[Background] playing %d of %d from %.0f s (%s)\n", s_pos + 1, (int)s_order.size(),
                   (double)from, method.c_str());
        c.reportPlaybackStart(s_server, s_auth, t.id, t.id, session, method.c_str(), index);
        s_base = from;
        s_started = nowMs();
        MP3Player_PlayFile(nullptr, readSong, nullptr);
        u64 lastReport = nowMs();
        while (!s_stop && MP3Player_IsPlaying()) {
            usleep(100000);
            if (nowMs() - lastReport > 10000) {
                lastReport = nowMs();
                c.reportPlaybackProgress(s_server, s_auth, t.id, t.id, session,
                                         (long long)(songSecs() * 10000000.0f), false, method.c_str(), index);
            }
        }
        const float at = songSecs();
        if (MP3Player_IsPlaying()) MP3Player_Stop();   /* the reader returns at once now */
        stopDownload();
        c.reportPlaybackStopped(s_server, s_auth, t.id, t.id, session, (long long)(at * 10000000.0f));
        if (!session.empty()) c.deleteActiveEncoding(s_server, s_auth, session);
        if (s_stop) { s_base = at; s_started = 0; break; }
        /* the next one: this one again, the queue from its start, or the end */
        from = 0.0f;
        s_started = 0;
        s_base = 0.0f;
        if (s_repeat == 2) continue;
        if (s_pos + 1 < (int)s_order.size()) s_pos = s_pos + 1;
        else if (s_repeat == 1) s_pos = 0;
        else break;
    }
    if (!s_stop) SYS_Report("[Background] the queue is over\n");
    Ui::setNowPlaying("");
    s_running = false;
    return nullptr;
}

} // namespace

void BackgroundMusic::start(const JellyfinClient& client, const std::string& serverUrl,
                            const JellyfinAuth& auth, const std::vector<MusicTrack>& tracks,
                            const std::vector<int>& order, int pos, float secs, int repeat,
                            bool menuMusic)
{
    stop(false);
    if (tracks.empty() || order.empty() || pos < 0 || pos >= (int)order.size()) return;
    if (!s_ring) s_ring = (u8*)malloc(RING);
    if (!s_ring) return;
    delete s_client;
    s_client = new JellyfinClient(client);
    s_server = serverUrl;
    s_auth   = auth;
    s_tracks = tracks;
    s_order  = order;
    s_pos    = pos;
    s_repeat = repeat;
    s_menuMusic = menuMusic;
    s_from   = secs;
    s_base   = secs;
    s_started = 0;
    /* the menu music makes room; the interface sounds stay */
    MusicBGM::stopMusic();
    MusicBGM::reinitAudio();
    s_stop = false;
    s_running = true;
    SYS_Report("[Background] the music goes on: %d song(s) left\n", (int)order.size() - pos);
    LWP_CreateThread(&s_thread, runQueue, nullptr, s_stack, sizeof(s_stack), 30);
}

bool BackgroundMusic::stop(bool resumeMenuMusic)
{
    if (s_thread == LWP_THREAD_NULL) return false;
    s_stop = true;
    LWP_JoinThread(s_thread, nullptr);
    s_thread = LWP_THREAD_NULL;
    s_running = false;
    Ui::setNowPlaying("");
    SYS_Report("[Background] stopped\n");
    if (resumeMenuMusic && s_menuMusic) MusicBGM::resume();
    return s_menuMusic;
}

void BackgroundMusic::pump()
{
    if (s_thread == LWP_THREAD_NULL || s_running) return;
    LWP_JoinThread(s_thread, nullptr);   /* the queue is over */
    s_thread = LWP_THREAD_NULL;
    if (s_menuMusic) MusicBGM::resume();
}

bool BackgroundMusic::playing()
{
    return s_running;
}

void BackgroundMusic::state(std::vector<MusicTrack>& tracks, std::vector<int>& order, int& pos,
                            float& secs, int& repeat)
{
    tracks = s_tracks;
    order  = s_order;
    pos    = s_pos < (int)s_order.size() ? s_pos : 0;
    secs   = songSecs();
    repeat = s_repeat;
}
