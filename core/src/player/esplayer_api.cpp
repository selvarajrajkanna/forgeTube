#include "player/esplayer_api.h"

#include <dlfcn.h>
#if defined(__linux__)
#include <elf.h>
#include <link.h>
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "util/log.h"

namespace omega::esp {
namespace {

Api g_api{};
const Api* g_loaded = nullptr;
std::string g_extra_knobs = "esp: not loaded";

std::string to_lower_ascii(std::string_view sv) {
    std::string out(sv);
    for (char& ch : out) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return out;
}

bool contains_ci(std::string_view hay, std::string_view needle) {
    if (needle.empty() || hay.size() < needle.size()) return false;
    std::string h = to_lower_ascii(hay);
    return h.find(needle) != std::string::npos;
}

bool is_known_esplayer_symbol(std::string_view name) {
    static constexpr std::string_view kKnown[] = {
        "esplusplayer_create",
        "esplusplayer_open",
        "esplusplayer_close",
        "esplusplayer_destroy",
        "esplusplayer_prepare_async",
        "esplusplayer_start",
        "esplusplayer_stop",
        "esplusplayer_pause",
        "esplusplayer_resume",
        "esplusplayer_seek",
        "esplusplayer_set_playback_rate",
        "esplusplayer_get_state",
        "esplusplayer_set_ecore_display",
        "esplusplayer_set_display_mode",
        "esplusplayer_set_app_info",
        "esplusplayer_set_app_info_ex",
        "esplusplayer_set_sound_stream_info",
        "esplusplayer_set_audio_stream_info",
        "esplusplayer_set_video_stream_info",
        "esplusplayer_set_video_scan_type",
        "esplusplayer_submit_packet",
        "esplusplayer_submit_eos_packet",
        "esplusplayer_get_playing_time",
        "esplusplayer_get_adaptive_info",
        "esplusplayer_get_virtual_rsc_id",
        "esplusplayer_set_error_cb",
        "esplusplayer_set_buffer_status_cb",
        "esplusplayer_set_eos_cb",
        "esplusplayer_set_ready_to_prepare_cb",
        "esplusplayer_set_prepare_async_done_cb",
        "esplusplayer_set_resource_conflicted_cb",
        "esplusplayer_set_ready_to_seek_cb",
        "esplusplayer_set_seek_done_cb",
        "esplusplayer_set_video_frame_dropped_cb",
        "esplusplayer_get_error_string",
    };
    for (std::string_view k : kKnown) {
        if (name == k) return true;
    }
    return false;
}

bool is_actionable_export(std::string_view short_name) {
    if (short_name.rfind("unset_", 0) == 0 || short_name.rfind("get_size_of_", 0) == 0) return false;
    if (short_name.size() >= 3 && short_name.substr(short_name.size() - 3) == "_cb") return false;
    return true;
}

bool is_sync_or_hdmi_keyword(std::string_view short_name) {
    static constexpr std::string_view kWords[] = {"lat", "delay", "sync", "hdmi", "arc", "run", "route", "av", "pcm"};
    std::string low = to_lower_ascii(short_name);
    for (std::string_view kw : kWords) {
        if (low.find(kw) != std::string::npos) return true;
    }
    return false;
}

void log_chunked(const char* prefix, const std::vector<std::string>& items) {
    if (items.empty()) return;
    std::string line;
    for (size_t i = 0; i < items.size(); ++i) {
        if (!line.empty() && line.size() + 2 + items[i].size() > 700) {
            OLOG("%s: %s", prefix, line.c_str());
            line.clear();
        }
        if (!line.empty()) line += ", ";
        line += items[i];
    }
    if (!line.empty()) OLOG("%s: %s", prefix, line.c_str());
}

std::string wrap_list_for_ui(const std::string& header, const std::vector<std::string>& items, size_t max_col) {
    std::string out = header;
    size_t col = header.size();
    for (size_t i = 0; i < items.size(); ++i) {
        std::string sep = (i > 0) ? ", " : "";
        if (i > 0 && col + sep.size() + items[i].size() > max_col) {
            out += ",\n  ";
            out += items[i];
            col = 2 + items[i].size();
        } else {
            out += sep;
            out += items[i];
            col += sep.size() + items[i].size();
        }
    }
    return out;
}

bool scan_elf_exports(const char* lib_match, const char* fallback_path,
                      bool (*keep_fn)(std::string_view), std::vector<std::string>& out,
                      std::string& resolved_path, const char*& err_tag) {
    out.clear();
#if !defined(__linux__)
    (void)lib_match;
    (void)fallback_path;
    (void)keep_fn;
    resolved_path.clear();
    err_tag = "unsupported";
    return false;
#else
    struct FindLib {
        const char* needle;
        std::string path;
    } found{lib_match, {}};
    dl_iterate_phdr(
        [](struct dl_phdr_info* info, size_t, void* data) -> int {
            auto* f = static_cast<FindLib*>(data);
            if (!info || !info->dlpi_name || !*info->dlpi_name) return 0;
            if (std::strstr(info->dlpi_name, f->needle)) {
                f->path = info->dlpi_name;
                return 1;
            }
            return 0;
        },
        &found);
    if (found.path.empty() || found.path.find('/') == std::string::npos) {
        found.path = fallback_path;
    }
    resolved_path = found.path;

    std::FILE* fp = std::fopen(found.path.c_str(), "rb");
    if (!fp) {
        err_tag = "read denied";
        return false;
    }

    auto read_ok = [&]() -> bool {
        ElfW(Ehdr) eh{};
        if (std::fread(&eh, 1, sizeof(eh), fp) != sizeof(eh)) return false;
        if (std::memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0) return false;
        constexpr unsigned char kExpectedClass = (sizeof(void*) == 8) ? ELFCLASS64 : ELFCLASS32;
        if (eh.e_ident[EI_CLASS] != kExpectedClass) return false;
        if (static_cast<size_t>(eh.e_shentsize) != sizeof(ElfW(Shdr)) || eh.e_shoff == 0 || eh.e_shnum == 0 ||
            eh.e_shnum > 4096)
            return false;

        if (std::fseek(fp, static_cast<long>(eh.e_shoff), SEEK_SET) != 0) return false;
        std::vector<ElfW(Shdr)> shdrs(eh.e_shnum);
        if (std::fread(shdrs.data(), sizeof(ElfW(Shdr)), eh.e_shnum, fp) != static_cast<size_t>(eh.e_shnum))
            return false;

        for (const ElfW(Shdr)& sh : shdrs) {
            if (sh.sh_type != SHT_DYNSYM) continue;
            if (sh.sh_entsize != sizeof(ElfW(Sym)) || sh.sh_size == 0 || sh.sh_size > (4u << 20)) continue;
            if (sh.sh_link >= shdrs.size()) continue;
            const ElfW(Shdr)& str_sh = shdrs[sh.sh_link];
            if (str_sh.sh_type != SHT_STRTAB || str_sh.sh_size == 0 || str_sh.sh_size > (4u << 20)) continue;

            std::vector<ElfW(Sym)> syms(sh.sh_size / sizeof(ElfW(Sym)));
            std::vector<char> strs(str_sh.sh_size);
            if (std::fseek(fp, static_cast<long>(sh.sh_offset), SEEK_SET) != 0) return false;
            if (std::fread(syms.data(), sizeof(ElfW(Sym)), syms.size(), fp) != syms.size()) return false;
            if (std::fseek(fp, static_cast<long>(str_sh.sh_offset), SEEK_SET) != 0) return false;
            if (std::fread(strs.data(), 1, strs.size(), fp) != strs.size()) return false;
            strs.back() = '\0';

            for (const ElfW(Sym)& sym : syms) {
                if (sym.st_shndx == SHN_UNDEF || (sym.st_info >> 4) == STB_LOCAL) continue;
                if (sym.st_name == 0 || sym.st_name >= strs.size()) continue;
                std::string_view name(&strs[sym.st_name]);
                if (keep_fn(name)) out.emplace_back(name);
            }
            return true;
        }
        return false;
    };

    bool ok = read_ok();
    std::fclose(fp);
    if (!ok) {
        err_tag = "no dynsym";
        return false;
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    err_tag = nullptr;
    return true;
#endif
}

void inspect_esplayer_elf() {
    std::vector<std::string> all_exports;
    std::string esp_path;
    const char* err_tag = nullptr;
    if (!scan_elf_exports(
            "libesplusplayer.so", "/usr/lib/libesplusplayer.so",
            [](std::string_view name) {
                return name.rfind("esplusplayer_", 0) == 0 || name.rfind("esplayer_", 0) == 0 ||
                       contains_ci(name, "dvde");
            },
            all_exports, esp_path, err_tag)) {
        g_extra_knobs = std::string("esp: ") + (err_tag ? err_tag : "unavail");
        OLOG("esplusplayer: export scan failed for %s (%s)", esp_path.c_str(), err_tag ? err_tag : "-");
        return;
    }

    std::vector<std::string> all_unknown;
    std::vector<std::string> actionable;
    for (const std::string& full : all_exports) {
        if (is_known_esplayer_symbol(full)) continue;
        std::string short_name = (full.rfind("esplusplayer_", 0) == 0) ? full.substr(13) : full;
        all_unknown.push_back(short_name);
        if (is_actionable_export(short_name)) actionable.push_back(short_name);
    }

    OLOG("esplusplayer exports (%s): %zu total, %zu unmapped, %zu actionable", esp_path.c_str(),
         all_exports.size(), all_unknown.size(), actionable.size());
    log_chunked("esp unmapped", all_unknown);

    std::string summary;
    if (!actionable.empty()) {
        summary = wrap_list_for_ui("esp +" + std::to_string(actionable.size()) + ": ", actionable, 122);
    } else {
        summary = "esp: 0 extra (" + std::to_string(all_unknown.size()) + " cb/meta / " +
                  std::to_string(all_exports.size()) + " total)";
    }

    std::vector<std::string> sm_exports;
    std::string sm_path;
    const char* sm_err = nullptr;
    if (scan_elf_exports(
            "libcapi-media-sound-manager.so", "/usr/lib/libcapi-media-sound-manager.so.0",
            [](std::string_view name) { return name.rfind("sound_manager_", 0) == 0; },
            sm_exports, sm_path, sm_err)) {
        std::vector<std::string> sm_sync;
        for (const std::string& full : sm_exports) {
            std::string short_name = (full.rfind("sound_manager_", 0) == 0) ? full.substr(14) : full;
            if (is_sync_or_hdmi_keyword(short_name)) sm_sync.push_back(short_name);
        }
        if (!sm_sync.empty()) {
            log_chunked("sound_manager sync/route", sm_sync);
            summary += "\n  " + wrap_list_for_ui("snd +" + std::to_string(sm_sync.size()) + ": ", sm_sync, 122);
        }
    }
    g_extra_knobs = std::move(summary);
}

template <typename T>
bool bind(void* lib, T& fn, const char* name) {
    fn = reinterpret_cast<T>(dlsym(lib, name));
    if (!fn) OLOG("esplusplayer: missing symbol %s", name);
    return fn != nullptr;
}

template <typename T>
void bind_optional(void* lib, T& fn, const char* name) {
    if (lib) fn = reinterpret_cast<T>(dlsym(lib, name));
}

void load() {
    void* lib = dlopen("libesplusplayer.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        OLOG("esplusplayer: dlopen failed: %s", dlerror());
        return;
    }
    bool ok = true;
    ok &= bind(lib, g_api.create, "esplusplayer_create");
    ok &= bind(lib, g_api.open, "esplusplayer_open");
    ok &= bind(lib, g_api.close, "esplusplayer_close");
    ok &= bind(lib, g_api.destroy, "esplusplayer_destroy");
    ok &= bind(lib, g_api.prepare_async, "esplusplayer_prepare_async");
    ok &= bind(lib, g_api.start, "esplusplayer_start");
    ok &= bind(lib, g_api.stop, "esplusplayer_stop");
    ok &= bind(lib, g_api.pause, "esplusplayer_pause");
    ok &= bind(lib, g_api.resume, "esplusplayer_resume");
    ok &= bind(lib, g_api.seek, "esplusplayer_seek");
    ok &= bind(lib, g_api.set_playback_rate, "esplusplayer_set_playback_rate");
    ok &= bind(lib, g_api.get_state, "esplusplayer_get_state");
    ok &= bind(lib, g_api.set_ecore_display, "esplusplayer_set_ecore_display");
    ok &= bind(lib, g_api.set_display_mode, "esplusplayer_set_display_mode");
    ok &= bind(lib, g_api.set_app_info, "esplusplayer_set_app_info");
    bind_optional(lib, g_api.set_app_info_ex, "esplusplayer_set_app_info_ex");
    bind_optional(lib, g_api.set_sound_stream_info, "esplusplayer_set_sound_stream_info");
    ok &= bind(lib, g_api.set_audio_stream_info, "esplusplayer_set_audio_stream_info");
    ok &= bind(lib, g_api.set_video_stream_info, "esplusplayer_set_video_stream_info");
    bind_optional(lib, g_api.set_video_scan_type, "esplusplayer_set_video_scan_type");
    ok &= bind(lib, g_api.submit_packet, "esplusplayer_submit_packet");
    ok &= bind(lib, g_api.submit_eos_packet, "esplusplayer_submit_eos_packet");
    ok &= bind(lib, g_api.get_playing_time, "esplusplayer_get_playing_time");
    bind_optional(lib, g_api.get_adaptive_info, "esplusplayer_get_adaptive_info");
    bind_optional(lib, g_api.get_virtual_rsc_id, "esplusplayer_get_virtual_rsc_id");
    ok &= bind(lib, g_api.set_error_cb, "esplusplayer_set_error_cb");
    ok &= bind(lib, g_api.set_buffer_status_cb, "esplusplayer_set_buffer_status_cb");
    ok &= bind(lib, g_api.set_eos_cb, "esplusplayer_set_eos_cb");
    ok &= bind(lib, g_api.set_ready_to_prepare_cb, "esplusplayer_set_ready_to_prepare_cb");
    ok &= bind(lib, g_api.set_prepare_async_done_cb, "esplusplayer_set_prepare_async_done_cb");
    ok &= bind(lib, g_api.set_resource_conflicted_cb, "esplusplayer_set_resource_conflicted_cb");
    ok &= bind(lib, g_api.set_ready_to_seek_cb, "esplusplayer_set_ready_to_seek_cb");
    ok &= bind(lib, g_api.set_seek_done_cb, "esplusplayer_set_seek_done_cb");
    bind_optional(lib, g_api.set_video_frame_dropped_cb, "esplusplayer_set_video_frame_dropped_cb");
    ok &= bind(lib, g_api.get_error_string, "esplusplayer_get_error_string");

    void* snd = dlopen("libcapi-media-sound-manager.so.0", RTLD_LAZY | RTLD_LOCAL);
    if (!snd) snd = dlopen("libcapi-media-sound-manager.so", RTLD_LAZY | RTLD_LOCAL);
    bind_optional(snd ? snd : lib, g_api.create_sound_stream_info, "sound_manager_create_stream_information");
    bind_optional(snd ? snd : lib, g_api.destroy_sound_stream_info, "sound_manager_destroy_stream_information");

    inspect_esplayer_elf();
    if (ok) g_loaded = &g_api;
}

using BusGetSyncFn = void* (*)(int bus_type, void* cancellable, void** error);
using BusCallSyncFn = void* (*)(void* conn, const char* bus_name, const char* object_path, const char* interface_name,
                                const char* method_name, void* parameters, const void* reply_type, int flags,
                                int timeout_msec, void* cancellable, void** error);
using VariantPrintFn = char* (*)(void* value, int type_annotate);
using VariantUnrefFn = void (*)(void* value);
using FreeFn = void (*)(void* mem);

struct GioApi {
    BusGetSyncFn bus_get_sync = nullptr;
    BusCallSyncFn bus_call_sync = nullptr;
    VariantPrintFn variant_print = nullptr;
    VariantUnrefFn variant_unref = nullptr;
    FreeFn g_free_fn = nullptr;
    void* sys_bus = nullptr;
    bool ready = false;
};

GioApi g_gio{};
std::mutex g_bt_mutex;
int g_last_bt_delay = 0;
int g_last_logged_bt_delay = -1;
std::chrono::steady_clock::time_point g_last_bt_query{};

void load_gio() {
    void* gio = dlopen("libgio-2.0.so.0", RTLD_LAZY | RTLD_LOCAL);
    if (!gio) gio = dlopen("libgio-2.0.so", RTLD_LAZY | RTLD_LOCAL);
    void* glib = dlopen("libglib-2.0.so.0", RTLD_LAZY | RTLD_LOCAL);
    if (!glib) glib = dlopen("libglib-2.0.so", RTLD_LAZY | RTLD_LOCAL);
    if (!glib) glib = gio;
    if (!gio) return;
    bind_optional(gio, g_gio.bus_get_sync, "g_bus_get_sync");
    bind_optional(gio, g_gio.bus_call_sync, "g_dbus_connection_call_sync");
    bind_optional(glib, g_gio.variant_print, "g_variant_print");
    bind_optional(glib, g_gio.variant_unref, "g_variant_unref");
    bind_optional(glib, g_gio.g_free_fn, "g_free");
    if (g_gio.bus_get_sync && g_gio.bus_call_sync && g_gio.variant_print && g_gio.variant_unref && g_gio.g_free_fn) {
        g_gio.sys_bus = g_gio.bus_get_sync(1 /* G_BUS_TYPE_SYSTEM */, nullptr, nullptr);
        g_gio.ready = (g_gio.sys_bus != nullptr);
    }
}

}  // namespace

const Api* api() {
    static std::once_flag once;
    std::call_once(once, load);
    return g_loaded;
}

const std::string& extra_knobs_summary() {
    api();
    return g_extra_knobs;
}

int query_bt_audio_delay_ms() {
    static std::once_flag gio_once;
    std::call_once(gio_once, load_gio);
    if (!g_gio.ready) return -1;

    std::lock_guard<std::mutex> lock(g_bt_mutex);
    auto now = std::chrono::steady_clock::now();
    if (g_last_bt_query != std::chrono::steady_clock::time_point{} &&
        now - g_last_bt_query < std::chrono::seconds(2)) {
        return g_last_bt_delay;
    }
    g_last_bt_query = now;

    void* res = g_gio.bus_call_sync(g_gio.sys_bus, "org.bluez", "/", "org.freedesktop.DBus.ObjectManager",
                                    "GetManagedObjects", nullptr, nullptr, 0, 200, nullptr, nullptr);
    if (!res) {
        g_last_bt_delay = -1;
        return -1;
    }

    char* text = g_gio.variant_print(res, 1);
    g_gio.variant_unref(res);
    if (!text) {
        g_last_bt_delay = -1;
        return -1;
    }

    unsigned long best_raw = 0;
    bool found_active = false;
    bool picked_active = false;
    const char* cursor = text;
    const char* kIface = "org.bluez.MediaTransport1";
    while ((cursor = std::strstr(cursor, kIface)) != nullptr) {
        cursor += std::strlen(kIface);
        const char* brace = std::strchr(cursor, '{');
        if (!brace) break;
        int depth = 0;
        const char* end = brace;
        for (; *end != '\0'; ++end) {
            if (*end == '{') ++depth;
            else if (*end == '}' && --depth == 0) {
                ++end;
                break;
            }
        }
        std::string_view dict(brace, static_cast<size_t>(end - brace));
        bool is_active = dict.find("'State': <'active'>") != std::string_view::npos;
        bool is_pending = dict.find("'State': <'pending'>") != std::string_view::npos;
        if (!is_active && !is_pending) {
            cursor = end;
            continue;
        }
        found_active = true;
        size_t dpos = dict.find("'Delay': <");
        if (dpos != std::string_view::npos) {
            size_t vpos = dpos + 10;
            while (vpos < dict.size() && dict[vpos] != '>' && !std::isdigit(static_cast<unsigned char>(dict[vpos]))) {
                ++vpos;
            }
            if (vpos < dict.size() && std::isdigit(static_cast<unsigned char>(dict[vpos]))) {
                unsigned long raw = std::strtoul(dict.data() + vpos, nullptr, 10);
                if (raw > 0 && (is_active || !picked_active)) {
                    best_raw = raw;
                    if (is_active) picked_active = true;
                }
            }
        }
        cursor = end;
    }
    g_gio.g_free_fn(text);

    if (!found_active) {
        g_last_bt_delay = 0;
        return 0;
    }
    // BlueZ MediaTransport1.Delay is in 1/10 ms (100 us) units; add 30 ms for A2DP encoder latency.
    // If an active A2DP transport omits DelayReport, fall back to 180 ms only while streaming.
    int delay_ms = (best_raw > 0) ? std::clamp(static_cast<int>((best_raw + 5) / 10) + 30, 40, 600) : 180;
    g_last_bt_delay = delay_ms;
    if (delay_ms != g_last_logged_bt_delay) {
        g_last_logged_bt_delay = delay_ms;
        OLOG("bt delay: active BlueZ MediaTransport1 Delay=%lu (0.1ms) -> %d ms", best_raw, delay_ms);
    }
    return delay_ms;
}

}  // namespace omega::esp
