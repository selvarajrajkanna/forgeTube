#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "audio/dsp.h"
#include "media/demuxer.h"
#include "net/http.h"
#include "player/esplayer_api.h"

namespace omega {

struct PlayerConfig {
    void* window = nullptr;  // Ecore_Wl2_Window*
    int width = 1920, height = 1080;
    std::string video_url, audio_url;
    net::Headers stream_headers;      // sent with every media request (e.g. YouTube User-Agent)
    int fps = 30;
    int max_width = 3840, max_height = 2160;
    int64_t audio_bitrate = 128000;
    int64_t duration_ms = 0;          // 0: derive from the stream index
    int64_t start_ms = 0;             // resume position (set before prepare: no flash of the beginning)
    // Live broadcast: the URLs are numbered-segment streams (net::LiveReader); no index, no seek.
    bool live = false;
    int live_behind_segments = 3;     // start this many segments (~5 s each) behind the live edge
    // Re-resolves expired stream URLs (YouTube URLs live ~6 h). Optional.
    std::function<bool(std::string& video_url, std::string& audio_url)> refresh_urls;
    std::string app_id, app_version;  // reported to the TV resource manager
    std::string app_type = "MSE";     // empty: skip esplusplayer_set_app_info
    std::string app_runtitle;         // optional runtitle passed via esplusplayer_set_app_info_ex
    bool use_video = true, use_audio = true, use_display = true;
    bool audio_dsp = true;          // decode Opus/AAC to float, sync-resample to 48 kHz, linear loudness -> PCM
    bool has_audio_loudness = false;
    double audio_loudness_db = 0.0; // YouTube audioConfig.loudnessDb relative to -14 LUFS
    int64_t audio_delay_ms = 0;     // initial A/V sync offset applied from Packet 0
    bool is_hdr = false;
    int hdr_mode = 0;               // 0 None, 1 HDR10, 2 HLG
    int64_t max_queue_ms = 30000;   // demuxed read-ahead per stream (network side)
    size_t max_queue_bytes = 48u << 20;  // ...and its memory cap (4K VP9 runs at 20-40 Mb/s)
    int64_t feed_ahead_ms = 15000;  // how far ahead of the playback clock we submit to ESPlayer
};

struct PlayerStats {
    int state;                   // esp::State
    int64_t position_ms;
    int64_t video_queued_ms, audio_queued_ms;
    int64_t video_submitted_ms, audio_submitted_ms;
    uint32_t video_underruns, audio_underruns;
    uint32_t stalls;             // playback clock stopped advancing while PLAYING
    int64_t longest_stall_ms;
    uint32_t errors;
    int eos;
    int64_t duration_ms;
    int reconnecting;  // a stream is retrying after a network error
    uint32_t dropped_frames;
    int32_t scaler_id;
    int32_t scaler_rc;
    int32_t dsp_bits;            // 32/24/16 PCM output, or 0 for TV HW passthrough
    int32_t dsp_hf;              // 0 off/bypass, 1 16 kHz->20 kHz, 2 12 kHz SWB->20 kHz
    uint32_t video_bitrate_kbps; // rolling 1-second encoded video bitrate (kb/s)
};

// Plays one DASH video track + one audio track (fragmented MP4 or WebM over HTTP) with ESPlayer.
class Player {
public:
    using Clock = std::chrono::steady_clock;
    explicit Player(PlayerConfig cfg);
    ~Player();

    bool start();
    void toggle_pause();
    // Jumps to the keyframe segment at or before `ms`. Returns false if not seekable (yet).
    bool seek(int64_t ms);
    bool set_rate(double rate);
    void set_audio_delay_ms(int64_t ms) { audio_delay_ms_ = ms; }
    PlayerStats stats();
    const media::TrackInfo& video_info() const { return video_.info; }
    bool progressive_scan() const { return progressive_scan_; }
    int tier_width() const { return cfg_.max_width; }
    int tier_height() const { return cfg_.max_height; }
    const std::string& error() const { return error_; }
    // start() failed because YouTube kept refusing a stream (403/410), not a network or codec issue.
    bool stream_refused() const { return stream_refused_; }

private:
    struct Track {
        esp::StreamType type;
        std::string url;
        media::TrackInfo info;
        std::mutex mutex;
        std::condition_variable cv;
        std::deque<media::Sample> queue;
        int64_t queued_ms = 0;
        size_t queued_bytes = 0;
        bool demux_done = false;
        std::atomic<bool> ready_to_feed{false};
        std::atomic<bool> packet_fed{false};
        std::atomic<bool> active{true};  // cleared to stop this track's threads (seek)
        std::atomic<bool> reconnecting{false};
        std::vector<media::Segment> segments;
        std::atomic<int64_t> submitted_ms{0};
        std::atomic<uint32_t> underruns{0};
        std::atomic<uint64_t> bytes{0};
        std::thread demux_thread, feed_thread;
    };

    void demux_loop(Track& t, media::Demuxer* reader, net::StreamSource* source);
    void start_track(Track& t, media::Demuxer* reader, net::StreamSource* source);
    void stop_track(Track& t);
    void attach_hooks(Track& t, net::StreamSource& source);
    void push_samples(Track& t, std::deque<media::Sample>& samples);
    std::string fresh_url(Track& t);
    bool submit_es_packet(Track& t, esp::StreamType type, const uint8_t* data, size_t size, int64_t pts_ms,
                          int64_t duration_ms);
    void teardown_esp();
    void feed_loop(Track& t);
    void control_loop();
    void notify_track(Track& t);
    void mark_track_fed(Track& t);
    void confirm_active_dsp();

    static void on_error(int err, void* self);
    static void on_buffer_status(esp::StreamType type, esp::BufferStatus status, void* self);
    static void on_eos(void* self);
    static void on_ready_to_prepare(esp::StreamType type, void* self);
    static void on_prepare_done(bool ok, void* self);
    static void on_resource_conflicted(void* self);
    static void on_ready_to_seek(esp::StreamType type, uint64_t time_ms, void* self);
    static void on_seek_done(void* self);
    static void on_video_frame_dropped(uint64_t count, void* self);

    PlayerConfig cfg_;
    const esp::Api* api_ = nullptr;
    esp::Handle handle_ = nullptr;
    void* sound_stream_info_ = nullptr;
    std::string error_;
    bool stream_refused_ = false;
    bool progressive_scan_ = false;

    bool has_color_info_ = false;
    esp::ESMatroskaColor color_info_{};
    std::unique_ptr<audio::Pipeline> audio_dsp_;

    Track video_, audio_;
    std::atomic<bool> running_{false};
    std::atomic<bool> prepared_{false}, prepare_failed_{false}, paused_{false}, eos_{false}, dsp_confirmed_{false};
    std::mutex state_mutex_;
    std::condition_variable state_cv_;
    std::atomic<uint32_t> errors_{0}, stalls_{0}, dropped_frames_{0}, video_bitrate_kbps_{0};
    std::atomic<int32_t> scaler_id_{-2};    // -2: unqueried, -1: unavailable, >=0: RM virtual id
    std::atomic<int32_t> scaler_rc_{-999};  // esplusplayer_get_virtual_rsc_id return code
    std::atomic<int64_t> longest_stall_ms_{0}, position_ms_{0};
    std::thread control_thread_;
    std::mutex seek_mutex_;
    std::atomic<int64_t> feed_from_ms_{0};  // audio samples before this are dropped after a seek
    std::atomic<int64_t> duration_ms_{0};
    int64_t live_base_ms_ = 0;  // live: absolute broadcast time of our playback position 0
    std::atomic<int64_t> stall_grace_until_ms_{0};
    std::atomic<bool> seek_pending_{false};
    std::atomic<int64_t> audio_delay_ms_{0};
    std::mutex refresh_mutex_;
    Clock::time_point last_refresh_{};
    std::atomic<int64_t> started_at_ms_{0};
};

}  // namespace omega
