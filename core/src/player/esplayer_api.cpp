#include "player/esplayer_api.h"

#include <dlfcn.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>

#include "util/log.h"

namespace omega::esp {
namespace {

Api g_api{};
const Api* g_loaded = nullptr;
std::string g_extra_knobs = "esp probe: not loaded";

void probe_sync_apis(void* lib, void* snd) {
    auto has = [](void* h, const char* sym) -> const char* {
        return (h && dlsym(h, sym) != nullptr) ? "ok" : "no";
    };
    g_extra_knobs = std::string("esp probe: get_audio_latency=") + has(lib, "esplusplayer_get_audio_latency") +
                    " get_off=" + has(lib, "esplusplayer_get_render_time_offset") +
                    " set_off=" + has(lib, "esplusplayer_set_render_time_offset") +
                    " ll=" + has(lib, "esplusplayer_set_low_latency_mode") +
                    " ars=" + has(snd, "sound_manager_set_avoid_resampling");
    OLOG("%s", g_extra_knobs.c_str());
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

    probe_sync_apis(lib, snd);
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
