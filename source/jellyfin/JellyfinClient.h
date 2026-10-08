#pragma once
#include <string>
#include <vector>

struct JellyfinLibrary {
    std::string id;
    std::string name;
    std::string collectionType; // "movies", "tvshows", "music", "books", ...
};

struct JellyfinItem {
    std::string id;
    std::string name;
    std::string type; // "Movie", "Series", "MusicAlbum", ...
    int         year = 0;
    long long   playbackPositionTicks = 0;
    long long   runtimeTicks          = 0;
    std::string seriesName;    // Episode only
    std::string seriesId;      // Episode only — parent series ID for backdrop lookup
    int         seasonNumber  = 0;
    int         episodeNumber = 0;
    // Filled by getItemsByQuery (carousel home)
    float       communityRating   = 0.0f;  // 0..10, 0 = unknown
    std::string officialRating;            // "TV-PG", "PG-13", ...
    int         childCount         = 0;    // Series: seasons
    int         recursiveItemCount = 0;    // Series: episodes
    std::string sortName;                  // what the list is sorted on ("matrix" for "The Matrix")
    std::string extraType;                 // a special feature's: "Trailer", "Featurette"...
};

struct JellyfinSeason {
    std::string id;
    std::string name;
    int         indexNumber = 0;
};

struct JellyfinEpisode {
    std::string id;
    std::string name;
    int         indexNumber  = 0; // episode number within season
    int         seasonNumber = 0;
    long long   playbackPositionTicks = 0;
    bool        played = false;   // UserData.Played
};

struct JellyfinPerson {
    std::string name;
    std::string role; // "Actor", "Director", "Writer", ...
    std::string character;
};

struct MediaStream {
    int         index        = 0;
    std::string type;         // "Audio", "Subtitle", "Video"
    std::string displayTitle;
    std::string language;
    std::string codec;
    bool        isText       = false;  // subtitles: text (SRT, ASS...), drawn by WiiFin
};

/* One file of a title that has several (MediaSources): widescreen and
 * fullscreen cuts, 1080p and 720p... each with its own tracks */
struct MediaVersion {
    std::string id;            // its MediaSourceId
    std::string name;          // "Widescreen", "1080p"... (the file's label)
    std::vector<MediaStream> audio, subs;
    int defaultAudio = -1, defaultSub = -1;   // the stream indexes the server picks
};

struct JellyfinItemDetail {
    std::string id;
    std::string name;
    std::string overview;      // synopsis
    std::string officialRating;
    int         year                  = 0;
    long long   runtimeTicks          = 0; // RunTimeTicks (64-bit from Jellyfin API)
    long long   playbackPositionTicks = 0; // UserData.PlaybackPositionTicks
    bool        isFavorite            = false; // UserData.IsFavorite
    bool        played                = false; // UserData.Played
    int         specialFeatures       = 0;     // its bonus videos (trailers included)
    std::string seriesId;      // Episode only: its series and season, for
    std::string seasonId;      //   next / previous wherever it was opened
    std::vector<std::string>    genres;          // up to 4
    std::vector<JellyfinPerson> people;          // up to 6 (cast + director)
    std::vector<MediaStream>    audioStreams;    // audio tracks
    std::vector<MediaStream>    subtitleStreams; // subtitle tracks
    std::vector<MediaVersion>   versions;        // two or more files, else empty
    /* the tracks to start with, as the server picks them for this user
     * (its subtitle mode and languages, the file's default and forced
     * flags): stream indexes, -1 none */
    int defaultAudioIndex = -1, defaultSubIndex = -1;
};

struct JellyfinAudioItem {
    std::string id;
    std::string name;        // track title
    std::string artist;      // AlbumArtist
    std::string album;
    int         trackNumber      = 0;
    long long   runtimeTicks     = 0;
    long long   playbackPositionTicks = 0;
};

/* What the music player shows about the playing track (getAudioTrackInfo) */
struct AudioTrackInfo {
    std::string artists;           // "A, B", empty when untagged
    std::string album;
    std::string albumId;
    std::string parentId;          // folder holding the file
    int         year          = 0;
    bool        isFavorite    = false;
    bool        hasImage      = false;  // the track's own cover (embedded)
    bool        albumHasImage = false;
};

struct JellyfinAuth {
    std::string userId;
    std::string accessToken;
    std::string serverName;
};

/* Intro / credits segment timestamps (from Jellyfin Intro Skipper API) */
/* The parts of a video the player offers to skip: Jellyfin's media
 * segments (10.10+, filled by Intro Skipper and the like), or Intro
 * Skipper's own API on older servers. */
struct MediaSegment {
    enum Kind { Intro, Recap, Preview, Commercial, Outro };
    Kind  kind  = Intro;
    float start = 0.0f;   /* seconds */
    float end   = 0.0f;
};
struct IntroInfo {
    std::vector<MediaSegment> segments;   /* in order of start */
};

/* Trickplay: the thumbnails Jellyfin takes along a video (10.9 and later),
 * served as tiles of tileW x tileH thumbnails, one every intervalMs. */
struct TrickplayInfo {
    int width = 0, height = 0;   /* one thumbnail, in pixels           */
    int tileW = 0, tileH = 0;    /* thumbnails per tile, across, down  */
    int count = 0;               /* thumbnails in all                  */
    int intervalMs = 0;
    bool ok() const {
        return width > 0 && height > 0 && tileW > 0 && tileH > 0 && count > 0 && intervalMs > 0;
    }
};

struct QuickConnectResult {
    std::string code;    // 6-char code shown to user
    std::string secret;  // used to poll / authenticate
    bool authenticated;  // true once user approved on another device
};

struct DiscoveredServer {
    std::string name;    // server display name
    std::string address; // full URL, e.g. "http://192.168.1.10:8096"
};

class JellyfinClient {
public:
    // Bring the network up in the background (call once at boot, and again
    // to retry after a failure).  networkBusy() is true while it runs.
    void startNetwork();
    bool networkBusy() const { return netBusy; }
    // Once networkBusy() is false: the outcome of the last attempt.
    bool takeNetworkResult();
    // IOS dropped the network (Wi-Fi lost): restart it, blocking.
    bool recoverNetwork();
    // Blocking: starts the network if needed and waits for the result.
    bool initNetwork();

    // Close the kept-alive HTTPS connection.  Call whenever IOS sockets may
    // be closed behind our back (MPlayer closes all of them after a video
    // session), so a stale socket number is never reused.
    void dropConnection();

    // Authenticate with username + password
    // Returns true on success, fills out auth
    bool authenticate(const std::string& serverUrl,
                      const std::string& username,
                      const std::string& password,
                      JellyfinAuth& out);

    // Quick Connect: initiate a session, returns code+secret or empty on failure
    bool quickConnectInitiate(const std::string& serverUrl, QuickConnectResult& out);

    // Quick Connect: poll until approved (call repeatedly), returns true when done
    bool quickConnectCheck(const std::string& serverUrl,
                           const std::string& secret,
                           QuickConnectResult& out);

    // Quick Connect: exchange secret for token after approval
    bool quickConnectAuthenticate(const std::string& serverUrl,
                                  const std::string& secret,
                                  JellyfinAuth& out);

    // Discover Jellyfin servers on the LAN via UDP broadcast (port 7359)
    // Blocks for ~2 s; returns true if socket opened (even if no servers found)
    bool discoverServers(std::vector<DiscoveredServer>& out);

    bool isNetworkReady() const { return networkReady; }
    const std::string& lastError() const { return errMsg; }

    // Fetch user views (libraries: movies, tvshows, music...)
    bool getLibraries(const std::string& serverUrl,
                      const JellyfinAuth& auth,
                      std::vector<JellyfinLibrary>& out);

    // Fetch items inside a library or folder (paginated)
    // extra: more of the query (the list's sort and filters, "&SortBy=...");
    // without a SortBy, by name
    bool getItems(const std::string& serverUrl,
                  const JellyfinAuth& auth,
                  const std::string& parentId,
                  int startIndex, int limit,
                  std::vector<JellyfinItem>& out,
                  int& totalCount,
                  const std::string& extra = "");

    // Whether the user has the item among their favourites, and how many
    // special features (trailers included) it has.
    bool getIsFavorite(const std::string& serverUrl, const JellyfinAuth& auth,
                       const std::string& itemId, bool& favorite, int* specials = nullptr);

    // A film's or a series' special features: its local trailers, then the
    // rest (featurettes, behind the scenes, deleted scenes...), each with
    // its extraType.
    bool getSpecialFeatures(const std::string& serverUrl, const JellyfinAuth& auth,
                            const std::string& itemId, std::vector<JellyfinItem>& out);

    // Fetch albums for a specific artist (uses AlbumArtistIds, more reliable than ParentId)
    bool getAlbumsByArtist(const std::string& serverUrl,
                            const JellyfinAuth& auth,
                            const std::string& artistId,
                            int startIndex, int limit,
                            std::vector<JellyfinItem>& out,
                            int& totalCount);

    // Fetch raw image bytes for an item's primary thumbnail
    bool getItemImageBytes(const std::string& serverUrl,
                           const JellyfinAuth& auth,
                           const std::string& itemId,
                           int maxWidth, int maxHeight,
                           std::string& outBytes);

    // Fetch the best landscape image for activity cards:
    //   Episode → Images/Thumb, fallback series Backdrop, then Primary
    //   Movie/Series → Images/Backdrop/0, fallback Primary
    bool getItemBackdropBytes(const std::string& serverUrl,
                              const JellyfinAuth& auth,
                              const JellyfinItem& item,
                              int maxWidth, int maxHeight,
                              std::string& outBytes);

    // Fetch the server name from /System/Info (fills auth.serverName)
    bool getServerName(const std::string& serverUrl,
                       const JellyfinAuth& auth,
                       std::string& outName);

    // Diagnostics for wiifin.log.  The server's Jellyfin version (once per
    // server), and after a failed transcode the end of the server's newest
    // FFmpeg log, file paths masked (only an administrator may read it).
    void logServerInfo(const std::string& serverUrl);
    void logTranscodeFailure(const std::string& serverUrl, const JellyfinAuth& auth);

    // Fetch up to 3 in-progress ("Continue Watching") video items
    bool getContinueWatching(const std::string& serverUrl,
                             const JellyfinAuth& auth,
                             std::vector<JellyfinItem>& out);

    // Fetch up to 3 "Next Up" episodes (next in series to watch)
    bool getNextUp(const std::string& serverUrl,
                   const JellyfinAuth& auth,
                   std::vector<JellyfinItem>& out);

    // Generic item query for the carousel home.  pathAndQuery is everything
    // after the server URL ("/Users/<id>/Items/Latest?ParentId=...&Limit=16");
    // the common Fields/EnableImages parameters are appended.  Accepts both
    // the {"Items":[...]} envelope and the raw arrays /Items/Latest returns.
    bool getItemsByQuery(const std::string& serverUrl,
                         const JellyfinAuth& auth,
                         const std::string& pathAndQuery,
                         std::vector<JellyfinItem>& out,
                         int* totalCount = nullptr);

    // Fetch BoxSet collections from a movies library (paginated)
    bool getMovieCollections(const std::string& serverUrl,
                             const JellyfinAuth& auth,
                             const std::string& parentId,
                             int startIndex, int limit,
                             std::vector<JellyfinItem>& out,
                             int& totalCount);

    // Fetch favourite movies from a library (paginated)
    bool getFavoriteMovies(const std::string& serverUrl,
                           const JellyfinAuth& auth,
                           const std::string& parentId,
                           int startIndex, int limit,
                           std::vector<JellyfinItem>& out,
                           int& totalCount);

    // Fetch all favourites globally (Movie, Series, MusicAlbum) across all libraries (paginated)
    bool getGlobalFavorites(const std::string& serverUrl,
                            const JellyfinAuth& auth,
                            int startIndex, int limit,
                            std::vector<JellyfinItem>& out,
                            int& totalCount);

    // Fetch continue-watching movies (up to 4)
    bool getMovieContinueWatching(const std::string& serverUrl,
                                  const JellyfinAuth& auth,
                                  std::vector<JellyfinItem>& out);

    // Fetch recently added movies via /Users/{id}/Items/Latest
    bool getMoviesLatest(const std::string& serverUrl,
                         const JellyfinAuth& auth,
                         const std::string& parentId,
                         int limit,
                         std::vector<JellyfinItem>& out);

    // Fetch continue-watching TV episodes (up to 4)
    bool getTVContinueWatching(const std::string& serverUrl,
                               const JellyfinAuth& auth,
                               std::vector<JellyfinItem>& out);

    // Fetch recently added TV series via /Users/{id}/Items/Latest
    bool getTVSeriesLatest(const std::string& serverUrl,
                           const JellyfinAuth& auth,
                           const std::string& parentId,
                           int limit,
                           std::vector<JellyfinItem>& out);

    // Fetch upcoming (unaired) episodes via /Shows/Upcoming
    bool getTVUpcoming(const std::string& serverUrl,
                       const JellyfinAuth& auth,
                       int limit,
                       std::vector<JellyfinItem>& out);

    // Fetch recently added music albums via /Users/{id}/Items/Latest
    bool getMusicLatest(const std::string& serverUrl,
                        const JellyfinAuth& auth,
                        const std::string& parentId,
                        int limit,
                        std::vector<JellyfinItem>& out);

    // Fetch all playlists (paginated)
    bool getPlaylists(const std::string& serverUrl,
                      const JellyfinAuth& auth,
                      int startIndex, int limit,
                      std::vector<JellyfinItem>& out,
                      int& totalCount);

    // Fetch audio items inside a playlist
    bool getPlaylistTracks(const std::string& serverUrl,
                           const JellyfinAuth& auth,
                           const std::string& playlistId,
                           std::vector<JellyfinAudioItem>& out);

    // Fetch full item metadata (overview, genres, cast, runtime)
    bool getItemDetail(const std::string& serverUrl,
                       const JellyfinAuth& auth,
                       const std::string& itemId,
                       JellyfinItemDetail& out);

    // Fetch seasons for a TV series
    bool getSeasons(const std::string& serverUrl,
                    const JellyfinAuth& auth,
                    const std::string& seriesId,
                    std::vector<JellyfinSeason>& out);

    // Fetch episodes for a season
    bool getEpisodes(const std::string& serverUrl,
                     const JellyfinAuth& auth,
                     const std::string& seriesId,
                     const std::string& seasonId,
                     std::vector<JellyfinEpisode>& out);

    // Every episode of a series, season after season (specials included)
    bool getSeriesEpisodes(const std::string& serverUrl,
                           const JellyfinAuth& auth,
                           const std::string& seriesId,
                           std::vector<JellyfinEpisode>& out);

    // Up to `limit` episodes in random order, shuffled by the server: the
    // whole series, or one season when seasonId is not empty.
    bool getShuffledEpisodes(const std::string& serverUrl,
                             const JellyfinAuth& auth,
                             const std::string& seriesId,
                             const std::string& seasonId,
                             int limit,
                             std::vector<JellyfinEpisode>& out);

    // Fetch intro/credits timestamps for an episode.
    // Tries the Intro Skipper plugin endpoint first; returns false (no error)
    // if the server responds 404 (plugin not installed or no data for item).
    /* the item's segments (see IntroInfo): /MediaSegments, else Intro
     * Skipper's IntroSkipperSegments, else its old IntroTimestamps */
    bool getIntroTimestamps(const std::string& serverUrl,
                            const JellyfinAuth& auth,
                            const std::string& episodeId,
                            IntroInfo& out);

    // Trickplay thumbnails of a video, at the smallest size the server made.
    // False when there are none (not generated, or a server before 10.9).
    bool getTrickplayInfo(const std::string& serverUrl,
                          const JellyfinAuth& auth,
                          const std::string& itemId,
                          const std::string& mediaSourceId,
                          TrickplayInfo& out);

    // One tile of them, a JPEG: /Videos/{id}/Trickplay/{width}/{index}.jpg
    bool getTrickplayTile(const std::string& serverUrl,
                          const JellyfinAuth& auth,
                          const std::string& itemId,
                          const std::string& mediaSourceId,
                          int width, int index,
                          std::string& outBytes);

    // A text subtitle track as SRT (Jellyfin converts ASS, VTT... to it).
    bool getSubtitleSrt(const std::string& serverUrl,
                        const JellyfinAuth& auth,
                        const std::string& itemId,
                        const std::string& mediaSourceId,
                        int streamIndex,
                        std::string& outSrt);

    // Items by id, in that order, with their type ("Play on..." sends ids):
    // Audio ones with what the music player shows.
    bool getItemsByIds(const std::string& serverUrl,
                       const JellyfinAuth& auth,
                       const std::vector<std::string>& ids,
                       std::vector<JellyfinItem>& outItems,
                       std::vector<JellyfinAudioItem>& outAudio);

    // Fetch tracks (Audio items) for a MusicAlbum
    bool getAlbumTracks(const std::string& serverUrl,
                        const JellyfinAuth& auth,
                        const std::string& albumId,
                        std::vector<JellyfinAudioItem>& out);

    // Artists, album, year, favourite and covers of one track.
    bool getAudioTrackInfo(const std::string& serverUrl,
                           const JellyfinAuth& auth,
                           const std::string& itemId,
                           AudioTrackInfo& out);

    // Mark an item watched or not watched (watched clears its resume point).
    bool setPlayed(const std::string& serverUrl, const JellyfinAuth& auth,
                   const std::string& itemId, bool played);

    // Mark or unmark an item as a favourite of the user.
    bool setFavorite(const std::string& serverUrl,
                     const JellyfinAuth& auth,
                     const std::string& itemId,
                     bool favorite);

    // Tracks to play after itemId once the queue runs out: Jellyfin's
    // instant mix (same genres and artists), or when it has none (untagged
    // files), random tracks of the same folder.  itemId itself is left out.
    bool getAutoplayTracks(const std::string& serverUrl,
                           const JellyfinAuth& auth,
                           const std::string& itemId,
                           const std::string& parentId,
                           int limit,
                           std::vector<JellyfinAudioItem>& out);

    // Build a direct audio transcoding URL for an Audio item.
    // Returns the full URL; empty string on failure.
    // outPlaySessionId is filled for later reportPlaybackStopped calls.
    // outPlayMethod: "DirectPlay" when the server sends the file as it is
    // (MP3 up to 320 kb/s, FLAC and WAV in stereo up to 48 kHz), else
    // "Transcode" (to MP3, the reasons in the URL); outAudioIndex: its
    // audio stream.
    bool getAudioStreamUrl(const std::string& serverUrl,
                           const JellyfinAuth& auth,
                           const std::string& itemId,
                           long long startTimeTicks,
                           std::string& outUrl,
                           std::string& outPlaySessionId,
                           std::string* outPlayMethod = nullptr,
                           int* outAudioIndex = nullptr);

    // Direct play: the file as it is, when MPlayer CE decodes it in real
    // time on the Wii (a cautious device profile: SD MPEG-4 ASP, MPEG-1/2,
    // H.264 up to 480p, VP8; AVI, MKV, MP4, MPEG-TS/PS) and the link carries
    // it.  Only without subtitles (they would have to be burned in) and with
    // the file's default audio track.  False: transcode (getTranscodingUrl).
    struct DirectPlay {
        std::string url;
        std::string playSessionId;
        std::string container, videoCodec, audioCodec;
        int   width = 0, height = 0;
        float fps   = 0.0f;
        std::string demuxer;    // MPlayer's for it: avi, mkv, mpegps, lavf:mov, lavf:mpegts
        std::string reasons;    // when not: Jellyfin's TranscodeReasons, for the dashboard
        int   aid   = -1;       // MPlayer's -aid for the chosen audio track, -1: the first
    };
    bool getDirectPlayUrl(const std::string& serverUrl,
                          const JellyfinAuth& auth,
                          const std::string& itemId,
                          const std::string& mediaSourceId,
                          int audioStreamIndex,
                          int subtitleStreamIndex,
                          DirectPlay& out);
    bool directPlay = true;   // the "Direct play" setting (Auto / Off)

    // How a video is played: the file as it is, or a transcode.
    struct PlaybackChoice {
        bool  direct = false;
        std::string demuxer;    // direct: MPlayer's demuxer for the file
        float fps    = 0.0f;    // the file's frame rate, a transcode's: the
                                //   source's within MaxFramerate; 0 = unknown
        int   aid    = -1;      // direct: MPlayer's -aid, -1 = the first track
    };
    // Direct play when it can (getDirectPlayUrl, allowDirect), else a
    // transcode (getTranscodingUrl).
    bool getPlaybackUrl(const std::string& serverUrl,
                        const JellyfinAuth& auth,
                        const std::string& itemId,
                        const std::string& mediaSourceId,
                        int audioStreamIndex,
                        int subtitleStreamIndex,
                        long long startTimeTicks,
                        std::string& outUrl,
                        std::string& outPlaySessionId,
                        PlaybackChoice& how,
                        bool allowDirect = true);

    // Ask Jellyfin to transcode via POST /Items/{id}/PlaybackInfo.
    // EnableDirectPlay and EnableDirectStream are set to false in the JSON body
    // (not just as query params) so Jellyfin cannot ignore them.
    // subtitleStreamIndex = -1 means no subtitles.
    // outPlaySessionId is filled from the response (needed for progress reporting).
    // Returns false if the request fails; outUrl/outPlaySessionId untouched.
    bool getTranscodingUrl(const std::string& serverUrl,
                           const JellyfinAuth& auth,
                           const std::string& itemId,
                           const std::string& mediaSourceId,
                           int audioStreamIndex,
                           int subtitleStreamIndex,
                           long long startTimeTicks,
                           std::string& outUrl,
                           std::string& outPlaySessionId,
                           const std::string& transcodeReasons = "");

    // What this client obeys from the server's remote control (the
    // dashboard's buttons, "Send message"): sent once signed in, the
    // commands then come through Remote's WebSocket.
    bool postCapabilities(const std::string& serverUrl, const JellyfinAuth& auth);

    // Playback reporting — call around a playback session
    // playMethod: "Transcode", "DirectStream" or "DirectPlay", what the
    // server's dashboard shows; the stream indexes are left out when < 0.
    bool reportPlaybackStart(const std::string& serverUrl,
                             const JellyfinAuth& auth,
                             const std::string& itemId,
                             const std::string& mediaSourceId,
                             const std::string& playSessionId,
                             const char* playMethod = "Transcode",
                             int audioStreamIndex = -1,
                             int subtitleStreamIndex = -1);

    bool reportPlaybackProgress(const std::string& serverUrl,
                                const JellyfinAuth& auth,
                                const std::string& itemId,
                                const std::string& mediaSourceId,
                                const std::string& playSessionId,
                                long long positionTicks,
                                bool isPaused,
                                const char* playMethod = "Transcode",
                                int audioStreamIndex = -1,
                                int subtitleStreamIndex = -1);

    bool reportPlaybackStopped(const std::string& serverUrl,
                               const JellyfinAuth& auth,
                               const std::string& itemId,
                               const std::string& mediaSourceId,
                               const std::string& playSessionId,
                               long long positionTicks);

    // Tell the server to kill the active FFmpeg transcode for this session.
    // Must be called after reportPlaybackStopped to prevent zombie encodes.
    // DELETE /Videos/ActiveEncodings?DeviceId={deviceId()}&PlaySessionId={id}
    bool deleteActiveEncoding(const std::string& serverUrl,
                              const JellyfinAuth& auth,
                              const std::string& playSessionId);

    // Search items across all libraries (Movie, Series, MusicAlbum, Audio)
    bool searchItems(const std::string& serverUrl,
                     const JellyfinAuth& auth,
                     const std::string& searchTerm,
                     int limit,
                     std::vector<JellyfinItem>& out);

    // DeviceId sent to Jellyfin: one per console, or the server mixes their
    // sessions up and signs one out when the other signs in.
    static void setDeviceId(const std::string& id);
    static const std::string& deviceId();

    bool sslVerify = true;   // true = verify certificate (set false to allow self-signed)

    // Video transcode quality: 0 = Low, 1 = Normal, 2 = High (see videoBitrate()).
    int videoQuality = 1;
    // Ask Jellyfin to re-encode video and audio instead of copying a
    // compatible source stream.  Set after the server failed a transcode
    // with HTTP 500 (copying an old AVI/Xvid file into MPEG-TS fails).
    bool forceReencode = false;

    static const int VIDEO_QUALITY_COUNT = 4;
    static const char* videoQualityName(int q);
    int videoBitrate() const;      // bits/s for effectiveQuality()
    static int qualityBitrate(int q);   // bits/s of a quality
    // The link measured at the first video of this run, kb/s (0: not yet),
    // and the quality it holds the setting to.
    int measuredKbps() const { return linkKbps; }
    int linkQuality() const  { return linkCap; }

    // videoQuality, capped to what the link carries (measured once per run
    // and server, at the first playback: see measureLink).
    int effectiveQuality() const { return videoQuality < linkCap ? videoQuality : linkCap; }
    void measureLink(const std::string& serverUrl, const JellyfinAuth& auth);

private:
    int         linkCap = VIDEO_QUALITY_COUNT - 1;
    int         linkKbps = 0;            // measured, 0 = not yet
    std::string linkMeasuredFor;
    // GET an episode list (/Shows/{id}/Episodes...) and parse it
    bool fetchEpisodes(const std::string& url, const JellyfinAuth& auth,
                       std::vector<JellyfinEpisode>& out, size_t maxResponse = 0);
    volatile bool networkReady = false;
    volatile bool netBusy      = false;
    unsigned int  netThread    = 0;         // lwp_t of the start-up thread, 0 = none
    void bringUpNetwork();
    static void* netThreadMain(void* self);
    std::string errMsg;
    std::string localIp_;   // set by initNetwork, used by discoverServers
    std::string localMask_;

    // Low-level HTTP/HTTPS: auto-detects scheme, returns HTTP status code.
    // A response is read into a 256 KB buffer; maxResponse lets it grow
    // past that on the heap (subtitles), 0 = cut at 256 KB.
    int httpRequest(const std::string& url,
                    const std::string& method,
                    const std::string& contentType,
                    const std::string& body,
                    const std::string& authToken,
                    std::string& responseBody,
                    size_t maxResponse = 0);

    // One HTTP/1.1 request on the kept-alive connection (TLS if tls is set)
    int request(bool tls, const std::string& host, int port,
                     const std::string& path,
                     const std::string& method,
                     const std::string& contentType,
                     const std::string& body,
                     const std::string& authToken,
                     std::string& responseBody,
                     size_t maxResponse = 0);

    // Minimal JSON field extractor: finds first "key":"value" and returns value
    std::string jsonGetString(const std::string& json, const std::string& key, bool newlines = false);
    static std::string nextJsonObject(const std::string& s, size_t& pos);
    void parseStreams(const std::string& json, std::vector<MediaStream>& audio, std::vector<MediaStream>& subs);
    // For boolean fields
    bool jsonGetBool(const std::string& json, const std::string& key);
    // For integer fields
    int  jsonGetInt(const std::string& json, const std::string& key);
    long long jsonGetLongLong(const std::string& json, const std::string& key);

    // Parse host/port/path from URL; sets isHttps to true for https:// scheme
    bool parseUrl(const std::string& url,
                  std::string& host, int& port,
                  std::string& basePath, bool& isHttps);
};
