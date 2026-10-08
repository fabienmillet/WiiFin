#pragma once
#include <grrlib.h>
#include <ogc/lwp.h>
#include <ogc/mutex.h>
#include <string>
#include <vector>
#include "../jellyfin/JellyfinClient.h"

/* -----------------------------------------------------------------------
 * ListFeed — the whole content of a library for the text list view, sorted
 * by name, without pages.
 *
 * open() fetches the first chunk and the total (blocking, run it behind a
 * loading screen).  The rest is fetched by a background thread in chunks of
 * CHUNK titles as the list scrolls (want()), so scrolling never waits on the
 * network.  It also answers letter jumps (Jellyfin's NameLessThan counts how
 * many titles sort before a letter, which is exactly that letter's index)
 * and loads the cover of the selected title when asked.
 *
 * Like BrowseHome, the thread uses the shared JellyfinClient connection:
 * stopWorker() before anything else makes a request.
 * ----------------------------------------------------------------------- */
class ListFeed {
public:
    static const int CHUNK = 50;

    ListFeed(JellyfinClient& client, const std::string& serverUrl, const JellyfinAuth& auth);
    ~ListFeed();

    /* filter: the query part selecting the titles, e.g. "ParentId=<id>",
     * with a sort of its own maybe ("&SortBy=...": then no letter jumps);
     * or a whole route and query of its own ("/Artists/AlbumArtists?...") */
    bool open(const std::string& filter, std::string& err);
    void close();
    bool isOpen() const { return opened; }

    void startWorker();
    void stopWorker();

    int  total() const { return count; }
    bool get(int i, JellyfinItem& out);       /* false while that chunk loads */
    void want(int first, int last);           /* rows on screen (plus margin) */
    /* Titles start .. start + limit - 1, fetched now (blocking). */
    bool fetchRange(int start, int limit, std::vector<JellyfinItem>& out);

    /* Letter jumps: dir +1 next letter, -1 previous (or the start of the
     * current letter).  Result arrives asynchronously. */
    void requestJump(int dir, int fromIndex);
    bool takeJump(int& index);
    bool jumpPending() const { return jumpState != 0; }

    /* Cover of one title (List + Cover style). */
    void requestCover(const std::string& itemId);
    GRRLIB_texImg* cover(const std::string& itemId);   /* null until loaded */

    /* '#' for digits/symbols, else 'A'..'Z', from the sort name */
    static char letterOf(const JellyfinItem& it);

private:
    JellyfinClient& client;
    std::string     serverUrl;
    JellyfinAuth    auth;
    std::string     filter;
    bool            opened = false;
    int             count  = 0;

    std::vector<JellyfinItem>  items;
    std::vector<unsigned char> chunkState;    /* 0 none, 1 loading, 2 loaded */
    volatile int    wantFirst = 0, wantLast = 0;

    /* jump job */
    volatile int    jumpState = 0;            /* 0 idle, 1 requested, 2 done */
    int             jumpDir = 0, jumpFrom = 0, jumpResult = -1;

    /* cover job */
    std::string     coverWanted, coverLoadedId;
    GRRLIB_texImg*  coverTex = nullptr;
    GRRLIB_texImg*  coverOld = nullptr;       /* freed by the main thread */

    lwp_t           thread = LWP_THREAD_NULL;
    mutex_t         lock   = LWP_MUTEX_NULL;
    volatile bool   stopReq = false;

    std::string query(int start, int limit, const char* nameLessThan) const;
    bool fetchChunk(int c);
    int  countBefore(char letter);            /* -1 on error */
    void runJump();
    void loop();
    static void* threadMain(void* self);
};
