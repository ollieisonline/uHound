// Exercises HoundClient (and the real http_client underneath it) against
// a fake Hound server: authentication (envelope + client id headers),
// browsing (catalog, collections, shows, seasons), search, home rows,
// watchlist/history toggles, stream resolution, and playback reporting.
#include "check.h"
#include <cstring>
#include "fake_http_server.h"
#include "hound_client.h"

#include <thread>

static bool contains(const std::string& s, const std::string& needle) {
    return s.find(needle) != std::string::npos;
}

// A MediaRecordCatalog movie, as /catalog/trending-movies returns.
static const char* kMovieJson =
    "{\"media_type\":\"movie\",\"media_source\":\"tmdb\",\"source_id\":\"123\","
    "\"media_title\":\"Some Movie\",\"overview\":\"A plot.\",\"duration\":120,"
    "\"release_date\":\"2023-05-16\","
    "\"thumbnail_uri\":\"https://image.tmdb.org/t/p/w300/poster.jpg\","
    "\"backdrop_uri\":\"https://image.tmdb.org/t/p/w1280/backdrop.jpg\"}";

static const char* kShowJson =
    "{\"media_type\":\"tvshow\",\"media_source\":\"tmdb\",\"source_id\":\"100\","
    "\"media_title\":\"Some Show\",\"duration\":45,\"release_date\":\"2019-01-01\","
    "\"thumbnail_uri\":\"https://image.tmdb.org/t/p/w300/show.jpg\","
    "\"backdrop_uri\":\"https://image.tmdb.org/t/p/w1280/showback.jpg\"}";

static const char* kSeasonsJson =
    "{\"media_type\":\"tvshow\",\"media_source\":\"tmdb\",\"source_id\":\"100\","
    "\"media_title\":\"Some Show\",\"backdrop_uri\":\"https://image.tmdb.org/t/p/w1280/showback.jpg\","
    "\"seasons\":["
    "{\"media_type\":\"season\",\"media_source\":\"tmdb\",\"source_id\":\"100\","
    "\"season_number\":1,\"name\":\"Season 1\",\"episode_count\":2,"
    "\"thumbnail_uri\":\"https://image.tmdb.org/t/p/w300/s1.jpg\"},"
    "{\"media_type\":\"season\",\"media_source\":\"tmdb\",\"source_id\":\"100\","
    "\"season_number\":2,\"name\":\"Season 2\",\"episode_count\":1}]}";

static const char* kEpisodesJson =
    "{\"episodes\":["
    "{\"media_type\":\"episode\",\"media_source\":\"tmdb\",\"source_id\":\"1001\","
    "\"media_title\":\"Pilot\",\"duration\":42,\"season_number\":1,\"episode_number\":1,"
    "\"thumbnail_uri\":\"https://image.tmdb.org/t/p/w300/e1.jpg\"},"
    "{\"media_type\":\"episode\",\"media_source\":\"tmdb\",\"source_id\":\"1002\","
    "\"media_title\":\"Episode Two\",\"duration\":44,\"season_number\":1,\"episode_number\":2}]}";

// The media-files/providers shape: {providers: [{provider, streams: [...]}]}.
static const char* kMediaFilesJson =
    "{\"providers\":[{\"provider\":\"local\",\"streams\":[{"
    "\"encoded_data\":\"ENC123\",\"stream_protocol\":\"file\","
    "\"video_metadata\":{\"duration\":7200000000000,\"width\":1920,\"height\":1080,"
    "\"streams\":[]}}]}]}";

static const char* kProvidersJson =
    "{\"providers\":[{\"provider\":\"stremio\",\"streams\":[{"
    "\"encoded_data\":\"PROV9\",\"stream_protocol\":\"http\","
    "\"video_metadata\":{\"duration\":5400000000000,\"width\":1280,\"height\":720}}]}]}";

static const char* kDecodeJson =
    "{\"status\":\"success\",\"data\":{\"encoded_data\":\"\",\"uri\":\"\","
    "\"video_metadata\":{\"duration\":7200000000000,\"width\":1920,\"height\":1080}}}";

int main() {
    FakeHttpServer server;
    CHECK(server.start());

    // --- routes ------------------------------------------------------------
    FakeRoute login;
    login.body = "{\"status\":\"success\",\"data\":{\"token\":\"tok123\",\"username\":\"wiiu\","
                 "\"display_name\":\"Wii U\",\"role\":\"user\"}}";
    server.addRoute("/api/v1/auth/login", login);

    FakeRoute denied;
    denied.status = 401;
    denied.body = "{\"status\":\"error\",\"error\":\"bad credentials\"}";

    FakeRoute serverInfo;
    serverInfo.body = "{\"status\":\"success\",\"data\":{\"server_id\":\"s1\",\"version\":\"1.0\"}}";
    server.addRoute("/api/v1/server_info", serverInfo);

    FakeRoute expired;
    expired.status = 401;
    expired.body = "{\"status\":\"error\"}";

    FakeRoute emptyOk;
    emptyOk.body = "{\"status\":\"success\",\"data\":{}}";

    FakeRoute trendingMovies;
    trendingMovies.body = std::string("{\"status\":\"success\",\"data\":[") + kMovieJson + "]}";
    server.addRoute("/api/v1/catalog/trending-movies", trendingMovies);

    FakeRoute trendingShows;
    trendingShows.body = std::string("{\"status\":\"success\",\"data\":[") + kShowJson + "]}";
    server.addRoute("/api/v1/catalog/trending-shows", trendingShows);

    FakeRoute collections;
    collections.body = "{\"status\":\"success\",\"data\":[{\"collection_id\":5,"
                       "\"collection_title\":\"My List\",\"kind\":\"manual\","
                       "\"thumbnail_uri\":\"https://image.tmdb.org/t/p/w300/c.jpg\"}]}";
    server.addRoute("/api/v1/collection/all", collections);

    FakeRoute collection5;
    collection5.body = std::string("{\"status\":\"success\",\"data\":{\"records\":[") + kMovieJson + "]}}";
    server.addRoute("/api/v1/collection/5", collection5);

    FakeRoute watchlist;
    watchlist.body = std::string("{\"status\":\"success\",\"data\":{\"records\":[") + kShowJson + "]}}";
    server.addRoute("/api/v1/collection/watchlist", watchlist);

    FakeRoute show;
    show.body = std::string("{\"status\":\"success\",\"data\":") + kSeasonsJson + "}";
    server.addRoute("/api/v1/tv/tmdb-100", show);

    FakeRoute season1;
    season1.body = std::string("{\"status\":\"success\",\"data\":") + kEpisodesJson + "}";
    server.addRoute("/api/v1/tv/tmdb-100/season/1", season1);

    FakeRoute seasonPlayback;
    seasonPlayback.body = "{\"status\":\"success\",\"data\":[{"
                          "\"season_number\":1,\"episode_number\":1,"
                          "\"current_progress_seconds\":600,\"total_duration_seconds\":2520}]}";
    server.addRoute("/api/v1/tv/tmdb-100/season/1/playback", seasonPlayback);

    FakeRoute seasonHistory;
    seasonHistory.body = "{\"status\":\"success\",\"data\":[{\"watch_events\":["
                         "{\"watch_event_id\":11,\"season_number\":1,\"episode_number\":2}]}]}";
    server.addRoute("/api/v1/tv/tmdb-100/season/1/history", seasonHistory);

    FakeRoute movieHistory;
    movieHistory.body = "{\"status\":\"success\",\"data\":[{\"watch_events\":["
                        "{\"watch_event_id\":7}]}]}";
    server.addRoute("/api/v1/movie/tmdb-123/history", movieHistory);

    FakeRoute deleted;
    deleted.body = "{\"status\":\"success\",\"data\":{}}";
    server.addRoute("/api/v1/movie/tmdb-123/history/delete", deleted);
    server.addRoute("/api/v1/tv/tmdb-100/history/delete", deleted);
    server.addRoute("/api/v1/movie/tmdb-999/playback", deleted);
    server.addRoute("/api/v1/tv/tmdb-100/playback", deleted);
    server.addRoute("/api/v1/tv/tmdb-100/history", deleted);
    // /api/v1/movie/tmdb-123/history keeps its GET route (the unwatch lookup
    // reads watch_event_id 7 from it); the scrobble POST only needs a 2xx.

    FakeRoute search;
    search.body = std::string("{\"status\":\"success\",\"data\":{\"movie_results\":[") + kMovieJson +
                  "],\"tv_results\":[" + kShowJson + "]}}";
    server.addRoute("/api/v1/search", search);

    FakeRoute continueWatching;
    continueWatching.body = "{\"status\":\"success\",\"data\":["
                            // A movie, part-watched.
                            "{\"media_type\":\"movie\",\"media_source\":\"tmdb\",\"source_id\":\"123\","
                            "\"watch_action_type\":\"resume\","
                            "\"watch_progress\":{\"season_number\":-1,\"episode_number\":-1,"
                            "\"current_progress_seconds\":3000,\"total_duration_seconds\":7200},"
                            "\"media_title\":\"Some Movie\",\"episode_title\":\"\","
                            "\"release_date\":\"2023-05-16\","
                            "\"thumbnail_uri\":\"https://image.tmdb.org/t/p/w300/poster.jpg\"},"
                            // A show the user finished an episode of: the next one.
                            "{\"media_type\":\"tvshow\",\"media_source\":\"tmdb\",\"source_id\":\"100\","
                            "\"watch_action_type\":\"next\","
                            "\"watch_progress\":null,"
                            "\"next_episode\":{\"season_number\":1,\"episode_number\":2,"
                            "\"episode_source_id\":\"1002\"},"
                            "\"media_title\":\"Some Show\",\"episode_title\":\"Episode Two\","
                            "\"release_date\":\"2019-01-01\","
                            "\"thumbnail_uri\":\"https://image.tmdb.org/t/p/w300/show.jpg\"}]}";
    server.addRoute("/api/v1/continue_watching", continueWatching);

    FakeRoute movieDetail;
    movieDetail.body = "{\"status\":\"success\",\"data\":{\"media_type\":\"movie\",\"runtime\":120}}";
    server.addRoute("/api/v1/movie/tmdb-123", movieDetail);

    FakeRoute mediaFiles;
    mediaFiles.body = std::string("{\"status\":\"success\",\"data\":") + kMediaFilesJson + "}";
    server.addRoute("/api/v1/movie/tmdb-123/media_files", mediaFiles);
    server.addRoute("/api/v1/tv/tmdb-100/media_files", mediaFiles);

    FakeRoute providers;
    providers.body = std::string("{\"status\":\"success\",\"data\":") + kProvidersJson + "}";
    server.addRoute("/api/v1/movie/tmdb-999/providers", providers);

    FakeRoute noStreams;
    noStreams.body = "{\"status\":\"success\",\"data\":{\"providers\":[]}}";
    server.addRoute("/api/v1/movie/tmdb-404/media_files", noStreams);
    server.addRoute("/api/v1/movie/tmdb-404/providers", noStreams);

    FakeRoute decode;
    decode.body = kDecodeJson;
    server.addRoute("/api/v1/decode", decode);

    FakeRoute image;
    image.contentType = "image/jpeg";
    image.body = "JPEGBYTES";
    server.addRoute("/img.jpg", image);

    HoundClient client("127.0.0.1", server.port());
    client.setDeviceId("test-device");

    // --- authentication ----------------------------------------------------
    {
        FakeHttpServer badServer;
        CHECK(badServer.start());
        badServer.addRoute("/api/v1/auth/login", denied);
        HoundClient bad("127.0.0.1", badServer.port());
        CHECK(!bad.authenticate("u", "p"));
        CHECK(contains(bad.lastError(), "Wrong username or password"));
    }

    CHECK(client.authenticate("wiiu", "secret"));
    {
        auto reqs = server.requests();
        CHECK(!reqs.empty());
        CHECK_STR(reqs.back().method, "POST");
        CHECK_STR(reqs.back().path, "/api/v1/auth/login");
        CHECK(contains(reqs.back().body, "\"username\":\"wiiu\""));
        CHECK(contains(reqs.back().body, "\"password\":\"secret\""));
        CHECK(contains(reqs.back().headers, "X-Client-Id: uhound-wiiu"));
        CHECK(contains(reqs.back().headers, "X-Client-Platform: wiiu"));
        CHECK(contains(reqs.back().headers, "X-Device-Id: test-device"));
    }
    CHECK_STR(client.userName(), "Wii U");
    CHECK_STR(client.accessToken(), "tok123");

    // A saved token validates against /server_info...
    {
        HoundClient saved("127.0.0.1", server.port());
        saved.useSavedLogin("wiiu", "tok123");
        CHECK(saved.validateLogin());
        auto reqs = server.requests();
        CHECK_STR(reqs.back().path, "/api/v1/server_info");
        CHECK(contains(reqs.back().headers, "Authorization: Bearer tok123"));
    }
    // ...and an expired one is reported as such.
    {
        FakeHttpServer expServer;
        CHECK(expServer.start());
        expServer.addRoute("/api/v1/server_info", expired);
        HoundClient saved("127.0.0.1", expServer.port());
        saved.useSavedLogin("wiiu", "old");
        CHECK(!saved.validateLogin());
        CHECK(contains(saved.lastError(), "expired"));
    }

    // --- views and browsing ------------------------------------------------
    std::vector<MediaItem> list;
    CHECK(client.getViews(list));
    CHECK_EQ((int)list.size(), 3);
    CHECK_STR(list[0].id, "ufin:movies");
    CHECK_STR(list[0].collectionType, "movies");
    CHECK_STR(list[1].id, "ufin:tv");
    CHECK_STR(list[2].id, "ufin:collections");

    CHECK(client.getItems("ufin:movies", list));
    CHECK_EQ((int)list.size(), 1);
    CHECK_STR(list[0].id, "tmdb-123");
    CHECK_STR(list[0].type, "Movie");
    CHECK_STR(list[0].name, "Some Movie");
    CHECK_EQ(list[0].productionYear, 2023);
    CHECK_EQ(list[0].runTimeTicks, 120LL * 60 * 10000000LL);
    CHECK_STR(list[0].imageItemId, "https://image.tmdb.org/t/p/w300/poster.jpg");
    CHECK_STR(list[0].imageTag, "tmdb");
    CHECK_STR(list[0].backdropItemId, "https://image.tmdb.org/t/p/w1280/backdrop.jpg");

    CHECK(client.getItems("ufin:tv", list));
    CHECK_EQ((int)list.size(), 1);
    CHECK_STR(list[0].id, "tmdb-100");
    CHECK_STR(list[0].type, "Series");

    CHECK(client.getItems("ufin:collections", list));
    CHECK_EQ((int)list.size(), 1);
    CHECK_STR(list[0].id, "collection-5");
    CHECK_STR(list[0].type, "BoxSet");
    CHECK_STR(list[0].name, "My List");

    CHECK(client.getItems("collection-5", list));
    CHECK_EQ((int)list.size(), 1);
    CHECK_STR(list[0].id, "tmdb-123");

    // A show: its seasons, with the show's backdrop folded in.
    CHECK(client.getItems("tmdb-100", list));
    CHECK_EQ((int)list.size(), 2);
    CHECK_STR(list[0].id, "tmdb-100:s1");
    CHECK_STR(list[0].type, "Season");
    CHECK_EQ(list[0].indexNumber, 1);
    CHECK_EQ(list[0].childCount, 2);
    CHECK_STR(list[0].seriesName, "Some Show");
    CHECK_STR(list[1].id, "tmdb-100:s2");

    // A season: its episodes, with resume positions and watched state.
    CHECK(client.getItems("tmdb-100:s1", list));
    CHECK_EQ((int)list.size(), 2);
    CHECK_STR(list[0].id, "tmdb-100:s1e1");
    CHECK_STR(list[0].type, "Episode");
    CHECK_STR(list[0].name, "Pilot");
    CHECK_EQ(list[0].indexNumber, 1);
    CHECK_EQ(list[0].parentIndexNumber, 1);
    CHECK_STR(list[0].seriesId, "tmdb-100");
    CHECK_EQ(list[0].runTimeTicks, 42LL * 60 * 10000000LL);
    CHECK_EQ(list[0].positionTicks, 600LL * 10000000LL);   // from /playback
    CHECK(list[0].playedPercentage > 0.0);
    CHECK(!list[0].played);
    CHECK(list[1].played);                                  // from /history
    CHECK_STR(list[1].backdropItemId, "https://image.tmdb.org/t/p/w1280/showback.jpg");
    {
        auto reqs = server.requests();
        CHECK(contains(reqs.back().headers, "Authorization: Bearer tok123"));
    }

    // --- search ------------------------------------------------------------
    CHECK(client.search("some", list));
    CHECK_EQ((int)list.size(), 2);
    CHECK_STR(list[0].id, "tmdb-123");
    CHECK_STR(list[0].type, "Movie");
    CHECK_STR(list[1].id, "tmdb-100");
    CHECK_STR(list[1].type, "Series");
    {
        auto reqs = server.requests();
        CHECK(contains(reqs.back().path, "/api/v1/search?q=some"));
    }

    // --- home rows ---------------------------------------------------------
    CHECK(client.getResume(list));
    CHECK_EQ((int)list.size(), 2);
    CHECK_STR(list[0].id, "tmdb-123");
    CHECK_STR(list[0].type, "Movie");
    CHECK_EQ(list[0].positionTicks, 3000LL * 10000000LL);
    CHECK_EQ(list[0].runTimeTicks, 7200LL * 10000000LL);
    CHECK_STR(list[1].id, "tmdb-100:s1e2");                 // the next episode
    CHECK_STR(list[1].type, "Episode");
    CHECK_STR(list[1].name, "Episode Two");
    CHECK_STR(list[1].seriesName, "Some Show");

    std::vector<MediaItem> nextUp;
    CHECK(client.getNextUp(nextUp));                        // no Hound equivalent
    CHECK(nextUp.empty());

    CHECK(client.getFavorites(list));
    CHECK_EQ((int)list.size(), 1);
    CHECK_STR(list[0].id, "tmdb-100");

    // --- watchlist + watched toggles ---------------------------------------
    MediaItem movie = list[0];
    CHECK(client.setFavorite(movie, true));
    {
        auto reqs = server.requests();
        CHECK_STR(reqs.back().method, "POST");
        CHECK_STR(reqs.back().path, "/api/v1/collection/watchlist");
        CHECK(contains(reqs.back().body, "\"media_type\":\"tvshow\""));
        CHECK(contains(reqs.back().body, "\"source_id\":\"100\""));
    }
    CHECK(client.setFavorite(movie, false));
    {
        auto reqs = server.requests();
        CHECK_STR(reqs.back().method, "DELETE");
        CHECK_STR(reqs.back().path, "/api/v1/collection/watchlist");
        CHECK(contains(reqs.back().body, "\"source_id\":\"100\""));
    }

    MediaItem episode;
    episode.id = "tmdb-100:s1e2";
    episode.type = "Episode";
    episode.seriesId = "tmdb-100";
    episode.parentIndexNumber = 1;
    episode.indexNumber = 2;
    CHECK(client.setPlayed(episode, true));
    {
        auto reqs = server.requests();
        CHECK_STR(reqs.back().method, "POST");
        CHECK_STR(reqs.back().path, "/api/v1/tv/tmdb-100/history");
        CHECK(contains(reqs.back().body, "\"action_type\":\"scrobble\""));
        CHECK(contains(reqs.back().body, "\"season_number\":1"));
        CHECK(contains(reqs.back().body, "\"episode_number\":2"));
    }

    // Unwatching an episode deletes exactly its watch events.
    CHECK(client.setPlayed(episode, false));
    {
        auto reqs = server.requests();
        CHECK_STR(reqs.back().method, "POST");
        CHECK_STR(reqs.back().path, "/api/v1/tv/tmdb-100/history/delete");
        CHECK(contains(reqs.back().body, "\"watch_event_ids\":[11]"));
    }

    MediaItem movieItem;
    movieItem.id = "tmdb-123";
    movieItem.type = "Movie";
    CHECK(client.setPlayed(movieItem, true));
    {
        auto reqs = server.requests();
        CHECK_STR(reqs.back().path, "/api/v1/movie/tmdb-123/history");
        CHECK(contains(reqs.back().body, "\"action_type\":\"scrobble\""));
        CHECK(!contains(reqs.back().body, "season_number"));
    }
    CHECK(client.setPlayed(movieItem, false));
    {
        auto reqs = server.requests();
        CHECK_STR(reqs.back().path, "/api/v1/movie/tmdb-123/history/delete");
        CHECK(contains(reqs.back().body, "\"watch_event_ids\":[7]"));
    }

    // --- video info + stream resolution ------------------------------------
    VideoInfo info;
    CHECK(client.getVideoInfo("tmdb-123", info));
    CHECK_EQ(info.runTimeTicks, 120LL * 60 * 10000000LL);   // from /decode's metadata
    CHECK_EQ(info.width, 1920);
    CHECK_EQ(info.height, 1080);
    CHECK(info.displayAspect > 1.77 && info.displayAspect < 1.79);
    {
        auto reqs = server.requests();
        // media_files first, then the decode round-trip.
        CHECK(contains(reqs.back().path, "/api/v1/decode"));
        CHECK(contains(reqs.back().body, "\"encoded_data\":\"ENC123\""));
    }

    StreamTarget target = client.buildVideoStreamUrl("tmdb-123");
    CHECK_STR(target.host, "127.0.0.1");
    CHECK_EQ(target.port, server.port());
    CHECK_STR(target.path, "/api/v1/stream/ENC123");

    // An item with no local media file falls back to its providers.
    VideoInfo provInfo;
    CHECK(client.getVideoInfo("tmdb-999", provInfo));
    StreamTarget provTarget = client.buildVideoStreamUrl("tmdb-999");
    CHECK_STR(provTarget.path, "/api/v1/stream/PROV9");
    {
        auto reqs = server.requests();
        CHECK(contains(reqs.back().body, "\"encoded_data\":\"PROV9\""));
    }

    // Nothing playable anywhere: a clear error, and an empty target.
    VideoInfo none;
    CHECK(client.getVideoInfo("tmdb-404", none));
    StreamTarget bad = client.buildVideoStreamUrl("tmdb-404");
    CHECK(bad.path.empty());
    CHECK(contains(client.lastError(), "no playable stream"));

    // --- playback reporting -------------------------------------------------
    // The stream context is tmdb-999 (providers) after the block above.
    CHECK(client.reportPlaybackStart("tmdb-999"));
    {
        auto reqs = server.requests();
        CHECK(!contains(reqs.back().path, "/playback"));    // start is a no-op
    }
    CHECK(client.reportPlaybackProgress("tmdb-999", 300LL * 10000000LL));
    {
        auto reqs = server.requests();
        CHECK_STR(reqs.back().method, "POST");
        CHECK_STR(reqs.back().path, "/api/v1/movie/tmdb-999/playback");
        CHECK(contains(reqs.back().body, "\"encoded_data\":\"PROV9\""));
        CHECK(contains(reqs.back().body, "\"current_progress_seconds\":300"));
        CHECK(contains(reqs.back().body, "\"total_duration_seconds\":"));
        CHECK(contains(reqs.back().body, "\"media_type\":\"movie\""));
        CHECK(contains(reqs.back().body, "\"client_platform\":\"wiiu\""));
    }
    // Under two minutes watched: skipped locally, no request sent.
    int before = (int)server.requests().size();
    CHECK(client.reportPlaybackProgress("tmdb-999", 60LL * 10000000LL));
    CHECK_EQ((int)server.requests().size(), before);

    // An episode report goes to the tv path, with season and episode.
    CHECK(client.getVideoInfo("tmdb-100:s1e1", info));
    CHECK(client.reportPlaybackProgress("tmdb-100:s1e1", 200LL * 10000000LL));
    {
        auto reqs = server.requests();
        CHECK_STR(reqs.back().path, "/api/v1/tv/tmdb-100/playback");
        CHECK(contains(reqs.back().body, "\"media_type\":\"tvshow\""));
        CHECK(contains(reqs.back().body, "\"season_number\":1"));
        CHECK(contains(reqs.back().body, "\"episode_number\":1"));
    }

    // No stream resolved for this item: reports stay silent no-ops.
    before = (int)server.requests().size();
    CHECK(client.reportPlaybackProgress("tmdb-404", 300LL * 10000000LL));
    CHECK_EQ((int)server.requests().size(), before);

    // --- images -------------------------------------------------------------
    CHECK_STR(client.buildImagePath("https://image.tmdb.org/t/p/w300/poster.jpg", "tmdb", 300, 450),
              "https://image.tmdb.org/t/p/w300/poster.jpg");
    CHECK_STR(client.buildBackdropPath("https://image.tmdb.org/t/p/w1280/backdrop.jpg", "tmdb", 1280, 720),
              "https://image.tmdb.org/t/p/w1280/backdrop.jpg");

    // A plain http:// image URL is fetched from its own host.
    std::string bytes;
    CHECK(client.fetchBinary("http://127.0.0.1:" + std::to_string(server.port()) + "/img.jpg", bytes));
    CHECK_STR(bytes, "JPEGBYTES");

    // --- sign out -----------------------------------------------------------
    client.signOut();
    CHECK(client.accessToken().empty());
    {
        auto reqs = server.requests();
        CHECK_STR(reqs.back().path, "/api/v1/auth/logout");
    }

    return check::finish("test_hound");
}
