#include "hound_client.h"
#include "version.h"
#include "http_client.h"
#include "https_client.h"
#include "vendor/cJSON.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <utility>

// The API prefix every Hound endpoint lives under.
static const char* kApi = "/api/v1";

// How this console identifies itself to Hound on login (see the
// authentication docs: X-Client-Id / X-Client-Platform / X-Device-Id).
// The server validates these against its own allow-lists (hound-web,
// hound-app; web, electron-desktop, ios/android variants) and rejects the
// login with a 400 otherwise, so custom values can't be used.
static const char* kClientId = "hound-app";
static const char* kClientPlatform = "web";

HoundClient::HoundClient(std::string host, int port)
    : host_(std::move(host)), port_(port) {}

std::string HoundClient::authHeader() const {
    if (token_.empty()) return "";
    return "Authorization: Bearer " + token_ + "\r\n";
}

static std::string urlEncode(const std::string& in) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : in) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == ':') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

static std::string jsonString(cJSON* obj, const char* key) {
    cJSON* v = cJSON_GetObjectItem(obj, key);
    return cJSON_IsString(v) ? std::string(v->valuestring) : std::string();
}

static int jsonInt(cJSON* obj, const char* key, int fallback) {
    cJSON* v = cJSON_GetObjectItem(obj, key);
    return cJSON_IsNumber(v) ? v->valueint : fallback;
}

static int64_t jsonInt64(cJSON* obj, const char* key, int64_t fallback) {
    cJSON* v = cJSON_GetObjectItem(obj, key);
    return cJSON_IsNumber(v) ? (int64_t)v->valuedouble : fallback;
}

static bool jsonHas(cJSON* obj, const char* key) {
    return obj && cJSON_IsNumber(cJSON_GetObjectItem(obj, key));
}

// "2023-05-16" -> 2023
static int yearOf(const std::string& date) {
    if (date.size() < 4) return 0;
    int y = atoi(date.substr(0, 4).c_str());
    return y > 1800 && y < 3000 ? y : 0;
}

// Minutes -> 100 ns ticks.
static int64_t minutesToTicks(int minutes) {
    return minutes > 0 ? (int64_t)minutes * 60LL * 10000000LL : 0;
}

// --- id scheme helpers -------------------------------------------------

struct ItemId {
    bool valid = false;
    std::string source;      // "tmdb"
    std::string sourceId;    // "1234"
    int season = -1;
    int episode = -1;
    bool isSeason = false;
    bool isEpisode = false;
};

static ItemId parseItemId(const std::string& id) {
    ItemId out;
    if (id.rfind("tmdb-", 0) != 0) return out;
    out.source = "tmdb";
    std::string rest = id.substr(5);
    size_t colon = rest.find(':');
    if (colon == std::string::npos) {
        out.sourceId = rest;
        out.valid = !rest.empty();
        return out;
    }
    out.sourceId = rest.substr(0, colon);
    std::string se = rest.substr(colon + 1); // "s2" or "s2e5"
    size_t e = se.find('e');
    if (!se.empty() && se[0] == 's' && e != std::string::npos) {
        out.season = atoi(se.substr(1, e - 1).c_str());
        out.episode = atoi(se.substr(e + 1).c_str());
        out.isEpisode = out.season >= 0 && out.episode >= 0;
    } else if (!se.empty() && se[0] == 's') {
        out.season = atoi(se.substr(1).c_str());
        out.isSeason = out.season >= 0;
    }
    out.valid = !out.sourceId.empty() && (out.isSeason || out.isEpisode);
    return out;
}

// The movie/show id a season or episode item belongs to.
static std::string showIdOf(const ItemId& parsed) {
    return parsed.source + "-" + parsed.sourceId;
}

// --- envelope -----------------------------------------------------------

// A parsed {status, data} response: the document root (owned, freed by
// putEnvelope) and the "data" value inside it (owned by the root).
struct Envelope {
    cJSON* root = nullptr;
    cJSON* data = nullptr;
    explicit operator bool() const { return data != nullptr; }
};

// "what failed (status N)". When there is no HTTP status at all (DNS,
// connect, timeout, a redirect this client can't follow), the socket-level
// reason from the response body is included; for a JSON error body
// ({"status":"error","error":"..."}) the server's own message is included,
// so the sign-in screen can show why a request was rejected.
static std::string statusError(const HttpResponse& resp, const char* what) {
    std::string detail;
    if (resp.status_code == 0) {
        detail = resp.body;
    } else {
        cJSON* json = cJSON_Parse(resp.body.c_str());
        if (json) {
            cJSON* e = cJSON_GetObjectItem(json, "error");
            if (cJSON_IsString(e) && e->valuestring[0]) detail = e->valuestring;
            cJSON_Delete(json);
        }
    }
    std::string out = std::string(what) + " failed (status " + std::to_string(resp.status_code);
    if (!detail.empty()) out += ": " + detail;
    out += ")";
    return out;
}

// GETs `path` and parses the {status, data} envelope. On failure, err says
// what happened and out is left empty.
static bool getEnvelope(const HoundClient& client, const std::string& auth, const std::string& path,
                        Envelope& out, std::string& err, const char* what) {
    out = Envelope{};
    HttpResponse resp = http_get(client.host(), client.port(), path, auth);
    if (!resp.success) {
        err = statusError(resp, what);
        return false;
    }
    cJSON* json = cJSON_Parse(resp.body.c_str());
    if (!json) {
        err = std::string("could not parse ") + what + " response";
        return false;
    }
    cJSON* data = cJSON_GetObjectItem(json, "data");
    if (!data) {
        err = std::string(what) + " response had no data";
        cJSON_Delete(json);
        return false;
    }
    out.root = json;
    out.data = data;
    return true;
}

// Frees an envelope filled by getEnvelope().
static void putEnvelope(Envelope& env) {
    if (env.root) cJSON_Delete(env.root);
    env.root = env.data = nullptr;
}

// --- catalog items ------------------------------------------------------

// A MediaRecordCatalog (movies, shows, seasons, episodes -- any list
// endpoint's item) into a MediaItem. `showId`/`showTitle` are filled in by
// the caller for episodes and seasons.
static void catalogToItem(cJSON* item, MediaItem& out, const std::string& showId = "",
                          const std::string& showTitle = "") {
    out = MediaItem{};
    const std::string type = jsonString(item, "media_type");
    const std::string source = jsonString(item, "media_source");
    const std::string sourceId = jsonString(item, "source_id");
    if (source.empty() || sourceId.empty()) return;
    const std::string baseId = source + "-" + sourceId;

    out.name = jsonString(item, "media_title");
    out.productionYear = yearOf(jsonString(item, "release_date"));
    out.runTimeTicks = minutesToTicks(jsonInt(item, "duration", 0));

    const std::string thumb = jsonString(item, "thumbnail_uri");
    if (!thumb.empty()) {
        out.imageItemId = thumb;
        out.imageTag = "tmdb";
    }
    const std::string backdrop = jsonString(item, "backdrop_uri");
    if (!backdrop.empty()) {
        out.backdropItemId = backdrop;
        out.backdropTag = "tmdb";
    }

    if (type == "movie") {
        out.type = "Movie";
        out.id = baseId;
    } else if (type == "tvshow") {
        out.type = "Series";
        out.id = baseId;
        out.seriesId = baseId;
    } else if (type == "season") {
        out.type = "Season";
        out.id = showId + ":s" + std::to_string(jsonInt(item, "season_number", 0));
        out.indexNumber = jsonInt(item, "season_number", -1);
        out.seriesId = showId;
        out.seriesName = showTitle;
        if (jsonHas(item, "episode_count")) out.childCount = jsonInt(item, "episode_count", -1);
    } else if (type == "episode") {
        out.type = "Episode";
        const int season = jsonInt(item, "season_number", -1);
        const int episode = jsonInt(item, "episode_number", -1);
        out.id = showId + ":s" + std::to_string(season) + "e" + std::to_string(episode);
        out.indexNumber = episode;
        out.parentIndexNumber = season;
        out.seriesId = showId;
        out.seriesName = showTitle;
    } else {
        out.type = "Video";
        out.id = baseId;
    }
}

// The media files / providers endpoints:
// {providers: [{provider, streams: [{encoded_data, stream_protocol,
// video_metadata: {...}}, ...]}, ...]}. Picks the first stream that has
// encoded data (Hound's proxy URL is built from it) and reports its metadata.
static bool firstStreamIn(cJSON* data, std::string& encodedData, std::string& protocol,
                          int64_t& durationNs, int& width, int& height) {
    if (!data) return false;
    cJSON* providers = cJSON_GetObjectItem(data, "providers");
    if (!cJSON_IsArray(providers)) return false;
    cJSON* provider;
    cJSON_ArrayForEach(provider, providers) {
        cJSON* streams = cJSON_GetObjectItem(provider, "streams");
        if (!cJSON_IsArray(streams)) continue;
        cJSON* stream;
        cJSON_ArrayForEach(stream, streams) {
            std::string enc = jsonString(stream, "encoded_data");
            if (enc.empty()) continue;
            encodedData = enc;
            protocol = jsonString(stream, "stream_protocol");
            cJSON* meta = cJSON_GetObjectItem(stream, "video_metadata");
            if (cJSON_IsObject(meta)) {
                durationNs = jsonInt64(meta, "duration", durationNs);
                width = jsonInt(meta, "width", width);
                height = jsonInt(meta, "height", height);
            }
            return true;
        }
    }
    return false;
}

// --- signing in ---------------------------------------------------------

bool HoundClient::authenticate(const std::string& username, const std::string& password) {
    cJSON* body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "username", username.c_str());
    cJSON_AddStringToObject(body, "password", password.c_str());
    char* body_str = cJSON_PrintUnformatted(body);

    const std::string loginHeaders =
        std::string("X-Client-Id: ") + kClientId + "\r\n" +
        "X-Client-Platform: " + kClientPlatform + "\r\n" +
        "X-Device-Id: " + (device_id_.empty() ? std::string("wiiu-uhound-001") : device_id_) + "\r\n";
    HttpResponse resp = http_post(host_, port_, std::string(kApi) + "/auth/login",
                                   body_str, "application/json", loginHeaders);
    free(body_str);
    cJSON_Delete(body);

    if (!resp.success) {
        // 401 = wrong password, 404 = unknown username; both are the same
        // to the person typing. Anything else (400 bad client headers, 5xx)
        // is a real problem and gets the server's own message back.
        last_error_ = resp.status_code == 401 || resp.status_code == 404
                          ? "Wrong username or password."
                          : statusError(resp, "login");
        return false;
    }

    cJSON* json = cJSON_Parse(resp.body.c_str());
    if (!json) {
        last_error_ = "could not parse login response";
        return false;
    }
    cJSON* data = cJSON_GetObjectItem(json, "data");
    cJSON* token = data ? cJSON_GetObjectItem(data, "token") : nullptr;
    if (!cJSON_IsString(token) || !token->valuestring[0]) {
        last_error_ = "login response had no token";
        cJSON_Delete(json);
        return false;
    }
    token_ = token->valuestring;
    user_name_ = jsonString(data, "display_name");
    if (user_name_.empty()) user_name_ = jsonString(data, "username");
    user_id_ = user_name_; // Hound's API has no user ids; the username is the id
    cJSON_Delete(json);
    return true;
}

void HoundClient::useSavedLogin(const std::string& userId, const std::string& token) {
    user_id_ = userId;
    user_name_ = userId;
    token_ = token;
}

bool HoundClient::validateLogin() {
    if (token_.empty()) {
        last_error_ = "not signed in";
        return false;
    }
    HttpResponse resp = http_get(host_, port_, std::string(kApi) + "/server_info", authHeader());
    if (!resp.success) {
        last_error_ = resp.status_code == 401 ? "the saved sign-in has expired"
                                              : statusError(resp, "checking the saved sign-in");
        return false;
    }
    return true;
}

void HoundClient::signOut() {
    if (!token_.empty()) http_post(host_, port_, std::string(kApi) + "/auth/logout", "",
                                   "application/json", authHeader());
    token_.clear();
    user_id_.clear();
    user_name_.clear();
}

// --- views, lists, search ----------------------------------------------

bool HoundClient::getViews(std::vector<MediaItem>& out) {
    out.clear();
    MediaItem movies;
    movies.type = "CollectionFolder";
    movies.collectionType = "movies";
    movies.id = "ufin:movies";
    movies.name = "Movies";
    out.push_back(movies);

    MediaItem tv;
    tv.type = "CollectionFolder";
    tv.collectionType = "tvshows";
    tv.id = "ufin:tv";
    tv.name = "TV Shows";
    out.push_back(tv);

    MediaItem collections;
    collections.type = "CollectionFolder";
    collections.collectionType = "boxsets";
    collections.id = "ufin:collections";
    collections.name = "Collections";
    out.push_back(collections);
    return true;
}

// A catalog list endpoint (data: [MediaRecordCatalog...]).
static bool getCatalogList(const HoundClient& client, const std::string& path, const char* what,
                           std::vector<MediaItem>& out, std::string& err) {
    Envelope env;
    if (!getEnvelope(client, client.authHeader(), path, env, err, what)) return false;
    cJSON* data = env.data;
    out.clear();
    cJSON* item;
    cJSON_ArrayForEach(item, data) {
        MediaItem mi;
        catalogToItem(item, mi);
        if (!mi.id.empty()) out.push_back(mi);
    }
    putEnvelope(env);
    return true;
}

// A collection's contents (data: {records: [...]}).
static bool getCollectionRecords(const HoundClient& client, const std::string& path, const char* what,
                                 std::vector<MediaItem>& out, std::string& err) {
    Envelope env;
    if (!getEnvelope(client, client.authHeader(), path, env, err, what)) return false;
    cJSON* data = env.data;
    out.clear();
    cJSON* records = cJSON_GetObjectItem(data, "records");
    cJSON* item;
    cJSON_ArrayForEach(item, records) {
        MediaItem mi;
        catalogToItem(item, mi);
        if (!mi.id.empty()) out.push_back(mi);
    }
    putEnvelope(env);
    return true;
}

// A season's episodes, with per-episode resume positions and watched state.
static bool getSeasonEpisodes(const HoundClient& client, const ItemId& parsed,
                              std::vector<MediaItem>& out, std::string& err) {
    const std::string show = showIdOf(parsed);
    const std::string seasonNum = std::to_string(parsed.season);
    Envelope env;
    if (!getEnvelope(client, client.authHeader(),
                     std::string(kApi) + "/tv/" + show + "/season/" + seasonNum,
                     env, err, "episodes")) {
        return false;
    }
    cJSON* data = env.data;

    // The show's title and backdrop, so episode rows can show them.
    std::string showTitle, showBackdrop;
    Envelope showEnv;
    if (getEnvelope(client, client.authHeader(), std::string(kApi) + "/tv/" + show,
                    showEnv, err, "show details")) {
        showTitle = jsonString(showEnv.data, "media_title");
        showBackdrop = jsonString(showEnv.data, "backdrop_uri");
        putEnvelope(showEnv);
    }
    err.clear();

    // Resume positions for the season's episodes.
    std::map<std::pair<int, int>, int64_t> progress; // (season, episode) -> position ticks
    Envelope progressEnv;
    if (getEnvelope(client, client.authHeader(),
                    std::string(kApi) + "/tv/" + show + "/season/" + seasonNum + "/playback",
                    progressEnv, err, "playback progress")) {
        cJSON* progressData = progressEnv.data;
        cJSON* p;
        cJSON_ArrayForEach(p, progressData) {
            const int s = jsonInt(p, "season_number", -1);
            const int e = jsonInt(p, "episode_number", -1);
            const int64_t cur = jsonInt64(p, "current_progress_seconds", 0) * 10000000LL;
            if (e >= 0 && cur > 0) progress[{s, e}] = cur;
        }
        putEnvelope(progressEnv);
    }
    err.clear();

    // Watched state: the season's watch history.
    std::map<std::pair<int, int>, bool> watched;
    Envelope historyEnv;
    if (getEnvelope(client, client.authHeader(),
                    std::string(kApi) + "/tv/" + show + "/season/" + seasonNum + "/history",
                    historyEnv, err, "watch history")) {
        cJSON* historyData = historyEnv.data;
        cJSON* rewatch;
        cJSON_ArrayForEach(rewatch, historyData) {
            cJSON* events = cJSON_GetObjectItem(rewatch, "watch_events");
            cJSON* ev;
            cJSON_ArrayForEach(ev, events) {
                const int s = jsonInt(ev, "season_number", -1);
                const int e = jsonInt(ev, "episode_number", -1);
                if (e >= 0) watched[{s, e}] = true;
            }
        }
        putEnvelope(historyEnv);
    }
    err.clear();

    out.clear();
    cJSON* episodes = cJSON_GetObjectItem(data, "episodes");
    cJSON* item;
    cJSON_ArrayForEach(item, episodes) {
        MediaItem mi;
        catalogToItem(item, mi, show, showTitle);
        if (mi.id.empty()) continue;
        const auto key = std::make_pair(mi.parentIndexNumber, mi.indexNumber);
        const auto pos = progress.find(key);
        if (pos != progress.end()) {
            mi.positionTicks = pos->second;
            if (mi.runTimeTicks > 0)
                mi.playedPercentage = 100.0 * (double)pos->second / (double)mi.runTimeTicks;
        }
        mi.played = watched.count(key) > 0;
        if (!showBackdrop.empty()) {
            mi.backdropItemId = showBackdrop;
            mi.backdropTag = "tmdb";
        }
        out.push_back(mi);
    }
    putEnvelope(env);
    return true;
}

bool HoundClient::getItems(const std::string& parentId, std::vector<MediaItem>& out) {
    std::string err;
    if (parentId == "ufin:movies") {
        const bool ok = getCatalogList(*this, std::string(kApi) + "/catalog/trending-movies", "movies", out, err);
        if (!ok) last_error_ = err;
        return ok;
    }
    if (parentId == "ufin:tv") {
        const bool ok = getCatalogList(*this, std::string(kApi) + "/catalog/trending-shows", "shows", out, err);
        if (!ok) last_error_ = err;
        return ok;
    }
    if (parentId == "ufin:collections") {
        Envelope env;
        if (!getEnvelope(*this, authHeader(), std::string(kApi) + "/collection/all", env, err, "collections")) {
            last_error_ = err;
            return false;
        }
        cJSON* data = env.data;
        out.clear();
        cJSON* item;
        cJSON_ArrayForEach(item, data) {
            MediaItem mi;
            mi.type = "BoxSet";
            mi.id = "collection-" + std::to_string(jsonInt64(item, "collection_id", 0));
            mi.name = jsonString(item, "collection_title");
            const std::string thumb = jsonString(item, "thumbnail_uri");
            if (!thumb.empty()) {
                mi.imageItemId = thumb;
                mi.imageTag = "tmdb";
            }
            out.push_back(mi);
        }
        putEnvelope(env);
        return true;
    }
    if (parentId.rfind("collection-", 0) == 0) {
        const bool ok = getCollectionRecords(*this, std::string(kApi) + "/collection/" + parentId.substr(11),
                                             "collection", out, err);
        if (!ok) last_error_ = err;
        return ok;
    }

    const ItemId parsed = parseItemId(parentId);
    if (!parsed.valid) {
        last_error_ = "unknown view: " + parentId;
        return false;
    }
    if (parsed.isEpisode) {
        last_error_ = "an episode has no contents";
        return false;
    }
    if (parsed.isSeason) {
        const bool ok = getSeasonEpisodes(*this, parsed, out, err);
        if (!ok) last_error_ = err;
        return ok;
    }

    // A show: its seasons.
    Envelope env;
    if (!getEnvelope(*this, authHeader(), std::string(kApi) + "/tv/" + parentId, env, err, "show")) {
        last_error_ = err;
        return false;
    }
    cJSON* data = env.data;
    const std::string showTitle = jsonString(data, "media_title");
    const std::string backdrop = jsonString(data, "backdrop_uri");
    out.clear();
    cJSON* seasons = cJSON_GetObjectItem(data, "seasons");
    cJSON* season;
    cJSON_ArrayForEach(season, seasons) {
        MediaItem mi;
        catalogToItem(season, mi, parentId, showTitle);
        if (mi.id.empty()) continue;
        if (!backdrop.empty()) {
            mi.backdropItemId = backdrop;
            mi.backdropTag = "tmdb";
        }
        out.push_back(mi);
    }
    putEnvelope(env);
    return true;
}

bool HoundClient::search(const std::string& term, std::vector<MediaItem>& out) {
    std::string err;
    Envelope env;
    if (!getEnvelope(*this, authHeader(), std::string(kApi) + "/search?q=" + urlEncode(term),
                     env, err, "search")) {
        last_error_ = err;
        return false;
    }
    cJSON* data = env.data;
    out.clear();
    static const char* groups[] = {"movie_results", "tv_results"};
    for (const char* group : groups) {
        cJSON* item;
        cJSON_ArrayForEach(item, cJSON_GetObjectItem(data, group)) {
            MediaItem mi;
            catalogToItem(item, mi);
            if (!mi.id.empty()) out.push_back(mi);
        }
    }
    putEnvelope(env);
    return true;
}

// --- home rows ----------------------------------------------------------

// GET /continue_watching: a list of "watch actions" -- the movie or the next
// episode the user stopped in, with its progress.
static void watchActionToItem(cJSON* action, MediaItem& out) {
    out = MediaItem{};
    const std::string type = jsonString(action, "media_type");
    const std::string source = jsonString(action, "media_source");
    const std::string sourceId = jsonString(action, "source_id");
    if (source.empty() || sourceId.empty()) return;
    const std::string baseId = source + "-" + sourceId;

    const std::string thumb = jsonString(action, "thumbnail_uri");
    if (!thumb.empty()) {
        out.imageItemId = thumb;
        out.imageTag = "tmdb";
    }
    out.productionYear = yearOf(jsonString(action, "release_date"));

    cJSON* progress = cJSON_GetObjectItem(action, "watch_progress");
    cJSON* nextEp = cJSON_GetObjectItem(action, "next_episode");
    if (type == "movie") {
        out.type = "Movie";
        out.id = baseId;
        out.name = jsonString(action, "media_title");
    } else {
        int season = -1, episode = -1;
        if (cJSON_IsObject(progress)) {
            season = jsonInt(progress, "season_number", -1);
            episode = jsonInt(progress, "episode_number", -1);
        } else if (cJSON_IsObject(nextEp)) {
            season = jsonInt(nextEp, "season_number", -1);
            episode = jsonInt(nextEp, "episode_number", -1);
        }
        if (season < 0 || episode < 0) return;
        out.type = "Episode";
        out.id = baseId + ":s" + std::to_string(season) + "e" + std::to_string(episode);
        out.parentIndexNumber = season;
        out.indexNumber = episode;
        out.seriesId = baseId;
        out.seriesName = jsonString(action, "media_title");
        out.name = jsonString(action, "episode_title");
        if (out.name.empty()) out.name = out.seriesName;
    }
    if (cJSON_IsObject(progress)) {
        out.positionTicks = jsonInt64(progress, "current_progress_seconds", 0) * 10000000LL;
        out.runTimeTicks = jsonInt64(progress, "total_duration_seconds", 0) * 10000000LL;
        if (out.runTimeTicks > 0)
            out.playedPercentage = 100.0 * (double)out.positionTicks / (double)out.runTimeTicks;
    }
}

bool HoundClient::getResume(std::vector<MediaItem>& out) {
    std::string err;
    Envelope env;
    if (!getEnvelope(*this, authHeader(), std::string(kApi) + "/continue_watching", env, err,
                     "continue watching")) {
        last_error_ = err;
        return false;
    }
    cJSON* data = env.data;
    out.clear();
    cJSON* action;
    cJSON_ArrayForEach(action, data) {
        MediaItem mi;
        watchActionToItem(action, mi);
        if (!mi.id.empty()) out.push_back(mi);
    }
    putEnvelope(env);
    return true;
}

bool HoundClient::getNextUp(std::vector<MediaItem>& out) {
    out.clear(); // Hound folds "next up" into its own home rows; nothing to fetch here.
    return true;
}

bool HoundClient::getFavorites(std::vector<MediaItem>& out) {
    std::string err;
    const bool ok = getCollectionRecords(*this, std::string(kApi) + "/collection/watchlist",
                                         "watchlist", out, err);
    if (!ok) last_error_ = err;
    return ok;
}

bool HoundClient::getEpisodes(const std::string& seriesId, std::vector<MediaItem>& out) {
    out.clear();
    const ItemId parsed = parseItemId(seriesId);
    if (!parsed.valid || parsed.isEpisode || parsed.isSeason) {
        last_error_ = "not a show: " + seriesId;
        return false;
    }
    std::string err;
    Envelope env;
    if (!getEnvelope(*this, authHeader(), std::string(kApi) + "/tv/" + seriesId, env, err, "show")) {
        last_error_ = err;
        return false;
    }
    cJSON* data = env.data;
    std::vector<int> seasonNumbers;
    cJSON* seasons = cJSON_GetObjectItem(data, "seasons");
    cJSON* season;
    cJSON_ArrayForEach(season, seasons) {
        const int n = jsonInt(season, "season_number", -1);
        if (n >= 0) seasonNumbers.push_back(n);
    }
    putEnvelope(env);

    std::sort(seasonNumbers.begin(), seasonNumbers.end());
    for (int n : seasonNumbers) {
        std::vector<MediaItem> episodes;
        ItemId seasonId = parsed;
        seasonId.isSeason = true;
        seasonId.season = n;
        if (getSeasonEpisodes(*this, seasonId, episodes, err)) {
            for (const MediaItem& e : episodes) out.push_back(e);
        }
    }
    return true;
}

// --- watchlist + history ------------------------------------------------

// Which title an item's watchlist entry is about: the episode's show, or the
// item itself.
static bool watchlistTarget(const MediaItem& item, std::string& mediaType, std::string& sourceId) {
    const ItemId parsed = parseItemId(item.seriesId.empty() ? item.id : item.seriesId);
    if (!parsed.valid) return false;
    mediaType = item.type == "Movie" ? "movie" : "tvshow";
    sourceId = parsed.sourceId;
    return true;
}

bool HoundClient::setFavorite(const MediaItem& item, bool favorite) {
    std::string mediaType, sourceId;
    if (!watchlistTarget(item, mediaType, sourceId)) {
        last_error_ = "unsupported item: " + item.id;
        return false;
    }
    cJSON* body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "media_source", "tmdb");
    cJSON_AddStringToObject(body, "media_type", mediaType.c_str());
    cJSON_AddStringToObject(body, "source_id", sourceId.c_str());
    char* bodyStr = cJSON_PrintUnformatted(body);
    const std::string bodyOwned = bodyStr;
    free(bodyStr);
    cJSON_Delete(body);

    HttpResponse resp;
    if (favorite) {
        resp = http_post(host_, port_, std::string(kApi) + "/collection/watchlist", bodyOwned,
                         "application/json", authHeader());
    } else {
        resp = http_delete(host_, port_, std::string(kApi) + "/collection/watchlist", authHeader(),
                           bodyOwned);
    }
    if (!resp.success) {
        last_error_ = statusError(resp, favorite ? "adding to the watchlist" : "removing from the watchlist");
        return false;
    }
    return true;
}

bool HoundClient::setPlayed(const MediaItem& item, bool played) {
    const ItemId parsed = parseItemId(item.seriesId.empty() ? item.id : item.seriesId);
    if (!parsed.valid) {
        last_error_ = "unsupported item: " + item.id;
        return false;
    }
    const bool isEpisode = item.type == "Episode" || parsed.isEpisode;
    const std::string base = showIdOf(parsed);

    if (played) {
        cJSON* body = cJSON_CreateObject();
        cJSON_AddStringToObject(body, "action_type", "scrobble");
        if (isEpisode) {
            cJSON_AddNumberToObject(body, "season_number", item.parentIndexNumber);
            cJSON_AddNumberToObject(body, "episode_number", item.indexNumber);
        }
        char* bodyStr = cJSON_PrintUnformatted(body);
        const std::string path = isEpisode ? std::string(kApi) + "/tv/" + base + "/history"
                                           : std::string(kApi) + "/movie/" + base + "/history";
        HttpResponse resp = http_post(host_, port_, path, bodyStr, "application/json", authHeader());
        free(bodyStr);
        cJSON_Delete(body);
        if (!resp.success) {
            last_error_ = statusError(resp, "marking watched");
            return false;
        }
        return true;
    }

    // Unwatch: collect the item's watch events, then delete them by id.
    std::string err;
    Envelope env;
    if (!getEnvelope(*this, authHeader(),
                     isEpisode ? std::string(kApi) + "/tv/" + base + "/season/" +
                                     std::to_string(item.parentIndexNumber) + "/history"
                               : std::string(kApi) + "/movie/" + base + "/history",
                     env, err, "watch history")) {
        last_error_ = err;
        return false;
    }
    cJSON* data = env.data;
    std::vector<int64_t> eventIds;
    cJSON* rewatch;
    cJSON_ArrayForEach(rewatch, data) {
        cJSON* events = cJSON_GetObjectItem(rewatch, "watch_events");
        cJSON* ev;
        cJSON_ArrayForEach(ev, events) {
            if (isEpisode && (jsonInt(ev, "season_number", -1) != item.parentIndexNumber ||
                              jsonInt(ev, "episode_number", -1) != item.indexNumber)) continue;
            const int64_t id = jsonInt64(ev, "watch_event_id", 0);
            if (id > 0) eventIds.push_back(id);
        }
    }
    putEnvelope(env);

    if (eventIds.empty()) return true; // nothing recorded; already unwatched
    cJSON* del = cJSON_CreateObject();
    cJSON* arr = cJSON_AddArrayToObject(del, "watch_event_ids");
    for (int64_t id : eventIds) cJSON_AddItemToArray(arr, cJSON_CreateNumber((double)id));
    char* bodyStr = cJSON_PrintUnformatted(del);
    const std::string path = isEpisode ? std::string(kApi) + "/tv/" + base + "/history/delete"
                                       : std::string(kApi) + "/movie/" + base + "/history/delete";
    HttpResponse resp = http_post(host_, port_, path, bodyStr, "application/json", authHeader());
    free(bodyStr);
    cJSON_Delete(del);
    if (!resp.success) {
        last_error_ = statusError(resp, "unmarking watched");
        return false;
    }
    return true;
}

// --- playback -----------------------------------------------------------

// Resolves what to play for an item: its media files (files the server has
// downloaded) first, then Stremio provider streams (P2P / debrid). Remembers
// the choice in play_context_ for the playback reports.
bool HoundClient::resolveStream(const std::string& itemId) {
    const ItemId parsed = parseItemId(itemId);
    if (!parsed.valid) {
        last_error_ = "unsupported item: " + itemId;
        return false;
    }
    const bool isEpisode = parsed.isEpisode;
    const std::string base = showIdOf(parsed);

    std::string query;
    if (isEpisode) {
        query = "?season=" + std::to_string(parsed.season) + "&episode=" + std::to_string(parsed.episode);
    }
    const std::string mediaPath = isEpisode ? std::string(kApi) + "/tv/" + base + "/media_files" + query
                                            : std::string(kApi) + "/movie/" + base + "/media_files";
    const std::string providerPath = isEpisode ? std::string(kApi) + "/tv/" + base + "/providers" + query
                                               : std::string(kApi) + "/movie/" + base + "/providers";

    std::string err;
    std::string encoded, protocol;
    int64_t durationNs = 0;
    int width = 0, height = 0;
    bool found = false;

    Envelope env;
    if (getEnvelope(*this, authHeader(), mediaPath, env, err, "media files")) {
        found = firstStreamIn(env.data, encoded, protocol, durationNs, width, height);
        putEnvelope(env);
    }
    if (!found) {
        err.clear();
        if (getEnvelope(*this, authHeader(), providerPath, env, err, "providers")) {
            found = firstStreamIn(env.data, encoded, protocol, durationNs, width, height);
            putEnvelope(env);
        }
    }
    if (!found) {
        last_error_ = "no playable stream found for this title (the server has no media file "
                      "and no provider returned one)";
        return false;
    }

    // Ask the server to decode the stream details: the file's own metadata
    // (duration, dimensions) comes back when it knows it.
    cJSON* body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "encoded_data", encoded.c_str());
    char* bodyStr = cJSON_PrintUnformatted(body);
    HttpResponse resp = http_post(host_, port_, std::string(kApi) + "/decode", bodyStr,
                                  "application/json", authHeader());
    free(bodyStr);
    cJSON_Delete(body);
    if (resp.success) {
        cJSON* json = cJSON_Parse(resp.body.c_str());
        if (json) {
            cJSON* decoded = cJSON_GetObjectItem(json, "data");
            cJSON* meta = decoded ? cJSON_GetObjectItem(decoded, "video_metadata") : nullptr;
            if (cJSON_IsObject(meta)) {
                durationNs = jsonInt64(meta, "duration", durationNs);
                width = jsonInt(meta, "width", width);
                height = jsonInt(meta, "height", height);
            }
            cJSON_Delete(json);
        }
    }

    play_context_ = PlayContext{};
    play_context_.itemId = itemId;
    play_context_.mediaType = isEpisode ? "tvshow" : "movie";
    play_context_.sourceId = parsed.sourceId;
    play_context_.encodedData = encoded;
    play_context_.streamProtocol = protocol;
    play_context_.season = parsed.season;
    play_context_.episode = parsed.episode;
    play_context_.totalDurationSeconds = durationNs > 0 ? (int)(durationNs / 1000000000LL) : 0;
    play_context_.width = width;
    play_context_.height = height;
    return true;
}

bool HoundClient::getVideoInfo(const std::string& itemId, VideoInfo& out) {
    out = VideoInfo{};

    const ItemId parsed = parseItemId(itemId);
    if (!parsed.valid) {
        last_error_ = "unsupported item: " + itemId;
        return false;
    }
    const std::string base = showIdOf(parsed);

    // Duration from the item's metadata (minutes).
    std::string err;
    if (parsed.isEpisode) {
        Envelope env;
        if (getEnvelope(*this, authHeader(),
                        std::string(kApi) + "/tv/" + base + "/season/" + std::to_string(parsed.season),
                        env, err, "episodes")) {
            cJSON* episodes = cJSON_GetObjectItem(env.data, "episodes");
            cJSON* ep;
            cJSON_ArrayForEach(ep, episodes) {
                if (jsonInt(ep, "episode_number", -1) == parsed.episode) {
                    out.runTimeTicks = minutesToTicks(jsonInt(ep, "duration", 0));
                    break;
                }
            }
            putEnvelope(env);
        }
    } else {
        Envelope env;
        if (getEnvelope(*this, authHeader(), std::string(kApi) + "/movie/" + base, env, err, "movie")) {
            out.runTimeTicks = minutesToTicks(jsonInt(env.data, "runtime", 0));
            putEnvelope(env);
        }
    }

    // The chosen stream's own metadata (dimensions, exact duration), when it
    // has any. Also primes play_context_ for the playback reports.
    if (resolveStream(itemId)) {
        if (play_context_.totalDurationSeconds > 0)
            out.runTimeTicks = (int64_t)play_context_.totalDurationSeconds * 10000000LL;
        out.width = play_context_.width;
        out.height = play_context_.height;
        if (out.width > 0 && out.height > 0)
            out.displayAspect = (double)out.width / (double)out.height;
    }
    return true;
}

StreamTarget HoundClient::buildVideoStreamUrl(const std::string& itemId, const VideoStreamOptions& options) {
    StreamTarget target;
    target.host = host_;
    target.port = port_;

    if (play_context_.itemId != itemId || play_context_.encodedData.empty()) {
        if (!resolveStream(itemId)) return target; // empty path = error for the caller
    }
    // Hound proxies every stream through this one endpoint (no transcode
    // parameters -- the file plays as it is). The Wii U can only decode
    // H.264 (the console's hardware decoder), so HEVC/x265 releases fail in
    // the decoder with a clear error until Hound's transcoding (WIP upstream)
    // covers them. MKV/AVI containers demux fine with the FFmpeg build the
    // docs describe (patches/ffmpeg-wiiu-mkv.patch).
    target.path = std::string(kApi) + "/stream/" + play_context_.encodedData;
    (void)options;
    return target;
}

StreamTarget HoundClient::buildAudioStreamUrl(const std::string& itemId, int64_t startTimeTicks,
                                              const std::string& playSessionId) const {
    (void)itemId;
    (void)startTimeTicks;
    (void)playSessionId;
    return StreamTarget{}; // Hound has no music libraries
}

// --- images -------------------------------------------------------------

std::string HoundClient::buildBackdropPath(const std::string& backdropItemId, const std::string& backdropTag,
                                           int maxW, int maxH) const {
    (void)backdropTag;
    (void)maxW;
    (void)maxH;
    return backdropItemId; // a full URL
}

std::string HoundClient::buildImagePath(const std::string& imageItemId, const std::string& imageTag,
                                        int width, int height) const {
    (void)imageTag;
    (void)width;
    (void)height;
    return imageItemId; // a full URL
}

bool HoundClient::fetchBinary(const std::string& path, std::string& out) const {
    if (path.rfind("https://", 0) == 0) {
        std::string host, urlPath;
        int port = 443;
        if (!https::splitUrl(path, host, port, urlPath)) return false;
        https::Options options;
        options.port = port;
        options.timeoutSeconds = 12;
        options.maxBytes = 12 * 1024 * 1024; // a full-screen backdrop JPEG
        https::Response resp = https::get(host, urlPath, "", options);
        if (!resp.ok || resp.status != 200) return false;
        out = resp.body;
        return true;
    }
    if (path.rfind("http://", 0) == 0) {
        std::string rest = path.substr(7);
        const size_t slash = rest.find('/');
        if (slash == std::string::npos) return false;
        std::string host = rest.substr(0, slash);
        const std::string urlPath = rest.substr(slash);
        int port = 80;
        const size_t colon = host.rfind(':');
        if (colon != std::string::npos) {
            port = atoi(host.substr(colon + 1).c_str());
            host = host.substr(0, colon);
        }
        HttpResponse resp = http_get(host, port, urlPath, "");
        if (!resp.success) return false;
        out.swap(resp.body);
        return true;
    }
    HttpResponse resp = http_get(host_, port_, path, authHeader());
    if (!resp.success) return false;
    out.swap(resp.body);
    return true;
}

// --- playback reports ---------------------------------------------------

bool HoundClient::reportPlaybackStart(const std::string& itemId, const PlaybackIds& ids) {
    (void)itemId;
    (void)ids;
    return true; // Hound has no session events; the progress POSTs say everything.
}

// POST /movie|tv/<id>/playback. The server ignores anything under 2 minutes
// watched (and rejects a total under 60 s), so tiny reports are skipped
// here rather than round-tripped.
bool HoundClient::reportPlaybackProgress(const std::string& itemId, int64_t positionTicks,
                                         const PlaybackIds& ids, bool isPaused) {
    (void)ids;
    (void)isPaused;
    if (play_context_.itemId != itemId || play_context_.encodedData.empty()) return true;
    const int current = (int)(positionTicks / 10000000LL);
    const int total = play_context_.totalDurationSeconds;
    if (current < 120 || total < 60) return true;

    cJSON* body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "encoded_data", play_context_.encodedData.c_str());
    if (!play_context_.streamProtocol.empty())
        cJSON_AddStringToObject(body, "stream_protocol", play_context_.streamProtocol.c_str());
    cJSON_AddStringToObject(body, "client_platform", kClientPlatform);
    cJSON_AddStringToObject(body, "media_type", play_context_.mediaType.c_str());
    cJSON_AddStringToObject(body, "media_source", "tmdb");
    cJSON_AddStringToObject(body, "source_id", play_context_.sourceId.c_str());
    cJSON_AddNumberToObject(body, "current_progress_seconds", (double)current);
    cJSON_AddNumberToObject(body, "total_duration_seconds", (double)total);
    if (play_context_.season >= 0) cJSON_AddNumberToObject(body, "season_number", play_context_.season);
    if (play_context_.episode >= 0) cJSON_AddNumberToObject(body, "episode_number", play_context_.episode);
    char* bodyStr = cJSON_PrintUnformatted(body);

    const std::string path = play_context_.mediaType == "tvshow"
                                 ? std::string(kApi) + "/tv/tmdb-" + play_context_.sourceId + "/playback"
                                 : std::string(kApi) + "/movie/tmdb-" + play_context_.sourceId + "/playback";
    HttpResponse resp = http_post(host_, port_, path, bodyStr, "application/json", authHeader());
    free(bodyStr);
    cJSON_Delete(body);
    return resp.success;
}

bool HoundClient::reportPlaybackStopped(const std::string& itemId, int64_t positionTicks,
                                        const PlaybackIds& ids) {
    // The same POST: the server marks the item watched (and clears the
    // resume point) when the remaining time is under its cutoff.
    return reportPlaybackProgress(itemId, positionTicks, ids, false);
}
