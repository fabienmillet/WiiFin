// Generated from JellyfinClient.h: each call to the backend of its server

bool JellyfinClient::authenticate(const std::string& serverUrl, const std::string& username, const std::string& password, JellyfinAuth& out) {
    return isPlex(serverUrl) ? plex_authenticate(serverUrl, username, password, out) : jf_authenticate(serverUrl, username, password, out);
}

bool JellyfinClient::deleteActiveEncoding(const std::string& serverUrl, const JellyfinAuth& auth, const std::string& playSessionId) {
    return isPlex(serverUrl) ? plex_deleteActiveEncoding(serverUrl, auth, playSessionId) : jf_deleteActiveEncoding(serverUrl, auth, playSessionId);
}

bool JellyfinClient::getAlbumTracks(const std::string& serverUrl, const JellyfinAuth& auth, const std::string& albumId, std::vector<JellyfinAudioItem>& out) {
    return isPlex(serverUrl) ? plex_getAlbumTracks(serverUrl, auth, albumId, out) : jf_getAlbumTracks(serverUrl, auth, albumId, out);
}

bool JellyfinClient::getAlbumsByArtist(const std::string& serverUrl, const JellyfinAuth& auth, const std::string& artistId, int startIndex, int limit, std::vector<JellyfinItem>& out, int& totalCount) {
    return isPlex(serverUrl) ? plex_getAlbumsByArtist(serverUrl, auth, artistId, startIndex, limit, out, totalCount) : jf_getAlbumsByArtist(serverUrl, auth, artistId, startIndex, limit, out, totalCount);
}

bool JellyfinClient::getAudioStreamUrl(const std::string& serverUrl, const JellyfinAuth& auth, const std::string& itemId, long long startTimeTicks, std::string& outUrl, std::string& outPlaySessionId, std::string* outPlayMethod, int* outAudioIndex) {
    return isPlex(serverUrl) ? plex_getAudioStreamUrl(serverUrl, auth, itemId, startTimeTicks, outUrl, outPlaySessionId, outPlayMethod, outAudioIndex) : jf_getAudioStreamUrl(serverUrl, auth, itemId, startTimeTicks, outUrl, outPlaySessionId, outPlayMethod, outAudioIndex);
}

bool JellyfinClient::getAudioTrackInfo(const std::string& serverUrl, const JellyfinAuth& auth, const std::string& itemId, AudioTrackInfo& out) {
    return isPlex(serverUrl) ? plex_getAudioTrackInfo(serverUrl, auth, itemId, out) : jf_getAudioTrackInfo(serverUrl, auth, itemId, out);
}

bool JellyfinClient::getAutoplayTracks(const std::string& serverUrl, const JellyfinAuth& auth, const std::string& itemId, const std::string& parentId, int limit, std::vector<JellyfinAudioItem>& out) {
    return isPlex(serverUrl) ? plex_getAutoplayTracks(serverUrl, auth, itemId, parentId, limit, out) : jf_getAutoplayTracks(serverUrl, auth, itemId, parentId, limit, out);
}

bool JellyfinClient::getChapterImageBytes(const std::string& serverUrl, const JellyfinAuth& auth, const std::string& itemId, int index, int maxWidth, int maxHeight, std::string& outBytes) {
    return isPlex(serverUrl) ? plex_getChapterImageBytes(serverUrl, auth, itemId, index, maxWidth, maxHeight, outBytes) : jf_getChapterImageBytes(serverUrl, auth, itemId, index, maxWidth, maxHeight, outBytes);
}

bool JellyfinClient::getChapters(const std::string& serverUrl, const JellyfinAuth& auth, const std::string& itemId, std::vector<JellyfinChapter>& out) {
    return isPlex(serverUrl) ? plex_getChapters(serverUrl, auth, itemId, out) : jf_getChapters(serverUrl, auth, itemId, out);
}

bool JellyfinClient::getContinueWatching(const std::string& serverUrl, const JellyfinAuth& auth, std::vector<JellyfinItem>& out) {
    return isPlex(serverUrl) ? plex_getContinueWatching(serverUrl, auth, out) : jf_getContinueWatching(serverUrl, auth, out);
}

bool JellyfinClient::getDirectPlayUrl(const std::string& serverUrl, const JellyfinAuth& auth, const std::string& itemId, const std::string& mediaSourceId, int audioStreamIndex, int subtitleStreamIndex, DirectPlay& out) {
    return isPlex(serverUrl) ? plex_getDirectPlayUrl(serverUrl, auth, itemId, mediaSourceId, audioStreamIndex, subtitleStreamIndex, out) : jf_getDirectPlayUrl(serverUrl, auth, itemId, embySource(serverUrl, itemId, mediaSourceId), audioStreamIndex, subtitleStreamIndex, out);
}

bool JellyfinClient::getEpisodes(const std::string& serverUrl, const JellyfinAuth& auth, const std::string& seriesId, const std::string& seasonId, std::vector<JellyfinEpisode>& out) {
    return isPlex(serverUrl) ? plex_getEpisodes(serverUrl, auth, seriesId, seasonId, out) : jf_getEpisodes(serverUrl, auth, seriesId, seasonId, out);
}

bool JellyfinClient::getFavoriteMovies(const std::string& serverUrl, const JellyfinAuth& auth, const std::string& parentId, int startIndex, int limit, std::vector<JellyfinItem>& out, int& totalCount) {
    return isPlex(serverUrl) ? plex_getFavoriteMovies(serverUrl, auth, parentId, startIndex, limit, out, totalCount) : jf_getFavoriteMovies(serverUrl, auth, parentId, startIndex, limit, out, totalCount);
}

bool JellyfinClient::getGlobalFavorites(const std::string& serverUrl, const JellyfinAuth& auth, int startIndex, int limit, std::vector<JellyfinItem>& out, int& totalCount) {
    return isPlex(serverUrl) ? plex_getGlobalFavorites(serverUrl, auth, startIndex, limit, out, totalCount) : jf_getGlobalFavorites(serverUrl, auth, startIndex, limit, out, totalCount);
}

bool JellyfinClient::getIntroTimestamps(const std::string& serverUrl, const JellyfinAuth& auth, const std::string& episodeId, IntroInfo& out) {
    return isPlex(serverUrl) ? plex_getIntroTimestamps(serverUrl, auth, episodeId, out) : jf_getIntroTimestamps(serverUrl, auth, episodeId, out);
}

bool JellyfinClient::getIsFavorite(const std::string& serverUrl, const JellyfinAuth& auth, const std::string& itemId, bool& favorite, int* specials) {
    return isPlex(serverUrl) ? plex_getIsFavorite(serverUrl, auth, itemId, favorite, specials) : jf_getIsFavorite(serverUrl, auth, itemId, favorite, specials);
}

bool JellyfinClient::getItemBackdropBytes(const std::string& serverUrl, const JellyfinAuth& auth, const JellyfinItem& item, int maxWidth, int maxHeight, std::string& outBytes) {
    return isPlex(serverUrl) ? plex_getItemBackdropBytes(serverUrl, auth, item, maxWidth, maxHeight, outBytes) : jf_getItemBackdropBytes(serverUrl, auth, item, maxWidth, maxHeight, outBytes);
}

bool JellyfinClient::getItemDetail(const std::string& serverUrl, const JellyfinAuth& auth, const std::string& itemId, JellyfinItemDetail& out) {
    return isPlex(serverUrl) ? plex_getItemDetail(serverUrl, auth, itemId, out) : jf_getItemDetail(serverUrl, auth, itemId, out);
}

bool JellyfinClient::getItemImageBytes(const std::string& serverUrl, const JellyfinAuth& auth, const std::string& itemId, int maxWidth, int maxHeight, std::string& outBytes) {
    return isPlex(serverUrl) ? plex_getItemImageBytes(serverUrl, auth, itemId, maxWidth, maxHeight, outBytes) : jf_getItemImageBytes(serverUrl, auth, itemId, maxWidth, maxHeight, outBytes);
}

bool JellyfinClient::getItems(const std::string& serverUrl, const JellyfinAuth& auth, const std::string& parentId, int startIndex, int limit, std::vector<JellyfinItem>& out, int& totalCount, const std::string& extra) {
    return isPlex(serverUrl) ? plex_getItems(serverUrl, auth, parentId, startIndex, limit, out, totalCount, extra) : jf_getItems(serverUrl, auth, parentId, startIndex, limit, out, totalCount, extra);
}

bool JellyfinClient::getItemsByIds(const std::string& serverUrl, const JellyfinAuth& auth, const std::vector<std::string>& ids, std::vector<JellyfinItem>& outItems, std::vector<JellyfinAudioItem>& outAudio) {
    return isPlex(serverUrl) ? plex_getItemsByIds(serverUrl, auth, ids, outItems, outAudio) : jf_getItemsByIds(serverUrl, auth, ids, outItems, outAudio);
}

bool JellyfinClient::getItemsByQuery(const std::string& serverUrl, const JellyfinAuth& auth, const std::string& pathAndQuery, std::vector<JellyfinItem>& out, int* totalCount) {
    return isPlex(serverUrl) ? plex_getItemsByQuery(serverUrl, auth, pathAndQuery, out, totalCount) : jf_getItemsByQuery(serverUrl, auth, pathAndQuery, out, totalCount);
}

bool JellyfinClient::getLibraries(const std::string& serverUrl, const JellyfinAuth& auth, std::vector<JellyfinLibrary>& out) {
    return isPlex(serverUrl) ? plex_getLibraries(serverUrl, auth, out) : jf_getLibraries(serverUrl, auth, out);
}

bool JellyfinClient::getLyrics(const std::string& serverUrl, const JellyfinAuth& auth, const std::string& itemId, std::vector<LyricLine>& out) {
    return isPlex(serverUrl) ? plex_getLyrics(serverUrl, auth, itemId, out) : jf_getLyrics(serverUrl, auth, itemId, out);
}

bool JellyfinClient::getMovieCollections(const std::string& serverUrl, const JellyfinAuth& auth, const std::string& parentId, int startIndex, int limit, std::vector<JellyfinItem>& out, int& totalCount) {
    return isPlex(serverUrl) ? plex_getMovieCollections(serverUrl, auth, parentId, startIndex, limit, out, totalCount) : jf_getMovieCollections(serverUrl, auth, parentId, startIndex, limit, out, totalCount);
}

bool JellyfinClient::getMovieContinueWatching(const std::string& serverUrl, const JellyfinAuth& auth, std::vector<JellyfinItem>& out) {
    return isPlex(serverUrl) ? plex_getMovieContinueWatching(serverUrl, auth, out) : jf_getMovieContinueWatching(serverUrl, auth, out);
}

bool JellyfinClient::getMoviesLatest(const std::string& serverUrl, const JellyfinAuth& auth, const std::string& parentId, int limit, std::vector<JellyfinItem>& out) {
    return isPlex(serverUrl) ? plex_getMoviesLatest(serverUrl, auth, parentId, limit, out) : jf_getMoviesLatest(serverUrl, auth, parentId, limit, out);
}

bool JellyfinClient::getMusicLatest(const std::string& serverUrl, const JellyfinAuth& auth, const std::string& parentId, int limit, std::vector<JellyfinItem>& out) {
    return isPlex(serverUrl) ? plex_getMusicLatest(serverUrl, auth, parentId, limit, out) : jf_getMusicLatest(serverUrl, auth, parentId, limit, out);
}

bool JellyfinClient::getNextUp(const std::string& serverUrl, const JellyfinAuth& auth, std::vector<JellyfinItem>& out) {
    return isPlex(serverUrl) ? plex_getNextUp(serverUrl, auth, out) : jf_getNextUp(serverUrl, auth, out);
}

bool JellyfinClient::getPhotoBytes(const std::string& serverUrl, const JellyfinAuth& auth, const std::string& itemId, int maxWidth, int maxHeight, std::string& outBytes) {
    return isPlex(serverUrl) ? plex_getPhotoBytes(serverUrl, auth, itemId, maxWidth, maxHeight, outBytes) : jf_getPhotoBytes(serverUrl, auth, itemId, maxWidth, maxHeight, outBytes);
}

bool JellyfinClient::getPlaybackUrl(const std::string& serverUrl, const JellyfinAuth& auth, const std::string& itemId, const std::string& mediaSourceId, int audioStreamIndex, int subtitleStreamIndex, long long startTimeTicks, std::string& outUrl, std::string& outPlaySessionId, PlaybackChoice& how, bool allowDirect) {
    return isPlex(serverUrl) ? plex_getPlaybackUrl(serverUrl, auth, itemId, mediaSourceId, audioStreamIndex, subtitleStreamIndex, startTimeTicks, outUrl, outPlaySessionId, how, allowDirect) : jf_getPlaybackUrl(serverUrl, auth, itemId, embySource(serverUrl, itemId, mediaSourceId), audioStreamIndex, subtitleStreamIndex, startTimeTicks, outUrl, outPlaySessionId, how, allowDirect);
}

bool JellyfinClient::getPlaylistTracks(const std::string& serverUrl, const JellyfinAuth& auth, const std::string& playlistId, std::vector<JellyfinAudioItem>& out) {
    return isPlex(serverUrl) ? plex_getPlaylistTracks(serverUrl, auth, playlistId, out) : jf_getPlaylistTracks(serverUrl, auth, playlistId, out);
}

bool JellyfinClient::getPlaylists(const std::string& serverUrl, const JellyfinAuth& auth, int startIndex, int limit, std::vector<JellyfinItem>& out, int& totalCount) {
    return isPlex(serverUrl) ? plex_getPlaylists(serverUrl, auth, startIndex, limit, out, totalCount) : jf_getPlaylists(serverUrl, auth, startIndex, limit, out, totalCount);
}

bool JellyfinClient::getSeasons(const std::string& serverUrl, const JellyfinAuth& auth, const std::string& seriesId, std::vector<JellyfinSeason>& out) {
    return isPlex(serverUrl) ? plex_getSeasons(serverUrl, auth, seriesId, out) : jf_getSeasons(serverUrl, auth, seriesId, out);
}

bool JellyfinClient::getSeriesEpisodes(const std::string& serverUrl, const JellyfinAuth& auth, const std::string& seriesId, std::vector<JellyfinEpisode>& out) {
    return isPlex(serverUrl) ? plex_getSeriesEpisodes(serverUrl, auth, seriesId, out) : jf_getSeriesEpisodes(serverUrl, auth, seriesId, out);
}

bool JellyfinClient::getServerName(const std::string& serverUrl, const JellyfinAuth& auth, std::string& outName) {
    return isPlex(serverUrl) ? plex_getServerName(serverUrl, auth, outName) : jf_getServerName(serverUrl, auth, outName);
}

bool JellyfinClient::getShuffledEpisodes(const std::string& serverUrl, const JellyfinAuth& auth, const std::string& seriesId, const std::string& seasonId, int limit, std::vector<JellyfinEpisode>& out) {
    return isPlex(serverUrl) ? plex_getShuffledEpisodes(serverUrl, auth, seriesId, seasonId, limit, out) : jf_getShuffledEpisodes(serverUrl, auth, seriesId, seasonId, limit, out);
}

bool JellyfinClient::getSpecialFeatures(const std::string& serverUrl, const JellyfinAuth& auth, const std::string& itemId, std::vector<JellyfinItem>& out) {
    return isPlex(serverUrl) ? plex_getSpecialFeatures(serverUrl, auth, itemId, out) : jf_getSpecialFeatures(serverUrl, auth, itemId, out);
}

bool JellyfinClient::getSubtitleSrt(const std::string& serverUrl, const JellyfinAuth& auth, const std::string& itemId, const std::string& mediaSourceId, int streamIndex, std::string& outSrt) {
    return isPlex(serverUrl) ? plex_getSubtitleSrt(serverUrl, auth, itemId, mediaSourceId, streamIndex, outSrt) : jf_getSubtitleSrt(serverUrl, auth, itemId, embySource(serverUrl, itemId, mediaSourceId), streamIndex, outSrt);
}

bool JellyfinClient::getTVContinueWatching(const std::string& serverUrl, const JellyfinAuth& auth, std::vector<JellyfinItem>& out) {
    return isPlex(serverUrl) ? plex_getTVContinueWatching(serverUrl, auth, out) : jf_getTVContinueWatching(serverUrl, auth, out);
}

bool JellyfinClient::getTVSeriesLatest(const std::string& serverUrl, const JellyfinAuth& auth, const std::string& parentId, int limit, std::vector<JellyfinItem>& out) {
    return isPlex(serverUrl) ? plex_getTVSeriesLatest(serverUrl, auth, parentId, limit, out) : jf_getTVSeriesLatest(serverUrl, auth, parentId, limit, out);
}

bool JellyfinClient::getTVUpcoming(const std::string& serverUrl, const JellyfinAuth& auth, int limit, std::vector<JellyfinItem>& out) {
    return isPlex(serverUrl) ? plex_getTVUpcoming(serverUrl, auth, limit, out) : jf_getTVUpcoming(serverUrl, auth, limit, out);
}

bool JellyfinClient::getTranscodingUrl(const std::string& serverUrl, const JellyfinAuth& auth, const std::string& itemId, const std::string& mediaSourceId, int audioStreamIndex, int subtitleStreamIndex, long long startTimeTicks, std::string& outUrl, std::string& outPlaySessionId, const std::string& transcodeReasons) {
    return isPlex(serverUrl) ? plex_getTranscodingUrl(serverUrl, auth, itemId, mediaSourceId, audioStreamIndex, subtitleStreamIndex, startTimeTicks, outUrl, outPlaySessionId, transcodeReasons) : jf_getTranscodingUrl(serverUrl, auth, itemId, embySource(serverUrl, itemId, mediaSourceId), audioStreamIndex, subtitleStreamIndex, startTimeTicks, outUrl, outPlaySessionId, transcodeReasons);
}

bool JellyfinClient::getTrickplayInfo(const std::string& serverUrl, const JellyfinAuth& auth, const std::string& itemId, const std::string& mediaSourceId, TrickplayInfo& out) {
    return isPlex(serverUrl) ? plex_getTrickplayInfo(serverUrl, auth, itemId, mediaSourceId, out) : jf_getTrickplayInfo(serverUrl, auth, itemId, embySource(serverUrl, itemId, mediaSourceId), out);
}

bool JellyfinClient::getTrickplayTile(const std::string& serverUrl, const JellyfinAuth& auth, const std::string& itemId, const std::string& mediaSourceId, int width, int index, std::string& outBytes) {
    return isPlex(serverUrl) ? plex_getTrickplayTile(serverUrl, auth, itemId, mediaSourceId, width, index, outBytes) : jf_getTrickplayTile(serverUrl, auth, itemId, embySource(serverUrl, itemId, mediaSourceId), width, index, outBytes);
}

void JellyfinClient::logServerInfo(const std::string& serverUrl) {
    return isPlex(serverUrl) ? plex_logServerInfo(serverUrl) : jf_logServerInfo(serverUrl);
}

void JellyfinClient::logTranscodeFailure(const std::string& serverUrl, const JellyfinAuth& auth) {
    return isPlex(serverUrl) ? plex_logTranscodeFailure(serverUrl, auth) : jf_logTranscodeFailure(serverUrl, auth);
}

bool JellyfinClient::postCapabilities(const std::string& serverUrl, const JellyfinAuth& auth) {
    return isPlex(serverUrl) ? plex_postCapabilities(serverUrl, auth) : jf_postCapabilities(serverUrl, auth);
}

bool JellyfinClient::quickConnectAuthenticate(const std::string& serverUrl, const std::string& secret, JellyfinAuth& out) {
    return isPlex(serverUrl) ? plex_quickConnectAuthenticate(serverUrl, secret, out) : jf_quickConnectAuthenticate(serverUrl, secret, out);
}

bool JellyfinClient::quickConnectCheck(const std::string& serverUrl, const std::string& secret, QuickConnectResult& out) {
    return isPlex(serverUrl) ? plex_quickConnectCheck(serverUrl, secret, out) : jf_quickConnectCheck(serverUrl, secret, out);
}

bool JellyfinClient::quickConnectInitiate(const std::string& serverUrl, QuickConnectResult& out) {
    return isPlex(serverUrl) ? plex_quickConnectInitiate(serverUrl, out) : jf_quickConnectInitiate(serverUrl, out);
}

bool JellyfinClient::reportPlaybackProgress(const std::string& serverUrl, const JellyfinAuth& auth, const std::string& itemId, const std::string& mediaSourceId, const std::string& playSessionId, long long positionTicks, bool isPaused, const char* playMethod, int audioStreamIndex, int subtitleStreamIndex) {
    return isPlex(serverUrl) ? plex_reportPlaybackProgress(serverUrl, auth, itemId, mediaSourceId, playSessionId, positionTicks, isPaused, playMethod, audioStreamIndex, subtitleStreamIndex) : jf_reportPlaybackProgress(serverUrl, auth, itemId, embySource(serverUrl, itemId, mediaSourceId), playSessionId, positionTicks, isPaused, playMethod, audioStreamIndex, subtitleStreamIndex);
}

bool JellyfinClient::reportPlaybackStart(const std::string& serverUrl, const JellyfinAuth& auth, const std::string& itemId, const std::string& mediaSourceId, const std::string& playSessionId, const char* playMethod, int audioStreamIndex, int subtitleStreamIndex) {
    return isPlex(serverUrl) ? plex_reportPlaybackStart(serverUrl, auth, itemId, mediaSourceId, playSessionId, playMethod, audioStreamIndex, subtitleStreamIndex) : jf_reportPlaybackStart(serverUrl, auth, itemId, embySource(serverUrl, itemId, mediaSourceId), playSessionId, playMethod, audioStreamIndex, subtitleStreamIndex);
}

bool JellyfinClient::reportPlaybackStopped(const std::string& serverUrl, const JellyfinAuth& auth, const std::string& itemId, const std::string& mediaSourceId, const std::string& playSessionId, long long positionTicks) {
    return isPlex(serverUrl) ? plex_reportPlaybackStopped(serverUrl, auth, itemId, mediaSourceId, playSessionId, positionTicks) : jf_reportPlaybackStopped(serverUrl, auth, itemId, embySource(serverUrl, itemId, mediaSourceId), playSessionId, positionTicks);
}

bool JellyfinClient::searchItems(const std::string& serverUrl, const JellyfinAuth& auth, const std::string& searchTerm, int limit, std::vector<JellyfinItem>& out) {
    return isPlex(serverUrl) ? plex_searchItems(serverUrl, auth, searchTerm, limit, out) : jf_searchItems(serverUrl, auth, searchTerm, limit, out);
}

bool JellyfinClient::setFavorite(const std::string& serverUrl, const JellyfinAuth& auth, const std::string& itemId, bool favorite) {
    return isPlex(serverUrl) ? plex_setFavorite(serverUrl, auth, itemId, favorite) : jf_setFavorite(serverUrl, auth, itemId, favorite);
}

bool JellyfinClient::setPlayed(const std::string& serverUrl, const JellyfinAuth& auth, const std::string& itemId, bool played) {
    return isPlex(serverUrl) ? plex_setPlayed(serverUrl, auth, itemId, played) : jf_setPlayed(serverUrl, auth, itemId, played);
}

void JellyfinClient::measureLink(const std::string& serverUrl, const JellyfinAuth& auth) {
    return isPlex(serverUrl) ? plex_measureLink(serverUrl, auth) : jf_measureLink(serverUrl, auth);
}
