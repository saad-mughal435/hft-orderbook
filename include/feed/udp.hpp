#pragma once

#if !defined(_WIN32)

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace hftob {

/// Minimal UDP socket for a MoldUDP64-style receiver: bind a loopback port in CI,
/// or a real multicast group in production. POSIX only (matches `mt5/tcp.hpp`).
class UdpSocket {
public:
    UdpSocket() {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) throw std::runtime_error("udp socket() failed");
    }
    ~UdpSocket() {
        if (fd_ >= 0) ::close(fd_);
    }
    UdpSocket(const UdpSocket&)            = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    /// Bind to 127.0.0.1:port (0 = OS-assigned); returns the bound port.
    std::uint16_t bind_loopback(std::uint16_t port) {
        sockaddr_in a{};
        a.sin_family      = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port        = htons(port);
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0)
            throw std::runtime_error("udp bind() failed");
        socklen_t len = sizeof(a);
        if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&a), &len) == 0) return ntohs(a.sin_port);
        return port;
    }

    void set_recv_timeout(int ms) {
        timeval tv;
        tv.tv_sec  = ms / 1000;
        tv.tv_usec = (ms % 1000) * 1000;
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    bool send_to(const std::uint8_t* data, std::size_t n, std::uint16_t port) {
        sockaddr_in a{};
        a.sin_family      = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port        = htons(port);
        return ::sendto(fd_, data, n, 0, reinterpret_cast<sockaddr*>(&a), sizeof(a)) ==
               static_cast<ssize_t>(n);
    }

    ssize_t recv(std::uint8_t* buf, std::size_t cap) { return ::recv(fd_, buf, cap, 0); }

private:
    int fd_ = -1;
};

}  // namespace hftob

#endif  // !_WIN32
