#include "util/remote_config.h"

#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <vector>

#include "net/http.h"
#include "util/log.h"

extern const char omega_remote_config_default[];  // generated from ../remote-config.json (CMakeLists.txt)

namespace omega::cfg {
namespace {

using json = nlohmann::json;

std::mutex g_mutex;
json g_config;
std::string g_cache_path;
std::string g_language = "en", g_region = "IN";

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
    std::lock_guard<std::mutex> lock(g_mutex);
    g_cache_path = cache_path;
    g_config = std::move(config);
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
    std::lock_guard<std::mutex> lock(g_mutex);
    merge(g_config, remote);
    if (!g_cache_path.empty()) {
        std::string tmp = g_cache_path + ".tmp";
        bool ok = false;
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            ok = out && out.write(res.body.data(), static_cast<std::streamsize>(res.body.size()));
        }
        if (ok) std::rename(tmp.c_str(), g_cache_path.c_str());
    }
    OLOG("remote config: updated from %s (version %d)", repo.c_str(), remote.value("version", 0));
    return true;
}

json snapshot() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_config;
}

std::string str(std::initializer_list<const char*> path, const std::string& fallback) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const json* v = lookup(g_config, path);
    return v && v->is_string() ? v->get<std::string>() : fallback;
}

json obj(std::initializer_list<const char*> path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const json* v = lookup(g_config, path);
    return v && v->is_object() ? *v : json::object();
}

void set_locale(const std::string& language, const std::string& region) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!language.empty()) g_language = language;
    if (!region.empty()) g_region = region;
}

std::string language() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_language;
}

std::string region() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_region;
}

}  // namespace omega::cfg
