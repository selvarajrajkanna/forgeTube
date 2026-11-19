#pragma once

#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <numeric>
#include <string>
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

struct Sample {
    std::vector<uint8_t> data;
    int64_t pts_ms = 0;
    int64_t duration_ms = 0;
    bool key = false;
};

// Sequential reader for a single-track DASH stream on a forward-only byte source.
class Demuxer {
public:
    using ReadFn = std::function<size_t(void*, size_t)>;
    virtual ~Demuxer() = default;

    // Reads the init segment and the stream index that follows it, when present.
    virtual bool read_header() = 0;
    const std::vector<Segment>& segments() const { return segments_; }
    // Appends the samples of the next fragment. Returns false on EOF or error.
    virtual bool read_fragment(std::deque<Sample>& out) = 0;

    const TrackInfo& track() const { return track_; }
    const std::string& error() const { return error_; }

protected:
    TrackInfo track_;
    std::string error_;
    std::vector<Segment> segments_;
};

// Sniffs the container (MP4 or WebM) from the first bytes and returns the matching reader.
std::unique_ptr<Demuxer> open_demuxer(Demuxer::ReadFn read);
// Resumes at a segment boundary (after a seek) of a stream whose header was already parsed.
std::unique_ptr<Demuxer> resume_demuxer(Demuxer::ReadFn read, const TrackInfo& track, const Segment& segment);

}  // namespace omega::media
