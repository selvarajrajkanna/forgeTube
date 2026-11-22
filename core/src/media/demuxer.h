#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

namespace omega::media {

enum class Codec { Unknown, H264, AAC, VP9, AV1, Opus };
enum class Container { Mp4, WebM };

const char* codec_name(Codec c);

struct TrackInfo {
    Container container = Container::Mp4;
    Codec codec = Codec::Unknown;
    uint64_t track_number = 0;  // WebM: blocks of other tracks are ignored
    uint32_t timescale = 0;  // ticks per second (WebM: 1e9 / TimecodeScale)
    int64_t media_time = 0;  // edit list shift, in track timescale
    std::vector<uint8_t> codec_data;  // avcC / av1C / vpcC payload, AudioSpecificConfig, or WebM CodecPrivate
    uint32_t width = 0, height = 0;
    uint32_t channels = 0, sample_rate = 0;
    uint32_t default_duration = 0, default_size = 0, default_flags = 0;  // from trex (WebM: DefaultDuration in ms)
    uint32_t fps_num = 0, fps_den = 0;  // rational video frame rate (e.g. 24000/1001, 24/1)

    // Colorimetry & HDR static metadata (parsed from 'colr'/'mdcv'/'clli' in MP4 or Colour in WebM).
    bool has_color = false;
    bool has_hdr = false;
    uint16_t color_primaries = 0;           // 1 = BT.709, 9 = BT.2020
    uint16_t transfer_characteristics = 0; // 1 = BT.709, 16 = SMPTE ST 2084 (PQ), 18 = ARIB STD-B67 (HLG)
    uint16_t matrix_coefficients = 0;      // 1 = BT.709, 9 = BT.2020 NCL
    uint8_t color_range = 0;               // Matroska Colour.Range: 0 = unspecified, 1 = limited (16-235), 2 = full (0-255)
    uint32_t bits_per_channel = 8;
    uint8_t chroma_siting_horz = 0;        // Matroska ChromaSitingHorz: 0 = unspec, 1 = left (co-sited), 2 = half
    uint8_t chroma_siting_vert = 0;        // Matroska ChromaSitingVert: 0 = unspec, 1 = top (co-sited), 2 = half

    bool has_mdcv = false;
    uint16_t display_primaries_x[3]{};     // G, B, R in 0.00002 units
    uint16_t display_primaries_y[3]{};
    uint16_t white_point_x = 0, white_point_y = 0;
    uint32_t max_luminance = 0, min_luminance = 0; // in 0.0001 cd/m² units

    bool has_clli = false;
    uint16_t max_cll = 0, max_fall = 0;    // in cd/m² (nits)
};

// Recompute has_hdr from container colorimetry/metadata so SDR 'colr'/'Colour' tags never masquerade as HDR.
inline void update_hdr_flag(TrackInfo& t) {
    t.has_hdr = (t.transfer_characteristics == 16 || t.transfer_characteristics == 18 ||
                 t.has_mdcv || (t.has_clli && (t.max_cll > 0 || t.max_fall > 0)));
}

// Sets t.fps_num / t.fps_den from a raw rational rate (e.g. timescale / sample_duration or 1e9 / duration_ns),
// snapping within ±0.005 fps to canonical cinema/broadcast fractions (24000/1001, 24/1, 25/1, 30000/1001, etc.).
inline void set_track_fps(TrackInfo& t, uint64_t num, uint64_t den) {
    if (num == 0 || den == 0) return;
    uint64_t g = std::gcd(num, den);
    num /= g;
    den /= g;
    struct Rate { uint32_t n, d; };
    static constexpr Rate kStandard[] = {
        {24000, 1001}, {24, 1}, {25000, 1001}, {25, 1}, {30000, 1001}, {30, 1},
        {48000, 1001}, {48, 1}, {50000, 1001}, {50, 1}, {60000, 1001}, {60, 1},
    };
    const double fps = static_cast<double>(num) / static_cast<double>(den);
    if (fps < 1.0 || fps > 240.0) return;
    for (const auto& r : kStandard) {
        const double std_fps = static_cast<double>(r.n) / static_cast<double>(r.d);
        if (std::abs(fps - std_fps) < 0.005) {
            t.fps_num = r.n;
            t.fps_den = r.d;
            return;
        }
    }
    if (num <= UINT32_MAX && den <= UINT32_MAX) {
        t.fps_num = static_cast<uint32_t>(num);
        t.fps_den = static_cast<uint32_t>(den);
    } else {
        t.fps_num = static_cast<uint32_t>(fps + 0.5);
        t.fps_den = 1;
    }
}

// One entry of the stream index (sidx / Cues): a self-contained fragment starting with a keyframe.
struct Segment {
    uint64_t offset;      // absolute byte offset of the segment (moof / Cluster)
    int64_t start_ms;     // presentation time of its first sample
    uint64_t start_dts;   // decode time of its first sample, in track timescale (MP4 only)
};

// Recycled fixed-size slab pool for demuxed frame payloads. Eliminates ~110 cross-thread
// malloc/free calls per second during 4K60 playback and prevents glibc arena fragmentation.
class SamplePool : public std::enable_shared_from_this<SamplePool> {
public:
    static constexpr size_t kDefaultSlabBytes = 512 * 1024;
    static constexpr size_t kDefaultMaxCached = 16;

    struct Slab {
        std::unique_ptr<uint8_t[]> mem;
        size_t cap = 0;
        size_t used = 0;
    };

    explicit SamplePool(size_t slab_bytes = kDefaultSlabBytes, size_t max_cached = kDefaultMaxCached)
        : slab_bytes_(slab_bytes), max_cached_(max_cached) {}

    // Single-producer bump allocation (called from demux_thread); slabs recycle from any thread (feed_thread).
    std::pair<std::shared_ptr<Slab>, size_t> alloc(size_t n) {
        if (n == 0) return {{}, 0};
        const size_t aligned = (n + 15u) & ~size_t{15};
        if (aligned > slab_bytes_) {
            auto slab = std::make_shared<Slab>();
            slab->mem = std::make_unique<uint8_t[]>(n);
            slab->cap = n;
            slab->used = n;
            return {std::move(slab), 0};
        }
        if (!cur_ || cur_->used + aligned > cur_->cap) {
            cur_ = acquire_slab();
        }
        size_t off = cur_->used;
        cur_->used += aligned;
        return {cur_, off};
    }

    void reset_active() { cur_.reset(); }

private:
    std::shared_ptr<Slab> acquire_slab() {
        Slab* raw = nullptr;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!free_slabs_.empty()) {
                raw = free_slabs_.back().release();
                free_slabs_.pop_back();
            }
        }
        if (!raw) {
            raw = new Slab();
            raw->mem = std::make_unique<uint8_t[]>(slab_bytes_);
            raw->cap = slab_bytes_;
        }
        raw->used = 0;
        std::weak_ptr<SamplePool> weak = weak_from_this();
        return std::shared_ptr<Slab>(raw, [weak](Slab* s) {
            if (auto self = weak.lock()) {
                self->recycle(s);
            } else {
                delete s;
            }
        });
    }

    void recycle(Slab* s) {
        std::lock_guard<std::mutex> lock(mu_);
        if (s->cap == slab_bytes_ && free_slabs_.size() < max_cached_) {
            free_slabs_.emplace_back(s);
            return;
        }
        delete s;
    }

    size_t slab_bytes_;
    size_t max_cached_;
    std::shared_ptr<Slab> cur_;
    std::mutex mu_;
    std::vector<std::unique_ptr<Slab>> free_slabs_;
};

// Contiguous slice of a recycled SamplePool slab (falls back to standalone buffer when no pool is attached).
struct SampleBuffer {
    std::shared_ptr<SamplePool::Slab> slab;
    size_t offset = 0;
    size_t len = 0;

    uint8_t* data() { return slab ? slab->mem.get() + offset : nullptr; }
    const uint8_t* data() const { return slab ? slab->mem.get() + offset : nullptr; }
    size_t size() const { return len; }
    bool empty() const { return len == 0; }

    void allocate(size_t n, SamplePool* pool = nullptr) {
        len = n;
        if (n == 0) {
            slab.reset();
            offset = 0;
            return;
        }
        if (pool) {
            auto [s, off] = pool->alloc(n);
            slab = std::move(s);
            offset = off;
        } else {
            slab = std::make_shared<SamplePool::Slab>();
            slab->mem = std::make_unique<uint8_t[]>(n);
            slab->cap = n;
            slab->used = n;
            offset = 0;
        }
    }

    void assign(const uint8_t* src, const uint8_t* end, SamplePool* pool = nullptr) {
        size_t n = static_cast<size_t>(end - src);
        allocate(n, pool);
        if (n > 0) std::memcpy(data(), src, n);
    }
};

struct Sample {
    SampleBuffer data;
    int64_t pts_ms = 0;
    int64_t duration_ms = 0;
    bool key = false;
};

// Sequential reader for a single-track DASH stream on a forward-only byte source.
class Demuxer {
public:
    using ReadFn = std::function<size_t(void*, size_t)>;
    // Optional incremental batch sink during read_fragment(). Returning false aborts the fragment early (seek/stop).
    using BatchCb = std::function<bool(std::deque<Sample>&)>;
    virtual ~Demuxer() = default;

    // Reads the init segment and the stream index that follows it, when present.
    virtual bool read_header() = 0;
    const std::vector<Segment>& segments() const { return segments_; }
    std::vector<Segment> take_segments() { return std::move(segments_); }
    // Optional: emit demuxed samples in small batches while streaming a fragment instead of waiting for EOF of the fragment.
    void set_batch_cb(BatchCb cb) { batch_cb_ = std::move(cb); }
    // Optional: recycle frame payload memory from a slab pool instead of per-sample heap allocations.
    void set_sample_pool(std::shared_ptr<SamplePool> pool) { pool_ = std::move(pool); }
    // Appends the samples of the next fragment. Returns false on EOF or error.
    virtual bool read_fragment(std::deque<Sample>& out) = 0;

    const TrackInfo& track() const { return track_; }
    const std::string& error() const { return error_; }

protected:
    TrackInfo track_;
    std::string error_;
    std::vector<Segment> segments_;
    BatchCb batch_cb_;
    std::shared_ptr<SamplePool> pool_;
};

// Sniffs the container (MP4 or WebM) from the first bytes and returns the matching reader.
std::unique_ptr<Demuxer> open_demuxer(Demuxer::ReadFn read);
// Resumes at a segment boundary (after a seek) of a stream whose header was already parsed.
std::unique_ptr<Demuxer> resume_demuxer(Demuxer::ReadFn read, const TrackInfo& track, const Segment& segment);

}  // namespace omega::media
