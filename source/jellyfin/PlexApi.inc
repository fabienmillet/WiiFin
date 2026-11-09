// ---------------------------------------------------------------------------
// WiiPlex: the Plex Media Server API behind JellyfinClient's interface.
//
// Included at the end of JellyfinClient.cpp, so it shares the HTTP/TLS stack
// and the JSON helpers.  The UI of WiiFin still speaks in Jellyfin's terms:
// item types ("Movie", "Series"...), ticks of 100 ns, and for lists Jellyfin
// queries ("/Users/<id>/Items?ParentId=...&IncludeItemTypes=Movie&SortBy=...").
// Those are translated here to Plex's endpoints (plexPathFor).
//
// Ids: an item is its Plex ratingKey ("14"); a library is "s" + its section
// key ("s4"), so the two can never be confused.
// ---------------------------------------------------------------------------

static int plexBitrate(int q);   /* below */

namespace {

const long long TICKS_PER_MS = 10000LL;

std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

/* value of name in a query string ("a=1&b=2"), "" when absent */
std::string queryParam(const std::string& q, const char* name) {
    std::string k = std::string(name) + "=";
    size_t p = 0;
    while ((p = q.find(k, p)) != std::string::npos) {
        if (p == 0 || q[p - 1] == '&' || q[p - 1] == '?') {
            size_t e = q.find('&', p);
            return q.substr(p + k.size(), (e == std::string::npos ? q.size() : e) - p - k.size());
        }
        p += k.size();
    }
    return "";
}

bool isSection(const std::string& id) { return id.size() > 1 && id[0] == 's'; }
std::string sectionKey(const std::string& id) { return id.substr(1); }

/* Plex's type of an item, in Jellyfin's words */
std::string jfType(const std::string& t) {
    if (t == "movie")      return "Movie";
    if (t == "show")       return "Series";
    if (t == "season")     return "Season";
    if (t == "episode")    return "Episode";
    if (t == "artist")     return "MusicArtist";
    if (t == "album")      return "MusicAlbum";
    if (t == "track")      return "Audio";
    if (t == "photo")      return "Photo";
    if (t == "collection") return "BoxSet";
    if (t == "playlist")   return "Playlist";
    if (t == "clip")       return "Video";
    if (t == "genre")      return "Genre";
    return "Folder";
}

/* Jellyfin's IncludeItemTypes (the first one) as Plex's type number */
int plexTypeNumber(const std::string& types) {
    std::string t = types.substr(0, types.find(','));
    if (t == "Movie")       return 1;
    if (t == "Series")      return 2;
    if (t == "Season")      return 3;
    if (t == "Episode")     return 4;
    if (t == "MusicArtist") return 8;
    if (t == "MusicAlbum")  return 9;
    if (t == "Audio")       return 10;
    if (t == "Photo")       return 13;
    return 0;
}

/* Jellyfin's SortBy (the first key) and SortOrder as Plex's sort= */
std::string plexSort(const std::string& sortBy, const std::string& order) {
    std::string k = sortBy.substr(0, sortBy.find(','));
    if (k.empty()) return "";
    std::string s;
    if      (k == "SortName" || k == "Name")                    s = "titleSort";
    else if (k == "ProductionYear" || k == "PremiereDate")      s = "originallyAvailableAt";
    else if (k == "DateCreated")                                s = "addedAt";
    else if (k == "CommunityRating" || k == "CriticRating")     s = "audienceRating";
    else if (k == "DatePlayed")                                 s = "lastViewedAt";
    else if (k == "PlayCount")                                  s = "viewCount";
    else if (k == "Runtime")                                    s = "duration";
    else if (k == "Random")                                     return "random";
    else if (k == "Album")                                      s = "album.titleSort";
    else if (k == "AlbumArtist" || k == "Artist")               s = "artist.titleSort";
    else return "";
    if (order.find("Descending") == 0) s += ":desc";
    return s;
}

/* Every object of every "<key>":[...] array of a Plex answer ("Metadata" in
 * a library list, in each Hub of a search...) */
void forEachPlexObject(const std::string& json, const char* key,
                       void (*cb)(const std::string&, void*), void* ctx) {
    std::string k = std::string("\"") + key + "\":[";
    size_t p = 0;
    while ((p = json.find(k, p)) != std::string::npos) {
        size_t open = p + k.size() - 1;
        forEachArrayObject(json, open, cb, ctx);
        p = open + 1;
    }
}

/* the object of a top-level child array ("Media":[{...}]), as a string, by
 * position: n = 0 the first */
std::string nthObject(const std::string& json, const char* key, int n) {
    std::string k = std::string("\"") + key + "\":[";
    size_t p = json.find(k);
    if (p == std::string::npos) return "";
    struct Pick { int want, at; std::string got; };
    Pick pick{ n, 0, "" };
    forEachArrayObject(json, p + k.size() - 1, [](const std::string& o, void* v) {
        Pick* pk = static_cast<Pick*>(v);
        if (pk->at++ == pk->want) pk->got = o;
    }, &pick);
    return pick.got;
}

std::string lower(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

} // namespace

// ---------------------------------------------------------------------------
// One Plex item → JellyfinItem
// ---------------------------------------------------------------------------
static void plexToItem(JellyfinClient* c, const std::string& o, JellyfinItem& it);

/* The JSON helpers are members; this friend-less adapter keeps the parsing
 * in one place. */
struct PlexParse {
    JellyfinClient* self;
    std::string s(const std::string& o, const char* k) { return self->plexStr(o, k); }
    int i(const std::string& o, const char* k)         { return self->plexInt(o, k); }
    long long ll(const std::string& o, const char* k)  { return self->plexLL(o, k); }
};

std::string JellyfinClient::plexStr(const std::string& o, const char* k) { return jsonGetString(o, k); }
int JellyfinClient::plexInt(const std::string& o, const char* k) { return jsonGetInt(o, k); }
long long JellyfinClient::plexLL(const std::string& o, const char* k) { return jsonGetLongLong(o, k); }

static void plexToItem(JellyfinClient* c, const std::string& o, JellyfinItem& it) {
    PlexParse p{ c };
    std::string t = p.s(o, "type");
    it.id   = p.s(o, "ratingKey");
    if (it.id.empty() && (t.empty() || t == "genre")) {   /* a Directory: a genre, its key */
        it.id = p.s(o, "key");
        it.type = "Genre";
    } else {
        it.type = jfType(t);
    }
    it.name = p.s(o, "title");
    it.year = p.i(o, "year");
    it.playbackPositionTicks = p.ll(o, "viewOffset") * TICKS_PER_MS;
    it.runtimeTicks          = p.ll(o, "duration") * TICKS_PER_MS;
    if (t == "episode") {
        it.seriesName    = p.s(o, "grandparentTitle");
        it.seriesId      = p.s(o, "grandparentRatingKey");
        it.seasonNumber  = p.i(o, "parentIndex");
        it.episodeNumber = p.i(o, "index");
    } else if (t == "season") {
        it.seriesName = p.s(o, "parentTitle");
        it.seriesId   = p.s(o, "parentRatingKey");
        it.seasonNumber = p.i(o, "index");
    }
    {
        size_t a = o.find("\"audienceRating\":");
        if (a == std::string::npos) a = o.find("\"rating\":");
        if (a != std::string::npos) it.communityRating = (float)atof(o.c_str() + o.find(':', a) + 1);
    }
    it.officialRating = p.s(o, "contentRating");
    it.childCount = p.i(o, "childCount");
    it.recursiveItemCount = p.i(o, "leafCount");
    it.sortName = p.s(o, "titleSort");
    if (it.sortName.empty()) it.sortName = lower(it.name);
    it.extraType = p.s(o, "subtype");   /* an extra: "trailer", "featurette"... */
    if (t == "album")      it.artist = p.s(o, "parentTitle");
    else if (t == "track") { it.artist = p.s(o, "originalTitle"); if (it.artist.empty()) it.artist = p.s(o, "grandparentTitle"); }
    if (t == "show" || t == "season") {
        int leaves = p.i(o, "leafCount");
        it.played = leaves > 0 && p.i(o, "viewedLeafCount") >= leaves;
    } else {
        it.played = p.i(o, "viewCount") > 0;
    }
}

static void plexToAudio(JellyfinClient* c, const std::string& o, JellyfinAudioItem& a) {
    PlexParse p{ c };
    a.id     = p.s(o, "ratingKey");
    a.name   = p.s(o, "title");
    a.artist = p.s(o, "originalTitle");
    if (a.artist.empty()) a.artist = p.s(o, "grandparentTitle");
    a.album  = p.s(o, "parentTitle");
    a.trackNumber  = p.i(o, "index");
    a.runtimeTicks = p.ll(o, "duration") * TICKS_PER_MS;
    a.playbackPositionTicks = p.ll(o, "viewOffset") * TICKS_PER_MS;
    std::string t = p.s(o, "type");
    a.video = !t.empty() && t != "track";
}

// ---------------------------------------------------------------------------
// GET a Plex path (with the token in the header) into items
// ---------------------------------------------------------------------------
bool JellyfinClient::plexGet(const std::string& serverUrl, const JellyfinAuth& auth,
                             const std::string& path, std::string& resp, size_t maxResponse) {
    int status = httpRequest(serverUrl + path, "GET", "", "", auth.accessToken, resp, maxResponse);
    if (status == 401) tokenRefused = true;
    if (status != 200) {
        if (status >= 0) errMsg = "Plex HTTP " + std::to_string(status);
        return false;
    }
    return true;
}

bool JellyfinClient::plexItems(const std::string& serverUrl, const JellyfinAuth& auth,
                               const std::string& path, std::vector<JellyfinItem>& out,
                               int* totalCount, const std::string& onlyType) {
    /* a list needs none of the files, people and long texts Plex sends
     * with each item: half the size (a big library's page went past the
     * 256 KB a response may take) */
    std::string p = path + (path.find('?') == std::string::npos ? "?" : "&") +
        "excludeElements=Media,Genre,Role,Director,Writer,Producer,Country,Similar,Image,UltraBlurColors,Guid,Rating,Collection,Label"
        "&excludeFields=summary,tagline,file";
    std::string resp;
    if (!plexGet(serverUrl, auth, p, resp, 1u << 20)) return false;
    struct Ctx { JellyfinClient* self; std::vector<JellyfinItem>* out; const std::string* only; };
    Ctx ctx{ this, &out, &onlyType };
    auto cb = [](const std::string& o, void* v) {
        Ctx* c = static_cast<Ctx*>(v);
        JellyfinItem it;
        plexToItem(c->self, o, it);
        if (it.id.empty() || it.name.empty()) return;
        if (!c->only->empty() && c->only->find(it.type) == std::string::npos) return;
        c->out->push_back(it);
    };
    forEachPlexObject(resp, "Metadata", cb, &ctx);
    if (out.empty()) forEachPlexObject(resp, "Directory", cb, &ctx);
    if (totalCount) {
        size_t m = resp.find("\"MediaContainer\":");
        std::string head = m == std::string::npos ? resp : resp.substr(m, 400);
        int total = jsonGetInt(head, "totalSize");
        *totalCount = total > 0 ? total : (int)out.size();
    }
    return true;
}

// ---------------------------------------------------------------------------
// A Jellyfin query of the UI → a Plex path (+ a type filter applied after)
// ---------------------------------------------------------------------------
std::string JellyfinClient::plexPathFor(const std::string& jf, std::string& onlyType, bool& none) {
    none = false;
    onlyType.clear();
    size_t qm = jf.find('?');
    std::string path = jf.substr(0, qm);
    std::string q = qm == std::string::npos ? "" : jf.substr(qm + 1);
    std::string parent = queryParam(q, "ParentId");
    std::string types  = queryParam(q, "IncludeItemTypes");
    std::string filters = queryParam(q, "Filters");
    std::string start = queryParam(q, "StartIndex"), limit = queryParam(q, "Limit");
    std::string paging;
    if (!start.empty()) paging += "&X-Plex-Container-Start=" + start;
    if (!limit.empty()) paging += "&X-Plex-Container-Size=" + limit;
    std::string sort = plexSort(queryParam(q, "SortBy"), queryParam(q, "SortOrder"));
    if (!types.empty()) {
        /* "Movie,Series" → "Movie,Series" kept for the check after */
        onlyType = types;
        for (size_t p = 0; (p = onlyType.find("%2C", p)) != std::string::npos; ) onlyType.replace(p, 3, ",");
    }

    /* what Plex has no notion of */
    if (path.find("/Shows/Upcoming") != std::string::npos) { none = true; return ""; }
    if (filters.find("IsFavorite") != std::string::npos) {
        /* favourites: the items the user rated 10 (see setFavorite) */
        if (isSection(parent))
            return "/library/sections/" + sectionKey(parent) + "/all?userRating%3E%3E=10" +
                   (plexTypeNumber(types) ? "&type=" + std::to_string(plexTypeNumber(types)) : "") + paging;
        none = true;   /* across all libraries: not in one request */
        return "";
    }
    /* Continue watching / next up */
    if (path.find("/Items/Resume") != std::string::npos || path.find("/Shows/NextUp") != std::string::npos ||
        filters.find("IsResumable") != std::string::npos) {
        if (path.find("/Shows/NextUp") != std::string::npos) onlyType = "Episode";
        return (isSection(parent) ? "/library/sections/" + sectionKey(parent) + "/onDeck" : std::string("/library/onDeck")) +
               "?X-Plex-Container-Start=0" + (limit.empty() ? "" : "&X-Plex-Container-Size=" + limit);
    }
    if (path.find("/Items/Latest") != std::string::npos) {
        std::string lim = limit.empty() ? "16" : limit;
        if (isSection(parent))
            return "/library/sections/" + sectionKey(parent) + "/recentlyAdded?X-Plex-Container-Start=0&X-Plex-Container-Size=" + lim;
        return "/library/recentlyAdded?X-Plex-Container-Start=0&X-Plex-Container-Size=" + lim;
    }
    if (path.find("/Genres") != std::string::npos) {
        onlyType.clear();
        if (isSection(parent)) return "/library/sections/" + sectionKey(parent) + "/genre?" + paging.substr(paging.empty() ? 0 : 1);
        none = true; return "";
    }
    std::string search = queryParam(q, "SearchTerm");
    if (!search.empty()) return "/hubs/search?query=" + search + "&limit=" + (limit.empty() ? "20" : limit);

    std::string ids = queryParam(q, "Ids");
    if (!ids.empty()) {
        for (size_t p = 0; (p = ids.find("%2C", p)) != std::string::npos; ) ids.replace(p, 3, ",");
        return "/library/metadata/" + ids;
    }
    if (types.find("Playlist") == 0) {
        std::string mt = queryParam(q, "MediaTypes");
        return std::string("/playlists?") + (mt == "Audio" ? "playlistType=audio" : "playlistType=video") + paging;
    }
    if (types.find("BoxSet") == 0) {
        onlyType = "BoxSet";
        if (isSection(parent)) return "/library/sections/" + sectionKey(parent) + "/collections?" + paging.substr(paging.empty() ? 0 : 1);
        none = true; return "";
    }
    std::string artist = queryParam(q, "AlbumArtistIds");
    if (!artist.empty()) return "/library/metadata/" + artist + "/children?X-Plex-Container-Start=0" + paging;

    std::string extra;
    if (filters.find("IsUnplayed") != std::string::npos) extra += "&unwatched=1";
    std::string genre = queryParam(q, "GenreIds");
    if (!genre.empty()) extra += "&genre=" + genre;
    if (!sort.empty()) extra += "&sort=" + sort;

    if (isSection(parent)) {
        int tn = plexTypeNumber(types);
        return "/library/sections/" + sectionKey(parent) + "/all?includeCollections=0" +
               (tn ? "&type=" + std::to_string(tn) : "") + extra + paging;
    }
    if (!parent.empty()) {
        /* a series' every episode, or the children of an item */
        bool leaves = queryParam(q, "Recursive") == "true" && (types == "Episode" || types == "Audio");
        return "/library/metadata/" + parent + (leaves ? "/allLeaves" : "/children") + "?includeCollections=0" + extra + paging;
    }
    none = true;   /* every library at once: not with Plex */
    return "";
}

bool JellyfinClient::plex_getItemsByQuery(const std::string& serverUrl,
                                     const JellyfinAuth& auth,
                                     const std::string& pathAndQuery,
                                     std::vector<JellyfinItem>& out,
                                     int* totalCount) {
    std::string only;
    bool none = false;
    std::string p = plexPathFor(pathAndQuery, only, none);
    if (totalCount) *totalCount = 0;
    if (none) return true;
    SYS_Report("[Plex] %s\n", p.substr(0, p.find('?')).c_str());
    /* Plex sends the music library's own folders as Directory: a genre list
     * is made of them, nothing else */
    return plexItems(serverUrl, auth, p, out, totalCount, only);
}

bool JellyfinClient::plex_getItems(const std::string& serverUrl, const JellyfinAuth& auth,
                              const std::string& parentId, int startIndex, int limit,
                              std::vector<JellyfinItem>& out, int& totalCount,
                              const std::string& extra) {
    char q[160];
    snprintf(q, sizeof(q), "/Users/x/Items?ParentId=%s&StartIndex=%d&Limit=%d",
             parentId.c_str(), startIndex, limit);
    std::string jf = std::string(q) + extra;
    if (extra.find("SortBy=") == std::string::npos) jf += "&SortBy=SortName&SortOrder=Ascending";
    return getItemsByQuery(serverUrl, auth, jf, out, &totalCount);
}

bool JellyfinClient::plex_getAlbumsByArtist(const std::string& serverUrl, const JellyfinAuth& auth,
                                       const std::string& artistId, int startIndex, int limit,
                                       std::vector<JellyfinItem>& out, int& totalCount) {
    char q[200];
    snprintf(q, sizeof(q), "/library/metadata/%s/children?X-Plex-Container-Start=%d&X-Plex-Container-Size=%d",
             artistId.c_str(), startIndex, limit);
    return plexItems(serverUrl, auth, q, out, &totalCount, "MusicAlbum");
}

// ---------------------------------------------------------------------------
// Sign-in: the server alone on a trusted network, a token, or a plex.tv
// account (username + password); "Quick Connect" is plex.tv/link's PIN.
// ---------------------------------------------------------------------------
static const char* PLEX_TV = "https://plex.tv";

/* The account's server (its own first, then one shared with it) and the
 * address the Wii reaches it at: a local one on the Wii's own network, else
 * the remote one (https://<ip>.<id>.plex.direct:32400).  The token is the
 * server's own (a shared server takes no other).  Addresses on other
 * networks (Docker's...) are not tried: each would wait out a timeout. */
bool JellyfinClient::plexFindServer(JellyfinAuth& out) {
    std::string resp;
    int st = httpRequest(std::string(PLEX_TV) + "/api/v2/resources?includeHttps=1&includeRelay=0", "GET", "", "",
                         out.accessToken, resp, 1u << 20);
    if (st != 200) { errMsg = "plex.tv resources: HTTP " + std::to_string(st); return false; }
    std::string net = localIp_.substr(0, localIp_.rfind('.') + 1);   /* "192.168.1." */
    struct Server { std::string name, token; bool owned; std::vector<std::string> local, remote; };
    struct Ctx { JellyfinClient* self; std::vector<Server>* list; const std::string* net; };
    std::vector<Server> list;
    Ctx ctx{ this, &list, &net };
    size_t p = resp.find('[');
    if (p != std::string::npos)
        forEachArrayObject(resp, p, [](const std::string& r, void* v) {
            Ctx* c = static_cast<Ctx*>(v);
            if (c->self->plexStr(r, "provides").find("server") == std::string::npos) return;
            Server s;
            s.name  = c->self->plexStr(r, "name");
            s.token = c->self->plexStr(r, "accessToken");
            s.owned = r.find("\"owned\":true") != std::string::npos;
            struct C2 { Server* s; JellyfinClient* self; const std::string* net; };
            C2 c2{ &s, c->self, c->net };
            size_t q = r.find("\"connections\":[");
            if (q != std::string::npos)
                forEachArrayObject(r, q + 14, [](const std::string& k, void* v2) {
                    C2* x = static_cast<C2*>(v2);
                    std::string uri = x->self->plexStr(k, "uri"), addr = x->self->plexStr(k, "address");
                    if (uri.empty() || k.find("\"relay\":true") != std::string::npos) return;
                    if (k.find("\"local\":true") != std::string::npos) {
                        if (!x->net->empty() && addr.compare(0, x->net->size(), *x->net) == 0) x->s->local.push_back(uri);
                    } else x->s->remote.push_back(uri);
                }, &c2);
            c->list->push_back(s);
        }, &ctx);
    std::stable_sort(list.begin(), list.end(), [](const Server& a, const Server& b) { return a.owned > b.owned; });
    for (const Server& s : list) {
        std::vector<std::string> tries = s.local;
        tries.insert(tries.end(), s.remote.begin(), s.remote.end());
        for (const std::string& uri : tries) {
            JellyfinAuth a = out;
            if (!s.token.empty()) a.accessToken = s.token;
            SYS_Report("[Plex] trying %s (%s)\n", s.name.c_str(), uri.find("plex.direct") != std::string::npos ? "plex.direct" : "local");
            if (plexCheckServer(uri, a)) {
                a.serverUrl = uri;
                out = a;
                return true;
            }
        }
    }
    errMsg = list.empty() ? N_("No Plex server on this account") : N_("No Plex server answers");
    return false;
}

bool JellyfinClient::plexCheckServer(const std::string& serverUrl, JellyfinAuth& out) {
    std::string resp;
    int status = httpRequest(serverUrl + "/library/sections", "GET", "", "", out.accessToken, resp);
    if (status != 200) {
        if (status == 401) errMsg = N_("The server asks for a Plex account");
        else if (status >= 0) errMsg = "Plex HTTP " + std::to_string(status);
        return false;
    }
    std::string ident;
    if (httpRequest(serverUrl + "/", "GET", "", "", out.accessToken, ident) == 200)
        out.serverName = jsonGetString(ident, "friendlyName");
    if (out.serverName.empty()) out.serverName = "Plex";
    if (out.userId.empty()) out.userId = "plex";
    return true;
}

/* the plex.tv account of out.accessToken: its name and id (the profile's) */
void JellyfinClient::plexAccountUser(JellyfinAuth& out) {
    std::string resp;
    int st = httpRequest(std::string(PLEX_TV) + "/api/v2/user", "GET", "", "", out.accessToken, resp);
    SYS_Report("[Plex] account: HTTP %d, %s\n", st, jsonGetString(resp, "username").empty() ? "no name" : "its name");
    if (st != 200) return;
    std::string name = jsonGetString(resp, "username");
    if (name.empty()) name = jsonGetString(resp, "title");
    if (!name.empty()) out.userName = name;
    std::string uuid = jsonGetString(resp, "uuid");
    if (!uuid.empty()) out.userId = uuid;
}

bool JellyfinClient::plex_authenticate(const std::string& serverUrl, const std::string& username,
                                  const std::string& password, JellyfinAuth& out) {
    out = JellyfinAuth();
    if (username.empty()) {
        /* no account: the server lets this network in (List of IP addresses
         * and networks that are allowed without auth), or the password field
         * holds a token */
        out.accessToken = password;
        return plexCheckServer(serverUrl, out);
    }
    if (username == "token") {
        out.accessToken = password;
        plexAccountUser(out);   /* a server's own token has no account: then none */
        return plexCheckServer(serverUrl, out);
    }
    std::string body = "user%5Blogin%5D=" + urlEncode(username) + "&user%5Bpassword%5D=" + urlEncode(password);
    std::string resp;
    int status = httpRequest(std::string(PLEX_TV) + "/users/sign_in.json", "POST",
                             "application/x-www-form-urlencoded", body, "", resp);
    if (status != 201 && status != 200) {
        if (status >= 0) {
            char buf[128];
            snprintf(buf, sizeof(buf), tr("Auth failed (HTTP %d)"), status);
            errMsg = buf;
        }
        return false;
    }
    out.accessToken = jsonGetString(resp, "authToken");
    out.userId      = jsonGetString(resp, "uuid");
    out.userName    = jsonGetString(resp, "username");
    if (out.accessToken.empty()) { errMsg = N_("No token in response"); return false; }
    return plexCheckServer(serverUrl, out);
}

/* Two PINs: a short code to type at plex.tv/link, and a strong one the QR
 * code opens in Plex's sign-in page (which takes only those); either one
 * approved signs in.  secret: "<short id>,<strong id>" */
static bool plexNewPin(JellyfinClient* c, bool strong, std::string& id, std::string& code, int& status);

bool JellyfinClient::plex_quickConnectInitiate(const std::string&, QuickConnectResult& out) {
    std::string id, code, sid, scode;
    int status = 0;
    if (!plexNewPin(this, false, id, code, status)) {
        if (status >= 0) {
            char buf[128];
            snprintf(buf, sizeof(buf), tr("QuickConnect initiate failed (HTTP %d)"), status);
            errMsg = buf;
        }
        return false;
    }
    out.code = code;
    out.secret = id;
    out.authenticated = false;
    out.qrUrl.clear();
    if (plexNewPin(this, true, sid, scode, status)) {
        out.secret += "," + sid;
        out.qrUrl = "https://app.plex.tv/auth#?clientID=" + urlEncode(s_deviceId) + "&code=" + scode +
                    "&context%5Bdevice%5D%5Bproduct%5D=WiiFin";
    }
    return true;
}

static bool plexNewPin(JellyfinClient* c, bool strong, std::string& id, std::string& code, int& status) {
    std::string resp;
    status = c->plexHttp(std::string(PLEX_TV) + "/api/v2/pins?strong=" + (strong ? "true" : "false"), "POST", resp);
    if (status != 201 && status != 200) return false;
    id = std::to_string(c->plexLL(resp, "id"));
    code = c->plexStr(resp, "code");
    return !code.empty();
}

/* the token of whichever PIN was approved, "" none yet */
std::string JellyfinClient::plexPinToken(const std::string& secret) {
    size_t b = 0;
    while (b <= secret.size()) {
        size_t e = secret.find(',', b);
        std::string id = secret.substr(b, e == std::string::npos ? std::string::npos : e - b);
        std::string resp;
        if (!id.empty() && httpRequest(std::string(PLEX_TV) + "/api/v2/pins/" + id, "GET", "", "", "", resp) == 200) {
            std::string t = jsonGetString(resp, "authToken");
            if (!t.empty()) return t;
        }
        if (e == std::string::npos) break;
        b = e + 1;
    }
    return "";
}

bool JellyfinClient::plex_quickConnectCheck(const std::string&, const std::string& secret,
                                            QuickConnectResult& out) {
    out.authenticated = !plexPinToken(secret).empty();
    out.secret = secret;
    return true;
}

bool JellyfinClient::plex_quickConnectAuthenticate(const std::string& serverUrl, const std::string& secret,
                                                   JellyfinAuth& out) {
    out = JellyfinAuth();
    out.accessToken = plexPinToken(secret);
    if (out.accessToken.empty()) { errMsg = N_("No token in QC auth response"); return false; }
    plexAccountUser(out);
    if (serverUrl == "plex.tv") return plexFindServer(out);
    return plexCheckServer(serverUrl, out);
}

void JellyfinClient::plex_logServerInfo(const std::string& serverUrl) {
    static std::string logged;
    if (logged == serverUrl) return;
    std::string resp;
    if (httpRequest(serverUrl + "/identity", "GET", "", "", "", resp) != 200) return;
    logged = serverUrl;
    SYS_Report("[Server] Plex Media Server %s\n", jsonGetString(resp, "version").c_str());
}

void JellyfinClient::plex_logTranscodeFailure(const std::string&, const JellyfinAuth&) {
    SYS_Report("[Server] the transcode failed: see the Plex server's Logs (Plex Transcoder)\n");
}

bool JellyfinClient::plex_getServerName(const std::string& serverUrl, const JellyfinAuth& auth,
                                   std::string& outName) {
    std::string resp;
    if (!plexGet(serverUrl, auth, "/", resp)) return false;
    outName = jsonGetString(resp, "friendlyName");
    return !outName.empty();
}

bool JellyfinClient::plex_postCapabilities(const std::string&, const JellyfinAuth&) { return true; }

// ---------------------------------------------------------------------------
// Libraries
// ---------------------------------------------------------------------------
bool JellyfinClient::plex_getLibraries(const std::string& serverUrl, const JellyfinAuth& auth,
                                  std::vector<JellyfinLibrary>& out) {
    tokenRefused = false;
    std::string resp;
    int status = httpRequest(serverUrl + "/library/sections", "GET", "", "", auth.accessToken, resp);
    tokenRefused = status == 401;
    if (status != 200) {
        if (status >= 0) errMsg = "getLibraries failed (HTTP " + std::to_string(status) + ")";
        return false;
    }
    struct Ctx { JellyfinClient* self; std::vector<JellyfinLibrary>* out; };
    Ctx ctx{ this, &out };
    forEachPlexObject(resp, "Directory", [](const std::string& o, void* v) {
        Ctx* c = static_cast<Ctx*>(v);
        PlexParse p{ c->self };
        JellyfinLibrary lib;
        lib.id   = "s" + p.s(o, "key");
        lib.name = p.s(o, "title");
        std::string t = p.s(o, "type");
        lib.collectionType = t == "movie" ? "movies" : t == "show" ? "tvshows" :
                             t == "artist" ? "music" : t == "photo" ? "photos" : "";
        if (lib.id.size() > 1 && !lib.name.empty()) c->out->push_back(lib);
    }, &ctx);
    return true;
}

// ---------------------------------------------------------------------------
// Pictures: Plex's photo transcoder (JPEG at the size asked)
// ---------------------------------------------------------------------------
bool JellyfinClient::plexImage(const std::string& serverUrl, const JellyfinAuth& auth,
                               const std::string& imagePath, int w, int h, bool crop,
                               std::string& outBytes) {
    char q[96];
    snprintf(q, sizeof(q), "/photo/:/transcode?width=%d&height=%d&minSize=%d&upscale=1&format=jpeg&url=",
             w, h, crop ? 1 : 0);
    outBytes.clear();
    return httpRequest(serverUrl + q + urlEncode(imagePath), "GET", "", "", auth.accessToken, outBytes) == 200 &&
           !outBytes.empty();
}

bool JellyfinClient::plex_getItemImageBytes(const std::string& serverUrl, const JellyfinAuth& auth,
                                       const std::string& itemId, int maxWidth, int maxHeight,
                                       std::string& outBytes) {
    if (itemId.empty()) return false;
    /* a library: Plex's collage of its titles */
    if (isSection(itemId))
        return plexImage(serverUrl, auth, "/library/sections/" + sectionKey(itemId) + "/composite/1",
                         maxWidth, maxHeight, true, outBytes);
    return plexImage(serverUrl, auth, "/library/metadata/" + itemId + "/thumb", maxWidth, maxHeight, true, outBytes);
}

bool JellyfinClient::plex_getPhotoBytes(const std::string& serverUrl, const JellyfinAuth& auth,
                                   const std::string& itemId, int maxWidth, int maxHeight,
                                   std::string& outBytes) {
    /* a photo's file is its Part */
    std::string resp;
    if (!plexGet(serverUrl, auth, "/library/metadata/" + itemId, resp)) return false;
    std::string part = nthObject(nthObject(resp, "Media", 0), "Part", 0);
    std::string key = jsonGetString(part, "key");
    if (key.empty()) key = "/library/metadata/" + itemId + "/thumb";
    return plexImage(serverUrl, auth, key, maxWidth, maxHeight, false, outBytes);
}

bool JellyfinClient::plex_getItemBackdropBytes(const std::string& serverUrl, const JellyfinAuth& auth,
                                          const JellyfinItem& item, int maxWidth, int maxHeight,
                                          std::string& outBytes) {
    if (item.type == "Episode") {
        if (plexImage(serverUrl, auth, "/library/metadata/" + item.id + "/thumb", maxWidth, maxHeight, true, outBytes))
            return true;
        if (!item.seriesId.empty() &&
            plexImage(serverUrl, auth, "/library/metadata/" + item.seriesId + "/art", maxWidth, maxHeight, true, outBytes))
            return true;
    } else if (plexImage(serverUrl, auth, "/library/metadata/" + item.id + "/art", maxWidth, maxHeight, true, outBytes)) {
        return true;
    }
    return getItemImageBytes(serverUrl, auth, item.id, maxWidth, maxHeight, outBytes);
}

// ---------------------------------------------------------------------------
// An item's page
// ---------------------------------------------------------------------------
/* a Media's audio and subtitle streams; MediaStream.index is Plex's stream id
 * (what the transcoder and /library/parts take) */
static void plexStreams(JellyfinClient* c, const std::string& media, std::vector<MediaStream>& audio,
                        std::vector<MediaStream>& subs, int& defAudio, int& defSub) {
    struct Ctx { JellyfinClient* self; std::vector<MediaStream>* a; std::vector<MediaStream>* s; int* da; int* ds; };
    Ctx ctx{ c, &audio, &subs, &defAudio, &defSub };
    std::string part = nthObject(media, "Part", 0);
    size_t p = part.find("\"Stream\":[");
    if (p == std::string::npos) return;
    forEachArrayObject(part, p + 9, [](const std::string& o, void* v) {
        Ctx* x = static_cast<Ctx*>(v);
        PlexParse pp{ x->self };
        int st = pp.i(o, "streamType");
        if (st != 2 && st != 3) return;
        MediaStream m;
        m.index = pp.i(o, "id");
        m.type = st == 2 ? "Audio" : "Subtitle";
        m.displayTitle = pp.s(o, "extendedDisplayTitle");
        if (m.displayTitle.empty()) m.displayTitle = pp.s(o, "displayTitle");
        m.language = pp.s(o, "languageCode");
        m.codec = pp.s(o, "codec");
        /* drawn by WiiFin: a text track Plex serves as a file (a sidecar,
         * its "key"); an embedded one is burned in by the transcoder (Plex
         * answers 501 to converting it) */
        m.isText = st == 3 && !pp.s(o, "key").empty() && (m.codec == "srt" || m.codec == "subrip" || m.codec == "ass" ||
                               m.codec == "ssa" || m.codec == "webvtt" || m.codec == "mov_text");
        bool sel = o.find("\"selected\":true") != std::string::npos;
        if (st == 2) { x->a->push_back(m); if (sel || *x->da < 0) *x->da = m.index; }
        else         { x->s->push_back(m); if (sel) *x->ds = m.index; }
    }, &ctx);
    /* the selected audio track wins over the first */
    for (const MediaStream& m : audio) {
        size_t at = part.find("\"id\":" + std::to_string(m.index) + ",");
        if (at != std::string::npos) {
            size_t e = part.find('}', at);
            if (part.substr(at, e - at).find("\"selected\":true") != std::string::npos) defAudio = m.index;
        }
    }
}

static void plexChapters(JellyfinClient* c, const std::string& item, std::vector<JellyfinChapter>& out) {
    out.clear();
    size_t p = item.find("\"Chapter\":[");
    if (p == std::string::npos) return;
    struct Ctx { JellyfinClient* self; std::vector<JellyfinChapter>* out; };
    Ctx ctx{ c, &out };
    forEachArrayObject(item, p + 10, [](const std::string& o, void* v) {
        Ctx* x = static_cast<Ctx*>(v);
        PlexParse pp{ x->self };
        JellyfinChapter ch;
        ch.startTicks = pp.ll(o, "startTimeOffset") * TICKS_PER_MS;
        ch.name = pp.s(o, "tag");
        ch.hasImage = !pp.s(o, "thumb").empty();
        x->out->push_back(ch);
    }, &ctx);
}

bool JellyfinClient::plex_getItemDetail(const std::string& serverUrl, const JellyfinAuth& auth,
                                   const std::string& itemId, JellyfinItemDetail& out) {
    std::string resp;
    if (!plexGet(serverUrl, auth, "/library/metadata/" + itemId + "?includeChapters=1&includeExtras=1", resp))
        return false;
    std::string o = nthObject(resp, "Metadata", 0);
    if (o.empty()) { errMsg = "Plex: no such item"; return false; }
    PlexParse p{ this };
    out = JellyfinItemDetail();
    out.id   = itemId;
    out.name = p.s(o, "title");
    out.overview = jsonGetString(o, "summary", true);
    out.officialRating = p.s(o, "contentRating");
    out.year = p.i(o, "year");
    out.runtimeTicks = p.ll(o, "duration") * TICKS_PER_MS;
    out.playbackPositionTicks = p.ll(o, "viewOffset") * TICKS_PER_MS;
    out.isFavorite = p.i(o, "userRating") >= 10;
    std::string t = p.s(o, "type");
    if (t == "show" || t == "season") {
        int leaves = p.i(o, "leafCount");
        out.played = leaves > 0 && p.i(o, "viewedLeafCount") >= leaves;
    } else out.played = p.i(o, "viewCount") > 0;
    if (t == "episode") {
        out.seriesId = p.s(o, "grandparentRatingKey");
        out.seasonId = p.s(o, "parentRatingKey");
        out.seriesName = p.s(o, "grandparentTitle");
    }
    /* genres and people: their "tag" */
    struct Tags { JellyfinClient* self; JellyfinItemDetail* d; const char* role; };
    auto tagArray = [&](const char* key, const char* role) {
        size_t a = o.find(std::string("\"") + key + "\":[");
        if (a == std::string::npos) return;
        Tags tg{ this, &out, role };
        forEachArrayObject(o, o.find('[', a), [](const std::string& x, void* v) {
            Tags* t2 = static_cast<Tags*>(v);
            PlexParse pp{ t2->self };
            std::string tag = pp.s(x, "tag");
            if (tag.empty()) return;
            if (!t2->role) { if (t2->d->genres.size() < 4) t2->d->genres.push_back(tag); return; }
            if (t2->d->people.size() >= 6) return;
            JellyfinPerson per;
            per.name = tag;
            per.role = t2->role;
            per.character = pp.s(x, "role");
            t2->d->people.push_back(per);
        }, &tg);
    };
    tagArray("Genre", nullptr);
    tagArray("Director", "Director");
    tagArray("Role", "Actor");
    tagArray("Writer", "Writer");
    /* extras */
    {
        size_t e = o.find("\"Extras\":{");
        if (e != std::string::npos) out.specialFeatures = jsonGetInt(o.substr(e, 80), "size");
    }
    /* the files: Media, each with its streams */
    int mediaCount = 0;
    for (;; ++mediaCount) {
        std::string media = nthObject(o, "Media", mediaCount);
        if (media.empty()) break;
        MediaVersion v;
        v.id = std::to_string(jsonGetInt(media, "id"));
        std::string res = jsonGetString(media, "videoResolution");
        v.name = (res.empty() ? std::string() : (isdigit((unsigned char)res[0]) ? res + "p" : res)) +
                 (jsonGetString(media, "container").empty() ? "" : " " + jsonGetString(media, "container"));
        plexStreams(this, media, v.audio, v.subs, v.defaultAudio, v.defaultSub);
        if (mediaCount == 0) {
            out.audioStreams = v.audio;
            out.subtitleStreams = v.subs;
            out.defaultAudioIndex = v.defaultAudio;
            out.defaultSubIndex = v.defaultSub;
        }
        out.versions.push_back(v);
    }
    if (out.versions.size() < 2) out.versions.clear();
    plexChapters(this, o, out.chapters);
    for (MediaVersion& v : out.versions) v.chapters = out.chapters;
    return true;
}

bool JellyfinClient::plex_getChapters(const std::string& serverUrl, const JellyfinAuth& auth,
                                 const std::string& itemId, std::vector<JellyfinChapter>& out) {
    std::string resp;
    if (!plexGet(serverUrl, auth, "/library/metadata/" + itemId + "?includeChapters=1", resp)) return false;
    plexChapters(this, nthObject(resp, "Metadata", 0), out);
    return true;
}

bool JellyfinClient::plex_getChapterImageBytes(const std::string& serverUrl, const JellyfinAuth& auth,
                                          const std::string& itemId, int index, int maxWidth, int maxHeight,
                                          std::string& outBytes) {
    std::string resp;
    if (!plexGet(serverUrl, auth, "/library/metadata/" + itemId + "?includeChapters=1", resp)) return false;
    std::string ch = nthObject(nthObject(resp, "Metadata", 0), "Chapter", index);
    std::string thumb = jsonGetString(ch, "thumb");
    if (thumb.empty()) return false;
    return plexImage(serverUrl, auth, thumb, maxWidth, maxHeight, true, outBytes);
}

bool JellyfinClient::plex_getIsFavorite(const std::string& serverUrl, const JellyfinAuth& auth,
                                   const std::string& itemId, bool& favorite, int* specials) {
    std::string resp;
    if (!plexGet(serverUrl, auth, "/library/metadata/" + itemId + "?includeExtras=1", resp)) return false;
    std::string o = nthObject(resp, "Metadata", 0);
    favorite = jsonGetInt(o, "userRating") >= 10;
    if (specials) {
        size_t e = o.find("\"Extras\":{");
        *specials = e == std::string::npos ? 0 : jsonGetInt(o.substr(e, 80), "size");
    }
    return true;
}

bool JellyfinClient::plex_getSpecialFeatures(const std::string& serverUrl, const JellyfinAuth& auth,
                                        const std::string& itemId, std::vector<JellyfinItem>& out) {
    if (!plexItems(serverUrl, auth, "/library/metadata/" + itemId + "/extras", out)) return false;
    for (JellyfinItem& it : out) {
        if (it.type == "Video" || it.type == "Folder") it.type = "Video";
        std::string& e = it.extraType;
        if (!e.empty()) e[0] = (char)toupper((unsigned char)e[0]);
    }
    /* trailers first */
    std::stable_sort(out.begin(), out.end(), [](const JellyfinItem& a, const JellyfinItem& b) {
        return (a.extraType == "Trailer") > (b.extraType == "Trailer");
    });
    return true;
}

// ---------------------------------------------------------------------------
// Series
// ---------------------------------------------------------------------------
bool JellyfinClient::plex_getSeasons(const std::string& serverUrl, const JellyfinAuth& auth,
                                const std::string& seriesId, std::vector<JellyfinSeason>& out) {
    std::vector<JellyfinItem> items;
    if (!plexItems(serverUrl, auth, "/library/metadata/" + seriesId + "/children", items, nullptr, "Season"))
        return false;
    for (const JellyfinItem& it : items) {
        JellyfinSeason s;
        s.id = it.id;
        s.name = it.name;
        s.indexNumber = it.seasonNumber;
        out.push_back(s);
    }
    return true;
}

bool JellyfinClient::plexFetchEpisodes(const std::string& path, const JellyfinAuth& auth,
                                   std::vector<JellyfinEpisode>& out, size_t) {
    /* path here is serverUrl + Plex path */
    std::string resp;
    int status = httpRequest(path, "GET", "", "", auth.accessToken, resp);
    if (status != 200) { if (status >= 0) errMsg = "Plex HTTP " + std::to_string(status); return false; }
    struct Ctx { JellyfinClient* self; std::vector<JellyfinEpisode>* out; };
    Ctx ctx{ this, &out };
    forEachPlexObject(resp, "Metadata", [](const std::string& o, void* v) {
        Ctx* c = static_cast<Ctx*>(v);
        PlexParse p{ c->self };
        if (p.s(o, "type") != "episode") return;
        JellyfinEpisode e;
        e.id = p.s(o, "ratingKey");
        e.name = p.s(o, "title");
        e.indexNumber = p.i(o, "index");
        e.seasonNumber = p.i(o, "parentIndex");
        e.playbackPositionTicks = p.ll(o, "viewOffset") * TICKS_PER_MS;
        e.played = p.i(o, "viewCount") > 0;
        c->out->push_back(e);
    }, &ctx);
    return true;
}

bool JellyfinClient::plex_getEpisodes(const std::string& serverUrl, const JellyfinAuth& auth,
                                 const std::string& seriesId, const std::string& seasonId,
                                 std::vector<JellyfinEpisode>& out) {
    if (seasonId.empty()) return getSeriesEpisodes(serverUrl, auth, seriesId, out);
    return plexFetchEpisodes(serverUrl + "/library/metadata/" + seasonId + "/children", auth, out);
}

bool JellyfinClient::plex_getSeriesEpisodes(const std::string& serverUrl, const JellyfinAuth& auth,
                                       const std::string& seriesId, std::vector<JellyfinEpisode>& out) {
    return plexFetchEpisodes(serverUrl + "/library/metadata/" + seriesId + "/allLeaves", auth, out);
}

bool JellyfinClient::plex_getShuffledEpisodes(const std::string& serverUrl, const JellyfinAuth& auth,
                                         const std::string& seriesId, const std::string& seasonId,
                                         int limit, std::vector<JellyfinEpisode>& out) {
    std::vector<JellyfinEpisode> all;
    std::string src = seasonId.empty() ? "/library/metadata/" + seriesId + "/allLeaves"
                                       : "/library/metadata/" + seasonId + "/children";
    if (!plexFetchEpisodes(serverUrl + src, auth, all)) return false;
    for (size_t i = all.size(); i > 1; --i) std::swap(all[i - 1], all[rand() % i]);
    if ((int)all.size() > limit) all.resize(limit);
    out = all;
    return true;
}

// ---------------------------------------------------------------------------
// Home rows
// ---------------------------------------------------------------------------
bool JellyfinClient::plex_getContinueWatching(const std::string& serverUrl, const JellyfinAuth& auth,
                                         std::vector<JellyfinItem>& out) {
    if (!plexItems(serverUrl, auth, "/library/onDeck?X-Plex-Container-Start=0&X-Plex-Container-Size=12", out)) return false;
    out.erase(std::remove_if(out.begin(), out.end(), [](const JellyfinItem& i) { return i.playbackPositionTicks <= 0; }), out.end());
    if (out.size() > 3) out.resize(3);
    return true;
}

bool JellyfinClient::plex_getNextUp(const std::string& serverUrl, const JellyfinAuth& auth,
                               std::vector<JellyfinItem>& out) {
    if (!plexItems(serverUrl, auth, "/library/onDeck?X-Plex-Container-Start=0&X-Plex-Container-Size=12", out, nullptr, "Episode"))
        return false;
    out.erase(std::remove_if(out.begin(), out.end(), [](const JellyfinItem& i) { return i.playbackPositionTicks > 0; }), out.end());
    if (out.size() > 3) out.resize(3);
    return true;
}

bool JellyfinClient::plex_getMovieContinueWatching(const std::string& serverUrl, const JellyfinAuth& auth,
                                              std::vector<JellyfinItem>& out) {
    if (!plexItems(serverUrl, auth, "/library/onDeck?X-Plex-Container-Start=0&X-Plex-Container-Size=12", out, nullptr, "Movie"))
        return false;
    if (out.size() > 4) out.resize(4);
    return true;
}

bool JellyfinClient::plex_getTVContinueWatching(const std::string& serverUrl, const JellyfinAuth& auth,
                                           std::vector<JellyfinItem>& out) {
    if (!plexItems(serverUrl, auth, "/library/onDeck?X-Plex-Container-Start=0&X-Plex-Container-Size=12", out, nullptr, "Episode"))
        return false;
    if (out.size() > 4) out.resize(4);
    return true;
}

static std::string recentPath(const std::string& parentId, int limit) {
    return (isSection(parentId) ? "/library/sections/" + sectionKey(parentId) + "/recentlyAdded"
                                : std::string("/library/recentlyAdded")) +
           "?X-Plex-Container-Start=0&X-Plex-Container-Size=" + std::to_string(limit);
}

bool JellyfinClient::plex_getMoviesLatest(const std::string& serverUrl, const JellyfinAuth& auth,
                                     const std::string& parentId, int limit, std::vector<JellyfinItem>& out) {
    return plexItems(serverUrl, auth, recentPath(parentId, limit), out, nullptr, "Movie");
}

bool JellyfinClient::plex_getTVSeriesLatest(const std::string& serverUrl, const JellyfinAuth& auth,
                                       const std::string& parentId, int limit, std::vector<JellyfinItem>& out) {
    /* Plex lists the new seasons and episodes: their series, once each */
    std::vector<JellyfinItem> got;
    if (!plexItems(serverUrl, auth, recentPath(parentId, limit * 3), got)) return false;
    std::vector<std::string> seen;
    for (const JellyfinItem& it : got) {
        if ((int)out.size() >= limit) break;
        std::string id = it.type == "Series" ? it.id : it.seriesId;
        if (id.empty() || std::find(seen.begin(), seen.end(), id) != seen.end()) continue;
        seen.push_back(id);
        JellyfinItem s = it;
        if (it.type != "Series") { s.id = id; s.name = it.seriesName.empty() ? it.name : it.seriesName; s.type = "Series"; }
        out.push_back(s);
    }
    return true;
}

bool JellyfinClient::plex_getTVUpcoming(const std::string&, const JellyfinAuth&, int, std::vector<JellyfinItem>&) {
    return true;   /* Plex's library has no unaired episodes */
}

bool JellyfinClient::plex_getMusicLatest(const std::string& serverUrl, const JellyfinAuth& auth,
                                    const std::string& parentId, int limit, std::vector<JellyfinItem>& out) {
    return plexItems(serverUrl, auth, recentPath(parentId, limit), out, nullptr, "MusicAlbum");
}

bool JellyfinClient::plex_getMovieCollections(const std::string& serverUrl, const JellyfinAuth& auth,
                                         const std::string& parentId, int startIndex, int limit,
                                         std::vector<JellyfinItem>& out, int& totalCount) {
    if (!isSection(parentId)) { totalCount = 0; return true; }
    char q[160];
    snprintf(q, sizeof(q), "/library/sections/%s/collections?X-Plex-Container-Start=%d&X-Plex-Container-Size=%d",
             sectionKey(parentId).c_str(), startIndex, limit);
    return plexItems(serverUrl, auth, q, out, &totalCount);
}

bool JellyfinClient::plex_getFavoriteMovies(const std::string& serverUrl, const JellyfinAuth& auth,
                                       const std::string& parentId, int startIndex, int limit,
                                       std::vector<JellyfinItem>& out, int& totalCount) {
    char q[200];
    snprintf(q, sizeof(q), "/Users/x/Items?ParentId=%s&IncludeItemTypes=Movie&Filters=IsFavorite&StartIndex=%d&Limit=%d",
             parentId.c_str(), startIndex, limit);
    return getItemsByQuery(serverUrl, auth, q, out, &totalCount);
}

bool JellyfinClient::plex_getGlobalFavorites(const std::string& serverUrl, const JellyfinAuth& auth,
                                        int, int, std::vector<JellyfinItem>& out, int& totalCount) {
    /* each library's items rated 10 */
    std::vector<JellyfinLibrary> libs;
    totalCount = 0;
    if (!getLibraries(serverUrl, auth, libs)) return false;
    for (const JellyfinLibrary& l : libs) {
        std::vector<JellyfinItem> got;
        plexItems(serverUrl, auth, "/library/sections/" + sectionKey(l.id) + "/all?userRating%3E%3E=10", got);
        out.insert(out.end(), got.begin(), got.end());
    }
    totalCount = (int)out.size();
    return true;
}

// ---------------------------------------------------------------------------
// Playlists, music
// ---------------------------------------------------------------------------
bool JellyfinClient::plex_getPlaylists(const std::string& serverUrl, const JellyfinAuth& auth,
                                  int startIndex, int limit, std::vector<JellyfinItem>& out, int& totalCount) {
    char q[160];
    snprintf(q, sizeof(q), "/playlists?X-Plex-Container-Start=%d&X-Plex-Container-Size=%d", startIndex, limit);
    return plexItems(serverUrl, auth, q, out, &totalCount);
}

bool JellyfinClient::plexAudioList(const std::string& serverUrl, const JellyfinAuth& auth,
                                   const std::string& path, std::vector<JellyfinAudioItem>& out) {
    std::string resp;
    if (!plexGet(serverUrl, auth, path, resp)) return false;
    struct Ctx { JellyfinClient* self; std::vector<JellyfinAudioItem>* out; };
    Ctx ctx{ this, &out };
    forEachPlexObject(resp, "Metadata", [](const std::string& o, void* v) {
        Ctx* c = static_cast<Ctx*>(v);
        JellyfinAudioItem a;
        plexToAudio(c->self, o, a);
        if (!a.id.empty()) c->out->push_back(a);
    }, &ctx);
    return true;
}

bool JellyfinClient::plex_getPlaylistTracks(const std::string& serverUrl, const JellyfinAuth& auth,
                                       const std::string& playlistId, std::vector<JellyfinAudioItem>& out) {
    return plexAudioList(serverUrl, auth, "/playlists/" + playlistId + "/items", out);
}

bool JellyfinClient::plex_getAlbumTracks(const std::string& serverUrl, const JellyfinAuth& auth,
                                    const std::string& albumId, std::vector<JellyfinAudioItem>& out) {
    return plexAudioList(serverUrl, auth, "/library/metadata/" + albumId + "/children", out);
}

bool JellyfinClient::plex_getItemsByIds(const std::string& serverUrl, const JellyfinAuth& auth,
                                   const std::vector<std::string>& ids,
                                   std::vector<JellyfinItem>& outItems,
                                   std::vector<JellyfinAudioItem>& outAudio) {
    if (ids.empty()) return true;
    std::string list;
    for (const std::string& id : ids) list += (list.empty() ? "" : ",") + id;
    if (!plexItems(serverUrl, auth, "/library/metadata/" + list, outItems)) return false;
    return plexAudioList(serverUrl, auth, "/library/metadata/" + list, outAudio);
}

bool JellyfinClient::plex_getAudioTrackInfo(const std::string& serverUrl, const JellyfinAuth& auth,
                                       const std::string& itemId, AudioTrackInfo& out) {
    std::string resp;
    if (!plexGet(serverUrl, auth, "/library/metadata/" + itemId, resp)) return false;
    std::string o = nthObject(resp, "Metadata", 0);
    out = AudioTrackInfo();
    out.artists = jsonGetString(o, "originalTitle");
    if (out.artists.empty()) out.artists = jsonGetString(o, "grandparentTitle");
    out.album   = jsonGetString(o, "parentTitle");
    out.albumId = jsonGetString(o, "parentRatingKey");
    out.parentId = out.albumId;
    out.year    = jsonGetInt(o, "parentYear");
    out.isFavorite = jsonGetInt(o, "userRating") >= 10;
    out.albumHasImage = !jsonGetString(o, "parentThumb").empty();
    out.hasImage = false;
    return true;
}

bool JellyfinClient::plex_getLyrics(const std::string&, const JellyfinAuth&, const std::string&,
                               std::vector<LyricLine>&) {
    return false;
}

bool JellyfinClient::plex_getAutoplayTracks(const std::string& serverUrl, const JellyfinAuth& auth,
                                       const std::string& itemId, const std::string& parentId,
                                       int limit, std::vector<JellyfinAudioItem>& out) {
    /* tracks of the same album, shuffled */
    if (parentId.empty()) return false;
    std::vector<JellyfinAudioItem> all;
    if (!plexAudioList(serverUrl, auth, "/library/metadata/" + parentId + "/children", all)) return false;
    for (size_t i = all.size(); i > 1; --i) std::swap(all[i - 1], all[rand() % i]);
    for (const JellyfinAudioItem& a : all)
        if (a.id != itemId && (int)out.size() < limit) out.push_back(a);
    return !out.empty();
}

// ---------------------------------------------------------------------------
// Watched, favourite (a rating of 10), search
// ---------------------------------------------------------------------------
bool JellyfinClient::plex_setPlayed(const std::string& serverUrl, const JellyfinAuth& auth,
                               const std::string& itemId, bool played) {
    std::string resp;
    std::string p = std::string(played ? "/:/scrobble" : "/:/unscrobble") +
                    "?identifier=com.plexapp.plugins.library&key=" + itemId;
    return plexGet(serverUrl, auth, p, resp);
}

bool JellyfinClient::plex_setFavorite(const std::string& serverUrl, const JellyfinAuth& auth,
                                 const std::string& itemId, bool favorite) {
    std::string resp;
    int status = httpRequest(serverUrl + "/:/rate?identifier=com.plexapp.plugins.library&key=" + itemId +
                             "&rating=" + (favorite ? "10" : "-1"), "PUT", "", "", auth.accessToken, resp);
    SYS_Report("[Favorite] %s: HTTP %d\n", favorite ? "on" : "off", status);
    return status == 200;
}

bool JellyfinClient::plex_searchItems(const std::string& serverUrl, const JellyfinAuth& auth,
                                 const std::string& searchTerm, int limit, std::vector<JellyfinItem>& out) {
    std::string p = "/hubs/search?query=" + urlEncode(searchTerm) + "&limit=" + std::to_string(limit);
    if (!plexItems(serverUrl, auth, p, out, nullptr, "Movie,Series,Episode,MusicAlbum,MusicArtist,Audio"))
        return false;
    if ((int)out.size() > limit) out.resize(limit);
    return true;
}

// ---------------------------------------------------------------------------
// Skip intro / credits: Plex's markers
// ---------------------------------------------------------------------------
bool JellyfinClient::plex_getIntroTimestamps(const std::string& serverUrl, const JellyfinAuth& auth,
                                        const std::string& episodeId, IntroInfo& out) {
    std::string resp;
    if (!plexGet(serverUrl, auth, "/library/metadata/" + episodeId + "?includeMarkers=1", resp)) return false;
    std::string o = nthObject(resp, "Metadata", 0);
    size_t p = o.find("\"Marker\":[");
    if (p == std::string::npos) return false;
    struct Ctx { JellyfinClient* self; IntroInfo* out; };
    Ctx ctx{ this, &out };
    forEachArrayObject(o, p + 9, [](const std::string& m, void* v) {
        Ctx* c = static_cast<Ctx*>(v);
        PlexParse pp{ c->self };
        std::string t = pp.s(m, "type");
        MediaSegment s;
        if (t == "intro") s.kind = MediaSegment::Intro;
        else if (t == "credits") s.kind = MediaSegment::Outro;
        else if (t == "commercial") s.kind = MediaSegment::Commercial;
        else return;
        s.start = pp.ll(m, "startTimeOffset") / 1000.0f;
        s.end   = pp.ll(m, "endTimeOffset") / 1000.0f;
        if (s.end > s.start) c->out->segments.push_back(s);
    }, &ctx);
    std::sort(out.segments.begin(), out.segments.end(),
              [](const MediaSegment& a, const MediaSegment& b) { return a.start < b.start; });
    return !out.segments.empty();
}

/* Plex's video preview thumbnails are BIF files: not read (yet) */
bool JellyfinClient::plex_getTrickplayInfo(const std::string&, const JellyfinAuth&, const std::string&,
                                      const std::string&, TrickplayInfo&) { return false; }
bool JellyfinClient::plex_getTrickplayTile(const std::string&, const JellyfinAuth&, const std::string&,
                                      const std::string&, int, int, std::string&) { return false; }

/* a text subtitle track: Plex sends a sidecar file as it is, and converts an
 * embedded one (?format=srt) */
bool JellyfinClient::plex_getSubtitleSrt(const std::string& serverUrl, const JellyfinAuth& auth,
                                    const std::string&, const std::string&, int streamIndex,
                                    std::string& outSrt) {
    return httpRequest(serverUrl + "/library/streams/" + std::to_string(streamIndex) + "?format=srt&encoding=utf-8",
                       "GET", "", "", auth.accessToken, outSrt, 4u << 20) == 200 && !outSrt.empty();
}

// ---------------------------------------------------------------------------
// Playback
// ---------------------------------------------------------------------------
namespace {
struct PlexFormat {
    const char* containers;   /* as Plex names them */
    const char* demuxer;      /* MPlayer's own, or FFmpeg's (lavf:<format>) */
    const char* video;
    const char* audio;
};
/* What MPlayer CE decodes in real time on the Wii (WiiFin's measurements) */
const PlexFormat PLEX_FORMATS[] = {
    { "avi",            "avi",         "mpeg4,h264,mpeg1video,mpeg2video",     "mp3,mp2,ac3,aac" },
    { "mkv,webm",       "mkv",         "mpeg4,h264,mpeg1video,mpeg2video,vp8", "mp3,mp2,ac3,aac,vorbis,flac" },
    { "mp4,m4v,mov",    "lavf:mov",    "mpeg4,h264",                           "aac,mp3,ac3" },
    { "mpegts,ts",      "lavf:mpegts", "mpeg2video,h264,mpeg4",                "mp2,mp3,ac3,aac" },
    { "mpeg,mpg,vob",   "mpegps",      "mpeg1video,mpeg2video",                "mp2,mp3,ac3" },
};

bool plexInList(const char* list, const std::string& name) {
    if (name.empty()) return false;
    for (const char* p = list; *p; ) {
        const char* e = strchr(p, ',');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n == name.size() && strncmp(p, name.c_str(), n) == 0) return true;
        if (!e) break;
        p = e + 1;
    }
    return false;
}

const PlexFormat* plexFormatOf(const std::string& container) {
    for (const PlexFormat& f : PLEX_FORMATS)
        if (plexInList(f.containers, container)) return &f;
    return nullptr;
}

/* "msmpeg4", "mpeg4" (Plex says "mpeg4" for Xvid/DivX) */
std::string plexCodec(const std::string& c) {
    if (c == "mpeg2video" || c == "mpeg2") return "mpeg2video";
    if (c == "mpeg1video" || c == "mpeg1") return "mpeg1video";
    return c;
}

/* the transcode the Wii takes: H.264 Main at most 640x480, level 3.0, MP3
 * stereo, in MPEG-TS over HTTP (Plex's transcoder has no MPEG-4 ASP) */
const char* PROFILE_EXTRA =
    "add-transcode-target(type=videoProfile&context=streaming&protocol=http&container=mpegts&videoCodec=h264&audioCodec=mp3)"
    "+add-limitation(scope=videoCodec&scopeName=h264&type=upperBound&name=video.width&value=640)"
    "+add-limitation(scope=videoCodec&scopeName=h264&type=upperBound&name=video.height&value=480)"
    "+add-limitation(scope=videoCodec&scopeName=h264&type=upperBound&name=video.level&value=30)"
    "+add-limitation(scope=videoAudioCodec&scopeName=mp3&type=upperBound&name=audio.channels&value=2)"
    "+add-transcode-target(type=musicProfile&context=streaming&protocol=http&container=mp3&audioCodec=mp3)";

unsigned s_session = 0;
/* each item's duration (ms), for the timeline reports */
std::map<std::string, long long> s_durationMs;
} // namespace

/* the X-Plex-* parameters MPlayer's requests carry in their URL */
std::string JellyfinClient::plexClientParams(const JellyfinAuth& auth) const {
    std::string p = "X-Plex-Client-Identifier=" + urlEncode(s_deviceId) +
                    "&X-Plex-Product=WiiFin&X-Plex-Version=" WIIFIN_VERSION
                    "&X-Plex-Platform=Generic&X-Plex-Device=Nintendo%20Wii&X-Plex-Device-Name=Wii";
    if (!auth.accessToken.empty()) p += "&X-Plex-Token=" + urlEncode(auth.accessToken);
    return p;
}

bool JellyfinClient::plex_getDirectPlayUrl(const std::string& serverUrl, const JellyfinAuth& auth,
                                      const std::string& itemId, const std::string& mediaSourceId,
                                      int audioStreamIndex, int subtitleStreamIndex, DirectPlay& out) {
    out = DirectPlay();
    if (!directPlay || forceReencode) { out.reasons = "direct play off"; return false; }
    if (subtitleStreamIndex >= 0) { out.reasons = "subtitles burned in"; return false; }
    std::string resp;
    if (!plexGet(serverUrl, auth, "/library/metadata/" + itemId, resp)) { out.reasons = "metadata"; return false; }
    std::string o = nthObject(resp, "Metadata", 0);
    std::string media;
    for (int i = 0; ; ++i) {
        std::string m = nthObject(o, "Media", i);
        if (m.empty()) break;
        if (media.empty() || std::to_string(jsonGetInt(m, "id")) == mediaSourceId) media = m;
    }
    std::string part = nthObject(media, "Part", 0);
    if (part.empty()) { out.reasons = "no file"; return false; }
    s_durationMs[itemId] = jsonGetLongLong(o, "duration");
    out.container  = jsonGetString(media, "container");
    out.videoCodec = plexCodec(jsonGetString(media, "videoCodec"));
    out.audioCodec = jsonGetString(media, "audioCodec");
    out.width  = jsonGetInt(media, "width");
    out.height = jsonGetInt(media, "height");
    /* the streams: the video's frame rate and level, the audio tracks in order */
    std::vector<std::string> audios;
    std::string video;
    {
        size_t p = part.find("\"Stream\":[");
        struct Ctx { std::vector<std::string>* a; std::string* v; JellyfinClient* self; };
        Ctx ctx{ &audios, &video, this };
        if (p != std::string::npos)
            forEachArrayObject(part, p + 9, [](const std::string& s, void* v) {
                Ctx* c = static_cast<Ctx*>(v);
                int t = c->self->plexInt(s, "streamType");
                if (t == 1 && c->v->empty()) *c->v = s;
                if (t == 2) c->a->push_back(s);
            }, &ctx);
    }
    {
        size_t f = video.find("\"frameRate\":");
        out.fps = f != std::string::npos ? (float)atof(video.c_str() + f + 12) : 0.0f;
    }
    int chosen = -1;
    for (size_t i = 0; i < audios.size(); ++i)
        if (jsonGetInt(audios[i], "id") == audioStreamIndex) chosen = (int)i;
    std::string audio = chosen >= 0 ? audios[chosen] : (audios.empty() ? "" : audios[0]);
    if (chosen >= 0) out.audioCodec = jsonGetString(audio, "codec");
    const PlexFormat* fmt = plexFormatOf(out.container);
    if (fmt) out.demuxer = fmt->demuxer;

    std::string why;
    auto add = [&](const char* w) { if (!why.empty()) why += ", "; why += w; };
    if (!fmt) add("container");
    else {
        if (!plexInList(fmt->video, out.videoCodec)) add("video codec");
        if (!audio.empty() && !plexInList(fmt->audio, out.audioCodec)) add("audio codec");
    }
    if (out.videoCodec == "h264") {
        if (out.width > 720 || out.height > 480) add("resolution");
        if (jsonGetInt(video, "level") > 31) add("level");
        if (jsonGetInt(video, "bitDepth") > 8) add("bit depth");
        if (jsonGetInt(media, "bitrate") > 2500) add("bitrate");
    } else if (out.width > 720 || out.height > 576) add("resolution");
    if (out.fps > 30.5f) add("frame rate");
    int channels = jsonGetInt(audio, "channels");
    if (channels > (out.audioCodec == "ac3" && !(fmt && strcmp(fmt->demuxer, "avi") == 0) ? 6 : 2)) add("audio channels");
    if (chosen > 0 && fmt) {
        if (strcmp(fmt->demuxer, "avi") == 0) out.aid = jsonGetInt(audio, "index");
        else if (strcmp(fmt->demuxer, "mpegps") != 0) out.aid = chosen;
        else add("second audio track");
    }
    SYS_Report("[DirectPlay] %s %s %dx%d %.2f fps, %s: %s%s\n", out.container.c_str(), out.videoCodec.c_str(),
               out.width, out.height, (double)out.fps, out.audioCodec.c_str(),
               why.empty() ? "direct" : "transcode, ", why.c_str());
    if (!why.empty()) { out.reasons = why; return false; }
    char sid[24];
    snprintf(sid, sizeof(sid), "wii%u%u", (unsigned)nowMs(), ++s_session);
    out.playSessionId = sid;
    out.url = addScheme(serverUrl) + jsonGetString(part, "key") + "?" + plexClientParams(auth);
    return true;
}

bool JellyfinClient::plex_getPlaybackUrl(const std::string& serverUrl, const JellyfinAuth& auth,
                                    const std::string& itemId, const std::string& mediaSourceId,
                                    int audioStreamIndex, int subtitleStreamIndex, long long startTimeTicks,
                                    std::string& outUrl, std::string& outPlaySessionId,
                                    PlaybackChoice& how, bool allowDirect) {
    how = PlaybackChoice();
    DirectPlay dp;
    if (allowDirect && getDirectPlayUrl(serverUrl, auth, itemId, mediaSourceId,
                                        audioStreamIndex, subtitleStreamIndex, dp)) {
        outUrl = dp.url;
        outPlaySessionId = dp.playSessionId;
        how.direct = true;
        how.demuxer = dp.demuxer;
        how.fps = dp.fps;
        how.aid = dp.aid;
        return true;
    }
    if (!getTranscodingUrl(serverUrl, auth, itemId, mediaSourceId, audioStreamIndex,
                           subtitleStreamIndex, startTimeTicks, outUrl, outPlaySessionId, dp.reasons))
        return false;
    how.fps = dp.fps > 1.0f && dp.fps <= 30.5f ? dp.fps : (dp.fps > 30.5f ? dp.fps / 2 : 0.0f);
    return true;
}

bool JellyfinClient::plex_getTranscodingUrl(const std::string& serverUrl, const JellyfinAuth& auth,
                                       const std::string& itemId, const std::string& mediaSourceId,
                                       int audioStreamIndex, int subtitleStreamIndex, long long startTimeTicks,
                                       std::string& outUrl, std::string& outPlaySessionId,
                                       const std::string& transcodeReasons) {
    /* the file (Media) to play, and its Part for the track choice */
    std::string resp;
    if (!plexGet(serverUrl, auth, "/library/metadata/" + itemId, resp)) return false;
    std::string o = nthObject(resp, "Metadata", 0);
    int mediaIndex = 0;
    std::string media = nthObject(o, "Media", 0);
    for (int i = 0; ; ++i) {
        std::string m = nthObject(o, "Media", i);
        if (m.empty()) break;
        if (std::to_string(jsonGetInt(m, "id")) == mediaSourceId) { mediaIndex = i; media = m; }
    }
    s_durationMs[itemId] = jsonGetLongLong(o, "duration");
    std::string partId = std::to_string(jsonGetInt(nthObject(media, "Part", 0), "id"));
    /* Plex's transcoder takes the tracks selected on the Part */
    {
        std::string sel = "/library/parts/" + partId + "?allParts=1";
        if (audioStreamIndex >= 0) sel += "&audioStreamID=" + std::to_string(audioStreamIndex);
        sel += "&subtitleStreamID=" + std::to_string(subtitleStreamIndex >= 0 ? subtitleStreamIndex : 0);
        std::string r;
        int st = httpRequest(serverUrl + sel, "PUT", "", "", auth.accessToken, r);
        if (st != 200) SYS_Report("[Transcode] track selection: HTTP %d\n", st);
    }
    measureLink(serverUrl, auth);
    int kbps = plexBitrate(effectiveQuality()) / 1000;
    /* a little before the position: the stream opens on a keyframe */
    long long ms = startTimeTicks / TICKS_PER_MS;
    ms = ms > 3000 ? ms - 3000 : 0;
    char sid[24];
    snprintf(sid, sizeof(sid), "wiit%u%u", (unsigned)nowMs(), ++s_session);
    outPlaySessionId = sid;
    char q[512];
    snprintf(q, sizeof(q),
             "path=%s&mediaIndex=%d&partIndex=0&protocol=http"
             "&directPlay=0&directStream=%d&directStreamAudio=0&fastSeek=1&videoResolution=640x480"
             "&maxVideoBitrate=%d&videoQuality=80&offset=%lld.%03lld&session=%s&subtitles=%s&copyts=%d&",
             urlEncode("/library/metadata/" + itemId).c_str(), mediaIndex, forceReencode ? 0 : 1, kbps,
             ms / 1000, ms % 1000, sid, subtitleStreamIndex >= 0 ? "burn" : "none",
             /* as WiiFin's player takes Jellyfin's: times from the stream's
              * start, or the item's own with burned-in subtitles (see
              * PlaySession's itemTimestamps) */
             subtitleStreamIndex >= 0 ? 1 : 0);
    std::string query = std::string(q) + plexClientParams(auth) +
                        "&X-Plex-Client-Profile-Extra=" + urlEncode(PROFILE_EXTRA) +
                        "&X-Plex-Session-Identifier=" + sid;
    /* Plex starts a session's transcode only after its decision ("session
     * lacking decision": 400), asked with the same parameters */
    {
        std::string r;
        int st = httpRequest(serverUrl + "/video/:/transcode/universal/decision?" + query, "GET", "", "",
                             auth.accessToken, r);
        std::string why = jsonGetString(r, "generalDecisionText");
        SYS_Report("[Transcode] decision: HTTP %d, %s\n", st, why.c_str());
        if (st != 200) { errMsg = "Plex decision HTTP " + std::to_string(st); return false; }
    }
    outUrl = addScheme(serverUrl) + "/video/:/transcode/universal/start?" + query;
    SYS_Report("[Transcode] %s, %d kb/s, from %lld ms (%s)\n", itemId.c_str(), kbps, ms,
               transcodeReasons.empty() ? "asked" : transcodeReasons.c_str());
    return true;
}

bool JellyfinClient::plex_getAudioStreamUrl(const std::string& serverUrl, const JellyfinAuth& auth,
                                       const std::string& itemId, long long startTimeTicks,
                                       std::string& outUrl, std::string& outPlaySessionId,
                                       std::string* outPlayMethod, int* outAudioIndex) {
    std::string resp;
    if (!plexGet(serverUrl, auth, "/library/metadata/" + itemId, resp)) return false;
    std::string o = nthObject(resp, "Metadata", 0);
    std::string media = nthObject(o, "Media", 0);
    std::string part = nthObject(media, "Part", 0);
    s_durationMs[itemId] = jsonGetLongLong(o, "duration");
    std::string codec = jsonGetString(media, "audioCodec");
    std::string container = jsonGetString(media, "container");
    int channels = jsonGetInt(media, "audioChannels");
    std::string stream = nthObject(part, "Stream", 0);
    int rate = jsonGetInt(stream, "samplingRate");
    int bitrate = jsonGetInt(media, "bitrate");
    if (outAudioIndex) *outAudioIndex = jsonGetInt(stream, "id");
    bool direct = channels <= 2 && rate <= 48000 &&
                  ((codec == "mp3" && bitrate <= 320) || codec == "flac" || codec == "pcm" || container == "wav");
    char sid[24];
    snprintf(sid, sizeof(sid), "wii%u%u", (unsigned)nowMs(), ++s_session);
    outPlaySessionId = sid;
    if (direct && startTimeTicks <= 0) {
        outUrl = addScheme(serverUrl) + jsonGetString(part, "key") + "?" + plexClientParams(auth);
        if (outPlayMethod) *outPlayMethod = "DirectPlay";
        SYS_Report("[Audio] %s %s: direct\n", container.c_str(), codec.c_str());
        return true;
    }
    long long ms = startTimeTicks / TICKS_PER_MS;
    char q[400];
    snprintf(q, sizeof(q),
             "path=%s&mediaIndex=0&partIndex=0&protocol=http"
             "&directPlay=0&directStream=0&musicBitrate=192&offset=%lld.%03lld&session=%s&",
             urlEncode("/library/metadata/" + itemId).c_str(), ms / 1000, ms % 1000, sid);
    std::string query = std::string(q) + plexClientParams(auth) +
                        "&X-Plex-Client-Profile-Extra=" + urlEncode(PROFILE_EXTRA) + "&X-Plex-Session-Identifier=" + sid;
    {   /* the decision first, as for a video */
        std::string r;
        int st = httpRequest(serverUrl + "/music/:/transcode/universal/decision?" + query, "GET", "", "",
                             auth.accessToken, r);
        if (st != 200) SYS_Report("[Audio] decision: HTTP %d\n", st);
    }
    outUrl = addScheme(serverUrl) + "/music/:/transcode/universal/start.mp3?" + query;
    if (outPlayMethod) *outPlayMethod = "Transcode";
    SYS_Report("[Audio] %s %s %d Hz %d ch: transcoded to MP3\n", container.c_str(), codec.c_str(), rate, channels);
    return true;
}

// ---------------------------------------------------------------------------
// Reports: Plex's timeline (resume points, "Now playing" on the server)
// ---------------------------------------------------------------------------
bool JellyfinClient::plexTimeline(const std::string& serverUrl, const JellyfinAuth& auth,
                                  const std::string& itemId, const char* state, long long positionTicks,
                                  const std::string& session) {
    long long ms = positionTicks / TICKS_PER_MS;
    /* Plex refuses a time past the duration (400): a transcode's times run a
     * little past the file's end */
    long long dur = s_durationMs[itemId];
    if (dur > 0 && ms > dur) ms = dur;
    if (ms < 0) ms = 0;
    std::string p = "/:/timeline?ratingKey=" + itemId + "&key=" + urlEncode("/library/metadata/" + itemId) +
                    "&state=" + state + "&time=" + std::to_string(ms) +
                    "&duration=" + std::to_string(s_durationMs[itemId]) + "&hasMDE=1" +
                    (session.empty() ? "" : "&X-Plex-Session-Identifier=" + session);
    std::string r;
    int st = httpRequest(serverUrl + p, "GET", "", "", auth.accessToken, r);
    return st == 200;
}

bool JellyfinClient::plex_reportPlaybackStart(const std::string& serverUrl, const JellyfinAuth& auth,
                                         const std::string& itemId, const std::string&,
                                         const std::string& playSessionId, const char*, int, int) {
    return plexTimeline(serverUrl, auth, itemId, "playing", 0, playSessionId);
}

bool JellyfinClient::plex_reportPlaybackProgress(const std::string& serverUrl, const JellyfinAuth& auth,
                                            const std::string& itemId, const std::string&,
                                            const std::string& playSessionId, long long positionTicks,
                                            bool isPaused, const char*, int, int) {
    return plexTimeline(serverUrl, auth, itemId, isPaused ? "paused" : "playing", positionTicks, playSessionId);
}

bool JellyfinClient::plex_reportPlaybackStopped(const std::string& serverUrl, const JellyfinAuth& auth,
                                           const std::string& itemId, const std::string&,
                                           const std::string& playSessionId, long long positionTicks) {
    return plexTimeline(serverUrl, auth, itemId, "stopped", positionTicks, playSessionId);
}

bool JellyfinClient::plex_deleteActiveEncoding(const std::string& serverUrl, const JellyfinAuth& auth,
                                          const std::string& playSessionId) {
    if (playSessionId.compare(0, 4, "wiit") != 0) return true;   /* not a transcode */
    std::string r;
    int a = httpRequest(serverUrl + "/video/:/transcode/universal/stop?session=" + playSessionId,
                        "GET", "", "", auth.accessToken, r);
    return a == 200 || a == 404;
}

// ---------------------------------------------------------------------------
// Link speed: Plex has no bitrate test; the quality setting is used as it is
// ---------------------------------------------------------------------------
void JellyfinClient::plex_measureLink(const std::string&, const JellyfinAuth&) {}

/* H.264 at 640 px: the Wii decodes it at these rates (WiiFin's direct play
 * limit for H.264 is 2.5 Mb/s) */
static int plexBitrate(int q) {
    static const int bitrates[JellyfinClient::VIDEO_QUALITY_COUNT] = { 1000000, 1500000, 2000000, 2500000 };
    return bitrates[(q >= 0 && q < JellyfinClient::VIDEO_QUALITY_COUNT) ? q : 1];
}
