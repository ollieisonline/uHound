#pragma once
#include "h264_profile.h"

#include <cstdint>
#include <string>
#include <vector>

// Client for a Hound media server (https://github.com/Hound-Media-Server/hound).
// API: https://hound-media-server.github.io/hound-site/operations/authentication.html
//
// Hound has no libraries/folders: everything is keyed by TMDB ids. This client
// maps Hound's data model onto the browsing model the UI already had
// (views -> lists -> playable items), using synthetic view ids:
//
//   "ufin:movies"      trending movies      (/catalog/trending-movies)
//   "ufin:tv"          trending shows       (/catalog/trending-shows)
//   "ufin:collections" the user's collections (/collection/all)
//   "collection-<id>"  one collection's titles (/collection/<id>)
//
// Real items use Hound's ids, with the season/episode folded in for TV:
//   movie   "tmdb-1234"
//   show    "tmdb-1234"
//   season  "tmdb-1234:s2"      (indexNumber = 2)
//   episode "tmdb-1234:s2e5"    (parentIndexNumber = 2, indexNumber = 5)

struct cJSON;

struct MediaItem {
    std::string id;
    std::string name;
    std::string type;           // "Movie", "Series", "Season", "Episode", "BoxSet", "CollectionFolder", ...
    std::string collectionType; // views only: "movies", "tvshows", "boxsets"
    int64_t runTimeTicks = 0;   // duration in 100 ns ticks, 0 if not provided
    int indexNumber = -1;       // episode / season number, -1 if not provided
    int parentIndexNumber = -1; // season number of an episode, -1 if not provided
    int productionYear = 0;     // 0 if not provided
    std::string channelNumber;
    std::string currentProgram;
    std::string seriesName;     // Episode / Season: the show it belongs to

    // Artwork: Hound hands out full image URLs (TMDB), so this is the URL
    // itself; imageTag is a non-empty marker ("tmdb") so callers can tell
    // "has artwork" from "doesn't". Empty = no artwork.
    std::string imageItemId;
    std::string imageTag;

    // Wide background artwork, same scheme as above.
    std::string backdropItemId;
    std::string backdropTag;

    std::string seriesId;       // Episode / Season: its show ("tmdb-...")

    // Per-user state
    int64_t positionTicks = 0;  // where the user stopped, 0 = not started
    bool played = false;        // watched
    bool favorite = false;
    double playedPercentage = 0.0;
    int unplayedCount = -1;     // -1 = unknown
    int childCount = -1;        // folders: items inside, -1 = unknown
};

// One audio or subtitle track of a video. Kept for the player's track panel;
// Hound streams play the file's own tracks, so this stays empty in practice.
struct MediaTrack {
    int index = -1;
    std::string title;
    std::string language;
    bool isDefault = false;
    bool isTextSubtitle = false;
};

// Everything needed to make HTTP requests against a media stream:
// http_client (and http_stream_io, for playback) work with host/port/path
// separately rather than a single URL string, so this is what
// buildVideoStreamUrl() below returns instead of a plain std::string.
struct StreamTarget {
    std::string host;
    int port = 0;
    std::string path; // includes leading "/" and the full query string
};

// Playback knobs. Jellyfin needed these to steer a server-side transcode;
// Hound streams the file as it is (no transcoding yet), so the fields the
// playback flow still touches are accepted but otherwise ignored.
struct VideoStreamOptions {
    int videoBitrate = 2500000;
    std::string profile = h264::wireName(h264::kDefault);
    int width = 1280;
    int height = 720;
    std::string mediaSourceId;
    std::string liveStreamId;
    std::string playSessionId;
    int64_t startTimeTicks = 0;
    int audioStreamIndex = -2;
    int subtitleStreamIndex = -2;
    bool subtitleIsText = false;
};

// What we need to know about a video before playing it.
struct VideoInfo {
    int width = 0;                // source video dimensions, 0 if unknown
    int height = 0;
    double displayAspect = 0.0;   // width/height the picture should be shown at, 0 if unknown
    int64_t runTimeTicks = 0;     // duration in 100 ns ticks, 0 if unknown
    std::string mediaSourceId;

    std::vector<MediaTrack> audioTracks;
    std::vector<MediaTrack> subtitleTracks;
    int defaultAudioIndex = -1;    // -1 = unknown
    int defaultSubtitleIndex = -1; // -1 = none
};

// Session ids carried around by the playback flow. Hound's playback reports
// don't use them (the stream is identified by its encoded_data, which the
// client remembers from the last buildVideoStreamUrl call), so this stays
// empty in practice.
struct PlaybackIds {
    std::string mediaSourceId;
    std::string liveStreamId;
    std::string playSessionId;
};

class HoundClient {
public:
    HoundClient(std::string host, int port);

    // Logs in with a username/password (POST /api/v1/auth/login, with the
    // client identification headers the docs require) and stashes the
    // bearer token for subsequent calls. Returns false on failure -- check
    // lastError() for details.
    bool authenticate(const std::string& username, const std::string& password);

    // The sidebar's top level: Movies / TV Shows / Collections. The home
    // rows (Continue watching, Watchlist) are fetched separately by the
    // app. See the id scheme in the header comment.
    bool getViews(std::vector<MediaItem>& out);

    // Contents of a view, a collection, a show (its seasons) or a season
    // (its episodes, with watched state and resume positions).
    bool getItems(const std::string& parentId, std::vector<MediaItem>& out);

    // Search movies and shows by name (GET /search).
    bool search(const std::string& term, std::vector<MediaItem>& out);

    // Home rows: what the user stopped watching, and their watchlist.
    // getNextUp has no Hound equivalent (the server's home rows do that
    // server-side); it succeeds with an empty list so the row is skipped.
    bool getResume(std::vector<MediaItem>& out);
    bool getNextUp(std::vector<MediaItem>& out);
    bool getFavorites(std::vector<MediaItem>& out);

    // Every episode of a show, in order (for autoplaying the next one).
    bool getEpisodes(const std::string& seriesId, std::vector<MediaItem>& out);

    // Favourite = membership in the user's watchlist collection (an episode
    // adds/removes its show).
    bool setFavorite(const MediaItem& item, bool favorite);
    // Watched = a scrobble watch event; unwatched deletes those events.
    bool setPlayed(const MediaItem& item, bool played);

    // Fetches the item's metadata (duration, and from the chosen stream's
    // decoded metadata its real aspect ratio). Returns false (with
    // lastError() set) if the request fails; fields that couldn't be
    // determined stay at their zero defaults, so callers should fall back
    // to 16:9.
    bool getVideoInfo(const std::string& itemId, VideoInfo& out);

    // Resolves the item's playable stream (media files first, then Stremio
    // providers) and returns the request target for it: Hound proxies every
    // stream through GET /api/v1/stream/<encoded data>. Not const: it
    // performs the resolution and remembers it for the playback reports.
    StreamTarget buildVideoStreamUrl(const std::string& itemId,
                                     const VideoStreamOptions& options = VideoStreamOptions());

    // Hound has no music libraries; this only exists so the (unreachable)
    // audio path in the player still compiles. Returns an empty target.
    StreamTarget buildAudioStreamUrl(const std::string& itemId, int64_t startTimeTicks = 0,
                                     const std::string& playSessionId = "") const;

    // Image paths. Hound items carry full image URLs (TMDB), so these
    // return the URL unchanged; fetchBinary() below knows what to do with it.
    std::string buildBackdropPath(const std::string& backdropItemId, const std::string& backdropTag,
                                  int maxW, int maxH) const;
    std::string buildImagePath(const std::string& imageItemId, const std::string& imageTag,
                               int width, int height) const;

    // Fetches bytes for an image path/URL: full http(s) URLs are fetched
    // from their own host (https only where the build has TLS), anything
    // else from this server. Safe to call from a background thread: touches
    // no client state besides reading the login token.
    bool fetchBinary(const std::string& path, std::string& out) const;

    // Playback reports. Hound wants progress (seconds, plus the stream's
    // encoded data) rather than Jellyfin's session events; start is a no-op,
    // progress/stopped POST /movie|tv/<id>/playback using the stream chosen
    // by buildVideoStreamUrl(). No stream chosen yet -> a silent no-op.
    bool reportPlaybackStart(const std::string& itemId, const PlaybackIds& ids = PlaybackIds());
    bool reportPlaybackProgress(const std::string& itemId, int64_t positionTicks,
                                const PlaybackIds& ids = PlaybackIds(), bool isPaused = false);
    bool reportPlaybackStopped(const std::string& itemId, int64_t positionTicks,
                               const PlaybackIds& ids = PlaybackIds());

    // --- signing in without config.json ---

    // Identifies this console to Hound (X-Device-Id on login). Keep it
    // stable across launches; empty = a fixed default.
    void setDeviceId(const std::string& id) { device_id_ = id; }

    // Server can change on the login screen.
    void setServer(const std::string& host, int port) { host_ = host; port_ = port; }

    // Reuses a token saved by an earlier sign-in; check it with
    // validateLogin() before trusting it. `userId` is the Hound username.
    void useSavedLogin(const std::string& userId, const std::string& token);
    bool validateLogin();    // GET /server_info with the token
    void signOut();          // forgets the token (and tells the server)

    const std::string& lastError() const { return last_error_; }
    const std::string& userId() const { return user_id_; }
    const std::string& userName() const { return user_name_; }
    const std::string& accessToken() const { return token_; }
    // "Authorization: Bearer ...\r\n" for http_client's extra_headers; empty
    // when not signed in.
    std::string authHeader() const;
    const std::string& host() const { return host_; }
    int port() const { return port_; }

private:
    std::string host_;
    int port_;
    std::string token_;
    std::string user_id_;
    std::string user_name_;
    std::string device_id_;
    std::string last_error_;

    // The stream chosen for the item currently playing (see
    // buildVideoStreamUrl): what the playback reports need to identify it.
    struct PlayContext {
        std::string itemId;
        std::string mediaType;      // "movie" or "tvshow"
        std::string sourceId;       // TMDB id, without the "tmdb-" prefix
        std::string encodedData;
        std::string streamProtocol;
        int season = -1;
        int episode = -1;
        int totalDurationSeconds = 0;
        int width = 0;
        int height = 0;
    };
    PlayContext play_context_;

    // Finds a playable stream for the item (media files, then providers)
    // and fills play_context_. Returns false with last_error_ set when the
    // item has nothing to play.
    bool resolveStream(const std::string& itemId);
};
