#pragma once

// A minimal blocking TCP line-socket layer for the MT5 bridge (NDJSON framing).
// POSIX sockets only — the bridge server runs on Linux/macOS in CI and in
// production; the MetaTrader 5 side uses MQL5's own Socket* API (see
// mt5/ITCHBridge.mq5), not this header. Guarded so a Windows build simply omits
// the server/integration-test targets rather than failing.

#if !defined(_WIN32)

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace hftob {
namespace mt5 {

/// One TCP connection with NDJSON line framing: `send_line` writes a pre-built
/// line (already '\n'-terminated by the encoders), `recv_line` returns the next
/// '\n'-delimited line, buffering partial reads internally.
class LineSocket {
public:
    LineSocket() = default;
    explicit LineSocket(int fd) : fd_(fd) {}

    LineSocket(LineSocket&& o) noexcept : fd_(o.fd_), buf_(std::move(o.buf_)) { o.fd_ = -1; }
    LineSocket& operator=(LineSocket&& o) noexcept {
        if (this != &o) {
            close_();
            fd_  = o.fd_;
            buf_ = std::move(o.buf_);
            o.fd_ = -1;
        }
        return *this;
    }
    LineSocket(const LineSocket&)            = delete;
    LineSocket& operator=(const LineSocket&) = delete;
    ~LineSocket() { close_(); }

    static LineSocket connect(const std::string& host, std::uint16_t port) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) throw std::runtime_error("socket() failed");
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(port);
        if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
            ::close(fd);
            throw std::runtime_error("inet_pton() failed for " + host);
        }
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            ::close(fd);
            throw std::runtime_error("connect() failed");
        }
        set_nodelay(fd);
        return LineSocket(fd);
    }

    bool send_line(const std::string& line) {
        std::size_t sent = 0;
        while (sent < line.size()) {
            const ssize_t n = ::send(fd_, line.data() + sent, line.size() - sent, 0);
            if (n <= 0) return false;
            sent += static_cast<std::size_t>(n);
        }
        return true;
    }

    /// Read the next '\n'-delimited line. Returns false on peer-close OR on a recv
    /// timeout (when `set_recv_timeout` is in effect) — `timed_out()` distinguishes
    /// the two. A partial line is preserved across a timeout, not handed back.
    bool recv_line(std::string& out) {
        last_timed_out_ = false;
        for (;;) {
            const std::size_t nl = buf_.find('\n');
            if (nl != std::string::npos) {
                out.assign(buf_, 0, nl);
                buf_.erase(0, nl + 1);
                return true;
            }
            char          tmp[4096];
            const ssize_t n = ::recv(fd_, tmp, sizeof(tmp), 0);
            if (n > 0) {
                buf_.append(tmp, static_cast<std::size_t>(n));
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                last_timed_out_ = true;         // SO_RCVTIMEO fired: idle, not closed
                return false;
            }
            if (!buf_.empty()) {                // peer closed: flush a final partial line
                out.swap(buf_);
                buf_.clear();
                return !out.empty();
            }
            return false;                       // EOF / error
        }
    }

    bool timed_out() const { return last_timed_out_; }

    /// Bound how long `recv_line` blocks waiting for data (0 disables). Lets the
    /// bridge detect a silent/dead peer and reclaim the session.
    void set_recv_timeout(int ms) {
        timeval tv;
        tv.tv_sec  = ms / 1000;
        tv.tv_usec = (ms % 1000) * 1000;
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    bool valid() const { return fd_ >= 0; }

private:
    static void set_nodelay(int fd) {
        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }
    void close_() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    int         fd_             = -1;
    bool        last_timed_out_ = false;
    std::string buf_;
};

/// A loopback TCP listener. Binds to 127.0.0.1 (no external exposure) and, when
/// constructed with port 0, reports the OS-assigned ephemeral port via `port()`.
class Listener {
public:
    explicit Listener(std::uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) throw std::runtime_error("socket() failed");
        int one = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port        = htons(port);
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            ::close(fd_);
            throw std::runtime_error("bind() failed");
        }
        if (::listen(fd_, 1) != 0) {
            ::close(fd_);
            throw std::runtime_error("listen() failed");
        }
        socklen_t len = sizeof(addr);
        if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0)
            port_ = ntohs(addr.sin_port);
    }
    ~Listener() {
        if (fd_ >= 0) ::close(fd_);
    }
    Listener(const Listener&)            = delete;
    Listener& operator=(const Listener&) = delete;

    LineSocket accept() {
        const int c = ::accept(fd_, nullptr, nullptr);
        if (c < 0) throw std::runtime_error("accept() failed");
        return LineSocket(c);
    }
    std::uint16_t port() const { return port_; }

private:
    int           fd_   = -1;
    std::uint16_t port_ = 0;
};

}  // namespace mt5
}  // namespace hftob

#endif  // !_WIN32
