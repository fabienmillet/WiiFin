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
};

struct JellyfinItemDetail {
    std::string id;
    std::string name;
    std::string overview;      // synopsis
    std::string officialRating;
    int         year                  = 0;
    long long   runtimeTicks          = 0; // RunTimeTicks (64-bit from Jellyfin API)
    long long   playbackPositionTicks = 0; // UserData.PlaybackPositionTicks
    std::vector<std::string>    genres;          // up to 4
    std::vector<JellyfinPerson> people;          // up to 6 (cast + director)
    std::vector<MediaStream>    audioStreams;    // audio tracks
    std::vector<MediaStream>    subtitleStreams; // subtitle tracks
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

struct JellyfinAuth {
    std::string userId;
    std::string accessToken;
    std::string serverName;
};

/* Intro / credits segment timestamps (from Jellyfin Intro Skipper API) */
struct IntroInfo {
    bool  hasIntro       = false;
    float introStart     = 0.0f; /* seconds — start of intro segment   */
    float introEnd       = 0.0f; /* seconds — end of intro segment      */
    float showPromptAt   = 0.0f; /* when to show the Skip-Intro prompt  */
    float hidePromptAt   = 0.0f; /* when to hide the prompt             */
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
    // Initialize networking (call once)
    bool initNetwork();

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
    bool getItems(const std::string& serverUrl,
                  const JellyfinAuth& auth,
                  const std::string& parentId,
                  int startIndex, int limit,
                  std::vector<JellyfinItem>& out,
                  int& totalCount);

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

    // Fetch up to 3 in-progress ("Continue Watching") video items
    bool getContinueWatching(const std::string& serverUrl,
                             const JellyfinAuth& auth,
                             std::vector<JellyfinItem>& out);

    // Fetch up to 3 "Next Up" episodes (next in series to watch)
    bool getNextUp(const std::string& serverUrl,
                   const JellyfinAuth& auth,
                   std::vector<JellyfinItem>& out);

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

    // Fetch intro/credits timestamps for an episode.
    // Tries the Intro Skipper plugin endpoint first; returns false (no error)
    // if the server responds 404 (plugin not installed or no data for item).
    bool getIntroTimestamps(const std::string& serverUrl,
                            const JellyfinAuth& auth,
                            const std::string& episodeId,
                            IntroInfo& out);

    // Fetch tracks (Audio items) for a MusicAlbum
    bool getAlbumTracks(const std::string& serverUrl,
                        const JellyfinAuth& auth,
                        const std::string& albumId,
                        std::vector<JellyfinAudioItem>& out);

    // Build a direct audio transcoding URL for an Audio item.
    // Returns the full URL; empty string on failure.
    // outPlaySessionId is filled for later reportPlaybackStopped calls.
    bool getAudioStreamUrl(const std::string& serverUrl,
                           const JellyfinAuth& auth,
                           const std::string& itemId,
                           long long startTimeTicks,
                           std::string& outUrl,
                           std::string& outPlaySessionId);

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
                           std::string& outPlaySessionId);

    // Playback reporting — call before and after wii_player_play()
    // positionTicks is ignored by reportPlaybackStart; pass 0.
    bool reportPlaybackStart(const std::string& serverUrl,
                             const JellyfinAuth& auth,
                             const std::string& itemId,
                             const std::string& mediaSourceId,
                             const std::string& playSessionId);

    bool reportPlaybackProgress(const std::string& serverUrl,
                                const JellyfinAuth& auth,
                                const std::string& itemId,
                                const std::string& mediaSourceId,
                                const std::string& playSessionId,
                                long long positionTicks,
                                bool isPaused);

    bool reportPlaybackStopped(const std::string& serverUrl,
                               const JellyfinAuth& auth,
                               const std::string& itemId,
                               const std::string& mediaSourceId,
                               const std::string& playSessionId,
                               long long positionTicks);

    // Tell the server to kill the active FFmpeg transcode for this session.
    // Must be called after reportPlaybackStopped to prevent zombie encodes.
    // DELETE /Videos/ActiveEncodings?DeviceId=wiifin-wii&PlaySessionId={id}
    bool deleteActiveEncoding(const std::string& serverUrl,
                              const JellyfinAuth& auth,
                              const std::string& playSessionId);

    // Search items across all libraries (Movie, Series, MusicAlbum, Audio)
    bool searchItems(const std::string& serverUrl,
                     const JellyfinAuth& auth,
                     const std::string& searchTerm,
                     int limit,
                     std::vector<JellyfinItem>& out);

    bool sslVerify = true;   // true = verify certificate (set false to allow self-signed)

private:
    bool networkReady = false;
    std::string errMsg;
    std::string localIp_;   // set by initNetwork, used by discoverServers
    std::string localMask_;

    // Low-level HTTP/HTTPS: auto-detects scheme, returns HTTP status code
    int httpRequest(const std::string& url,
                    const std::string& method,
                    const std::string& contentType,
                    const std::string& body,
                    const std::string& authToken,
                    std::string& responseBody);

    // TLS path — called by httpRequest when scheme is https://
    int httpsRequest(const std::string& host, int port,
                     const std::string& path,
                     const std::string& method,
                     const std::string& contentType,
                     const std::string& body,
                     const std::string& authToken,
                     std::string& responseBody);

    // Minimal JSON field extractor: finds first "key":"value" and returns value
    std::string jsonGetString(const std::string& json, const std::string& key);
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
