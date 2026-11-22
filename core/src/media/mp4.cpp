#include "media/mp4.h"

#include <algorithm>
#include <cstring>

namespace omega::media {
namespace {

constexpr uint32_t fourcc(const char (&s)[5]) {
    return (uint32_t(uint8_t(s[0])) << 24) | (uint32_t(uint8_t(s[1])) << 16) | (uint32_t(uint8_t(s[2])) << 8) |
           uint32_t(uint8_t(s[3]));
}

// Bounds-checked big-endian cursor over a box payload.
struct Cursor {
    const uint8_t* p;
    const uint8_t* end;

    size_t left() const { return static_cast<size_t>(end - p); }
    bool has(size_t n) const { return left() >= n; }
    void skip(size_t n) { p += std::min(n, left()); }
    uint8_t u8() { return has(1) ? *p++ : 0; }
    uint16_t u16() {
        if (!has(2)) return static_cast<uint16_t>(u8() << 8);
        uint16_t v = static_cast<uint16_t>((uint16_t(p[0]) << 8) | p[1]);
        p += 2;
        return v;
    }
    uint32_t u32() {
        if (!has(4)) { uint32_t v = 0; while (has(1)) v = (v << 8) | *p++; return v; }
        uint32_t v = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
        p += 4;
        return v;
    }
    uint64_t u64() { uint64_t hi = u32(); return hi << 32 | u32(); }
};

// Iterates child boxes of a container payload.
template <typename Fn>
void for_each_box(Cursor c, Fn&& fn) {
    while (c.has(8)) {
        const uint8_t* start = c.p;
        uint64_t size = c.u32();
        uint32_t type = c.u32();
        size_t header = 8;
        if (size == 1) {
            size = c.u64();
            header = 16;
        } else if (size == 0) {
            size = static_cast<uint64_t>(c.end - start);
        }
        if (size < header || size > static_cast<uint64_t>(c.end - start)) return;
        fn(type, Cursor{start + header, start + size});
        c.p = start + size;
    }
}

// MPEG-4 descriptor length (1-4 bytes, 7 bits each).
uint32_t descriptor_len(Cursor& c) {
    uint32_t len = 0;
    for (int i = 0; i < 4; ++i) {
        uint8_t b = c.u8();
        len = len << 7 | (b & 0x7f);
        if (!(b & 0x80)) break;
    }
    return len;
}

std::vector<uint8_t> parse_esds_asc(Cursor c) {
    c.skip(4);  // version + flags
    while (c.has(2)) {
        uint8_t tag = c.u8();
        uint32_t len = descriptor_len(c);
        if (tag == 0x03) {  // ES_Descriptor
            c.skip(2);
            uint8_t flags = c.u8();
            if (flags & 0x80) c.skip(2);
            if (flags & 0x40) c.skip(c.u8());
            if (flags & 0x20) c.skip(2);
        } else if (tag == 0x04) {  // DecoderConfigDescriptor
            c.skip(13);
        } else if (tag == 0x05) {  // DecoderSpecificInfo = AudioSpecificConfig
            len = std::min<uint32_t>(len, static_cast<uint32_t>(c.left()));
            return {c.p, c.p + len};
        } else {
            c.skip(len);
        }
    }
    return {};
}

// Inspects a buffered moof payload without mutating reader state to extract total sample count and duration.
void peek_moof_timing(std::span<const uint8_t> moof, uint32_t default_dur, uint64_t& out_samples,
                      uint64_t& out_duration) {
    out_samples = 0;
    out_duration = 0;
    Cursor root{moof.data(), moof.data() + moof.size()};
    for_each_box(root, [&](uint32_t type, Cursor traf) {
        if (type != fourcc("traf")) return;
        uint32_t def_duration = default_dur;
        for_each_box(traf, [&](uint32_t t, Cursor c) {
            if (t == fourcc("tfhd")) {
                uint32_t flags = c.u32() & 0xffffff;
                c.skip(4);  // track_ID
                if (flags & 0x1) c.skip(8);
                if (flags & 0x2) c.skip(4);
                if (flags & 0x8) def_duration = c.u32();
            } else if (t == fourcc("trun")) {
                uint32_t vf = c.u32();
                uint32_t flags = vf & 0xffffff;
                uint32_t count = c.u32();
                if (flags & 0x1) c.skip(4);
                if (flags & 0x4) c.skip(4);
                size_t entry_bytes = ((flags & 0x100) ? 4 : 0) + ((flags & 0x200) ? 4 : 0) +
                                     ((flags & 0x400) ? 4 : 0) + ((flags & 0x800) ? 4 : 0);
                uint32_t max_count = entry_bytes ? static_cast<uint32_t>(c.left() / entry_bytes) : std::min<uint32_t>(count, 65536);
                count = std::min(count, max_count);
                for (uint32_t i = 0; i < count; ++i) {
                    uint32_t d = (flags & 0x100) ? c.u32() : def_duration;
                    if (flags & 0x200) c.skip(4);
                    if (flags & 0x400) c.skip(4);
                    if (flags & 0x800) c.skip(4);
                    if (d > 0) {
                        ++out_samples;
                        out_duration += d;
                    }
                }
            }
        });
    });
}

constexpr uint64_t kMaxBoxBytes = 128ULL << 20;

// Overflow-safe (a * b) / c for 64-bit timestamps without relying on __int128 (unavailable on 32-bit ARM).
uint64_t mul_div_u64(uint64_t a, uint64_t b, uint64_t c) {
    if (c == 0) return 0;
    if (b == c) return a;
    return (a / c) * b + ((a % c) * b) / c;
}

int64_t mul_div_i64(int64_t a, uint64_t b, uint64_t c) {
    if (c == 0) return 0;
    if (b == c) return a;
    bool neg = a < 0;
    uint64_t mag = neg ? (static_cast<uint64_t>(-(a + 1)) + 1ULL) : static_cast<uint64_t>(a);
    uint64_t res = mul_div_u64(mag, b, c);
    return neg ? -static_cast<int64_t>(res) : static_cast<int64_t>(res);
}

}  // namespace

bool Fmp4Reader::read_exact(std::vector<uint8_t>& buf, uint64_t n) {
    if (n > kMaxBoxBytes) {
        error_ = "box too large";
        return false;
    }
    buf.resize(static_cast<size_t>(n));
    size_t got = n ? read_(buf.data(), buf.size()) : 0;
    offset_ += got;
    return got == n;
}

bool Fmp4Reader::skip(uint64_t n) {
    uint8_t tmp[16 * 1024];
    while (n > 0) {
        size_t want = static_cast<size_t>(std::min<uint64_t>(n, sizeof tmp));
        size_t got = read_(tmp, want);
        offset_ += got;
        if (got != want) return false;
        n -= got;
    }
    return true;
}

bool Fmp4Reader::read_box_header(uint32_t& type, uint64_t& payload) {
    uint8_t h[16];
    size_t got = read_(h, 8);
    offset_ += got;
    if (got != 8) return false;
    uint64_t size = (uint32_t(h[0]) << 24) | (uint32_t(h[1]) << 16) | (uint32_t(h[2]) << 8) | h[3];
    type = (uint32_t(h[4]) << 24) | (uint32_t(h[5]) << 16) | (uint32_t(h[6]) << 8) | h[7];
    uint64_t header = 8;
    if (size == 1) {
        if (read_(h + 8, 8) != 8) return false;
        offset_ += 8;
        size = 0;
        for (int i = 8; i < 16; ++i) size = size << 8 | h[i];
        header = 16;
    }
    if (size < header) {
        error_ = "bad box size";
        return false;
    }
    payload = size - header;
    return true;
}

bool Fmp4Reader::read_header() {
    std::vector<uint8_t> buf;
    for (;;) {
        uint32_t type;
        uint64_t payload;
        if (!read_box_header(type, payload)) {
            if (error_.empty()) error_ = "eof before moov";
            return false;
        }
        if (!read_exact(buf, payload)) {
            error_ = "truncated box";
            return false;
        }
        if (type == fourcc("moov")) {
            if (!parse_moov(buf)) return false;
            break;
        }
    }
    const bool is_video =
        (track_.codec == Codec::H264 || track_.codec == Codec::VP9 || track_.codec == Codec::AV1);
    if (is_video && track_.default_duration > 0) {
        set_track_fps(track_, track_.timescale, track_.default_duration);
    }
    // Peek at the next box: YouTube (and ffmpeg's global_sidx) put the segment index right here.
    uint64_t start = offset_;
    uint32_t type;
    uint64_t payload;
    if (!read_box_header(type, payload)) return true;  // header-only file
    uint64_t payload_start = offset_;
    if (!read_exact(buf, payload)) return true;
    if (type == fourcc("sidx")) {
        parse_sidx(buf, offset_);
        // If trex had no default duration, peek the first moof once (kept in pending_ for read_fragment).
        if (is_video && track_.fps_num == 0) {
            uint64_t next_start = offset_;
            uint32_t next_type = 0;
            uint64_t next_payload = 0;
            if (read_box_header(next_type, next_payload)) {
                uint64_t next_payload_start = offset_;
                if (read_exact(buf, next_payload)) {
                    has_pending_ = true;
                    pending_type_ = next_type;
                    pending_start_ = next_start;
                    pending_payload_start_ = next_payload_start;
                    pending_.swap(buf);
                }
            }
        }
    } else {
        has_pending_ = true;
        pending_type_ = type;
        pending_start_ = start;
        pending_payload_start_ = payload_start;
        pending_.swap(buf);
    }
    if (is_video && track_.fps_num == 0 && has_pending_ && pending_type_ == fourcc("moof")) {
        uint64_t samples = 0, total_dur = 0;
        peek_moof_timing(pending_, track_.default_duration, samples, total_dur);
        if (samples > 0 && total_dur > 0) {
            set_track_fps(track_, static_cast<uint64_t>(track_.timescale) * samples, total_dur);
        }
    }
    return true;
}

void Fmp4Reader::parse_sidx(std::span<const uint8_t> sidx, uint64_t sidx_end) {
    Cursor c{sidx.data(), sidx.data() + sidx.size()};
    uint8_t version = c.u8();
    c.skip(3 + 4);  // flags, reference_ID
    uint32_t timescale = c.u32();
    uint64_t earliest = version == 0 ? c.u32() : c.u64();
    uint64_t first_offset = version == 0 ? c.u32() : c.u64();
    c.skip(2);
    uint16_t count = c.u16();
    if (timescale == 0) return;
    segments_.reserve(segments_.size() + count);
    uint64_t offset = sidx_end + first_offset;
    uint64_t time = earliest;
    for (uint16_t i = 0; i < count && c.has(12); ++i) {
        uint32_t size = c.u32() & 0x7fffffff;
        uint32_t duration = c.u32();
        c.skip(4);  // SAP info
        int64_t media_shift = mul_div_i64(track_.media_time, timescale, track_.timescale);
        int64_t start_ticks = static_cast<int64_t>(time) - media_shift;
        uint64_t dts = mul_div_u64(time, track_.timescale, timescale);
        int64_t start_ms = static_cast<int64_t>(mul_div_u64(static_cast<uint64_t>(std::max<int64_t>(start_ticks, 0)),
                                                            1000ULL, timescale));
        segments_.push_back({offset, start_ms, dts});
        offset += size;
        time += duration;
    }
}

bool Fmp4Reader::parse_moov(std::span<const uint8_t> moov) {
    Cursor root{moov.data(), moov.data() + moov.size()};
    for_each_box(root, [&](uint32_t type, Cursor c) {
        if (type == fourcc("mvex")) {
            for_each_box(c, [&](uint32_t t, Cursor e) {
                if (t != fourcc("trex")) return;
                e.skip(4 + 4 + 4);  // version/flags, track_ID, default_sample_description_index
                track_.default_duration = e.u32();
                track_.default_size = e.u32();
                track_.default_flags = e.u32();
            });
        } else if (type == fourcc("trak")) {
            for_each_box(c, [&](uint32_t t, Cursor trak) {
                if (t == fourcc("edts")) {
                    for_each_box(trak, [&](uint32_t et, Cursor e) {
                        if (et != fourcc("elst")) return;
                        uint8_t version = e.u8();
                        e.skip(3);
                        uint32_t count = e.u32();
                        for (uint32_t i = 0; i < count; ++i) {
                            int64_t media_time;
                            if (version == 1) {
                                e.skip(8);
                                media_time = static_cast<int64_t>(e.u64());
                            } else {
                                e.skip(4);
                                media_time = static_cast<int32_t>(e.u32());
                            }
                            e.skip(4);
                            if (media_time >= 0) {
                                track_.media_time = media_time;
                                break;
                            }
                        }
                    });
                } else if (t == fourcc("mdia")) {
                    for_each_box(trak, [&](uint32_t mt, Cursor mdia) {
                        if (mt == fourcc("mdhd")) {
                            uint8_t version = mdia.u8();
                            mdia.skip(3 + (version == 1 ? 16 : 8));
                            track_.timescale = mdia.u32();
                        } else if (mt == fourcc("minf")) {
                            for_each_box(mdia, [&](uint32_t it, Cursor minf) {
                                if (it != fourcc("stbl")) return;
                                for_each_box(minf, [&](uint32_t st, Cursor stbl) {
                                    if (st != fourcc("stsd")) return;
                                    stbl.skip(8);  // version/flags + entry_count
                                    for_each_box(stbl, [&](uint32_t entry, Cursor se) {
                                        uint32_t config = 0;  // codec configuration box of a video entry
                                        if (entry == fourcc("avc1") || entry == fourcc("avc3")) {
                                            track_.codec = Codec::H264;
                                            config = fourcc("avcC");
                                        } else if (entry == fourcc("av01")) {
                                            track_.codec = Codec::AV1;
                                            config = fourcc("av1C");
                                        } else if (entry == fourcc("vp09")) {
                                            track_.codec = Codec::VP9;
                                            config = fourcc("vpcC");
                                        }
                                        if (config) {
                                            se.skip(24);
                                            track_.width = se.u16();
                                            track_.height = se.u16();
                                            se.skip(50);
                                            for_each_box(se, [&](uint32_t ct, Cursor cc) {
                                                if (ct == config) {
                                                    if (config == fourcc("av1C")) {
                                                        if (cc.left() >= 4) {
                                                            uint8_t flags = cc.p[2];
                                                            if (flags & 0x40) track_.bits_per_channel = 10;
                                                            if (flags & 0x20) track_.bits_per_channel = 12;
                                                            uint8_t csp = flags & 0x03;  // chroma_sample_position: 1=vertical (left,half), 2=colocated (left,top)
                                                            if (csp == 1) {
                                                                track_.chroma_siting_horz = 1;
                                                                track_.chroma_siting_vert = 2;
                                                            } else if (csp == 2) {
                                                                track_.chroma_siting_horz = 1;
                                                                track_.chroma_siting_vert = 1;
                                                            }
                                                            cc.skip(4);
                                                        }
                                                        track_.codec_data.assign(cc.p, cc.end);
                                                    } else if (config == fourcc("vpcC")) {
                                                        // ISO/IEC 14496-15 VPCodecConfigurationBox (FullBox v1: 4B version/flags + 8B record).
                                                        if (cc.left() >= 12 && cc.p[0] == 1) {
                                                            uint8_t depth = cc.p[6] >> 4;
                                                            if (depth == 8 || depth == 10 || depth == 12) track_.bits_per_channel = depth;
                                                            uint8_t sub = (cc.p[6] >> 1) & 0x07;  // 0=4:2:0 vertical (left,half), 1=4:2:0 colocated (left,top)
                                                            if (sub == 0) {
                                                                track_.chroma_siting_horz = 1;
                                                                track_.chroma_siting_vert = 2;
                                                            } else if (sub == 1) {
                                                                track_.chroma_siting_horz = 1;
                                                                track_.chroma_siting_vert = 1;
                                                            }
                                                            if (!track_.has_color) {
                                                                uint8_t prim = cc.p[7], tc = cc.p[8], mat = cc.p[9];
                                                                if (prim != 2 || tc != 2) {
                                                                    track_.has_color = true;
                                                                    track_.color_primaries = prim;
                                                                    track_.transfer_characteristics = tc;
                                                                    track_.matrix_coefficients = mat;
                                                                    track_.color_range = (cc.p[6] & 0x01) ? 2 : 1;
                                                                }
                                                            }
                                                        }
                                                    } else {
                                                        track_.codec_data.assign(cc.p, cc.end);
                                                    }
                                                } else if (ct == fourcc("colr")) {
                                                    if (cc.left() >= 11) {
                                                        uint32_t type = cc.u32();
                                                        if (type == fourcc("nclx")) {
                                                            track_.has_color = true;
                                                            track_.color_primaries = cc.u16();
                                                            track_.transfer_characteristics = cc.u16();
                                                            track_.matrix_coefficients = cc.u16();
                                                            // ISO BMFF nclx full_range_flag (0=limited, 1=full) -> Matroska Range (1=limited, 2=full).
                                                            track_.color_range = (cc.u8() >> 7) ? 2 : 1;
                                                        }
                                                    }
                                                } else if (ct == fourcc("mdcv")) {
                                                    if (cc.left() >= 24) {
                                                        track_.has_mdcv = true;
                                                        for (int i = 0; i < 3; ++i) {
                                                            track_.display_primaries_x[i] = cc.u16();
                                                            track_.display_primaries_y[i] = cc.u16();
                                                        }
                                                        track_.white_point_x = cc.u16();
                                                        track_.white_point_y = cc.u16();
                                                        track_.max_luminance = cc.u32();
                                                        track_.min_luminance = cc.u32();
                                                    }
                                                } else if (ct == fourcc("clli")) {
                                                    if (cc.left() >= 4) {
                                                        track_.has_clli = true;
                                                        track_.max_cll = cc.u16();
                                                        track_.max_fall = cc.u16();
                                                    }
                                                }
                                            });
                                            update_hdr_flag(track_);
                                        } else if (entry == fourcc("mp4a")) {
                                            track_.codec = Codec::AAC;
                                            se.skip(16);
                                            track_.channels = se.u16();
                                            se.skip(6);
                                            track_.sample_rate = se.u32() >> 16;
                                            for_each_box(se, [&](uint32_t ct, Cursor cc) {
                                                if (ct == fourcc("esds")) track_.codec_data = parse_esds_asc(cc);
                                            });
                                        }
                                    });
                                });
                            });
                        }
                    });
                }
            });
        }
    });
    // VP9 and AV1 (with empty configOBUs) decoders need no configuration record (frame headers carry sequence OBUs).
    if (track_.codec == Codec::Unknown || track_.timescale == 0 ||
        (track_.codec_data.empty() && track_.codec != Codec::VP9 && track_.codec != Codec::AV1)) {
        error_ = "unsupported or incomplete moov";
        return false;
    }
    return true;
}

bool Fmp4Reader::parse_moof(std::span<const uint8_t> moof, uint64_t moof_start) {
    runs_.clear();
    Cursor root{moof.data(), moof.data() + moof.size()};
    for_each_box(root, [&](uint32_t type, Cursor traf) {
        if (type != fourcc("traf")) return;
        uint64_t base = moof_start;
        uint64_t next_data_pos = moof_start;
        bool has_prev_run = false;
        uint32_t def_duration = track_.default_duration, def_size = track_.default_size, def_flags = track_.default_flags;
        for_each_box(traf, [&](uint32_t t, Cursor c) {
            if (t == fourcc("tfhd")) {
                uint32_t flags = c.u32() & 0xffffff;
                c.skip(4);  // track_ID
                if (flags & 0x1) {
                    base = c.u64();
                    next_data_pos = base;
                }
                if (flags & 0x2) c.skip(4);
                if (flags & 0x8) def_duration = c.u32();
                if (flags & 0x10) def_size = c.u32();
                if (flags & 0x20) def_flags = c.u32();
            } else if (t == fourcc("tfdt")) {
                uint8_t version = c.u8();
                c.skip(3);
                next_dts_ = version == 1 ? c.u64() : c.u32();
            } else if (t == fourcc("trun")) {
                uint32_t vf = c.u32();
                uint8_t version = uint8_t(vf >> 24);
                uint32_t flags = vf & 0xffffff;
                uint32_t count = c.u32();
                Run run;
                if (flags & 0x1) {
                    int32_t data_offset = static_cast<int32_t>(c.u32());
                    run.data_start = base + static_cast<uint64_t>(static_cast<int64_t>(data_offset));
                } else {
                    run.data_start = has_prev_run ? next_data_pos : base;
                }
                uint32_t first_flags = (flags & 0x4) ? c.u32() : def_flags;
                size_t entry_bytes = ((flags & 0x100) ? 4 : 0) + ((flags & 0x200) ? 4 : 0) +
                                     ((flags & 0x400) ? 4 : 0) + ((flags & 0x800) ? 4 : 0);
                uint32_t max_count = entry_bytes ? static_cast<uint32_t>(c.left() / entry_bytes) : std::min<uint32_t>(count, 65536);
                count = std::min(count, max_count);
                run.durations.reserve(count);
                run.sizes.reserve(count);
                run.flags.reserve(count);
                run.ctos.reserve(count);
                uint64_t run_bytes = 0;
                for (uint32_t i = 0; i < count; ++i) {
                    run.durations.push_back((flags & 0x100) ? c.u32() : def_duration);
                    uint32_t sz = (flags & 0x200) ? c.u32() : def_size;
                    run.sizes.push_back(sz);
                    run_bytes += sz;
                    uint32_t sf = (flags & 0x400) ? c.u32() : (i == 0 && (flags & 0x4) ? first_flags : def_flags);
                    run.flags.push_back(sf);
                    uint32_t cto = (flags & 0x800) ? c.u32() : 0;
                    run.ctos.push_back(version == 0 ? static_cast<int32_t>(std::min<uint32_t>(cto, INT32_MAX))
                                                    : static_cast<int32_t>(cto));
                }
                next_data_pos = run.data_start + run_bytes;
                has_prev_run = true;
                runs_.push_back(std::move(run));
            }
        });
    });
    return !runs_.empty();
}

bool Fmp4Reader::read_fragment(std::deque<Sample>& out) {
    auto& buf = frag_buf_;
    for (;;) {
        uint64_t box_start = 0, payload_start = 0, payload = 0;
        uint32_t type = 0;
        bool from_pending = false;
        if (has_pending_) {
            has_pending_ = false;
            from_pending = true;
            type = pending_type_;
            box_start = pending_start_;
            payload_start = pending_payload_start_;
            buf.swap(pending_);
            payload = buf.size();
        } else {
            box_start = offset_;
            if (!read_box_header(type, payload)) return false;
            payload_start = offset_;
        }

        if (type == fourcc("moof")) {
            if (!from_pending && !read_exact(buf, payload)) {
                error_ = "truncated box";
                return false;
            }
            if (!parse_moof(buf, box_start)) {
                error_ = "moof without trun";
                return false;
            }
        } else if (type == fourcc("mdat") && !runs_.empty()) {
            if (payload > kMaxBoxBytes) {
                error_ = "box too large";
                return false;
            }
            const uint64_t mdat_end = payload_start + payload;
            const double to_ms = 1000.0 / track_.timescale;
            const size_t initial_out = out.size();
            const uint64_t initial_dts = next_dts_;

            // Fast zero-copy path (100% of YouTube DASH fMP4): verify runs are monotonic inside mdat
            // and stream each sample straight into Sample::data without retaining a multi-MB frag_buf_.
            bool can_stream = !from_pending && mdat_end >= payload_start;
            if (can_stream) {
                uint64_t cursor = payload_start;
                for (const Run& run : runs_) {
                    uint64_t at = run.data_start;
                    if (at < cursor || at > mdat_end) {
                        can_stream = false;
                        break;
                    }
                    for (uint32_t sz : run.sizes) {
                        if (sz > mdat_end - at) {
                            can_stream = false;
                            break;
                        }
                        at += sz;
                    }
                    if (!can_stream) break;
                    cursor = at;
                }
            }

            if (can_stream) {
                constexpr size_t kStreamBatch = 12;
                bool streamed_any = false;
                auto rollback_if_unflushed = [&] {
                    if (!streamed_any) {
                        out.resize(initial_out);
                        next_dts_ = initial_dts;
                    }
                };
                for (const Run& run : runs_) {
                    if (run.data_start > offset_ && !skip(run.data_start - offset_)) {
                        rollback_if_unflushed();
                        error_ = "truncated box";
                        return false;
                    }
                    for (size_t i = 0; i < run.sizes.size(); ++i) {
                        Sample s;
                        const size_t sz = run.sizes[i];
                        if (sz > 0) {
                            s.data.allocate(sz, pool_.get());
                            size_t got = read_(s.data.data(), sz);
                            offset_ += got;
                            if (got != sz) {
                                rollback_if_unflushed();
                                error_ = "truncated box";
                                return false;
                            }
                        }
                        int64_t pts = static_cast<int64_t>(next_dts_) + run.ctos[i] - track_.media_time;
                        s.pts_ms = static_cast<int64_t>(std::max<int64_t>(pts, 0) * to_ms + 0.5);
                        s.duration_ms = static_cast<int64_t>(run.durations[i] * to_ms + 0.5);
                        s.key = !(run.flags[i] & 0x10000);
                        out.push_back(std::move(s));
                        next_dts_ += run.durations[i];
                        if (batch_cb_ && out.size() >= kStreamBatch) {
                            streamed_any = true;
                            if (!batch_cb_(out)) {
                                runs_.clear();
                                return false;
                            }
                        }
                    }
                }
                if (mdat_end > offset_ && !skip(mdat_end - offset_)) {
                    rollback_if_unflushed();
                    error_ = "truncated box";
                    return false;
                }
                runs_.clear();
                return true;
            }

            // Fallback for non-monotonic runs or pre-buffered mdat.
            if (!from_pending && !read_exact(buf, payload)) {
                error_ = "truncated box";
                return false;
            }
            const uint64_t buf_end = payload_start + buf.size();
            for (const Run& run : runs_) {
                uint64_t at = run.data_start;
                for (size_t i = 0; i < run.sizes.size(); ++i) {
                    if (at < payload_start || at > buf_end || run.sizes[i] > buf_end - at) {
                        out.resize(initial_out);
                        next_dts_ = initial_dts;
                        error_ = "sample outside mdat";
                        return false;
                    }
                    Sample s;
                    const uint8_t* src = buf.data() + (at - payload_start);
                    s.data.assign(src, src + run.sizes[i], pool_.get());
                    int64_t pts = static_cast<int64_t>(next_dts_) + run.ctos[i] - track_.media_time;
                    s.pts_ms = static_cast<int64_t>(std::max<int64_t>(pts, 0) * to_ms + 0.5);
                    s.duration_ms = static_cast<int64_t>(run.durations[i] * to_ms + 0.5);
                    s.key = !(run.flags[i] & 0x10000);
                    out.push_back(std::move(s));
                    next_dts_ += run.durations[i];
                    at += run.sizes[i];
                }
            }
            runs_.clear();
            if (frag_buf_.capacity() > 256 * 1024) std::vector<uint8_t>().swap(frag_buf_);
            return true;
        } else if (!from_pending) {
            if (!skip(payload)) {
                error_ = "truncated box";
                return false;
            }
        }
    }
}

}  // namespace omega::media
