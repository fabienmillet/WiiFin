#pragma once

#include <grrlib.h>
#include <string>
#include <vector>
#include "../jellyfin/JellyfinClient.h"

/* -----------------------------------------------------------------------
 * Trickplay — the thumbnails shown over the seek bar while seeking.
 *
 * Jellyfin serves them as tiles of 10 x 10 thumbnails (3200 x 1800 for
 * the usual 320-wide ones, one every 10 s: a tile covers 16 min).  A loader
 * thread downloads the tile the player asks for and cuts it, decoded at
 * half size, into one small texture per thumbnail; the main thread swaps
 * them in.  One tile is kept, about 3 MB.
 *
 * Main thread:
 *     tp.start(...);                 for each video
 *     tp.setOnline(...);             each frame: may the loader use the network?
 *     tex = tp.thumbnail(secs, show); nullptr until loaded, or when there are none
 *     tp.stop();
 * ----------------------------------------------------------------------- */
class Trickplay {
public:
    ~Trickplay() { stop(); }

    void start(JellyfinClient& client, const std::string& serverUrl, const JellyfinAuth& auth,
               const std::string& itemId, const std::string& mediaSourceId);
    void stop();
    const std::string& item() const { return itemId; }

    /* MPlayer closes every socket when it stops a stream and reports to the
     * server while it opens the next one: no requests meanwhile.  roomy:
     * the video's cache has a margin, so a tile may be fetched ahead. */
    void setOnline(bool on, bool roomy) { online = on; this->roomy = roomy; }

    /* Thumbnail at secs into the video, and asks for its tile: right away
     * when shown (seeking), else only while the cache is roomy, so a slow
     * link never rebuffers for a thumbnail nobody looks at.  The texture
     * stays valid until the next call. */
    GRRLIB_texImg* thumbnail(float secs, bool show);

private:
    JellyfinClient* client = nullptr;
    std::string serverUrl, itemId, mediaSourceId;
    JellyfinAuth auth;
    TrickplayInfo info;                  /* written by the loader before infoDone */

    unsigned int thread = 0;             /* lwp_t, 0 = not running */
    volatile bool quit    = false;
    volatile bool online  = false;
    volatile bool roomy   = false;
    volatile bool urgent  = false;       /* the wanted tile is to be shown */
    volatile bool infoDone = false;      /* the loader asked the server */
    volatile int  wantTile = -1;         /* main -> loader */

    /* loader -> main: a decoded tile, handed over when readyTile >= 0 */
    std::vector<GRRLIB_texImg*> ready;
    volatile int readyTile = -1;

    std::vector<GRRLIB_texImg*> shown;   /* main thread only */
    int shownTile = -1;

    static void* threadMain(void* self);
    void loop();
    bool decodeTile(const std::string& jpeg, int tile, std::vector<GRRLIB_texImg*>& out);
    static void freeAll(std::vector<GRRLIB_texImg*>& v);
};
