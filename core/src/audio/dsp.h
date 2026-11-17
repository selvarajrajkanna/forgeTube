#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "media/demuxer.h"

namespace omega::audio {

struct PcmFrame {
    std::vector<uint8_t> data;
    int64_t pts_ms = 0;
    int64_t duration_ms = 0;
};

// High-precision floating-point decode, polyphase sinc resampling (44.1 -> 48 kHz), linear
// loudness/headroom scaling, and fixed-20 ms PCM output pipeline. Keeps the source waveform
// strictly linear without dynamic compression, EQ coloration, or TV DSP effects.
class Pipeline {
public:
    Pipeline();
    ~Pipeline();
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    bool init(const media::TrackInfo& track, bool has_loudness_db, double loudness_db, int pcm_bits);
    bool reconfigure_bits(int pcm_bits);

    // Decodes one compressed audio packet. Set emit_output=false while priming after a seek so
    // MDCT overlap and the FIR resampler state stabilize before playback resumes.
    bool process(const uint8_t* data, size_t size, int64_t pts_ms, bool emit_output, std::vector<PcmFrame>& out);
    void flush(std::vector<PcmFrame>& out);
    void reset();

    int output_sample_rate() const;
    int output_channels() const;
    int source_sample_rate() const;
    int pcm_bits() const;
    double attenuation_db() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace omega::audio
