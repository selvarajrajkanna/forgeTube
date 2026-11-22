#include "audio/dsp.h"

#include <opus.h>
#include <neaacdec.h>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <utility>

#include "util/log.h"

namespace omega::audio {
namespace {

constexpr int kTargetRate = 48000;
constexpr int kTargetChannels = 2;
constexpr int kFrameSamples = 960;        // 20 ms exact at 48 kHz
constexpr int kHalfTaps = 80;             // 160-tap Kaiser sinc (~125 dB stopband, flat to 20 kHz)
constexpr int kTaps = kHalfTaps * 2;
static_assert(kTaps % 8 == 0, "kTaps must be a multiple of 8 for 2x4-wide NEON MAC");
constexpr double kKaiserBeta = 12.6;
constexpr double kSincCutoff = 0.479;
constexpr double kPi = 3.14159265358979323846;

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
inline float32x4_t neon_fma(float32x4_t acc, float32x4_t a, float32x4_t b) {
#if defined(__ARM_FEATURE_FMA)
    return vfmaq_f32(acc, a, b);
#else
    return vmlaq_f32(acc, a, b);
#endif
}

inline float32x4_t neon_fnma(float32x4_t acc, float32x4_t a, float32x4_t b) {
#if defined(__ARM_FEATURE_FMA)
    return vfmsq_f32(acc, a, b);
#else
    return vmlsq_f32(acc, a, b);
#endif
}

// 32-bit ARMv8 + AArch64 compatible horizontal sum (avoids AArch64-only vaddvq_f32).
inline float neon_hsum(float32x4_t v) {
    float32x2_t pair = vadd_f32(vget_low_f32(v), vget_high_f32(v));
    pair = vpadd_f32(pair, pair);
    return vget_lane_f32(pair, 0);
}

inline float32x4_t neon_rev4(float32x4_t v) {
    const float32x4_t r = vrev64q_f32(v);
    return vcombine_f32(vget_high_f32(r), vget_low_f32(r));
}
#endif

// Branchless IEEE-754 log2f for spectral envelope / SFM / dB math (~0.008 log2 ≈ 0.025 dB max error, monotonic).
// Eliminates softfp libm calls on 32-bit ARM; never used on PCM audio samples.
inline float fast_log2f(float x) {
    const uint32_t bits = std::bit_cast<uint32_t>(x);
    const int32_t exp = static_cast<int32_t>((bits >> 23) & 0xFFu) - 127;
    const float m = std::bit_cast<float>((bits & 0x007FFFFFu) | 0x3F800000u);  // [1.0, 2.0)
    const float poly = (-0.34484843f * m + 2.02466578f) * m - 1.67487759f;
    return static_cast<float>(exp) + poly;
}

double bessel_i0(double x) {
    double sum = 1.0;
    double term = 1.0;
    const double half_x = 0.5 * x;
    for (int k = 1; k <= 40; ++k) {
        const double f = half_x / static_cast<double>(k);
        term *= f * f;
        sum += term;
        if (term < sum * 1e-16) break;
    }
    return sum;
}

uint16_t le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

int16_t sle16(const uint8_t* p) {
    return static_cast<int16_t>(le16(p));
}

// Preserves 100% linearity inside [-0.995, +0.995]; softly rolls off only rare spikes near full scale.
double guard_peak(double v) {
    const double av = std::abs(v);
    if (av <= 0.995) return v;
    const double shaped = 0.995 + 0.005 * std::tanh((av - 0.995) / 0.005);
    return v < 0.0 ? -shaped : shaped;
}

// Stereo polyphase Kaiser-windowed sinc resampler targeting 48 kHz.
// Kernel is designed in double precision (~125 dB stopband) and convolved in float32 (NEON-accelerated
// on ARMv8, ~136 dB MAC floor vs 144 dB float32 decoder output). Bypasses completely at 48 kHz.
class SincResampler {
public:
    void configure(int src_rate) {
        if (src_rate <= 0) src_rate = kTargetRate;
        if (src_rate == src_rate_) return;
        src_rate_ = src_rate;
        bypass_ = (src_rate_ == kTargetRate);
        if (bypass_) {
            coeffs_.clear();
            in_l_.clear();
            in_r_.clear();
            return;
        }
        const int g = std::gcd(src_rate_, kTargetRate);
        const int ratio_up = kTargetRate / g;
        const int ratio_down = src_rate_ / g;
        rational_ = (ratio_up <= 512);
        up_ = rational_ ? static_cast<size_t>(ratio_up) : 512u;
        down_ = rational_ ? static_cast<size_t>(ratio_down) : 0u;
        step_double_ = static_cast<double>(src_rate_) / static_cast<double>(kTargetRate);

        build_kernel();
        reset();
    }

    void reset() {
        phase_num_ = 0;
        phase_pos_ = 0.0;
        if (bypass_) {
            in_l_.clear();
            in_r_.clear();
            return;
        }
        in_l_.assign(static_cast<size_t>(kHalfTaps - 1), 0.0f);
        in_r_.assign(static_cast<size_t>(kHalfTaps - 1), 0.0f);
    }

    int source_rate() const { return src_rate_; }
    bool configured() const { return src_rate_ > 0; }
    bool active() const { return !bypass_; }

    void process(const float* stereo_in, size_t in_frames, bool emit, std::vector<float>& out) {
        if (in_frames == 0) return;
        if (bypass_) {
            if (emit) out.insert(out.end(), stereo_in, stereo_in + in_frames * kTargetChannels);
            return;
        }
        const size_t base = in_l_.size();
        in_l_.resize(base + in_frames);
        in_r_.resize(base + in_frames);
        float* __restrict dl = in_l_.data() + base;
        float* __restrict dr = in_r_.data() + base;
        size_t i = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
        for (; i + 4 <= in_frames; i += 4) {
            const float32x4x2_t lr = vld2q_f32(stereo_in + 2 * i);
            vst1q_f32(dl + i, lr.val[0]);
            vst1q_f32(dr + i, lr.val[1]);
        }
#endif
        for (; i < in_frames; ++i) {
            dl[i] = stereo_in[2 * i];
            dr[i] = stereo_in[2 * i + 1];
        }
        run_available(emit, out);
    }

    void flush(std::vector<float>& out) {
        if (bypass_ || in_l_.empty()) return;
        in_l_.insert(in_l_.end(), static_cast<size_t>(kHalfTaps), 0.0f);
        in_r_.insert(in_r_.end(), static_cast<size_t>(kHalfTaps), 0.0f);
        run_available(true, out);
    }

private:
    void build_kernel() {
        const double cutoff = std::min(kSincCutoff, kSincCutoff * static_cast<double>(kTargetRate) / src_rate_);
        const double i0_beta = bessel_i0(kKaiserBeta);
        const size_t num_rows = rational_ ? up_ : (up_ + 1);
        coeffs_.assign(num_rows * kTaps, 0.0f);
        std::array<double, kTaps> tmp{};

        for (size_t p = 0; p < num_rows; ++p) {
            const double frac = static_cast<double>(p) / static_cast<double>(up_);
            float* row = &coeffs_[p * kTaps];
            double norm = 0.0;
            tmp.fill(0.0);
            for (int k = 0; k < kTaps; ++k) {
                const int n = k - (kHalfTaps - 1);
                const double d = frac - static_cast<double>(n);
                const double win_x = d / static_cast<double>(kHalfTaps);
                if (std::abs(win_x) >= 1.0) continue;
                const double w = bessel_i0(kKaiserBeta * std::sqrt(1.0 - win_x * win_x)) / i0_beta;
                const double sinc = (std::abs(d) < 1e-12)
                                        ? (2.0 * cutoff)
                                        : (std::sin(2.0 * kPi * cutoff * d) / (kPi * d));
                const double h = sinc * w;
                tmp[k] = h;
                norm += h;
            }
            if (norm > 0.0) {
                const double inv = 1.0 / norm;
                for (int k = 0; k < kTaps; ++k) row[k] = static_cast<float>(tmp[k] * inv);
            }
        }
    }

    void run_available(bool emit, std::vector<float>& out) {
        const size_t total_frames = in_l_.size();
        if (total_frames < static_cast<size_t>(kTaps)) return;
        const size_t base_out = out.size();
        size_t written = base_out;
        if (emit) {
            const size_t avail = total_frames - static_cast<size_t>(kTaps) + 1;
            const size_t est_out =
                rational_ ? ((avail * up_ + std::max<size_t>(1, down_) - 1) / std::max<size_t>(1, down_) + 2)
                          : (static_cast<size_t>(static_cast<double>(avail) / std::max(step_double_, 1e-6)) + 4);
            out.resize(base_out + est_out * kTargetChannels);
        }
        size_t consumed = 0;
        while (consumed + static_cast<size_t>(kTaps) <= total_frames) {
            if (emit) {
                const float* __restrict xl = in_l_.data() + consumed;
                const float* __restrict xr = in_r_.data() + consumed;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
                // 2-sample fused rational NEON path: when consecutive output samples share input samples
                // (adv1 == 0 or 1, true for 100% of 44.1->48 kHz), shift xl/xr in registers via vextq_f32
                // to cut A72 Load-pipe traffic by ~32.5% (162 vs 240 loads per 2 output frames, 12 q-regs).
                if (rational_) {
                    const size_t next_p = phase_num_ + down_;
                    const size_t adv1 = next_p / up_;
                    if (adv1 <= 1 && consumed + adv1 + static_cast<size_t>(kTaps) <= total_frames) {
                        const size_t phase_b = next_p % up_;
                        const float* __restrict ha = &coeffs_[phase_num_ * kTaps];
                        const float* __restrict hb = &coeffs_[phase_b * kTaps];
                        float32x4_t al = vdupq_n_f32(0.0f);
                        float32x4_t ar = vdupq_n_f32(0.0f);
                        float32x4_t bl = vdupq_n_f32(0.0f);
                        float32x4_t br = vdupq_n_f32(0.0f);
                        if (adv1 == 0) {
                            for (int k = 0; k < kTaps; k += 4) {
                                const float32x4_t xl0 = vld1q_f32(xl + k);
                                const float32x4_t xr0 = vld1q_f32(xr + k);
                                const float32x4_t ta = vld1q_f32(ha + k);
                                const float32x4_t tb = vld1q_f32(hb + k);
                                al = neon_fma(al, ta, xl0);
                                ar = neon_fma(ar, ta, xr0);
                                bl = neon_fma(bl, tb, xl0);
                                br = neon_fma(br, tb, xr0);
                            }
                        } else {
                            float32x4_t xl_cur = vld1q_f32(xl);
                            float32x4_t xr_cur = vld1q_f32(xr);
                            for (int k = 0; k < kTaps - 4; k += 4) {
                                const float32x4_t xl_nxt = vld1q_f32(xl + k + 4);
                                const float32x4_t xr_nxt = vld1q_f32(xr + k + 4);
                                const float32x4_t ta = vld1q_f32(ha + k);
                                const float32x4_t tb = vld1q_f32(hb + k);
                                al = neon_fma(al, ta, xl_cur);
                                ar = neon_fma(ar, ta, xr_cur);
                                bl = neon_fma(bl, tb, vextq_f32(xl_cur, xl_nxt, 1));
                                br = neon_fma(br, tb, vextq_f32(xr_cur, xr_nxt, 1));
                                xl_cur = xl_nxt;
                                xr_cur = xr_nxt;
                            }
                            const float32x4_t ta = vld1q_f32(ha + (kTaps - 4));
                            const float32x4_t tb = vld1q_f32(hb + (kTaps - 4));
                            al = neon_fma(al, ta, xl_cur);
                            ar = neon_fma(ar, ta, xr_cur);
                            bl = neon_fma(bl, tb, vld1q_f32(xl + (kTaps - 3)));
                            br = neon_fma(br, tb, vld1q_f32(xr + (kTaps - 3)));
                        }
                        if (written + 4 > out.size()) out.resize(out.size() + 64);
                        out[written++] = neon_hsum(al);
                        out[written++] = neon_hsum(ar);
                        out[written++] = neon_hsum(bl);
                        out[written++] = neon_hsum(br);
                        const size_t next_p2 = phase_b + down_;
                        consumed += adv1 + next_p2 / up_;
                        phase_num_ = next_p2 % up_;
                        continue;
                    }
                }
#endif
                float out_l = 0.0f;
                float out_r = 0.0f;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
                // Planar 2x4-wide NEON FMA (8 stereo taps/iter) uses 1-uop vld1q_f32 loads on A72's Load pipe.
                float32x4_t acc_l0 = vdupq_n_f32(0.0f);
                float32x4_t acc_r0 = vdupq_n_f32(0.0f);
                float32x4_t acc_l1 = vdupq_n_f32(0.0f);
                float32x4_t acc_r1 = vdupq_n_f32(0.0f);
                if (rational_) {
                    const float* __restrict h = &coeffs_[phase_num_ * kTaps];
                    for (int k = 0; k < kTaps; k += 8) {
                        const float32x4_t t0 = vld1q_f32(h + k);
                        const float32x4_t t1 = vld1q_f32(h + k + 4);
                        acc_l0 = neon_fma(acc_l0, t0, vld1q_f32(xl + k));
                        acc_r0 = neon_fma(acc_r0, t0, vld1q_f32(xr + k));
                        acc_l1 = neon_fma(acc_l1, t1, vld1q_f32(xl + k + 4));
                        acc_r1 = neon_fma(acc_r1, t1, vld1q_f32(xr + k + 4));
                    }
                } else {
                    const double scaled = phase_pos_ * static_cast<double>(up_);
                    const size_t p0 = std::min<size_t>(static_cast<size_t>(scaled), up_ - 1);
                    const float32x4_t alpha_v = vdupq_n_f32(static_cast<float>(scaled - static_cast<double>(p0)));
                    const float* __restrict h0 = &coeffs_[p0 * kTaps];
                    const float* __restrict h1 = &coeffs_[(p0 + 1) * kTaps];
                    for (int k = 0; k < kTaps; k += 8) {
                        const float32x4_t a0 = vld1q_f32(h0 + k);
                        const float32x4_t a1 = vld1q_f32(h0 + k + 4);
                        const float32x4_t t0 = neon_fma(a0, alpha_v, vsubq_f32(vld1q_f32(h1 + k), a0));
                        const float32x4_t t1 = neon_fma(a1, alpha_v, vsubq_f32(vld1q_f32(h1 + k + 4), a1));
                        acc_l0 = neon_fma(acc_l0, t0, vld1q_f32(xl + k));
                        acc_r0 = neon_fma(acc_r0, t0, vld1q_f32(xr + k));
                        acc_l1 = neon_fma(acc_l1, t1, vld1q_f32(xl + k + 4));
                        acc_r1 = neon_fma(acc_r1, t1, vld1q_f32(xr + k + 4));
                    }
                }
                out_l = neon_hsum(vaddq_f32(acc_l0, acc_l1));
                out_r = neon_hsum(vaddq_f32(acc_r0, acc_r1));
#else
                float acc_l0 = 0.0f, acc_l1 = 0.0f;
                float acc_r0 = 0.0f, acc_r1 = 0.0f;
                if (rational_) {
                    const float* __restrict h = &coeffs_[phase_num_ * kTaps];
                    for (int k = 0; k < kTaps; k += 2) {
                        const float t0 = h[k];
                        const float t1 = h[k + 1];
                        acc_l0 += t0 * xl[k];
                        acc_r0 += t0 * xr[k];
                        acc_l1 += t1 * xl[k + 1];
                        acc_r1 += t1 * xr[k + 1];
                    }
                } else {
                    const double scaled = phase_pos_ * static_cast<double>(up_);
                    const size_t p0 = std::min<size_t>(static_cast<size_t>(scaled), up_ - 1);
                    const float alpha = static_cast<float>(scaled - static_cast<double>(p0));
                    const float* __restrict h0 = &coeffs_[p0 * kTaps];
                    const float* __restrict h1 = &coeffs_[(p0 + 1) * kTaps];
                    for (int k = 0; k < kTaps; k += 2) {
                        const float t0 = h0[k] + alpha * (h1[k] - h0[k]);
                        const float t1 = h0[k + 1] + alpha * (h1[k + 1] - h0[k + 1]);
                        acc_l0 += t0 * xl[k];
                        acc_r0 += t0 * xr[k];
                        acc_l1 += t1 * xl[k + 1];
                        acc_r1 += t1 * xr[k + 1];
                    }
                }
                out_l = acc_l0 + acc_l1;
                out_r = acc_r0 + acc_r1;
#endif
                if (written + 2 > out.size()) out.resize(out.size() + 64);
                out[written++] = out_l;
                out[written++] = out_r;
            }

            if (rational_) {
                phase_num_ += down_;
                consumed += phase_num_ / up_;
                phase_num_ %= up_;
            } else {
                phase_pos_ += step_double_;
                const size_t adv = static_cast<size_t>(std::floor(phase_pos_));
                phase_pos_ -= static_cast<double>(adv);
                consumed += adv;
            }
        }
        if (emit) out.resize(written);
        if (consumed > 0) {
            in_l_.erase(in_l_.begin(), in_l_.begin() + static_cast<std::ptrdiff_t>(consumed));
            in_r_.erase(in_r_.begin(), in_r_.begin() + static_cast<std::ptrdiff_t>(consumed));
        }
    }

    int src_rate_ = 0;
    bool bypass_ = true;
    bool rational_ = true;
    size_t up_ = 1;
    size_t down_ = 1;
    size_t phase_num_ = 0;
    double step_double_ = 1.0;
    double phase_pos_ = 0.0;
    std::vector<float> coeffs_;
    std::vector<float> in_l_;
    std::vector<float> in_r_;
};

// Signal-gated, dry-preserving Hybrid Spectral Band Extender at 48 kHz (187.5 Hz/bin, 256-pt 75% WOLA, 4.0 ms latency).
// Only injects subtle high-frequency air when a sharp codec brickwall is measured (16 kHz AAC / baked Opus, or
// 12 kHz SWB Opus). Combines full-spectrum macro-tilt conditioning + encoder transition-droop recovery with
// energy-invariant, stereo-locked harmonic + quadrature diffuse-air synthesis, power-complementary existing-HF
// capping, spectral-flatness tonality damping, HF transient flux weighting, and 6-10 kHz speech sibilance ducking.
// Everything below the codec cutoff stays 100% dry and phase-untouched.
class HfExtender {
public:
    static constexpr int kLog2Fft = 8;
    static constexpr int kFft = 1 << kLog2Fft;  // 256 samples = 5.333 ms window for tight transient response
    static constexpr int kHop = kFft / 4;       // 64 samples = 1.333 ms hop (75% COLA overlap)
    static constexpr int kDelay = kFft - kHop;  // 192 samples = 4.000 ms exact COLA alignment
    static constexpr int kNumSub = 4;
    static constexpr int kStageTwSize = kFft - 4;  // 4 + 8 + 16 + 32 + 64 + 128 = 252 contiguous twiddles

    HfExtender() {
        for (int i = 0; i < kFft; ++i) {
            hann_[i] =
                static_cast<float>(0.5 * (1.0 - std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(kFft))));
        }
        for (int i = 0; i < kFft; ++i) {
            int rev = 0;
            for (int b = 0; b < kLog2Fft; ++b) {
                if ((i >> b) & 1) rev |= (1 << (kLog2Fft - 1 - b));
            }
            bitrev_[i] = rev;
        }
        for (int half = 4; half <= kFft / 2; half <<= 1) {
            const int offset = half - 4;
            const double step = -kPi / static_cast<double>(half);
            for (int k = 0; k < half; ++k) {
                const double ang = step * static_cast<double>(k);
                const float c = static_cast<float>(std::cos(ang));
                const float s = static_cast<float>(std::sin(ang));
                stage_tw_re_[offset + k] = c;
                stage_tw_im_[offset + k] = s;
                stage_tw_im_inv_[offset + k] = -s;
            }
        }
        reset_all();
    }

    void reset_all() {
        p_low_hi_ema_ = 0.0f;
        p_pre16_ema_ = 0.0f;
        p_post16_ema_ = 0.0f;
        sib_slow_ema_ = 0.0f;
        ema_initialized_ = false;
        brickwall_16_ = false;
        blend_ = 0.0f;
        mode12_blend_ = 0.0f;
        idle_probe_count_ = 0;
        logged_state_ = -1;
        reset_buffers();
    }

    void reset_buffers() {
        in_l_.fill(0.0f);
        in_r_.fill(0.0f);
        ola_l_.fill(0.0f);
        ola_r_.fill(0.0f);
        smooth_g16_.fill(0.0f);
        smooth_g12a_.fill(0.0f);
        smooth_g12b_.fill(0.0f);
        ring_head_ = 0;
        hop_fill_ = 0;
        hop_idx_ = 0;
        ola_tail_hops_ = 0;
        priming_skip_ = kDelay;
        sib_duck_ = 1.0f;
        tonal_guard_ = 1.0f;
        src_pwr_ema_ = 0.0f;
        trans_boost_ = 1.0f;
        rng_state_ = 0x9E3779B9u;
    }

    int active_mode() const { return (blend_ > 0.05f) ? (mode12_blend_ > 0.5f ? 2 : 1) : 0; }

    void process(const float* stereo_in, size_t in_frames, bool enabled, bool swb_hint, bool emit,
                 std::vector<float>& out) {
        if (in_frames == 0) return;
        size_t i = 0;
        while (i < in_frames) {
            const int take = static_cast<int>(std::min<size_t>(in_frames - i, static_cast<size_t>(kHop - hop_fill_)));
            // ring_head_ is always a multiple of kHop (64), so [dst_base .. dst_base + take) never wraps past kFft.
            const int dst_base = (ring_head_ + kDelay + hop_fill_) & (kFft - 1);
            float* __restrict dl0 = in_l_.data() + dst_base;
            float* __restrict dl1 = dl0 + kFft;
            float* __restrict dr0 = in_r_.data() + dst_base;
            float* __restrict dr1 = dr0 + kFft;
            const float* __restrict src = stereo_in + 2 * i;
            int k = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
            for (; k + 4 <= take; k += 4) {
                const float32x4x2_t lr = vld2q_f32(src + 2 * k);
                vst1q_f32(dl0 + k, lr.val[0]);
                vst1q_f32(dl1 + k, lr.val[0]);
                vst1q_f32(dr0 + k, lr.val[1]);
                vst1q_f32(dr1 + k, lr.val[1]);
            }
#endif
            for (; k < take; ++k) {
                const float l = src[2 * k];
                const float r = src[2 * k + 1];
                dl0[k] = dl1[k] = l;
                dr0[k] = dr1[k] = r;
            }
            i += static_cast<size_t>(take);
            hop_fill_ += take;
            if (hop_fill_ == kHop) {
                step_hop(enabled, swb_hint, emit, out);
                hop_fill_ = 0;
            }
        }
    }

    void flush(bool enabled, bool swb_hint, std::vector<float>& out) {
        const int real_buffered = (kDelay - priming_skip_) + hop_fill_;
        if (real_buffered <= 0) return;
        static const float zeros[kFft * kTargetChannels]{};
        const int pad_zeros = kDelay + (hop_fill_ > 0 ? (kHop - hop_fill_) : 0);
        const size_t base_out = out.size();
        process(zeros, static_cast<size_t>(pad_zeros), enabled, swb_hint, true, out);
        const size_t target_size = base_out + static_cast<size_t>(real_buffered) * kTargetChannels;
        if (out.size() > target_size) out.resize(target_size);
        reset_buffers();
    }

private:
    static constexpr float kLog2ToDb = 3.010299956639812f;   // 10 * log10(2)
    static constexpr float kDbToLog2 = 0.3321928094887362f;  // 1 / (10 * log10(2))

    // Planar (SoA) radix-2 FFT operating directly on fft_re_ / fft_im_ with 1-uop NEON vld1q/vst1q.
    void fft_inplace(bool inverse) {
        for (int i = 0; i < kFft; ++i) {
            const int j = bitrev_[i];
            if (i < j) {
                std::swap(fft_re_[i], fft_re_[j]);
                std::swap(fft_im_[i], fft_im_[j]);
            }
        }
        // Stage 1 (len = 2, half = 1): w = (1, 0) -> multiply-free.
        for (int s = 0; s < kFft; s += 2) {
            const float ur = fft_re_[s], ui = fft_im_[s];
            const float vr = fft_re_[s + 1], vi = fft_im_[s + 1];
            fft_re_[s] = ur + vr;
            fft_im_[s] = ui + vi;
            fft_re_[s + 1] = ur - vr;
            fft_im_[s + 1] = ui - vi;
        }
        // Stage 2 (len = 4, half = 2): w0 = (1, 0), w1 = (0, +/-1) -> multiply-free.
        if (!inverse) {
            for (int s = 0; s < kFft; s += 4) {
                const float ur0 = fft_re_[s], ui0 = fft_im_[s];
                const float vr0 = fft_re_[s + 2], vi0 = fft_im_[s + 2];
                fft_re_[s] = ur0 + vr0;
                fft_im_[s] = ui0 + vi0;
                fft_re_[s + 2] = ur0 - vr0;
                fft_im_[s + 2] = ui0 - vi0;

                const float ur1 = fft_re_[s + 1], ui1 = fft_im_[s + 1];
                const float tr1 = fft_im_[s + 3];
                const float ti1 = -fft_re_[s + 3];
                fft_re_[s + 1] = ur1 + tr1;
                fft_im_[s + 1] = ui1 + ti1;
                fft_re_[s + 3] = ur1 - tr1;
                fft_im_[s + 3] = ui1 - ti1;
            }
        } else {
            for (int s = 0; s < kFft; s += 4) {
                const float ur0 = fft_re_[s], ui0 = fft_im_[s];
                const float vr0 = fft_re_[s + 2], vi0 = fft_im_[s + 2];
                fft_re_[s] = ur0 + vr0;
                fft_im_[s] = ui0 + vi0;
                fft_re_[s + 2] = ur0 - vr0;
                fft_im_[s + 2] = ui0 - vi0;

                const float ur1 = fft_re_[s + 1], ui1 = fft_im_[s + 1];
                const float tr1 = -fft_im_[s + 3];
                const float ti1 = fft_re_[s + 3];
                fft_re_[s + 1] = ur1 + tr1;
                fft_im_[s + 1] = ui1 + ti1;
                fft_re_[s + 3] = ur1 - tr1;
                fft_im_[s + 3] = ui1 - ti1;
            }
        }
        // Stages 3..8 (len = 8..256, half = 4..128): 4-wide NEON butterflies with contiguous twiddles.
        const float* const tw_im_base = inverse ? stage_tw_im_inv_.data() : stage_tw_im_.data();
        for (int len = 8; len <= kFft; len <<= 1) {
            const int half = len >> 1;
            const float* __restrict wr_tab = stage_tw_re_.data() + (half - 4);
            const float* __restrict wi_tab = tw_im_base + (half - 4);
            for (int start = 0; start < kFft; start += len) {
                float* __restrict ar = fft_re_.data() + start;
                float* __restrict ai = fft_im_.data() + start;
                float* __restrict br = ar + half;
                float* __restrict bi = ai + half;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
                for (int k = 0; k < half; k += 4) {
                    const float32x4_t wr = vld1q_f32(wr_tab + k);
                    const float32x4_t wi = vld1q_f32(wi_tab + k);
                    const float32x4_t vr = vld1q_f32(br + k);
                    const float32x4_t vi = vld1q_f32(bi + k);
                    const float32x4_t tr = neon_fnma(vmulq_f32(wr, vr), wi, vi);
                    const float32x4_t ti = neon_fma(vmulq_f32(wr, vi), wi, vr);
                    const float32x4_t ur = vld1q_f32(ar + k);
                    const float32x4_t ui = vld1q_f32(ai + k);
                    vst1q_f32(ar + k, vaddq_f32(ur, tr));
                    vst1q_f32(ai + k, vaddq_f32(ui, ti));
                    vst1q_f32(br + k, vsubq_f32(ur, tr));
                    vst1q_f32(bi + k, vsubq_f32(ui, ti));
                }
#else
                for (int k = 0; k < half; ++k) {
                    const float wr = wr_tab[k];
                    const float wi = wi_tab[k];
                    const float vr = br[k];
                    const float vi = bi[k];
                    const float tr = wr * vr - wi * vi;
                    const float ti = wr * vi + wi * vr;
                    const float ur = ar[k];
                    const float ui = ai[k];
                    ar[k] = ur + tr;
                    ai[k] = ui + ti;
                    br[k] = ur - tr;
                    bi[k] = ui - ti;
                }
#endif
            }
        }
        if (inverse) {
            constexpr float inv_n = 1.0f / static_cast<float>(kFft);
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
            const float32x4_t inv_v = vdupq_n_f32(inv_n);
            for (int i = 0; i < kFft; i += 4) {
                vst1q_f32(fft_re_.data() + i, vmulq_f32(vld1q_f32(fft_re_.data() + i), inv_v));
                vst1q_f32(fft_im_.data() + i, vmulq_f32(vld1q_f32(fft_im_.data() + i), inv_v));
            }
#else
            for (int i = 0; i < kFft; ++i) {
                fft_re_[i] *= inv_n;
                fft_im_[i] *= inv_n;
            }
#endif
        }
    }

    static float band_avg(const std::array<float, kFft / 2 + 1>& pwr, int lo, int hi) {
        float s = 0.0f;
        for (int k = lo; k <= hi; ++k) s += pwr[k];
        return s / static_cast<float>(hi - lo + 1);
    }

    // All shifts (24, 20, 40) are exact multiples of kFft/kHop = 4, so per-hop carrier phase advance
    // is an integer multiple of 2*pi (zero phase rotation needed between consecutive hops).
    // Synthesizes a power-normalized blend of coherent harmonic transposition and quadrature-decorrelated
    // diffuse air (|w_b|^2 == 1.0 exact per bin, preserving 100% L/R energy and inter-channel stereo phase).
    void synthesize_band_stereo(int src_lo, int sub_w, int shift, int step_offset, int fade_in, int fade_out,
                                float gain_mult, float body_db, float pres_db, float diffuse_beta,
                                std::array<float, kNumSub>& smooth_gains) {
        // Precomputed 0.5 * (1 - cos(pi * k / (fade + 1))) ramps for fade=3 and fade=5 (zero runtime trig).
        static constexpr float kFade3[3] = {0.14644661f, 0.5f, 0.85355339f};
        static constexpr float kFade5[5] = {0.06698730f, 0.25f, 0.5f, 0.75f, 0.93301270f};

        std::array<float, kNumSub> src_pwr{};
        std::array<float, kNumSub> src_db{};
        for (int m = 0; m < kNumSub; ++m) {
            const int s_lo = src_lo + m * sub_w;
            const float p = band_avg(pwr_, s_lo, s_lo + sub_w - 1);
            src_pwr[m] = p;
            src_db[m] = kLog2ToDb * fast_log2f(std::max(p, 1e-24f));
        }
        if (src_pwr[kNumSub - 1] < 1e-20f) {
            for (float& g : smooth_gains) g *= 0.75f;
            return;
        }

        // Full-spectrum macro-tilt conditioning + encoder low-pass transition droop compensation.
        const float mid_src_db = 0.5f * (src_db[1] + src_db[2]);
        const float slope_lo = std::clamp(0.5f * (src_db[2] - src_db[0]), -12.0f, -0.8f);
        const float expected_db3 = src_db[2] + slope_lo;
        // If the final pre-cutoff subband droops abnormally steeper than subbands 0..2 while the 3-8 kHz
        // presence band confirms a genuinely bright signal, recover up to +4 dB of encoder low-pass knee droop.
        float droop_rec = 0.0f;
        if (src_db[3] < expected_db3 - 1.5f && (pres_db - mid_src_db) < 14.0f) {
            droop_rec = std::clamp(0.65f * (expected_db3 - src_db[3]), 0.0f, 4.0f);
        }
        const float anchor_db = src_db[3] + droop_rec;
        const float slope_hi = std::clamp(0.5f * (anchor_db - src_db[1]), -12.0f, -0.8f);
        const float curv = std::clamp(slope_hi - slope_lo, -2.5f, 0.0f);
        const float step_db = std::clamp(slope_hi + 0.35f * curv, -12.0f, -1.0f);

        // Dark-mix macro guard: tuck further down if 8-15 kHz is already far below the 0.4-3 kHz midrange body.
        const float body_drop = body_db - mid_src_db;
        const float macro_tuck_db =
            (body_drop > 18.0f) ? std::clamp(-0.35f * (body_drop - 18.0f), -6.0f, 0.0f)
                                : ((body_drop < 10.0f && diffuse_beta > 0.25f) ? 1.2f : 0.0f);
        const float tuck_db = -9.0f + macro_tuck_db;
        const int dst_lo = src_lo + shift;
        const int dst_hi = dst_lo + kNumSub * sub_w - 1;

        for (int m = 0; m < kNumSub; ++m) {
            const int d_lo = dst_lo + m * sub_w;
            const int d_hi = std::min(d_lo + sub_w - 1, kFft / 2 - 1);
            const float target_db = anchor_db + static_cast<float>(step_offset + m + 1) * step_db + tuck_db;
            const float target_pwr = std::exp2f(target_db * kDbToLog2);
            // Power-complementary cap: only synthesize the missing HF deficit above any surviving real HF energy.
            const float exist_pwr = (d_lo <= d_hi) ? band_avg(pwr_, d_lo, d_hi) : 0.0f;
            const float deficit_pwr = std::max(0.0f, target_pwr - exist_pwr);
            const float raw_g =
                (src_pwr[m] > 1e-22f && deficit_pwr > 0.0f) ? std::min(std::sqrt(deficit_pwr / src_pwr[m]), 1.25f) : 0.0f;
            // Fast 2 ms attack so transients lock immediately; smooth 5 ms release to prevent tail flutter.
            const float alpha = (raw_g > smooth_gains[m]) ? 0.55f : 0.25f;
            smooth_gains[m] += alpha * (raw_g - smooth_gains[m]);
        }

        const float inv_sub_w = 1.0f / static_cast<float>(sub_w);
        const float first_center = static_cast<float>(dst_lo) + 0.5f * static_cast<float>(sub_w) - 0.5f;
        const float* const f_in_tab = (fade_in == 5) ? kFade5 : kFade3;
        const float* const f_out_tab = (fade_out == 5) ? kFade5 : kFade3;

        // Orthogonal coherent + quadrature diffuse weights: c_coh^2 + c_diff^2 == 1.0 exact.
        const float c_diff = std::clamp(diffuse_beta, 0.0f, 0.70710678f);
        const float c_coh = std::sqrt(std::max(0.0f, 1.0f - c_diff * c_diff));

        for (int b = dst_lo; b <= dst_hi; ++b) {
            const int s = b - shift;
            if (s <= 0 || s >= kFft / 2 || b >= kFft / 2) continue;

            // Smooth piecewise-linear interpolation across the 4 subband centers (no staircase edges).
            const float pos = (static_cast<float>(b) - first_center) * inv_sub_w;
            float sub_g;
            if (pos <= 0.0f) {
                sub_g = smooth_gains[0];
            } else if (pos >= static_cast<float>(kNumSub - 1)) {
                sub_g = smooth_gains[kNumSub - 1];
            } else {
                const int idx = static_cast<int>(pos);
                const float frac = pos - static_cast<float>(idx);
                sub_g = smooth_gains[idx] + frac * (smooth_gains[idx + 1] - smooth_gains[idx]);
            }

            float edge = 1.0f;
            if (b - dst_lo < fade_in) {
                edge *= f_in_tab[b - dst_lo];
            }
            if (dst_hi - b < fade_out) {
                edge *= f_out_tab[dst_hi - b];
            }

            const float scale = sub_g * edge * gain_mult;
            if (scale <= 1e-7f) continue;

            // Deterministic xorshift32 quadrature phase decorrelator: identical complex rotor (wr + j*wi)
            // applied to L and R preserves exact bin power and L/R stereo panning while breaking comb correlation.
            uint32_t r = rng_state_;
            r ^= r << 13;
            r ^= r >> 17;
            r ^= r << 5;
            rng_state_ = r;
            const float wr = scale * c_coh;
            const float wi = scale * ((r & 0x80000000u) ? -c_diff : c_diff);

            const float rlr = spec_l_re_[s];
            const float rli = spec_l_im_[s];
            const float rrr = spec_r_re_[s];
            const float rri = spec_r_im_[s];
            const float vlr = rlr * wr - rli * wi;
            const float vli = rlr * wi + rli * wr;
            const float vrr = rrr * wr - rri * wi;
            const float vri = rrr * wi + rri * wr;

            // Pack L and R Hermitian-symmetrically into one complex spectrum: Y[b] = YL[b] + j*YR[b].
            fft_re_[b] += vlr - vri;
            fft_im_[b] += vli + vrr;
            fft_re_[kFft - b] += vlr + vri;
            fft_im_[kFft - b] += -vli + vrr;
        }
    }

    void step_hop(bool enabled, bool swb_hint, bool emit, std::vector<float>& out) {
        if (enabled) {
            // While bypassed on clean Fullband audio: probe every 4th hop (187.5 Hz) during initial settle,
            // then every 16th hop (46.875 Hz) once confirmed clean, scaling EMA steps to preserve time constants.
            const bool idle_bypass =
                (blend_ <= 1e-4f && !swb_hint && !brickwall_16_ && mode12_blend_ <= 1e-4f);
            const bool slow_idle = idle_bypass && (idle_probe_count_ >= 16);
            const bool run_probe = !idle_bypass || (slow_idle ? (hop_idx_ == 0) : ((hop_idx_ & 3) == 0));
            if (run_probe) {
                if (idle_bypass) {
                    if (idle_probe_count_ < 16) ++idle_probe_count_;
                } else {
                    idle_probe_count_ = 0;
                }
                const float* __restrict src_l = in_l_.data() + ring_head_;
                const float* __restrict src_r = in_r_.data() + ring_head_;
                const float* __restrict win = hann_.data();
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
                for (int n = 0; n < kFft; n += 4) {
                    const float32x4_t w = vld1q_f32(win + n);
                    vst1q_f32(fft_re_.data() + n, vmulq_f32(vld1q_f32(src_l + n), w));
                    vst1q_f32(fft_im_.data() + n, vmulq_f32(vld1q_f32(src_r + n), w));
                }
#else
                for (int n = 0; n < kFft; ++n) {
                    const float w = win[n];
                    fft_re_[n] = src_l[n] * w;
                    fft_im_[n] = src_r[n] * w;
                }
#endif
                fft_inplace(false);

                // Unpack stereo L/R complex bins [0 .. kFft/2] from single dual-real FFT.
                spec_l_re_[0] = fft_re_[0];
                spec_l_im_[0] = 0.0f;
                spec_r_re_[0] = fft_im_[0];
                spec_r_im_[0] = 0.0f;
                pwr_[0] = spec_l_re_[0] * spec_l_re_[0] + spec_r_re_[0] * spec_r_re_[0];
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
                {
                    const float32x4_t half_v = vdupq_n_f32(0.5f);
                    const float32x4_t nhalf_v = vdupq_n_f32(-0.5f);
                    for (int k = 1; k <= kFft / 2; k += 4) {
                        const float32x4_t zkr = vld1q_f32(fft_re_.data() + k);
                        const float32x4_t zki = vld1q_f32(fft_im_.data() + k);
                        const float32x4_t zcr = neon_rev4(vld1q_f32(fft_re_.data() + (kFft - k - 3)));
                        const float32x4_t zci_pos = neon_rev4(vld1q_f32(fft_im_.data() + (kFft - k - 3)));
                        const float32x4_t lr = vmulq_f32(half_v, vaddq_f32(zkr, zcr));
                        const float32x4_t li = vmulq_f32(half_v, vsubq_f32(zki, zci_pos));
                        const float32x4_t rr = vmulq_f32(half_v, vaddq_f32(zki, zci_pos));
                        const float32x4_t ri = vmulq_f32(nhalf_v, vsubq_f32(zkr, zcr));
                        vst1q_f32(spec_l_re_.data() + k, lr);
                        vst1q_f32(spec_l_im_.data() + k, li);
                        vst1q_f32(spec_r_re_.data() + k, rr);
                        vst1q_f32(spec_r_im_.data() + k, ri);
                        const float32x4_t p = neon_fma(neon_fma(neon_fma(vmulq_f32(lr, lr), li, li), rr, rr), ri, ri);
                        vst1q_f32(pwr_.data() + k, p);
                    }
                }
#else
                for (int k = 1; k <= kFft / 2; ++k) {
                    const float zkr = fft_re_[k];
                    const float zki = fft_im_[k];
                    const float zcr = fft_re_[kFft - k];
                    const float zci = -fft_im_[kFft - k];
                    const float lr = 0.5f * (zkr + zcr);
                    const float li = 0.5f * (zki + zci);
                    const float rr = 0.5f * (zki - zci);
                    const float ri = -0.5f * (zkr - zcr);
                    spec_l_re_[k] = lr;
                    spec_l_im_[k] = li;
                    spec_r_re_[k] = rr;
                    spec_r_im_[k] = ri;
                    pwr_[k] = lr * lr + li * li + rr * rr + ri * ri;
                }
#endif

                // Shared stereo detection across calibrated 48 kHz bands (df = 187.5 Hz/bin).
                const float p_body = band_avg(pwr_, 2, 16);       // 375 - 3000 Hz
                const float p_sib = band_avg(pwr_, 32, 53);       // 6.0 - 9.94 kHz
                const float p_low_hi = band_avg(pwr_, 45, 61);    // 8.44 - 11.44 kHz
                const float p_swb_pre = band_avg(pwr_, 51, 61);   // 9.56 - 11.44 kHz
                const float p_pre16 = band_avg(pwr_, 67, 80);     // 12.56 - 15.00 kHz
                const float p_src = band_avg(pwr_, 59, 82);       // 11.06 - 15.38 kHz
                const float p_post16 = band_avg(pwr_, 90, 104);   // 16.88 - 19.50 kHz

                const float sib_alpha = slow_idle ? 0.115f : (idle_bypass ? 0.03f : 0.0075f);
                if (p_sib > 0.0f) {
                    sib_slow_ema_ =
                        (sib_slow_ema_ <= 0.0f) ? p_sib : ((1.0f - sib_alpha) * sib_slow_ema_ + sib_alpha * p_sib);
                }
                const bool sib_spike = (sib_slow_ema_ > 1e-11f && p_sib > 2.6f * sib_slow_ema_);
                const bool sib_dominant =
                    (p_sib > 8.0f * std::max(p_src, 1e-15f)) && (p_sib > 0.20f * std::max(p_body, 1e-15f));
                const float target_duck = (sib_spike || sib_dominant) ? 0.0f : 1.0f;
                // Fast 12 ms attack down on fricatives ("S"/"T"), smooth 120 ms release back up.
                const float duck_coef =
                    (target_duck < sib_duck_)
                        ? (slow_idle ? 0.85f : (idle_bypass ? 0.40f : 0.117f))
                        : (slow_idle ? 0.16f : (idle_bypass ? 0.043f : 0.011f));
                sib_duck_ += duck_coef * (target_duck - sib_duck_);

                // Only train the brickwall detector when HF content is musical/non-sibilant and above silence.
                if (!sib_dominant && p_body > 1e-9f && p_pre16 > 1e-11f) {
                    if (!ema_initialized_) {
                        ema_initialized_ = true;
                        p_low_hi_ema_ = p_low_hi;
                        p_pre16_ema_ = p_pre16;
                        p_post16_ema_ = p_post16;
                    } else {
                        const float bw_alpha = slow_idle ? 0.062f : (idle_bypass ? 0.016f : 0.004f);
                        const float bw_keep = 1.0f - bw_alpha;
                        p_low_hi_ema_ = bw_keep * p_low_hi_ema_ + bw_alpha * p_low_hi;
                        p_pre16_ema_ = bw_keep * p_pre16_ema_ + bw_alpha * p_pre16;
                        p_post16_ema_ = bw_keep * p_post16_ema_ + bw_alpha * p_post16;
                    }
                    const float nat_slope =
                        kLog2ToDb * fast_log2f(std::max(p_low_hi_ema_, 1e-18f) / std::max(p_pre16_ema_, 1e-18f));
                    const float cliff16 =
                        kLog2ToDb * fast_log2f(std::max(p_pre16_ema_, 1e-18f) / std::max(p_post16_ema_, 1e-18f));
                    brickwall_16_ = (cliff16 >= 20.0f) && (cliff16 >= std::max(0.0f, nat_slope) + 11.0f);
                }

                const bool swb_active = swb_hint && (p_swb_pre > 1e-10f) && (p_pre16 < 0.04f * p_swb_pre);
                mode12_blend_ += (swb_active ? 0.02f : -0.01f);
                mode12_blend_ = std::clamp(mode12_blend_, 0.0f, 1.0f);

                const bool want_ext = swb_active || brickwall_16_;
                blend_ += (want_ext ? (slow_idle ? 0.048f : (idle_bypass ? 0.012f : 0.003f)) : -0.002f);
                blend_ = std::clamp(blend_, 0.0f, 1.0f);

                const int cur_mode = active_mode();
                if (cur_mode > 0 && logged_state_ != cur_mode) {
                    logged_state_ = cur_mode;
                    OLOG("audio dsp: HF extension active (%s)",
                         cur_mode == 2 ? "12kHz SWB -> 20kHz" : "16kHz brickwall -> 20kHz");
                }

                const float eff_gain = blend_ * sib_duck_;
                if (eff_gain > 1e-4f) {
                    const float w12 = mode12_blend_;
                    const float w16 = 1.0f - w12;
                    const bool dom_band12 = (w12 > 0.5f);

                    // Spectral Flatness Measure (SFM) tonality guard + HF transient flux weighting.
                    const int sfm_lo = dom_band12 ? 43 : 59;
                    const int sfm_hi = dom_band12 ? 62 : 82;
                    const int sfm_n = sfm_hi - sfm_lo + 1;
                    float log_sum = 0.0f;
                    float lin_sum = 0.0f;
                    for (int k = sfm_lo; k <= sfm_hi; ++k) {
                        const float p = pwr_[k] + 1e-24f;
                        log_sum += fast_log2f(p);
                        lin_sum += p;
                    }
                    const float geo_mean = std::exp2f(log_sum / static_cast<float>(sfm_n));
                    const float arith_mean = lin_sum / static_cast<float>(sfm_n);
                    const float sfm = std::clamp(geo_mean / std::max(arith_mean, 1e-24f), 0.0f, 1.0f);
                    // Damp pure narrowband tones (~-7 dB) to avoid inharmonic metallic zing; keep broadband air/cymbals at 1.0.
                    const float target_tonal = std::clamp(0.45f + 0.55f * ((sfm - 0.06f) / 0.44f), 0.45f, 1.0f);
                    tonal_guard_ += 0.20f * (target_tonal - tonal_guard_);

                    if (arith_mean > 0.0f) {
                        src_pwr_ema_ = (src_pwr_ema_ <= 0.0f) ? arith_mean : (0.96f * src_pwr_ema_ + 0.04f * arith_mean);
                    }
                    const float flux = arith_mean / std::max(src_pwr_ema_, 1e-24f);
                    const float target_trans = std::clamp(1.0f + 0.13f * (flux - 1.3f), 1.0f, 1.26f);
                    const float trans_coef = (target_trans > trans_boost_) ? 0.35f : 0.03f;
                    trans_boost_ += trans_coef * (target_trans - trans_boost_);

                    const float shaped_gain = eff_gain * tonal_guard_ * trans_boost_;
                    const float p_pres = band_avg(pwr_, 17, 42);  // 3.19 - 7.88 kHz presence anchor
                    const float body_db = kLog2ToDb * fast_log2f(std::max(p_body, 1e-24f));
                    const float pres_db = kLog2ToDb * fast_log2f(std::max(p_pres, 1e-24f));
                    // Quadrature diffuse-air weight: high on broadband air/cymbals (high SFM), tucked back on
                    // sharp transient attacks and tonal harmonics so attacks stay phase-locked.
                    const float sfm_diffuse = std::clamp((sfm - 0.12f) / 0.50f, 0.0f, 0.70710678f);
                    const float trans_lock = std::clamp(1.85f - 0.75f * trans_boost_, 0.35f, 1.0f);
                    const float diffuse_beta = sfm_diffuse * trans_lock;

                    fft_re_.fill(0.0f);
                    fft_im_.fill(0.0f);
                    if (w16 > 1e-4f) {
                        // Translate [11.06 .. 15.38 kHz] (+24 bins = +4.50 kHz) -> [15.56 .. 19.88 kHz]
                        synthesize_band_stereo(59, 6, 24, 0, 3, 5, shaped_gain * w16, body_db, pres_db, diffuse_beta,
                                               smooth_g16_);
                    } else {
                        for (float& g : smooth_g16_) g *= 0.85f;
                    }
                    if (w12 > 1e-4f) {
                        // SWB 12 kHz Opus: [8.06 .. 11.63 kHz] -> [11.81 .. 15.38 kHz] (+20) and [15.56 .. 19.13 kHz] (+40)
                        synthesize_band_stereo(43, 5, 20, 0, 3, 3, shaped_gain * w12, body_db, pres_db,
                                               0.85f * diffuse_beta, smooth_g12a_);
                        synthesize_band_stereo(43, 5, 40, kNumSub, 3, 5, shaped_gain * w12, body_db, pres_db,
                                               diffuse_beta, smooth_g12b_);
                    } else {
                        for (int m = 0; m < kNumSub; ++m) {
                            smooth_g12a_[m] *= 0.85f;
                            smooth_g12b_[m] *= 0.85f;
                        }
                    }
                    fft_inplace(true);
                    constexpr float inv_ola = 1.0f / 1.5f;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
                    {
                        const float32x4_t inv_v = vdupq_n_f32(inv_ola);
                        for (int n = 0; n < kFft; n += 4) {
                            const float32x4_t ws = vmulq_f32(vld1q_f32(win + n), inv_v);
                            vst1q_f32(ola_l_.data() + n,
                                      neon_fma(vld1q_f32(ola_l_.data() + n), vld1q_f32(fft_re_.data() + n), ws));
                            vst1q_f32(ola_r_.data() + n,
                                      neon_fma(vld1q_f32(ola_r_.data() + n), vld1q_f32(fft_im_.data() + n), ws));
                        }
                    }
#else
                    for (int n = 0; n < kFft; ++n) {
                        const float ws = win[n] * inv_ola;
                        ola_l_[n] += fft_re_[n] * ws;
                        ola_r_[n] += fft_im_[n] * ws;
                    }
#endif
                    ola_tail_hops_ = kFft / kHop;
                } else {
                    for (int m = 0; m < kNumSub; ++m) {
                        smooth_g16_[m] *= 0.85f;
                        smooth_g12a_[m] *= 0.85f;
                        smooth_g12b_[m] *= 0.85f;
                    }
                }
            }
        }

        // Pop the alignment-matched oldest 64 frames: strictly (dry + HF residual).
        if (priming_skip_ >= kHop) {
            priming_skip_ -= kHop;
        } else {
            const int start = priming_skip_;
            priming_skip_ = 0;
            if (emit) {
                const size_t base_out = out.size();
                const size_t count = static_cast<size_t>(kHop - start);
                out.resize(base_out + count * kTargetChannels);
                float* __restrict dst = out.data() + base_out;
                const float* __restrict dry_l = in_l_.data() + ring_head_ + start;
                const float* __restrict dry_r = in_r_.data() + ring_head_ + start;
                size_t idx = 0;
                if (ola_tail_hops_ == 0) {
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
                    for (; idx + 4 <= count; idx += 4) {
                        float32x4x2_t lr;
                        lr.val[0] = vld1q_f32(dry_l + idx);
                        lr.val[1] = vld1q_f32(dry_r + idx);
                        vst2q_f32(dst + 2 * idx, lr);
                    }
#endif
                    for (; idx < count; ++idx) {
                        dst[2 * idx] = dry_l[idx];
                        dst[2 * idx + 1] = dry_r[idx];
                    }
                } else {
                    const float* __restrict wet_l = ola_l_.data() + start;
                    const float* __restrict wet_r = ola_r_.data() + start;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
                    for (; idx + 4 <= count; idx += 4) {
                        float32x4x2_t lr;
                        lr.val[0] = vaddq_f32(vld1q_f32(dry_l + idx), vld1q_f32(wet_l + idx));
                        lr.val[1] = vaddq_f32(vld1q_f32(dry_r + idx), vld1q_f32(wet_r + idx));
                        vst2q_f32(dst + 2 * idx, lr);
                    }
#endif
                    for (; idx < count; ++idx) {
                        dst[2 * idx] = dry_l[idx] + wet_l[idx];
                        dst[2 * idx + 1] = dry_r[idx] + wet_r[idx];
                    }
                }
            }
        }

        ring_head_ = (ring_head_ + kHop) & (kFft - 1);
        if (ola_tail_hops_ > 0) {
            std::memmove(ola_l_.data(), ola_l_.data() + kHop, static_cast<size_t>(kDelay) * sizeof(float));
            std::memmove(ola_r_.data(), ola_r_.data() + kHop, static_cast<size_t>(kDelay) * sizeof(float));
            std::fill(ola_l_.begin() + kDelay, ola_l_.end(), 0.0f);
            std::fill(ola_r_.begin() + kDelay, ola_r_.end(), 0.0f);
            --ola_tail_hops_;
        }
        hop_idx_ = (hop_idx_ + 1) & 15;
    }

    alignas(64) std::array<float, kFft> hann_{};
    std::array<int, kFft> bitrev_{};
    alignas(64) std::array<float, kStageTwSize> stage_tw_re_{};
    alignas(64) std::array<float, kStageTwSize> stage_tw_im_{};
    alignas(64) std::array<float, kStageTwSize> stage_tw_im_inv_{};

    // Mirrored planar input rings (2x kFft): &in_l_[ring_head_] is always a contiguous 64B-aligned 256-sample window.
    alignas(64) std::array<float, kFft * 2> in_l_{};
    alignas(64) std::array<float, kFft * 2> in_r_{};
    alignas(64) std::array<float, kFft> ola_l_{};
    alignas(64) std::array<float, kFft> ola_r_{};
    alignas(64) std::array<float, kFft> fft_re_{};
    alignas(64) std::array<float, kFft> fft_im_{};
    alignas(64) std::array<float, kFft / 2 + 1> spec_l_re_{};
    alignas(64) std::array<float, kFft / 2 + 1> spec_l_im_{};
    alignas(64) std::array<float, kFft / 2 + 1> spec_r_re_{};
    alignas(64) std::array<float, kFft / 2 + 1> spec_r_im_{};
    alignas(64) std::array<float, kFft / 2 + 1> pwr_{};
    std::array<float, kNumSub> smooth_g16_{};
    std::array<float, kNumSub> smooth_g12a_{};
    std::array<float, kNumSub> smooth_g12b_{};

    int ring_head_ = 0;
    int hop_fill_ = 0;
    int hop_idx_ = 0;
    int ola_tail_hops_ = 0;
    int idle_probe_count_ = 0;
    int priming_skip_ = kDelay;

    float p_low_hi_ema_ = 0.0f;
    float p_pre16_ema_ = 0.0f;
    float p_post16_ema_ = 0.0f;
    float sib_slow_ema_ = 0.0f;
    bool ema_initialized_ = false;
    bool brickwall_16_ = false;
    float blend_ = 0.0f;
    float mode12_blend_ = 0.0f;
    float sib_duck_ = 1.0f;
    float tonal_guard_ = 1.0f;
    float src_pwr_ema_ = 0.0f;
    float trans_boost_ = 1.0f;
    uint32_t rng_state_ = 0x9E3779B9u;
    int logged_state_ = -1;
};

}  // namespace

struct Pipeline::Impl {
    media::Codec codec = media::Codec::Unknown;
    int pcm_bits = 32;
    double gain = 1.0;
    double atten_db = 1.0;
    std::atomic<int> hf_mode{0};

    // Decoders (at most one active)
    OpusDecoder* opus = nullptr;
    int opus_channels = 2;
    int opus_pre_skip_init = 0;
    int opus_pre_skip_left = 0;
    bool opus_is_swb = false;
    std::vector<float> opus_buf;

    NeAACDecHandle aac = nullptr;
    int aac_rate = 0;
    bool aac_native_sbr = false;

    SincResampler resampler;
    HfExtender hf_extender;
    std::vector<float> work_in;
    std::vector<float> resampled_buf;
    std::vector<float> out_fifo;
    size_t out_fifo_pos = 0;
    std::vector<std::vector<uint8_t>> free_bufs;

    size_t out_fifo_frames() const { return (out_fifo.size() - out_fifo_pos) / kTargetChannels; }
    void clear_out_fifo() {
        out_fifo.clear();
        out_fifo_pos = 0;
    }

    void reclaim_frames(std::vector<PcmFrame>& out) {
        for (auto& f : out) {
            if (free_bufs.size() < 8 && f.data.capacity() > 0) {
                f.data.clear();
                free_bufs.push_back(std::move(f.data));
            }
        }
        out.clear();
    }

    // Contiguous 48 kHz sample-locked timestamp tracking
    bool pts_anchored = false;
    int64_t base_pts_ms = 0;
    int64_t emitted_frames = 0;
    uint64_t frames_decoded = 0;

    // 16-bit TPDF dither + 1st-order noise-shaping fallback state
    uint64_t rng = 0x9e3779b97f4a7c15ULL;
    double ns_err_l = 0.0;
    double ns_err_r = 0.0;

    ~Impl() {
        if (opus) {
            opus_decoder_destroy(opus);
            opus = nullptr;
        }
        if (aac) {
            NeAACDecClose(aac);
            aac = nullptr;
        }
    }

    double uniform_half() {
        uint64_t z = (rng += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        z ^= (z >> 31);
        return static_cast<double>(z >> 11) * (1.0 / 9007199254740992.0) - 0.5;
    }

    double tpdf() { return uniform_half() - uniform_half(); }

    void pack_samples(const float* stereo, size_t frames, std::vector<uint8_t>& bytes) {
        const size_t bps = static_cast<size_t>(pcm_bits / 8);
        bytes.resize(frames * kTargetChannels * bps);
        uint8_t* dst = bytes.data();

        if (pcm_bits == 32) {
            const size_t total = frames * kTargetChannels;
            uint8_t* const dst_base = dst;
            size_t i = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
            const float32x4_t gain_v = vdupq_n_f32(static_cast<float>(gain));
            const float32x4_t limit_v = vdupq_n_f32(0.995f);
            const float32x4_t scale32_v = vdupq_n_f32(2147483648.0f);
            const float32x4_t zero_v = vdupq_n_f32(0.0f);
            const float32x4_t pos_half_v = vdupq_n_f32(0.5f);
            const float32x4_t neg_half_v = vdupq_n_f32(-0.5f);
            uint32x4_t over_any = vdupq_n_u32(0u);
            // Branchless 8-wide NEON loop: zero NEON->ARM cross-domain transfers inside the hot loop.
            for (; i + 8 <= total; i += 8) {
                const float32x4_t v0 = vmulq_f32(vld1q_f32(stereo + i), gain_v);
                const float32x4_t v1 = vmulq_f32(vld1q_f32(stereo + i + 4), gain_v);
                over_any = vorrq_u32(over_any, vcgtq_f32(vabsq_f32(v0), limit_v));
                over_any = vorrq_u32(over_any, vcgtq_f32(vabsq_f32(v1), limit_v));
                const float32x4_t s0 = vmulq_f32(v0, scale32_v);
                const float32x4_t s1 = vmulq_f32(v1, scale32_v);
                const float32x4_t b0 = vbslq_f32(vcltq_f32(s0, zero_v), neg_half_v, pos_half_v);
                const float32x4_t b1 = vbslq_f32(vcltq_f32(s1, zero_v), neg_half_v, pos_half_v);
                vst1q_u8(dst, vreinterpretq_u8_s32(vcvtq_s32_f32(vaddq_f32(s0, b0))));
                vst1q_u8(dst + 16, vreinterpretq_u8_s32(vcvtq_s32_f32(vaddq_f32(s1, b1))));
                dst += 32;
            }
            for (; i + 4 <= total; i += 4) {
                const float32x4_t v = vmulq_f32(vld1q_f32(stereo + i), gain_v);
                over_any = vorrq_u32(over_any, vcgtq_f32(vabsq_f32(v), limit_v));
                const float32x4_t s = vmulq_f32(v, scale32_v);
                const float32x4_t b = vbslq_f32(vcltq_f32(s, zero_v), neg_half_v, pos_half_v);
                vst1q_u8(dst, vreinterpretq_u8_s32(vcvtq_s32_f32(vaddq_f32(s, b))));
                dst += 16;
            }
            const uint32x2_t any2 = vorr_u32(vget_low_u32(over_any), vget_high_u32(over_any));
            if (vget_lane_u32(vpmax_u32(any2, any2), 0) != 0u) {
                // Rare (<0.1%): fix up only samples exceeding 0.995 with exact soft-peak guard.
                for (size_t k = 0; k < i; ++k) {
                    const double raw = static_cast<double>(stereo[k]) * gain;
                    if (std::abs(raw) > 0.995) {
                        const double sv = guard_peak(raw);
                        const long long sq = std::llrint(sv * 2147483647.0);
                        const uint32_t u = static_cast<uint32_t>(
                            static_cast<int32_t>(std::clamp<long long>(sq, INT32_MIN, INT32_MAX)));
                        uint8_t* p = dst_base + 4 * k;
                        p[0] = static_cast<uint8_t>(u & 0xffu);
                        p[1] = static_cast<uint8_t>((u >> 8) & 0xffu);
                        p[2] = static_cast<uint8_t>((u >> 16) & 0xffu);
                        p[3] = static_cast<uint8_t>((u >> 24) & 0xffu);
                    }
                }
            }
#endif
            (void)dst_base;
            for (; i < total; ++i) {
                const double v = guard_peak(static_cast<double>(stereo[i]) * gain);
                const long long q = std::llrint(v * 2147483647.0);
                const uint32_t u = static_cast<uint32_t>(
                    static_cast<int32_t>(std::clamp<long long>(q, INT32_MIN, INT32_MAX)));
                dst[0] = static_cast<uint8_t>(u & 0xffu);
                dst[1] = static_cast<uint8_t>((u >> 8) & 0xffu);
                dst[2] = static_cast<uint8_t>((u >> 16) & 0xffu);
                dst[3] = static_cast<uint8_t>((u >> 24) & 0xffu);
                dst += 4;
            }
        } else if (pcm_bits == 24) {
            constexpr double inv_24 = 1.0 / 8388607.0;
            for (size_t i = 0; i < frames * kTargetChannels; ++i) {
                const double v = guard_peak(static_cast<double>(stereo[i]) * gain) + tpdf() * inv_24;
                const long long q = std::llrint(v * 8388607.0);
                const uint32_t u = static_cast<uint32_t>(
                    static_cast<int32_t>(std::clamp<long long>(q, -8388608LL, 8388607LL)));
                dst[0] = static_cast<uint8_t>(u & 0xffu);
                dst[1] = static_cast<uint8_t>((u >> 8) & 0xffu);
                dst[2] = static_cast<uint8_t>((u >> 16) & 0xffu);
                dst += 3;
            }
        } else {
            // 16-bit fallback: Lipshitz TPDF dither + 1st-order E-weighted error feedback.
            for (size_t f = 0; f < frames; ++f) {
                double* errs[2] = {&ns_err_l, &ns_err_r};
                for (int c = 0; c < kTargetChannels; ++c) {
                    const double v = guard_peak(static_cast<double>(stereo[f * kTargetChannels + c]) * gain);
                    const double target = v * 32767.0;
                    const double shaped = target + tpdf() + 0.5 * (*errs[c]);
                    const long long q = std::llrint(std::clamp(shaped, -32768.0, 32767.0));
                    *errs[c] = target - static_cast<double>(q);
                    const uint16_t u = static_cast<uint16_t>(static_cast<int16_t>(q));
                    dst[0] = static_cast<uint8_t>(u & 0xffu);
                    dst[1] = static_cast<uint8_t>((u >> 8) & 0xffu);
                    dst += 2;
                }
            }
        }
    }

    void drain_chunks(std::vector<PcmFrame>& out, bool flush_tail) {
        const size_t min_frames = flush_tail ? 1 : static_cast<size_t>(kFrameSamples);
        size_t offset_frames = 0;
        const size_t avail_frames = out_fifo_frames();
        while (avail_frames - offset_frames >= min_frames) {
            const size_t take = std::min<size_t>(kFrameSamples, avail_frames - offset_frames);
            if (!flush_tail && take < static_cast<size_t>(kFrameSamples)) break;
            PcmFrame pkt;
            if (!free_bufs.empty()) {
                pkt.data = std::move(free_bufs.back());
                free_bufs.pop_back();
            }
            pkt.pts_ms = base_pts_ms + (emitted_frames * 1000LL + kTargetRate / 2) / kTargetRate;
            pkt.duration_ms = std::max<int64_t>(1, (static_cast<int64_t>(take) * 1000LL + kTargetRate / 2) / kTargetRate);
            pack_samples(out_fifo.data() + out_fifo_pos + offset_frames * kTargetChannels, take, pkt.data);
            out.push_back(std::move(pkt));
            offset_frames += take;
            emitted_frames += static_cast<int64_t>(take);
        }
        if (offset_frames > 0) {
            out_fifo_pos += offset_frames * kTargetChannels;
            if (out_fifo_pos >= out_fifo.size()) {
                clear_out_fifo();
            } else if (out_fifo_pos >= static_cast<size_t>(kFrameSamples) * 4 * kTargetChannels) {
                out_fifo.erase(out_fifo.begin(), out_fifo.begin() + static_cast<std::ptrdiff_t>(out_fifo_pos));
                out_fifo_pos = 0;
            }
        }
    }
};

Pipeline::Pipeline() : impl_(std::make_unique<Impl>()) {}
Pipeline::~Pipeline() = default;

bool Pipeline::init(const media::TrackInfo& track, bool has_loudness_db, double loudness_db, int pcm_bits) {
    if (pcm_bits != 32 && pcm_bits != 24 && pcm_bits != 16) return false;
    impl_->codec = track.codec;
    impl_->pcm_bits = pcm_bits;

    // Linear-only normalization: honor YouTube loudnessDb attenuation (never boost into limiting),
    // with >= 1.0 dB inter-sample peak headroom so hot lossy frames never clip DAC/Bluetooth stages.
    const double yt_atten = (has_loudness_db && loudness_db > 0.0) ? std::clamp(loudness_db, 0.0, 18.0) : 0.0;
    impl_->atten_db = std::max(1.0, yt_atten);
    impl_->gain = std::pow(10.0, -impl_->atten_db / 20.0);
    impl_->frames_decoded = 0;
    impl_->opus_is_swb = false;
    impl_->aac_native_sbr = false;
    impl_->hf_mode.store(0, std::memory_order_relaxed);

    if (track.codec == media::Codec::Opus) {
        int ch = track.channels > 0 ? static_cast<int>(track.channels) : 2;
        int gain_q8 = 0;
        impl_->opus_pre_skip_init = 0;
        if (track.codec_data.size() >= 19 && std::memcmp(track.codec_data.data(), "OpusHead", 8) == 0) {
            ch = static_cast<int>(track.codec_data[9]);
            impl_->opus_pre_skip_init = static_cast<int>(le16(track.codec_data.data() + 10));
            gain_q8 = static_cast<int>(sle16(track.codec_data.data() + 16));
        }
        if (ch < 1 || ch > 2) {
            OLOG("audio dsp: unsupported Opus channel count (%d)", ch);
            return false;
        }
        impl_->opus_pre_skip_left = impl_->opus_pre_skip_init;
        impl_->opus_channels = ch;
        int err = OPUS_OK;
        impl_->opus = opus_decoder_create(kTargetRate, ch, &err);
        if (!impl_->opus || err != OPUS_OK) {
            OLOG("audio dsp: opus_decoder_create failed (%d)", err);
            return false;
        }
        if (gain_q8 != 0) {
            opus_decoder_ctl(impl_->opus, OPUS_SET_GAIN(static_cast<opus_int32>(gain_q8)));
        }
        impl_->opus_buf.resize(static_cast<size_t>(5760) * ch);
        impl_->resampler.configure(kTargetRate);
    } else if (track.codec == media::Codec::AAC) {
        if (track.codec_data.empty()) {
            OLOG("audio dsp: AAC missing AudioSpecificConfig");
            return false;
        }
        impl_->aac = NeAACDecOpen();
        if (!impl_->aac) {
            OLOG("audio dsp: NeAACDecOpen failed");
            return false;
        }
        NeAACDecConfigurationPtr cfg = NeAACDecGetCurrentConfiguration(impl_->aac);
        cfg->outputFormat = FAAD_FMT_FLOAT;
        cfg->downMatrix = 1;
        cfg->dontUpSampleImplicitSBR = 0;
        NeAACDecSetConfiguration(impl_->aac, cfg);

        unsigned long sr = 0;
        unsigned char ch = 0;
        const int init_rc = static_cast<signed char>(
            NeAACDecInit2(impl_->aac, const_cast<unsigned char*>(track.codec_data.data()),
                          static_cast<unsigned long>(track.codec_data.size()), &sr, &ch));
        if (init_rc < 0 || sr == 0 || ch == 0) {
            OLOG("audio dsp: NeAACDecInit2 failed");
            return false;
        }
        impl_->aac_rate = static_cast<int>(sr);
        // Implicit-SBR HE-AAC reports the half base rate (22.05/24 kHz) until frame 1 decodes;
        // defer table creation in that case so we don't build and discard a 320-phase Kaiser table.
        if (impl_->aac_rate > 24000) {
            impl_->resampler.configure(impl_->aac_rate);
        }
    } else {
        return false;
    }

    impl_->work_in.reserve(static_cast<size_t>(kFrameSamples) * 4 * kTargetChannels);
    impl_->resampled_buf.reserve(static_cast<size_t>(kFrameSamples) * 4 * kTargetChannels);
    impl_->out_fifo.reserve(static_cast<size_t>(kFrameSamples) * 4 * kTargetChannels);
    return true;
}

bool Pipeline::reconfigure_bits(int pcm_bits) {
    if (pcm_bits != 32 && pcm_bits != 24 && pcm_bits != 16) return false;
    impl_->pcm_bits = pcm_bits;
    return true;
}

void Pipeline::reset() {
    if (impl_->opus) {
        opus_decoder_ctl(impl_->opus, OPUS_RESET_STATE);
        impl_->opus_pre_skip_left = 0;  // post-seek priming skips MDCT warmup window cleanly via pts threshold
    }
    if (impl_->aac) {
        NeAACDecPostSeekReset(impl_->aac, -1);
    }
    impl_->resampler.reset();
    impl_->hf_extender.reset_buffers();
    impl_->resampled_buf.clear();
    impl_->clear_out_fifo();
    impl_->pts_anchored = false;
    impl_->emitted_frames = 0;
    impl_->ns_err_l = 0.0;
    impl_->ns_err_r = 0.0;
}

bool Pipeline::process(const uint8_t* data, size_t size, int64_t pts_ms, bool emit_output, std::vector<PcmFrame>& out) {
    impl_->reclaim_frames(out);
    if (!data || size == 0) return true;

    const float* decoded_ptr = nullptr;
    size_t decoded_frames = 0;
    int dec_ch = 2;

    if (impl_->opus) {
        const int n = opus_decode_float(impl_->opus, data, static_cast<opus_int32>(size), impl_->opus_buf.data(), 5760, 0);
        if (n < 0) {
            OLOG("audio dsp: opus decode error %d", n);
            return impl_->frames_decoded > 0;
        }
        opus_int32 bw = OPUS_BANDWIDTH_FULLBAND;
        if (opus_decoder_ctl(impl_->opus, OPUS_GET_BANDWIDTH(&bw)) == OPUS_OK) {
            impl_->opus_is_swb = (bw == OPUS_BANDWIDTH_SUPERWIDEBAND);
        }
        int skip = std::min(n, impl_->opus_pre_skip_left);
        impl_->opus_pre_skip_left -= skip;
        decoded_frames = static_cast<size_t>(n - skip);
        decoded_ptr = impl_->opus_buf.data() + static_cast<size_t>(skip) * impl_->opus_channels;
        dec_ch = impl_->opus_channels;
    } else if (impl_->aac) {
        NeAACDecFrameInfo fi{};
        void* pcm = NeAACDecDecode(impl_->aac, &fi, const_cast<unsigned char*>(data), static_cast<unsigned long>(size));
        if (fi.error != 0) {
            OLOG("audio dsp: aac decode error %u (%s)", fi.error, NeAACDecGetErrorMessage(fi.error));
            return impl_->frames_decoded > 0;
        }
        if (!pcm || fi.samples == 0 || fi.channels == 0) return true;
        if (!impl_->resampler.configured() || (fi.samplerate > 0 && static_cast<int>(fi.samplerate) != impl_->aac_rate)) {
            if (fi.samplerate > 0) impl_->aac_rate = static_cast<int>(fi.samplerate);
            impl_->resampler.configure(impl_->aac_rate);
        }
        impl_->aac_native_sbr = (fi.sbr == SBR_UPSAMPLED || fi.sbr == SBR_DOWNSAMPLED);
        dec_ch = static_cast<int>(fi.channels);
        decoded_frames = static_cast<size_t>(fi.samples) / static_cast<size_t>(dec_ch);
        decoded_ptr = static_cast<const float*>(pcm);
    } else {
        return false;
    }

    if (decoded_frames == 0) return true;
    impl_->frames_decoded += decoded_frames;

    if (!emit_output) {
        // Warm up decoder MDCT + resampler FIR + WOLA state cleanly before post-seek start point.
        impl_->pts_anchored = false;
        impl_->clear_out_fifo();
    } else {
        const int64_t expected_pts =
            impl_->base_pts_ms +
            ((impl_->emitted_frames + static_cast<int64_t>(impl_->out_fifo_frames())) * 1000LL +
             kTargetRate / 2) /
                kTargetRate;
        if (!impl_->pts_anchored || std::abs(pts_ms - expected_pts) > 80) {
            if (impl_->pts_anchored && impl_->out_fifo_frames() > 0) {
                impl_->drain_chunks(out, true);
            }
            impl_->pts_anchored = true;
            impl_->base_pts_ms = pts_ms;
            impl_->emitted_frames = 0;
        }
    }

    const float* stereo_ptr = decoded_ptr;
    if (dec_ch == 1) {
        impl_->work_in.resize(decoded_frames * kTargetChannels);
        size_t i = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
        float* __restrict dst_st = impl_->work_in.data();
        for (; i + 4 <= decoded_frames; i += 4) {
            const float32x4_t m = vld1q_f32(decoded_ptr + i);
            float32x4x2_t st;
            st.val[0] = m;
            st.val[1] = m;
            vst2q_f32(dst_st + 2 * i, st);
        }
#endif
        for (; i < decoded_frames; ++i) {
            const float s = decoded_ptr[i];
            impl_->work_in[2 * i] = s;
            impl_->work_in[2 * i + 1] = s;
        }
        stereo_ptr = impl_->work_in.data();
    } else if (dec_ch > 2) {
        impl_->work_in.resize(decoded_frames * kTargetChannels);
        for (size_t i = 0; i < decoded_frames; ++i) {
            impl_->work_in[2 * i] = decoded_ptr[static_cast<size_t>(dec_ch) * i];
            impl_->work_in[2 * i + 1] = decoded_ptr[static_cast<size_t>(dec_ch) * i + 1];
        }
        stereo_ptr = impl_->work_in.data();
    }

    // Resample to 48 kHz unconditionally while priming so FIR history warms up smoothly too.
    // At native 48 kHz (e.g. Opus), feed stereo_ptr straight into HfExtender with zero extra copies.
    const float* rate48_ptr = stereo_ptr;
    size_t rate48_frames = decoded_frames;
    if (impl_->resampler.active()) {
        impl_->resampled_buf.clear();
        impl_->resampler.process(stereo_ptr, decoded_frames, true, impl_->resampled_buf);
        rate48_ptr = impl_->resampled_buf.data();
        rate48_frames = impl_->resampled_buf.size() / kTargetChannels;
    }

    const bool allow_ext = !(impl_->aac && impl_->aac_native_sbr);
    impl_->hf_extender.process(rate48_ptr, rate48_frames, allow_ext, impl_->opus_is_swb, emit_output, impl_->out_fifo);
    impl_->hf_mode.store(allow_ext ? impl_->hf_extender.active_mode() : 0, std::memory_order_relaxed);
    if (emit_output) {
        impl_->drain_chunks(out, false);
    }
    return true;
}

void Pipeline::flush(std::vector<PcmFrame>& out) {
    out.clear();
    if (!impl_->pts_anchored) return;
    impl_->resampled_buf.clear();
    impl_->resampler.flush(impl_->resampled_buf);
    const bool allow_ext = !(impl_->aac && impl_->aac_native_sbr);
    if (!impl_->resampled_buf.empty()) {
        impl_->hf_extender.process(impl_->resampled_buf.data(), impl_->resampled_buf.size() / kTargetChannels, allow_ext,
                                   impl_->opus_is_swb, true, impl_->out_fifo);
    }
    impl_->hf_extender.flush(allow_ext, impl_->opus_is_swb, impl_->out_fifo);
    impl_->drain_chunks(out, true);
}

int Pipeline::output_sample_rate() const { return kTargetRate; }
int Pipeline::output_channels() const { return kTargetChannels; }
int Pipeline::source_sample_rate() const {
    return impl_->resampler.source_rate() > 0 ? impl_->resampler.source_rate() : impl_->aac_rate;
}
int Pipeline::pcm_bits() const { return impl_->pcm_bits; }
int Pipeline::hf_mode() const { return impl_->hf_mode.load(std::memory_order_relaxed); }
double Pipeline::attenuation_db() const { return impl_->atten_db; }

}  // namespace omega::audio
