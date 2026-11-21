#include "audio/dsp.h"

#include <opus.h>
#include <neaacdec.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cmath>
#include <complex>
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
constexpr double kKaiserBeta = 12.6;
constexpr double kSincCutoff = 0.479;
constexpr double kPi = 3.14159265358979323846;

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

// Double-precision stereo polyphase Kaiser-windowed sinc resampler targeting 48 kHz.
// When source rate is already 48 kHz, bypasses completely with zero filtering or phase shift.
class SincResampler {
public:
    void configure(int src_rate) {
        if (src_rate <= 0) src_rate = kTargetRate;
        if (src_rate == src_rate_) return;
        src_rate_ = src_rate;
        bypass_ = (src_rate_ == kTargetRate);
        if (bypass_) {
            coeffs_.clear();
            in_buf_.clear();
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
        in_buf_.assign(static_cast<size_t>(kHalfTaps - 1) * kTargetChannels, 0.0);
    }

    int source_rate() const { return src_rate_; }
    bool active() const { return !bypass_; }

    void process(const double* stereo_in, size_t in_frames, bool emit, std::vector<double>& out) {
        if (in_frames == 0) return;
        if (bypass_) {
            if (emit) out.insert(out.end(), stereo_in, stereo_in + in_frames * kTargetChannels);
            return;
        }
        in_buf_.insert(in_buf_.end(), stereo_in, stereo_in + in_frames * kTargetChannels);
        run_available(emit, out);
    }

    void flush(std::vector<double>& out) {
        if (bypass_ || in_buf_.empty()) return;
        in_buf_.insert(in_buf_.end(), static_cast<size_t>(kHalfTaps) * kTargetChannels, 0.0);
        run_available(true, out);
    }

private:
    void build_kernel() {
        const double cutoff = std::min(kSincCutoff, kSincCutoff * static_cast<double>(kTargetRate) / src_rate_);
        const double i0_beta = bessel_i0(kKaiserBeta);
        const size_t num_rows = rational_ ? up_ : (up_ + 1);
        coeffs_.assign(num_rows * kTaps, 0.0);

        for (size_t p = 0; p < num_rows; ++p) {
            const double frac = static_cast<double>(p) / static_cast<double>(up_);
            double* row = &coeffs_[p * kTaps];
            double norm = 0.0;
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
                row[k] = h;
                norm += h;
            }
            if (norm > 0.0) {
                const double inv = 1.0 / norm;
                for (int k = 0; k < kTaps; ++k) row[k] *= inv;
            }
        }
    }

    void run_available(bool emit, std::vector<double>& out) {
        const size_t total_frames = in_buf_.size() / kTargetChannels;
        if (total_frames < static_cast<size_t>(kTaps)) return;
        if (emit) {
            const size_t avail = total_frames - static_cast<size_t>(kTaps) + 1;
            const size_t est_out =
                rational_ ? ((avail * up_) / std::max<size_t>(1, down_) + 2)
                          : (static_cast<size_t>(static_cast<double>(avail) / std::max(step_double_, 1e-6)) + 2);
            out.reserve(out.size() + est_out * kTargetChannels);
        }
        size_t consumed = 0;
        while (consumed + static_cast<size_t>(kTaps) <= total_frames) {
            if (emit) {
                // Dual accumulator pairs break the 4-cycle FMA latency chain on in-order ARMv8 cores.
                double acc_l0 = 0.0, acc_l1 = 0.0;
                double acc_r0 = 0.0, acc_r1 = 0.0;
                const double* __restrict x = &in_buf_[consumed * kTargetChannels];
                if (rational_) {
                    const double* __restrict h = &coeffs_[phase_num_ * kTaps];
                    for (int k = 0; k < kTaps; k += 2) {
                        const double t0 = h[k];
                        const double t1 = h[k + 1];
                        acc_l0 += t0 * x[2 * k];
                        acc_r0 += t0 * x[2 * k + 1];
                        acc_l1 += t1 * x[2 * k + 2];
                        acc_r1 += t1 * x[2 * k + 3];
                    }
                } else {
                    const double scaled = phase_pos_ * static_cast<double>(up_);
                    const size_t p0 = std::min<size_t>(static_cast<size_t>(scaled), up_ - 1);
                    const double alpha = scaled - static_cast<double>(p0);
                    const double* __restrict h0 = &coeffs_[p0 * kTaps];
                    const double* __restrict h1 = &coeffs_[(p0 + 1) * kTaps];
                    for (int k = 0; k < kTaps; k += 2) {
                        const double t0 = h0[k] + alpha * (h1[k] - h0[k]);
                        const double t1 = h0[k + 1] + alpha * (h1[k + 1] - h0[k + 1]);
                        acc_l0 += t0 * x[2 * k];
                        acc_r0 += t0 * x[2 * k + 1];
                        acc_l1 += t1 * x[2 * k + 2];
                        acc_r1 += t1 * x[2 * k + 3];
                    }
                }
                out.push_back(acc_l0 + acc_l1);
                out.push_back(acc_r0 + acc_r1);
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
        if (consumed > 0) {
            in_buf_.erase(in_buf_.begin(), in_buf_.begin() + static_cast<std::ptrdiff_t>(consumed * kTargetChannels));
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
    std::vector<double> coeffs_;
    std::vector<double> in_buf_;
};

// Signal-gated, dry-preserving Spectral Band Extender at 48 kHz (187.5 Hz/bin, 256-pt 75% WOLA, 4.0 ms latency).
// Only injects subtle high-frequency air when a sharp codec brickwall is measured (16 kHz AAC / baked Opus, or
// 12 kHz SWB Opus). Uses stereo-linked 4-subband envelope extrapolation with power-complementary existing-HF capping,
// spectral-flatness tonality damping, HF transient flux weighting, and 6-10 kHz speech sibilance ducking.
// Everything below the codec cutoff stays 100% dry and phase-untouched.
class HfExtender {
public:
    static constexpr int kLog2Fft = 8;
    static constexpr int kFft = 1 << kLog2Fft;  // 256 samples = 5.333 ms window for tight transient response
    static constexpr int kHop = kFft / 4;       // 64 samples = 1.333 ms hop (75% COLA overlap)
    static constexpr int kDelay = kFft - kHop;  // 192 samples = 4.000 ms exact COLA alignment
    static constexpr int kNumSub = 4;

    HfExtender() {
        for (int i = 0; i < kFft; ++i) {
            hann_[i] = 0.5 * (1.0 - std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(kFft)));
        }
        for (int i = 0; i < kFft; ++i) {
            int rev = 0;
            for (int b = 0; b < kLog2Fft; ++b) {
                if ((i >> b) & 1) rev |= (1 << (kLog2Fft - 1 - b));
            }
            bitrev_[i] = rev;
        }
        for (int i = 0; i < kFft / 2; ++i) {
            const double ang = -2.0 * kPi * static_cast<double>(i) / static_cast<double>(kFft);
            tw_fwd_[i] = std::complex<double>(std::cos(ang), std::sin(ang));
        }
        reset_all();
    }

    void reset_all() {
        p_low_hi_ema_ = 0.0;
        p_pre16_ema_ = 0.0;
        p_post16_ema_ = 0.0;
        sib_slow_ema_ = 0.0;
        ema_initialized_ = false;
        brickwall_16_ = false;
        blend_ = 0.0;
        mode12_blend_ = 0.0;
        logged_state_ = -1;
        reset_buffers();
    }

    void reset_buffers() {
        in_ring_.fill(0.0);
        ola_buf_.fill(0.0);
        smooth_g16_.fill(0.0);
        smooth_g12a_.fill(0.0);
        smooth_g12b_.fill(0.0);
        hop_fill_ = 0;
        hop_idx_ = 0;
        priming_skip_ = kDelay;
        sib_duck_ = 1.0;
        tonal_guard_ = 1.0;
        src_pwr_ema_ = 0.0;
        trans_boost_ = 1.0;
    }

    int active_mode() const { return (blend_ > 0.05) ? (mode12_blend_ > 0.5 ? 2 : 1) : 0; }

    void process(const double* stereo_in, size_t in_frames, bool enabled, bool swb_hint, bool emit,
                 std::vector<double>& out) {
        if (in_frames == 0) return;
        for (size_t i = 0; i < in_frames; ++i) {
            in_ring_[2 * (kDelay + hop_fill_)] = stereo_in[2 * i];
            in_ring_[2 * (kDelay + hop_fill_) + 1] = stereo_in[2 * i + 1];
            ++hop_fill_;
            if (hop_fill_ == kHop) {
                step_hop(enabled, swb_hint, emit, out);
                hop_fill_ = 0;
            }
        }
    }

    void flush(bool enabled, bool swb_hint, std::vector<double>& out) {
        const int real_buffered = (kDelay - priming_skip_) + hop_fill_;
        if (real_buffered <= 0) return;
        static const double zeros[kFft * kTargetChannels]{};
        const int pad_zeros = kDelay + (hop_fill_ > 0 ? (kHop - hop_fill_) : 0);
        const size_t base_out = out.size();
        process(zeros, static_cast<size_t>(pad_zeros), enabled, swb_hint, true, out);
        const size_t target_size = base_out + static_cast<size_t>(real_buffered) * kTargetChannels;
        if (out.size() > target_size) out.resize(target_size);
        reset_buffers();
    }

private:

    void fft_inplace(std::array<std::complex<double>, kFft>& buf, bool inverse) const {
        for (int i = 0; i < kFft; ++i) {
            const int j = bitrev_[i];
            if (i < j) std::swap(buf[i], buf[j]);
        }
        for (int len = 2; len <= kFft; len <<= 1) {
            const int half = len >> 1;
            const int stride = kFft / len;
            for (int start = 0; start < kFft; start += len) {
                for (int k = 0; k < half; ++k) {
                    const std::complex<double> w = tw_fwd_[k * stride];
                    const double wr = w.real();
                    const double wi = inverse ? -w.imag() : w.imag();
                    const std::complex<double> rhs = buf[start + k + half];
                    const double br = rhs.real();
                    const double bi = rhs.imag();
                    // Direct real/imag FMA avoids GCC Annex-G __muldc3 libgcc call on every butterfly.
                    const std::complex<double> t(wr * br - wi * bi, wr * bi + wi * br);
                    const std::complex<double> u = buf[start + k];
                    buf[start + k] = u + t;
                    buf[start + k + half] = u - t;
                }
            }
        }
        if (inverse) {
            constexpr double inv_n = 1.0 / static_cast<double>(kFft);
            for (auto& v : buf) {
                v = std::complex<double>(v.real() * inv_n, v.imag() * inv_n);
            }
        }
    }

    static double band_avg(const std::array<double, kFft / 2 + 1>& pwr, int lo, int hi) {
        double s = 0.0;
        for (int k = lo; k <= hi; ++k) s += pwr[k];
        return s / static_cast<double>(hi - lo + 1);
    }

    static std::complex<double> rot_quarter(std::complex<double> z, int step) {
        switch (step & 3) {
            case 0: return z;
            case 1: return {-z.imag(), z.real()};
            case 2: return {-z.real(), -z.imag()};
            default: return {z.imag(), -z.real()};
        }
    }

    void synthesize_band_stereo(int src_lo, int sub_w, int shift, int step_offset, int fade_in, int fade_out,
                                double gain_mult, std::array<double, kNumSub>& smooth_gains,
                                std::array<std::complex<double>, kFft>& dst_spec) const {
        // Precomputed 0.5 * (1 - cos(pi * k / (fade + 1))) ramps for fade=3 and fade=5 (zero runtime trig).
        static constexpr double kFade3[3] = {0.14644660940672624, 0.5, 0.8535533905932737};
        static constexpr double kFade5[5] = {0.06698729810778065, 0.25, 0.5, 0.75, 0.9330127018922193};

        std::array<double, kNumSub> src_pwr{};
        std::array<double, kNumSub> src_db{};
        for (int m = 0; m < kNumSub; ++m) {
            const int s_lo = src_lo + m * sub_w;
            const double p = band_avg(pwr_, s_lo, s_lo + sub_w - 1);
            src_pwr[m] = p;
            src_db[m] = 10.0 * std::log10(std::max(p, 1e-24));
        }
        if (src_pwr[kNumSub - 1] < 1e-20) {
            for (double& g : smooth_gains) g *= 0.75;
            return;
        }

        // Stereo-linked local slope + downward curvature extrapolation (clamped so HF always rolls off naturally).
        const double slope_lo = std::clamp(0.5 * (src_db[2] - src_db[0]), -12.0, -0.8);
        const double slope_hi = std::clamp(0.5 * (src_db[3] - src_db[1]), -12.0, -0.8);
        const double curv = std::clamp(slope_hi - slope_lo, -2.5, 0.0);
        const double step_db = std::clamp(slope_hi + 0.35 * curv, -12.0, -1.0);

        constexpr double kTuckDb = -9.0;  // Tucked -9 dB below natural rolloff baseline
        const int dst_lo = src_lo + shift;
        const int dst_hi = dst_lo + kNumSub * sub_w - 1;

        for (int m = 0; m < kNumSub; ++m) {
            const int d_lo = dst_lo + m * sub_w;
            const int d_hi = std::min(d_lo + sub_w - 1, kFft / 2 - 1);
            const double target_db = src_db[kNumSub - 1] + static_cast<double>(step_offset + m + 1) * step_db + kTuckDb;
            const double target_pwr = std::pow(10.0, target_db * 0.1);
            // Power-complementary cap: only synthesize the missing HF deficit above any surviving real HF energy.
            const double exist_pwr = (d_lo <= d_hi) ? band_avg(pwr_, d_lo, d_hi) : 0.0;
            const double deficit_pwr = std::max(0.0, target_pwr - exist_pwr);
            const double raw_g =
                (src_pwr[m] > 1e-22 && deficit_pwr > 0.0) ? std::min(std::sqrt(deficit_pwr / src_pwr[m]), 1.25) : 0.0;
            // Fast 2 ms attack so transients lock immediately; smooth 5 ms release to prevent tail flutter.
            const double alpha = (raw_g > smooth_gains[m]) ? 0.55 : 0.25;
            smooth_gains[m] += alpha * (raw_g - smooth_gains[m]);
        }

        const int rot_step = (shift * hop_idx_) & 3;
        const double inv_sub_w = 1.0 / static_cast<double>(sub_w);
        const double first_center = static_cast<double>(dst_lo) + 0.5 * static_cast<double>(sub_w) - 0.5;
        const double* const f_in_tab = (fade_in == 5) ? kFade5 : kFade3;
        const double* const f_out_tab = (fade_out == 5) ? kFade5 : kFade3;

        for (int b = dst_lo; b <= dst_hi; ++b) {
            const int s = b - shift;
            if (s <= 0 || s >= kFft / 2 || b >= kFft / 2) continue;

            // Smooth piecewise-linear interpolation across the 4 subband centers (no staircase edges).
            const double pos = (static_cast<double>(b) - first_center) * inv_sub_w;
            double sub_g;
            if (pos <= 0.0) {
                sub_g = smooth_gains[0];
            } else if (pos >= static_cast<double>(kNumSub - 1)) {
                sub_g = smooth_gains[kNumSub - 1];
            } else {
                const int idx = static_cast<int>(pos);
                const double frac = pos - static_cast<double>(idx);
                sub_g = smooth_gains[idx] + frac * (smooth_gains[idx + 1] - smooth_gains[idx]);
            }

            double edge = 1.0;
            if (b - dst_lo < fade_in) {
                edge *= f_in_tab[b - dst_lo];
            }
            if (dst_hi - b < fade_out) {
                edge *= f_out_tab[dst_hi - b];
            }

            // Apply identical stereo-linked scalar gain to L and R so stereo panning & phase remain locked.
            const double scale = sub_g * edge * gain_mult;
            if (scale <= 1e-9) continue;
            const std::complex<double> rl = rot_quarter(spec_l_[s], rot_step);
            const std::complex<double> rr = rot_quarter(spec_r_[s], rot_step);
            const double vlr = rl.real() * scale;
            const double vli = rl.imag() * scale;
            const double vrr = rr.real() * scale;
            const double vri = rr.imag() * scale;

            // Pack L and R Hermitian-symmetrically into one complex spectrum: Y[b] = YL[b] + j*YR[b].
            dst_spec[b] += std::complex<double>(vlr - vri, vli + vrr);
            dst_spec[kFft - b] += std::complex<double>(vlr + vri, -vli + vrr);
        }
    }

    void step_hop(bool enabled, bool swb_hint, bool emit, std::vector<double>& out) {
        if (enabled) {
            for (int n = 0; n < kFft; ++n) {
                fft_in_[n] = std::complex<double>(in_ring_[2 * n] * hann_[n], in_ring_[2 * n + 1] * hann_[n]);
            }
            fft_inplace(fft_in_, false);

            // Unpack stereo L/R complex bins [0 .. kFft/2] from single dual-real FFT.
            spec_l_[0] = std::complex<double>(fft_in_[0].real(), 0.0);
            spec_r_[0] = std::complex<double>(fft_in_[0].imag(), 0.0);
            pwr_[0] = std::norm(spec_l_[0]) + std::norm(spec_r_[0]);
            for (int k = 1; k <= kFft / 2; ++k) {
                const std::complex<double> zk = fft_in_[k];
                const std::complex<double> zc = std::conj(fft_in_[kFft - k]);
                spec_l_[k] = 0.5 * (zk + zc);
                spec_r_[k] = std::complex<double>(0.5 * (zk.imag() - zc.imag()), -0.5 * (zk.real() - zc.real()));
                pwr_[k] = std::norm(spec_l_[k]) + std::norm(spec_r_[k]);
            }

            // Shared stereo detection across calibrated 48 kHz bands (df = 187.5 Hz/bin).
            const double p_body = band_avg(pwr_, 2, 16);       // 375 - 3000 Hz
            const double p_sib = band_avg(pwr_, 32, 53);       // 6.0 - 9.94 kHz
            const double p_low_hi = band_avg(pwr_, 45, 61);    // 8.44 - 11.44 kHz
            const double p_swb_pre = band_avg(pwr_, 51, 61);   // 9.56 - 11.44 kHz
            const double p_pre16 = band_avg(pwr_, 67, 80);     // 12.56 - 15.00 kHz
            const double p_src = band_avg(pwr_, 59, 82);       // 11.06 - 15.38 kHz
            const double p_post16 = band_avg(pwr_, 90, 104);   // 16.88 - 19.50 kHz

            if (p_sib > 0.0) {
                sib_slow_ema_ = (sib_slow_ema_ <= 0.0) ? p_sib : (0.9925 * sib_slow_ema_ + 0.0075 * p_sib);
            }
            const bool sib_spike = (sib_slow_ema_ > 1e-11 && p_sib > 2.6 * sib_slow_ema_);
            const bool sib_dominant = (p_sib > 8.0 * std::max(p_src, 1e-15)) && (p_sib > 0.20 * std::max(p_body, 1e-15));
            const double target_duck = (sib_spike || sib_dominant) ? 0.0 : 1.0;
            // Fast 12 ms attack down on fricatives ("S"/"T"), smooth 120 ms release back up.
            const double duck_coef = (target_duck < sib_duck_) ? 0.117 : 0.011;
            sib_duck_ += duck_coef * (target_duck - sib_duck_);

            // Only train the brickwall detector when HF content is musical/non-sibilant and above silence.
            if (!sib_dominant && p_body > 1e-9 && p_pre16 > 1e-11) {
                if (!ema_initialized_) {
                    ema_initialized_ = true;
                    p_low_hi_ema_ = p_low_hi;
                    p_pre16_ema_ = p_pre16;
                    p_post16_ema_ = p_post16;
                } else {
                    p_low_hi_ema_ = 0.996 * p_low_hi_ema_ + 0.004 * p_low_hi;
                    p_pre16_ema_ = 0.996 * p_pre16_ema_ + 0.004 * p_pre16;
                    p_post16_ema_ = 0.996 * p_post16_ema_ + 0.004 * p_post16;
                }
                const double nat_slope = 10.0 * std::log10(std::max(p_low_hi_ema_, 1e-18) / std::max(p_pre16_ema_, 1e-18));
                const double cliff16 = 10.0 * std::log10(std::max(p_pre16_ema_, 1e-18) / std::max(p_post16_ema_, 1e-18));
                brickwall_16_ = (cliff16 >= 20.0) && (cliff16 >= std::max(0.0, nat_slope) + 11.0);
            }

            const bool swb_active = swb_hint && (p_swb_pre > 1e-10) && (p_pre16 < 0.04 * p_swb_pre);
            mode12_blend_ += (swb_active ? 0.02 : -0.01);
            mode12_blend_ = std::clamp(mode12_blend_, 0.0, 1.0);

            const bool want_ext = swb_active || brickwall_16_;
            blend_ += (want_ext ? 0.003 : -0.002);
            blend_ = std::clamp(blend_, 0.0, 1.0);

            const int cur_mode = active_mode();
            if (cur_mode > 0 && logged_state_ != cur_mode) {
                logged_state_ = cur_mode;
                OLOG("audio dsp: HF extension active (%s)",
                     cur_mode == 2 ? "12kHz SWB -> 20kHz" : "16kHz brickwall -> 20kHz");
            }

            const double eff_gain = blend_ * sib_duck_;
            if (eff_gain > 1e-4) {
                const bool use_band12 = (mode12_blend_ > 0.5);

                // Spectral Flatness Measure (SFM) tonality guard + HF transient flux weighting.
                const int sfm_lo = use_band12 ? 43 : 59;
                const int sfm_hi = use_band12 ? 62 : 82;
                const int sfm_n = sfm_hi - sfm_lo + 1;
                double log_sum = 0.0;
                double lin_sum = 0.0;
                for (int k = sfm_lo; k <= sfm_hi; ++k) {
                    const double p = pwr_[k] + 1e-24;
                    log_sum += std::log(p);
                    lin_sum += p;
                }
                const double geo_mean = std::exp(log_sum / static_cast<double>(sfm_n));
                const double arith_mean = lin_sum / static_cast<double>(sfm_n);
                const double sfm = std::clamp(geo_mean / std::max(arith_mean, 1e-24), 0.0, 1.0);
                // Damp pure narrowband tones (~-7 dB) to avoid inharmonic metallic zing; keep broadband air/cymbals at 1.0.
                const double target_tonal = std::clamp(0.45 + 0.55 * ((sfm - 0.06) / 0.44), 0.45, 1.0);
                tonal_guard_ += 0.20 * (target_tonal - tonal_guard_);

                if (arith_mean > 0.0) {
                    src_pwr_ema_ = (src_pwr_ema_ <= 0.0) ? arith_mean : (0.96 * src_pwr_ema_ + 0.04 * arith_mean);
                }
                const double flux = arith_mean / std::max(src_pwr_ema_, 1e-24);
                const double target_trans = std::clamp(1.0 + 0.13 * (flux - 1.3), 1.0, 1.26);
                const double trans_coef = (target_trans > trans_boost_) ? 0.35 : 0.03;
                trans_boost_ += trans_coef * (target_trans - trans_boost_);

                const double shaped_gain = eff_gain * tonal_guard_ * trans_boost_;
                fft_in_.fill(std::complex<double>(0.0, 0.0));
                if (!use_band12) {
                    // Translate [11.06 .. 15.38 kHz] (+24 bins = +4.50 kHz) -> [15.56 .. 19.88 kHz]
                    synthesize_band_stereo(59, 6, 24, 0, 3, 5, shaped_gain, smooth_g16_, fft_in_);
                    for (int m = 0; m < kNumSub; ++m) {
                        smooth_g12a_[m] *= 0.85;
                        smooth_g12b_[m] *= 0.85;
                    }
                } else {
                    // SWB 12 kHz Opus: [8.06 .. 11.63 kHz] -> [11.81 .. 15.38 kHz] (+20) and [15.56 .. 19.13 kHz] (+40)
                    synthesize_band_stereo(43, 5, 20, 0, 3, 3, shaped_gain, smooth_g12a_, fft_in_);
                    synthesize_band_stereo(43, 5, 40, kNumSub, 3, 5, shaped_gain, smooth_g12b_, fft_in_);
                    for (double& g : smooth_g16_) g *= 0.85;
                }
                fft_inplace(fft_in_, true);
                constexpr double inv_ola = 1.0 / 1.5;
                for (int n = 0; n < kFft; ++n) {
                    const double ws = hann_[n] * inv_ola;
                    ola_buf_[2 * n] += fft_in_[n].real() * ws;
                    ola_buf_[2 * n + 1] += fft_in_[n].imag() * ws;
                }
            } else {
                for (int m = 0; m < kNumSub; ++m) {
                    smooth_g16_[m] *= 0.85;
                    smooth_g12a_[m] *= 0.85;
                    smooth_g12b_[m] *= 0.85;
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
                for (int n = start; n < kHop; ++n) {
                    const size_t o = base_out + static_cast<size_t>(n - start) * kTargetChannels;
                    out[o] = in_ring_[2 * n] + ola_buf_[2 * n];
                    out[o + 1] = in_ring_[2 * n + 1] + ola_buf_[2 * n + 1];
                }
            }
        }

        std::memmove(in_ring_.data(), in_ring_.data() + 2 * kHop, static_cast<size_t>(2 * kDelay) * sizeof(double));
        std::memmove(ola_buf_.data(), ola_buf_.data() + 2 * kHop, static_cast<size_t>(2 * kDelay) * sizeof(double));
        std::fill(ola_buf_.begin() + 2 * kDelay, ola_buf_.end(), 0.0);
        hop_idx_ = (hop_idx_ + 1) & 3;
    }

    std::array<double, kFft> hann_{};
    std::array<int, kFft> bitrev_{};
    std::array<std::complex<double>, kFft / 2> tw_fwd_{};

    std::array<double, kFft * kTargetChannels> in_ring_{};
    std::array<double, kFft * kTargetChannels> ola_buf_{};
    std::array<std::complex<double>, kFft> fft_in_{};
    std::array<std::complex<double>, kFft / 2 + 1> spec_l_{};
    std::array<std::complex<double>, kFft / 2 + 1> spec_r_{};
    std::array<double, kFft / 2 + 1> pwr_{};
    std::array<double, kNumSub> smooth_g16_{};
    std::array<double, kNumSub> smooth_g12a_{};
    std::array<double, kNumSub> smooth_g12b_{};

    int hop_fill_ = 0;
    int hop_idx_ = 0;
    int priming_skip_ = kDelay;

    double p_low_hi_ema_ = 0.0;
    double p_pre16_ema_ = 0.0;
    double p_post16_ema_ = 0.0;
    double sib_slow_ema_ = 0.0;
    bool ema_initialized_ = false;
    bool brickwall_16_ = false;
    double blend_ = 0.0;
    double mode12_blend_ = 0.0;
    double sib_duck_ = 1.0;
    double tonal_guard_ = 1.0;
    double src_pwr_ema_ = 0.0;
    double trans_boost_ = 1.0;
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
    std::vector<double> work_in;
    std::vector<double> resampled_buf;
    std::vector<double> out_fifo;
    std::vector<std::vector<uint8_t>> free_bufs;

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

    void pack_samples(const double* stereo, size_t frames, std::vector<uint8_t>& bytes) {
        const size_t bps = static_cast<size_t>(pcm_bits / 8);
        bytes.resize(frames * kTargetChannels * bps);
        uint8_t* dst = bytes.data();

        if (pcm_bits == 32) {
            for (size_t i = 0; i < frames * kTargetChannels; ++i) {
                const double v = guard_peak(stereo[i] * gain);
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
                const double v = guard_peak(stereo[i] * gain) + tpdf() * inv_24;
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
                    const double v = guard_peak(stereo[f * kTargetChannels + c] * gain);
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
        const size_t avail_frames = out_fifo.size() / kTargetChannels;
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
            pack_samples(&out_fifo[offset_frames * kTargetChannels], take, pkt.data);
            out.push_back(std::move(pkt));
            offset_frames += take;
            emitted_frames += static_cast<int64_t>(take);
        }
        if (offset_frames > 0) {
            out_fifo.erase(out_fifo.begin(),
                           out_fifo.begin() + static_cast<std::ptrdiff_t>(offset_frames * kTargetChannels));
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
        int ch = track.channels > 0 ? std::clamp<int>(static_cast<int>(track.channels), 1, 2) : 2;
        int gain_q8 = 0;
        impl_->opus_pre_skip_init = 0;
        if (track.codec_data.size() >= 19 && std::memcmp(track.codec_data.data(), "OpusHead", 8) == 0) {
            ch = std::clamp<int>(static_cast<int>(track.codec_data[9]), 1, 2);
            impl_->opus_pre_skip_init = static_cast<int>(le16(track.codec_data.data() + 10));
            gain_q8 = static_cast<int>(sle16(track.codec_data.data() + 16));
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
        impl_->resampler.configure(impl_->aac_rate);
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
    impl_->out_fifo.clear();
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
        if (fi.samplerate > 0 && static_cast<int>(fi.samplerate) != impl_->aac_rate) {
            impl_->aac_rate = static_cast<int>(fi.samplerate);
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
        impl_->out_fifo.clear();
    } else {
        const int64_t expected_pts =
            impl_->base_pts_ms +
            ((impl_->emitted_frames + static_cast<int64_t>(impl_->out_fifo.size() / kTargetChannels)) * 1000LL +
             kTargetRate / 2) /
                kTargetRate;
        if (!impl_->pts_anchored || std::abs(pts_ms - expected_pts) > 80) {
            if (impl_->pts_anchored && !impl_->out_fifo.empty()) {
                impl_->drain_chunks(out, true);
            }
            impl_->pts_anchored = true;
            impl_->base_pts_ms = pts_ms;
            impl_->emitted_frames = 0;
        }
    }

    impl_->work_in.resize(decoded_frames * kTargetChannels);
    if (dec_ch == 1) {
        for (size_t i = 0; i < decoded_frames; ++i) {
            const double s = static_cast<double>(decoded_ptr[i]);
            impl_->work_in[2 * i] = s;
            impl_->work_in[2 * i + 1] = s;
        }
    } else {
        for (size_t i = 0; i < decoded_frames; ++i) {
            impl_->work_in[2 * i] = static_cast<double>(decoded_ptr[static_cast<size_t>(dec_ch) * i]);
            impl_->work_in[2 * i + 1] = static_cast<double>(decoded_ptr[static_cast<size_t>(dec_ch) * i + 1]);
        }
    }

    // Resample to 48 kHz unconditionally while priming so FIR history warms up smoothly too.
    impl_->resampled_buf.clear();
    impl_->resampler.process(impl_->work_in.data(), decoded_frames, true, impl_->resampled_buf);

    const bool allow_ext = !(impl_->aac && impl_->aac_native_sbr);
    impl_->hf_extender.process(impl_->resampled_buf.data(), impl_->resampled_buf.size() / kTargetChannels, allow_ext,
                               impl_->opus_is_swb, emit_output, impl_->out_fifo);
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
int Pipeline::source_sample_rate() const { return impl_->resampler.source_rate(); }
int Pipeline::pcm_bits() const { return impl_->pcm_bits; }
int Pipeline::hf_mode() const { return impl_->hf_mode.load(std::memory_order_relaxed); }
double Pipeline::attenuation_db() const { return impl_->atten_db; }

}  // namespace omega::audio
