#include "util/remote_config.h"

#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>

#include "net/http.h"
#include "util/log.h"
#include "util/no_destroy.h"

extern const char omega_remote_config_default[];  // generated from ../remote-config.json (CMakeLists.txt)

namespace omega::cfg {
namespace {

using json = nlohmann::json;

struct CfgState {
    std::mutex mutex;
    json config;
    std::string cache_path;
    std::string language = "en", region = "IN";
};

CfgState& state() {
    static omega::NoDestroy<CfgState> s;
    return *s;
}

// Objects merge key by key; anything else replaces.
void merge(json& base, const json& over) {
    if (!base.is_object() || !over.is_object()) {
        base = over;
        return;
    }
    for (auto it = over.begin(); it != over.end(); ++it) merge(base[it.key()], it.value());
}

const json* lookup(const json& root, std::initializer_list<const char*> path) {
    const json* node = &root;
    for (const char* key : path) {
        if (!node->is_object() || !node->contains(key)) return nullptr;
        node = &(*node)[key];
    }
    return node;
}

}  // namespace

void init(const std::string& cache_path) {
    json config = json::parse(omega_remote_config_default, nullptr, false);
    if (!config.is_object()) config = json::object();
    std::ifstream in(cache_path);
    if (in) {
        std::stringstream ss;
        ss << in.rdbuf();
        json cached = json::parse(ss.str(), nullptr, false);
        if (cached.is_object()) merge(config, cached);
    }
    CfgState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.cache_path = cache_path;
    s.config = std::move(config);
}

bool refresh(std::string& error) {
    std::string repo = str({"updates", "repo"});
    if (repo.empty()) {
        error = "no repo configured";
        return false;
    }
    net::Request req;
    req.url = "https://raw.githubusercontent.com/" + repo + "/main/remote-config.json";
    net::Response res = net::fetch(req);
    if (!res.ok()) {
        error = "remote config: " + (res.error.empty() ? "http " + std::to_string(res.status) : res.error);
        return false;
    }
    json remote = json::parse(res.body, nullptr, false);
    if (!remote.is_object()) {
        error = "remote config: invalid json";
        return false;
    }
    CfgState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    merge(s.config, remote);
    if (!s.cache_path.empty()) {
        std::string tmp = s.cache_path + ".tmp";
        bool ok = false;
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            ok = out && out.write(res.body.data(), static_cast<std::streamsize>(res.body.size()));
        }
        if (ok) std::rename(tmp.c_str(), s.cache_path.c_str());
    }
    OLOG("remote config: updated from %s (version %d)", repo.c_str(), remote.value("version", 0));
    return true;
}

json snapshot() {
    CfgState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.config;
}

std::string str(std::initializer_list<const char*> path, const std::string& fallback) {
    CfgState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    const json* v = lookup(s.config, path);
    return v && v->is_string() ? v->get<std::string>() : fallback;
}

json obj(std::initializer_list<const char*> path) {
    CfgState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    const json* v = lookup(s.config, path);
    return v && v->is_object() ? *v : json::object();
}

void set_locale(const std::string& language, const std::string& region) {
    CfgState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!language.empty()) s.language = language;
    if (!region.empty()) s.region = region;
}

std::string language() {
    CfgState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.language;
}

std::string region() {
    CfgState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.region;
}

}  // namespace omega::cfg
