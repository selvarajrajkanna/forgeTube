#include "yt/innertube.h"

#include <nlohmann/json.hpp>
#include <yyjson.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_map>

#include "util/log.h"
#include "util/no_destroy.h"
#include "util/remote_config.h"
#include "yt/auth.h"
#include "yt/js_solver.h"

namespace omega::yt {
namespace {

using json = nlohmann::json;

// One InnerTube client as described by remote-config.json ("player", "browse" or "search").
struct ClientDef {
    json client;  // context.client
    std::string user_agent, version, name_id;
};

ClientDef client_def_from(const json& c, const std::string& visitor, const std::string& version_override = "") {
    ClientDef d;
    d.user_agent = c.value("userAgent", "");
    d.version = version_override.empty() ? c.value("clientVersion", "") : version_override;
    d.name_id = std::to_string(c.value("clientNameId", 0));
    d.client = c.value("context", json::object());
    d.client["clientName"] = c.value("clientName", "");
    d.client["clientVersion"] = d.version;
    d.client["hl"] = cfg::language();
    d.client["gl"] = cfg::region();
    if (!visitor.empty()) d.client["visitorData"] = visitor;
    return d;
}

ClientDef client_def(const char* role, const std::string& visitor, const std::string& version_override = "") {
    return client_def_from(cfg::obj({"innertube", role}), visitor, version_override);
}

net::Headers client_headers(const ClientDef& d, const std::string& visitor) {
    net::Headers h = {{"Content-Type", "application/json"},
                      {"User-Agent", d.user_agent},
                      {"X-YouTube-Client-Name", d.name_id},
                      {"X-YouTube-Client-Version", d.version},
                      {"Origin", "https://www.youtube.com"}};
    if (!visitor.empty()) h.emplace_back("X-Goog-Visitor-Id", visitor);
    return h;
}

// RAII wrapper for a single-arena yyjson document (4-8x smaller and 5-10x faster than nlohmann::json DOM).
struct YDoc {
    yyjson_doc* doc = nullptr;
    YDoc() = default;
    explicit YDoc(yyjson_doc* d) : doc(d) {}
    ~YDoc() { reset(); }
    YDoc(YDoc&& o) noexcept : doc(o.doc) { o.doc = nullptr; }
    YDoc& operator=(YDoc&& o) noexcept {
        if (this != &o) {
            reset();
            doc = o.doc;
            o.doc = nullptr;
        }
        return *this;
    }
    YDoc(const YDoc&) = delete;
    YDoc& operator=(const YDoc&) = delete;
    yyjson_val* root() const { return doc ? yyjson_doc_get_root(doc) : nullptr; }
    explicit operator bool() const { return doc != nullptr; }
    void reset() {
        if (doc) {
            yyjson_doc_free(doc);
            doc = nullptr;
        }
    }
};

YDoc yparse(std::string& body) {
    yyjson_doc* doc = body.empty() ? nullptr : yyjson_read(body.data(), body.size(), 0);
    std::string().swap(body);  // free raw HTTP body immediately after single-arena parse
    return YDoc(doc);
}

yyjson_val* yobj(yyjson_val* j, const char* key) {
    yyjson_val* v = yyjson_is_obj(j) ? yyjson_obj_get(j, key) : nullptr;
    return yyjson_is_obj(v) ? v : nullptr;
}

yyjson_val* yarr(yyjson_val* j, const char* key) {
    yyjson_val* v = yyjson_is_obj(j) ? yyjson_obj_get(j, key) : nullptr;
    return yyjson_is_arr(v) ? v : nullptr;
}

std::string_view ysv(yyjson_val* v) {
    if (!yyjson_is_str(v)) return {};
    const char* s = yyjson_get_str(v);
    return s ? std::string_view(s, yyjson_get_len(v)) : std::string_view{};
}

std::string ystr(yyjson_val* j, const char* key, const char* def = "") {
    yyjson_val* v = yyjson_is_obj(j) ? yyjson_obj_get(j, key) : nullptr;
    if (yyjson_is_str(v)) {
        const char* s = yyjson_get_str(v);
        return s ? std::string(s, yyjson_get_len(v)) : std::string(def);
    }
    return def;
}

bool ybool(yyjson_val* j, const char* key, bool def = false) {
    yyjson_val* v = yyjson_is_obj(j) ? yyjson_obj_get(j, key) : nullptr;
    return yyjson_is_bool(v) ? yyjson_get_bool(v) : def;
}

int64_t yint(yyjson_val* j, const char* key) {
    yyjson_val* v = yyjson_is_obj(j) ? yyjson_obj_get(j, key) : nullptr;
    if (!v) return 0;
    if (yyjson_is_sint(v)) return yyjson_get_sint(v);
    if (yyjson_is_uint(v)) return static_cast<int64_t>(yyjson_get_uint(v));
    if (yyjson_is_real(v)) return static_cast<int64_t>(yyjson_get_real(v));
    if (yyjson_is_str(v)) {
        std::string_view sv = ysv(v);
        int64_t n = 0;
        if (std::from_chars(sv.data(), sv.data() + sv.size(), n).ec == std::errc{}) return n;
    }
    return 0;
}

std::string ytext(yyjson_val* j) {
    if (!yyjson_is_obj(j)) return "";
    if (yyjson_val* st = yyjson_obj_get(j, "simpleText"); yyjson_is_str(st)) {
        std::string_view sv = ysv(st);
        return std::string(sv.data(), sv.size());
    }
    std::string out;
    if (yyjson_val* runs = yarr(j, "runs")) {
        yyjson_arr_iter it = yyjson_arr_iter_with(runs);
        while (yyjson_val* run = yyjson_arr_iter_next(&it)) {
            std::string_view sv = ysv(yyjson_is_obj(run) ? yyjson_obj_get(run, "text") : nullptr);
            out.append(sv.data(), sv.size());
        }
    }
    if (out.empty()) {
        if (yyjson_val* ct = yyjson_obj_get(j, "content"); yyjson_is_str(ct)) {
            std::string_view sv = ysv(ct);
            out.assign(sv.data(), sv.size());  // view-model texts
        }
    }
    return out;
}

std::string trimmed(std::string s) {
    size_t a = s.find_first_not_of(" \t\n\xC2\xA0");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\n");
    return s.substr(a, b - a + 1);
}

// First value of `key` anywhere under j (depth-first), or nullptr.
yyjson_val* yfind(yyjson_val* j, const char* key) {
    if (!j) return nullptr;
    if (yyjson_is_obj(j)) {
        if (yyjson_val* hit = yyjson_obj_get(j, key)) return hit;
        yyjson_obj_iter it = yyjson_obj_iter_with(j);
        while (yyjson_val* k = yyjson_obj_iter_next(&it)) {
            if (yyjson_val* f = yfind(yyjson_obj_iter_get_val(k), key)) return f;
        }
    } else if (yyjson_is_arr(j)) {
        yyjson_arr_iter it = yyjson_arr_iter_with(j);
        while (yyjson_val* v = yyjson_arr_iter_next(&it)) {
            if (yyjson_val* f = yfind(v, key)) return f;
        }
    }
    return nullptr;
}

// A playlist card and a video card of the same first video are different entries.
bool already_listed(const Feed& out, const FeedItem& item) {
    for (const FeedItem& existing : out.items)
        if (existing.video_id == item.video_id && existing.playlist_id == item.playlist_id) return true;
    return false;
}

std::string playlist_kind(const std::string& playlist_id) { return playlist_id.compare(0, 2, "RD") == 0 ? "mix" : "playlist"; }

std::string ythumb(yyjson_val* thumbs) {
    yyjson_val* list = yarr(thumbs, "thumbnails");
    yyjson_val* last = list ? yyjson_arr_get_last(list) : nullptr;
    return ystr(last, "url");
}

bool ycontains_str(yyjson_val* j, const char* needle) {
    if (!j) return false;
    if (yyjson_is_str(j)) return ysv(j).find(needle) != std::string_view::npos;
    if (yyjson_is_obj(j)) {
        yyjson_obj_iter it = yyjson_obj_iter_with(j);
        while (yyjson_val* k = yyjson_obj_iter_next(&it))
            if (ycontains_str(yyjson_obj_iter_get_val(k), needle)) return true;
    } else if (yyjson_is_arr(j)) {
        yyjson_arr_iter it = yyjson_arr_iter_with(j);
        while (yyjson_val* v = yyjson_arr_iter_next(&it))
            if (ycontains_str(v, needle)) return true;
    }
    return false;
}

// The account's progress on a video: resume overlay (videoRenderer / tile) or progress bar (lockup).
int ywatched(yyjson_val* j) {
    for (const char* key : {"percentDurationWatched", "startPercent"}) {
        if (yyjson_val* v = yfind(j, key)) {
            int pct = 0;
            if (yyjson_is_num(v)) pct = static_cast<int>(yyjson_get_num(v));
            else if (yyjson_is_str(v)) pct = atoi(yyjson_get_str(v));
            return std::max(0, std::min(100, pct));
        }
    }
    return 0;
}

void parse_video_renderer(yyjson_val* v, Feed& out) {
    if (!yyjson_is_obj(v)) return;
    FeedItem item;
    item.video_id = ystr(v, "videoId");
    if (item.video_id.empty()) return;
    item.title = ytext(yobj(v, "title"));
    yyjson_val* owner = yobj(v, "ownerText");
    item.channel = ytext(owner ? owner : yobj(v, "longBylineText"));
    item.duration = ytext(yobj(v, "lengthText"));
    yyjson_val* short_views = yobj(v, "shortViewCountText");
    item.views = ytext(short_views ? short_views : yobj(v, "viewCountText"));
    item.published = ytext(yobj(v, "publishedTimeText"));
    item.thumbnail = "https://i.ytimg.com/vi/" + item.video_id + "/mqdefault.jpg";
    if (yyjson_val* badges = yarr(v, "badges")) {
        yyjson_arr_iter it = yyjson_arr_iter_with(badges);
        while (yyjson_val* badge = yyjson_arr_iter_next(&it))
            if (ycontains_str(badge, "LIVE")) item.is_live = true;
    }
    // Broadcasts have no length and count viewers ("18 k spectateurs" / "1.2K watching") instead of views.
    if (item.duration.empty() && (item.views.find("spectateur") != std::string::npos ||
                                  item.views.find("watching") != std::string::npos))
        item.is_live = true;
    item.watched_percent = ywatched(yarr(v, "thumbnailOverlays"));
    out.items.push_back(std::move(item));
}

// A row of a playlist page (WEB browse VL<id>): no channel avatar, "views • age" in videoInfo.
void parse_playlist_video(yyjson_val* v, Feed& out) {
    if (!yyjson_is_obj(v) || !ybool(v, "isPlayable", true)) return;  // deleted / private entries
    FeedItem item;
    item.video_id = ystr(v, "videoId");
    if (item.video_id.empty()) return;
    item.title = ytext(yobj(v, "title"));
    item.channel = ytext(yobj(v, "shortBylineText"));
    item.duration = ytext(yobj(v, "lengthText"));
    if (yyjson_val* runs = yarr(yobj(v, "videoInfo"), "runs")) {
        if (yyjson_val* r0 = yyjson_arr_get(runs, 0)) item.views = ystr(r0, "text");
        if (yyjson_val* r2 = yyjson_arr_get(runs, 2)) item.published = ystr(r2, "text");
    }
    item.thumbnail = "https://i.ytimg.com/vi/" + item.video_id + "/mqdefault.jpg";
    item.watched_percent = ywatched(yarr(v, "thumbnailOverlays"));
    out.items.push_back(std::move(item));
}

// TV client layout (as in SmartTube's TileItem / ytps5): id in onSelectCommand.watchEndpoint, title in
// metadata.tileMetadataRenderer, thumbnail and duration in header.tileHeaderRenderer.
void parse_tile(yyjson_val* t, Feed& out) {
    if (!yyjson_is_obj(t)) return;
    yyjson_val* watch = yobj(yobj(t, "onSelectCommand"), "watchEndpoint");
    if (!watch) return;  // Shorts (reelWatchEndpoint), channels
    FeedItem item;
    item.video_id = ystr(watch, "videoId");
    if (item.video_id.empty()) return;
    if (ysv(yyjson_obj_get(t, "contentType")) == "TILE_CONTENT_TYPE_PLAYLIST") {
        item.playlist_id = ystr(watch, "playlistId");
        if (item.playlist_id.empty()) return;
        item.kind = playlist_kind(item.playlist_id);
    }
    if (already_listed(out, item)) return;
    yyjson_val* meta = yobj(yobj(t, "metadata"), "tileMetadataRenderer");
    yyjson_val* header = yobj(yobj(t, "header"), "tileHeaderRenderer");
    item.title = ytext(yobj(meta, "title"));
    if (item.title.empty()) return;
    // lines[0]: channel • views • age (sometimes split over two lines); "•" separators are skipped.
    std::vector<std::string> parts;
    if (yyjson_val* lines = yarr(meta, "lines")) {
        yyjson_arr_iter lit = yyjson_arr_iter_with(lines);
        while (yyjson_val* line = yyjson_arr_iter_next(&lit)) {
            if (yyjson_val* items = yarr(yobj(line, "lineRenderer"), "items")) {
                yyjson_arr_iter iit = yyjson_arr_iter_with(items);
                while (yyjson_val* li = yyjson_arr_iter_next(&iit)) {
                    std::string text = ytext(yobj(yobj(li, "lineItemRenderer"), "text"));
                    if (!text.empty() && text != "\xE2\x80\xA2" && text != " \xE2\x80\xA2 ") parts.push_back(std::move(text));
                }
            }
        }
    }
    if (!parts.empty()) item.channel = std::move(parts[0]);
    if (parts.size() > 1) item.views = std::move(parts[1]);
    if (parts.size() > 2) item.published = std::move(parts[2]);
    item.thumbnail = "https://i.ytimg.com/vi/" + item.video_id + "/mqdefault.jpg";
    yyjson_val* overlays = yarr(header, "thumbnailOverlays");
    if (overlays) {
        yyjson_arr_iter oit = yyjson_arr_iter_with(overlays);
        while (yyjson_val* o = yyjson_arr_iter_next(&oit)) {
            yyjson_val* ts = yobj(o, "thumbnailOverlayTimeStatusRenderer");
            if (!ts) continue;
            item.duration = ytext(yobj(ts, "text"));
            if (ysv(yyjson_obj_get(ts, "style")) == "LIVE") item.is_live = true;
        }
    }
    item.watched_percent = ywatched(overlays);
    out.items.push_back(std::move(item));
}

// 2025+ layout: lockupViewModel (contentId, lockupMetadataViewModel; duration in a thumbnail badge).
void parse_lockup(yyjson_val* l, Feed& out) {
    if (!yyjson_is_obj(l)) return;
    std::string_view type = ysv(yyjson_obj_get(l, "contentType"));
    FeedItem item;
    if (type == "LOCKUP_CONTENT_TYPE_PLAYLIST") {
        // Playlist / mix: contentId is the list; play it from its first video (the card's watchEndpoint).
        item.playlist_id = ystr(l, "contentId");
        yyjson_val* watch = yfind(l, "watchEndpoint");
        if (item.playlist_id.empty() || !yyjson_is_obj(watch)) return;
        item.video_id = ystr(watch, "videoId");
        item.kind = playlist_kind(item.playlist_id);
    } else if (type == "LOCKUP_CONTENT_TYPE_VIDEO") {
        item.video_id = ystr(l, "contentId");
    } else {
        return;
    }
    if (item.video_id.size() != 11) return;
    if (already_listed(out, item)) return;
    yyjson_val* meta = yobj(yobj(l, "metadata"), "lockupMetadataViewModel");
    item.title = ytext(yobj(meta, "title"));
    if (item.title.empty()) return;
    // metadataRows: [channel], [views, age]
    std::vector<std::string> parts;
    if (yyjson_val* rows = yarr(yobj(yobj(meta, "metadata"), "contentMetadataViewModel"), "metadataRows")) {
        yyjson_arr_iter rit = yyjson_arr_iter_with(rows);
        while (yyjson_val* row = yyjson_arr_iter_next(&rit)) {
            if (yyjson_val* mparts = yarr(row, "metadataParts")) {
                yyjson_arr_iter pit = yyjson_arr_iter_with(mparts);
                while (yyjson_val* part = yyjson_arr_iter_next(&pit)) {
                    std::string text = ytext(yobj(part, "text"));
                    if (!text.empty()) parts.push_back(std::move(text));
                }
            }
        }
    }
    if (!parts.empty()) item.channel = std::move(parts[0]);
    if (parts.size() > 1) item.views = std::move(parts[1]);
    if (parts.size() > 2) item.published = std::move(parts[2]);
    item.thumbnail = "https://i.ytimg.com/vi/" + item.video_id + "/mqdefault.jpg";
    yyjson_val* content_image = yobj(l, "contentImage");
    if (yyjson_val* badge = yfind(content_image, "thumbnailBadgeViewModel")) {
        if (yyjson_val* txt = yfind(badge, "text"); yyjson_is_str(txt)) {
            std::string_view sv = ysv(txt);
            item.duration.assign(sv.data(), sv.size());
            if (item.duration == "LIVE" || item.duration == "EN DIRECT") item.is_live = true;
        }
    }
    if (item.kind == "mix") item.duration = "MIX";  // its badge is an icon
    if (item.views == "Playlist") item.views.clear();  // the type label, already shown by the badge
    item.watched_percent = ywatched(content_image);
    out.items.push_back(std::move(item));
}

bool is_ignored_branch(std::string_view k) {
    return k == "responseContext" || k == "trackingParams" || k == "clickTrackingParams" ||
           k == "adPlacements" || k == "adSlots" || k == "playerAds" || k == "adSlotRenderer" ||
           k == "storyboards" || k == "microformat" || k == "loggingDirectives";
}

// Walks the whole response: YouTube moves renderers around between layouts, so search by key.
// highlights: where titled TV shelves go (subscriptions), instead of the main list.
void collect(yyjson_val* j, Feed& out, Feed* highlights = nullptr) {
    if (!j) return;
    if (yyjson_is_obj(j)) {
        yyjson_obj_iter it = yyjson_obj_iter_with(j);
        while (yyjson_val* k = yyjson_obj_iter_next(&it)) {
            std::string_view key = ysv(k);
            yyjson_val* val = yyjson_obj_iter_get_val(k);
            if (is_ignored_branch(key) || key == "reelShelfRenderer" || key == "shortsLockupViewModel") continue;
            if (key == "shelfRenderer" && yyjson_is_obj(val)) {
                if (ysv(yyjson_obj_get(val, "tvhtml5ShelfRendererType")) == "TVHTML5_SHELF_RENDERER_TYPE_SHORTS") continue;
                if (highlights && yyjson_obj_get(val, "headerRenderer")) collect(val, *highlights);
                else collect(val, out, highlights);
            } else if (key == "videoRenderer" || key == "compactVideoRenderer") {
                parse_video_renderer(val, out);
            } else if (key == "lockupViewModel") {
                parse_lockup(val, out);
            } else if (key == "playlistVideoRenderer") {
                parse_playlist_video(val, out);
            } else if (key == "tileRenderer") {
                parse_tile(val, out);
            } else if (key == "nextContinuationData" && out.continuation.empty()) {
                out.continuation = ystr(val, "continuation");
            } else if (key == "continuationCommand" && out.continuation.empty() && yyjson_is_obj(val) && yyjson_obj_get(val, "token")) {
                out.continuation = ystr(val, "token");
            } else {  // continuationItemViewModel: the token sits in a nested continuationCommand
                collect(val, out, highlights);
            }
        }
    } else if (yyjson_is_arr(j)) {
        yyjson_arr_iter it = yyjson_arr_iter_with(j);
        while (yyjson_val* e = yyjson_arr_iter_next(&it)) collect(e, out, highlights);
    }
}

// The TV home: one Shelf per titled shelfRenderer (Shorts and ads left out); the page continuation is the one
// outside the shelves (theirs only extend their own row).
void collect_shelves(yyjson_val* j, Feed& out) {
    if (!j) return;
    if (yyjson_is_obj(j)) {
        yyjson_obj_iter it = yyjson_obj_iter_with(j);
        while (yyjson_val* k = yyjson_obj_iter_next(&it)) {
            std::string_view key = ysv(k);
            yyjson_val* val = yyjson_obj_iter_get_val(k);
            if (is_ignored_branch(key)) continue;
            if (key == "shelfRenderer" && yyjson_is_obj(val)) {
                if (ysv(yyjson_obj_get(val, "tvhtml5ShelfRendererType")) == "TVHTML5_SHELF_RENDERER_TYPE_SHORTS") continue;
                Shelf shelf;
                if (yyjson_val* hdr = yyjson_obj_get(val, "headerRenderer"))
                    if (yyjson_val* title = yfind(hdr, "title")) shelf.title = ytext(title);
                Feed row;
                collect(yobj(val, "content"), row);
                if (row.items.empty()) continue;
                shelf.items = std::move(row.items);
                shelf.continuation = row.continuation;
                out.shelves.push_back(std::move(shelf));
            } else if (key == "nextContinuationData" && out.continuation.empty()) {
                out.continuation = ystr(val, "continuation");
            } else {
                collect_shelves(val, out);
            }
        }
    } else if (yyjson_is_arr(j)) {
        yyjson_arr_iter it = yyjson_arr_iter_with(j);
        while (yyjson_val* e = yyjson_arr_iter_next(&it)) collect_shelves(e, out);
    }
}

// "video/mp4; codecs=\"avc1.64002A\"" -> mime + codecs
void split_mime(const std::string& full, std::string& mime, std::string& codecs) {
    size_t semi = full.find(';');
    mime = full.substr(0, semi);
    size_t q1 = full.find('"', semi == std::string::npos ? 0 : semi);
    size_t q2 = q1 == std::string::npos ? std::string::npos : full.find('"', q1 + 1);
    codecs = (q1 != std::string::npos && q2 != std::string::npos) ? full.substr(q1 + 1, q2 - q1 - 1) : "";
}

// Subtitle tracks and the machine-translation targets (captions.playerCaptionsTracklistRenderer).
void parse_captions(yyjson_val* captions, VideoInfo& out) {
    out.captions.clear();
    out.translations.clear();
    yyjson_val* list = yobj(captions, "playerCaptionsTracklistRenderer");
    if (yyjson_val* tracks = yarr(list, "captionTracks")) {
        yyjson_arr_iter it = yyjson_arr_iter_with(tracks);
        while (yyjson_val* t = yyjson_arr_iter_next(&it)) {
            CaptionTrack track;
            track.url = ystr(t, "baseUrl");
            if (track.url.empty()) continue;
            if (track.url[0] == '/') track.url = "https://www.youtube.com" + track.url;
            track.lang = ystr(t, "languageCode");
            track.name = ytext(yobj(t, "name"));
            track.automatic = ysv(yyjson_is_obj(t) ? yyjson_obj_get(t, "kind") : nullptr) == "asr";
            track.translatable = ybool(t, "isTranslatable");
            if (track.name.empty()) track.name = track.lang;
            out.captions.push_back(std::move(track));
        }
    }
    if (yyjson_val* langs = yarr(list, "translationLanguages")) {
        yyjson_arr_iter it = yyjson_arr_iter_with(langs);
        while (yyjson_val* l = yyjson_arr_iter_next(&it)) {
            std::string code = ystr(l, "languageCode");
            if (!code.empty()) out.translations.emplace_back(std::move(code), ytext(yobj(l, "languageName")));
        }
    }
}

// `url` without its `name` query parameter.
std::string drop_param(const std::string& url, const std::string& name) {
    size_t mark = url.find('?');
    if (mark == std::string::npos) return url;
    std::string_view sv(url);
    std::string out;
    out.reserve(url.size());
    out.append(sv.substr(0, mark + 1));
    bool first = true;
    for (size_t at = mark + 1; at <= sv.size();) {
        size_t end = sv.find('&', at);
        if (end == std::string_view::npos) end = sv.size();
        std::string_view pair = sv.substr(at, end - at);
        if (!pair.empty() && pair.substr(0, pair.find('=')) != name) {
            if (!first) out += '&';
            out.append(pair);
            first = false;
        }
        at = end + 1;
    }
    return out;
}

// POST to an InnerTube endpoint as the signed-in account (TV client + Bearer), as YouTube's TV app does.
net::Response post_as_account(const std::string& endpoint, json body, const std::string& access_token) {
    ClientDef def = client_def("browse", "");
    body["context"] = {{"client", def.client}};
    net::Request req;
    req.method = "POST";
    req.url = "https://www.youtube.com/youtubei/v1/" + endpoint + "?prettyPrint=false";
    req.headers = client_headers(def, "");
    req.headers.emplace_back("Authorization", "Bearer " + access_token);
    req.body = body.dump();
    return net::fetch(req);
}

std::string http_error(const char* what, const net::Response& res) {
    return std::string(what) + ": " + (res.error.empty() ? "http " + std::to_string(res.status) : res.error);
}

// Every text of `key` objects under j (e.g. all "metadataParts" texts of a header).
void texts_under(yyjson_val* j, const char* key, std::vector<std::string>& out) {
    if (!j) return;
    if (yyjson_is_obj(j)) {
        yyjson_obj_iter it = yyjson_obj_iter_with(j);
        while (yyjson_val* k = yyjson_obj_iter_next(&it)) {
            yyjson_val* val = yyjson_obj_iter_get_val(k);
            if (yyjson_equals_str(k, key) && yyjson_is_arr(val)) {
                yyjson_arr_iter ait = yyjson_arr_iter_with(val);
                while (yyjson_val* part = yyjson_arr_iter_next(&ait)) out.push_back(ytext(yobj(part, "text")));
            } else {
                texts_under(val, key, out);
            }
        }
    } else if (yyjson_is_arr(j)) {
        yyjson_arr_iter it = yyjson_arr_iter_with(j);
        while (yyjson_val* e = yyjson_arr_iter_next(&it)) texts_under(e, key, out);
    }
}

bool contains_any(const std::string& s, std::initializer_list<const char*> words) {
    for (const char* w : words)
        if (s.find(w) != std::string::npos) return true;
    return false;
}

VideoCodec classify_video_codec(const std::string& mime, const std::string& codecs) {
    if (mime == "video/mp4" && codecs.compare(0, 4, "avc1") == 0) return VideoCodec::H264;
    if ((mime == "video/webm" || mime == "video/mp4") &&
        (codecs == "vp9" || codecs.compare(0, 5, "vp09.") == 0))
        return VideoCodec::VP9;
    if (mime == "video/mp4" && codecs.compare(0, 5, "av01.") == 0) return VideoCodec::AV1;
    return VideoCodec::Unknown;
}

}  // namespace

bool Client::ensure_visitor(std::string& visitor_out, std::string& version_out, std::string& error) {
    struct VisitorState {
        std::mutex mutex;
        std::condition_variable cv;
        bool loading = false;
        std::string visitor_data;
        std::string web_client_version;
    };
    static omega::NoDestroy<VisitorState> g_visitor;
    VisitorState& st = *g_visitor;

    std::unique_lock<std::mutex> lock(st.mutex);
    while (st.loading) {
        if (auto* ct = net::CancelScope::current(); ct && ct->is_aborted()) {
            error = "cancelled";
            return false;
        }
        st.cv.wait_for(lock, std::chrono::milliseconds(100));
    }
    if (!st.visitor_data.empty()) {
        visitor_out = st.visitor_data;
        version_out = st.web_client_version;
        return true;
    }
    st.loading = true;
    lock.unlock();

    struct LoadingGuard {
        VisitorState& s;
        ~LoadingGuard() {
            {
                std::lock_guard<std::mutex> lk(s.mutex);
                s.loading = false;
            }
            s.cv.notify_all();
        }
    } guard{st};

    net::Request req;
    req.url = "https://www.youtube.com/";
    req.headers = {{"User-Agent", cfg::str({"innertube", "search", "userAgent"})}, {"Accept-Language", "en-US"}};
    net::Response res = net::fetch(req);
    if (!res.ok()) {
        error = "youtube.com: " + (res.error.empty() ? "http " + std::to_string(res.status) : res.error);
        return false;
    }
    const std::string key = "\"VISITOR_DATA\":\"";
    size_t at = res.body.find(key);
    if (at == std::string::npos) {
        error = "VISITOR_DATA not found";
        return false;
    }
    at += key.size();
    size_t end = res.body.find('"', at);
    if (end == std::string::npos || end == at) {
        error = "VISITOR_DATA malformed";
        return false;
    }
    std::string visitor = res.body.substr(at, end - at);
    std::string web_ver;
    const std::string ver_key = "\"INNERTUBE_CLIENT_VERSION\":\"";
    size_t v = res.body.find(ver_key);
    if (v != std::string::npos) {
        v += ver_key.size();
        size_t vend = res.body.find('"', v);
        if (vend != std::string::npos && vend > v) web_ver = res.body.substr(v, vend - v);
    }
    if (web_ver.empty()) web_ver = cfg::str({"innertube", "search", "clientVersion"});
    OLOG("yt: visitorData %zu chars, web client %s", visitor.size(), web_ver.c_str());

    lock.lock();
    st.visitor_data = std::move(visitor);
    st.web_client_version = std::move(web_ver);
    visitor_out = st.visitor_data;
    version_out = st.web_client_version;
    return true;
}

net::Headers Client::stream_headers() const { return {{"User-Agent", cfg::str({"innertube", "player", "userAgent"})}}; }

namespace {

// The player clients to try, in order (remote-config innertube.playerClients, as yt-dlp's client list).
// Older configs only have "player" (direct URLs) and "playerFallback" (TV client, deciphered, account retry).
// include_disabled: also the entries with "enabled": false (dev tooling picking one by name).
std::vector<json> player_clients(bool include_disabled) {
    json all = cfg::snapshot();
    const json* list = nullptr;
    if (all.contains("innertube") && all["innertube"].contains("playerClients")) list = &all["innertube"]["playerClients"];
    std::vector<json> out;
    if (list && list->is_array()) {
        for (const json& c : *list)
            if (c.is_object() && !c.value("clientName", "").empty() && (include_disabled || c.value("enabled", true)))
                out.push_back(c);
        if (!out.empty()) return out;
    }
    json primary = cfg::obj({"innertube", "player"});
    if (!primary.value("clientName", "").empty()) out.push_back(primary);
    json fallback = cfg::obj({"innertube", "playerFallback"});
    if (!fallback.value("clientName", "").empty()) {
        fallback["decipher"] = true;
        fallback["auth"] = "retry";
        out.push_back(fallback);
    }
    return out;
}

}  // namespace

bool Client::player(const std::string& video_id, VideoInfo& out, std::string& error, const std::string& only_client) {
    refusal_ = {};
    std::string visitor, web_ver;
    if (!ensure_visitor(visitor, web_ver, error)) {
        refusal_ = {"NETWORK", error};
        return false;
    }
    // Always in the config's order, not the last client that worked first: after one video only TVHTML5 could
    // play, every video went through it (player JS + googlevideo refusing its URLs for ~5 s) while VISIONOS,
    // ~1 s, would have played them. A refused VISIONOS costs ~0.2 s.
    std::vector<json> clients = player_clients(!only_client.empty());
    std::string errors;
    for (size_t i = 0; i < clients.size(); i++) {
        if (auto* ct = net::CancelScope::current(); ct && ct->is_aborted()) {
            error = "cancelled";
            return false;
        }
        const json& c = clients[i];
        std::string name = c.value("clientName", "");
        if (!only_client.empty() && name != only_client) continue;
        if (std::find(skipped_clients_.begin(), skipped_clients_.end(), name) != skipped_clients_.end()) continue;
        std::string err;
        if (try_client(c, video_id, visitor, out, err)) {
            out.client = name;
            if (i > 0) OLOG("yt: %s played through %s", video_id.c_str(), name.c_str());
            return true;
        }
        OLOG("yt: %s via %s: %s", video_id.c_str(), name.c_str(), err.c_str());
        errors += (errors.empty() ? "" : "; ") + name + ": " + err;
    }
    error = errors.empty() ? "no player client" : errors;
    return false;
}

// One client: deciphering when its URLs need the player JS, and (auth "retry") once more as the signed-in
// account when YouTube wants a login (bot check, age); auth "always" sends the account right away.
bool Client::try_client(const json& c, const std::string& video_id, const std::string& visitor, VideoInfo& out,
                        std::string& error) {
    bool decipher = c.value("decipher", false);
    if (decipher && !shared_decipher().ensure(error)) return false;
    std::string auth = c.value("auth", "never");
    std::string token;
    if (auth != "never" && shared_auth().signed_in()) {
        std::string token_error;
        token = shared_auth().access_token(token_error);
    }
    if (auth == "always" && !token.empty())
        return player_with(c, video_id, visitor, out, error, token) && !out.formats.empty();
    if (player_with(c, video_id, visitor, out, error) && !out.formats.empty()) return true;
    bool account_may_help = refusal_.status == "LOGIN_REQUIRED" || refusal_.status.rfind("AGE_", 0) == 0;
    if (auth == "retry" && account_may_help && !token.empty()) {
        error.clear();
        if (player_with(c, video_id, visitor, out, error, token) && !out.formats.empty()) {
            OLOG("yt: %s played as the signed-in account", video_id.c_str());
            return true;
        }
    }
    if (error.empty()) error = decipher ? "no playable formats" : "no direct formats";
    return false;
}

bool Client::player_with(const json& c, const std::string& video_id, const std::string& visitor, VideoInfo& out,
                         std::string& error, const std::string& access_token) {
    bool decipher = c.value("decipher", false);
    ClientDef def = client_def_from(c, visitor);
    json body = {
        {"context", {{"client", def.client}}},
        {"videoId", video_id},
        {"contentCheckOk", true},
        {"racyCheckOk", true},
    };
    json playback_context = json::object();
    json cpc = {{"html5Preference", "HTML5_PREF_WANTS"}};
    if (decipher) {
        cpc["signatureTimestamp"] = shared_decipher().signature_timestamp();
    }
    playback_context["contentPlaybackContext"] = std::move(cpc);
    playback_context["devicePlaybackCapabilities"] = {
        {"supportsAv1", true},
        {"supportsVp9", true},
        {"hdrCapable", true}
    };
    body["playbackContext"] = std::move(playback_context);
    net::Request req;
    req.method = "POST";
    req.url = "https://www.youtube.com/youtubei/v1/player?prettyPrint=false";
    req.headers = client_headers(def, visitor);
    if (!access_token.empty()) req.headers.emplace_back("Authorization", "Bearer " + access_token);
    req.body = body.dump();
    net::Response res = net::fetch(req);
    if (!res.ok()) {
        error = "player: " + (res.error.empty() ? "http " + std::to_string(res.status) : res.error);
        refusal_ = res.status > 0 ? Refusal{"HTTP_" + std::to_string(res.status), ""} : Refusal{"NETWORK", res.error};
        return false;
    }

    record_response(res.body);
    YDoc doc = yparse(res.body);
    yyjson_val* r = doc.root();
    if (!yyjson_is_obj(r)) {
        error = "player: invalid json";
        return false;
    }
    yyjson_val* ps = yobj(r, "playabilityStatus");
    std::string status = ystr(ps, "status");
    if (status != "OK") {
        std::string reason = ystr(ps, "reason");
        error = "playability " + status + ": " + reason;
        refusal_ = {std::move(status), std::move(reason)};
        return false;
    }
    yyjson_val* details = yobj(r, "videoDetails");
    out.id = video_id;
    out.title = ystr(details, "title");
    out.author = ystr(details, "author");
    out.length_s = yint(details, "lengthSeconds");
    out.is_live = ybool(details, "isLive");
    out.description = ystr(details, "shortDescription");
    out.channel_id = ystr(details, "channelId");
    out.views = ystr(details, "viewCount");
    out.has_audio_loudness = false;
    out.audio_loudness_db = 0.0;
    if (yyjson_val* ac = yobj(yobj(r, "playerConfig"), "audioConfig")) {
        if (yyjson_val* ld = yyjson_obj_get(ac, "loudnessDb"); yyjson_is_num(ld)) {
            out.has_audio_loudness = true;
            out.audio_loudness_db = yyjson_get_num(ld);
        }
    }
    parse_captions(yobj(r, "captions"), out);
    out.formats.clear();
    out.user_agent = def.user_agent;
    size_t failed = 0;
    size_t total_adaptive = 0;
    std::string first_err;
    if (yyjson_val* af = yarr(yobj(r, "streamingData"), "adaptiveFormats")) {
        yyjson_arr_iter it = yyjson_arr_iter_with(af);
        while (yyjson_val* f = yyjson_arr_iter_next(&it)) {
            if (!yyjson_is_obj(f)) continue;
            ++total_adaptive;
            Format fmt;
            if (decipher) {
                std::string err;
                std::string sc = ystr(f, "signatureCipher");
                if (sc.empty()) sc = ystr(f, "cipher");
                if (!shared_decipher().stream_url(ystr(f, "url"), sc, fmt.url, err)) {
                    if (failed++ == 0) {
                        first_err = err;
                        OLOG("yt: decipher itag %lld: %s", static_cast<long long>(yint(f, "itag")), err.c_str());
                    }
                    continue;
                }
            } else {
                fmt.url = ystr(f, "url");
                if (fmt.url.empty()) continue;  // ciphered formats: only through a deciphering client
            }
            fmt.itag = static_cast<int>(yint(f, "itag"));
            split_mime(ystr(f, "mimeType"), fmt.mime, fmt.codecs);
            fmt.width = static_cast<int>(yint(f, "width"));
            fmt.height = static_cast<int>(yint(f, "height"));
            fmt.fps = static_cast<int>(yint(f, "fps"));
            int64_t avg_br = yint(f, "averageBitrate");
            fmt.bitrate = avg_br > 0 ? avg_br : yint(f, "bitrate");
            fmt.content_length = yint(f, "contentLength");
            fmt.audio_sample_rate = static_cast<int>(yint(f, "audioSampleRate"));
            int ch = static_cast<int>(yint(f, "audioChannels"));
            fmt.audio_channels = ch > 0 ? ch : 2;
            if (ybool(f, "isDrc")) continue;  // loudness-compressed duplicate of an audio format
            yyjson_val* track = yobj(f, "audioTrack");
            fmt.audio_track_id = ystr(track, "id");
            fmt.audio_track_name = ystr(track, "displayName");
            fmt.audio_default = ybool(track, "audioIsDefault");

            if (fmt.is_video()) {
                fmt.codec = classify_video_codec(fmt.mime, fmt.codecs);
                yyjson_val* ci = yobj(f, "colorInfo");
                std::string_view tc = ysv(ci ? yyjson_obj_get(ci, "transferCharacteristics") : nullptr);
                if (tc.find("SMPTEST2084") != std::string_view::npos) {
                    fmt.hdr_mode = HdrMode::HDR10;
                } else if (tc.find("ARIB_STD_B67") != std::string_view::npos) {
                    fmt.hdr_mode = HdrMode::HLG;
                }
            }

            out.formats.push_back(std::move(fmt));
        }
    }
    OLOG("yt: '%s' by %s, %llds, %zu formats (%s client%s)", out.title.c_str(), out.author.c_str(),
         static_cast<long long>(out.length_s), out.formats.size(), def.client.value("clientName", "").c_str(),
         failed ? (", " + std::to_string(failed) + " not deciphered").c_str() : "");
    if (out.formats.empty()) {
        if (total_adaptive == 0) {
            error = "no adaptiveFormats in streamingData";
        } else if (failed > 0) {
            error = std::to_string(failed) + "/" + std::to_string(total_adaptive) + " decipher failed: " + first_err;
        } else {
            error = "0 usable of " + std::to_string(total_adaptive) + " adaptiveFormats";
        }
    }
    return true;
}

bool Client::search(const std::string& query, const std::string& continuation, Feed& out, std::string& error) {
    std::string visitor, web_ver;
    if (!ensure_visitor(visitor, web_ver, error)) return false;
    ClientDef def = client_def("search", visitor, web_ver);
    json body = {{"context", {{"client", def.client}}}};
    if (continuation.empty()) {
        body["query"] = query;
    } else {
        body["continuation"] = continuation;
    }
    net::Request req;
    req.method = "POST";
    req.url = "https://www.youtube.com/youtubei/v1/search?prettyPrint=false";
    req.headers = client_headers(def, visitor);
    req.body = body.dump();
    net::Response res = net::fetch(req);
    if (!res.ok()) {
        error = "search: " + (res.error.empty() ? "http " + std::to_string(res.status) : res.error);
        return false;
    }
    record_response(res.body);
    YDoc doc = yparse(res.body);
    if (!doc) {
        error = "search: invalid json";
        return false;
    }
    out = Feed{};
    collect(doc.root(), out);
    OLOG("yt: search '%s'%s -> %zu items", query.c_str(), continuation.empty() ? "" : " (next page)", out.items.size());
    return true;
}

bool Client::playlist(const std::string& video_id, const std::string& playlist_id, Feed& out, std::string& title,
                      std::string& error) {
    std::string visitor, web_ver;
    if (!ensure_visitor(visitor, web_ver, error)) return false;
    ClientDef def = client_def("search", visitor, web_ver);
    json body = {{"context", {{"client", def.client}}}, {"videoId", video_id}, {"playlistId", playlist_id}};
    net::Request req;
    req.method = "POST";
    req.url = "https://www.youtube.com/youtubei/v1/next?prettyPrint=false";
    req.headers = client_headers(def, visitor);
    req.body = body.dump();
    net::Response res = net::fetch(req);
    if (!res.ok()) {
        error = "playlist: "+ (res.error.empty() ? "http " + std::to_string(res.status) : res.error);
        return false;
    }
    YDoc doc = yparse(res.body);
    yyjson_val* panel = doc ? yfind(doc.root(), "playlist") : nullptr;
    if (yyjson_val* inner = yobj(panel, "playlist")) panel = inner;
    yyjson_val* contents = yarr(panel, "contents");
    if (!panel || !contents) {
        error = "playlist: no playlist panel";
        return false;
    }
    yyjson_val* t = yyjson_obj_get(panel, "title");
    title = yyjson_is_str(t) ? std::string(ysv(t)) : ytext(t);
    out = Feed{};
    // The panel is a window of the list around the current video: keep what comes after it.
    std::vector<FeedItem> all;
    size_t after = 0;
    yyjson_arr_iter cit = yyjson_arr_iter_with(contents);
    while (yyjson_val* c = yyjson_arr_iter_next(&cit)) {
        yyjson_val* v = yobj(c, "playlistPanelVideoRenderer");
        if (!v) continue;
        FeedItem item;
        item.video_id = ystr(v, "videoId");
        if (item.video_id.empty()) continue;  // unavailable entries
        item.title = ytext(yobj(v, "title"));
        item.channel = ytext(yobj(v, "shortBylineText"));
        item.duration = ytext(yobj(v, "lengthText"));
        item.thumbnail = "https://i.ytimg.com/vi/" + item.video_id + "/mqdefault.jpg";
        item.playlist_id = playlist_id;
        if (ybool(v, "selected") || item.video_id == video_id) after = all.size() + 1;
        all.push_back(std::move(item));
    }
    // A window of what follows (each new video asks again, so it slides along the list): a row of hundreds of
    // cards reaches coordinates the TV's GPU does not render reliably.
    constexpr size_t kWindow = 25;
    size_t first = std::min(after, all.size());
    out.items.assign(all.begin() + static_cast<long>(first),
                     all.begin() + static_cast<long>(std::min(all.size(), first + kWindow)));
    OLOG("yt: playlist %s '%s' from %s -> %zu next items", playlist_id.c_str(), title.c_str(), video_id.c_str(),
         out.items.size());
    return true;
}

bool Client::playlist_page(const std::string& playlist_id, const std::string& continuation, Feed& out,
                           std::string& title, std::string& error) {
    std::string visitor, web_ver;
    if (!ensure_visitor(visitor, web_ver, error)) return false;
    ClientDef def = client_def("search", visitor, web_ver);
    json body = {{"context", {{"client", def.client}}}};
    if (continuation.empty()) body["browseId"] = "VL" + playlist_id;
    else body["continuation"] = continuation;
    net::Request req;
    req.method = "POST";
    req.url = "https://www.youtube.com/youtubei/v1/browse?prettyPrint=false";
    req.headers = client_headers(def, visitor);
    req.body = body.dump();
    net::Response res = net::fetch(req);
    if (!res.ok()) {
        error = http_error("playlist", res);
        return false;
    }
    record_response(res.body);
    YDoc doc = yparse(res.body);
    if (!doc) {
        error = "playlist: invalid json";
        return false;
    }
    out = Feed{};
    collect(doc.root(), out);
    for (FeedItem& item : out.items) item.playlist_id = playlist_id;
    if (yyjson_val* meta = yfind(doc.root(), "playlistMetadataRenderer")) title = ystr(meta, "title");
    OLOG("yt: playlist page %s%s '%s' -> %zu items", playlist_id.c_str(), continuation.empty() ? "" : " (next page)",
         title.c_str(), out.items.size());
    return true;
}

bool Client::related(const std::string& video_id, const std::string& continuation, const std::string& access_token,
                     Feed& out, std::string& error) {
    // Signed in: TV "next" gives personalized suggestions (current video + account history).
    // Read singleColumnWatchNextResults.results in order (not the whole JSON tree), and promote
    // YouTube's organic autoplay pick to index 0 for Up Next.
    if (!access_token.empty()) {
        json tv_params = continuation.empty() ? json{{"videoId", video_id}} : json{{"continuation", continuation}};
        net::Response tv = post_as_account("next", tv_params, access_token);
        YDoc tdoc = tv.ok() ? yparse(tv.body) : YDoc();
        yyjson_val* t = tdoc.root();
        if (yyjson_is_obj(t)) {
            out = Feed{};
            yyjson_val* sc = continuation.empty() ? yobj(yobj(t, "contents"), "singleColumnWatchNextResults") : nullptr;
            if (yyjson_val* results = sc ? yyjson_obj_get(sc, "results") : nullptr) {
                collect(results, out);
                if (yyjson_val* autoplay = yyjson_obj_get(sc, "autoplay")) {
                    if (yyjson_val* we = yfind(autoplay, "watchEndpoint")) {
                        std::string next_id = ystr(we, "videoId");
                        if (!next_id.empty()) {
                            auto it = std::find_if(out.items.begin(), out.items.end(),
                                                   [&](const FeedItem& i) { return i.video_id == next_id; });
                            if (it != out.items.end() && it != out.items.begin()) {
                                std::rotate(out.items.begin(), it, it + 1);
                            }
                        }
                    }
                }
            } else {
                collect(t, out);
            }
            out.items.erase(std::remove_if(out.items.begin(), out.items.end(),
                                           [&](const FeedItem& i) { return i.video_id == video_id; }),
                            out.items.end());
            OLOG("yt: related %s (account%s) -> %zu items", video_id.c_str(),
                 continuation.empty() ? "" : ", cont", out.items.size());
            if (!out.items.empty() || !continuation.empty()) return true;
        } else {
            error = http_error("tv next", tv);
            OLOG("yt: related %s: %s", video_id.c_str(), error.c_str());
            if (!continuation.empty()) return false;
        }
    }
    std::string visitor, web_ver;
    if (!ensure_visitor(visitor, web_ver, error)) return false;
    ClientDef def = client_def("search", visitor, web_ver);
    json body = {{"context", {{"client", def.client}}}};
    if (continuation.empty()) {
        body["videoId"] = video_id;
    } else {
        body["continuation"] = continuation;
    }
    net::Request req;
    req.method = "POST";
    req.url = "https://www.youtube.com/youtubei/v1/next?prettyPrint=false";
    req.headers = client_headers(def, visitor);
    req.body = body.dump();
    net::Response res = net::fetch(req);
    if (!res.ok()) {
        error = "next: "+ (res.error.empty() ? "http " + std::to_string(res.status) : res.error);
        return false;
    }
    YDoc doc = yparse(res.body);
    if (!doc) {
        error = "next: invalid json";
        return false;
    }
    out = Feed{};
    // Only the secondary results column: the primary one repeats the current video.
    yyjson_val* secondary = yobj(yobj(yobj(doc.root(), "contents"), "twoColumnWatchNextResults"), "secondaryResults");
    collect(secondary ? secondary : doc.root(), out);
    out.items.erase(std::remove_if(out.items.begin(), out.items.end(),
                                   [&](const FeedItem& i) { return i.video_id == video_id; }),
                    out.items.end());
    OLOG("yt: related %s%s -> %zu items", video_id.c_str(), continuation.empty() ? "" : " (cont)", out.items.size());
    return true;
}

namespace {

// First continuationCommand token under j.
std::string continuation_token(yyjson_val* j) {
    if (yyjson_val* cmd = yfind(j, "continuationCommand"); yyjson_is_obj(cmd))
        return ystr(cmd, "token");
    return "";
}

// Extracts the initial comments section continuation token from a WEB "next" watch-page response.
std::string find_comments_token(yyjson_val* j) {
    if (!j) return "";
    if (yyjson_is_obj(j)) {
        if (yyjson_val* isr = yobj(j, "itemSectionRenderer");
            isr && ysv(yyjson_obj_get(isr, "sectionIdentifier")) == "comment-item-section") {
            return continuation_token(isr);
        }
        yyjson_obj_iter it = yyjson_obj_iter_with(j);
        while (yyjson_val* k = yyjson_obj_iter_next(&it)) {
            if (is_ignored_branch(ysv(k))) continue;
            std::string tok = find_comments_token(yyjson_obj_iter_get_val(k));
            if (!tok.empty()) return tok;
        }
    } else if (yyjson_is_arr(j)) {
        yyjson_arr_iter it = yyjson_arr_iter_with(j);
        while (yyjson_val* v = yyjson_arr_iter_next(&it)) {
            std::string tok = find_comments_token(v);
            if (!tok.empty()) return tok;
        }
    }
    return "";
}

}  // namespace

bool Client::details(const std::string& video_id, const std::string& access_token, VideoDetails& out,
                     std::string& error) {
    std::string visitor, web_ver;
    if (!ensure_visitor(visitor, web_ver, error)) return false;
    ClientDef def = client_def("search", visitor, web_ver);
    json body = {{"context", {{"client", def.client}}}, {"videoId", video_id}};
    net::Request req;
    req.method = "POST";
    req.url = "https://www.youtube.com/youtubei/v1/next?prettyPrint=false";
    req.headers = client_headers(def, visitor);
    req.body = body.dump();

    // Fetch the account's subscription state (TV "next") concurrently with the WEB "next" page.
    net::Response tv;
    std::thread tv_thread;
    if (!access_token.empty()) {
        tv_thread = std::thread([&] { tv = post_as_account("next", {{"videoId", video_id}}, access_token); });
    }
    net::Response res = net::fetch(req);
    if (tv_thread.joinable()) tv_thread.join();

    if (!res.ok()) {
        error = http_error("next", res);
        return false;
    }
    YDoc doc = yparse(res.body);
    yyjson_val* r = doc.root();
    if (!r) {
        error = "next: invalid json";
        return false;
    }
    out = VideoDetails{};
    if (yyjson_val* primary = yfind(r, "videoPrimaryInfoRenderer"))
        out.date = ytext(yobj(primary, "dateText"));
    if (yyjson_val* owner = yfind(r, "videoOwnerRenderer")) {
        out.channel = ytext(yobj(owner, "title"));
        out.channel_avatar = ythumb(yobj(owner, "thumbnail"));
        out.subscribers = ytext(yobj(owner, "subscriberCountText"));
        // The channel link moved from navigationEndpoint into the title runs: take any browseId under the owner.
        if (yyjson_val* browse = yfind(owner, "browseId"); yyjson_is_str(browse))
            out.channel_id.assign(yyjson_get_str(browse), yyjson_get_len(browse));
    }
    if (out.channel_id.empty())
        if (yyjson_val* sub = yfind(r, "subscribeButtonRenderer"); yyjson_is_obj(sub))
            out.channel_id = ystr(sub, "channelId");
    if (out.channel_avatar.compare(0, 2, "//") == 0) out.channel_avatar = "https:" + out.channel_avatar;
    if (yyjson_val* lb = yfind(r, "likeButtonViewModel")) {
        if (yyjson_val* def_btn = yfind(lb, "defaultButtonViewModel")) {
            if (yyjson_val* vm = yfind(def_btn, "buttonViewModel")) {
                std::string t = ystr(vm, "title");
                if (!t.empty()) out.like_count = trimmed(std::move(t));
            }
        }
        if (out.like_count.empty()) {
            if (yyjson_val* ent = yfind(lb, "likeCountEntity")) {
                if (yyjson_val* txt = yfind(ent, "likeCountIfIndifferent"))
                    out.like_count = ytext(txt);
            }
        }
    } else if (yyjson_val* seg = yfind(r, "segmentedLikeDislikeButtonRenderer")) {
        if (yyjson_val* lb = yfind(seg, "likeButton"))
            out.like_count = ytext(yobj(yobj(lb, "toggleButtonRenderer"), "defaultText"));
    }
    if (out.like_count.find_first_of("0123456789") == std::string::npos) out.like_count.clear();
    // Cache the comments section continuation token from this WEB "next" response so opening Comments
    // skips a redundant ~1.5 MB watch-page fetch and parse.
    if (std::string comments_tok = find_comments_token(r); !comments_tok.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        cached_comments_video_id_ = video_id;
        cached_comments_token_ = std::move(comments_tok);
        cached_comments_at_ = std::chrono::steady_clock::now();
    }
    doc.reset();  // release the WEB arena before parsing the TV account-state arena

    // Parse the account's subscribe state fetched above.
    if (!access_token.empty()) {
        YDoc tdoc = tv.ok() ? yparse(tv.body) : YDoc();
        yyjson_val* t = tdoc.root();
        if (yyjson_is_obj(t)) {
            if (yyjson_val* sub = yfind(t, "subscribeButtonRenderer"); yyjson_is_obj(sub)) {
                if (yyjson_obj_get(sub, "subscribed")) out.subscribed = ybool(sub, "subscribed") ? 1 : 0;
                if (out.channel_id.empty()) out.channel_id = ystr(sub, "channelId");
            }
            if (out.subscribed < 0) {
                if (yyjson_val* sse = yfind(t, "subscriptionStateEntity"); yyjson_is_obj(sse)) {
                    if (yyjson_obj_get(sse, "subscribed")) out.subscribed = ybool(sse, "subscribed") ? 1 : 0;
                }
            }
        } else {
            OLOG("yt: details %s: account state %s", video_id.c_str(), http_error("tv next", tv).c_str());
        }
    }
    OLOG("yt: details %s: '%s' %s, %s, likes %s, subscribed %d", video_id.c_str(), out.channel.c_str(),
         out.subscribers.c_str(), out.date.c_str(), out.like_count.c_str(), out.subscribed);
    return true;
}

bool Client::channel(const std::string& channel_id, const std::string& continuation, Feed& out, ChannelInfo& header,
                     std::string& error) {
    std::string visitor, web_ver;
    if (!ensure_visitor(visitor, web_ver, error)) return false;
    ClientDef def = client_def("search", visitor, web_ver);
    json body = {{"context", {{"client", def.client}}}};
    if (continuation.empty()) {
        body["browseId"] = channel_id;
        body["params"] = "EgZ2aWRlb3PyBgQKAjoA";  // "Vidéos" tab
    } else {
        body["continuation"] = continuation;
    }
    net::Request req;
    req.method = "POST";
    req.url = "https://www.youtube.com/youtubei/v1/browse?prettyPrint=false";
    req.headers = client_headers(def, visitor);
    req.body = body.dump();
    net::Response res = net::fetch(req);
    if (!res.ok()) {
        error = http_error("channel", res);
        return false;
    }
    record_response(res.body);
    YDoc doc = yparse(res.body);
    yyjson_val* r = doc.root();
    if (!r) {
        error = "channel: invalid json";
        return false;
    }
    out = Feed{};
    collect(r, out);
    if (yyjson_val* meta = yfind(r, "channelMetadataRenderer")) {
        header.title = ystr(meta, "title");
        header.avatar = ythumb(yobj(meta, "avatar"));
    }
    if (yyjson_val* page = yfind(r, "pageHeaderViewModel")) {
        std::vector<std::string> parts;
        texts_under(page, "metadataParts", parts);
        for (const std::string& p : parts)
            if (contains_any(p, {"abonn", "subscriber"})) header.subscribers = p;
    }
    // Videos of a channel page carry no channel name: lockups then list [views, age] where parse_lockup
    // expects [channel, views, age].
    for (FeedItem& item : out.items) {
        if (contains_any(item.channel, {"vue", "view"})) {
            item.published = item.views;
            item.views = item.channel;
            item.channel.clear();
        }
        if (item.channel.empty()) item.channel = header.title;
    }
    OLOG("yt: channel %s%s -> %zu items", channel_id.c_str(), continuation.empty() ? "" : " (next page)",
         out.items.size());
    return true;
}

namespace {

std::string ypath_str(yyjson_val* j, std::initializer_list<const char*> path) {
    yyjson_val* at = j;
    for (const char* k : path) {
        if (!yyjson_is_obj(at)) return "";
        at = yyjson_obj_get(at, k);
        if (!at) return "";
    }
    return yyjson_is_str(at) ? std::string(yyjson_get_str(at), yyjson_get_len(at)) : "";
}

// New layout: the thread only holds keys, the texts are commentEntityPayload mutations of frameworkUpdates.
Comment comment_from_entity(yyjson_val* p) {
    Comment c;
    yyjson_val* props = yobj(p, "properties");
    yyjson_val* author = yobj(p, "author");
    yyjson_val* toolbar = yobj(p, "toolbar");
    c.id = ystr(props, "commentId");
    c.text = ypath_str(props, {"content", "content"});
    c.published = ystr(props, "publishedTime");
    c.author = ystr(author, "displayName");
    c.avatar = ystr(author, "avatarThumbnailUrl");
    c.creator = ybool(author, "isCreator");
    if (c.avatar.empty())
        if (yyjson_val* src = yfind(yobj(p, "avatar"), "sources"); yyjson_is_arr(src))
            if (yyjson_val* last = yyjson_arr_get_last(src)) c.avatar = ystr(last, "url");
    c.likes = trimmed(ystr(toolbar, "likeCountNotliked"));
    if (c.likes == "0") c.likes.clear();
    c.replies = trimmed(ystr(toolbar, "replyCount"));
    if (c.replies == "0") c.replies.clear();
    return c;
}

// Old layout: commentRenderer carries everything.
Comment comment_from_renderer(yyjson_val* r) {
    Comment c;
    c.id = ystr(r, "commentId");
    c.author = ytext(yobj(r, "authorText"));
    c.avatar = ythumb(yobj(r, "authorThumbnail"));
    c.text = ytext(yobj(r, "contentText"));
    c.published = ytext(yobj(r, "publishedTimeText"));
    c.likes = trimmed(ytext(yobj(r, "voteCount")));
    if (yyjson_is_obj(r) && yyjson_obj_get(r, "replyCount"))
        c.replies = std::to_string(yint(r, "replyCount"));
    if (c.replies == "0") c.replies.clear();
    c.creator = ybool(r, "authorIsChannelOwner");
    c.pinned = yyjson_is_obj(r) && yyjson_obj_get(r, "pinnedCommentBadge") != nullptr;
    return c;
}

// Header + threads + next page of a comments continuation answer (reload or append), or of a create_comment one.
void parse_comments(yyjson_val* r, CommentPage& out) {
    std::unordered_map<std::string_view, yyjson_val*> entities;
    if (yyjson_val* mutations = yarr(yobj(yobj(r, "frameworkUpdates"), "entityBatchUpdate"), "mutations")) {
        yyjson_arr_iter it = yyjson_arr_iter_with(mutations);
        while (yyjson_val* m = yyjson_arr_iter_next(&it)) {
            yyjson_val* e = yobj(yobj(m, "payload"), "commentEntityPayload");
            if (!e) continue;
            if (std::string_view ek = ysv(yyjson_obj_get(m, "entityKey")); !ek.empty()) entities[ek] = e;
            if (std::string_view k = ysv(yyjson_obj_get(e, "key")); !k.empty()) entities[k] = e;
        }
    }
    auto from_view_model = [&](yyjson_val* vm, Comment& c) {
        yyjson_val* model = yobj(vm, "commentViewModel");
        if (!model) model = vm;
        auto e = entities.find(ysv(yyjson_is_obj(model) ? yyjson_obj_get(model, "commentKey") : nullptr));
        if (e == entities.end()) return false;
        c = comment_from_entity(e->second);
        c.pinned = yyjson_is_obj(model) && yyjson_obj_get(model, "pinnedText") != nullptr;
        return true;
    };
    auto add = [&](Comment c) {
        if (c.avatar.compare(0, 2, "//") == 0) c.avatar = "https:" + c.avatar;
        if (!c.text.empty() || !c.author.empty()) out.items.push_back(std::move(c));
    };
    std::function<void(yyjson_val*)> walk = [&](yyjson_val* j) {
        if (!j) return;
        if (yyjson_is_arr(j)) {
            yyjson_arr_iter it = yyjson_arr_iter_with(j);
            while (yyjson_val* e = yyjson_arr_iter_next(&it)) walk(e);
            return;
        }
        if (!yyjson_is_obj(j)) return;
        yyjson_obj_iter it = yyjson_obj_iter_with(j);
        while (yyjson_val* kp = yyjson_obj_iter_next(&it)) {
            std::string_view k = ysv(kp);
            yyjson_val* v = yyjson_obj_iter_get_val(kp);
            if (is_ignored_branch(k) || k == "frameworkUpdates") continue;
            if (k == "commentsHeaderRenderer" && yyjson_is_obj(v)) {
                out.count = ytext(yobj(v, "countText"));
                if (out.count.empty()) out.count = ytext(yobj(v, "commentsCount"));
                if (yyjson_val* items = yfind(v, "subMenuItems"); yyjson_is_arr(items)) {
                    if (yyjson_val* i0 = yyjson_arr_get(items, 0)) out.sort_top = continuation_token(i0);
                    if (yyjson_val* i1 = yyjson_arr_get(items, 1)) out.sort_newest = continuation_token(i1);
                }
                if (yyjson_val* params = yfind(v, "createCommentParams"); yyjson_is_str(params))
                    out.create_params.assign(yyjson_get_str(params), yyjson_get_len(params));
            } else if (k == "commentThreadRenderer" && yyjson_is_obj(v)) {
                Comment c;
                if (yyjson_val* vm = yfind(v, "commentViewModel")) {
                    if (!from_view_model(vm, c)) continue;
                } else if (yyjson_val* cr = yfind(v, "commentRenderer")) {
                    c = comment_from_renderer(cr);
                } else {
                    continue;
                }
                if (yyjson_val* rep = yyjson_obj_get(v, "replies")) c.reply_token = continuation_token(rep);
                add(std::move(c));
            } else if (k == "commentViewModel" && yyjson_is_obj(v)) {
                // A reply: outside any thread.
                Comment c;
                if (from_view_model(v, c)) add(std::move(c));
            } else if (k == "commentRenderer" && yyjson_is_obj(v)) {
                add(comment_from_renderer(v));
            } else if (k == "continuationItemRenderer" && yyjson_is_obj(v)) {
                // The thread's own replies continuation sits inside commentThreadRenderer, skipped above.
                std::string token = continuation_token(v);
                if (!token.empty()) out.continuation = std::move(token);
            } else {
                walk(v);
            }
        }
    };
    walk(r);
}

// Params of the comment endpoints. YouTube only hands them to WEB sessions, which refuse the TV OAuth token, so
// they are built here: protobuf, base64, '=' percent-encoded.
void pb_varint(std::string& out, uint64_t v) {
    for (; v >= 0x80; v >>= 7) out += static_cast<char>((v & 0x7f) | 0x80);
    out += static_cast<char>(v);
}

void pb_int(std::string& out, int field, uint64_t v) {
    pb_varint(out, static_cast<uint64_t>(field) << 3);
    pb_varint(out, v);
}

void pb_string(std::string& out, int field, const std::string& v) {
    pb_varint(out, (static_cast<uint64_t>(field) << 3) | 2);
    pb_varint(out, v.size());
    out += v;
}

std::string params_token(const std::string& pb) {
    static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((pb.size() + 2) / 3) * 4 + 4);
    for (size_t i = 0; i < pb.size(); i += 3) {
        size_t left = pb.size() - i;
        unsigned n = static_cast<unsigned char>(pb[i]) << 16;
        if (left > 1) n |= static_cast<unsigned char>(pb[i + 1]) << 8;
        if (left > 2) n |= static_cast<unsigned char>(pb[i + 2]);
        out += kAlphabet[(n >> 18) & 63];
        out += kAlphabet[(n >> 12) & 63];
        if (left > 1) out += kAlphabet[(n >> 6) & 63];
        else out += "%3D";
        if (left > 2) out += kAlphabet[n & 63];
        else out += "%3D";
    }
    return out;
}

// {2: videoId, [4: commentId,] 5: {1: 0}, 10: 7}: a comment, or a reply in a thread.
std::string create_params(const std::string& video_id, const std::string& comment_id) {
    if (video_id.empty()) return "";
    std::string pb, inner;
    pb_string(pb, 2, video_id);
    if (!comment_id.empty()) pb_string(pb, 4, comment_id);
    pb_int(inner, 1, 0);
    pb_string(pb, 5, inner);
    pb_int(pb, 10, 7);
    return params_token(pb);
}

// {1: action, 2: 2, 3: commentId, 5: videoId, 6: 0, 7: undo, 23: " "}, as in the WEB toolbar's own commands.
std::string comment_action_params(int action, bool undo, const std::string& video_id, const std::string& comment_id) {
    std::string pb;
    pb_int(pb, 1, static_cast<uint64_t>(action));
    pb_int(pb, 2, 2);
    pb_string(pb, 3, comment_id);
    pb_string(pb, 5, video_id);
    pb_int(pb, 6, 0);
    pb_int(pb, 7, undo ? 1 : 0);
    pb_string(pb, 23, " ");
    return params_token(pb);
}

// The account's vote on each comment: the TV client answers the same continuation as WEB, as commentRenderers
// carrying it. Cosmetic: on any failure the comments simply show no vote.
void apply_votes(net::Response& res, CommentPage& page) {
    if (!res.ok()) {
        OLOG("yt: comment votes: http %d", res.status);
        return;
    }
    YDoc doc = yparse(res.body);
    if (!doc) return;
    std::unordered_map<std::string_view, std::string_view> votes;
    std::function<void(yyjson_val*)> walk = [&](yyjson_val* j) {
        if (!j) return;
        if (yyjson_is_arr(j)) {
            yyjson_arr_iter it = yyjson_arr_iter_with(j);
            while (yyjson_val* e = yyjson_arr_iter_next(&it)) walk(e);
        } else if (yyjson_is_obj(j)) {
            yyjson_obj_iter it = yyjson_obj_iter_with(j);
            while (yyjson_val* kp = yyjson_obj_iter_next(&it)) {
                std::string_view k = ysv(kp);
                yyjson_val* v = yyjson_obj_iter_get_val(kp);
                if (is_ignored_branch(k)) continue;
                if (k == "commentRenderer" && yyjson_is_obj(v)) {
                    std::string_view cid = ysv(yyjson_obj_get(v, "commentId"));
                    if (!cid.empty()) {
                        votes[cid] = ybool(v, "isLiked") ? std::string_view("LIKE") : ysv(yyjson_obj_get(v, "voteStatus"));
                    }
                } else {
                    walk(v);
                }
            }
        }
    };
    walk(doc.root());
    for (Comment& c : page.items) {
        auto v = votes.find(c.id);
        if (v == votes.end()) continue;
        c.liked = v->second == "LIKE";
        c.disliked = v->second == "DISLIKE";
    }
}

}  // namespace

bool Client::comments(const std::string& video_id, const std::string& continuation, const std::string& access_token,
                      CommentPage& out, std::string& error) {
    std::string visitor, web_ver;
    if (!ensure_visitor(visitor, web_ver, error)) return false;
    ClientDef def = client_def("search", visitor, web_ver);
    auto next = [&](json body, YDoc& doc) {
        body["context"] = {{"client", def.client}};
        net::Request req;
        req.method = "POST";
        req.url = "https://www.youtube.com/youtubei/v1/next?prettyPrint=false";
        req.headers = client_headers(def, visitor);
        req.body = body.dump();
        net::Response res = net::fetch(req);
        if (!res.ok()) {
            error = http_error("comments", res);
            return false;
        }
        record_response(res.body);
        doc = yparse(res.body);
        if (!doc) {
            error = "comments: invalid json";
            return false;
        }
        return true;
    };
    out = CommentPage{};
    std::string token = continuation;
    bool from_cache = false;
    if (token.empty()) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (cached_comments_video_id_ == video_id && !cached_comments_token_.empty() &&
                std::chrono::steady_clock::now() - cached_comments_at_ < std::chrono::minutes(10)) {
                token = cached_comments_token_;
                from_cache = true;
            }
        }
        if (token.empty()) {
            // The watch page only holds the continuation of the comments section.
            YDoc watch;
            if (!next({{"videoId", video_id}}, watch)) return false;
            token = find_comments_token(watch.root());
            if (token.empty()) {
                out.disabled = true;  // turned off, or a live chat instead
                OLOG("yt: comments %s: none (disabled)", video_id.c_str());
                return true;
            }
        }
    }
    // Fetch the account's votes (TV "next") concurrently with the WEB comments page when signed in.
    net::Response votes_res;
    std::thread votes_thread;
    if (!access_token.empty()) {
        votes_thread = std::thread([&] { votes_res = post_as_account("next", {{"continuation", token}}, access_token); });
    }
    YDoc doc;
    bool ok = next({{"continuation", token}}, doc);
    if (votes_thread.joinable()) votes_thread.join();
    if (!ok && from_cache) {
        // Fallback if the cached continuation token expired or failed: fetch the watch page normally.
        YDoc watch;
        if (!next({{"videoId", video_id}}, watch)) return false;
        token = find_comments_token(watch.root());
        if (token.empty()) {
            out.disabled = true;
            return true;
        }
        if (!access_token.empty()) {
            votes_thread = std::thread([&] { votes_res = post_as_account("next", {{"continuation", token}}, access_token); });
        }
        ok = next({{"continuation", token}}, doc);
        if (votes_thread.joinable()) votes_thread.join();
    }
    if (!ok) return false;
    parse_comments(doc.root(), out);
    doc.reset();  // release WEB comments arena before parsing TV votes arena
    if (out.create_params.empty()) out.create_params = create_params(video_id, "");
    if (!access_token.empty() && !out.items.empty()) apply_votes(votes_res, out);
    OLOG("yt: comments %s%s -> %zu items, count '%s', next %s, post %s", video_id.c_str(),
         continuation.empty() ? "" : " (continuation)", out.items.size(), out.count.c_str(),
         out.continuation.empty() ? "no" : "yes", out.create_params.empty() ? "no" : "yes");
    return true;
}

bool Client::browse(const std::string& browse_id, const std::string& continuation, const std::string& access_token,
                    Feed& out, std::string& error) {
    ClientDef def = client_def("browse", "");
    json body = {{"context", {{"client", def.client}}}};
    if (continuation.empty()) body["browseId"] = browse_id;
    else body["continuation"] = continuation;
    net::Request req;
    req.method = "POST";
    req.url = "https://www.youtube.com/youtubei/v1/browse?prettyPrint=false";
    req.headers = client_headers(def, "");
    req.headers.emplace_back("Authorization", "Bearer " + access_token);
    req.body = body.dump();
    net::Response res = net::fetch(req);
    if (!res.ok()) {
        error = "browse " + browse_id + ": " + (res.error.empty() ? "http " + std::to_string(res.status) : res.error);
        return false;
    }
    record_response(res.body);
    YDoc doc = yparse(res.body);
    if (!doc) {
        error = "browse: invalid json";
        return false;
    }
    out = Feed{};
    if (browse_id == "FEwhat_to_watch") {
        // Home: rows by topic (next pages bring more rows).
        collect_shelves(doc.root(), out);
    } else {
        // Subscriptions "Tous" tab: a titled "most relevant" shelf, then untitled shelves in date order.
        Feed picked;
        collect(doc.root(), out, browse_id == "FEsubscriptions" && continuation.empty() ? &picked : nullptr);
        out.highlights = std::move(picked.items);
    }
    OLOG("yt: browse %s%s -> %zu items, %zu highlights, %zu shelves", browse_id.c_str(),
         continuation.empty() ? "" : " (next page)", out.items.size(), out.highlights.size(), out.shelves.size());
    return true;
}

bool suggest(const std::string& query, std::vector<std::string>& out, std::string& error) {
    std::string q;
    for (unsigned char c : query) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.') {
            q += static_cast<char>(c);
        } else {
            char hex[4];
            snprintf(hex, sizeof(hex), "%%%02X", c);
            q += hex;
        }
    }
    net::Request req;
    req.url = "https://suggestqueries.google.com/complete/search?client=firefox&ds=yt&ie=utf-8&oe=utf-8&hl=" + cfg::language() + "&q=" + q;
    net::Response res = net::fetch(req);
    if (!res.ok()) {
        error = "suggest: " + (res.error.empty() ? "http " + std::to_string(res.status) : res.error);
        return false;
    }
    YDoc doc = yparse(res.body);  // ["query", ["s1", "s2", ...]]
    out.clear();
    if (yyjson_val* r = doc.root(); yyjson_is_arr(r)) {
        if (yyjson_val* arr = yyjson_arr_get(r, 1); yyjson_is_arr(arr)) {
            yyjson_arr_iter it = yyjson_arr_iter_with(arr);
            while (yyjson_val* s = yyjson_arr_iter_next(&it))
                if (yyjson_is_str(s)) out.emplace_back(yyjson_get_str(s), yyjson_get_len(s));
        }
    }
    return true;
}

void Client::record_response(const std::string& body) {
    if (!capture_response_) return;
    std::lock_guard<std::mutex> lock(mutex_);
    last_response_ = body;
}

Client& shared_client() {
    static omega::NoDestroy<Client> client;
    return *client;
}

std::string Format::video_codec() const {
    if (!is_video()) return "";
    VideoCodec c = codec != VideoCodec::Unknown ? codec : classify_video_codec(mime, codecs);
    switch (c) {
        case VideoCodec::H264: return "avc1";
        case VideoCodec::VP9: return "vp9";
        case VideoCodec::AV1: return "av01";
        default: return "";
    }
}

std::vector<const Format*> rank_videos(const VideoInfo& info, int max_height, const std::vector<std::string>& codecs) {
    const long no_match = static_cast<long>(codecs.size());
    auto find_codec_pref = [&](const char* name) -> long {
        auto it = std::find(codecs.begin(), codecs.end(), name);
        return it != codecs.end() ? static_cast<long>(it - codecs.begin()) : no_match;
    };
    const long pref_h264 = find_codec_pref("avc1");
    const long pref_vp9 = find_codec_pref("vp9");
    const long pref_av1 = find_codec_pref("av01");

    auto codec_of = [](const Format* f) -> VideoCodec {
        if (!f->is_video()) return VideoCodec::Unknown;
        return f->codec != VideoCodec::Unknown ? f->codec : classify_video_codec(f->mime, f->codecs);
    };
    auto pref = [&](VideoCodec c) -> long {
        switch (c) {
            case VideoCodec::H264: return pref_h264;
            case VideoCodec::VP9: return pref_vp9;
            case VideoCodec::AV1: return pref_av1;
            default: return no_match;
        }
    };
    // H.264 is only chosen when explicitly set as priority (p == 0) or when neither AV1 nor VP9 exists;
    // AV1 and VP9 share tier 1 and compete by efficiency-weighted relative bitrate (AV1 ~1.22x VP9).
    auto codec_tier = [](VideoCodec c, long p) -> int {
        if (c == VideoCodec::H264) return (p == 0) ? 2 : 0;
        return 1;
    };
    auto effective_bitrate = [](const Format* f, VideoCodec c, long p) -> int64_t {
        int64_t weight = (c == VideoCodec::AV1) ? 122 : (c == VideoCodec::VP9) ? 100 : 80;
        int64_t pref_bonus = (p == 0) ? 130 : 100;
        return (f->bitrate * weight * pref_bonus) / 10000;
    };
    std::vector<const Format*> out;
    out.reserve(info.formats.size());
    for (const Format& f : info.formats)
        if (f.height > 0 && f.height <= max_height && pref(codec_of(&f)) < no_match) out.push_back(&f);
    auto hdr_rank = [](HdrMode m) -> int {
        if (m == HdrMode::HDR10) return 2;
        if (m == HdrMode::HLG) return 1;
        return 0;
    };
    std::stable_sort(out.begin(), out.end(), [&](const Format* a, const Format* b) {
        if (a->height != b->height) return a->height > b->height;
        int ha = hdr_rank(a->hdr_mode), hb = hdr_rank(b->hdr_mode);
        if (ha != hb) return ha > hb;
        if (a->fps != b->fps) return a->fps > b->fps;
        bool a_prem = (a->itag == 356 || a->itag == 616);
        bool b_prem = (b->itag == 356 || b->itag == 616);
        if (a_prem != b_prem) return a_prem > b_prem;
        VideoCodec ca = codec_of(a), cb = codec_of(b);
        long pa = pref(ca), pb = pref(cb);
        int ta = codec_tier(ca, pa), tb = codec_tier(cb, pb);
        if (ta != tb) return ta > tb;
        int64_t ea = effective_bitrate(a, ca, pa), eb = effective_bitrate(b, cb, pb);
        if (ea != eb) return ea > eb;
        if (pa != pb) return pa < pb;
        return a->bitrate > b->bitrate;
    });
    return out;
}

std::vector<std::string> parse_codec_list(const std::string& list) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= list.size()) {
        size_t comma = list.find(',', start);
        if (comma == std::string::npos) comma = list.size();
        std::string item = list.substr(start, comma - start);
        item.erase(0, item.find_first_not_of(' '));
        item.erase(item.find_last_not_of(' ') + 1);
        if (!item.empty()) out.push_back(item);
        start = comma + 1;
    }
    return out;
}

std::vector<const Format*> rank_audios(const VideoInfo& info, const std::string& track) {
    auto playable_audio = [&info](const Format& f) {
        if (!f.is_audio() || f.audio_channels > 2) return false;
        if (f.mime == "audio/mp4" && f.codecs.compare(0, 4, "mp4a") == 0) return true;
        if (!info.is_live && f.mime == "audio/webm" && f.codecs == "opus") return true;
        return false;
    };
    auto is_descriptive = [](const Format& f) {
        const std::string& name = f.audio_track_name;
        constexpr std::string_view needle = "descriptive";
        if (name.size() < needle.size()) return false;
        for (size_t i = 0; i + needle.size() <= name.size(); ++i) {
            size_t j = 0;
            while (j < needle.size() &&
                   std::tolower(static_cast<unsigned char>(name[i + j])) == static_cast<unsigned char>(needle[j]))
                ++j;
            if (j == needle.size()) return true;
        }
        return false;
    };
    // The track to play: exact id, else same language ("fr" matches "fr.3" and "fr-FR.3", preferring default /
    // non-descriptive tracks), else the default.
    std::string wanted;
    if (!track.empty()) {
        for (const Format& f : info.formats)
            if (playable_audio(f) && f.audio_track_id == track) {
                wanted = track;
                break;
            }
        if (wanted.empty()) {
            std::string lang = track.substr(0, track.find_first_of(".-"));
            int best_score = -1;
            for (const Format& f : info.formats) {
                if (!playable_audio(f) || f.audio_track_id.empty()) continue;
                if (f.audio_track_id.substr(0, f.audio_track_id.find_first_of(".-")) != lang) continue;
                int score = (is_descriptive(f) ? 0 : 2) + (f.audio_default ? 1 : 0);
                if (score > best_score) {
                    best_score = score;
                    wanted = f.audio_track_id;
                }
            }
        }
    }
    if (wanted.empty()) {
        int best_score = -1;
        for (const Format& f : info.formats) {
            if (!playable_audio(f) || !f.audio_default) continue;
            int score = is_descriptive(f) ? 0 : 1;
            if (score > best_score) {
                best_score = score;
                wanted = f.audio_track_id;
            }
        }
    }
    std::vector<const Format*> out;
    for (const Format& f : info.formats) {
        if (!playable_audio(f) || (!wanted.empty() && f.audio_track_id != wanted)) continue;
        out.push_back(&f);
    }
    // Weight Opus ~1.35x AAC: 128-160k Opus (itag 251) beats 128k AAC (140), while 256k Premium AAC (141) beats
    // 128-160k Opus and 128k AAC beats low-bitrate 48-64k Opus (249/250).
    auto effective_audio_bitrate = [](const Format* f) -> int64_t {
        int64_t weight = (f->codecs == "opus") ? 135 : 100;
        return (f->bitrate * weight) / 100;
    };
    std::stable_sort(out.begin(), out.end(), [&](const Format* a, const Format* b) {
        int64_t ea = effective_audio_bitrate(a), eb = effective_audio_bitrate(b);
        if (ea != eb) return ea > eb;
        bool a_opus = (a->codecs == "opus");
        bool b_opus = (b->codecs == "opus");
        if (a_opus != b_opus) return a_opus > b_opus;
        return a->bitrate > b->bitrate;
    });
    return out;
}

const Format* pick_audio(const VideoInfo& info, const std::string& track) {
    auto list = rank_audios(info, track);
    return list.empty() ? nullptr : list.front();
}

bool fetch_captions(const std::string& url, const std::string& translate_to, std::vector<CaptionCue>& out,
                    std::string& error) {
    net::Request req;
    std::string base = drop_param(drop_param(url, "fmt"), "tlang");
    if (base.find('?') == std::string::npos) base += "?fmt=json3";
    else if (base.back() == '?' || base.back() == '&') base += "fmt=json3";
    else base += "&fmt=json3";
    req.url = std::move(base);
    if (!translate_to.empty()) req.url += "&tlang=" + translate_to;
    req.headers = {{"User-Agent", cfg::str({"innertube", "search", "userAgent"})}};
    net::Response res = net::fetch(req);
    if (!res.ok()) {
        error = "captions: " + (res.error.empty() ? "http " + std::to_string(res.status) : res.error);
        return false;
    }
    if (res.body.empty()) {
        error = "captions: empty answer";
        return false;
    }
    YDoc doc = yparse(res.body);
    yyjson_val* r = doc.root();
    if (!yyjson_is_obj(r)) {
        error = "captions: invalid json";
        return false;
    }
    out.clear();
    // json3: events[].{tStartMs, dDurationMs, segs:[{utf8}]}; ASR tracks split lines into word segments.
    if (yyjson_val* events = yarr(r, "events")) {
        out.reserve(yyjson_arr_size(events));
        yyjson_arr_iter eit = yyjson_arr_iter_with(events);
        while (yyjson_val* e = yyjson_arr_iter_next(&eit)) {
            yyjson_val* segs = yarr(e, "segs");
            if (!segs) continue;
            CaptionCue cue;
            yyjson_arr_iter sit = yyjson_arr_iter_with(segs);
            while (yyjson_val* seg = yyjson_arr_iter_next(&sit)) {
                std::string_view sv = ysv(yyjson_is_obj(seg) ? yyjson_obj_get(seg, "utf8") : nullptr);
                cue.text.append(sv.data(), sv.size());
            }
            size_t first = cue.text.find_first_not_of(" \r\n\t");
            if (first == std::string::npos) continue;
            cue.text = cue.text.substr(first, cue.text.find_last_not_of(" \r\n\t") + 1 - first);
            cue.start_ms = yint(e, "tStartMs");
            int64_t dur = yint(e, "dDurationMs");
            cue.end_ms = dur > 0 ? cue.start_ms + dur : 0;
            out.push_back(std::move(cue));
        }
    }
    // Infer end_ms for cues that omit dDurationMs (common on ASR / live-originated json3 tracks).
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i].end_ms > out[i].start_ms) continue;
        if (i + 1 < out.size() && out[i + 1].start_ms > out[i].start_ms) {
            out[i].end_ms = std::min(out[i + 1].start_ms, out[i].start_ms + int64_t{5000});
        } else {
            out[i].end_ms = out[i].start_ms + 3000;
        }
    }
    OLOG("yt: captions %s -> %zu cues", translate_to.empty() ? "" : ("-> " + translate_to).c_str(), out.size());
    return true;
}

bool set_subscribed(const std::string& channel_id, bool subscribe, const std::string& access_token, std::string& error) {
    std::string endpoint = subscribe ? "subscription/subscribe" : "subscription/unsubscribe";
    net::Response res = post_as_account(endpoint, {{"channelIds", json::array({channel_id})}}, access_token);
    if (!res.ok()) {
        error = http_error(endpoint.c_str(), res);
        return false;
    }
    OLOG("yt: %s %s ok", endpoint.c_str(), channel_id.c_str());
    return true;
}

namespace {

// A comment or reply creation: only a succeeded status or the new comment itself counts (a 200 can be a refusal).
bool create_comment_request(const char* endpoint, const json& body, const std::string& access_token, Comment& out,
                            std::string& error) {
    net::Response res = post_as_account(endpoint, body, access_token);
    if (!res.ok()) {
        error = http_error(endpoint, res);
        OLOG("yt: %s %s", error.c_str(), res.body.substr(0, 300).c_str());
        return false;
    }
    std::string body_preview = res.body.substr(0, 600);
    YDoc doc = yparse(res.body);
    CommentPage page;
    std::string status;
    if (yyjson_val* r = doc.root(); yyjson_is_obj(r)) {
        parse_comments(r, page);
        status = ystr(yobj(r, "actionResult"), "status");
    }
    if (page.items.empty() && status != "STATUS_SUCCEEDED") {
        error = std::string(endpoint) + ": refused" + (status.empty() ? std::string() : " (" + status + ")");
        OLOG("yt: %s %s", error.c_str(), body_preview.c_str());
        return false;
    }
    if (!page.items.empty()) out = page.items.front();
    OLOG("yt: %s ok (%s, status '%s')", endpoint, page.items.empty() ? "not returned" : out.id.c_str(), status.c_str());
    return true;
}

}  // namespace

bool post_comment(const std::string& create_params, const std::string& text, const std::string& access_token,
                  Comment& out, std::string& error) {
    return create_comment_request("comment/create_comment", {{"createCommentParams", create_params}, {"commentText", text}},
                                  access_token, out, error);
}

bool post_reply(const std::string& video_id, const std::string& comment_id, const std::string& text,
                const std::string& access_token, Comment& out, std::string& error) {
    return create_comment_request("comment/create_comment_reply",
                                  {{"createReplyParams", create_params(video_id, comment_id)}, {"commentText", text}},
                                  access_token, out, error);
}

bool comment_action(const std::string& video_id, const std::string& comment_id, const std::string& action,
                    const std::string& access_token, std::string& error) {
    // Codes read from the WEB toolbar: 5 like, 4 dislike, field 7 undoes them. Never guess one: 6 DELETES the comment.
    int type = action == "like" || action == "unlike" ? 5 : action == "dislike" || action == "undislike" ? 4 : 0;
    bool undo = action == "unlike" || action == "undislike";
    if (type == 0 || video_id.empty() || comment_id.empty()) {
        error = "comment action '" + action + "' not supported";
        return false;
    }
    json body = {{"actions", json::array({comment_action_params(type, undo, video_id, comment_id)})}};
    net::Response res = post_as_account("comment/perform_comment_action", body, access_token);
    if (!res.ok()) {
        error = http_error("comment action", res);
        OLOG("yt: %s %s", error.c_str(), res.body.substr(0, 300).c_str());
        return false;
    }
    std::string body_preview = res.body.substr(0, 600);
    YDoc doc = yparse(res.body);
    bool done = false;
    if (yyjson_val* arr = yarr(doc.root(), "actionResults")) {
        yyjson_arr_iter it = yyjson_arr_iter_with(arr);
        while (yyjson_val* a = yyjson_arr_iter_next(&it))
            done = done || (ysv(yyjson_is_obj(a) ? yyjson_obj_get(a, "status") : nullptr) == "STATUS_SUCCEEDED");
    }
    if (!done) {
        error = "comment action: refused";
        OLOG("yt: %s %s", error.c_str(), body_preview.c_str());
        return false;
    }
    OLOG("yt: comment %s %s ok", action.c_str(), comment_id.c_str());
    return true;
}

bool account_info(const std::string& access_token, AccountInfo& out, std::string& error) {
    ClientDef def = client_def("browse", "");
    json body = {{"context", {{"client", def.client}}}};
    net::Request req;
    req.method = "POST";
    req.url = "https://www.youtube.com/youtubei/v1/account/accounts_list?prettyPrint=false";
    req.headers = client_headers(def, "");
    req.headers.emplace_back("Authorization", "Bearer " + access_token);
    req.body = body.dump();
    net::Response res = net::fetch(req);
    if (!res.ok()) {
        error = "accounts_list: " + (res.error.empty() ? "http " + std::to_string(res.status) : res.error);
        return false;
    }
    YDoc doc = yparse(res.body);
    // accountItem {accountName, accountPhoto, channelHandle, isSelected}: the selected one, else the first.
    std::vector<yyjson_val*> items;
    std::function<void(yyjson_val*)> walk = [&](yyjson_val* j) {
        if (!j) return;
        if (yyjson_is_obj(j)) {
            yyjson_obj_iter it = yyjson_obj_iter_with(j);
            while (yyjson_val* kp = yyjson_obj_iter_next(&it)) {
                std::string_view k = ysv(kp);
                yyjson_val* v = yyjson_obj_iter_get_val(kp);
                if ((k == "accountItem" || k == "accountItemRenderer") && yyjson_is_obj(v))
                    items.push_back(v);
                else
                    walk(v);
            }
        } else if (yyjson_is_arr(j)) {
            yyjson_arr_iter it = yyjson_arr_iter_with(j);
            while (yyjson_val* e = yyjson_arr_iter_next(&it)) walk(e);
        }
    };
    walk(doc.root());
    yyjson_val* item = nullptr;
    for (yyjson_val* i : items)
        if (!item || ybool(i, "isSelected")) item = i;
    if (!item) {
        error = "accounts_list: no account";
        return false;
    }
    out.name = ytext(yobj(item, "accountName"));
    out.handle = ytext(yobj(item, "channelHandle"));
    out.photo = ythumb(yobj(item, "accountPhoto"));
    if (out.photo.compare(0, 2, "//") == 0) out.photo = "https:" + out.photo;
    return true;
}

}  // namespace omega::yt
