#include "yt/js_solver.h"

#include <quickjs.h>

#include <dirent.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string_view>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

#include "net/http.h"
#include "util/log.h"
#include "util/no_destroy.h"

// The EJS solver bundles, embedded at build time (CMakeLists.txt).
extern const char omega_ejs_lib[];
extern const char omega_ejs_core[];

namespace omega::yt {
namespace {

using Clock = std::chrono::steady_clock;

constexpr const char* kTvPage = "https://www.youtube.com/tv";
constexpr const char* kTvUserAgent =
    "Mozilla/5.0 (SMART-TV; Linux; Tizen 2.4.0) AppleWebKit/538.1 (KHTML, like Gecko) Version/2.4.0 TV Safari/538.1";

std::string exception_text(JSContext* ctx) {
    JSValue e = JS_GetException(ctx);
    const char* s = JS_ToCString(ctx, e);
    std::string out = s ? s : "unknown JS error";
    if (s) JS_FreeCString(ctx, s);
    JSValue stack = JS_IsObject(e) ? JS_GetPropertyStr(ctx, e, "stack") : JS_UNDEFINED;
    if (JS_IsString(stack)) {
        const char* st = JS_ToCString(ctx, stack);
        if (st) {
            out += " | " + std::string(st).substr(0, 300);
            JS_FreeCString(ctx, st);
        }
    }
    JS_FreeValue(ctx, stack);
    JS_FreeValue(ctx, e);
    return out;
}

// Evaluates null-terminated `code` (global scope); out receives the result as a string. False with error on exception.
bool eval_cstr(JSContext* ctx, const char* code, size_t len, const char* name, std::string* out, std::string& error) {
    JSValue v = JS_Eval(ctx, code, len, name, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        error = std::string(name) + ": " + exception_text(ctx);
        return false;
    }
    if (out) {
        size_t slen = 0;
        const char* s = JS_ToCStringLen(ctx, &slen, v);
        if (!s) {
            JS_FreeValue(ctx, v);
            error = std::string(name) + ": result is not a string";
            return false;
        }
        out->assign(s, slen);
        JS_FreeCString(ctx, s);
    }
    JS_FreeValue(ctx, v);
    return true;
}

bool eval_string(JSContext* ctx, const std::string& code, const char* name, std::string* out, std::string& error) {
    return eval_cstr(ctx, code.c_str(), code.size(), name, out, error);
}

void set_global_string(JSContext* ctx, const char* name, const std::string& value) {
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, name, JS_NewStringLen(ctx, value.data(), value.size()));
    JS_FreeValue(ctx, global);
}

JSRuntime* new_runtime() {
    JSRuntime* rt = JS_NewRuntime();
    if (!rt) return nullptr;
    JS_SetMemoryLimit(rt, 128u << 20);
    JS_SetMaxStackSize(rt, 512u << 10);  // stays safely inside 1-2 MiB 32-bit ARM worker thread stacks
    return rt;
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return "";
    const std::streamoff sz = in.tellg();
    if (sz <= 0) return "";
    std::string out(static_cast<size_t>(sz), '\0');
    in.seekg(0, std::ios::beg);
    if (!in.read(out.data(), sz)) return "";
    return out;
}

bool write_file(const std::string& path, const std::string& data) {
    std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out || !out.write(data.data(), static_cast<std::streamsize>(data.size()))) return false;
    }
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string url_decode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && hex(s[i + 1]) >= 0 && hex(s[i + 2]) >= 0) {
            out += static_cast<char>(hex(s[i + 1]) << 4 | hex(s[i + 2]));
            i += 2;
        } else {
            out += s[i] == '+' ? ' ' : s[i];
        }
    }
    return out;
}

std::string url_encode(std::string_view s) {
    static const char* digits = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() + 16);
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += digits[c >> 4];
            out += digits[c & 15];
        }
    }
    return out;
}

// key=value&... -> value of `key` (decoded), "" if absent.
std::string query_value(std::string_view query, std::string_view key) {
    size_t start = 0;
    while (start < query.size()) {
        size_t amp = query.find('&', start);
        if (amp == std::string_view::npos) amp = query.size();
        size_t eq = query.find('=', start);
        if (eq != std::string_view::npos && eq < amp && query.substr(start, eq - start) == key)
            return url_decode(query.substr(eq + 1, amp - eq - 1));
        start = amp + 1;
    }
    return "";
}

// "/s/player/<id>/tv-player-ias.vflset/tv-player-ias.js" referenced by the TV page.
bool find_player(std::string& id, std::string& url, std::string& error) {
    net::Request req;
    req.url = kTvPage;
    req.headers = {{"User-Agent", kTvUserAgent}};
    net::Response page = net::fetch(req);
    if (!page.ok()) {
        error = "tv page: " + (page.error.empty() ? "http " + std::to_string(page.status) : page.error);
        return false;
    }
    const std::string marker = "/s/player/";
    for (size_t at = page.body.find(marker); at != std::string::npos; at = page.body.find(marker, at + 1)) {
        size_t end = page.body.find_first_of("\"'", at);
        if (end == std::string::npos) break;
        std::string path = page.body.substr(at, end - at);
        if (path.size() < 3 || path.compare(path.size() - 3, 3, ".js") != 0) continue;
        size_t id_begin = marker.size(), id_end = path.find('/', id_begin);
        if (id_end == std::string::npos) continue;
        id = path.substr(id_begin, id_end - id_begin);
        url = "https://www.youtube.com" + path;
        return true;
    }
    error = "player js not found on the TV page";
    return false;
}

int parse_sts(const std::string& js) {
    const std::string key = "signatureTimestamp:";
    size_t at = js.find(key);
    return at == std::string::npos ? 0 : atoi(js.c_str() + at + key.size());
}

}  // namespace

JsSolver::~JsSolver() { reset(); }

void JsSolver::reset() {
    if (rt_) JS_UpdateStackTop(rt_);
    if (ctx_) JS_FreeContext(ctx_);
    if (rt_) JS_FreeRuntime(rt_);
    ctx_ = nullptr;
    rt_ = nullptr;
}

bool JsSolver::preprocess(std::string&& player_js, std::string& preprocessed, std::string& error) {
    JSRuntime* rt = new_runtime();
    JSContext* ctx = rt ? JS_NewContext(rt) : nullptr;
    if (!ctx) {
        if (rt) JS_FreeRuntime(rt);
        error = "quickjs: out of memory";
        return false;
    }
    // lib.min.js defines `lib` (meriyah + astring); core.min.js expects them as globals.
    static constexpr const char kAssignLib[] = "Object.assign(globalThis, lib);";
    bool ok = eval_cstr(ctx, omega_ejs_lib, std::strlen(omega_ejs_lib), "ejs-lib", nullptr, error) &&
              eval_cstr(ctx, kAssignLib, sizeof(kAssignLib) - 1, "ejs-assign", nullptr, error) &&
              eval_cstr(ctx, omega_ejs_core, std::strlen(omega_ejs_core), "ejs-core", nullptr, error);
    if (ok) {
        set_global_string(ctx, "_player", player_js);
        std::string().swap(player_js);  // free the ~2.5 MB C++ buffer before QuickJS builds the AST
        // A throwaway n challenge checks that the functions were really found.
        ok = eval_string(ctx,
                         "(() => { const p = _player; delete globalThis._player;"
                         " const r = jsc({type: 'player', player: p, output_preprocessed: true,"
                         " requests: [{type: 'n', challenges: ['forgetubeprobe00']}]});"
                         " const n = r.responses[0];"
                         " if (n.type !== 'result') throw new Error('n: ' + n.error);"
                         " return r.preprocessed_player; })()",
                         "ejs-preprocess", &preprocessed, error);
    }
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
#if defined(__GLIBC__)
    malloc_trim(0);
#endif
    return ok;
}

bool JsSolver::load(std::string&& preprocessed, std::string& error, std::string* bytecode_out) {
    reset();
    rt_ = new_runtime();
    ctx_ = rt_ ? JS_NewContext(rt_) : nullptr;
    if (!ctx_) {
        reset();
        error = "quickjs: out of memory";
        return false;
    }
    // Compile once into a QuickJS script object so we can both execute it and cache its bytecode to disk.
    static constexpr const char kPrefix[] = "var _fns = {n: null, sig: null}; (function(_result){\n";
    static constexpr const char kSuffix[] = "\n})(_fns); (typeof _fns.n) + ',' + (typeof _fns.sig);";
    std::string src;
    src.reserve(sizeof(kPrefix) + preprocessed.size() + sizeof(kSuffix));
    src.append(kPrefix, sizeof(kPrefix) - 1);
    src.append(preprocessed);
    std::string().swap(preprocessed);  // free the caller's ~2.5 MB buffer immediately
    src.append(kSuffix, sizeof(kSuffix) - 1);

    JSValue compiled = JS_Eval(ctx_, src.c_str(), src.size(), "ejs-load", JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    std::string().swap(src);
    if (JS_IsException(compiled)) {
        error = "ejs-load: " + exception_text(ctx_);
        reset();
        return false;
    }
    if (bytecode_out) {
        size_t bc_len = 0;
        if (uint8_t* bc = JS_WriteObject(ctx_, &bc_len, compiled, JS_WRITE_OBJ_BYTECODE)) {
            bytecode_out->assign(reinterpret_cast<const char*>(bc), bc_len);
            js_free(ctx_, bc);
        }
    }
    JSValue v = JS_EvalFunction(ctx_, compiled);  // consumes `compiled`
    if (JS_IsException(v)) {
        error = "ejs-load: " + exception_text(ctx_);
        reset();
        return false;
    }
    std::string kinds;
    if (const char* s = JS_ToCString(ctx_, v)) {
        kinds = s;
        JS_FreeCString(ctx_, s);
    }
    JS_FreeValue(ctx_, v);
    if (kinds.compare(0, 8, "function") != 0) {
        error = "ejs-load: no n function (" + kinds + ")";
        reset();
        return false;
    }
    // Collect temporary bootstrap IIFE / Function cyclic garbage immediately (~5-15 MiB).
    JS_RunGC(rt_);
#if defined(__GLIBC__)
    malloc_trim(0);
#endif
    return true;
}

bool JsSolver::load_bytecode(const std::string& bytecode, std::string& error) {
    reset();
    if (bytecode.empty()) {
        error = "ejs-bc: empty bytecode";
        return false;
    }
    rt_ = new_runtime();
    ctx_ = rt_ ? JS_NewContext(rt_) : nullptr;
    if (!ctx_) {
        reset();
        error = "quickjs: out of memory";
        return false;
    }
    JSValue compiled = JS_ReadObject(ctx_, reinterpret_cast<const uint8_t*>(bytecode.data()), bytecode.size(),
                                     JS_READ_OBJ_BYTECODE);
    if (JS_IsException(compiled)) {
        error = "ejs-bc: " + exception_text(ctx_);
        reset();
        return false;
    }
    JSValue v = JS_EvalFunction(ctx_, compiled);  // consumes `compiled`
    if (JS_IsException(v)) {
        error = "ejs-bc: " + exception_text(ctx_);
        reset();
        return false;
    }
    std::string kinds;
    if (const char* s = JS_ToCString(ctx_, v)) {
        kinds = s;
        JS_FreeCString(ctx_, s);
    }
    JS_FreeValue(ctx_, v);
    if (kinds.compare(0, 8, "function") != 0) {
        error = "ejs-bc: no n function (" + kinds + ")";
        reset();
        return false;
    }
    JS_RunGC(rt_);
#if defined(__GLIBC__)
    malloc_trim(0);
#endif
    return true;
}

bool JsSolver::solve(const char* type, const std::string& challenge, std::string& out, std::string& error) {
    if (!ctx_) {
        error = "solver not loaded";
        return false;
    }
    // QuickJS measures its stack limit from the thread that created the runtime; each video asks from
    // another thread, whose stack can sit anywhere: without this, spurious "Maximum call stack size exceeded".
    JS_UpdateStackTop(rt_);
    JSValue global = JS_GetGlobalObject(ctx_);
    JSValue fns = JS_GetPropertyStr(ctx_, global, "_fns");
    JSValue fn = JS_GetPropertyStr(ctx_, fns, type);
    bool ok = false;
    if (!JS_IsFunction(ctx_, fn)) {
        error = std::string("no ") + type + " function";
    } else {
        JSValue arg = JS_NewStringLen(ctx_, challenge.data(), challenge.size());
        JSValue r = JS_Call(ctx_, fn, JS_UNDEFINED, 1, &arg);
        JS_FreeValue(ctx_, arg);
        if (JS_IsException(r)) {
            error = std::string(type) + ": " + exception_text(ctx_);
        } else if (!JS_IsString(r)) {
            error = std::string(type) + ": non-string result";
        } else if (const char* s = JS_ToCString(ctx_, r)) {
            out = s;
            JS_FreeCString(ctx_, s);
            ok = !out.empty();
            if (!ok) error = std::string(type) + ": empty result";
        }
        JS_FreeValue(ctx_, r);
    }
    JS_FreeValue(ctx_, fn);
    JS_FreeValue(ctx_, fns);
    JS_FreeValue(ctx_, global);
    return ok;
}

void Decipher::set_cache_dir(const std::string& dir) {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_dir_ = dir;
}

bool Decipher::ensure(std::string& error) {
    std::string cache_dir;
    std::string known_id;
    bool had_solver = false;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        while (loading_) {
            if (auto* ct = net::CancelScope::current(); ct && ct->is_aborted()) {
                error = "cancelled";
                return false;
            }
            cv_.wait_for(lock, std::chrono::milliseconds(100));
        }
        if (solver_ && Clock::now() - checked_ < std::chrono::hours(1)) return true;
        loading_ = true;
        cache_dir = cache_dir_;
        known_id = player_id_;
        had_solver = (solver_ != nullptr);
    }

    struct LoadingGuard {
        std::mutex& mu;
        std::condition_variable& cv;
        bool& flag;
        ~LoadingGuard() {
            {
                std::lock_guard<std::mutex> lk(mu);
                flag = false;
            }
            cv.notify_all();
        }
    } guard{mutex_, cv_, loading_};

    std::string id, url;
    if (!find_player(id, url, error)) return had_solver;  // keep using the known player offline
    const auto now = Clock::now();
    if (had_solver && id == known_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        checked_ = now;
        return true;
    }

    // Release the previous QuickJS runtime before allocating a new preprocess/load heap on player rotation.
    if (had_solver) {
        std::lock_guard<std::mutex> lock(mutex_);
        solver_.reset();
        player_id_.clear();
        n_cache_.clear();
    }
    auto t0 = Clock::now();
    std::string base = cache_dir.empty() ? "" : cache_dir + "/player-" + id;
    const std::string bc_ext = "-qjs0180.bc";
    int sts = base.empty() ? 0 : atoi(read_file(base + ".sts").c_str());
    auto solver = std::make_unique<JsSolver>();
    bool loaded_bc = false;
    if (!base.empty() && sts != 0) {
        std::string bc = read_file(base + bc_ext);
        if (!bc.empty()) {
            std::string bc_err;
            loaded_bc = solver->load_bytecode(bc, bc_err);
            std::string().swap(bc);
            if (!loaded_bc) OLOG("decipher: cached bytecode rejected (%s), recompiling", bc_err.c_str());
        }
    }
    if (!loaded_bc) {
        std::string preprocessed = base.empty() ? "" : read_file(base + ".js");
        if (preprocessed.empty() || sts == 0) {
            net::Request req;
            req.url = url;
            req.headers = {{"User-Agent", kTvUserAgent}};
            net::Response js = net::fetch(req);
            if (!js.ok()) {
                error = "player js: " + (js.error.empty() ? "http " + std::to_string(js.status) : js.error);
                return false;
            }
            sts = parse_sts(js.body);
            OLOG("decipher: player %s (%zu bytes, sts %d), preprocessing...", id.c_str(), js.body.size(), sts);
            if (!JsSolver::preprocess(std::move(js.body), preprocessed, error)) return false;
            if (!base.empty() && (!write_file(base + ".js", preprocessed) || !write_file(base + ".sts", std::to_string(sts)))) {
                OLOG("decipher: could not cache %s", base.c_str());
            } else if (!cache_dir.empty()) {
                // Prune older player-<id>.* files so cache storage never grows unbounded across YouTube rotations.
                const std::string keep_js = "player-" + id + ".js";
                const std::string keep_sts = "player-" + id + ".sts";
                const std::string keep_bc = "player-" + id + bc_ext;
                if (DIR* dir = opendir(cache_dir.c_str())) {
                    while (dirent* ent = readdir(dir)) {
                        std::string name(ent->d_name);
                        if (name.rfind("player-", 0) == 0 && name != keep_js && name != keep_sts && name != keep_bc) {
                            std::remove((cache_dir + "/" + name).c_str());
                        }
                    }
                    closedir(dir);
                }
            }
        }
        std::string bytecode;
        if (!solver->load(std::move(preprocessed), error, base.empty() ? nullptr : &bytecode)) return false;
        if (!base.empty() && !bytecode.empty()) {
            write_file(base + bc_ext, bytecode);
        }
    }
    OLOG("decipher: player %s ready in %lld ms%s", id.c_str(),
         static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count()),
         loaded_bc ? " (bytecode)" : "");
    std::lock_guard<std::mutex> lock(mutex_);
    checked_ = now;
    solver_ = std::move(solver);
    player_id_ = id;
    sts_ = sts;
    n_cache_.clear();
    return true;
}

int Decipher::signature_timestamp() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (loading_) cv_.wait_for(lock, std::chrono::milliseconds(100));
    return sts_;
}

bool Decipher::solve_cached(const char* type, const std::string& challenge, std::string& out, std::string& error) {
    bool is_n = (type[0] == 'n' && type[1] == '\0');
    if (is_n) {
        auto it = n_cache_.find(challenge);
        if (it != n_cache_.end()) {
            out = it->second;
            return true;
        }
    }
    if (!solver_ || !solver_->solve(type, challenge, out, error)) {
        if (!solver_) error = "decipher not ready";
        return false;
    }
    if (is_n) {
        if (n_cache_.size() >= 256) n_cache_.clear();
        n_cache_[challenge] = out;
    }
    return true;
}

bool Decipher::stream_url(const std::string& url, const std::string& signature_cipher, std::string& out,
                          std::string& error) {
    std::unique_lock<std::mutex> lock(mutex_);
    while (loading_) {
        if (auto* ct = net::CancelScope::current(); ct && ct->is_aborted()) {
            error = "cancelled";
            return false;
        }
        cv_.wait_for(lock, std::chrono::milliseconds(100));
    }
    out = url;
    if (out.empty() && !signature_cipher.empty()) {
        out = query_value(signature_cipher, "url");
        std::string s = query_value(signature_cipher, "s");
        std::string sp = query_value(signature_cipher, "sp");
        std::string sig;
        if (out.empty() || s.empty() || !solve_cached("sig", s, sig, error)) {
            if (error.empty()) error = "bad signatureCipher";
            return false;
        }
        out += "&" + (sp.empty() ? std::string("signature") : sp) + "=" + url_encode(sig);
    }
    if (out.empty()) {
        error = "format without url";
        return false;
    }
    // Throttling parameter: n=<challenge> -> n=<answer>.
    size_t q = out.find('?');
    for (size_t at = q; at != std::string::npos; at = out.find('&', at + 1)) {
        if (out.compare(at + 1, 2, "n=") != 0) continue;
        size_t begin = at + 3, end = out.find('&', begin);
        std::string challenge = url_decode(out.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
        std::string answer;
        if (!solve_cached("n", challenge, answer, error)) return false;
        out.replace(begin, (end == std::string::npos ? out.size() : end) - begin, url_encode(answer));
        break;
    }
    return true;
}

Decipher& shared_decipher() {
    static omega::NoDestroy<Decipher> d;
    return *d;
}

}  // namespace omega::yt
