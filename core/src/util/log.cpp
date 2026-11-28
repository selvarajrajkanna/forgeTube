#include "util/log.h"
#include "util/no_destroy.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>

namespace omega::log {
namespace {

constexpr size_t kRingSize = 200;
constexpr long kFileLimit = 1024 * 1024;  // per session; the file stops growing past it

struct LogState {
    std::mutex mutex;
    std::deque<std::string> ring;
    int sock = -1;
    FILE* file = nullptr;
    long file_size = 0;
    sockaddr_in sink{};
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};

LogState& state() {
    static omega::NoDestroy<LogState> s;
    return *s;
}

}  // namespace

void init_udp(const std::string& host, int port) {
    LogState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.sock >= 0) close(s.sock);
    s.sock = socket(AF_INET, SOCK_DGRAM, 0);
    s.sink = {};
    s.sink.sin_family = AF_INET;
    s.sink.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, host.c_str(), &s.sink.sin_addr) != 1) {
        close(s.sock);
        s.sock = -1;
    }
}

void init_file(const std::string& path) {
    LogState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.file) fclose(s.file);
    rename(path.c_str(), (path + ".prev").c_str());
    s.file = fopen(path.c_str(), "w");
    s.file_size = 0;
}

void write(const char* fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    LogState& s = state();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - s.start).count();
    char line[1100];
    // Not "%7.3f": it follows the locale ("1,234" on a French TV).
    int n = snprintf(line, sizeof(line), "[%3lld.%03lld] %s", static_cast<long long>(ms / 1000), static_cast<long long>(ms % 1000), msg);

    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.sock >= 0)
        sendto(s.sock, line, static_cast<size_t>(n), MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&s.sink), sizeof(s.sink));
    if (s.file && s.file_size < kFileLimit) {
        // Flushed per line: the file must hold the last lines before a crash.
        fprintf(s.file, "%s\n", line);
        fflush(s.file);
        s.file_size += n + 1;
        if (s.file_size >= kFileLimit) fputs("[log file full]\n", s.file);
    }
    s.ring.emplace_back(line);
    if (s.ring.size() > kRingSize) s.ring.pop_front();
}

std::vector<std::string> tail(size_t max_lines) {
    LogState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    size_t start = s.ring.size() > max_lines ? s.ring.size() - max_lines : 0;
    return {s.ring.begin() + static_cast<long>(start), s.ring.end()};
}

}  // namespace omega::log
