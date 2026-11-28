#include "yt/watch.h"

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <random>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "net/http.h"
#include "util/log.h"
#include "util/no_destroy.h"
#include "util/remote_config.h"
#include "yt/auth.h"

namespace omega::yt {
namespace {

using json = nlohmann::json;
using Fields = std::vector<std::pair<std::string, std::string>>;

std::string make_cpn() {
    static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::random_device rd;
    std::mt19937 rng(rd());
    std::string out;
    for (int i = 0; i < 16; ++i) out += kAlphabet[rng() & 63];
    return out;
}

// "12.345". Not printf("%.3f"): that follows the process locale and gives "12,345" on a French TV.
std::string seconds_text(double s) {
    long long ms = s < 0 ? 0 : std::llround(s * 1000);
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld.%03lld", ms / 1000, ms % 1000);
    return buf;
}

// `base` with `fields` set (replacing any of the same name) and `fexp` dropped (the stats endpoint
// refuses experiments the client did not run).
std::string stats_url(const std::string& base, const Fields& fields) {
    size_t mark = base.find('?');
    std::string out = base.substr(0, mark), query;
    if (mark != std::string::npos) {
        for (size_t at = mark + 1; at <= base.size();) {
            size_t end = base.find('&', at);
            if (end == std::string::npos) end = base.size();
            std::string pair = base.substr(at, end - at);
            std::string key = pair.substr(0, pair.find('='));
            bool drop = key.empty() || key == "fexp";
            for (const auto& f : fields) drop = drop || key == f.first;
            if (!drop) query += (query.empty() ? "" : "&") + pair;
            at = end + 1;
        }
    }
    for (const auto& f : fields) query += (query.empty() ? "" : "&") + f.first + "=" + f.second;
    return query.empty() ? out : out + "?" + query;
}

// The tracking URLs point at s.youtube.com, which only reads cookies; YouTube's TV player sends the
// pings with its OAuth token to www.youtube.com, the host that reads the Authorization header.
std::string account_host(const std::string& url) {
    const std::string stats = "https://s.youtube.com/";
    return url.compare(0, stats.size(), stats) == 0 ? "https://www.youtube.com/" + url.substr(stats.size()) : url;
}

void add_client(Fields& f, const std::string& version) {
    f.push_back({"c", cfg::str({"innertube", "browse", "clientName"})});
    f.push_back({"cver", version});
    f.push_back({"cplayer", "UNIPLAYER"});
    f.push_back({"cplatform", "TV"});
    f.push_back({"hl", "en_US"});
}

bool ping(const std::string& url, const std::string& token) {
    net::Request req;
    req.url = account_host(url);
    req.headers = {{"User-Agent", cfg::str({"innertube", "browse", "userAgent"})},
                   {"Authorization", "Bearer " + token}};
    net::Response res = net::fetch(req);
    return res.error.empty() && res.status >= 200 && res.status < 300;
}

// The account's tracking URLs: the TV client's player answer with the Bearer token (a token-less
// client's URLs are anonymous).
bool tracking_urls(const std::string& video_id, const std::string& token, std::string& playback,
                   std::string& watchtime) {
    json c = cfg::obj({"innertube", "browse"});
    json client = c.value("context", json::object());
    client["clientName"] = c.value("clientName", "");
    client["clientVersion"] = c.value("clientVersion", "");
    client["hl"] = cfg::language();
    client["gl"] = cfg::region();
    json body = {{"context", {{"client", client}}}, {"videoId", video_id}, {"contentCheckOk", true}, {"racyCheckOk", true}};
    net::Request req;
    req.method = "POST";
    req.url = "https://www.youtube.com/youtubei/v1/player?prettyPrint=false";
    req.headers = {{"Content-Type", "application/json"},
                   {"User-Agent", c.value("userAgent", "")},
                   {"X-YouTube-Client-Name", std::to_string(c.value("clientNameId", 7))},
                   {"X-YouTube-Client-Version", c.value("clientVersion", "")},
                   {"Authorization", "Bearer " + token},
                   {"Origin", "https://www.youtube.com"}};
    req.body = body.dump();
    net::Response res = net::fetch(req);
    if (!res.ok()) {
        OLOG("history: tracking player http %d %s", res.status, res.error.c_str());
        return false;
    }
    // Zero-DOM SAX scan: only extract playbackTracking.{videostatsPlaybackUrl,videostatsWatchtimeUrl}.baseUrl.
    struct TrackingSax {
        std::string& playback;
        std::string& watchtime;
        int depth = 0;
        bool next_is_tracking = false;
        bool in_tracking = false;
        int next_target = 0;  // 1 = playback, 2 = watchtime
        int target = 0;
        bool want_base_url = false;

        bool clear_flags() {
            if (depth == 1) next_is_tracking = false;
            else if (depth == 2) next_target = 0;
            else if (depth == 3) want_base_url = false;
            return true;
        }
        bool null() { return clear_flags(); }
        bool boolean(bool) { return clear_flags(); }
        bool number_integer(json::number_integer_t) { return clear_flags(); }
        bool number_unsigned(json::number_unsigned_t) { return clear_flags(); }
        bool number_float(json::number_float_t, const std::string&) { return clear_flags(); }
        bool binary(json::binary_t&) { return clear_flags(); }
        bool string(std::string& v) {
            if (depth == 3 && target != 0 && want_base_url) {
                if (target == 1) playback = std::move(v);
                else if (target == 2) watchtime = std::move(v);
                want_base_url = false;
                if (!playback.empty() && !watchtime.empty()) return false;  // both found: stop scanning
            }
            return clear_flags();
        }
        bool start_object(std::size_t) {
            ++depth;
            if (depth == 2 && next_is_tracking) {
                in_tracking = true;
                next_is_tracking = false;
            } else if (depth == 3 && in_tracking && next_target != 0) {
                target = next_target;
                next_target = 0;
            }
            return true;
        }
        bool end_object() {
            if (depth == 3) {
                target = 0;
                want_base_url = false;
            } else if (depth == 2) {
                in_tracking = false;
                next_target = 0;
            }
            --depth;
            return true;
        }
        bool start_array(std::size_t) {
            clear_flags();
            ++depth;
            return true;
        }
        bool end_array() {
            --depth;
            return true;
        }
        bool key(std::string& k) {
            if (depth == 1) next_is_tracking = (k == "playbackTracking");
            else if (depth == 2 && in_tracking) {
                next_target = (k == "videostatsPlaybackUrl")    ? 1
                              : (k == "videostatsWatchtimeUrl") ? 2
                                                                : 0;
            } else if (depth == 3 && target != 0) {
                want_base_url = (k == "baseUrl");
            }
            return true;
        }
        bool parse_error(std::size_t, const std::string&, const json::exception&) { return false; }
    };
    TrackingSax sax{playback, watchtime};
    json::sax_parse(res.body, &sax);
    return !playback.empty();
}

struct FinalPing {
    std::string url;
    std::string token;
    std::string video_id;
    int64_t span_start = 0;
    int64_t last = 0;
};

class FinalPingQueue {
public:
    static constexpr size_t kMaxPending = 4;

    void enqueue(FinalPing item) {
        std::lock_guard<std::mutex> lock(mu_);
        if (!started_) {
            started_ = true;
            std::thread([this] { worker(); }).detach();
        }
        if (queue_.size() >= kMaxPending) queue_.pop_front();
        queue_.push_back(std::move(item));
        cv_.notify_one();
    }

private:
    void worker() {
        for (;;) {
            FinalPing job;
            {
                std::unique_lock<std::mutex> lock(mu_);
                cv_.wait(lock, [&] { return !queue_.empty(); });
                job = std::move(queue_.front());
                queue_.pop_front();
            }
            bool sent = ping(job.url, job.token);
            OLOG("history: final watchtime %s %.0f-%.0f s -> %s", job.video_id.c_str(),
                 job.span_start / 1000.0, job.last / 1000.0, sent ? "ok" : "failed");
        }
    }

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<FinalPing> queue_;
    bool started_ = false;
};

FinalPingQueue& final_ping_queue() {
    static omega::NoDestroy<FinalPingQueue> q;
    return *q;
}

}  // namespace

WatchReporter::WatchReporter(std::string video_id, int64_t length_ms, PositionFn position)
    : video_id_(std::move(video_id)), length_ms_(length_ms), position_(std::move(position)) {
    thread_ = std::thread([this] { run(); });
}

WatchReporter::~WatchReporter() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
    }
    cancel_token_.abort();
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void WatchReporter::run() {
    net::CancelScope cancel_scope(&cancel_token_);
    std::string error;
    std::string token = shared_auth().access_token(error);
    if (token.empty() || stop_) return;  // signed out or stopped: nothing to report
    std::string playback, watchtime;
    if (!tracking_urls(video_id_, token, playback, watchtime)) {
        OLOG("history: no tracking URLs for %s", video_id_.c_str());
        return;
    }
    if (stop_) return;
    const std::string cpn = make_cpn();
    const std::string version = cfg::str({"innertube", "browse", "clientVersion"});
    // el=detailpage: without it YouTube files the view as a Short and it never shows in the history.
    Fields pf = {{"ver", "2"}, {"cpn", cpn}, {"cmt", "0"}, {"el", "detailpage"}};
    add_client(pf, version);
    pf.insert(pf.end(), {{"rt", "0.000"}, {"fs", "0"}, {"muted", "0"}, {"volume", "100"}});
    bool ok = ping(stats_url(playback, pf), token);
    OLOG("history: playback ping %s -> %s", video_id_.c_str(), ok ? "ok" : "failed");

    const auto started = std::chrono::steady_clock::now();
    int64_t span_start = -1, last = -1;
    int waited = 0;
    while (!stop_) {
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait_for(lock, std::chrono::milliseconds(500), [&] { return stop_.load(); });
        }
        int64_t pos = position_();
        bool final_report = stop_;
        if (pos >= 0) {
            if (span_start < 0 || pos < last || pos - last > 5000) span_start = pos;  // start or seek
            last = pos;
        }
        if (++waited < 40 && !final_report) continue;  // every 20 s
        waited = 0;
        // Nothing new since the last report; the final one is still sent (it carries the resume point).
        if (span_start < 0 || (last <= span_start && !final_report)) continue;
        if (watchtime.empty()) continue;
        token = shared_auth().access_token(error);
        if (token.empty()) return;
        double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        Fields wf = {{"ver", "2"},
                     {"cpn", cpn},
                     {"st", seconds_text(span_start / 1000.0)},
                     {"et", seconds_text(last / 1000.0)},
                     {"cmt", seconds_text(last / 1000.0)},
                     {"len", seconds_text(length_ms_ / 1000.0)},
                     {"el", "detailpage"},
                     {"state", final_report ? "paused" : "playing"}};
        // The last report of the playback (player closed): YouTube's players flag it, and it is the one
        // the account's resume point follows on other devices.
        if (final_report) wf.push_back({"final", "1"});
        add_client(wf, version);
        wf.insert(wf.end(), {{"rt", seconds_text(elapsed)}, {"muted", "0"}, {"volume", "100"}});
        const std::string final_url = stats_url(watchtime, wf);
        if (final_report) {
            final_ping_queue().enqueue({final_url, token, video_id_, span_start, last});
            break;
        }
        bool sent = ping(final_url, token);
        if (!sent)
            OLOG("history: watchtime %s %.0f-%.0f s -> failed", video_id_.c_str(), span_start / 1000.0, last / 1000.0);
        span_start = last;
    }
}

}  // namespace omega::yt
