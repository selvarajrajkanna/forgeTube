#include "player/player.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "net/http.h"
#include "util/log.h"

namespace omega {
namespace {

using Clock = std::chrono::steady_clock;

const char* stream_name(esp::StreamType t) { return t == esp::STREAM_VIDEO ? "video" : "audio"; }

int64_t ms_since(Clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count();
}

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

bool video_mime(media::Codec c, esp::VideoMime& out) {
    switch (c) {
        case media::Codec::H264: out = esp::VIDEO_H264; return true;
        case media::Codec::VP9: out = esp::VIDEO_VP9; return true;
        case media::Codec::AV1: out = esp::VIDEO_AV1; return true;
        default: return false;
    }
}

bool audio_mime(media::Codec c, esp::AudioMime& out) {
    switch (c) {
        case media::Codec::AAC: out = esp::AUDIO_AAC; return true;
        case media::Codec::Opus: out = esp::AUDIO_OPUS; return true;
        default: return false;
    }
}

esp::AudioMime pcm_mime(int bits) {
    switch (bits) {
        case 32: return esp::AUDIO_PCM_S32LE;
        case 24: return esp::AUDIO_PCM_S24LE;
        default: return esp::AUDIO_PCM_S16LE;
    }
}

// -1: probe 32->24->16->0 (Off), >0: verified TV PCM bit depth, 0: TV explicitly rejected all PCM modes (HW bypass).
std::atomic<int> g_cached_pcm_bits{-1};

}  // namespace

Player::Player(PlayerConfig cfg) : cfg_(std::move(cfg)) {
    video_.type = esp::STREAM_VIDEO;
    video_.url = cfg_.video_url;
    audio_.type = esp::STREAM_AUDIO;
    audio_.url = cfg_.audio_url;
    audio_delay_ms_ = cfg_.audio_delay_ms;
}

void Player::teardown_esp() {
    if (handle_) {
        api_->stop(handle_);
        api_->close(handle_);
        api_->destroy(handle_);
        handle_ = nullptr;
        OLOG("player: destroyed");
    }
    if (sound_stream_info_ && api_ && api_->destroy_sound_stream_info) {
        api_->destroy_sound_stream_info(sound_stream_info_);
        sound_stream_info_ = nullptr;
    }
}

Player::~Player() {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        running_ = false;
    }
    state_cv_.notify_all();
    for (Track* t : {&video_, &audio_}) notify_track(*t);
    if (control_thread_.joinable()) control_thread_.join();
    for (Track* t : {&video_, &audio_}) {
        if (t->feed_thread.joinable()) t->feed_thread.join();
        if (t->demux_thread.joinable()) t->demux_thread.join();
    }
    teardown_esp();
}

bool Player::start() {
    api_ = esp::api();
    if (!api_) {
        error_ = "libesplusplayer.so unavailable";
        return false;
    }

    // Open the HTTP streams and parse their init segments first: ESPlayer needs codec info before prepare.
    struct Source {
        Track* track;
        std::unique_ptr<net::StreamSource> http;
        std::unique_ptr<media::Demuxer> reader;
    };
    std::vector<Source> sources;
    if (cfg_.use_video) sources.push_back({&video_, nullptr, nullptr});
    if (cfg_.use_audio) sources.push_back({&audio_, nullptr, nullptr});
    // Live: both tracks start at the same segment number, a few segments behind the newest one.
    int64_t live_seq = -1;
    if (cfg_.live) {
        std::string err;
        int64_t head = net::LiveReader::head_sequence(cfg_.use_video ? video_.url : audio_.url, cfg_.stream_headers, err);
        if (head < 0) {
            error_ = err;
            return false;
        }
        live_seq = std::max<int64_t>(0, head - cfg_.live_behind_segments);
        OLOG("live: head segment %lld, starting at %lld", static_cast<long long>(head), static_cast<long long>(live_seq));
    }
    std::deque<media::Sample> first_samples[2];  // live: first fragment of each source, read up front
    struct OpenResult {
        bool ok = false;
        bool refused = false;
        std::string error;
    };
    OpenResult results[2];
    auto open_source = [&](size_t idx) {
        Source& src = sources[idx];
        Track& t = *src.track;
        std::string init_url;
        {
            std::lock_guard<std::mutex> lock(t.mutex);
            init_url = t.url;
        }
        if (cfg_.live) src.http = std::make_unique<net::LiveReader>(init_url, cfg_.stream_headers, live_seq);
        else src.http = std::make_unique<net::RangeReader>(init_url, cfg_.stream_headers);
        if (!cfg_.live) {
            // Opening: fresh URLs on 403/410, but give up soon if the stream stays refused (the caller then
            // tries another player client); start_track() puts the playback hooks back.
            net::StreamSource::Hooks hooks;
            hooks.refresh_url = [this, &t] { return fresh_url(t); };
            hooks.refusal_budget_ms = 20000;
            src.http->set_hooks(std::move(hooks));
        }
        net::StreamSource* http = src.http.get();
        src.reader = media::open_demuxer([http, &t](void* dst, size_t n) {
            size_t got = http->read(dst, n);
            t.bytes += got;
            return got;
        });
        if (!src.reader->read_header()) {
            results[idx].refused = http->refused();
            results[idx].error = std::string(stream_name(t.type)) + " demux: " + src.reader->error() +
                                 (http->error().empty() ? "" : " / http: " + http->error());
            return;
        }
        t.info = src.reader->track();
        t.segments = src.reader->segments();
        if (cfg_.live && !src.reader->read_fragment(first_samples[&t == &video_ ? 0 : 1])) {
            results[idx].error = std::string(stream_name(t.type)) + " live: no first segment (" + src.reader->error() + ")";
            return;
        }
        {
            std::lock_guard<std::mutex> lock(t.mutex);
            if (t.url == init_url) t.url = http->url();  // keep redirected edge URL unless concurrently refreshed
        }
        OLOG("%s: %s %s timescale=%u %ux%u fps=%u/%u ch=%u rate=%u codec_data=%zu bytes, media_time=%lld, %zu segments",
             stream_name(t.type), t.info.container == media::Container::WebM ? "webm" : "mp4",
             media::codec_name(t.info.codec), t.info.timescale, t.info.width, t.info.height,
             t.info.fps_num, t.info.fps_den, t.info.channels, t.info.sample_rate, t.info.codec_data.size(),
             static_cast<long long>(t.info.media_time), t.segments.size());
        results[idx].ok = true;
    };
    if (sources.size() > 1) {
        std::thread second([&] { open_source(1); });
        open_source(0);
        second.join();
    } else if (!sources.empty()) {
        open_source(0);
    }
    for (size_t i = 0; i < sources.size(); ++i) {
        if (!results[i].ok) {
            stream_refused_ = results[i].refused;
            error_ = results[i].error;
            return false;
        }
    }
    esp::VideoMime vmime = esp::VIDEO_H264;
    esp::AudioMime amime = esp::AUDIO_AAC;
    if ((cfg_.use_video && !video_mime(video_.info.codec, vmime)) ||
        (cfg_.use_audio && !audio_mime(audio_.info.codec, amime))) {
        error_ = std::string("unsupported codecs: video ") + media::codec_name(video_.info.codec) + ", audio " +
                 media::codec_name(audio_.info.codec) + " (expected H.264/VP9/AV1 + Opus/AAC)";
        return false;
    }

    const int cached = g_cached_pcm_bits.load();
    std::vector<int> audio_candidates = {0};
    std::unique_ptr<audio::Pipeline> dsp;
    if (cfg_.use_audio && cfg_.audio_dsp) {
        if (cached != 0) {
            const int init_bits = cached > 0 ? cached : 32;
            dsp = std::make_unique<audio::Pipeline>();
            if (dsp->init(audio_.info, cfg_.has_audio_loudness, cfg_.audio_loudness_db, init_bits)) {
                if (cached > 0) {
                    audio_candidates = {cached, 0};
                } else {
                    audio_candidates = {32, 24, 16, 0};
                }
            } else {
                OLOG("audio dsp: init failed, falling back to TV HW decoder");
                dsp.reset();
            }
        }
    }
    std::deque<media::Sample> saved_live_samples[2];
    if (cfg_.live && audio_candidates.size() > 1) {
        saved_live_samples[0] = first_samples[0];
        saved_live_samples[1] = first_samples[1];
    }

    auto rewind_sources = [&]() -> bool {
        for (size_t i = 0; i < sources.size(); ++i) {
            Track& t = *sources[i].track;
            if (cfg_.live) {
                sources[i].http = std::make_unique<net::LiveReader>(t.url, cfg_.stream_headers, live_seq);
            } else if (!t.segments.empty()) {
                const media::Segment& seg = t.segments.front();
                sources[i].http = std::make_unique<net::RangeReader>(t.url, cfg_.stream_headers, 4 * 1024 * 1024, seg.offset);
                net::StreamSource* http = sources[i].http.get();
                sources[i].reader = media::resume_demuxer(
                    [http, &t](void* dst, size_t n) {
                        size_t got = http->read(dst, n);
                        t.bytes += got;
                        return got;
                    },
                    t.info, seg);
                continue;
            } else {
                sources[i].http = std::make_unique<net::RangeReader>(t.url, cfg_.stream_headers);
            }
            net::StreamSource* http = sources[i].http.get();
            sources[i].reader = media::open_demuxer([http, &t](void* dst, size_t n) {
                size_t got = http->read(dst, n);
                t.bytes += got;
                return got;
            });
            if (!sources[i].reader->read_header()) return false;
            if (cfg_.live) {
                std::deque<media::Sample> discard;
                if (!sources[i].reader->read_fragment(discard)) return false;
                first_samples[&t == &video_ ? 0 : 1] = saved_live_samples[&t == &video_ ? 0 : 1];
            }
        }
        return true;
    };

    auto await_prepare = [&](int net_wait_ms, int esp_wait_ms) {
        const auto net_deadline = Clock::now() + std::chrono::milliseconds(net_wait_ms);
        Clock::time_point esp_deadline{};
        auto tracks_fed = [&] {
            return (!cfg_.use_video || video_.packet_fed.load(std::memory_order_relaxed)) &&
                   (!cfg_.use_audio || audio_.packet_fed.load(std::memory_order_relaxed));
        };
        bool fed = false;
        std::unique_lock<std::mutex> lock(state_mutex_);
        for (;;) {
            fed = tracks_fed();
            if (!running_ || prepared_ || prepare_failed_) break;
            const auto now = Clock::now();
            if (esp_deadline == Clock::time_point{} && fed) {
                esp_deadline = now + std::chrono::milliseconds(esp_wait_ms);
            }
            const auto deadline = (esp_deadline != Clock::time_point{}) ? esp_deadline : net_deadline;
            if (now >= deadline) break;
            state_cv_.wait_until(lock, deadline);
        }
        return fed;
    };

    bool sources_consumed = false;
    // Only latch g_cached_pcm_bits = 0 on initial uncached probing when every PCM mode was explicitly refused;
    // prepare timeouts or a transient glitch after PCM was already proven fall back to HW for this video only.
    bool all_pcm_rejected = (cached < 0);
    bool pcm_fell_back = false;

    for (size_t attempt = 0; attempt < audio_candidates.size(); ++attempt) {
        const int pcm_bits = audio_candidates[attempt];
        const bool has_fallback = (pcm_bits > 0 && attempt + 1 < audio_candidates.size());

        has_color_info_ = false;
        progressive_scan_ = false;
        scaler_id_ = -2;
        scaler_rc_ = -999;

        if (sources_consumed) {
            if (!rewind_sources()) {
                error_ = "stream rewind failed after audio DSP fallback";
                return false;
            }
            sources_consumed = false;
        }
        if (pcm_bits > 0 && dsp) {
            dsp->reconfigure_bits(pcm_bits);
            audio_dsp_ = std::move(dsp);
        } else {
            audio_dsp_.reset();
        }

        handle_ = api_->create();
        if (!handle_) {
            error_ = "esplusplayer_create failed";
            return false;
        }
        api_->set_error_cb(handle_, &Player::on_error, this);
        api_->set_buffer_status_cb(handle_, &Player::on_buffer_status, this);
        api_->set_eos_cb(handle_, &Player::on_eos, this);
        api_->set_ready_to_prepare_cb(handle_, &Player::on_ready_to_prepare, this);
        api_->set_prepare_async_done_cb(handle_, &Player::on_prepare_done, this);
        api_->set_resource_conflicted_cb(handle_, &Player::on_resource_conflicted, this);
        api_->set_ready_to_seek_cb(handle_, &Player::on_ready_to_seek, this);
        api_->set_seek_done_cb(handle_, &Player::on_seek_done, this);
        if (api_->set_video_frame_dropped_cb) {
            api_->set_video_frame_dropped_cb(handle_, &Player::on_video_frame_dropped, this);
        }

        int rc = api_->open(handle_);
        OLOG("esp open -> %d", rc);
        if (rc != 0) {
            error_ = "esplusplayer_open failed: " + std::to_string(rc);
            teardown_esp();
            return false;
        }
        if (!cfg_.app_type.empty()) {
            if (api_->set_app_info_ex && !cfg_.app_runtitle.empty()) {
                esp::AppInfoEx app{const_cast<char*>(cfg_.app_id.c_str()), const_cast<char*>(cfg_.app_version.c_str()),
                                   const_cast<char*>(cfg_.app_type.c_str()), const_cast<char*>(cfg_.app_runtitle.c_str())};
                rc = api_->set_app_info_ex(handle_, &app);
                OLOG("esp set_app_info_ex(%s %s %s %s) -> %d", app.id, app.version, app.type, app.runtitle, rc);
            } else {
                esp::AppInfo app{const_cast<char*>(cfg_.app_id.c_str()), const_cast<char*>(cfg_.app_version.c_str()),
                                 const_cast<char*>(cfg_.app_type.c_str())};
                rc = api_->set_app_info(handle_, &app);
                OLOG("esp set_app_info(%s %s %s) -> %d", app.id, app.version, app.type, rc);
            }
        }
        if (cfg_.use_display) {
            rc = api_->set_ecore_display(handle_, esp::DISPLAY_OVERLAY, cfg_.window, 0, 0, cfg_.width, cfg_.height);
            OLOG("esp set_ecore_display(%p, %dx%d) -> %d", cfg_.window, cfg_.width, cfg_.height, rc);
            rc = api_->set_display_mode(handle_, esp::DISPLAY_MODE_LETTER_BOX);
            OLOG("esp set_display_mode -> %d", rc);
        }

        if (cfg_.use_audio) {
            if (api_->create_sound_stream_info && api_->set_sound_stream_info) {
                int s_rc = api_->create_sound_stream_info(0 /* SOUND_STREAM_TYPE_MEDIA */, nullptr, nullptr, &sound_stream_info_);
                if (s_rc == 0 && sound_stream_info_) {
                    rc = api_->set_sound_stream_info(handle_, sound_stream_info_);
                    OLOG("esp set_sound_stream_info(media) -> %d", rc);
                }
            }
            esp::AudioStreamInfo ai{};
            if (audio_dsp_) {
                ai.mime_type = pcm_mime(pcm_bits);
                ai.channels = static_cast<uint32_t>(audio_dsp_->output_channels());
                ai.sample_rate = static_cast<uint32_t>(audio_dsp_->output_sample_rate());
                ai.bitrate = ai.sample_rate * ai.channels * static_cast<uint32_t>(pcm_bits / 8) * 8u;
                rc = api_->set_audio_stream_info(handle_, &ai);
                OLOG("esp set_audio_stream_info(PCM_S%dLE %u ch %u Hz %u bps) -> %d", pcm_bits, ai.channels,
                     ai.sample_rate, ai.bitrate, rc);
            } else {
                if (!audio_.info.codec_data.empty()) {
                    ai.codec_data = reinterpret_cast<char*>(audio_.info.codec_data.data());
                    ai.codec_data_length = static_cast<uint32_t>(audio_.info.codec_data.size());
                }
                ai.mime_type = amime;
                ai.bitrate = cfg_.audio_bitrate > 0 ? static_cast<uint32_t>(cfg_.audio_bitrate) : 128000;
                ai.channels = audio_.info.channels;
                ai.sample_rate = audio_.info.sample_rate;
                rc = api_->set_audio_stream_info(handle_, &ai);
                OLOG("esp set_audio_stream_info(%s %u ch %u Hz %u bps) -> %d", media::codec_name(audio_.info.codec),
                     ai.channels, ai.sample_rate, ai.bitrate, rc);
            }
            if (rc != 0) {
                if (has_fallback) {
                    OLOG("audio dsp: TV refused PCM_S%dLE (%d), trying fallback", pcm_bits, rc);
                    pcm_fell_back = true;
                    dsp = std::move(audio_dsp_);
                    teardown_esp();
                    continue;
                }
                error_ = std::string("TV refused audio ") + media::codec_name(audio_.info.codec) + " (" + std::to_string(rc) + ")";
                teardown_esp();
                return false;
            }
        }
        if (cfg_.use_video) {
            if (api_->set_video_scan_type) {
                rc = api_->set_video_scan_type(handle_, esp::VIDEO_SCAN_PROGRESSIVE);
                progressive_scan_ = (rc == 0);
                OLOG("esp set_video_scan_type(progressive) -> %d", rc);
            }
            esp::VideoStreamInfo vi{};
            if (!video_.info.codec_data.empty()) {
                vi.codec_data = reinterpret_cast<char*>(video_.info.codec_data.data());
                vi.codec_data_length = static_cast<uint32_t>(video_.info.codec_data.size());
            }
            vi.mime_type = vmime;
            vi.width = video_.info.width;
            vi.height = video_.info.height;
            vi.max_width = static_cast<uint32_t>(cfg_.max_width);
            vi.max_height = static_cast<uint32_t>(cfg_.max_height);
            vi.framerate_num = video_.info.fps_num > 0 ? video_.info.fps_num : static_cast<uint32_t>(cfg_.fps);
            vi.framerate_den = (video_.info.fps_num > 0 && video_.info.fps_den > 0) ? video_.info.fps_den : 1;
            rc = api_->set_video_stream_info(handle_, &vi);
            OLOG("esp set_video_stream_info(%s %ux%u max %ux%u @%u/%u) -> %d", media::codec_name(video_.info.codec), vi.width,
                 vi.height, vi.max_width, vi.max_height, vi.framerate_num, vi.framerate_den, rc);
            if (rc != 0) {
                error_ = std::string("TV refused ") + media::codec_name(video_.info.codec) + " " +
                         std::to_string(vi.width) + "x" + std::to_string(vi.height) + " (" + std::to_string(rc) + ")";
                teardown_esp();
                return false;
            }

            const bool is_hdr = video_.info.has_hdr || cfg_.is_hdr;
            if (is_hdr || video_.info.has_color || video_.info.chroma_siting_horz || video_.info.chroma_siting_vert ||
                video_.info.height >= 720) {
                has_color_info_ = true;
                color_info_ = esp::ESMatroskaColor{};
                const uint32_t def_mat = is_hdr ? 9 : 1;
                const uint32_t def_prim = is_hdr ? 9 : 1;
                const uint32_t def_tc = is_hdr ? ((cfg_.hdr_mode == 2) ? 18 : 16) : 1;
                const uint32_t def_bits = is_hdr ? 10 : 8;
                color_info_.matrix_coefficients = video_.info.matrix_coefficients ? video_.info.matrix_coefficients : def_mat;
                color_info_.primaries = video_.info.color_primaries ? video_.info.color_primaries : def_prim;
                color_info_.transfer_characteristics =
                    video_.info.transfer_characteristics ? video_.info.transfer_characteristics : def_tc;
                color_info_.range = video_.info.color_range ? video_.info.color_range : 1;
                color_info_.bits_per_channel = video_.info.bits_per_channel ? video_.info.bits_per_channel : def_bits;
                color_info_.chroma_subsampling_horizontal = 1;
                color_info_.chroma_subsampling_vertical = 1;
                color_info_.chroma_siting_horizontal = video_.info.chroma_siting_horz ? video_.info.chroma_siting_horz : 1;
                color_info_.chroma_siting_vertical =
                    video_.info.chroma_siting_vert ? video_.info.chroma_siting_vert : (color_info_.primaries == 9 ? 1 : 2);
                if (is_hdr) {
                    if (video_.info.has_clli && (video_.info.max_cll > 0 || video_.info.max_fall > 0)) {
                        color_info_.max_cll = video_.info.max_cll;
                        color_info_.max_fall = video_.info.max_fall;
                    } else if (color_info_.transfer_characteristics == 16) {
                        color_info_.max_cll = 1000;
                        color_info_.max_fall = 300;
                    }
                    if (video_.info.has_mdcv && video_.info.max_luminance > 0) {
                        color_info_.mastering_metadata.primary_r_chromaticity_x = video_.info.display_primaries_x[2] * 0.00002;
                        color_info_.mastering_metadata.primary_r_chromaticity_y = video_.info.display_primaries_y[2] * 0.00002;
                        color_info_.mastering_metadata.primary_g_chromaticity_x = video_.info.display_primaries_x[0] * 0.00002;
                        color_info_.mastering_metadata.primary_g_chromaticity_y = video_.info.display_primaries_y[0] * 0.00002;
                        color_info_.mastering_metadata.primary_b_chromaticity_x = video_.info.display_primaries_x[1] * 0.00002;
                        color_info_.mastering_metadata.primary_b_chromaticity_y = video_.info.display_primaries_y[1] * 0.00002;
                        color_info_.mastering_metadata.white_point_chromaticity_x = video_.info.white_point_x * 0.00002;
                        color_info_.mastering_metadata.white_point_chromaticity_y = video_.info.white_point_y * 0.00002;
                        color_info_.mastering_metadata.luminance_max = video_.info.max_luminance * 0.0001;
                        color_info_.mastering_metadata.luminance_min = video_.info.min_luminance * 0.0001;
                    } else if (color_info_.transfer_characteristics == 16) {
                        color_info_.mastering_metadata.primary_r_chromaticity_x = 0.708;
                        color_info_.mastering_metadata.primary_r_chromaticity_y = 0.292;
                        color_info_.mastering_metadata.primary_g_chromaticity_x = 0.170;
                        color_info_.mastering_metadata.primary_g_chromaticity_y = 0.797;
                        color_info_.mastering_metadata.primary_b_chromaticity_x = 0.131;
                        color_info_.mastering_metadata.primary_b_chromaticity_y = 0.046;
                        color_info_.mastering_metadata.white_point_chromaticity_x = 0.3127;
                        color_info_.mastering_metadata.white_point_chromaticity_y = 0.3290;
                        color_info_.mastering_metadata.luminance_max = 1000.0;
                        color_info_.mastering_metadata.luminance_min = 0.0001;
                    }
                }
                OLOG("esp %s colorimetry: matrix=%u prim=%u tc=%u range=%u bits=%u siting=%u,%u max_cll=%u max_fall=%u",
                     is_hdr ? "HDR" : "SDR", color_info_.matrix_coefficients, color_info_.primaries,
                     color_info_.transfer_characteristics, color_info_.range, color_info_.bits_per_channel,
                     color_info_.chroma_siting_horizontal, color_info_.chroma_siting_vertical, color_info_.max_cll,
                     color_info_.max_fall);
            }
        }

        if (cfg_.live) {
            std::deque<media::Sample>& ref = first_samples[cfg_.use_video ? 0 : 1];
            live_base_ms_ = INT64_MAX;
            for (const auto& s : ref) live_base_ms_ = std::min(live_base_ms_, s.pts_ms);
            if (ref.empty()) live_base_ms_ = 0;
            push_samples(video_, first_samples[0]);
            push_samples(audio_, first_samples[1]);
        }

        duration_ms_ = cfg_.duration_ms;
        if (duration_ms_ == 0 && !video_.segments.empty()) duration_ms_ = video_.segments.back().start_ms;

        prepared_ = false;
        prepare_failed_ = false;
        dsp_confirmed_ = false;
        paused_ = false;
        eos_ = false;
        errors_ = 0;
        feed_from_ms_ = 0;
        position_ms_ = 0;
        video_.submitted_ms = 0;
        audio_.submitted_ms = 0;
        video_.underruns = 0;
        audio_.underruns = 0;
        running_ = true;

        if (cfg_.start_ms > 0 && !video_.segments.empty()) {
            int64_t target = std::min<int64_t>(cfg_.start_ms, duration_ms_ > 0 ? duration_ms_ - 1000 : cfg_.start_ms);
            rc = api_->seek(handle_, static_cast<uint64_t>(target));
            OLOG("esp start position %lld ms -> %d", static_cast<long long>(target), rc);
            feed_from_ms_ = target;
            position_ms_ = target;
            for (Source& src : sources) {
                Track& t = *src.track;
                media::Segment seg{0, 0, 0};
                if (!t.segments.empty()) {
                    auto it = std::upper_bound(t.segments.begin(), t.segments.end(), target,
                                               [](int64_t ms, const media::Segment& s) { return ms < s.start_ms; });
                    seg = (it == t.segments.begin()) ? t.segments.front() : *std::prev(it);
                }
                std::string url = src.http->url();
                t.url = url;
                src.http = std::make_unique<net::RangeReader>(url, cfg_.stream_headers, 4 * 1024 * 1024, seg.offset);
                net::StreamSource* http = src.http.get();
                src.reader = media::resume_demuxer(
                    [http, &t](void* dst, size_t n) {
                        size_t got = http->read(dst, n);
                        t.bytes += got;
                        return got;
                    },
                    t.info, seg);
            }
        }
        sources_consumed = true;
        for (Source& src : sources) {
            Track& t = *src.track;
            t.ready_to_feed = false;
            t.packet_fed = false;
            start_track(t, src.reader.release(), src.http.release());
        }

        rc = api_->prepare_async(handle_);
        OLOG("esp prepare_async -> %d", rc);
        if (rc != 0) {
            running_ = false;
            for (Source& src : sources) stop_track(*src.track);
            teardown_esp();
            if (has_fallback) {
                pcm_fell_back = true;
                dsp = std::move(audio_dsp_);
                dsp->reset();
                continue;
            }
            error_ = "prepare_async failed: " + std::to_string(rc);
            return false;
        }

        if (audio_dsp_) {
            const bool fed = await_prepare((cached > 0) ? 5000 : 20000, (cached > 0) ? 5000 : 3500);
            if (!prepared_) {
                const bool rejected = prepare_failed_.load();
                if (!rejected) all_pcm_rejected = false;
                if (!rejected && (cached > 0 || !fed)) {
                    // Slow network/resume loading, not a TV PCM rejection: keep DSP active and let control_loop() wait.
                    OLOG("audio dsp: prepare still loading (%s), keeping PCM_S%dLE", fed ? "buffering" : "network",
                         pcm_bits);
                } else {
                    OLOG("audio dsp: esp prepare %s for PCM_S%dLE", rejected ? "failed" : "timed out", pcm_bits);
                    running_ = false;
                    for (Source& src : sources) stop_track(*src.track);
                    teardown_esp();
                    if (has_fallback) {
                        pcm_fell_back = true;
                        dsp = std::move(audio_dsp_);
                        dsp->reset();
                        continue;
                    }
                    error_ = "prepare failed after audio DSP fallback";
                    return false;
                }
            } else {
                confirm_active_dsp();
            }
        } else if (pcm_fell_back) {
            await_prepare((cached < 0 && all_pcm_rejected) ? 12000 : 5000,
                          (cached < 0 && all_pcm_rejected) ? 3500 : 5000);
            if (prepared_ && cached < 0 && all_pcm_rejected) {
                g_cached_pcm_bits = 0;
                OLOG("audio dsp: TV rejected all PCM modes, latching HW decoder");
            } else if (!prepared_ && prepare_failed_) {
                running_ = false;
                for (Source& src : sources) stop_track(*src.track);
                teardown_esp();
                error_ = "prepare failed in HW audio fallback";
                return false;
            }
        }

        control_thread_ = std::thread([this] { control_loop(); });
        return true;
    }
    return false;
}

void Player::attach_hooks(Track& t, net::StreamSource& source) {
    net::StreamSource::Hooks hooks;
    hooks.aborted = [this, &t] { return !running_ || !t.active; };
    hooks.refresh_url = [this, &t] { return fresh_url(t); };
    hooks.on_recovering = [&t](bool on) { t.reconnecting = on; };
    source.set_hooks(std::move(hooks));
}

// Both tracks hit the expiry together: the first caller re-resolves, the second reuses the result.
std::string Player::fresh_url(Track& t) {
    std::lock_guard<std::mutex> lock(refresh_mutex_);
    if (!cfg_.refresh_urls) return "";
    if (last_refresh_ == Clock::time_point{} || Clock::now() - last_refresh_ > std::chrono::seconds(30)) {
        std::string v, a;
        if (!cfg_.refresh_urls(v, a)) {
            OLOG("refresh: stream URLs could not be re-resolved");
            return "";
        }
        last_refresh_ = Clock::now();
        std::lock_guard<std::mutex> lv(video_.mutex), la(audio_.mutex);
        video_.url = v;
        audio_.url = a;
        OLOG("refresh: new stream URLs");
    }
    std::lock_guard<std::mutex> lt(t.mutex);
    return t.url;
}

void Player::start_track(Track& t, media::Demuxer* reader, net::StreamSource* source) {
    attach_hooks(t, *source);
    t.active = true;
    t.demux_thread = std::thread([this, &t, reader, source] { demux_loop(t, reader, source); });
    t.feed_thread = std::thread([this, &t] { feed_loop(t); });
}

void Player::notify_track(Track& t) {
    {
        std::lock_guard<std::mutex> lock(t.mutex);
    }
    t.cv.notify_all();
}

void Player::mark_track_fed(Track& t) {
    if (t.packet_fed.load(std::memory_order_relaxed)) return;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        t.packet_fed.store(true, std::memory_order_relaxed);
    }
    state_cv_.notify_all();
}

void Player::confirm_active_dsp() {
    if (!audio_dsp_ || dsp_confirmed_.exchange(true)) return;
    const int bits = audio_dsp_->pcm_bits();
    g_cached_pcm_bits = bits;
    OLOG("audio dsp active: %s %dHz -> 48000Hz PCM_S%dLE (atten -%.2fdB%s)",
         media::codec_name(audio_.info.codec), audio_dsp_->source_sample_rate(), bits,
         audio_dsp_->attenuation_db(),
         audio_dsp_->source_sample_rate() != 48000 ? ", 160-tap sinc" : ", 1:1 bypass");
}

void Player::stop_track(Track& t) {
    {
        std::lock_guard<std::mutex> lock(t.mutex);
        t.active = false;
    }
    t.cv.notify_all();
    if (t.feed_thread.joinable()) t.feed_thread.join();
    if (t.demux_thread.joinable()) t.demux_thread.join();
    std::lock_guard<std::mutex> lock(t.mutex);
    t.queue.clear();
    t.queued_ms = 0;
    t.queued_bytes = 0;
    t.demux_done = false;
}

bool Player::seek(int64_t ms) {
    std::lock_guard<std::mutex> lock(seek_mutex_);
    if (!prepared_ || video_.segments.empty()) return false;
    ms = std::max<int64_t>(0, std::min<int64_t>(ms, duration_ms_ > 0 ? duration_ms_ - 1000 : ms));
    // Exact seek: ESPlayer is told the exact target; video is fed from the keyframe segment at or before it
    // (the decoder needs it) and audio from the target. Precision matters for SponsorBlock skips.
    int64_t target = ms;
    auto t0 = Clock::now();

    Track* tracks[] = {&video_, &audio_};
    bool use[] = {cfg_.use_video, cfg_.use_audio};
    for (int i = 0; i < 2; ++i)
        if (use[i]) stop_track(*tracks[i]);
    if (audio_dsp_) audio_dsp_->reset();

    for (Track* t : tracks) {
        t->ready_to_feed = false;
        t->packet_fed = false;
    }
    feed_from_ms_ = target;
    eos_ = false;
    seek_pending_ = true;
    position_ms_ = target;
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
    const int64_t started_ms = started_at_ms_.load();
    stall_grace_until_ms_ = (started_ms > 0 ? (now_ms - started_ms) : 0) + 3000;
    int rc = api_->seek(handle_, static_cast<uint64_t>(target));
    if (rc != 0) seek_pending_ = false;

    for (int i = 0; i < 2; ++i) {
        if (!use[i]) continue;
        Track& t = *tracks[i];
        media::Segment seg{0, 0, 0};
        if (!t.segments.empty()) {
            auto it = std::upper_bound(t.segments.begin(), t.segments.end(), target,
                                       [](int64_t ms, const media::Segment& s) { return ms < s.start_ms; });
            seg = (it == t.segments.begin()) ? t.segments.front() : *std::prev(it);
        }
        std::string url;
        {
            std::lock_guard<std::mutex> lt(t.mutex);
            url = t.url;
        }
        auto* http = new net::RangeReader(url, cfg_.stream_headers, 4 * 1024 * 1024, seg.offset);
        auto reader = media::resume_demuxer(
            [http, &t](void* dst, size_t n) {
                size_t got = http->read(dst, n);
                t.bytes += got;
                return got;
            },
            t.info, seg);
        start_track(t, reader.release(), http);
    }
    OLOG("seek %lld -> %lld ms (esp %d), threads restarted in %lld ms", static_cast<long long>(ms),
         static_cast<long long>(target), rc, static_cast<long long>(ms_since(t0)));
    return rc == 0;
}

// Rebases (live) and queues demuxed samples for the feeder.
void Player::push_samples(Track& t, std::deque<media::Sample>& samples) {
    int64_t ms = 0;
    size_t bytes = 0;
    for (auto& s : samples) {
        s.pts_ms -= live_base_ms_;
        ms += s.duration_ms;
        bytes += s.data.size();
    }
    std::lock_guard<std::mutex> lock(t.mutex);
    for (auto& s : samples) t.queue.push_back(std::move(s));
    t.queued_ms += ms;
    t.queued_bytes += bytes;
    t.cv.notify_all();
}

void Player::demux_loop(Track& t, media::Demuxer* reader_raw, net::StreamSource* source_raw) {
    std::unique_ptr<media::Demuxer> reader(reader_raw);
    std::unique_ptr<net::StreamSource> source(source_raw);
    const std::string initial_url = source->url();
    reader->set_batch_cb([this, &t](std::deque<media::Sample>& batch) {
        if (!running_ || !t.active) return false;
        if (!batch.empty()) {
            push_samples(t, batch);
            batch.clear();
        }
        return running_ && t.active;
    });
    std::deque<media::Sample> fragment;
    while (running_) {
        {
            std::unique_lock<std::mutex> lock(t.mutex);
            t.cv.wait(lock, [&] {
                return !running_ || !t.active || (t.queued_ms < cfg_.max_queue_ms && t.queued_bytes < cfg_.max_queue_bytes);
            });
        }
        if (!running_ || !t.active) break;
        fragment.clear();
        if (!reader->read_fragment(fragment)) {
            if (!running_ || !t.active) break;
            OLOG("%s demux: end at byte %llu/%lld (%s%s%s)", stream_name(t.type),
                 static_cast<unsigned long long>(source->position()), static_cast<long long>(source->total_size()),
                 reader->error().empty() ? "eof" : reader->error().c_str(), source->error().empty() ? "" : ", http: ",
                 source->error().c_str());
            break;
        }
        if (!fragment.empty()) push_samples(t, fragment);
    }
    std::lock_guard<std::mutex> lock(t.mutex);
    if (source->url() != initial_url && t.url == initial_url) {
        t.url = source->url();  // keep redirected edge URL without overwriting a concurrent fresh_url()
    }
    t.demux_done = true;
    t.cv.notify_all();
}

bool Player::submit_es_packet(Track& t, esp::StreamType type, const uint8_t* data, size_t size, int64_t pts_ms,
                              int64_t duration_ms) {
    esp::EsPacket pkt{};
    pkt.type = type;
    pkt.buffer = reinterpret_cast<char*>(const_cast<uint8_t*>(data));
    pkt.buffer_size = static_cast<uint32_t>(size);
    const int64_t delay = audio_delay_ms_.load();
    int64_t pts = pts_ms;
    if (type == esp::STREAM_VIDEO && delay > 0) pts += delay;
    else if (type == esp::STREAM_AUDIO && delay < 0) pts += -delay;
    pkt.pts = static_cast<uint64_t>(pts);
    pkt.duration = static_cast<uint64_t>(std::max<int64_t>(0, duration_ms));
    if (type == esp::STREAM_VIDEO && has_color_info_) {
        pkt.matroska_color_info = &color_info_;
    }
    int wait_ms = 10;
    int oom_retries = 0;
    for (;;) {
        const uint64_t seq_before = t.buffer_wake_seq.load(std::memory_order_acquire);
        const int st = api_->submit_packet(handle_, &pkt);
        if (st == esp::SUBMIT_SUCCESS) {
            mark_track_fed(t);
            return true;
        }
        if (st == esp::SUBMIT_FULL || st == esp::SUBMIT_NOT_PREPARED || st == esp::SUBMIT_OUT_OF_MEMORY) {
            if (st == esp::SUBMIT_OUT_OF_MEMORY && ++oom_retries == 10) {
                OLOG("%s feed: ESPlayer OOM at pts=%lld size=%u, waiting for decoder headroom", stream_name(type),
                     static_cast<long long>(pts_ms), pkt.buffer_size);
            }
            bool woken = false;
            {
                std::unique_lock<std::mutex> lock(t.mutex);
                woken = t.cv.wait_for(lock, std::chrono::milliseconds(wait_ms), [&] {
                    return !running_ || !t.active || t.buffer_wake_seq.load(std::memory_order_relaxed) != seq_before;
                });
            }
            if (!running_ || !t.active) return false;
            wait_ms = woken ? 10 : std::min(wait_ms * 2, 40);
            continue;
        }
        OLOG("%s feed: submit status %d at pts=%lld size=%u", stream_name(type), st,
             static_cast<long long>(pts_ms), pkt.buffer_size);
        return true;
    }
}

void Player::feed_loop(Track& t) {
    {
        std::unique_lock<std::mutex> lock(t.mutex);
        t.cv.wait(lock, [&] { return !running_ || !t.active || t.ready_to_feed.load(); });
    }
    if (!running_ || !t.active) return;
    OLOG("%s feed: start", stream_name(t.type));
    std::vector<audio::PcmFrame> pcm_out;
    uint64_t br_bytes = 0;
    int64_t br_dur_ms = 0;
    while (running_ && t.active) {
        media::Sample s;
        {
            std::unique_lock<std::mutex> lock(t.mutex);
            t.cv.wait(lock, [&] { return !running_ || !t.active || !t.queue.empty() || t.demux_done; });
            if (!running_ || !t.active) break;
            if (t.queue.empty()) {
                lock.unlock();
                if (t.type == esp::STREAM_AUDIO && audio_dsp_) {
                    audio_dsp_->flush(pcm_out);
                    for (const auto& f : pcm_out) {
                        if (!running_ || !t.active) return;
                        if (!submit_es_packet(t, t.type, f.data.data(), f.data.size(), f.pts_ms, f.duration_ms)) return;
                        t.submitted_ms = f.pts_ms + f.duration_ms;
                    }
                }
                api_->submit_eos_packet(handle_, t.type);
                mark_track_fed(t);
                OLOG("%s feed: eos submitted", stream_name(t.type));
                break;
            }
            const bool was_full = (t.queued_ms >= cfg_.max_queue_ms || t.queued_bytes >= cfg_.max_queue_bytes);
            s = std::move(t.queue.front());
            t.queue.pop_front();
            t.queued_ms -= s.duration_ms;
            t.queued_bytes -= s.data.size();
            const bool now_room = (t.queued_ms < cfg_.max_queue_ms && t.queued_bytes < cfg_.max_queue_bytes);
            lock.unlock();
            if (was_full && now_room) t.cv.notify_all();
        }

        // Pace submission on the playback clock so ESPlayer never holds more than feed_ahead_ms
        // (keeps memory bounded and makes seeks / quality switches cheap). Paused => clock frozen => we wait.
        if (s.pts_ms > position_ms_.load(std::memory_order_relaxed) + cfg_.feed_ahead_ms) {
            std::unique_lock<std::mutex> lock(t.mutex);
            t.cv.wait(lock, [&] {
                return !running_ || !t.active ||
                       s.pts_ms <= position_ms_.load(std::memory_order_relaxed) + cfg_.feed_ahead_ms;
            });
        }
        if (!running_ || !t.active) break;
        if (s.pts_ms < 0) continue;

        if (t.type == esp::STREAM_AUDIO && audio_dsp_) {
            const int64_t from_ms = feed_from_ms_.load();
            if (s.pts_ms + 120 < from_ms) continue;
            const bool emit = (s.pts_ms >= from_ms);
            if (!audio_dsp_->process(s.data.data(), s.data.size(), s.pts_ms, emit, pcm_out)) {
                OLOG("audio dsp: frame error at pts=%lld", static_cast<long long>(s.pts_ms));
                continue;
            }
            for (const auto& f : pcm_out) {
                if (!running_ || !t.active) return;
                if (!submit_es_packet(t, t.type, f.data.data(), f.data.size(), f.pts_ms, f.duration_ms)) return;
                t.submitted_ms = f.pts_ms + f.duration_ms;
            }
            continue;
        }

        // After a seek (or a live start) the audio segment starts a bit before the video keyframe.
        if (t.type == esp::STREAM_AUDIO && s.pts_ms < feed_from_ms_) continue;
        if (!submit_es_packet(t, t.type, s.data.data(), s.data.size(), s.pts_ms, s.duration_ms)) return;
        t.submitted_ms = s.pts_ms + s.duration_ms;
        if (t.type == esp::STREAM_VIDEO) {
            br_bytes += s.data.size();
            br_dur_ms += std::max<int64_t>(1, s.duration_ms);
            if (br_dur_ms >= 1000) {
                video_bitrate_kbps_.store(
                    static_cast<uint32_t>((br_bytes * 8ULL) / static_cast<uint64_t>(br_dur_ms)),
                    std::memory_order_relaxed);
                br_bytes = 0;
                br_dur_ms = 0;
            }
        }
    }
}

void Player::control_loop() {
    {
        std::unique_lock<std::mutex> lock(state_mutex_);
        state_cv_.wait(lock, [&] { return !running_ || prepared_ || prepare_failed_; });
    }
    if (!running_ || prepare_failed_) return;
    confirm_active_dsp();
    int rc = api_->start(handle_);
    const auto started_at = Clock::now();
    started_at_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(started_at.time_since_epoch()).count();
    OLOG("esp start -> %d", rc);

    // Stall detector: the playback clock must advance with wall time while PLAYING.
    uint64_t last_pos = 0;
    auto last_advance = Clock::now();
    bool in_stall = false;
    while (running_) {
        sleep_ms(50);
        uint64_t pos = 0;
        api_->get_playing_time(handle_, &pos);
        // Until seek_done the player still reports the old position: keep the target so feeding
        // (paced on position_ms_) is not throttled against a stale clock.
        if (!seek_pending_) {
            int64_t new_pos = static_cast<int64_t>(pos);
            if (new_pos != position_ms_.exchange(new_pos)) {
                for (Track* t : {&video_, &audio_}) notify_track(*t);
            }
        }
        if (paused_ || eos_ || ms_since(started_at) < stall_grace_until_ms_) {
            last_advance = Clock::now();
            continue;
        }
        if (pos != last_pos) {
            if (in_stall) {
                int64_t dur = ms_since(last_advance);
                if (dur > longest_stall_ms_) longest_stall_ms_ = dur;
                OLOG("stall ended after %lld ms at %llu ms", static_cast<long long>(dur),
                     static_cast<unsigned long long>(pos));
                in_stall = false;
            }
            last_pos = pos;
            last_advance = Clock::now();
        } else if (!in_stall && ms_since(last_advance) > 150 && ms_since(started_at) > 1000) {
            in_stall = true;
            ++stalls_;
            int64_t vq = 0, aq = 0;
            {
                std::lock_guard<std::mutex> lv(video_.mutex);
                vq = video_.queued_ms;
            }
            {
                std::lock_guard<std::mutex> la(audio_.mutex);
                aq = audio_.queued_ms;
            }
            OLOG("stall started at %llu ms (vq=%lld aq=%lld)", static_cast<unsigned long long>(pos),
                 static_cast<long long>(vq), static_cast<long long>(aq));
        }
    }
}

void Player::toggle_pause() {
    if (!handle_ || !prepared_) return;
    paused_ = !paused_;
    int rc = paused_ ? api_->pause(handle_) : api_->resume(handle_);
    OLOG("esp %s -> %d", paused_ ? "pause" : "resume", rc);
}

bool Player::set_rate(double rate) {
    if (!handle_ || !prepared_) return false;
    int rc = api_->set_playback_rate(handle_, rate, false);
    OLOG("esp set_playback_rate(%.2f) -> %d", rate, rc);
    return rc == 0;
}

PlayerStats Player::stats() {
    PlayerStats s{};
    s.state = handle_ ? api_->get_state(handle_) : esp::STATE_NONE;
    s.position_ms = position_ms_;
    {
        std::lock_guard<std::mutex> lock(video_.mutex);
        s.video_queued_ms = video_.queued_ms;
    }
    {
        std::lock_guard<std::mutex> lock(audio_.mutex);
        s.audio_queued_ms = audio_.queued_ms;
    }
    s.video_submitted_ms = video_.submitted_ms;
    s.audio_submitted_ms = audio_.submitted_ms;
    s.video_underruns = video_.underruns;
    s.audio_underruns = audio_.underruns;
    s.stalls = stalls_;
    s.longest_stall_ms = longest_stall_ms_;
    s.errors = errors_;
    s.eos = eos_ ? 1 : 0;
    s.duration_ms = duration_ms_;
    s.reconnecting = (video_.reconnecting || audio_.reconnecting) ? 1 : 0;
    s.dropped_frames = dropped_frames_;
    s.scaler_id = scaler_id_;
    s.scaler_rc = scaler_rc_;
    s.dsp_bits = audio_dsp_ ? audio_dsp_->pcm_bits() : 0;
    s.dsp_hf = audio_dsp_ ? audio_dsp_->hf_mode() : 0;
    s.video_bitrate_kbps = video_bitrate_kbps_.load(std::memory_order_relaxed);
    if (handle_ && prepared_ && s.state >= esp::STATE_READY) {
        if (api_->get_virtual_rsc_id && scaler_id_ == -2) {
            int rid = -1;
            int rsc_rc = api_->get_virtual_rsc_id(handle_, esp::RSC_VIDEO_RENDERER, &rid);
            scaler_rc_ = rsc_rc;
            s.scaler_rc = rsc_rc;
            int32_t resolved = (rsc_rc == 0 && rid >= 0) ? rid : -1;
            scaler_id_ = resolved;
            s.scaler_id = resolved;
            OLOG("esp virtual_rsc_id(video_renderer) -> %d (rc=%d)", resolved, rsc_rc);
        }
        uint64_t adaptive_drops = 0;
        if (api_->get_adaptive_info &&
            api_->get_adaptive_info(handle_, &adaptive_drops, esp::ADAPT_DROPPED_FRAMES) == 0) {
            s.dropped_frames = static_cast<uint32_t>(std::min<uint64_t>(adaptive_drops, UINT32_MAX));
        }
    }
    if (s.scaler_id == -2) s.scaler_id = -1;
    return s;
}

void Player::on_error(int err, void* self) {
    auto* p = static_cast<Player*>(self);
    ++p->errors_;
    OLOG("esp error 0x%x (%s)", err, p->api_->get_error_string(err));
}

void Player::on_buffer_status(esp::StreamType type, esp::BufferStatus status, void* self) {
    auto* p = static_cast<Player*>(self);
    // Underruns are expected while filling up before prepare; only count them during playback.
    if (status == esp::BUFFER_UNDERRUN && p->prepared_) {
        Track& t = type == esp::STREAM_VIDEO ? p->video_ : p->audio_;
        ++t.underruns;
        int64_t queued;
        {
            std::lock_guard<std::mutex> lock(t.mutex);
            queued = t.queued_ms;
            ++t.buffer_wake_seq;
        }
        t.cv.notify_all();
        OLOG("%s buffer underrun: position=%lld submitted=%lld (lead %lld ms) demux queue=%lld ms", stream_name(type),
             static_cast<long long>(p->position_ms_.load()), static_cast<long long>(t.submitted_ms.load()),
             static_cast<long long>(t.submitted_ms - p->position_ms_), static_cast<long long>(queued));
    } else if (status != esp::BUFFER_OVERRUN) {
        Track& t = type == esp::STREAM_VIDEO ? p->video_ : p->audio_;
        {
            std::lock_guard<std::mutex> lock(t.mutex);
            ++t.buffer_wake_seq;
        }
        t.cv.notify_all();
    }
}

void Player::on_eos(void* self) {
    static_cast<Player*>(self)->eos_ = true;
    OLOG("esp eos");
}

void Player::on_ready_to_prepare(esp::StreamType type, void* self) {
    auto* p = static_cast<Player*>(self);
    OLOG("esp ready_to_prepare(%s)", stream_name(type));
    Track& t = (type == esp::STREAM_VIDEO ? p->video_ : p->audio_);
    {
        std::lock_guard<std::mutex> lock(t.mutex);
        t.ready_to_feed = true;
        ++t.buffer_wake_seq;
    }
    t.cv.notify_all();
}

void Player::on_prepare_done(bool ok, void* self) {
    auto* p = static_cast<Player*>(self);
    OLOG("esp prepare_done ok=%d", ok ? 1 : 0);
    {
        std::lock_guard<std::mutex> lock(p->state_mutex_);
        (ok ? p->prepared_ : p->prepare_failed_) = true;
    }
    p->state_cv_.notify_all();
}

void Player::on_ready_to_seek(esp::StreamType type, uint64_t time_ms, void* self) {
    auto* p = static_cast<Player*>(self);
    OLOG("esp ready_to_seek(%s, %llu)", stream_name(type), static_cast<unsigned long long>(time_ms));
    Track& t = (type == esp::STREAM_VIDEO ? p->video_ : p->audio_);
    {
        std::lock_guard<std::mutex> lock(t.mutex);
        t.ready_to_feed = true;
        ++t.buffer_wake_seq;
    }
    t.cv.notify_all();
}

void Player::on_seek_done(void* self) {
    auto* p = static_cast<Player*>(self);
    p->seek_pending_ = false;
    for (Track* t : {&p->video_, &p->audio_}) {
        {
            std::lock_guard<std::mutex> lock(t->mutex);
            ++t->buffer_wake_seq;
        }
        t->cv.notify_all();
    }
    OLOG("esp seek_done");
}

void Player::on_resource_conflicted(void* self) {
    ++static_cast<Player*>(self)->errors_;
    OLOG("esp resource conflicted");
}

void Player::on_video_frame_dropped(uint64_t count, void* self) {
    static_cast<Player*>(self)->dropped_frames_ = static_cast<uint32_t>(std::min<uint64_t>(count, UINT32_MAX));
}

}  // namespace omega
