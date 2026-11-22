#include "net/http.h"

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>
#include <zlib.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "util/log.h"

extern const char omega_cacert_pem[];  // generated from the Mozilla CA bundle (see CMakeLists.txt)

namespace omega::net {
namespace {

bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
               return tolower(x) == tolower(y);
           });
}

bool is_redirect(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

std::string tls_error(int rc) {
    char buf[128];
    mbedtls_strerror(rc, buf, sizeof(buf));
    return buf;
}

// Process-wide TLS configuration: CA chain, RNG and client defaults are shared by all connections.
struct TlsContext {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_x509_crt ca;
    mbedtls_ssl_config conf;
    bool ok = false;

    TlsContext() {
        psa_crypto_init();
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&drbg);
        mbedtls_x509_crt_init(&ca);
        mbedtls_ssl_config_init(&conf);
        const char* pers = "omegatube-modern";
        int rc = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                       reinterpret_cast<const unsigned char*>(pers), strlen(pers));
        if (rc != 0) {
            OLOG("tls: drbg seed failed: %s", tls_error(rc).c_str());
            return;
        }
        rc = mbedtls_x509_crt_parse(&ca, reinterpret_cast<const unsigned char*>(omega_cacert_pem),
                                    strlen(omega_cacert_pem) + 1);
        if (rc < 0) {
            OLOG("tls: CA bundle parse failed: %s", tls_error(rc).c_str());
            return;
        }
        if (rc > 0) OLOG("tls: %d CA certificates skipped", rc);
        mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT);
#if defined(__arm__) && !defined(__aarch64__)
        // Mbed TLS 3.6 AESCE hardware AES is AArch64-only; on 32-bit ARM prefer ChaCha20-Poly1305 first
        // (~2x faster than table-based C AES-GCM) while keeping AES-GCM fallbacks for all servers.
        static constexpr int kArm32Ciphersuites[] = {
#if defined(MBEDTLS_SSL_PROTO_TLS1_3)
            MBEDTLS_TLS1_3_CHACHA20_POLY1305_SHA256,
            MBEDTLS_TLS1_3_AES_128_GCM_SHA256,
            MBEDTLS_TLS1_3_AES_256_GCM_SHA384,
#endif
            MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
            MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
            MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
            MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
            MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
            MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
            0
        };
        mbedtls_ssl_conf_ciphersuites(&conf, kArm32Ciphersuites);
#endif
        mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&conf, &ca, nullptr);
        mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
        ok = true;
    }
};

TlsContext& tls() {
    static TlsContext ctx;
    return ctx;
}

struct CachedAddr {
    sockaddr_storage addr{};
    socklen_t len = 0;
    int family = 0;
    int socktype = 0;
    int protocol = 0;
};

class DnsCache {
public:
    static constexpr auto kTtl = std::chrono::seconds(60);
    static constexpr size_t kMaxEntries = 32;

    std::vector<CachedAddr> lookup(const std::string& key) {
        std::lock_guard lock(mu_);
        auto now = std::chrono::steady_clock::now();
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->key == key) {
                if (now - it->ts < kTtl) return it->addrs;
                entries_.erase(it);
                break;
            }
        }
        return {};
    }

    void put(const std::string& key, std::vector<CachedAddr> addrs) {
        if (addrs.empty()) return;
        std::lock_guard lock(mu_);
        auto now = std::chrono::steady_clock::now();
        for (auto& e : entries_) {
            if (e.key == key) {
                e.addrs = std::move(addrs);
                e.ts = now;
                return;
            }
        }
        if (entries_.size() >= kMaxEntries) entries_.erase(entries_.begin());
        entries_.push_back({key, std::move(addrs), now});
    }

    void invalidate(const std::string& key) {
        std::lock_guard lock(mu_);
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->key == key) {
                entries_.erase(it);
                break;
            }
        }
    }

private:
    struct Entry {
        std::string key;
        std::vector<CachedAddr> addrs;
        std::chrono::steady_clock::time_point ts;
    };
    std::mutex mu_;
    std::vector<Entry> entries_;
};

DnsCache& dns_cache() {
    static DnsCache cache;
    return cache;
}

int open_and_connect(const CachedAddr& ca, const std::string& host, bool large_rcvbuf, std::string& error) {
    int fd = socket(ca.family, ca.socktype, ca.protocol);
    if (fd < 0) {
        error = std::string("socket ") + host + ": " + strerror(errno);
        return -1;
    }
    timeval tv{15, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (large_rcvbuf) {
        int rcvbuf = 2 * 1024 * 1024;
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    if (connect(fd, reinterpret_cast<const sockaddr*>(&ca.addr), ca.len) == 0) return fd;
    error = std::string("connect ") + host + ": " + strerror(errno);
    ::close(fd);
    return -1;
}

int connect_tcp(const std::string& host, const std::string& port, bool large_rcvbuf, std::string& error) {
    std::string key = host + ":" + port;
    auto cached = dns_cache().lookup(key);
    for (const auto& ca : cached) {
        int fd = open_and_connect(ca, host, large_rcvbuf, error);
        if (fd >= 0) return fd;
    }
    if (!cached.empty()) dns_cache().invalidate(key);

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (int rc = getaddrinfo(host.c_str(), port.c_str(), &hints, &res); rc != 0) {
        error = std::string("dns ") + host + ": " + gai_strerror(rc);
        return -1;
    }
    std::vector<CachedAddr> addrs;
    int fd = -1;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        if (ai->ai_addrlen > sizeof(sockaddr_storage)) continue;
        CachedAddr ca;
        std::memcpy(&ca.addr, ai->ai_addr, ai->ai_addrlen);
        ca.len = static_cast<socklen_t>(ai->ai_addrlen);
        ca.family = ai->ai_family;
        ca.socktype = ai->ai_socktype;
        ca.protocol = ai->ai_protocol;
        addrs.push_back(ca);
    }
    freeaddrinfo(res);
    for (const auto& ca : addrs) {
        fd = open_and_connect(ca, host, large_rcvbuf, error);
        if (fd >= 0) break;
    }
    if (fd >= 0) dns_cache().put(key, std::move(addrs));
    return fd;
}

struct SslSession {
    mbedtls_ssl_session raw{};
    SslSession() { mbedtls_ssl_session_init(&raw); }
    ~SslSession() { mbedtls_ssl_session_free(&raw); }
    SslSession(const SslSession&) = delete;
    SslSession& operator=(const SslSession&) = delete;
};

class TlsSessionCache {
public:
    static constexpr size_t kMaxEntries = 16;

    void apply(const std::string& key, mbedtls_ssl_context* ssl) {
        std::lock_guard lock(mu_);
        for (const auto& e : entries_) {
            if (e.key == key) {
                mbedtls_ssl_set_session(ssl, &e.session->raw);
                return;
            }
        }
    }

    void save(const std::string& key, const mbedtls_ssl_context* ssl) {
        auto s = std::make_unique<SslSession>();
        if (mbedtls_ssl_get_session(ssl, &s->raw) != 0) return;
        std::lock_guard lock(mu_);
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->key == key) {
                it->session = std::move(s);
                return;
            }
        }
        if (entries_.size() >= kMaxEntries) entries_.erase(entries_.begin());
        entries_.push_back({key, std::move(s)});
    }

private:
    struct Entry {
        std::string key;
        std::unique_ptr<SslSession> session;
    };
    std::mutex mu_;
    std::vector<Entry> entries_;
};

TlsSessionCache& tls_sessions() {
    static TlsSessionCache cache;
    return cache;
}

}  // namespace

class Transport {
public:
    virtual ~Transport() = default;
    virtual long send(const void* data, size_t n) = 0;  // <0 on error
    virtual long recv(void* data, size_t n) = 0;        // 0 on EOF, <0 on error
};

namespace {

class PlainTransport : public Transport {
public:
    explicit PlainTransport(int fd) : fd_(fd) {}
    ~PlainTransport() override { ::close(fd_); }
    long send(const void* data, size_t n) override {
        size_t sent = 0;
        while (sent < n) {
            ssize_t r = ::send(fd_, static_cast<const uint8_t*>(data) + sent, n - sent, MSG_NOSIGNAL);
            if (r < 0) {
                if (errno == EINTR) continue;
                return r;
            }
            if (r == 0) return -1;
            sent += static_cast<size_t>(r);
        }
        return static_cast<long>(sent);
    }
    long recv(void* data, size_t n) override {
        for (;;) {
            ssize_t r = ::recv(fd_, data, n, 0);
            if (r < 0 && errno == EINTR) continue;
            return r;
        }
    }

private:
    int fd_;
};

class TlsTransport : public Transport {
public:
    explicit TlsTransport(int fd) : fd_(fd) { mbedtls_ssl_init(&ssl_); }
    ~TlsTransport() override {
        mbedtls_ssl_free(&ssl_);
        ::close(fd_);
    }

    bool handshake(const std::string& host, const std::string& port, std::string& error) {
        TlsContext& ctx = tls();
        if (!ctx.ok) {
            error = "tls unavailable";
            return false;
        }
        int rc = mbedtls_ssl_setup(&ssl_, &ctx.conf);
        if (rc == 0) rc = mbedtls_ssl_set_hostname(&ssl_, host.c_str());
        if (rc != 0) {
            error = "tls setup: " + tls_error(rc);
            return false;
        }
        session_key_ = host + ":" + port;
        tls_sessions().apply(session_key_, &ssl_);
        mbedtls_ssl_set_bio(&ssl_, this, &TlsTransport::bio_send, &TlsTransport::bio_recv, nullptr);
        while ((rc = mbedtls_ssl_handshake(&ssl_)) != 0) {
            if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            error = "tls handshake " + host + ": " + tls_error(rc);
            uint32_t flags = mbedtls_ssl_get_verify_result(&ssl_);
            if (flags != 0) {
                char info[256];
                mbedtls_x509_crt_verify_info(info, sizeof(info), "", flags);
                error += std::string(" (") + info + ")";
            }
            return false;
        }
        tls_sessions().save(session_key_, &ssl_);
        return true;
    }

    long send(const void* data, size_t n) override {
        size_t sent = 0;
        while (sent < n) {
            int rc = mbedtls_ssl_write(&ssl_, static_cast<const unsigned char*>(data) + sent, n - sent);
            if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            if (rc < 0) return rc;
            sent += static_cast<size_t>(rc);
        }
        return static_cast<long>(sent);
    }

    long recv(void* data, size_t n) override {
        for (;;) {
            int rc = mbedtls_ssl_read(&ssl_, static_cast<unsigned char*>(data), n);
            if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            // TLS 1.3 servers send session tickets after the handshake; save them for resumption.
            if (rc == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) {
                if (!session_key_.empty()) tls_sessions().save(session_key_, &ssl_);
                continue;
            }
            if (rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
            return rc;
        }
    }

private:
    static int bio_send(void* self, const unsigned char* buf, size_t len) {
        ssize_t r = ::send(static_cast<TlsTransport*>(self)->fd_, buf, len, MSG_NOSIGNAL);
        if (r < 0) return (errno == EINTR) ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
        return static_cast<int>(r);
    }
    static int bio_recv(void* self, unsigned char* buf, size_t len) {
        ssize_t r = ::recv(static_cast<TlsTransport*>(self)->fd_, buf, len, 0);
        if (r < 0) return (errno == EINTR) ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_NET_RECV_FAILED;
        return static_cast<int>(r);
    }

    int fd_;
    mbedtls_ssl_context ssl_;
    std::string session_key_;
};

}  // namespace

bool Url::parse(std::string_view url, Url& out) {
    size_t at;
    if (url.starts_with("https://")) {
        out.tls = true;
        at = 8;
    } else if (url.starts_with("http://")) {
        out.tls = false;
        at = 7;
    } else {
        return false;
    }
    size_t slash = url.find_first_of("/?", at);
    std::string_view authority = url.substr(at, slash == std::string_view::npos ? std::string_view::npos : slash - at);
    if (slash == std::string_view::npos) {
        out.path = "/";
    } else if (url[slash] == '?') {
        out.path.assign("/");
        out.path.append(url.substr(slash));
    } else {
        out.path.assign(url.substr(slash));
    }
    size_t colon = authority.rfind(':');
    out.host.assign(authority.substr(0, colon));
    if (colon == std::string_view::npos) {
        out.port = out.tls ? "443" : "80";
    } else {
        out.port.assign(authority.substr(colon + 1));
    }
    return !out.host.empty();
}

Connection::Connection() : buf_(16 * 1024) {}
Connection::~Connection() = default;

void Connection::release_headers() {
    Headers().swap(headers_);
}

void Connection::close() {
    transport_.reset();
    reusable_ = false;
    host_key_.clear();
    release_headers();
    buf_pos_ = buf_len_ = 0;
}

void warmup() {
    (void)tls();
}

bool Connection::connect_to(const Url& url) {
    close();
    int fd = connect_tcp(url.host, url.port, large_rcvbuf_, error_);
    if (fd < 0) return false;
    if (url.tls) {
        auto t = std::make_unique<TlsTransport>(fd);
        if (!t->handshake(url.host, url.port, error_)) return false;
        transport_ = std::move(t);
    } else {
        transport_ = std::make_unique<PlainTransport>(fd);
    }
    host_key_ = (url.tls ? "s:" : "p:") + url.host + ":" + url.port;
    return true;
}

bool Connection::send(const Request& req) {
    error_.clear();
    Url url;
    if (!Url::parse(req.url, url)) {
        error_ = "bad url";
        return false;
    }
    std::string key = (url.tls ? "s:" : "p:") + url.host + ":" + url.port;
    if (!body_done_) close();  // previous body not consumed: cannot reuse

    const bool split_body = req.body.size() > 4096;
    size_t est = req.method.size() + url.path.size() + url.host.size() + (split_body ? 0 : req.body.size()) + 128;
    for (const auto& h : req.headers) est += h.first.size() + h.second.size() + 4;
    std::string msg;
    msg.reserve(est);
    msg.append(req.method).append(" ").append(url.path).append(" HTTP/1.1\r\nHost: ").append(url.host).append("\r\n");
    bool has_ua = false;
    for (const auto& h : req.headers) {
        msg.append(h.first).append(": ").append(h.second).append("\r\n");
        has_ua |= iequals(h.first, "User-Agent");
    }
    if (!has_ua) msg.append("User-Agent: omegatube-modern\r\n");
    if (!req.body.empty() || req.method == "POST") {
        msg.append("Content-Length: ").append(std::to_string(req.body.size())).append("\r\n");
    }
    msg.append("Connection: keep-alive\r\n\r\n");
    if (!split_body) msg.append(req.body);

    // A kept-alive socket may have been closed by the server meanwhile: retry once on a fresh one.
    for (int attempt = 0; attempt < 2; ++attempt) {
        bool fresh = false;
        if (!transport_ || !reusable_ || key != host_key_) {
            if (!connect_to(url)) return false;
            fresh = true;
        }
        buf_pos_ = buf_len_ = 0;
        bool sent = transport_->send(msg.data(), msg.size()) == static_cast<long>(msg.size());
        if (sent && split_body) {
            sent = transport_->send(req.body.data(), req.body.size()) == static_cast<long>(req.body.size());
        }
        if (sent && read_headers()) return true;
        close();
        if (fresh) {
            if (error_.empty()) error_ = "connection failed";
            return false;
        }
    }
    return false;
}

size_t Connection::raw_read(void* dst, size_t n) {
    if (buf_pos_ < buf_len_) {
        size_t got = std::min(n, buf_len_ - buf_pos_);
        memcpy(dst, buf_.data() + buf_pos_, got);
        buf_pos_ += got;
        return got;
    }
    if (!transport_) return 0;
    // Buffer small reads (box/EBML headers, audio frames) through buf_ to avoid per-sample syscalls,
    // while large reads (>= 4 KiB video frames) stream zero-copy straight into dst.
    if (n < 4096) {
        size_t want = (!chunked_ && remaining_ > 0)
                          ? std::min<size_t>(buf_.size(), static_cast<size_t>(remaining_))
                          : buf_.size();
        long r = transport_->recv(buf_.data(), want);
        if (r < 0) {
            error_ = "recv error " + std::to_string(r);
            return 0;
        }
        buf_pos_ = 0;
        buf_len_ = static_cast<size_t>(r);
        size_t got = std::min(n, buf_len_);
        if (got > 0) {
            memcpy(dst, buf_.data(), got);
            buf_pos_ = got;
        }
        return got;
    }
    long r = transport_->recv(dst, n);
    if (r < 0) {
        error_ = "recv error " + std::to_string(r);
        return 0;
    }
    return static_cast<size_t>(r);
}

bool Connection::read_line(std::string& line) {
    line.clear();
    for (;;) {
        if (buf_pos_ == buf_len_) {
            if (!transport_) return false;
            long r = transport_->recv(buf_.data(), buf_.size());
            if (r <= 0) return false;
            buf_pos_ = 0;
            buf_len_ = static_cast<size_t>(r);
        }
        const uint8_t* start = buf_.data() + buf_pos_;
        size_t avail = buf_len_ - buf_pos_;
        const void* nl = memchr(start, '\n', avail);
        size_t take = nl ? static_cast<size_t>(static_cast<const uint8_t*>(nl) - start) : avail;
        if (line.size() + take > 16384) return false;
        line.append(reinterpret_cast<const char*>(start), take);
        buf_pos_ += take;
        if (nl) {
            ++buf_pos_;  // consume '\n'
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return true;
        }
    }
}

bool Connection::read_headers() {
    std::string line;
    if (!read_line(line)) {
        error_ = "no response";
        return false;
    }
    status_ = 0;
    if (sscanf(line.c_str(), "HTTP/%*d.%*d %d", &status_) != 1) {
        error_ = "bad status line";
        return false;
    }
    headers_.clear();
    for (;;) {
        if (!read_line(line)) {
            error_ = "truncated headers";
            reusable_ = false;
            return false;
        }
        if (line.empty()) break;
        std::string_view sv(line);
        size_t colon = sv.find(':');
        if (colon == std::string_view::npos) continue;
        size_t v = sv.find_first_not_of(" \t", colon + 1);
        headers_.emplace_back(sv.substr(0, colon), v == std::string_view::npos ? std::string_view{} : sv.substr(v));
    }
    const bool no_body = (status_ == 204 || status_ == 304 || (status_ >= 100 && status_ < 200));
    std::string_view cl = header_view("Content-Length");
    content_length_ = no_body ? 0 : -1;
    if (!no_body && !cl.empty()) {
        int64_t val = -1;
        if (std::from_chars(cl.data(), cl.data() + cl.size(), val).ec == std::errc{} && val >= 0) content_length_ = val;
    }
    chunked_ = !no_body && (header_view("Transfer-Encoding").find("chunked") != std::string_view::npos);
    reusable_ = !iequals(header_view("Connection"), "close") && (no_body || chunked_ || content_length_ >= 0);
    body_done_ = no_body;
    if (no_body) {
        remaining_ = 0;
    } else if (chunked_) {
        remaining_ = 0;
    } else {
        remaining_ = content_length_;
        if (remaining_ == 0) body_done_ = true;
    }
    return true;
}

bool Connection::next_chunk() {
    std::string line;
    if (remaining_ == 0 && !read_line(line)) {
        error_ = "truncated chunk";
        reusable_ = false;
        return false;
    }  // CRLF after previous chunk (not before the first)
    if (line.empty() && !read_line(line)) {
        error_ = "truncated chunk";
        reusable_ = false;
        return false;
    }
    int64_t chunk_len = 0;
    auto res = std::from_chars(line.data(), line.data() + line.size(), chunk_len, 16);
    if (res.ec != std::errc{} || chunk_len < 0) {
        error_ = "bad chunk size";
        reusable_ = false;
        return false;
    }
    remaining_ = chunk_len;
    if (remaining_ == 0) {
        for (;;) {
            if (!read_line(line)) {
                error_ = "truncated chunk trailers";
                reusable_ = false;
                return false;
            }
            if (line.empty()) break;
        }
        body_done_ = true;
    }
    return true;
}

size_t Connection::read_some(void* dst, size_t n) {
    if (body_done_) return 0;
    if (chunked_ && remaining_ == 0) {
        if (!next_chunk() || body_done_) return 0;
    }
    if (remaining_ >= 0 && static_cast<int64_t>(n) > remaining_) n = static_cast<size_t>(remaining_);
    size_t got = raw_read(dst, n);
    if (got == 0) {
        reusable_ = false;
        if (remaining_ < 0 && !chunked_ && error_.empty()) {
            // Close-delimited HTTP/1.0 response: peer FIN marks clean end of body.
            body_done_ = true;
        } else if (error_.empty()) {
            error_ = "premature eof";
        }
        return 0;
    }
    if (remaining_ >= 0) {
        remaining_ -= static_cast<int64_t>(got);
        if (remaining_ == 0 && !chunked_) body_done_ = true;
    }
    return got;
}

std::string Connection::read_all(size_t limit) {
    std::string out;
    if (content_length_ > 0) out.reserve(std::min<size_t>(static_cast<size_t>(content_length_), limit));
    char tmp[16384];
    while (out.size() < limit) {
        size_t got = read_some(tmp, sizeof(tmp));
        if (got == 0) break;
        out.append(tmp, got);
    }
    return out;
}

std::string_view Connection::header_view(std::string_view name) const {
    for (const auto& h : headers_)
        if (iequals(h.first, name)) return h.second;
    return {};
}

std::string Connection::header(std::string_view name) const {
    return std::string(header_view(name));
}

class ConnectionPool {
public:
    static constexpr size_t kMaxIdle = 8;
    static constexpr auto kIdleTtl = std::chrono::seconds(45);

    std::unique_ptr<Connection> take(const std::string& key) {
        std::lock_guard lock(mu_);
        auto now = std::chrono::steady_clock::now();
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (now - it->ts >= kIdleTtl) {
                it = entries_.erase(it);
                continue;
            }
            if (it->conn->host_key() == key) {
                auto conn = std::move(it->conn);
                entries_.erase(it);
                return conn;
            }
            ++it;
        }
        return std::make_unique<Connection>();
    }

    void put(std::unique_ptr<Connection> conn) {
        if (!conn || !conn->is_reusable()) return;
        conn->release_headers();
        std::lock_guard lock(mu_);
        auto now = std::chrono::steady_clock::now();
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (now - it->ts >= kIdleTtl) it = entries_.erase(it);
            else ++it;
        }
        if (entries_.size() >= kMaxIdle) entries_.erase(entries_.begin());
        entries_.push_back({std::move(conn), now});
    }

private:
    struct Entry {
        std::unique_ptr<Connection> conn;
        std::chrono::steady_clock::time_point ts;
    };
    std::mutex mu_;
    std::vector<Entry> entries_;
};

ConnectionPool& conn_pool() {
    static ConnectionPool pool;
    return pool;
}

bool read_compressed(Connection& conn, bool gzip, std::string& out, size_t limit, bool& too_large) {
    too_large = false;
    out.clear();
    z_stream zs{};
    if (inflateInit2(&zs, (gzip ? 16 : 0) + MAX_WBITS) != Z_OK) return false;
    if (conn.content_length() > 0) {
        out.reserve(std::min<size_t>(static_cast<size_t>(conn.content_length()) * 4, limit));
    }
    char in_buf[16384];
    char out_buf[16384];
    int rc = Z_OK;
    bool got_any = false;
    while (rc == Z_OK) {
        size_t n = conn.read_some(in_buf, sizeof(in_buf));
        if (n == 0) {
            if (!got_any) {
                inflateEnd(&zs);
                return conn.body_done() && conn.error().empty();
            }
            break;
        }
        got_any = true;
        zs.next_in = reinterpret_cast<Bytef*>(in_buf);
        zs.avail_in = static_cast<uInt>(n);
        while (rc == Z_OK) {
            zs.next_out = reinterpret_cast<Bytef*>(out_buf);
            zs.avail_out = sizeof(out_buf);
            rc = inflate(&zs, Z_NO_FLUSH);
            if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) break;
            size_t have = sizeof(out_buf) - zs.avail_out;
            if (have > 0) {
                if (out.size() + have > limit) {
                    inflateEnd(&zs);
                    out.clear();
                    too_large = true;
                    return false;
                }
                out.append(out_buf, have);
            }
            if (rc == Z_BUF_ERROR) {
                rc = Z_OK;
                break;
            }
            if (zs.avail_in == 0 && zs.avail_out > 0) break;
        }
    }
    inflateEnd(&zs);
    if (rc != Z_STREAM_END) return false;
    while (!conn.body_done()) {
        if (conn.read_some(in_buf, sizeof(in_buf)) == 0) break;
    }
    return conn.body_done() && conn.error().empty();
}

Response fetch(const Request& req, size_t max_body) {
    Response res;
    Url first_url;
    std::string initial_key;
    if (Url::parse(req.url, first_url)) {
        initial_key = (first_url.tls ? "s:" : "p:") + first_url.host + ":" + first_url.port;
    }
    auto conn = initial_key.empty() ? std::make_unique<Connection>() : conn_pool().take(initial_key);
    Request current = req;
    bool has_ae = false;
    for (const auto& h : current.headers) {
        if (iequals(h.first, "Accept-Encoding")) {
            has_ae = true;
            break;
        }
    }
    if (!has_ae) current.headers.emplace_back("Accept-Encoding", "gzip");
    for (int redirects = 0;; ++redirects) {
        if (!conn->send(current) && !conn->send(current)) {  // one retry: TV Wi-Fi hiccups are common
            res.error = conn->error();
            return res;
        }
        res.status = conn->status();
        if (is_redirect(res.status) && redirects < 5) {
            std::string location = conn->header("Location");
            if (!location.empty()) {
                conn->read_all(256 * 1024);
                current.url = location;
                if (res.status == 303) {
                    current.method = "GET";
                    current.body.clear();
                }
                continue;
            }
        }
        std::string_view encoding = conn->header_view("Content-Encoding");
        if (iequals(encoding, "gzip") || iequals(encoding, "x-gzip") || iequals(encoding, "deflate")) {
            bool too_large = false;
            bool is_gzip = !iequals(encoding, "deflate");
            if (!read_compressed(*conn, is_gzip, res.body, max_body, too_large)) {
                res.body.clear();
                res.error = too_large ? "response too large"
                                      : (!conn->error().empty() ? conn->error() : "gzip decode failed");
                if (conn->is_reusable()) conn_pool().put(std::move(conn));
                return res;
            }
        } else {
            res.body = conn->read_all(max_body);
            if (res.body.size() >= max_body && !conn->body_done()) {
                res.body.clear();
                res.error = "response too large";
                return res;
            }
            if (!conn->error().empty() || !conn->body_done()) {
                res.error = conn->error().empty() ? "truncated response" : conn->error();
                return res;
            }
        }
        break;
    }
    conn_pool().put(std::move(conn));
    return res;
}

RangeReader::RangeReader(std::string url, Headers headers, uint64_t chunk_size, uint64_t start, bool large_rcvbuf,
                         uint64_t first_chunk_size)
    : url_(std::move(url)),
      headers_(std::move(headers)),
      chunk_size_(chunk_size),
      first_chunk_size_(first_chunk_size),
      pos_(start) {
    if (large_rcvbuf) conn_.set_large_rcvbuf(true);
}

bool RangeReader::open_next() {
    if (total_ >= 0 && static_cast<int64_t>(pos_) >= total_) {
        eof_ = true;
        return false;
    }
    Request req;
    req.url = url_;
    req.headers = headers_;
    // Adaptive chunk sizing: the body is streamed as it arrives, so the first range's size does not delay the
    // first frame; when resuming mid-video (first_chunk_size_ > 0) a small initial probe avoids downloading and
    // discarding a 4 MB range at offset 0; after two clean ranges, 8 MB halves range round-trips on 4K60 streams.
    // Retries fall back to the base size.
    uint64_t req_chunk = chunk_size_;
    if (chunks_opened_ == 0 && first_chunk_size_ > 0 && failures_ == 0) {
        req_chunk = first_chunk_size_;
    } else if (chunks_opened_ >= 2 && failures_ == 0) {
        req_chunk = std::max<uint64_t>(chunk_size_, 8 * 1024 * 1024);
    }
    uint64_t end = pos_ + req_chunk - 1;
    if (total_ >= 0) end = std::min<uint64_t>(end, static_cast<uint64_t>(total_) - 1);
    req.headers.emplace_back("Range", "bytes=" + std::to_string(pos_) + "-" + std::to_string(end));

    int status = 0;
    for (int redirects = 0;; ++redirects) {
        last_status_ = 0;
        if (!conn_.send(req)) {
            error_ = conn_.error();
            return false;
        }
        status = conn_.status();
        last_status_ = status;
        if (!is_redirect(status)) break;
        // googlevideo redirects to another edge server; keep using the new URL for the following ranges.
        std::string location = conn_.header("Location");
        conn_.read_all(1024 * 1024);
        if (location.empty() || redirects == 5) {
            error_ = "redirect without usable Location";
            conn_.close();
            return false;
        }
        OLOG("range: http %d -> %.60s...", status, location.c_str());
        url_ = req.url = location;
    }
    if (status == 206) {
        std::string_view cr = conn_.header_view("Content-Range");  // bytes a-b/total
        size_t slash = cr.find('/');
        if (slash != std::string_view::npos && slash + 1 < cr.size() && cr[slash + 1] != '*') {
            int64_t val = -1;
            if (std::from_chars(cr.data() + slash + 1, cr.data() + cr.size(), val).ec == std::errc{}) total_ = val;
        }
    } else if (status == 200) {
        if (pos_ != 0) {
            error_ = "server ignored Range at offset " + std::to_string(pos_);
            conn_.close();
            return false;
        }
        sequential_ = true;  // server ignored Range: stream the whole body
        total_ = conn_.content_length();
    } else {
        error_ = "http " + std::to_string(status);
        conn_.close();
        return false;
    }
    open_ = true;
    ++chunks_opened_;
    return true;
}

bool RangeReader::recover() {
    if (sequential_) return false;  // no Range support: cannot resume mid-file
    constexpr int kMaxFailures = 40;  // ~3 minutes of retrying
    if (++failures_ > kMaxFailures) return false;
    if (hooks_.aborted && hooks_.aborted()) return false;
    bool refusal = last_status_ == 403 || last_status_ == 410;
    refusals_ = refusal ? refusals_ + 1 : 0;
    if (refusals_ == 1) first_refusal_ = std::chrono::steady_clock::now();
    if (hooks_.refusal_budget_ms > 0 && refusals_ > 1 &&
        std::chrono::steady_clock::now() - first_refusal_ > std::chrono::milliseconds(hooks_.refusal_budget_ms)) {
        OLOG("range: stream still refused (http %d) after %d retries, giving up", last_status_, refusals_ - 1);
        refused_ = true;
        return false;
    }
    if (hooks_.on_recovering) hooks_.on_recovering(true);
    OLOG("range: %s at byte %llu (http %d), retry %d", error_.c_str(), static_cast<unsigned long long>(pos_),
         last_status_, failures_);
    conn_.close();
    if ((last_status_ == 403 || last_status_ == 410) && hooks_.refresh_url) {
        std::string fresh = hooks_.refresh_url();
        if (!fresh.empty()) url_ = fresh;
    }
    // Refusals: short waits, a refused stream sometimes opens a few seconds later (seen with TVHTML5 URLs).
    int wait_ms = std::min(refusal ? 2000 : 5000, 250 << std::min(failures_, 5));
    for (int waited = 0; waited < wait_ms; waited += 100) {
        if (hooks_.aborted && hooks_.aborted()) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    error_.clear();
    return true;
}

size_t RangeReader::read(void* dst, size_t n) {
    auto* out = static_cast<uint8_t*>(dst);
    size_t total = 0;
    while (total < n && !eof_) {
        if (!open_ && !open_next()) {
            if (eof_ || !recover()) break;
            continue;
        }
        size_t got = conn_.read_some(out + total, n - total);
        if (got == 0) {
            open_ = false;
            if (sequential_) {
                eof_ = true;
            } else if (!conn_.body_done() || !conn_.error().empty()) {
                error_ = "range body ended early: " + conn_.error();
                if (!recover()) break;
            }
            continue;
        }
        if (failures_ > 0) {
            OLOG("range: recovered at byte %llu after %d retries", static_cast<unsigned long long>(pos_), failures_);
            failures_ = 0;
            refusals_ = 0;
            if (hooks_.on_recovering) hooks_.on_recovering(false);
        }
        total += got;
        pos_ += got;
    }
    return total;
}

void RangeReader::seek_to(uint64_t offset) {
    eof_ = false;
    error_.clear();
    failures_ = 0;
    refusals_ = 0;
    if (offset == pos_) return;
    if (open_ && !sequential_) {
        const int64_t rem = conn_.remaining();
        // If target offset lies within the next 256 KiB of the active range, skip forward in-place.
        if (offset > pos_ && rem >= 0 && (offset - pos_) <= static_cast<uint64_t>(rem) &&
            (offset - pos_) <= 256 * 1024) {
            uint8_t sink[16384];
            uint64_t to_skip = offset - pos_;
            while (to_skip > 0) {
                size_t step = static_cast<size_t>(std::min<uint64_t>(to_skip, sizeof(sink)));
                size_t got = conn_.read_some(sink, step);
                if (got == 0) break;
                pos_ += got;
                to_skip -= got;
            }
            if (pos_ == offset && !conn_.body_done() && conn_.error().empty()) return;
        } else if (rem >= 0 && rem <= 256 * 1024) {
            // Drain small remaining probe tail so body_done() becomes true and the warm TLS socket is reused.
            uint8_t sink[16384];
            while (!conn_.body_done()) {
                if (conn_.read_some(sink, sizeof(sink)) == 0) break;
            }
        }
    }
    open_ = false;
    if (!conn_.is_reusable()) conn_.close();
    pos_ = offset;
}

LiveReader::LiveReader(std::string url, Headers headers, int64_t first_sequence)
    : url_(std::move(url)), headers_(std::move(headers)), seq_(first_sequence) {
    conn_.set_large_rcvbuf(true);
}

namespace {

// GET with redirects followed on `conn`; the original URL is kept by the caller (Location carries the sq).
bool send_following(Connection& conn, Request req, int& status) {
    for (int redirects = 0;; ++redirects) {
        status = 0;
        if (!conn.send(req)) return false;
        status = conn.status();
        if (!is_redirect(status)) return true;
        std::string location = conn.header("Location");
        conn.read_all(1024 * 1024);
        if (location.empty() || redirects == 5) return true;
        req.url = location;
    }
}

}  // namespace

int64_t LiveReader::head_sequence(const std::string& url, const Headers& headers, std::string& error) {
    Connection conn;
    Request req;
    req.url = url;
    req.headers = headers;
    req.headers.emplace_back("Range", "bytes=0-0");  // the head segment itself is not needed
    int status = 0;
    if (!send_following(conn, req, status)) {
        error = conn.error();
        return -1;
    }
    std::string head = conn.header("X-Head-Seqnum");
    conn.close();
    if (status / 100 != 2 || head.empty()) {
        error = "live head: http " + std::to_string(status) + (head.empty() ? ", no X-Head-Seqnum" : "");
        return -1;
    }
    int64_t seq = -1;
    auto parsed = std::from_chars(head.data(), head.data() + head.size(), seq);
    if (parsed.ec != std::errc{} || seq < 0) {
        error = "live head: invalid X-Head-Seqnum";
        return -1;
    }
    return seq;
}

bool LiveReader::open_segment() {
    Request req;
    req.url = url_ + (url_.find('?') == std::string::npos ? "?" : "&") + "sq=" + std::to_string(seq_);
    req.headers = headers_;
    // Resume a segment cut by a network error where it stopped.
    if (segment_pos_ > 0) req.headers.emplace_back("Range", "bytes=" + std::to_string(segment_pos_) + "-");
    if (!send_following(conn_, req, last_status_)) {
        error_ = conn_.error();
        return false;
    }
    if (last_status_ == 200 && segment_pos_ == 0) {
        open_ = true;
        return true;
    }
    if (last_status_ == 206 && segment_pos_ > 0) {
        open_ = true;
        return true;
    }
    error_ = "segment " + std::to_string(seq_) + ": http " + std::to_string(last_status_);
    conn_.close();
    return false;
}

bool LiveReader::retry(int max_failures) {
    if (++failures_ > max_failures) return false;
    if (hooks_.aborted && hooks_.aborted()) return false;
    // A 404 at the live edge is the normal wait for the next segment: only report longer outages.
    if (failures_ > 2 && hooks_.on_recovering) hooks_.on_recovering(true);
    if (last_status_ != 404 || failures_ > 2)
        OLOG("live: %s, retry %d", error_.c_str(), failures_);
    conn_.close();
    if ((last_status_ == 403 || last_status_ == 410) && hooks_.refresh_url) {
        std::string fresh = hooks_.refresh_url();
        if (!fresh.empty()) url_ = fresh;
    }
    // The server already held a 404 for several seconds; other errors back off.
    int wait_ms = last_status_ == 404 ? 1000 : std::min(5000, 250 << std::min(failures_, 5));
    for (int waited = 0; waited < wait_ms; waited += 100) {
        if (hooks_.aborted && hooks_.aborted()) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    error_.clear();
    return true;
}

size_t LiveReader::read(void* dst, size_t n) {
    auto* out = static_cast<uint8_t*>(dst);
    size_t total = 0;
    while (total < n && !eof_) {
        if (!open_ && !open_segment()) {
            // ~8 s per 404: give up after ~1 min without a new segment (broadcast over), ~3 min of network errors.
            if (!retry(last_status_ == 404 ? 7 : 40)) {
                OLOG("live: stream ends at segment %lld (%s)", static_cast<long long>(seq_), error_.c_str());
                eof_ = true;
            }
            continue;
        }
        size_t got = conn_.read_some(out + total, n - total);
        if (got == 0) {
            open_ = false;
            if (conn_.body_done() && conn_.error().empty()) {
                ++seq_;
                segment_pos_ = 0;
            } else {
                error_ = "segment body ended early: " + conn_.error();
                last_status_ = 0;
                if (!retry(40)) eof_ = true;
            }
            continue;
        }
        if (failures_ > 0) {
            if (failures_ > 2) {
                OLOG("live: recovered at segment %lld after %d retries", static_cast<long long>(seq_), failures_);
                if (hooks_.on_recovering) hooks_.on_recovering(false);
            }
            failures_ = 0;
        }
        total += got;
        pos_ += got;
        segment_pos_ += got;
    }
    return total;
}

}  // namespace omega::net
