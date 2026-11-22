#include "omega/core.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#include <nlohmann/json.hpp>

#include "net/http.h"
#include "player/player.h"
#include "util/log.h"
#include "util/remote_config.h"
#include "yt/auth.h"
#include "yt/innertube.h"
#include "yt/js_solver.h"
#include "yt/sponsorblock.h"
#include "yt/watch.h"

namespace {
constexpr const char* kVersion = OMEGA_VERSION;  // from the repo's VERSION file
std::atomic<int> g_default_audio_delay_ms{0};
}

struct omega_player {
    omega::Player impl;
    std::unique_ptr<omega::yt::WatchReporter> history;  // signed-in YouTube playback only
    std::string formats_json = "{\"available\":[]}";    // pre-serialized JSON for omega_player_formats
    explicit omega_player(omega::PlayerConfig cfg) : impl(std::move(cfg)) {}
    ~omega_player() { history.reset(); }  // stop pinging before the player goes away
};

const char* omega_version(void) { return kVersion; }

int omega_tls_info(char* buf, int buf_size) {
    if (!buf || buf_size <= 0) return 0;
    std::string info = omega::net::tls_info();
    size_t n = std::min(info.size(), static_cast<size_t>(buf_size - 1));
    if (n > 0) memcpy(buf, info.data(), n);
    buf[n] = '\0';
    return static_cast<int>(n);
}

void omega_log_init_udp(const char* host, int port) {
    omega::log::init_udp(host ? host : "", port);
    OLOG("omegacore %s: log sink %s:%d", kVersion, host ? host : "-", port);
}

void omega_log_line(const char* text) {
    OLOG("app: %s", text ? text : "");
}

int omega_log_tail(char* buf, int buf_size, int max_lines) {
    if (!buf || buf_size <= 0) return 0;
    std::string joined;
    for (const auto& line : omega::log::tail(max_lines > 0 ? static_cast<size_t>(max_lines) : 0)) {
        joined += line;
        joined += '\n';
    }
    size_t n = std::min(joined.size(), static_cast<size_t>(buf_size - 1));
    memcpy(buf, joined.data(), n);
    buf[n] = '\0';
    return static_cast<int>(n);
}

namespace {

omega_player* start_youtube(void* window, int width, int height, omega::yt::Client& client,
                            const omega::yt::VideoInfo& info, const omega::yt::Format* video,
                            const omega::yt::Format* audio, int64_t start_ms,
                            const std::vector<std::string>& skipped_clients, bool& stream_refused,
                            std::string& start_error) {
    stream_refused = false;
    start_error.clear();
    OLOG("youtube: video itag %d %dx%d@%d %s %lld kb/s, audio itag %d %s %d Hz", video->itag, video->width,
         video->height, video->fps, video->codecs.c_str(), static_cast<long long>(video->bitrate / 1000), audio->itag,
         audio->codecs.c_str(), audio->audio_sample_rate);

    omega::PlayerConfig cfg;
    cfg.window = window;
    cfg.width = width;
    cfg.height = height;
    cfg.video_url = video->url;
    cfg.audio_url = audio->url;
    // Stream requests carry the User-Agent of the client that resolved them (primary or fallback).
    cfg.stream_headers = info.user_agent.empty() ? client.stream_headers()
                                                 : omega::net::Headers{{"User-Agent", info.user_agent}};
    cfg.fps = video->fps > 0 ? video->fps : 30;
    // Provision at least 4K UHD decoder/scaler headroom so 1080p streams allocate the UHD tier.
    cfg.max_width = std::max(video->width, 3840);
    cfg.max_height = std::max(video->height, 2160);
    cfg.audio_bitrate = audio->bitrate > 0 ? audio->bitrate : 128000;
    cfg.has_audio_loudness = info.has_audio_loudness;
    cfg.audio_loudness_db = info.audio_loudness_db;
    cfg.audio_delay_ms = g_default_audio_delay_ms.load();
    cfg.duration_ms = info.length_s * 1000;
    cfg.live = info.is_live;
    cfg.start_ms = info.is_live ? 0 : start_ms;
    cfg.is_hdr = (video->hdr_mode != omega::yt::HdrMode::None);
    cfg.hdr_mode = (video->hdr_mode == omega::yt::HdrMode::HLG) ? 2 : (cfg.is_hdr ? 1 : 0);
    // Expired URLs (403/410): ask InnerTube again and keep the same formats (and audio language).
    std::string id = info.id, audio_track = audio->audio_track_id;
    int video_itag = video->itag, audio_itag = audio->itag;
    cfg.refresh_urls = [id, video_itag, audio_itag, audio_track, skipped_clients](std::string& v, std::string& a) {
        omega::yt::Client fresh_client;
        for (const auto& name : skipped_clients) fresh_client.skip_client(name);
        omega::yt::VideoInfo fresh;
        std::string err;
        if (!fresh_client.player(id, fresh, err)) {
            OLOG("refresh %s: %s", id.c_str(), err.c_str());
            return false;
        }
        for (const auto& f : fresh.formats) {
            if (f.itag == video_itag) v = f.url;
            if (f.itag == audio_itag && f.audio_track_id == audio_track) a = f.url;
        }
        return !v.empty() && !a.empty();
    };
    cfg.app_id = "org.omegatube.modern";
    cfg.app_version = kVersion;
    cfg.app_runtitle = "YouTube";
    auto* p = new omega_player(std::move(cfg));
    if (!p->impl.start()) {
        start_error = p->impl.error();
        OLOG("player start failed: %s", start_error.c_str());
        stream_refused = p->impl.stream_refused();
        delete p;
        return nullptr;
    }
    if (omega::yt::shared_auth().signed_in() && !info.is_live) {
        omega::Player* player = &p->impl;
        p->history = std::make_unique<omega::yt::WatchReporter>(id, info.length_s * 1000, [player] {
            omega::PlayerStats s = player->stats();
            return s.state == 3 ? s.position_ms : int64_t(-1);  // only while PLAYING
        });
    }
    return p;
}

}  // namespace

namespace {

// Why the last omega_player_create_youtube failed (omega_player_last_error).
std::mutex g_play_error_mutex;
nlohmann::json g_play_error = nlohmann::json::object();

void set_play_error(const std::string& kind, const std::string& status, const std::string& reason,
                    const std::string& detail) {
    std::lock_guard<std::mutex> lock(g_play_error_mutex);
    g_play_error = {{"kind", kind},
                    {"status", status},
                    {"reason", reason},
                    {"detail", detail},
                    {"signedIn", omega::yt::shared_auth().signed_in()}};
}

// YouTube's playabilityStatus (or HTTP refusal) -> the app's error kinds.
std::string refusal_kind(const std::string& status) {
    if (status == "LOGIN_REQUIRED") return "login";
    if (status.rfind("AGE_", 0) == 0 || status == "CONTENT_CHECK_REQUIRED") return "age";
    if (status == "LIVE_STREAM_OFFLINE") return "upcoming";
    if (status == "UNPLAYABLE") return "unplayable";
    if (status == "ERROR") return "unavailable";
    if (status == "HTTP_429") return "rate_limited";
    if (status.rfind("HTTP_", 0) == 0) return "http";
    if (status == "NETWORK") return "network";
    return "other";
}

// Audio languages of a multi-track video, in YouTube's order (one entry per track id).
nlohmann::json audio_tracks_json(const omega::yt::VideoInfo& info) {
    nlohmann::json tracks = nlohmann::json::array();
    std::vector<std::string> seen;
    for (const auto& f : info.formats) {
        if (!f.is_audio() || f.audio_track_id.empty() ||
            std::find(seen.begin(), seen.end(), f.audio_track_id) != seen.end())
            continue;
        seen.push_back(f.audio_track_id);
        tracks.push_back({{"id", f.audio_track_id}, {"name", f.audio_track_name}, {"default", f.audio_default}});
    }
    return tracks;
}

struct PreloadedVideo {
    std::string video_id;
    omega::yt::VideoInfo info;
    std::chrono::steady_clock::time_point ts;
    std::string in_flight;  // video id being resolved by omega_yt_preload right now
};
std::mutex g_preload_mutex;
std::condition_variable g_preload_cv;
PreloadedVideo g_preload;
constexpr auto kPreloadTtl = std::chrono::seconds(45);

}  // namespace

void omega_yt_preload(const char* video_id) {
    if (!video_id || !*video_id) return;
    std::string vid(video_id);
    {
        std::lock_guard<std::mutex> lock(g_preload_mutex);
        if (g_preload.in_flight == vid) return;
        if (g_preload.video_id == vid && !g_preload.info.formats.empty() &&
            std::chrono::steady_clock::now() - g_preload.ts < kPreloadTtl) {
            return;
        }
        g_preload.in_flight = vid;
    }
    bool ok = false;
    try {
        omega::yt::Client client;
        omega::yt::VideoInfo info;
        std::string error;
        ok = client.player(vid, info, error) && !info.formats.empty();
        {
            std::lock_guard<std::mutex> lock(g_preload_mutex);
            if (g_preload.in_flight == vid) g_preload.in_flight.clear();
            if (ok) {
                g_preload.video_id = vid;
                g_preload.info = std::move(info);
                g_preload.ts = std::chrono::steady_clock::now();
                OLOG("youtube %s: preloaded (%zu formats)", vid.c_str(), g_preload.info.formats.size());
            }
        }
    } catch (const std::exception& e) {
        OLOG("youtube %s: preload exception: %s", vid.c_str(), e.what());
        std::lock_guard<std::mutex> lock(g_preload_mutex);
        if (g_preload.in_flight == vid) g_preload.in_flight.clear();
    }
    g_preload_cv.notify_all();
}

omega_player* omega_player_create_youtube(void* window, int width, int height, const char* video_id,
                                          int max_height, const char* codecs, int64_t start_ms,
                                          const char* audio_track) try {
    omega::yt::Client client;
    omega::yt::VideoInfo info;
    std::string error;
    std::string vid = video_id ? video_id : "";
    bool used_preload = false;
    if (!vid.empty()) {
        std::unique_lock<std::mutex> lock(g_preload_mutex);
        // A preload of this very video still resolving: wait for it instead of resolving it twice.
        g_preload_cv.wait_for(lock, std::chrono::seconds(4), [&] { return g_preload.in_flight != vid; });
        if (g_preload.video_id == vid && !g_preload.info.formats.empty() &&
            std::chrono::steady_clock::now() - g_preload.ts < kPreloadTtl) {
            info = std::move(g_preload.info);
            used_preload = true;
        }
        // Single use; a stale entry for another video is dropped too.
        g_preload.info = omega::yt::VideoInfo{};
        g_preload.video_id.clear();
    }
    if (used_preload) {
        OLOG("youtube %s: using preloaded VideoInfo (%zu formats)", vid.c_str(), info.formats.size());
    } else if (!client.player(vid, info, error)) {
        OLOG("youtube %s: %s", video_id ? video_id : "-", error.c_str());
        const omega::yt::Client::Refusal& r = client.refusal();
        set_play_error(refusal_kind(r.status), r.status, r.reason, error);
        return nullptr;
    }
    std::vector<std::string> prefs = omega::yt::parse_codec_list(codecs && *codecs ? codecs : "av01,vp9,avc1");
    // Clients whose streams googlevideo refused (403 even with fresh URLs: our IP flagged as a bot).
    std::vector<std::string> skipped_clients;
    for (;;) {
        std::vector<const omega::yt::Format*> videos =
            omega::yt::rank_videos(info, max_height > 0 ? max_height : 1080, prefs);
        std::vector<const omega::yt::Format*> audios = omega::yt::rank_audios(info, audio_track ? audio_track : "");
        if (videos.empty() || audios.empty()) {
            OLOG("youtube %s: no playable video (%s) / audio format", video_id, codecs ? codecs : "-");
            set_play_error("formats", "", "", videos.empty() ? "no video format for " + std::string(codecs ? codecs : "-")
                                                              : "no audio format");
            return nullptr;
        }
        const omega::yt::Format* audio = audios.front();
        const omega::yt::Format* fallback_aac = nullptr;
        for (const omega::yt::Format* a : audios) {
            if (a->codecs.compare(0, 4, "mp4a") == 0) {
                fallback_aac = a;
                break;
            }
        }
        // A codec the TV refuses (ESPlayer rejects its stream info) is dropped and the next best format tried.
        std::vector<std::string> refused;
        bool stream_refused = false;
        std::string last_start_error;
        for (const omega::yt::Format* video : videos) {
            if (std::find(refused.begin(), refused.end(), video->video_codec()) != refused.end()) continue;
            omega_player* p = start_youtube(window, width, height, client, info, video, audio, start_ms,
                                            skipped_clients, stream_refused, last_start_error);
            if (!p && !stream_refused && audio->codecs == "opus" && fallback_aac) {
                // If the TV refused Opus audio rather than the video codec, retry with AAC.
                if (last_start_error.find("audio Opus") != std::string::npos) {
                    audio = fallback_aac;
                    p = start_youtube(window, width, height, client, info, video, audio, start_ms,
                                      skipped_clients, stream_refused, last_start_error);
                }
            }
            if (p) {
                // What the quality panel offers: the format each height cap would pick (codecs the TV refused aside).
                auto entry = [](const omega::yt::Format* f) {
                    return nlohmann::json{
                        {"width", f->width},
                        {"height", f->height},
                        {"fps", f->fps},
                        {"codec", f->video_codec()},
                        {"bitrate", f->bitrate},
                        {"hdr", f->hdr_mode != omega::yt::HdrMode::None},
                        {"hdr_mode", f->hdr_mode == omega::yt::HdrMode::HDR10 ? "HDR10" : (f->hdr_mode == omega::yt::HdrMode::HLG ? "HLG" : "None")}};
                };
                nlohmann::json available = nlohmann::json::array();
                int last_height = -1;
                for (const omega::yt::Format* f : omega::yt::rank_videos(info, INT32_MAX, prefs)) {
                    if (f->height == last_height ||
                        std::find(refused.begin(), refused.end(), f->video_codec()) != refused.end())
                        continue;
                    last_height = f->height;
                    available.push_back(entry(f));
                }
                const auto& vi = p->impl.video_info();
                nlohmann::json current_entry = entry(video);
                if (vi.fps_num > 0 && vi.fps_den > 0) {
                    current_entry["fpsNum"] = vi.fps_num;
                    current_entry["fpsDen"] = vi.fps_den;
                }
                nlohmann::json formats = {{"current", current_entry},  {"available", available}, {"live", info.is_live},
                                          {"client", info.client},
                                          {"container", vi.container == omega::media::Container::WebM ? "webm" : "mp4"},
                                          {"maxWidth", p->impl.tier_width()},
                                          {"maxHeight", p->impl.tier_height()},
                                          {"progressive", p->impl.progressive_scan()},
                                          {"espKnobs", omega::esp::extra_knobs_summary()},
                                          {"colorPrimaries", vi.color_primaries},
                                          {"colorTransfer", vi.transfer_characteristics},
                                          {"colorBits", vi.bits_per_channel},
                                          {"colorRange", vi.color_range},
                                          {"containerHdr", vi.has_hdr},
                                          {"title", info.title}, {"author", info.author}, {"description", info.description},
                                          {"views", info.views}, {"channelId", info.channel_id}, {"audioTrack", audio->audio_track_id},
                                          {"audioCodec", audio ? audio->codecs : ""},
                                          {"audioBitrate", audio ? audio->bitrate : 0}};
                formats["audioTracks"] = audio_tracks_json(info);
                nlohmann::json captions = nlohmann::json::array(), translations = nlohmann::json::array();
                for (const auto& c : info.captions)
                    captions.push_back({{"url", c.url}, {"lang", c.lang}, {"name", c.name}, {"auto", c.automatic},
                                        {"translatable", c.translatable}});
                for (const auto& t : info.translations) translations.push_back({{"code", t.first}, {"name", t.second}});
                formats["captions"] = std::move(captions);
                formats["translations"] = std::move(translations);
                p->formats_json = formats.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
                return p;
            }
            if (stream_refused) break;  // not the codec: the other formats of this client would be refused too
            refused.push_back(video->video_codec());
        }
        if (!stream_refused) {
            set_play_error("player", "", "", last_start_error);
            return nullptr;
        }
        OLOG("youtube %s: streams refused for %s, trying another player client", video_id, info.client.c_str());
        skipped_clients.push_back(info.client);
        client.skip_client(info.client);
        if (!client.player(video_id, info, error)) {
            OLOG("youtube %s: %s", video_id, error.c_str());
            std::string refused_list;
            for (size_t i = 0; i < skipped_clients.size(); ++i) {
                if (i > 0) refused_list += ", ";
                refused_list += skipped_clients[i];
            }
            std::string detail = "Streams refused (403/410) for: " + refused_list;
            if (!error.empty()) detail += " | Fallback: " + error;
            // Shown like a bot check: same cause (flagged IP), same advice.
            set_play_error("login", "STREAM_REFUSED", "", detail);
            return nullptr;
        }
    }
} catch (const std::exception& e) {
    OLOG("youtube %s: exception: %s", video_id ? video_id : "-", e.what());
    set_play_error("player", "", "", e.what());
    return nullptr;
}

void omega_player_toggle_pause(omega_player* p) {
    if (p) p->impl.toggle_pause();
}

int omega_player_seek(omega_player* p, int64_t position_ms) { return p && p->impl.seek(position_ms) ? 0 : -1; }

int omega_player_set_rate(omega_player* p, double rate) { return p && p->impl.set_rate(rate) ? 0 : -1; }

void omega_set_default_audio_delay(int delay_ms) { g_default_audio_delay_ms = delay_ms; }

void omega_player_set_audio_delay(omega_player* p, int delay_ms) {
    if (p) p->impl.set_audio_delay_ms(delay_ms);
}

int omega_bt_audio_delay_ms(void) { return omega::esp::query_bt_audio_delay_ms(); }

void omega_player_get_stats(omega_player* p, omega_player_stats* out) {
    if (!p || !out) return;
    omega::PlayerStats s = p->impl.stats();
    out->state = s.state;
    out->eos = s.eos;
    out->position_ms = s.position_ms;
    out->video_queued_ms = s.video_queued_ms;
    out->audio_queued_ms = s.audio_queued_ms;
    out->video_submitted_ms = s.video_submitted_ms;
    out->audio_submitted_ms = s.audio_submitted_ms;
    out->longest_stall_ms = s.longest_stall_ms;
    out->video_underruns = s.video_underruns;
    out->audio_underruns = s.audio_underruns;
    out->stalls = s.stalls;
    out->errors = s.errors;
    out->duration_ms = s.duration_ms;
    out->reconnecting = s.reconnecting;
    out->dropped_frames = s.dropped_frames;
    out->scaler_id = s.scaler_id;
    out->scaler_rc = s.scaler_rc;
    out->dsp_bits = s.dsp_bits;
    out->dsp_hf = s.dsp_hf;
    out->video_bitrate_kbps = s.video_bitrate_kbps;
}

void omega_player_destroy(omega_player* p) { delete p; }

namespace {
char* to_c_json(const nlohmann::json& j);
char* to_c_string(const std::string& s) {
    char* out = static_cast<char*>(malloc(s.size() + 1));
    memcpy(out, s.c_str(), s.size() + 1);
    return out;
}
}

char* omega_player_last_error(void) {
    std::lock_guard<std::mutex> lock(g_play_error_mutex);
    return to_c_json(g_play_error);
}

char* omega_player_formats(omega_player* p) {
    static const std::string kEmpty = "{\"available\":[]}";
    return to_c_string(p ? p->formats_json : kEmpty);
}

namespace {

char* to_c_json(const nlohmann::json& j) {
    std::string s = j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    char* out = static_cast<char*>(malloc(s.size() + 1));
    memcpy(out, s.c_str(), s.size() + 1);
    return out;
}

template <typename Fn>
char* c_api_json_guard(const char* name, Fn fn) {
    try {
        return fn();
    } catch (const std::exception& e) {
        OLOG("%s: exception: %s", name, e.what());
        return to_c_json({{"error", e.what()}});
    } catch (...) {
        OLOG("%s: unknown exception", name);
        return to_c_json({{"error", "internal error"}});
    }
}

// Single-allocation little-endian binary writer backed directly by malloc/realloc so
// release() hands ownership to the caller (released via omega_free) with zero extra copies.
class BinWriter {
public:
    BinWriter() = default;
    ~BinWriter() { std::free(buf_); }
    BinWriter(const BinWriter&) = delete;
    BinWriter& operator=(const BinWriter&) = delete;

    void reserve(size_t extra) {
        if (extra > std::numeric_limits<size_t>::max() - len_) throw std::bad_alloc();
        size_t need = len_ + extra;
        if (need <= cap_) return;
        size_t next = cap_ ? cap_ : 256;
        while (next < need) {
            if (next > std::numeric_limits<size_t>::max() / 2) {
                next = need;
                break;
            }
            next *= 2;
        }
        auto* p = static_cast<uint8_t*>(std::realloc(buf_, next));
        if (!p) throw std::bad_alloc();
        buf_ = p;
        cap_ = next;
    }

    void u8(uint8_t v) {
        reserve(1);
        buf_[len_++] = v;
    }

    void u32(uint32_t v) {
        reserve(4);
        buf_[len_++] = static_cast<uint8_t>(v & 0xFFu);
        buf_[len_++] = static_cast<uint8_t>((v >> 8) & 0xFFu);
        buf_[len_++] = static_cast<uint8_t>((v >> 16) & 0xFFu);
        buf_[len_++] = static_cast<uint8_t>((v >> 24) & 0xFFu);
    }

    void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }

    void i64(int64_t v) {
        uint64_t u = static_cast<uint64_t>(v);
        reserve(8);
        for (int i = 0; i < 8; ++i) buf_[len_++] = static_cast<uint8_t>((u >> (i * 8)) & 0xFFu);
    }

    void str(std::string_view s) {
        uint32_t n = static_cast<uint32_t>(s.size());
        reserve(4 + static_cast<size_t>(n));
        buf_[len_++] = static_cast<uint8_t>(n & 0xFFu);
        buf_[len_++] = static_cast<uint8_t>((n >> 8) & 0xFFu);
        buf_[len_++] = static_cast<uint8_t>((n >> 16) & 0xFFu);
        buf_[len_++] = static_cast<uint8_t>((n >> 24) & 0xFFu);
        if (n > 0) {
            std::memcpy(buf_ + len_, s.data(), n);
            len_ += n;
        }
    }

    uint8_t* release(uint32_t* out_len) {
        if (out_len) *out_len = static_cast<uint32_t>(len_);
        uint8_t* out = buf_;
        buf_ = nullptr;
        len_ = cap_ = 0;
        return out;
    }

private:
    uint8_t* buf_ = nullptr;
    size_t len_ = 0;
    size_t cap_ = 0;
};

uint8_t* bin_error(std::string_view msg, uint32_t* out_len) noexcept {
    try {
        BinWriter w;
        w.reserve(5 + msg.size());
        w.u8(0);
        w.str(msg);
        return w.release(out_len);
    } catch (...) {
        if (out_len) *out_len = 0;
        return nullptr;
    }
}

template <typename Fn>
uint8_t* c_api_bin_guard(const char* name, uint32_t* out_len, Fn fn) noexcept {
    if (out_len) *out_len = 0;
    try {
        return fn();
    } catch (const std::exception& e) {
        OLOG("%s: exception: %s", name, e.what());
        return bin_error(e.what(), out_len);
    } catch (...) {
        OLOG("%s: unknown exception", name);
        return bin_error("internal error", out_len);
    }
}

void write_feed_items(BinWriter& w, const std::vector<omega::yt::FeedItem>& list) {
    w.u32(static_cast<uint32_t>(list.size()));
    for (const auto& it : list) {
        w.str(it.video_id);
        w.str(it.title);
        w.str(it.channel);
        w.str(it.duration);
        w.str(it.views);
        w.str(it.published);
        w.str(it.thumbnail);
        w.str(it.playlist_id);
        w.str(it.kind);
        w.u8(it.is_live ? 1 : 0);
        w.i32(it.watched_percent);
    }
}

uint8_t* feed_to_bin(const omega::yt::Feed& feed, std::string_view title, std::string_view avatar,
                     std::string_view subscribers, uint32_t* out_len) {
    size_t total_items = feed.items.size() + feed.highlights.size();
    for (const auto& s : feed.shelves) total_items += s.items.size();
    BinWriter w;
    w.reserve(256 + total_items * 240 + feed.shelves.size() * 64);
    w.u8(1);
    w.str(feed.continuation);
    w.str(title);
    w.str(avatar);
    w.str(subscribers);
    write_feed_items(w, feed.items);
    write_feed_items(w, feed.highlights);
    w.u32(static_cast<uint32_t>(feed.shelves.size()));
    for (const auto& s : feed.shelves) {
        w.str(s.title);
        w.str(s.continuation);
        write_feed_items(w, s.items);
    }
    return w.release(out_len);
}

}  // namespace

uint8_t* omega_yt_search(const char* query, const char* continuation, uint32_t* out_len) {
    return c_api_bin_guard("omega_yt_search", out_len, [&] {
        omega::yt::Feed feed;
        std::string error;
        if (!omega::yt::shared_client().search(query ? query : "", continuation ? continuation : "", feed, error))
            return bin_error(error, out_len);
        return feed_to_bin(feed, "", "", "", out_len);
    });
}

namespace {

std::mutex g_device_code_mutex;
omega::yt::DeviceCode g_device_code;  // last code from omega_auth_begin (poll only receives its id)

}  // namespace

uint8_t* omega_yt_browse(const char* browse_id, const char* continuation, uint32_t* out_len) {
    return c_api_bin_guard("omega_yt_browse", out_len, [&] {
        std::string error;
        std::string token = omega::yt::shared_auth().access_token(error);
        if (token.empty()) return bin_error(error.empty() ? "signed out" : error, out_len);
        omega::yt::Feed feed;
        if (!omega::yt::shared_client().browse(browse_id ? browse_id : "", continuation ? continuation : "", token, feed,
                                               error))
            return bin_error(error, out_len);
        return feed_to_bin(feed, "", "", "", out_len);
    });
}

uint8_t* omega_yt_playlist(const char* video_id, const char* playlist_id, uint32_t* out_len) {
    return c_api_bin_guard("omega_yt_playlist", out_len, [&] {
        omega::yt::Feed feed;
        std::string title, error;
        if (!omega::yt::shared_client().playlist(video_id ? video_id : "", playlist_id ? playlist_id : "", feed, title,
                                                 error))
            return bin_error(error, out_len);
        return feed_to_bin(feed, title, "", "", out_len);
    });
}

uint8_t* omega_yt_playlist_page(const char* playlist_id, const char* continuation, uint32_t* out_len) {
    return c_api_bin_guard("omega_yt_playlist_page", out_len, [&] {
        omega::yt::Feed feed;
        std::string title, error;
        if (!omega::yt::shared_client().playlist_page(playlist_id ? playlist_id : "", continuation ? continuation : "",
                                                      feed, title, error))
            return bin_error(error, out_len);
        return feed_to_bin(feed, title, "", "", out_len);
    });
}

uint8_t* omega_yt_related(const char* video_id, const char* continuation, uint32_t* out_len) {
    return c_api_bin_guard("omega_yt_related", out_len, [&] {
        omega::yt::Feed feed;
        std::string error;
        std::string token = omega::yt::shared_auth().signed_in() ? omega::yt::shared_auth().access_token(error) : "";
        error.clear();
        if (!omega::yt::shared_client().related(video_id ? video_id : "", continuation ? continuation : "", token, feed, error))
            return bin_error(error, out_len);
        return feed_to_bin(feed, "", "", "", out_len);
    });
}

char* omega_yt_details(const char* video_id) {
    return c_api_json_guard("omega_yt_details", [&] {
        std::string id = video_id ? video_id : "", error;
        std::string token = omega::yt::shared_auth().signed_in() ? omega::yt::shared_auth().access_token(error) : "";
        omega::yt::VideoDetails d;
        if (!omega::yt::shared_client().details(id, token, d, error)) return to_c_json({{"error", error}});
        return to_c_json({{"channelId", d.channel_id},
                          {"channel", d.channel},
                          {"channelAvatar", d.channel_avatar},
                          {"subscribers", d.subscribers},
                          {"date", d.date},
                          {"likeStatus", d.like_status},
                          {"likeCount", d.like_count},
                          {"subscribed", d.subscribed}});
    });
}

namespace {

// Runs an account action with a fresh access token: {"ok":true} or {"error"}.
template <typename Fn>
char* account_action(const char* name, Fn action) {
    return c_api_json_guard(name, [&] {
        std::string error;
        std::string token = omega::yt::shared_auth().access_token(error);
        if (token.empty()) return to_c_json({{"error", error.empty() ? "signed out" : error}});
        if (!action(token, error)) return to_c_json({{"error", error}});
        return to_c_json({{"ok", true}});
    });
}

}  // namespace

char* omega_yt_rate(const char* video_id, const char* rating) {
    std::string id = video_id ? video_id : "", r = rating ? rating : "none";
    return account_action("omega_yt_rate", [&](const std::string& token, std::string& error) {
        return omega::yt::rate_video(id, r, token, error);
    });
}

char* omega_yt_subscribe(const char* channel_id, int subscribe) {
    std::string id = channel_id ? channel_id : "";
    return account_action("omega_yt_subscribe", [&](const std::string& token, std::string& error) {
        return omega::yt::set_subscribed(id, subscribe != 0, token, error);
    });
}

uint8_t* omega_yt_channel(const char* channel_id, const char* continuation, uint32_t* out_len) {
    return c_api_bin_guard("omega_yt_channel", out_len, [&] {
        omega::yt::Feed feed;
        omega::yt::ChannelInfo header;
        std::string error;
        if (!omega::yt::shared_client().channel(channel_id ? channel_id : "", continuation ? continuation : "", feed, header,
                                                error))
            return bin_error(error, out_len);
        return feed_to_bin(feed, header.title, header.avatar, header.subscribers, out_len);
    });
}

namespace {

nlohmann::json comment_json(const omega::yt::Comment& c) {
    return {{"id", c.id},           {"author", c.author}, {"avatar", c.avatar},   {"text", c.text},
            {"published", c.published}, {"likes", c.likes}, {"replies", c.replies}, {"pinned", c.pinned},
            {"creator", c.creator}, {"replyToken", c.reply_token},
            {"liked", c.liked}, {"disliked", c.disliked}};
}

}  // namespace

uint8_t* omega_yt_comments(const char* video_id, const char* continuation, uint32_t* out_len) {
    return c_api_bin_guard("omega_yt_comments", out_len, [&] {
        omega::yt::CommentPage page;
        std::string error;
        std::string token = omega::yt::shared_auth().signed_in() ? omega::yt::shared_auth().access_token(error) : "";
        error.clear();  // no token: the comments still load, without the account's votes
        if (!omega::yt::shared_client().comments(video_id ? video_id : "", continuation ? continuation : "", token, page, error))
            return bin_error(error, out_len);
        BinWriter w;
        w.reserve(256 + page.items.size() * 256);
        w.u8(1);
        w.str(page.continuation);
        w.str(page.count);
        w.str(page.sort_top);
        w.str(page.sort_newest);
        w.str(page.create_params);
        w.u8(page.disabled ? 1 : 0);
        w.u32(static_cast<uint32_t>(page.items.size()));
        for (const auto& c : page.items) {
            w.str(c.id);
            w.str(c.author);
            w.str(c.avatar);
            w.str(c.text);
            w.str(c.published);
            w.str(c.likes);
            w.str(c.replies);
            w.str(c.reply_token);
            uint8_t flags = static_cast<uint8_t>((c.pinned ? 1u : 0u) | (c.creator ? 2u : 0u) |
                                                 (c.liked ? 4u : 0u) | (c.disliked ? 8u : 0u));
            w.u8(flags);
        }
        return w.release(out_len);
    });
}

char* omega_yt_post_comment(const char* create_params, const char* text) {
    return c_api_json_guard("omega_yt_post_comment", [&] {
        std::string error;
        std::string token = omega::yt::shared_auth().access_token(error);
        if (token.empty()) return to_c_json({{"error", error.empty() ? "signed out" : error}});
        omega::yt::Comment c;
        if (!omega::yt::post_comment(create_params ? create_params : "", text ? text : "", token, c, error))
            return to_c_json({{"error", error}});
        nlohmann::json comment = c.id.empty() && c.text.empty() ? nlohmann::json() : comment_json(c);
        return to_c_json({{"ok", true}, {"comment", comment}});
    });
}

char* omega_yt_post_reply(const char* video_id, const char* comment_id, const char* text) {
    return c_api_json_guard("omega_yt_post_reply", [&] {
        std::string error;
        std::string token = omega::yt::shared_auth().access_token(error);
        if (token.empty()) return to_c_json({{"error", error.empty() ? "signed out" : error}});
        omega::yt::Comment c;
        if (!omega::yt::post_reply(video_id ? video_id : "", comment_id ? comment_id : "", text ? text : "", token, c, error))
            return to_c_json({{"error", error}});
        nlohmann::json comment = c.id.empty() && c.text.empty() ? nlohmann::json() : comment_json(c);
        return to_c_json({{"ok", true}, {"comment", comment}});
    });
}

char* omega_yt_comment_action(const char* video_id, const char* comment_id, const char* action) {
    return c_api_json_guard("omega_yt_comment_action", [&] {
        std::string error;
        std::string token = omega::yt::shared_auth().access_token(error);
        if (token.empty()) return to_c_json({{"error", error.empty() ? "signed out" : error}});
        if (!omega::yt::comment_action(video_id ? video_id : "", comment_id ? comment_id : "", action ? action : "", token, error))
            return to_c_json({{"error", error}});
        return to_c_json({{"ok", true}});
    });
}

uint8_t* omega_yt_captions(const char* url, const char* translate_to, uint32_t* out_len) {
    return c_api_bin_guard("omega_yt_captions", out_len, [&] {
        std::vector<omega::yt::CaptionCue> cues;
        std::string error;
        if (!omega::yt::fetch_captions(url ? url : "", translate_to ? translate_to : "", cues, error))
            return bin_error(error, out_len);
        BinWriter w;
        w.reserve(16 + cues.size() * 48);
        w.u8(1);
        w.u32(static_cast<uint32_t>(cues.size()));
        for (const auto& c : cues) {
            w.i64(c.start_ms);
            w.i64(c.end_ms);
            w.str(c.text);
        }
        return w.release(out_len);
    });
}

char* omega_sb_segments(const char* video_id) {
    return c_api_json_guard("omega_sb_segments", [&] {
        std::vector<omega::yt::SkipSegment> segments;
        std::string error;
        if (!omega::yt::sponsor_segments(video_id ? video_id : "", segments, error)) return to_c_json({{"error", error}});
        nlohmann::json list = nlohmann::json::array();
        for (const auto& s : segments) list.push_back({{"category", s.category}, {"start", s.start_s}, {"end", s.end_s}});
        return to_c_json({{"segments", list}});
    });
}

void omega_init(const char* data_dir) {
    std::string dir = data_dir ? data_dir : "";
    if (!dir.empty() && dir.back() != '/') dir += '/';
    if (!dir.empty()) omega::log::init_file(dir + "omega.log");
    OLOG("omegacore %s", kVersion);
    omega::cfg::init(dir + "remote-config.json");
    omega::yt::shared_auth().init(dir + "accounts.json");  // migrates a legacy auth.json
    if (!dir.empty()) omega::yt::shared_decipher().set_cache_dir(dir.substr(0, dir.size() - 1));
    std::thread([] { omega::net::warmup(); }).detach();
}

char* omega_config_refresh(void) {
    return c_api_json_guard("omega_config_refresh", [] {
        std::string error;
        if (!omega::cfg::refresh(error)) return to_c_json({{"error", error}});
        return to_c_json({{"ok", true}});
    });
}

void omega_set_locale(const char* language, const char* region) {
    omega::cfg::set_locale(language ? language : "", region ? region : "");
}

namespace {
std::string form_escape(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            char hex[4];
            snprintf(hex, sizeof(hex), "%%%02X", c);
            out += hex;
        }
    }
    return out;
}
}  // namespace

int omega_report_available(void) { return omega::cfg::str({"reports", "paste"}).empty() ? 0 : 1; }

// reports.paste: the service's URL (OmegaTube's own Worker, github.com/IIyn/omegatube-logs). reports.field
// empty: the text is the raw body; otherwise a form with the text in that field plus reports.params.
// Either way the service must answer with the paste's https URL.
char* omega_report_upload(const char* text) {
    return c_api_json_guard("omega_report_upload", [&] {
        omega::net::Request req;
        req.method = "POST";
        req.url = omega::cfg::str({"reports", "paste"});
        if (req.url.empty()) return to_c_json({{"error", "no log service configured"}});
        std::string field = omega::cfg::str({"reports", "field"});
        if (field.empty()) {
            req.headers = {{"User-Agent", "omegatube-modern"}, {"Content-Type", "text/plain; charset=utf-8"}};
            req.body = text ? text : "";
        } else {
            req.headers = {{"User-Agent", "omegatube-modern"}, {"Content-Type", "application/x-www-form-urlencoded"}};
            req.body = field + "=" + form_escape(text ? text : "");
            std::string params = omega::cfg::str({"reports", "params"});
            if (!params.empty()) req.body += "&" + params;
        }
        omega::net::Response res = omega::net::fetch(req);
        if (!res.ok()) {
            OLOG("report upload: HTTP %d %s", res.status, res.error.c_str());
            return to_c_json({{"error", res.error.empty() ? "HTTP " + std::to_string(res.status) : res.error}});
        }
        std::string url = res.body;
        while (!url.empty() && isspace(static_cast<unsigned char>(url.back()))) url.pop_back();
        if (url.rfind("https://", 0) != 0) return to_c_json({{"error", "unexpected answer"}});
        // 206: some services (paste.rs) keep only the beginning of a text over their size limit.
        return to_c_json({{"url", url}, {"partial", res.status == 206}});
    });
}

char* omega_auth_status(void) {
    return c_api_json_guard("omega_auth_status", [] {
        return to_c_json({{"signedIn", omega::yt::shared_auth().signed_in()}});
    });
}

char* omega_auth_begin(void) {
    return c_api_json_guard("omega_auth_begin", [] {
        omega::yt::DeviceCode code;
        std::string error;
        if (!omega::yt::shared_auth().begin(code, error)) return to_c_json({{"error", error}});
        {
            std::lock_guard<std::mutex> lock(g_device_code_mutex);
            g_device_code = code;
        }
        return to_c_json({{"deviceCode", code.device_code},
                          {"userCode", code.user_code},
                          {"verificationUrl", code.verification_url},
                          {"qrUrl", omega::yt::prefilled_verification_url(code.verification_url, code.user_code)},
                          {"interval", code.interval_s},
                          {"expiresIn", code.expires_in_s}});
    });
}

char* omega_auth_poll(const char* device_code) {
    return c_api_json_guard("omega_auth_poll", [&] {
        omega::yt::DeviceCode code;
        {
            std::lock_guard<std::mutex> lock(g_device_code_mutex);
            code = g_device_code;
        }
        if (device_code) code.device_code = device_code;
        std::string error;
        static const char* names[] = {"pending", "slow_down", "done", "denied", "expired", "failed"};
        omega::yt::PollResult r = omega::yt::shared_auth().poll(code, error);
        if (r == omega::yt::PollResult::Done) {
            // Cache the new account's profile so the picker can show its name/avatar without another round-trip.
            std::string e;
            std::string token = omega::yt::shared_auth().access_token(e);
            omega::yt::AccountInfo info;
            if (!token.empty() && omega::yt::account_info(token, info, e))
                omega::yt::shared_auth().cache_profile(omega::yt::shared_auth().active_id(), info.name, info.handle, info.photo);
        }
        return to_c_json({{"status", names[static_cast<int>(r)]}, {"error", error}});
    });
}

void omega_auth_sign_out(void) { omega::yt::shared_auth().sign_out(); }

char* omega_auth_account(void) {
    return c_api_json_guard("omega_auth_account", [] {
        std::string error;
        std::string token = omega::yt::shared_auth().access_token(error);
        if (token.empty()) return to_c_json({{"error", error.empty() ? "signed out" : error}});
        omega::yt::AccountInfo account;
        if (!omega::yt::account_info(token, account, error)) return to_c_json({{"error", error}});
        omega::yt::shared_auth().cache_profile(omega::yt::shared_auth().active_id(), account.name, account.handle, account.photo);
        return to_c_json({{"name", account.name}, {"handle", account.handle}, {"photo", account.photo}});
    });
}

// All signed-in accounts for the picker: {"accounts":[{"id","name","handle","photo","active"}]}.
char* omega_auth_accounts(void) {
    return c_api_json_guard("omega_auth_accounts", [] {
        nlohmann::json arr = nlohmann::json::array();
        std::string active = omega::yt::shared_auth().active_id();
        for (const auto& a : omega::yt::shared_auth().accounts())
            arr.push_back({{"id", a.id}, {"name", a.name}, {"handle", a.handle}, {"photo", a.photo}, {"active", a.id == active}});
        return to_c_json({{"accounts", arr}});
    });
}

// Makes the account with this id active. {"ok":true} or {"error"}.
char* omega_auth_switch(const char* id) {
    return c_api_json_guard("omega_auth_switch", [&] {
        if (!omega::yt::shared_auth().switch_to(id ? id : "")) return to_c_json({{"error", "no such account"}});
        return to_c_json({{"ok", true}});
    });
}

char* omega_yt_suggest(const char* query) {
    return c_api_json_guard("omega_yt_suggest", [&] {
        std::vector<std::string> list;
        std::string error;
        if (!omega::yt::suggest(query ? query : "", list, error)) return to_c_json({{"error", error}});
        return to_c_json({{"suggestions", list}});
    });
}

void omega_free(void* p) { free(p); }
