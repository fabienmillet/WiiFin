#include "ListFeed.h"
#include "JpegTexture.h"
#include "../core/ExitZone.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ogc/system.h>

ListFeed::ListFeed(JellyfinClient& c, const std::string& url, const JellyfinAuth& a)
    : client(c), serverUrl(url), auth(a)
{
    LWP_MutexInit(&lock, false);
}

ListFeed::~ListFeed()
{
    close();
    if (lock != LWP_MUTEX_NULL) LWP_MutexDestroy(lock);
}

std::string ListFeed::query(int start, int limit, const char* nameLessThan) const
{
    char q[640];
    /* a filter starting with '/' is a route of its own (/Artists/...) */
    std::string base = filter[0] == '/' ? filter : "/Users/" + auth.userId + "/Items?" + filter;
    snprintf(q, sizeof(q),
             "%s%s&Limit=%d&StartIndex=%d%s%s",
             base.c_str(),
             /* the filter's own sort, else by name (the letters need it) */
             filter.find("SortBy=") == std::string::npos ? "&SortBy=SortName&SortOrder=Ascending" : "",
             limit, start,
             nameLessThan ? "&NameLessThan=" : "", nameLessThan ? nameLessThan : "");
    return q;
}

bool ListFeed::open(const std::string& f, std::string& err)
{
    close();
    filter = f;
    std::vector<JellyfinItem> first;
    int total = 0;
    if (!client.getItemsByQuery(serverUrl, auth, query(0, CHUNK, nullptr), first, &total)) {
        err = client.lastError();
        return false;
    }
    if (total < (int)first.size()) total = (int)first.size();
    LWP_MutexLock(lock);
    count = total;
    items.assign(total, JellyfinItem());
    chunkState.assign((total + CHUNK - 1) / CHUNK, 0);
    for (size_t i = 0; i < first.size() && (int)i < total; ++i) items[i] = first[i];
    if (!chunkState.empty()) chunkState[0] = 2;
    LWP_MutexUnlock(lock);
    wantFirst = 0; wantLast = CHUNK - 1;
    jumpState = 0;
    opened = true;
    return true;
}

void ListFeed::close()
{
    stopWorker();
    LWP_MutexLock(lock);
    items.clear();
    chunkState.clear();
    count = 0;
    coverWanted.clear();
    coverLoadedId.clear();
    if (coverTex) { GRRLIB_FreeTexture(coverTex); coverTex = nullptr; }
    if (coverOld) { GRRLIB_FreeTexture(coverOld); coverOld = nullptr; }
    LWP_MutexUnlock(lock);
    opened = false;
}

bool ListFeed::get(int i, JellyfinItem& out)
{
    bool ok = false;
    LWP_MutexLock(lock);
    if (i >= 0 && i < count && chunkState[i / CHUNK] == 2) { out = items[i]; ok = true; }
    LWP_MutexUnlock(lock);
    return ok;
}

void ListFeed::want(int first, int last)
{
    wantFirst = first;
    wantLast  = last;
}

char ListFeed::letterOf(const JellyfinItem& it)
{
    const std::string& s = it.sortName.empty() ? it.name : it.sortName;
    char c = s.empty() ? '#' : s[0];
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    return (c >= 'A' && c <= 'Z') ? c : '#';
}

/* ---- jobs (worker thread) ------------------------------------------------ */

bool ListFeed::fetchChunk(int c)
{
    std::vector<JellyfinItem> got;
    bool ok = client.getItemsByQuery(serverUrl, auth, query(c * CHUNK, CHUNK, nullptr), got);
    LWP_MutexLock(lock);
    if (c < (int)chunkState.size()) {
        if (ok) {
            for (size_t i = 0; i < got.size(); ++i) {
                int idx = c * CHUNK + (int)i;
                if (idx < count) items[idx] = got[i];
            }
            chunkState[c] = 2;
        } else {
            chunkState[c] = 0;          /* retried later */
        }
    }
    LWP_MutexUnlock(lock);
    return ok;
}

bool ListFeed::fetchRange(int start, int limit, std::vector<JellyfinItem>& out)
{
    out.clear();
    return client.getItemsByQuery(serverUrl, auth, query(start, limit, nullptr), out);
}

int ListFeed::countBefore(char letter)
{
    if (letter == '#') return 0;                    /* digits/symbols sort first */
    char q[2] = { (char)(letter - 'A' + 'a'), 0 };  /* sort names are lower case */
    /* Limit=1: Jellyfin takes 0 for "no limit" and sends every title before
     * the letter, which overflowed the response buffer from a few hundred on
     * (the count, at the end, was lost and the jump went nowhere) */
    std::vector<JellyfinItem> one;
    int total = -1;
    if (!client.getItemsByQuery(serverUrl, auth, query(0, 1, q), one, &total)) return -1;
    return total;
}

void ListFeed::runJump()
{
    int from = jumpFrom, dir = jumpDir, result = -1;
    JellyfinItem cur;
    bool have = get(from, cur);
    char letter = have ? letterOf(cur) : '#';

    if (dir > 0) {
        /* first title of the next letter (or of the next non-empty one) */
        if (letter != 'Z') {
            int idx = countBefore(letter == '#' ? 'A' : (char)(letter + 1));
            if (idx >= 0 && idx < count && idx > from) result = idx;
        }
    } else {
        int start = countBefore(letter);
        if (start >= 0 && start < from) {
            result = start;                          /* start of this letter */
        } else if (start > 0) {
            /* already there: start of the previous non-empty letter */
            JellyfinItem prev;
            char l = get(start - 1, prev) ? letterOf(prev) : (char)(letter - 1);
            for (; result < 0; --l) {
                int idx = (l < 'A') ? 0 : countBefore(l);
                if (idx >= 0 && idx < start) result = idx;
                if (l < 'A') break;
            }
        }
    }
    SYS_Report("[ListFeed] jump dir=%d from=%d letter=%c -> %d\n", dir, from, letter, result);
    jumpResult = result;
    jumpState  = 2;
}

void ListFeed::loop()
{
    while (!stopReq) {
        if (jumpState == 1) { runJump(); continue; }

        /* chunks around what is on screen */
        int first = wantFirst, last = wantLast;
        int c0 = first / CHUNK, c1 = last / CHUNK, pick = -1;
        LWP_MutexLock(lock);
        int nChunks = (int)chunkState.size();
        for (int c = c0; c <= c1 + 1 && pick < 0; ++c)      /* visible, then the next one */
            if (c >= 0 && c < nChunks && chunkState[c] == 0) pick = c;
        if (pick >= 0) chunkState[pick] = 1;
        std::string wantCover = coverWanted;
        bool coverNeeded = !wantCover.empty() && wantCover != coverLoadedId;
        LWP_MutexUnlock(lock);
        if (pick >= 0) {
            if (!fetchChunk(pick)) usleep(200 * 1000);   /* network trouble: don't spin */
            continue;
        }

        if (coverNeeded) {
            std::string bytes;
            client.getItemImageBytes(serverUrl, auth, wantCover, 200, 300, bytes);
            GRRLIB_texImg* t = bytes.empty() ? nullptr
                             : loadJPEGTexture((const u8*)bytes.data(), (u32)bytes.size());
            LWP_MutexLock(lock);
            if (coverOld) {
                /* the main thread has not freed the previous one yet: retry */
                if (t) GRRLIB_FreeTexture(t);
            } else {
                coverOld      = coverTex;     /* the main thread frees it between frames */
                coverTex      = t;            /* may be null: title without artwork */
                coverLoadedId = wantCover;
            }
            LWP_MutexUnlock(lock);
            continue;
        }
        usleep(20 * 1000);
    }
}

void* ListFeed::threadMain(void* self)
{
    static_cast<ListFeed*>(self)->loop();
    return nullptr;
}

void ListFeed::startWorker()
{
    if (thread != LWP_THREAD_NULL || !opened) return;
    static u8 stack[64 * 1024] DEAD_AT_EXIT __attribute__((aligned(32)));
    stopReq = false;
    /* below the main thread: it runs while the UI waits for vsync */
    if (LWP_CreateThread(&thread, threadMain, this, stack, sizeof(stack), 40) < 0)
        thread = LWP_THREAD_NULL;
}

void ListFeed::stopWorker()
{
    if (thread == LWP_THREAD_NULL) return;
    stopReq = true;
    LWP_JoinThread(thread, nullptr);
    thread = LWP_THREAD_NULL;
}

/* ---- main-thread API ------------------------------------------------------ */

void ListFeed::requestJump(int dir, int fromIndex)
{
    if (jumpState != 0) return;
    jumpDir = dir; jumpFrom = fromIndex; jumpResult = -1;
    jumpState = 1;
}

bool ListFeed::takeJump(int& index)
{
    if (jumpState != 2) return false;
    index = jumpResult;
    jumpState = 0;
    return true;
}

void ListFeed::requestCover(const std::string& itemId)
{
    LWP_MutexLock(lock);
    coverWanted = itemId;
    LWP_MutexUnlock(lock);
}

GRRLIB_texImg* ListFeed::cover(const std::string& itemId)
{
    LWP_MutexLock(lock);
    /* the previous cover is no longer drawn: the GPU finished with it */
    if (coverOld) { GRRLIB_FreeTexture(coverOld); coverOld = nullptr; }
    GRRLIB_texImg* t = (coverLoadedId == itemId) ? coverTex : nullptr;
    LWP_MutexUnlock(lock);
    return t;
}
