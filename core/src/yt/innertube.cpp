#include "yt/innertube.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>
#include <functional>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_map>

#include "util/log.h"
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

const json& child_obj(const json& j, const char* key) {
    static const json kEmpty = json::object();
    if (!j.is_object()) return kEmpty;
    auto it = j.find(key);
    return (it != j.end() && it->is_object()) ? *it : kEmpty;
}

const json& child_arr(const json& j, const char* key) {
    static const json kEmpty = json::array();
    if (!j.is_object()) return kEmpty;
    auto it = j.find(key);
    return (it != j.end() && it->is_array()) ? *it : kEmpty;
}

bool is_skipped_innertube_key(std::string_view k) {
    return k == "responseContext" || k == "trackingParams" || k == "clickTrackingParams" ||
           k == "adPlacements" || k == "adSlots" || k == "playerAds" || k == "adSlotRenderer" ||
           k == "storyboards" || k == "microformat" || k == "loggingDirectives";
}

// SAX-to-DOM builder that skips unused telemetry/ad subtrees with zero allocations.
struct FilteringDomSax {
    json root;
    std::vector<json*> stack;
    std::string pending_key;
    bool skip_next_value = false;
    int skip_depth = 0;
    bool failed = false;

    json* attach(json val) {
        if (stack.empty()) {
            root = std::move(val);
            return &root;
        }
        json* parent = stack.back();
        if (parent->is_array()) {
            parent->push_back(std::move(val));
            return &parent->back();
        }
        json& slot = (*parent)[pending_key];
        pending_key.clear();
        slot = std::move(val);
        return &slot;
    }

    bool consume_scalar(json val) {
        if (skip_depth > 0) return true;
        if (skip_next_value) {
            skip_next_value = false;
            return true;
        }
        attach(std::move(val));
        return true;
    }

    bool null() { return consume_scalar(nullptr); }
    bool boolean(bool v) { return consume_scalar(v); }
    bool number_integer(json::number_integer_t v) { return consume_scalar(v); }
    bool number_unsigned(json::number_unsigned_t v) { return consume_scalar(v); }
    bool number_float(json::number_float_t v, const std::string&) { return consume_scalar(v); }
    bool string(std::string& v) { return consume_scalar(std::move(v)); }
    bool binary(json::binary_t& v) { return consume_scalar(std::move(v)); }

    bool start_object(std::size_t) {
        if (skip_depth > 0) {
            ++skip_depth;
            return true;
        }
        if (skip_next_value) {
            skip_next_value = false;
            skip_depth = 1;
            return true;
        }
        stack.push_back(attach(json::object()));
        return true;
    }
    bool end_object() {
        if (skip_depth > 0) {
            --skip_depth;
            return true;
        }
        if (!stack.empty()) stack.pop_back();
        return true;
    }
    bool start_array(std::size_t) {
        if (skip_depth > 0) {
            ++skip_depth;
            return true;
        }
        if (skip_next_value) {
            skip_next_value = false;
            skip_depth = 1;
            return true;
        }
        stack.push_back(attach(json::array()));
        return true;
    }
    bool end_array() {
        if (skip_depth > 0) {
            --skip_depth;
            return true;
        }
        if (!stack.empty()) stack.pop_back();
        return true;
    }
    bool key(std::string& k) {
        if (skip_depth > 0) return true;
        if (is_skipped_innertube_key(k)) {
            skip_next_value = true;
            return true;
        }
        pending_key = std::move(k);
        return true;
    }
    bool parse_error(std::size_t, const std::string&, const json::exception&) {
        failed = true;
        return false;
    }
};

json parse_innertube_json(std::string& body) {
    FilteringDomSax sax;
    bool ok = json::sax_parse(body, &sax);
    std::string().swap(body);  // free raw HTTP body immediately after parsing
    if (!ok || sax.failed) return json(json::value_t::discarded);
    return std::move(sax.root);
}

std::string text_of(const json& j) {
    if (!j.is_object()) return "";
    auto st = j.find("simpleText");
    if (st != j.end() && st->is_string()) return st->get<std::string>();
    std::string out;
    for (const json& run : child_arr(j, "runs")) out += run.value("text", "");
    if (out.empty()) {
        auto ct = j.find("content");
        if (ct != j.end() && ct->is_string()) out = ct->get<std::string>();  // view-model texts
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
const json* find_key(const json& j, const char* key) {
    if (j.is_object()) {
        auto it = j.find(key);
        if (it != j.end()) return &*it;
        for (const json& v : j)
            if (const json* f = find_key(v, key)) return f;
    } else if (j.is_array()) {
        for (const json& v : j)
            if (const json* f = find_key(v, key)) return f;
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

std::string last_thumbnail(const json& thumbs) {
    const json& list = child_arr(thumbs, "thumbnails");
    return list.empty() ? "" : list.back().value("url", "");
}

bool json_contains_str(const json& j, const char* needle) {
    if (j.is_string()) return j.get_ref<const std::string&>().find(needle) != std::string::npos;
    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it)
            if (json_contains_str(it.value(), needle)) return true;
    } else if (j.is_array()) {
        for (const json& v : j)
            if (json_contains_str(v, needle)) return true;
    }
    return false;
}

// The account's progress on a video: resume overlay (videoRenderer / tile) or progress bar (lockup).
int watched_percent(const json& j) {
    for (const char* key : {"percentDurationWatched", "startPercent"}) {
        if (const json* v = find_key(j, key)) {
            int pct = 0;
            if (v->is_number()) pct = static_cast<int>(v->get<double>());
            else if (v->is_string()) pct = atoi(v->get_ref<const std::string&>().c_str());
            return std::max(0, std::min(100, pct));
        }
    }
    return 0;
}

void parse_video_renderer(const json& v, Feed& out) {
    FeedItem item;
    item.video_id = v.value("videoId", "");
    if (item.video_id.empty()) return;
    item.title = text_of(child_obj(v, "title"));
    const json& owner = child_obj(v, "ownerText");
    item.channel = text_of(!owner.empty() ? owner : child_obj(v, "longBylineText"));
    item.duration = text_of(child_obj(v, "lengthText"));
    const json& short_views = child_obj(v, "shortViewCountText");
    item.views = text_of(!short_views.empty() ? short_views : child_obj(v, "viewCountText"));
    item.published = text_of(child_obj(v, "publishedTimeText"));
    item.thumbnail = "https://i.ytimg.com/vi/" + item.video_id + "/mqdefault.jpg";
    for (const json& badge : child_arr(v, "badges"))
        if (json_contains_str(badge, "LIVE")) item.is_live = true;
    // Broadcasts have no length and count viewers ("18 k spectateurs" / "1.2K watching") instead of views.
    if (item.duration.empty() && (item.views.find("spectateur") != std::string::npos ||
                                  item.views.find("watching") != std::string::npos))
        item.is_live = true;
    item.watched_percent = watched_percent(child_arr(v, "thumbnailOverlays"));
    out.items.push_back(std::move(item));
}

// A row of a playlist page (WEB browse VL<id>): no channel avatar, "views • age" in videoInfo.
void parse_playlist_video(const json& v, Feed& out) {
    if (!v.value("isPlayable", true)) return;  // deleted / private entries
    FeedItem item;
    item.video_id = v.value("videoId", "");
    if (item.video_id.empty()) return;
    item.title = text_of(child_obj(v, "title"));
    item.channel = text_of(child_obj(v, "shortBylineText"));
    item.duration = text_of(child_obj(v, "lengthText"));
    const json& runs = child_arr(child_obj(v, "videoInfo"), "runs");
    if (!runs.empty()) item.views = runs[0].value("text", "");
    if (runs.size() >= 3) item.published = runs[2].value("text", "");
    item.thumbnail = "https://i.ytimg.com/vi/" + item.video_id + "/mqdefault.jpg";
    item.watched_percent = watched_percent(child_arr(v, "thumbnailOverlays"));
    out.items.push_back(std::move(item));
}

// TV client layout (as in SmartTube's TileItem / ytps5): id in onSelectCommand.watchEndpoint, title in
// metadata.tileMetadataRenderer, thumbnail and duration in header.tileHeaderRenderer.
void parse_tile(const json& t, Feed& out) {
    const json& cmd = child_obj(t, "onSelectCommand");
    if (!cmd.contains("watchEndpoint")) return;  // Shorts (reelWatchEndpoint), channels
    FeedItem item;
    item.video_id = cmd["watchEndpoint"].value("videoId", "");
    if (item.video_id.empty()) return;
    if (t.value("contentType", "") == "TILE_CONTENT_TYPE_PLAYLIST") {
        item.playlist_id = cmd["watchEndpoint"].value("playlistId", "");
        if (item.playlist_id.empty()) return;
        item.kind = playlist_kind(item.playlist_id);
    }
    if (already_listed(out, item)) return;
    const json& meta = child_obj(child_obj(t, "metadata"), "tileMetadataRenderer");
    const json& header = child_obj(child_obj(t, "header"), "tileHeaderRenderer");
    item.title = text_of(child_obj(meta, "title"));
    if (item.title.empty()) return;
    // lines[0]: channel • views • age (sometimes split over two lines); "•" separators are skipped.
    std::vector<std::string> parts;
    for (const json& line : child_arr(meta, "lines"))
        for (const json& li : child_arr(child_obj(line, "lineRenderer"), "items")) {
            std::string text = text_of(child_obj(child_obj(li, "lineItemRenderer"), "text"));
            if (!text.empty() && text != "\xE2\x80\xA2" && text != " \xE2\x80\xA2 ") parts.push_back(std::move(text));
        }
    if (!parts.empty()) item.channel = std::move(parts[0]);
    if (parts.size() > 1) item.views = std::move(parts[1]);
    if (parts.size() > 2) item.published = std::move(parts[2]);
    item.thumbnail = "https://i.ytimg.com/vi/" + item.video_id + "/mqdefault.jpg";
    const json& overlays = child_arr(header, "thumbnailOverlays");
    for (const json& o : overlays) {
        const json& ts = child_obj(o, "thumbnailOverlayTimeStatusRenderer");
        if (ts.empty()) continue;
        item.duration = text_of(child_obj(ts, "text"));
        if (ts.value("style", "") == "LIVE") item.is_live = true;
    }
    item.watched_percent = watched_percent(overlays);
    out.items.push_back(std::move(item));
}

// 2025+ layout: lockupViewModel (contentId, lockupMetadataViewModel; duration in a thumbnail badge).
void parse_lockup(const json& l, Feed& out) {
    std::string type = l.value("contentType", "");
    FeedItem item;
    if (type == "LOCKUP_CONTENT_TYPE_PLAYLIST") {
        // Playlist / mix: contentId is the list; play it from its first video (the card's watchEndpoint).
        item.playlist_id = l.value("contentId", "");
        const json* watch = find_key(l, "watchEndpoint");
        if (item.playlist_id.empty() || !watch || !watch->is_object()) return;
        item.video_id = watch->value("videoId", "");
        item.kind = playlist_kind(item.playlist_id);
    } else if (type == "LOCKUP_CONTENT_TYPE_VIDEO") {
        item.video_id = l.value("contentId", "");
    } else {
        return;
    }
    if (item.video_id.size() != 11) return;
    if (already_listed(out, item)) return;
    const json& meta = child_obj(child_obj(l, "metadata"), "lockupMetadataViewModel");
    item.title = text_of(child_obj(meta, "title"));
    if (item.title.empty()) return;
    // metadataRows: [channel], [views, age]
    std::vector<std::string> parts;
    const json& rows = child_arr(child_obj(child_obj(meta, "metadata"), "contentMetadataViewModel"), "metadataRows");
    for (const json& row : rows)
        for (const json& part : child_arr(row, "metadataParts")) {
            std::string text = text_of(child_obj(part, "text"));
            if (!text.empty()) parts.push_back(std::move(text));
        }
    if (!parts.empty()) item.channel = std::move(parts[0]);
    if (parts.size() > 1) item.views = std::move(parts[1]);
    if (parts.size() > 2) item.published = std::move(parts[2]);
    item.thumbnail = "https://i.ytimg.com/vi/" + item.video_id + "/mqdefault.jpg";
    const json& content_image = child_obj(l, "contentImage");
    if (const json* badge = find_key(content_image, "thumbnailBadgeViewModel")) {
        if (const json* txt = find_key(*badge, "text"); txt && txt->is_string()) {
            item.duration = txt->get<std::string>();
            if (item.duration == "LIVE" || item.duration == "EN DIRECT") item.is_live = true;
        }
    }
    if (item.kind == "mix") item.duration = "MIX";  // its badge is an icon
    if (item.views == "Playlist") item.views.clear();  // the type label, already shown by the badge
    item.watched_percent = watched_percent(content_image);
    out.items.push_back(std::move(item));
}

// Walks the whole response: YouTube moves renderers around between layouts, so search by key.
// highlights: where titled TV shelves go (subscriptions), instead of the main list.
void collect(const json& j, Feed& out, Feed* highlights = nullptr) {
    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (it.key() == "shelfRenderer" && it.value().is_object()) {
                const json& shelf = it.value();
                if (shelf.value("tvhtml5ShelfRendererType", "") == "TVHTML5_SHELF_RENDERER_TYPE_SHORTS") continue;
                if (highlights && shelf.contains("headerRenderer")) collect(shelf, *highlights);
                else collect(shelf, out, highlights);
            } else if (it.key() == "videoRenderer" || it.key() == "compactVideoRenderer") {
                parse_video_renderer(it.value(), out);
            } else if (it.key() == "lockupViewModel") {
                parse_lockup(it.value(), out);
            } else if (it.key() == "playlistVideoRenderer") {
                parse_playlist_video(it.value(), out);
            } else if (it.key() == "tileRenderer") {
                parse_tile(it.value(), out);
            } else if (it.key() == "nextContinuationData" && out.continuation.empty()) {
                out.continuation = it.value().value("continuation", "");
            } else if (it.key() == "reelShelfRenderer" || it.key() == "shortsLockupViewModel") {
                continue;  // Shorts: not playable in a TV layout
            } else if (it.key() == "continuationCommand" && out.continuation.empty() && it.value().contains("token")) {
                out.continuation = it.value().value("token", "");
            } else {  // continuationItemViewModel: the token sits in a nested continuationCommand
                collect(it.value(), out, highlights);
            }
        }
    } else if (j.is_array()) {
        for (const json& e : j) collect(e, out, highlights);
    }
}

// The TV home: one Shelf per titled shelfRenderer (Shorts and ads left out); the page continuation is the one
// outside the shelves (theirs only extend their own row).
void collect_shelves(const json& j, Feed& out) {
    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (it.key() == "shelfRenderer" && it.value().is_object()) {
                const json& s = it.value();
                if (s.value("tvhtml5ShelfRendererType", "") == "TVHTML5_SHELF_RENDERER_TYPE_SHORTS") continue;
                Shelf shelf;
                if (s.contains("headerRenderer"))
                    if (const json* title = find_key(s["headerRenderer"], "title")) shelf.title = text_of(*title);
                Feed row;
                collect(child_obj(s, "content"), row);
                if (row.items.empty()) continue;
                shelf.items = std::move(row.items);
                shelf.continuation = row.continuation;
                out.shelves.push_back(std::move(shelf));
            } else if (it.key() == "nextContinuationData" && out.continuation.empty()) {
                out.continuation = it.value().value("continuation", "");
            } else if (it.key() != "adSlotRenderer") {
                collect_shelves(it.value(), out);
            }
        }
    } else if (j.is_array()) {
        for (const json& e : j) collect_shelves(e, out);
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

int64_t to_int(const json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end()) return 0;
    if (it->is_number()) return it->get<int64_t>();
    if (it->is_string()) {
        const std::string& s = it->get_ref<const std::string&>();
        int64_t v = 0;
        if (std::from_chars(s.data(), s.data() + s.size(), v).ec == std::errc{}) return v;
    }
    return 0;
}

// Subtitle tracks and the machine-translation targets (captions.playerCaptionsTracklistRenderer).
void parse_captions(const json& captions, VideoInfo& out) {
    out.captions.clear();
    out.translations.clear();
    const json& list = child_obj(captions, "playerCaptionsTracklistRenderer");
    for (const json& t : child_arr(list, "captionTracks")) {
        CaptionTrack track;
        track.url = t.value("baseUrl", "");
        if (track.url.empty()) continue;
        if (track.url[0] == '/') track.url = "https://www.youtube.com" + track.url;
        track.lang = t.value("languageCode", "");
        track.name = text_of(child_obj(t, "name"));
        track.automatic = t.value("kind", "") == "asr";
        track.translatable = t.value("isTranslatable", false);
        if (track.name.empty()) track.name = track.lang;
        out.captions.push_back(std::move(track));
    }
    for (const json& l : child_arr(list, "translationLanguages")) {
        std::string code = l.value("languageCode", "");
        if (!code.empty()) out.translations.emplace_back(code, text_of(child_obj(l, "languageName")));
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
void texts_under(const json& j, const char* key, std::vector<std::string>& out) {
    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (it.key() == key && it.value().is_array()) {
                for (const json& part : it.value()) out.push_back(text_of(child_obj(part, "text")));
            } else {
                texts_under(it.value(), key, out);
            }
        }
    } else if (j.is_array()) {
        for (const json& e : j) texts_under(e, key, out);
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

bool Client::ensure_visitor(std::string& error) {
    std::lock_guard<std::mutex> inst_lock(mutex_);
    if (!visitor_data_.empty()) return true;
    static std::mutex g_visitor_mutex;
    static std::string g_visitor_data;
    static std::string g_web_client_version;
    std::lock_guard<std::mutex> lock(g_visitor_mutex);
    if (g_visitor_data.empty()) {
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
        g_visitor_data = res.body.substr(at, end - at);
        const std::string ver_key = "\"INNERTUBE_CLIENT_VERSION\":\"";
        size_t v = res.body.find(ver_key);
        if (v != std::string::npos) {
            v += ver_key.size();
            size_t vend = res.body.find('"', v);
            if (vend != std::string::npos && vend > v) g_web_client_version = res.body.substr(v, vend - v);
        }
        if (g_web_client_version.empty()) g_web_client_version = cfg::str({"innertube", "search", "clientVersion"});
        OLOG("yt: visitorData %zu chars, web client %s", g_visitor_data.size(), g_web_client_version.c_str());
    }
    visitor_data_ = g_visitor_data;
    web_client_version_ = g_web_client_version;
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
    if (!ensure_visitor(error)) {
        refusal_ = {"NETWORK", error};
        return false;
    }
    // Always in the config's order, not the last client that worked first: after one video only TVHTML5 could
    // play, every video went through it (player JS + googlevideo refusing its URLs for ~5 s) while VISIONOS,
    // ~1 s, would have played them. A refused VISIONOS costs ~0.2 s.
    std::vector<json> clients = player_clients(!only_client.empty());
    std::string errors;
    for (size_t i = 0; i < clients.size(); i++) {
        const json& c = clients[i];
        std::string name = c.value("clientName", "");
        if (!only_client.empty() && name != only_client) continue;
        if (std::find(skipped_clients_.begin(), skipped_clients_.end(), name) != skipped_clients_.end()) continue;
        std::string err;
        if (try_client(c, video_id, out, err)) {
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
bool Client::try_client(const json& c, const std::string& video_id, VideoInfo& out, std::string& error) {
    bool decipher = c.value("decipher", false);
    if (decipher && !shared_decipher().ensure(error)) return false;
    std::string auth = c.value("auth", "never");
    std::string token;
    if (auth != "never" && shared_auth().signed_in()) {
        std::string token_error;
        token = shared_auth().access_token(token_error);
    }
    if (auth == "always" && !token.empty()) return player_with(c, video_id, out, error, token) && !out.formats.empty();
    if (player_with(c, video_id, out, error) && !out.formats.empty()) return true;
    bool account_may_help = refusal_.status == "LOGIN_REQUIRED" || refusal_.status.rfind("AGE_", 0) == 0;
    if (auth == "retry" && account_may_help && !token.empty()) {
        error.clear();
        if (player_with(c, video_id, out, error, token) && !out.formats.empty()) {
            OLOG("yt: %s played as the signed-in account", video_id.c_str());
            return true;
        }
    }
    if (error.empty()) error = decipher ? "no playable formats" : "no direct formats";
    return false;
}

bool Client::player_with(const json& c, const std::string& video_id, VideoInfo& out, std::string& error,
                         const std::string& access_token) {
    bool decipher = c.value("decipher", false);
    ClientDef def = client_def_from(c, visitor_data_);
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
    req.headers = client_headers(def, visitor_data_);
    if (!access_token.empty()) req.headers.emplace_back("Authorization", "Bearer " + access_token);
    req.body = body.dump();
    net::Response res = net::fetch(req);
    if (!res.ok()) {
        error = "player: " + (res.error.empty() ? "http " + std::to_string(res.status) : res.error);
        refusal_ = res.status > 0 ? Refusal{"HTTP_" + std::to_string(res.status), ""} : Refusal{"NETWORK", res.error};
        return false;
    }

    record_response(res.body);
    json r = parse_innertube_json(res.body);
    if (r.is_discarded()) {
        error = "player: invalid json";
        return false;
    }
    const json& ps = child_obj(r, "playabilityStatus");
    std::string status = ps.value("status", "");
    if (status != "OK") {
        error = "playability " + status + ": " + ps.value("reason", "");
        refusal_ = {status, ps.value("reason", "")};
        return false;
    }
    const json& details = child_obj(r, "videoDetails");
    out.id = video_id;
    out.title = details.value("title", "");
    out.author = details.value("author", "");
    out.length_s = to_int(details, "lengthSeconds");
    out.is_live = details.value("isLive", false);
    out.description = details.value("shortDescription", "");
    out.channel_id = details.value("channelId", "");
    out.views = details.value("viewCount", "");
    out.has_audio_loudness = false;
    out.audio_loudness_db = 0.0;
    const json& ac = child_obj(child_obj(r, "playerConfig"), "audioConfig");
    if (ac.contains("loudnessDb") && ac["loudnessDb"].is_number()) {
        out.has_audio_loudness = true;
        out.audio_loudness_db = ac["loudnessDb"].get<double>();
    }
    parse_captions(child_obj(r, "captions"), out);
    out.formats.clear();
    out.user_agent = def.user_agent;
    size_t failed = 0;
    size_t total_adaptive = 0;
    std::string first_err;
    const json& sd = child_obj(r, "streamingData");
    for (const json& f : child_arr(sd, "adaptiveFormats")) {
        ++total_adaptive;
        Format fmt;
        if (decipher) {
            std::string err;
            std::string sc = f.value("signatureCipher", f.value("cipher", ""));
            if (!shared_decipher().stream_url(f.value("url", ""), sc, fmt.url, err)) {
                if (failed++ == 0) {
                    first_err = err;
                    OLOG("yt: decipher itag %lld: %s", static_cast<long long>(to_int(f, "itag")), err.c_str());
                }
                continue;
            }
        } else {
            if (!f.contains("url")) continue;  // ciphered formats: only through a deciphering client
            fmt.url = f.value("url", "");
            if (fmt.url.empty()) continue;
        }
        fmt.itag = static_cast<int>(to_int(f, "itag"));
        split_mime(f.value("mimeType", ""), fmt.mime, fmt.codecs);
        fmt.width = static_cast<int>(to_int(f, "width"));
        fmt.height = static_cast<int>(to_int(f, "height"));
        fmt.fps = static_cast<int>(to_int(f, "fps"));
        int64_t avg_br = to_int(f, "averageBitrate");
        fmt.bitrate = avg_br > 0 ? avg_br : to_int(f, "bitrate");
        fmt.content_length = to_int(f, "contentLength");
        fmt.audio_sample_rate = static_cast<int>(to_int(f, "audioSampleRate"));
        int ch = static_cast<int>(to_int(f, "audioChannels"));
        fmt.audio_channels = ch > 0 ? ch : 2;
        if (f.value("isDrc", false)) continue;  // loudness-compressed duplicate of an audio format
        const json& track = child_obj(f, "audioTrack");
        fmt.audio_track_id = track.value("id", "");
        fmt.audio_track_name = track.value("displayName", "");
        fmt.audio_default = track.value("audioIsDefault", false);

        if (fmt.is_video()) {
            fmt.codec = classify_video_codec(fmt.mime, fmt.codecs);

            const json& ci = child_obj(f, "colorInfo");
            std::string tc = ci.value("transferCharacteristics", "");
            if (tc.find("SMPTEST2084") != std::string::npos) {
                fmt.hdr_mode = HdrMode::HDR10;
            } else if (tc.find("ARIB_STD_B67") != std::string::npos) {
                fmt.hdr_mode = HdrMode::HLG;
            }
        }

        out.formats.push_back(std::move(fmt));
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
    if (!ensure_visitor(error)) return false;
    ClientDef def = client_def("search", visitor_data_, web_client_version_);
    json body = {{"context", {{"client", def.client}}}};
    if (continuation.empty()) {
        body["query"] = query;
    } else {
        body["continuation"] = continuation;
    }
    net::Request req;
    req.method = "POST";
    req.url = "https://www.youtube.com/youtubei/v1/search?prettyPrint=false";
    req.headers = client_headers(def, visitor_data_);
    req.body = body.dump();
    net::Response res = net::fetch(req);
    if (!res.ok()) {
        error = "search: " + (res.error.empty() ? "http " + std::to_string(res.status) : res.error);
        return false;
    }
    record_response(res.body);
    json r = parse_innertube_json(res.body);
    if (r.is_discarded()) {
        error = "search: invalid json";
        return false;
    }
    out = Feed{};
    collect(r, out);
    OLOG("yt: search '%s'%s -> %zu items", query.c_str(), continuation.empty() ? "" : " (next page)", out.items.size());
    return true;
}

bool Client::playlist(const std::string& video_id, const std::string& playlist_id, Feed& out, std::string& title,
                      std::string& error) {
    if (!ensure_visitor(error)) return false;
    ClientDef def = client_def("search", visitor_data_, web_client_version_);
    json body = {{"context", {{"client", def.client}}}, {"videoId", video_id}, {"playlistId", playlist_id}};
    net::Request req;
    req.method = "POST";
    req.url = "https://www.youtube.com/youtubei/v1/next?prettyPrint=false";
    req.headers = client_headers(def, visitor_data_);
    req.body = body.dump();
    net::Response res = net::fetch(req);
    if (!res.ok()) {
        error = "playlist: "+ (res.error.empty() ? "http " + std::to_string(res.status) : res.error);
        return false;
    }
    json r = parse_innertube_json(res.body);
    const json* panel = r.is_discarded() ? nullptr : find_key(r, "playlist");
    if (panel && panel->is_object() && panel->contains("playlist")) panel = &(*panel)["playlist"];
    if (!panel || !panel->is_object() || !panel->contains("contents")) {
        error = "playlist: no playlist panel";
        return false;
    }
    const json& t = panel->value("title", json());
    title = t.is_string() ? t.get<std::string>() : text_of(t);
    out = Feed{};
    // The panel is a window of the list around the current video: keep what comes after it.
    std::vector<FeedItem> all;
    size_t after = 0;
    for (const json& c : child_arr(*panel, "contents")) {
        const json& v = child_obj(c, "playlistPanelVideoRenderer");
        FeedItem item;
        item.video_id = v.value("videoId", "");
        if (item.video_id.empty()) continue;  // unavailable entries
        item.title = text_of(child_obj(v, "title"));
        item.channel = text_of(child_obj(v, "shortBylineText"));
        item.duration = text_of(child_obj(v, "lengthText"));
        item.thumbnail = "https://i.ytimg.com/vi/" + item.video_id + "/mqdefault.jpg";
        item.playlist_id = playlist_id;
        if (v.value("selected", false) || item.video_id == video_id) after = all.size() + 1;
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
    if (!ensure_visitor(error)) return false;
    ClientDef def = client_def("search", visitor_data_, web_client_version_);
    json body = {{"context", {{"client", def.client}}}};
    if (continuation.empty()) body["browseId"] = "VL" + playlist_id;
    else body["continuation"] = continuation;
    net::Request req;
    req.method = "POST";
    req.url = "https://www.youtube.com/youtubei/v1/browse?prettyPrint=false";
    req.headers = client_headers(def, visitor_data_);
    req.body = body.dump();
    net::Response res = net::fetch(req);
    if (!res.ok()) {
        error = http_error("playlist", res);
        return false;
    }
    record_response(res.body);
    json r = parse_innertube_json(res.body);
    if (r.is_discarded()) {
        error = "playlist: invalid json";
        return false;
    }
    out = Feed{};
    collect(r, out);
    for (FeedItem& item : out.items) item.playlist_id = playlist_id;
    if (const json* meta = find_key(r, "playlistMetadataRenderer")) title = meta->value("title", "");
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
        json t = tv.ok() ? parse_innertube_json(tv.body) : json();
        if (t.is_object()) {
            out = Feed{};
            const json* sc = nullptr;
            if (continuation.empty()) {
                try {
                    sc = &t.at("contents").at("singleColumnWatchNextResults");
                } catch (const json::exception&) {
                }
            }
            if (sc && sc->contains("results")) {
                collect((*sc)["results"], out);
                if (sc->contains("autoplay")) {
                    if (const json* we = find_key((*sc)["autoplay"], "watchEndpoint")) {
                        std::string next_id = we->value("videoId", "");
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
    if (!ensure_visitor(error)) return false;
    ClientDef def = client_def("search", visitor_data_, web_client_version_);
    json body = {{"context", {{"client", def.client}}}};
    if (continuation.empty()) {
        body["videoId"] = video_id;
    } else {
        body["continuation"] = continuation;
    }
    net::Request req;
    req.method = "POST";
    req.url = "https://www.youtube.com/youtubei/v1/next?prettyPrint=false";
    req.headers = client_headers(def, visitor_data_);
    req.body = body.dump();
    net::Response res = net::fetch(req);
    if (!res.ok()) {
        error = "next: "+ (res.error.empty() ? "http " + std::to_string(res.status) : res.error);
        return false;
    }
    json r = parse_innertube_json(res.body);
    if (r.is_discarded()) {
        error = "next: invalid json";
        return false;
    }
    out = Feed{};
    // Only the secondary results column: the primary one repeats the current video.
    const json* secondary = nullptr;
    try {
        secondary = &r.at("contents").at("twoColumnWatchNextResults").at("secondaryResults");
    } catch (const json::exception&) {
    }
    collect(secondary ? *secondary : r, out);
    out.items.erase(std::remove_if(out.items.begin(), out.items.end(),
                                   [&](const FeedItem& i) { return i.video_id == video_id; }),
                    out.items.end());
    OLOG("yt: related %s%s -> %zu items", video_id.c_str(), continuation.empty() ? "" : " (cont)", out.items.size());
    return true;
}

bool Client::details(const std::string& video_id, const std::string& access_token, VideoDetails& out,
                     std::string& error) {
    if (!ensure_visitor(error)) return false;
    ClientDef def = client_def("search", visitor_data_, web_client_version_);
    json body = {{"context", {{"client", def.client}}}, {"videoId", video_id}};
    net::Request req;
    req.method = "POST";
    req.url = "https://www.youtube.com/youtubei/v1/next?prettyPrint=false";
    req.headers = client_headers(def, visitor_data_);
    req.body = body.dump();

    // Fetch the account's like/subscribe state (TV "next") concurrently with the WEB "next" page.
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
    json r = parse_innertube_json(res.body);
    if (r.is_discarded()) {
        error = "next: invalid json";
        return false;
    }
    out = VideoDetails{};
    if (const json* primary = find_key(r, "videoPrimaryInfoRenderer"))
        out.date = text_of(child_obj(*primary, "dateText"));
    if (const json* owner = find_key(r, "videoOwnerRenderer")) {
        out.channel = text_of(child_obj(*owner, "title"));
        out.channel_avatar = last_thumbnail(child_obj(*owner, "thumbnail"));
        out.subscribers = text_of(child_obj(*owner, "subscriberCountText"));
        // The channel link moved from navigationEndpoint into the title runs: take any browseId under the owner.
        if (const json* browse = find_key(*owner, "browseId"))
            if (browse->is_string()) out.channel_id = browse->get<std::string>();
    }
    if (out.channel_id.empty())
        if (const json* sub = find_key(r, "subscribeButtonRenderer"))
            if (sub->is_object()) out.channel_id = sub->value("channelId", "");
    if (out.channel_avatar.compare(0, 2, "//") == 0) out.channel_avatar = "https:" + out.channel_avatar;
    if (const json* lb = find_key(r, "likeButtonViewModel")) {
        if (const json* def = find_key(*lb, "defaultButtonViewModel")) {
            if (const json* vm = find_key(*def, "buttonViewModel");
                vm && vm->contains("title") && (*vm)["title"].is_string())
                out.like_count = trimmed((*vm)["title"].get<std::string>());
        }
        if (out.like_count.empty()) {
            if (const json* ent = find_key(*lb, "likeCountEntity")) {
                if (const json* txt = find_key(*ent, "likeCountIfIndifferent"))
                    out.like_count = text_of(*txt);
            }
        }
    } else if (const json* seg = find_key(r, "segmentedLikeDislikeButtonRenderer")) {
        if (const json* lb = find_key(*seg, "likeButton"))
            out.like_count = text_of(child_obj(child_obj(*lb, "toggleButtonRenderer"), "defaultText"));
    }
    if (out.like_count.find_first_of("0123456789") == std::string::npos) out.like_count.clear();
    r = json();  // release the WEB DOM before parsing the TV account-state DOM

    // Parse the account's like/subscribe state fetched above.
    if (!access_token.empty()) {
        json t = tv.ok() ? parse_innertube_json(tv.body) : json();
        if (t.is_object()) {
            if (const json* like = find_key(t, "likeStatus"))
                if (like->is_string()) out.like_status = like->get<std::string>();
            if (const json* sub = find_key(t, "subscribeButtonRenderer"))
                if (sub->is_object()) {
                    if (sub->contains("subscribed")) out.subscribed = sub->value("subscribed", false) ? 1 : 0;
                    if (out.channel_id.empty()) out.channel_id = sub->value("channelId", "");
                }
        } else {
            OLOG("yt: details %s: account state %s", video_id.c_str(), http_error("tv next", tv).c_str());
        }
    }
    OLOG("yt: details %s: '%s' %s, %s, like %s, subscribed %d", video_id.c_str(), out.channel.c_str(),
         out.subscribers.c_str(), out.date.c_str(), out.like_status.c_str(), out.subscribed);
    return true;
}

bool Client::channel(const std::string& channel_id, const std::string& continuation, Feed& out, ChannelInfo& header,
                     std::string& error) {
    if (!ensure_visitor(error)) return false;
    ClientDef def = client_def("search", visitor_data_, web_client_version_);
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
    req.headers = client_headers(def, visitor_data_);
    req.body = body.dump();
    net::Response res = net::fetch(req);
    if (!res.ok()) {
        error = http_error("channel", res);
        return false;
    }
    record_response(res.body);
    json r = parse_innertube_json(res.body);
    if (r.is_discarded()) {
        error = "channel: invalid json";
        return false;
    }
    out = Feed{};
    collect(r, out);
    if (const json* meta = find_key(r, "channelMetadataRenderer")) {
        header.title = meta->value("title", "");
        header.avatar = last_thumbnail(child_obj(*meta, "avatar"));
    }
    if (const json* page = find_key(r, "pageHeaderViewModel")) {
        std::vector<std::string> parts;
        texts_under(*page, "metadataParts", parts);
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

std::string string_at(const json& j, std::initializer_list<const char*> path) {
    const json* at = &j;
    for (const char* k : path) {
        if (!at->is_object() || !at->contains(k)) return "";
        at = &(*at)[k];
    }
    return at->is_string() ? at->get<std::string>() : "";
}

// First continuationCommand token under j.
std::string continuation_token(const json& j) {
    if (const json* cmd = find_key(j, "continuationCommand"))
        if (cmd->is_object()) return cmd->value("token", "");
    return "";
}

// New layout: the thread only holds keys, the texts are commentEntityPayload mutations of frameworkUpdates.
Comment comment_from_entity(const json& p) {
    Comment c;
    const json& props = child_obj(p, "properties");
    const json& author = child_obj(p, "author");
    const json& toolbar = child_obj(p, "toolbar");
    c.id = props.value("commentId", "");
    c.text = string_at(props, {"content", "content"});
    c.published = props.value("publishedTime", "");
    c.author = author.value("displayName", "");
    c.avatar = author.value("avatarThumbnailUrl", "");
    c.creator = author.value("isCreator", false);
    if (c.avatar.empty())
        if (const json* src = find_key(child_obj(p, "avatar"), "sources"))
            if (src->is_array() && !src->empty()) c.avatar = src->back().value("url", "");
    c.likes = trimmed(toolbar.value("likeCountNotliked", ""));
    if (c.likes == "0") c.likes.clear();
    c.replies = trimmed(toolbar.value("replyCount", ""));
    if (c.replies == "0") c.replies.clear();
    c.hearted = toolbar.value("heartState", "") == "TOOLBAR_HEART_STATE_HEARTED";
    return c;
}

// Old layout: commentRenderer carries everything.
Comment comment_from_renderer(const json& r) {
    Comment c;
    c.id = r.value("commentId", "");
    c.author = text_of(child_obj(r, "authorText"));
    c.avatar = last_thumbnail(child_obj(r, "authorThumbnail"));
    c.text = text_of(child_obj(r, "contentText"));
    c.published = text_of(child_obj(r, "publishedTimeText"));
    c.likes = trimmed(text_of(child_obj(r, "voteCount")));
    c.replies = r.contains("replyCount") ? std::to_string(r.value("replyCount", 0)) : "";
    if (c.replies == "0") c.replies.clear();
    c.creator = r.value("authorIsChannelOwner", false);
    c.pinned = r.contains("pinnedCommentBadge");
    return c;
}

// Header + threads + next page of a comments continuation answer (reload or append), or of a create_comment one.
void parse_comments(const json& r, CommentPage& out) {
    std::unordered_map<std::string, const json*> entities;
    static const json kNone = json::array();
    const json* mutations = &kNone;
    if (r.contains("frameworkUpdates") && r["frameworkUpdates"].is_object()) {
        const json& fu = r["frameworkUpdates"];
        if (fu.contains("entityBatchUpdate") && fu["entityBatchUpdate"].contains("mutations"))
            mutations = &fu["entityBatchUpdate"]["mutations"];
    }
    // Pointers into r itself (value() would return copies).
    if (mutations->is_array())
        for (const json& m : *mutations) {
            if (!m.is_object() || !m.contains("payload") || !m["payload"].is_object()) continue;
            const json& p = m["payload"];
            if (!p.contains("commentEntityPayload") || !p["commentEntityPayload"].is_object()) continue;
            const json& e = p["commentEntityPayload"];
            entities[m.value("entityKey", "")] = &e;
            if (e.contains("key")) entities[e.value("key", "")] = &e;
        }
    auto from_view_model = [&](const json& vm, Comment& c) {
        const json& model = vm.contains("commentViewModel") ? vm["commentViewModel"] : vm;
        auto e = entities.find(model.value("commentKey", ""));
        if (e == entities.end()) return false;
        c = comment_from_entity(*e->second);
        c.pinned = model.contains("pinnedText");
        return true;
    };
    auto add = [&](Comment c) {
        if (c.avatar.compare(0, 2, "//") == 0) c.avatar = "https:" + c.avatar;
        if (!c.text.empty() || !c.author.empty()) out.items.push_back(std::move(c));
    };
    std::function<void(const json&)> walk = [&](const json& j) {
        if (j.is_array()) {
            for (const json& e : j) walk(e);
            return;
        }
        if (!j.is_object()) return;
        for (auto it = j.begin(); it != j.end(); ++it) {
            const std::string& k = it.key();
            const json& v = it.value();
            if (k == "commentsHeaderRenderer" && v.is_object()) {
                out.count = text_of(child_obj(v, "countText"));
                if (out.count.empty()) out.count = text_of(child_obj(v, "commentsCount"));
                if (const json* items = find_key(v, "subMenuItems"))
                    if (items->is_array()) {
                        if (items->size() > 0) out.sort_top = continuation_token((*items)[0]);
                        if (items->size() > 1) out.sort_newest = continuation_token((*items)[1]);
                    }
                if (const json* params = find_key(v, "createCommentParams"))
                    if (params->is_string()) out.create_params = params->get<std::string>();
            } else if (k == "commentThreadRenderer" && v.is_object()) {
                Comment c;
                if (const json* vm = find_key(v, "commentViewModel")) {
                    if (!from_view_model(*vm, c)) continue;
                } else if (const json* cr = find_key(v, "commentRenderer")) {
                    c = comment_from_renderer(*cr);
                } else {
                    continue;
                }
                if (v.contains("replies")) c.reply_token = continuation_token(v["replies"]);
                add(std::move(c));
            } else if (k == "commentViewModel" && v.is_object()) {
                // A reply: outside any thread.
                Comment c;
                if (from_view_model(v, c)) add(std::move(c));
            } else if (k == "commentRenderer" && v.is_object()) {
                add(comment_from_renderer(v));
            } else if (k == "continuationItemRenderer" && v.is_object()) {
                // The thread's own replies continuation sits inside commentThreadRenderer, skipped above.
                std::string token = continuation_token(v);
                if (!token.empty()) out.continuation = token;
            } else if (k == "frameworkUpdates") {
                continue;
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
    json r = parse_innertube_json(res.body);
    std::unordered_map<std::string, std::string> votes;
    std::function<void(const json&)> walk = [&](const json& j) {
        if (j.is_array()) {
            for (const json& e : j) walk(e);
        } else if (j.is_object()) {
            for (auto it = j.begin(); it != j.end(); ++it) {
                if (it.key() == "commentRenderer" && it.value().is_object()) {
                    const json& c = it.value();
                    votes[c.value("commentId", "")] = c.value("isLiked", false) ? "LIKE" : c.value("voteStatus", "");
                } else {
                    walk(it.value());
                }
            }
        }
    };
    try {
        walk(r);
    } catch (const json::exception& e) {
        OLOG("yt: comment votes: %s", e.what());
        return;
    }
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
    if (!ensure_visitor(error)) return false;
    ClientDef def = client_def("search", visitor_data_, web_client_version_);
    auto next = [&](json body, json& r) {
        body["context"] = {{"client", def.client}};
        net::Request req;
        req.method = "POST";
        req.url = "https://www.youtube.com/youtubei/v1/next?prettyPrint=false";
        req.headers = client_headers(def, visitor_data_);
        req.body = body.dump();
        net::Response res = net::fetch(req);
        if (!res.ok()) {
            error = http_error("comments", res);
            return false;
        }
        record_response(res.body);
        r = parse_innertube_json(res.body);
        if (r.is_discarded()) {
            error = "comments: invalid json";
            return false;
        }
        return true;
    };
    out = CommentPage{};
    std::string token = continuation;
    if (token.empty()) {
        // The watch page only holds the continuation of the comments section.
        json watch;
        if (!next({{"videoId", video_id}}, watch)) return false;
        std::function<void(const json&)> find_section = [&](const json& j) {
            if (!token.empty()) return;
            if (j.is_object()) {
                auto it = j.find("itemSectionRenderer");
                if (it != j.end() && it->is_object() && it->value("sectionIdentifier", "") == "comment-item-section") {
                    token = continuation_token(*it);
                    return;
                }
                for (const json& v : j) find_section(v);
            } else if (j.is_array()) {
                for (const json& v : j) find_section(v);
            }
        };
        find_section(watch);
        if (token.empty()) {
            out.disabled = true;  // turned off, or a live chat instead
            OLOG("yt: comments %s: none (disabled)", video_id.c_str());
            return true;
        }
    }
    // Fetch the account's votes (TV "next") concurrently with the WEB comments page when signed in.
    net::Response votes_res;
    std::thread votes_thread;
    if (!access_token.empty()) {
        votes_thread = std::thread([&] { votes_res = post_as_account("next", {{"continuation", token}}, access_token); });
    }
    json r;
    bool ok = next({{"continuation", token}}, r);
    if (votes_thread.joinable()) votes_thread.join();
    if (!ok) return false;
    try {
        parse_comments(r, out);
    } catch (const json::exception& e) {
        error = std::string("comments: ") + e.what();
        return false;
    }
    r = json();  // release WEB comments DOM before parsing TV votes DOM
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
    json r = parse_innertube_json(res.body);
    if (r.is_discarded()) {
        error = "browse: invalid json";
        return false;
    }
    out = Feed{};
    if (browse_id == "FEwhat_to_watch") {
        // Home: rows by topic (next pages bring more rows).
        collect_shelves(r, out);
    } else {
        // Subscriptions "Tous" tab: a titled "most relevant" shelf, then untitled shelves in date order.
        Feed picked;
        collect(r, out, browse_id == "FEsubscriptions" && continuation.empty() ? &picked : nullptr);
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
    json r = json::parse(res.body, nullptr, false);  // ["query", ["s1", "s2", ...]]
    out.clear();
    if (r.is_array() && r.size() > 1 && r[1].is_array())
        for (const json& s : r[1])
            if (s.is_string()) out.push_back(s.get<std::string>());
    return true;
}

void Client::record_response(const std::string& body) {
    if (!capture_response_) return;
    std::lock_guard<std::mutex> lock(mutex_);
    last_response_ = body;
}

Client& shared_client() {
    static Client client;
    static std::once_flag init_once;
    std::call_once(init_once, [] { client.set_capture_response(false); });
    return client;
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
    out.clear();
    // Zero-DOM SAX parse of json3: events[].{tStartMs, dDurationMs, segs:[{utf8}]}.
    // Speech-recognition tracks split lines into word segments and add "\n"-only events.
    struct CaptionsSax {
        std::vector<CaptionCue>& out;
        int depth = 0;
        bool root_is_object = false;
        bool failed = false;
        bool next_is_events = false;
        bool in_events = false;
        bool in_event = false;
        enum class EvField { None, Start, Dur, Segs } ev_field = EvField::None;
        bool in_segs = false;
        bool in_seg = false;
        bool want_utf8 = false;
        int64_t cur_start = 0;
        int64_t cur_dur = 0;
        bool cur_has_segs = false;
        std::string cur_text;

        void assign_ev_int(int64_t v) {
            if (depth == 3 && in_event) {
                if (ev_field == EvField::Start) cur_start = v;
                else if (ev_field == EvField::Dur) cur_dur = v;
                ev_field = EvField::None;
            }
        }
        bool clear_scalar() {
            if (depth == 1) next_is_events = false;
            else if (depth == 3 && in_event) ev_field = EvField::None;
            else if (depth == 5 && in_seg) want_utf8 = false;
            return true;
        }
        bool null() { return clear_scalar(); }
        bool boolean(bool) { return clear_scalar(); }
        bool number_integer(json::number_integer_t v) {
            assign_ev_int(static_cast<int64_t>(v));
            return clear_scalar();
        }
        bool number_unsigned(json::number_unsigned_t v) {
            assign_ev_int(static_cast<int64_t>(v));
            return clear_scalar();
        }
        bool number_float(json::number_float_t v, const std::string&) {
            assign_ev_int(static_cast<int64_t>(v));
            return clear_scalar();
        }
        bool binary(json::binary_t&) { return clear_scalar(); }
        bool string(std::string& v) {
            if (depth == 5 && in_seg && want_utf8) {
                cur_text += v;
                want_utf8 = false;
            } else if (depth == 3 && in_event && (ev_field == EvField::Start || ev_field == EvField::Dur)) {
                int64_t n = 0;
                auto parsed = std::from_chars(v.data(), v.data() + v.size(), n);
                if (parsed.ec == std::errc{}) assign_ev_int(n);
            }
            return clear_scalar();
        }
        bool start_object(std::size_t) {
            ++depth;
            if (depth == 1) {
                root_is_object = true;
            } else if (depth == 2) {
                next_is_events = false;
            } else if (depth == 3 && in_events) {
                in_event = true;
                ev_field = EvField::None;
                cur_start = 0;
                cur_dur = 0;
                cur_has_segs = false;
                cur_text.clear();
            } else if (depth == 4 && in_event) {
                ev_field = EvField::None;
            } else if (depth == 5 && in_segs) {
                in_seg = true;
                want_utf8 = false;
            }
            return true;
        }
        bool end_object() {
            if (depth == 5 && in_seg) {
                in_seg = false;
                want_utf8 = false;
            } else if (depth == 3 && in_event) {
                if (cur_has_segs) {
                    size_t first = cur_text.find_first_not_of(" \r\n\t");
                    if (first != std::string::npos) {
                        size_t last = cur_text.find_last_not_of(" \r\n\t");
                        CaptionCue cue;
                        cue.text = cur_text.substr(first, last + 1 - first);
                        cue.start_ms = cur_start;
                        cue.end_ms = cur_dur > 0 ? cur_start + cur_dur : 0;
                        out.push_back(std::move(cue));
                    }
                }
                in_event = false;
                ev_field = EvField::None;
            }
            --depth;
            return true;
        }
        bool start_array(std::size_t) {
            ++depth;
            if (depth == 2 && next_is_events) {
                in_events = true;
                next_is_events = false;
            } else if (depth == 2) {
                next_is_events = false;
            } else if (depth == 4 && in_event && ev_field == EvField::Segs) {
                in_segs = true;
                cur_has_segs = true;
                ev_field = EvField::None;
            } else if (depth == 4 && in_event) {
                ev_field = EvField::None;
            }
            return true;
        }
        bool end_array() {
            if (depth == 4 && in_segs) in_segs = false;
            else if (depth == 2 && in_events) in_events = false;
            --depth;
            return true;
        }
        bool key(std::string& k) {
            if (depth == 1) {
                next_is_events = (k == "events");
            } else if (depth == 3 && in_event) {
                ev_field = (k == "tStartMs")      ? EvField::Start
                           : (k == "dDurationMs") ? EvField::Dur
                           : (k == "segs")        ? EvField::Segs
                                                  : EvField::None;
            } else if (depth == 5 && in_seg) {
                want_utf8 = (k == "utf8");
            }
            return true;
        }
        bool parse_error(std::size_t, const std::string&, const json::exception&) {
            failed = true;
            return false;
        }
    };
    CaptionsSax sax{out};
    bool ok = json::sax_parse(res.body, &sax);
    std::string().swap(res.body);
    if (!ok || sax.failed || !sax.root_is_object) {
        out.clear();
        error = "captions: invalid json";
        return false;
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

bool rate_video(const std::string& video_id, const std::string& rating, const std::string& access_token,
                std::string& error) {
    std::string endpoint = rating == "like" ? "like/like" : rating == "dislike" ? "like/dislike" : "like/removelike";
    net::Response res = post_as_account(endpoint, {{"target", {{"videoId", video_id}}}}, access_token);
    if (!res.ok()) {
        error = http_error(endpoint.c_str(), res);
        return false;
    }
    OLOG("yt: %s %s ok", endpoint.c_str(), video_id.c_str());
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
    json r = json::parse(res.body, nullptr, false);
    CommentPage page;
    std::string status;
    if (r.is_object()) {
        try {
            parse_comments(r, page);
            status = child_obj(r, "actionResult").value("status", "");
        } catch (const json::exception& e) {
            OLOG("yt: %s answer: %s", endpoint, e.what());
        }
    }
    if (page.items.empty() && status != "STATUS_SUCCEEDED") {
        error = std::string(endpoint) + ": refused" + (status.empty() ? std::string() : " (" + status + ")");
        OLOG("yt: %s %s", error.c_str(), res.body.substr(0, 600).c_str());
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
    json r = json::parse(res.body, nullptr, false);
    bool done = false;
    try {
        if (r.is_object() && r.contains("actionResults") && r["actionResults"].is_array())
            for (const json& a : r["actionResults"]) done = done || a.value("status", "") == "STATUS_SUCCEEDED";
    } catch (const json::exception& e) {
        OLOG("yt: comment action answer: %s", e.what());
    }
    if (!done) {
        error = "comment action: refused";
        OLOG("yt: %s %s", error.c_str(), res.body.substr(0, 600).c_str());
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
    json r = json::parse(res.body, nullptr, false);
    // accountItem {accountName, accountPhoto, channelHandle, isSelected}: the selected one, else the first.
    std::vector<const json*> items;
    std::function<void(const json&)> walk = [&](const json& j) {
        if (j.is_object()) {
            for (auto it = j.begin(); it != j.end(); ++it) {
                if ((it.key() == "accountItem" || it.key() == "accountItemRenderer") && it.value().is_object())
                    items.push_back(&it.value());
                else
                    walk(it.value());
            }
        } else if (j.is_array()) {
            for (const json& e : j) walk(e);
        }
    };
    if (!r.is_discarded()) walk(r);
    const json* item = nullptr;
    for (const json* i : items)
        if (!item || i->value("isSelected", false)) item = i;
    if (!item) {
        error = "accounts_list: no account";
        return false;
    }
    out.name = text_of(child_obj(*item, "accountName"));
    out.handle = text_of(child_obj(*item, "channelHandle"));
    out.photo = last_thumbnail(child_obj(*item, "accountPhoto"));
    if (out.photo.compare(0, 2, "//") == 0) out.photo = "https:" + out.photo;
    return true;
}

}  // namespace omega::yt
