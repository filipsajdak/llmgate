module;

#include <cerrno>

#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

export module llmgate.net;

import std;
import llmgate.async;
import llmgate.tls;

export namespace llmgate::net {

using result_void = std::expected<void, std::error_code>;

class unique_fd {
public:
    unique_fd() noexcept = default;
    explicit unique_fd(int const fd) noexcept : fd_{fd} {}
    unique_fd(unique_fd&& other) noexcept : fd_{std::exchange(other.fd_, -1)} {}
    unique_fd& operator=(unique_fd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    unique_fd(unique_fd const&) = delete;
    unique_fd& operator=(unique_fd const&) = delete;
    ~unique_fd() { reset(); }

    int get() const noexcept { return fd_; }
    explicit operator bool() const noexcept { return fd_ >= 0; }

private:
    void reset() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = -1;
    }

    int fd_ = -1;
};

}  // namespace llmgate::net

namespace llmgate::net::detail {

constexpr std::size_t receive_buffer_bytes = 16384;

struct addrinfo_deleter {
    void operator()(addrinfo* list) const noexcept { ::freeaddrinfo(list); }
};

std::error_code last_error() noexcept {
    return {errno, std::system_category()};
}

addrinfo stream_hints() noexcept {
    // Exception: addrinfo member order differs between libcs, so a designated initializer cannot be portable.
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    return hints;
}

bool would_block(int const error) noexcept {
    return error == EAGAIN || error == EWOULDBLOCK;
}

std::error_code make_nonblocking(int const fd) noexcept {
    auto const flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return last_error();
    }
#ifdef SO_NOSIGPIPE
    int const enabled = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof enabled);
#endif
    return {};
}

async::task<std::expected<unique_fd, std::error_code>> tcp_connect(async::reactor& reactor, std::string const& host,
                                                                   std::uint16_t const port) {
    auto const hints = stream_hints();
    addrinfo* found = nullptr;
    // Exception: getaddrinfo blocks the reactor thread until an async resolver replaces it.
    if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found) != 0) {
        co_return std::unexpected{std::make_error_code(std::errc::host_unreachable)};
    }
    std::unique_ptr<addrinfo, addrinfo_deleter> const candidates{found};
    std::error_code failure = std::make_error_code(std::errc::host_unreachable);
    for (auto const* candidate = candidates.get(); candidate != nullptr; candidate = candidate->ai_next) {
        unique_fd socket{::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol)};
        if (!socket) {
            failure = last_error();
            continue;
        }
        if (auto const error = make_nonblocking(socket.get())) {
            failure = error;
            continue;
        }
        if (::connect(socket.get(), candidate->ai_addr, candidate->ai_addrlen) == 0) {
            co_return socket;
        }
        if (errno != EINPROGRESS) {
            failure = last_error();
            continue;
        }
        if (auto const error = co_await reactor.writable(socket.get())) {
            failure = error;
            continue;
        }
        int pending = 0;
        socklen_t length = sizeof pending;
        ::getsockopt(socket.get(), SOL_SOCKET, SO_ERROR, &pending, &length);
        if (pending != 0) {
            failure = std::error_code{pending, std::system_category()};
            continue;
        }
        co_return socket;
    }
    co_return std::unexpected{failure};
}

}  // namespace llmgate::net::detail

export namespace llmgate::net {

class tls_stream {
public:
    static async::task<std::expected<tls_stream, std::error_code>> connect(async::reactor& reactor,
                                                                           tls::client_context const& context,
                                                                           std::string host, std::uint16_t const port) {
        auto socket = co_await detail::tcp_connect(reactor, host, port);
        if (!socket) {
            co_return std::unexpected{socket.error()};
        }
        auto session = tls::connection::make(context, host);
        if (!session) {
            co_return std::unexpected{session.error()};
        }
        tls_stream stream{reactor, std::move(*socket), std::move(*session)};
        if (auto const done = co_await stream.complete_handshake(); !done) {
            co_return std::unexpected{done.error()};
        }
        co_return stream;
    }

    async::task<std::expected<std::size_t, std::error_code>> read_some(std::span<std::byte> destination) {
        for (;;) {
            auto const result = session_.read(destination);
            if (!result) {
                if (result.error() == tls::errc::peer_closed) {
                    co_return std::size_t{0};
                }
                co_return std::unexpected{result.error()};
            }
            if (result->bytes > 0) {
                co_return result->bytes;
            }
            if (auto const sent = co_await flush(); !sent) {
                co_return std::unexpected{sent.error()};
            }
            if (auto const filled = co_await fill(); !filled) {
                co_return std::unexpected{filled.error()};
            }
        }
    }

    async::task<result_void> write_all(std::span<std::byte const> data) {
        while (!data.empty()) {
            auto const result = session_.write(data);
            if (!result) {
                co_return std::unexpected{result.error()};
            }
            data = data.subspan(result->bytes);
            if (auto const sent = co_await flush(); !sent) {
                co_return sent;
            }
        }
        co_return result_void{};
    }

    tls::connection const& session() const noexcept { return session_; }

private:
    tls_stream(async::reactor& reactor, unique_fd socket, tls::connection session)
        : reactor_{&reactor}, socket_{std::move(socket)}, session_{std::move(session)},
          buffer_(detail::receive_buffer_bytes) {}

    async::task<result_void> complete_handshake() {
        for (;;) {
            auto const progress = session_.handshake();
            if (!progress) {
                co_return std::unexpected{progress.error()};
            }
            if (auto const sent = co_await flush(); !sent) {
                co_return sent;
            }
            if (*progress == tls::want::none) {
                co_return result_void{};
            }
            if (auto const filled = co_await fill(); !filled) {
                co_return filled;
            }
        }
    }

    async::task<result_void> flush() {
        auto const pending = session_.take_outgoing();
        std::span<std::byte const> remaining{pending};
        while (!remaining.empty()) {
            auto const sent = ::send(socket_.get(), remaining.data(), remaining.size(), 0);
            if (sent >= 0) {
                remaining = remaining.subspan(static_cast<std::size_t>(sent));
            } else if (detail::would_block(errno)) {
                if (auto const error = co_await reactor_->writable(socket_.get())) {
                    co_return std::unexpected{error};
                }
            } else if (errno != EINTR) {
                co_return std::unexpected{detail::last_error()};
            }
        }
        co_return result_void{};
    }

    async::task<result_void> fill() {
        for (;;) {
            if (auto const error = co_await reactor_->readable(socket_.get())) {
                co_return std::unexpected{error};
            }
            auto const received = ::recv(socket_.get(), buffer_.data(), buffer_.size(), 0);
            if (received > 0) {
                co_return session_.feed(std::span<std::byte const>{buffer_}.first(static_cast<std::size_t>(received)));
            }
            if (received == 0) {
                session_.feed_eof();
                co_return result_void{};
            }
            if (!detail::would_block(errno) && errno != EINTR) {
                co_return std::unexpected{detail::last_error()};
            }
        }
    }

    async::reactor* reactor_;
    unique_fd socket_;
    tls::connection session_;
    std::vector<std::byte> buffer_;
};

}  // namespace llmgate::net
