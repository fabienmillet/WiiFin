#pragma once
#include <grrlib.h>
#include <wiiuse/wpad.h>
#include <ogc/lwp.h>
#include <functional>
#include <vector>
#include <string>
#include "../jellyfin/JellyfinClient.h"
#include "MusicPlayerView.h"
#include "BrowseHome.h"
#include "Keyboard.h"
#include "ListFeed.h"

class LibraryView {
public:
    LibraryView(GRRLIB_ttfFont* font, GRRLIB_ttfFont* jpFont, GRRLIB_texImg* cursorTex,
                GRRLIB_texImg* ringTex,
                JellyfinClient& client,
                const JellyfinAuth& auth, const std::string& serverUrl);

    // Returns true when the user exits (B from libraries grid) OR requests play.
    // If pendingPlayUrl is non-empty on return, the caller should play that URL
    // then recreate the LibraryView; otherwise the user just navigated back.
    bool update(ir_t& ir);
    void render(ir_t& ir);
    void drawLoadingFrame(); // draw+flush one spinning ring frame (also called by network callback)

    // Playback finished: keep the current page and its textures (the heap
    // spills into MEM2, which keeps ~45 MB free while MPlayer runs), and
    // reload the detail page for lastItemId (fresh resume position).
    void onPlaybackFinished(const std::string& lastItemId);

    // Name of the signed-in user (header avatar).
    void setUserName(const std::string& name) {
        userName = name; browse.setUserName(name); catalog.setUserName(name);
    }

    std::string pendingPlayUrl;           // set before returning true when the user hits Play
    std::string pendingPlayTitle;         // shown in the player's top bar
    std::string pendingPlayItemId;        // Jellyfin item id for /Sessions/Playing reporting
    std::string pendingPlayMediaSourceId; // MediaSourceId (same as itemId for transcoded streams)
    std::string pendingPlaySessionId;     // PlaySessionId from PlaybackInfo (for progress reporting)
    long long   pendingPlayStartTimeTicks = 0; // seek offset passed to the transcoder (100-ns ticks)
    long long   pendingPlayRuntimeTicks  = 0; // RunTimeTicks from Jellyfin (for progress bar duration)

    // Music play request (set when returning true from a music library)
    bool                              pendingPlayIsMusic  = false;
    std::vector<MusicOverlay::Track>  pendingMusicTracks;
    int                               pendingMusicTrackIdx = 0;

    // Episode list context for the player overlay (next/prev navigation)
    // Non-empty only when playing a TV-show episode.
    std::vector<JellyfinEpisode> pendingPlayEpisodes;   // all episodes in current season
    int                          pendingPlayEpisodeIdx; // index of current episode in the list
    std::string                  pendingPlaySeriesId;   // for re-fetching URLs on track change

    // Media stream lists (for in-player audio/subtitle switching)
    std::vector<MediaStream> pendingPlayAudioStreams;
    std::vector<MediaStream> pendingPlaySubStreams;
    int pendingPlayAudioIdx = 0;   // currently-selected audio stream index (from API)
    int pendingPlaySubIdx   = -1;  // currently-selected sub stream index (-1 = none)

private:
    void clearPendingPlay();
    GRRLIB_ttfFont* font;
    GRRLIB_ttfFont* jpFont;
    GRRLIB_texImg*  cursorTex;
    GRRLIB_texImg*  ringTex;
    JellyfinClient& client;
    JellyfinAuth    auth;
    std::string     serverUrl;

    enum class State {
        LibsInit,     // render loading screen, then transition
        LibsLoad,     // blocking API call
        LibsReady,    // show library grid
        ItemsInit,    // render loading screen, then transition
        ItemsLoad,    // blocking API call
        ItemsReady,   // show items list
        PostersLoad,  // blocking: load poster images for current page
        PostersReady, // show poster grid
        SeasonsLoad,  // blocking: fetch seasons for a TV series
        SeasonsReady, // show season list
        EpisodesLoad, // blocking: fetch episodes for a season
        EpisodesReady,// show episode list
        DetailLoad,   // blocking: load full item metadata + large poster
        DetailReady,  // show item detail page
        ResumePrompt, // ask Continue / Start Over when playbackPositionTicks > 0
        MusicTracksLoad, // blocking: fetch audio tracks for a MusicAlbum
        MusicTracksReady,// show track list for an album
        CollectionsLoad,      // blocking: fetch BoxSet collections
        FavoritesLoad,        // blocking: fetch favourite movies
        MovieSuggestionsLoad, // blocking: fetch continue-watching + recently-added movies
        MovieSuggestionsReady,// show suggestions tab (two horizontal rows)
        TVSuggestionsLoad,    // blocking: fetch continue-watching episodes + recently-added series
        TVSuggestionsReady,   // show TV suggestions tab (two horizontal rows)
        TVUpcomingLoad,       // blocking: fetch upcoming (unaired) episodes
        TVUpcomingReady,      // show Upcoming tab (single horizontal row)
        MusicSuggestionsLoad, // blocking: fetch recently-added albums
        MusicSuggestionsReady,// show music suggestions tab (single card row)
        PlaylistsLoad,        // blocking: fetch playlists
        PlaylistsReady,       // show playlists list
        GlobalFavoritesLoad,  // blocking: fetch all-library favourites
        GlobalFavoritesReady, // show global favourites poster grid
        SearchInput,          // virtual keyboard for search query
        SearchLoad,           // blocking: search API call
        SearchReady,          // show search results
        Error
    };
    State state = State::LibsInit;

    // Libraries
    std::vector<JellyfinLibrary> libraries;
    int  libSel = 0;
    bool  irMode  = false;
    float irLastX  = -1.0f;
    float irLastY  = -1.0f;

    // Items (list mode and poster mode share these)
    std::vector<JellyfinItem> items;
    int itemSel        = 0;
    int viewTop        = 0;
    int itemPage       = 0;
    int itemTotal      = 0;
    std::string currentLibId;
    std::string currentLibName;
    std::string currentLibType;  // collectionType of selected library
    bool posterMode    = false;   // true → poster grid; false → text list
    bool globFavMode   = false;   // true → loadPosters() targets GlobalFavoritesReady

    // Parent drilldown state (e.g. music artist → albums)
    std::string parentLibId;
    std::string parentLibName;
    int         parentItemPage   = 0;
    bool        inItemsDrilldown = false;

    std::string errMsg;
    std::string loggedErr;     /* last error written to the log */
    float       spinAngle = 0.0f;

    static const int ITEMS_PER_PAGE = 50;
    static const int ITEMS_VISIBLE  = 8;
    // Text list of a library (ItemsReady): thinner rows, no pages
    static const int LIST_ROWS  = 10;
    static const int LIST_ROW_H = 36;
    int listWidth() const;      // narrower when the cover panel is shown

    // Tile grid (3 columns)
    static const int TILE_COLS = 3;
    static const int TILE_W    = 190;
    static const int TILE_H    = 90;
    static const int TILE_GAP  = 15;
    static const int GRID_X    = 20;
    static const int GRID_Y    = 65;

    // Items list
    static const int ROW_H  = 46;
    static const int LIST_X = 40;
    static const int LIST_Y = 58;
    static const int LIST_W = 560;

    // Poster grid: 4 columns × 2 rows = 8 posters per page
    static const int POSTER_COLS     = 4;
    static const int POSTER_ROWS     = 2;
    static const int POSTER_VISIBLE  = 8;   // POSTER_COLS * POSTER_ROWS
    static const int POSTERS_PER_PAGE= 8;
    static const int POSTER_W        = 130;
    static const int POSTER_H        = 185;
    static const int POSTER_X0       = 15;
    static const int POSTER_Y0       = 50;
    static const int POSTER_STRIDE_X = 150; // POSTER_W + 20
    static const int POSTER_STRIDE_Y = 193; // POSTER_H + 8 (no title text below)
    // Page arrow buttons rendered to the right of the poster grid
    static const int ARROW_CX        = 617; // horizontal centre of arrow triangles
    static const int ARROW_UP_CY     = 143; // vertical centre — aligns with row 0
    static const int ARROW_DN_CY     = 335; // vertical centre — aligns with row 1
    static const int ARROW_HIT_R     = 17;  // hit-test radius (x and y)
    int posterSel = 0;
    GRRLIB_texImg* posterTextures[8] = {};
    std::string    posterLabels[8];   // pre-truncated display titles, built in loadPosters()

    // Item detail
    JellyfinItemDetail detail;
    GRRLIB_texImg*     detailTex  = nullptr; // large poster for detail view
    std::string        detailItemId;         // id of item being loaded/shown
    State              detailReturnState = State::PostersReady; // where B goes from detail
    bool               detailIsEpisode   = false; // true → use 16:9 thumbnail layout
    std::vector<std::string> detailLines;    // pre-computed word-wrapped overview (built once)
    int detailAudioSel = 0;  // index into detail.audioStreams
    int detailSubSel   = -1; // index into detail.subtitleStreams; -1 = off
    int detailFocusRow = 0;  // 0 = audio row focused, 1 = subtitle row focused
    int resumeSel      = 0;  // 0 = Continue, 1 = Start Over (used in ResumePrompt state)

    // TV show navigation
    std::string currentSeriesId;
    std::string currentSeriesName;
    std::vector<JellyfinSeason>  seasons;
    int seasonSel = 0;
    int seasonTop = 0;
    std::string currentSeasonId;
    std::string currentSeasonName;
    std::vector<JellyfinEpisode> episodes;
    int episodeSel = 0;
    int episodeTop = 0;

    // Music track browsing
    std::string                       musicAlbumId;
    std::string                       musicAlbumName;
    std::string                       musicAlbumArtist;
    std::vector<JellyfinAudioItem>    musicTracks;
    int  musicTrackSel = 0;
    int  musicTrackTop = 0;
    static const int MUSIC_TRACKS_VISIBLE = 8;
    void loadMusicTracks();
    void clampMusicTrackScroll();

    // Continue Watching / Next Up (activity page)
    std::vector<JellyfinItem> continueItems;
    GRRLIB_texImg*            cwTextures[3]     = {};
    std::vector<JellyfinItem> nextUpItems;
    GRRLIB_texImg*            nextUpTextures[3]  = {};
    int  continueSel         = 0;
    int  nextUpSel           = 0;
    int  homePage            = 0; // 0=libs, 1=activity
    int  actRow              = 0; // 0=CW row, 1=NextUp row (activity page)
    bool detailIsEpisodeHint = false;

    // Movie tabs (only active when currentLibType == "movies")
    int         movieTab          = 0;    // 0=Movies, 1=Collections, 2=Favorites, 3=Suggestions
    std::string movieLibId;               // root movies library id (preserved across BoxSet drilldown)
    bool        inBoxSetDrilldown = false;// true when inside a collection's movies

    // TV show tabs (only active when currentLibType == "tvshows")
    int         tvTab   = 0;    // 0=Series, 1=Suggestions, 2=Upcoming
    std::string tvLibId;         // root tvshows library id
    State       seasonsCallerState = State::PostersReady; // where B goes from season list

    // Music tabs (only active when currentLibType == "music")
    int         musicTab       = 0;     // 0=Albums, 1=Suggestions, 2=Playlists
    std::string musicLibId;             // root music library id
    bool        musicIsPlaylist = false;// true when MusicTracksLoad should use getPlaylistTracks

    // Movie suggestions tab data
    std::vector<JellyfinItem> movieContItems;          // continue-watching movies (up to 4)
    GRRLIB_texImg*            movieContTex[4]   = {};
    std::vector<JellyfinItem> movieRecentItems;        // recently-added movies (up to 8)
    GRRLIB_texImg*            movieRecentTex[8] = {};
    int movieSuggestRow     = 0; // 0=continue row, 1=recent row
    int movieSuggestContSel = 0; // selected column in continue row
    int movieSuggestRecSel  = 0; // selected column in recent row
    int movieSuggestContOff = 0; // horizontal scroll offset for continue row
    int movieSuggestRecOff  = 0; // horizontal scroll offset for recent row

    // TV suggestions tab data
    std::vector<JellyfinItem> tvContItems;             // continue-watching episodes (up to 4)
    GRRLIB_texImg*            tvContTex[4]   = {};
    std::vector<JellyfinItem> tvRecentItems;           // recently-added series (up to 8)
    GRRLIB_texImg*            tvRecentTex[8] = {};
    int tvSuggestRow     = 0;
    int tvSuggestContSel = 0;
    int tvSuggestRecSel  = 0;
    int tvSuggestContOff = 0;
    int tvSuggestRecOff  = 0;

    // TV upcoming tab data
    std::vector<JellyfinItem> tvUpcomingItems;         // upcoming episodes (up to 8)
    GRRLIB_texImg*            tvUpcomingTex[8] = {};
    int tvUpcomingSel = 0;
    int tvUpcomingOff = 0;

    // Music suggestions tab data (recently added albums)
    std::vector<JellyfinItem> musicRecentItems;        // recently added albums (up to 8)
    GRRLIB_texImg*            musicRecentTex[8] = {};
    int musicSuggestSel = 0;
    int musicSuggestOff = 0;
    static const int SUGG_VISIBLE = 4; // cards shown per row (4 × 130px + 3 × 20px gap = 580 fits in 640)
    // Pre-computed display strings for activity cards (built once at load time,
    // avoids per-frame Text::width truncation loops that drop frame rate)
    std::string cwDisplayMain[3], cwDisplaySub[3];
    std::string nuDisplayMain[3], nuDisplaySub[3];

    void loadLibraries();
    void loadContinueWatching();
    void loadNextUp();
    void buildActDisplayStrings(); // pre-compute truncated titles for activity cards
    void freeCWTextures();
    void freeNextUpTextures();
    void loadItems();
    void loadPosters();
    void loadDetail();
    void preparePlay(long long startTicks, const std::vector<JellyfinEpisode>* queue = nullptr);
    bool startShuffle(const std::string& seasonId);   /* "" = whole series */
    void loadSeasons();
    void loadEpisodes();
    void loadMovieCollections();
    void loadMovieFavorites();
    void loadGlobalFavorites();
    void loadMovieSuggestions();
    void freeMovieSuggestions();
    void loadTVSuggestions();
    void freeTVSuggestions();
    void loadTVUpcoming();
    void freeTVUpcoming();
    void loadMusicSuggestions();
    void freeMusicSuggestions();
    void loadPlaylistsTab();
    void runWithLoading(std::function<void()> fn);
    void freePosters();
    void freeDetail();
    void drawCursor(ir_t& ir);
    void drawDetailView(ir_t& ir);
    void clampScroll();
    void clampSeasonScroll();
    void clampEpisodeScroll();

    // Search
    std::string               searchQuery;
    std::vector<JellyfinItem> searchResults;
    int                       searchSel         = 0;
    int                       searchTop         = 0;
    State                     searchReturnState = State::LibsReady;
    static const int          SEARCH_VISIBLE    = 8;
    bool                      drilldownFromSearch = false; // true when ItemsReady/PostersReady was entered from search
    Keyboard                  searchKb;          // on-screen keyboard for search

    void performSearch();
    void clampSearchScroll();
    void renderSearchInput(ir_t& ir);
    void renderSearchResults(ir_t& ir);

    u32         colorForType(const std::string& type, bool selected);
    const char* labelForType(const std::string& type);

    // "Rows" home layout: carousel home (rows of posters) instead of the tile grid
    BrowseHome browse;                  // what to watch now
    BrowseHome catalog;                 // the "browse" page (libraries, A-Z, genres)
    ListFeed   feed;                    // ItemsReady: whole library, loaded as it scrolls
    int        listRestoreSel = -1;     // selection to restore after a drill-down
    unsigned long long letterFlashMs = 0; // letter jump feedback
    char       letterFlash = 0;
    std::string userName;
    bool       browsePage     = false;  // catalog shown instead of the home rows
    bool       tracksFromHome = false;  // album opened from the carousel home
    bool       episodesFromHome = false; // season opened from the carousel home
    bool flixHome() const;
    bool updateState(ir_t& ir);
    void openItem(const JellyfinItem& it, State returnState);
    void openLibrary(int index);
};
