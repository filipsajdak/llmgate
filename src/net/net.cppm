module;

#include <cerrno>
#include <cstdio>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

export module llmgate.net;

import std;
import llmgate.async;
import llmgate.proxy;
import llmgate.tls;

export namespace llmgate::net {

using result_void = std::expected<void, std::error_code>;
using result_size = std::expected<std::size_t, std::error_code>;
using idle_limit = std::optional<async::duration>;

struct read_options {
    idle_limit idle{};
    int watch = -1;
};

struct fd_handle {
    int value = -1;

    fd_handle() = default;
    constexpr fd_handle(std::nullptr_t) noexcept {}
    constexpr explicit fd_handle(int const descriptor) noexcept : value{descriptor} {}
    constexpr explicit operator bool() const noexcept { return value >= 0; }
    friend constexpr bool operator==(fd_handle const&, fd_handle const&) = default;
};

struct fd_closer {
    using pointer = fd_handle;
    void operator()(fd_handle const handle) const noexcept { ::close(handle.value); }
};

using unique_fd = std::unique_ptr<int, fd_closer>;

inline unique_fd adopt(int const descriptor) noexcept {
    return unique_fd{fd_handle{descriptor}};
}

constexpr int raw(unique_fd const& owner) noexcept {
    return owner.get().value;
}

template <class Stream>
concept byte_stream = requires(Stream& stream, std::span<std::byte> into, std::span<std::byte const> from, read_options options) {
    { stream.read_some(into, options) } -> std::same_as<async::task<result_size>>;
    { stream.write_all(from) } -> std::same_as<async::task<result_void>>;
};

}  // namespace llmgate::net

namespace llmgate::net::detail {

constexpr std::size_t receive_buffer_bytes = 16384;
constexpr int listen_backlog = 128;
#ifdef MSG_NOSIGNAL
constexpr int send_flags = MSG_NOSIGNAL;
#else
constexpr int send_flags = 0;
#endif

[[noreturn]] void abort_destroyed_while_busy(std::string_view const what) noexcept {
    std::println(stderr, "llmgate: {} destroyed while an operation was still pending", what);
    std::terminate();
}

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

std::error_code prepare_tcp_socket(int const fd) noexcept {
    auto const flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return last_error();
    }
    int const enabled = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof enabled);
#ifdef SO_NOSIGPIPE
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof enabled);
#endif
    return {};
}

enum class watched_peer { quiet, speaking, gone };

watched_peer inspect(int const watch) noexcept {
    char probe = 0;
    auto const peeked = ::recv(watch, &probe, 1, MSG_PEEK);
    if (peeked == 0 || (peeked < 0 && !would_block(errno) && errno != EINTR)) {
        return watched_peer::gone;
    }
    return peeked > 0 ? watched_peer::speaking : watched_peer::quiet;
}

result_void as_result(std::error_code const error) noexcept {
    if (error) {
        return std::unexpected{error};
    }
    return {};
}

async::task<result_void> await_data(async::reactor& reactor, int const fd, idle_limit const idle, int& watch) {
    if (watch < 0) {
        co_return as_result(co_await reactor.readable(fd, idle));
    }
    auto const raced = co_await reactor.readable_or_other(fd, watch, idle);
    if (raced.error) {
        co_return std::unexpected{raced.error};
    }
    if (!raced.other_ready) {
        co_return result_void{};
    }
    switch (inspect(watch)) {
        case watched_peer::gone:
            co_return std::unexpected{std::make_error_code(std::errc::connection_aborted)};
        case watched_peer::speaking:
            watch = -1;
            break;
        case watched_peer::quiet:
            break;
    }
    co_return result_void{};
}

async::task<result_size> receive_some(async::reactor& reactor, int const fd, std::span<std::byte> into,
                                      read_options const options) {
    auto watch = options.watch;
    for (;;) {
        auto const received = ::recv(fd, into.data(), into.size(), 0);
        if (received >= 0) {
            co_return static_cast<std::size_t>(received);
        }
        if (errno == EINTR) {
            continue;
        }
        if (!would_block(errno)) {
            co_return std::unexpected{last_error()};
        }
        if (auto const waited = co_await await_data(reactor, fd, options.idle, watch); !waited) {
            co_return std::unexpected{waited.error()};
        }
    }
}

async::task<result_void> send_all(async::reactor& reactor, int const fd, std::span<std::byte const> remaining) {
    while (!remaining.empty()) {
        auto const sent = ::send(fd, remaining.data(), remaining.size(), send_flags);
        if (sent >= 0) {
            remaining = remaining.subspan(static_cast<std::size_t>(sent));
        } else if (would_block(errno)) {
            if (auto const error = co_await reactor.writable(fd)) {
                co_return std::unexpected{error};
            }
        } else if (errno != EINTR) {
            co_return std::unexpected{last_error()};
        }
    }
    co_return result_void{};
}

async::task<std::expected<unique_fd, std::error_code>> connect_candidate(async::reactor& reactor,
                                                                         addrinfo const& candidate,
                                                                         idle_limit const timeout) {
    auto socket = adopt(::socket(candidate.ai_family, candidate.ai_socktype, candidate.ai_protocol));
    if (!socket) {
        co_return std::unexpected{last_error()};
    }
    if (auto const error = prepare_tcp_socket(raw(socket))) {
        co_return std::unexpected{error};
    }
    if (::connect(raw(socket), candidate.ai_addr, candidate.ai_addrlen) == 0) {
        co_return socket;
    }
    if (errno != EINPROGRESS) {
        co_return std::unexpected{last_error()};
    }
    if (auto const error = co_await reactor.writable(raw(socket), timeout)) {
        co_return std::unexpected{error};
    }
    int pending = 0;
    socklen_t length = sizeof pending;
    ::getsockopt(raw(socket), SOL_SOCKET, SO_ERROR, &pending, &length);
    if (pending != 0) {
        co_return std::unexpected{std::error_code{pending, std::system_category()}};
    }
    co_return socket;
}

async::task<std::expected<unique_fd, std::error_code>> tcp_connect(async::reactor& reactor, std::string host,
                                                                   std::uint16_t const port, idle_limit const timeout) {
    auto const hints = stream_hints();
    addrinfo* found = nullptr;
    // Exception: getaddrinfo blocks the reactor thread until an async resolver replaces it.
    if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found) != 0) {
        co_return std::unexpected{std::make_error_code(std::errc::host_unreachable)};
    }
    std::unique_ptr<addrinfo, addrinfo_deleter> const candidates{found};
    std::error_code failure = std::make_error_code(std::errc::host_unreachable);
    for (auto const* candidate = candidates.get(); candidate != nullptr; candidate = candidate->ai_next) {
        auto socket = co_await connect_candidate(reactor, *candidate, timeout);
        if (socket) {
            co_return socket;
        }
        failure = socket.error();
    }
    co_return std::unexpected{failure};
}

constexpr std::size_t tunnel_chunk_bytes = 1024;

result_void tunnel_verdict(proxy::reply const& answer, std::size_t const received) {
    if (answer.which == proxy::outcome::tunnel && answer.consumed == received) {
        return {};
    }
    return std::unexpected{make_error_code(answer.which == proxy::outcome::authentication_required ? proxy::errc::proxy_wants_credentials : proxy::errc::proxy_refused)};
}

async::task<result_void> open_tunnel(async::reactor& reactor, int const descriptor, std::string host, std::uint16_t const port, std::string authorization,
                                     idle_limit const timeout) {
    auto const request = proxy::connect_request(host, port, authorization);
    if (auto const sent = co_await send_all(reactor, descriptor, std::as_bytes(std::span{request})); !sent) {
        co_return sent;
    }
    std::string answer;
    std::array<std::byte, tunnel_chunk_bytes> chunk{};
    for (;;) {
        auto const received = co_await receive_some(reactor, descriptor, chunk, {.idle = timeout});
        if (!received) {
            co_return std::unexpected{received.error()};
        }
        if (*received == 0) {
            co_return std::unexpected{make_error_code(proxy::errc::proxy_closed)};
        }
        std::ranges::transform(std::span{chunk}.first(*received), std::back_inserter(answer), [](std::byte const each) { return static_cast<char>(each); });
        if (auto const parsed = proxy::parse_reply(answer); parsed.which != proxy::outcome::incomplete) {
            co_return tunnel_verdict(parsed, answer.size());
        }
    }
}

}  // namespace llmgate::net::detail

export namespace llmgate::net {

class tcp_stream {
public:
    tcp_stream(async::reactor& reactor, unique_fd socket) noexcept
        : reactor_{&reactor}, socket_{std::move(socket)}, write_lock_{reactor} {}
    tcp_stream(tcp_stream&&) noexcept = default;
    tcp_stream& operator=(tcp_stream&&) noexcept = default;
    ~tcp_stream() {
        if (reading_ || write_lock_.in_use()) {
            detail::abort_destroyed_while_busy("tcp_stream");
        }
    }

    static async::task<std::expected<tcp_stream, std::error_code>> connect(async::reactor& reactor, std::string host,
                                                                           std::uint16_t const port,
                                                                           idle_limit const timeout = std::nullopt) {
        auto socket = co_await detail::tcp_connect(reactor, host, port, timeout);
        if (!socket) {
            co_return std::unexpected{socket.error()};
        }
        co_return tcp_stream{reactor, std::move(*socket)};
    }

    async::task<result_size> read_some(std::span<std::byte> into, read_options const options) {
        if (reading_) {
            co_return std::unexpected{std::make_error_code(std::errc::operation_in_progress)};
        }
        async::exclusive_scope const exclusive{reading_};
        co_return co_await detail::receive_some(*reactor_, raw(socket_), into, options);
    }

    async::task<result_void> write_all(std::span<std::byte const> data) {
        auto const turn = co_await write_lock_.lock();
        co_return co_await detail::send_all(*reactor_, raw(socket_), data);
    }

    int descriptor() const noexcept { return raw(socket_); }
    unique_fd release() && noexcept { return std::move(socket_); }

private:
    async::reactor* reactor_;
    unique_fd socket_;
    async::mutex write_lock_;
    bool reading_ = false;
};

class listener {
public:
    static std::expected<listener, std::error_code> bind(async::reactor& reactor, std::string const& address,
                                                         std::uint16_t const port) {
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = htons(port);
        if (::inet_pton(AF_INET, address.c_str(), &local.sin_addr) != 1) {
            return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
        }
        auto socket = adopt(::socket(AF_INET, SOCK_STREAM, 0));
        if (!socket) {
            return std::unexpected{detail::last_error()};
        }
        int const enabled = 1;
        ::setsockopt(raw(socket), SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof enabled);
        if (auto const error = detail::prepare_tcp_socket(raw(socket))) {
            return std::unexpected{error};
        }
        if (::bind(raw(socket), reinterpret_cast<sockaddr const*>(&local), sizeof local) < 0 ||
            ::listen(raw(socket), detail::listen_backlog) < 0) {
            return std::unexpected{detail::last_error()};
        }
        sockaddr_in bound{};
        socklen_t length = sizeof bound;
        ::getsockname(raw(socket), reinterpret_cast<sockaddr*>(&bound), &length);
        return listener{reactor, std::move(socket), ntohs(bound.sin_port)};
    }

    async::task<std::expected<tcp_stream, std::error_code>> accept(int const stop_descriptor = -1) {
        for (;;) {
            auto client = adopt(::accept(raw(socket_), nullptr, nullptr));
            if (client) {
                if (auto const error = detail::prepare_tcp_socket(raw(client))) {
                    co_return std::unexpected{error};
                }
                co_return tcp_stream{*reactor_, std::move(client)};
            }
            if (errno == EINTR) {
                continue;
            }
            if (!detail::would_block(errno)) {
                co_return std::unexpected{detail::last_error()};
            }
            auto const waited = co_await reactor_->readable_or_other(raw(socket_), stop_descriptor);
            if (waited.error) {
                co_return std::unexpected{waited.error};
            }
            if (waited.other_ready) {
                co_return std::unexpected{std::make_error_code(std::errc::operation_canceled)};
            }
        }
    }

    void stop_listening() noexcept { socket_.reset(); }

    std::uint16_t port() const noexcept { return port_; }
    async::reactor& reactor() const noexcept { return *reactor_; }

private:
    listener(async::reactor& reactor, unique_fd socket, std::uint16_t const port) noexcept
        : reactor_{&reactor}, socket_{std::move(socket)}, port_{port} {}

    async::reactor* reactor_;
    unique_fd socket_;
    std::uint16_t port_;
};

class tls_stream {
public:
    static async::task<std::expected<tls_stream, std::error_code>> connect(async::reactor& reactor,
                                                                           tls::client_context const& context,
                                                                           std::string host, std::uint16_t const port,
                                                                           idle_limit const timeout = std::nullopt) {
        auto const via = proxy::route_for(context.route(), host, port);
        auto socket = co_await detail::tcp_connect(reactor, via ? via->host : host, via ? via->port : port, timeout);
        if (!socket) {
            co_return std::unexpected{socket.error()};
        }
        if (via) {
            if (auto const opened = co_await detail::open_tunnel(reactor, raw(*socket), host, port, via->authorization, timeout); !opened) {
                co_return std::unexpected{opened.error()};
            }
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

    static async::task<std::expected<tls_stream, std::error_code>> accept(async::reactor& reactor,
                                                                          tls::server_context const& context,
                                                                          tcp_stream plain) {
        auto session = tls::connection::accept(context);
        if (!session) {
            co_return std::unexpected{session.error()};
        }
        tls_stream stream{reactor, std::move(plain).release(), std::move(*session)};
        if (auto const done = co_await stream.complete_handshake(); !done) {
            co_return std::unexpected{done.error()};
        }
        co_return stream;
    }

    async::task<result_void> close() {
        auto const turn = co_await write_lock_.lock();
        session_.shutdown();
        co_return co_await flush_locked();
    }

    async::task<result_size> read_some(std::span<std::byte> destination, read_options const options) {
        if (reading_) {
            co_return std::unexpected{std::make_error_code(std::errc::operation_in_progress)};
        }
        async::exclusive_scope const exclusive{reading_};
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
            if (auto const filled = co_await fill(options); !filled) {
                co_return std::unexpected{filled.error()};
            }
        }
    }

    async::task<result_void> write_all(std::span<std::byte const> data) {
        auto const turn = co_await write_lock_.lock();
        while (!data.empty()) {
            auto const result = session_.write(data);
            if (!result) {
                co_return std::unexpected{result.error()};
            }
            data = data.subspan(result->bytes);
            if (auto const sent = co_await flush_locked(); !sent) {
                co_return sent;
            }
        }
        co_return result_void{};
    }

    tls_stream(tls_stream&&) noexcept = default;
    tls_stream& operator=(tls_stream&&) noexcept = default;
    ~tls_stream() {
        if (reading_ || write_lock_.in_use()) {
            detail::abort_destroyed_while_busy("tls_stream");
        }
    }

    tls::connection const& session() const noexcept { return session_; }
    int descriptor() const noexcept { return raw(socket_); }

private:
    tls_stream(async::reactor& reactor, unique_fd socket, tls::connection session)
        : reactor_{&reactor}, socket_{std::move(socket)}, session_{std::move(session)},
          buffer_(detail::receive_buffer_bytes), write_lock_{reactor} {}

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
            if (auto const filled = co_await fill({}); !filled) {
                co_return filled;
            }
        }
    }

    async::task<result_void> flush() {
        auto const turn = co_await write_lock_.lock();
        co_return co_await flush_locked();
    }

    async::task<result_void> flush_locked() {
        auto const pending = session_.take_outgoing();
        co_return co_await detail::send_all(*reactor_, raw(socket_), pending);
    }

    async::task<result_void> fill(read_options const options) {
        auto const received = co_await detail::receive_some(*reactor_, raw(socket_), buffer_, options);
        if (!received) {
            co_return std::unexpected{received.error()};
        }
        if (*received == 0) {
            session_.feed_eof();
            co_return result_void{};
        }
        co_return session_.feed(std::span<std::byte const>{buffer_}.first(*received));
    }

    async::reactor* reactor_;
    unique_fd socket_;
    tls::connection session_;
    std::vector<std::byte> buffer_;
    async::mutex write_lock_;
    bool reading_ = false;
};

static_assert(byte_stream<tcp_stream>);
static_assert(byte_stream<tls_stream>);

class any_stream {
public:
    explicit any_stream(tcp_stream stream) : implementation_{std::move(stream)} {}
    explicit any_stream(tls_stream stream) : implementation_{std::move(stream)} {}

    async::task<result_size> read_some(std::span<std::byte> into, read_options const options) {
        co_return co_await std::visit([&](auto& stream) { return stream.read_some(into, options); }, implementation_);
    }

    async::task<result_void> write_all(std::span<std::byte const> data) {
        co_return co_await std::visit([&](auto& stream) { return stream.write_all(data); }, implementation_);
    }

    int descriptor() const noexcept {
        return std::visit([](auto const& stream) { return stream.descriptor(); }, implementation_);
    }

private:
    std::variant<tcp_stream, tls_stream> implementation_;
};

static_assert(byte_stream<any_stream>);

struct endpoint {
    std::string host;
    std::uint16_t port;
    auto operator<=>(endpoint const&) const = default;
};

struct pool_limits {
    std::size_t max_idle_per_endpoint = 8;
    async::duration max_idle_age{90000};
};

template <class Stream>
concept pooled_stream = byte_stream<Stream> && requires(Stream const& stream) {
    { stream.descriptor() } -> std::same_as<int>;
};

template <pooled_stream Stream>
class connection_pool {
public:
    using clock = std::chrono::steady_clock;

    explicit connection_pool(pool_limits const limits = {}) noexcept : limits_{limits} {}

    std::optional<Stream> take(endpoint const& where, clock::time_point const now = clock::now()) {
        auto const found = idle_.find(where);
        if (found == idle_.end()) {
            return std::nullopt;
        }
        auto& candidates = found->second;
        while (!candidates.empty()) {
            auto candidate = std::move(candidates.back());
            candidates.pop_back();
            if (now - candidate.since <= limits_.max_idle_age && peer_is_quiet(candidate.stream.descriptor())) {
                return std::move(candidate.stream);
            }
        }
        return std::nullopt;
    }

    void give_back(endpoint where, Stream stream, clock::time_point const now = clock::now()) {
        auto& candidates = idle_[std::move(where)];
        if (candidates.size() >= limits_.max_idle_per_endpoint) {
            candidates.erase(candidates.begin());
        }
        candidates.push_back({.stream = std::move(stream), .since = now});
    }

    std::size_t idle_count(endpoint const& where) const {
        auto const found = idle_.find(where);
        return found == idle_.end() ? 0 : found->second.size();
    }

private:
    struct idle {
        Stream stream;
        clock::time_point since;
    };

    static bool peer_is_quiet(int const fd) noexcept {
        char probe = 0;
        return ::recv(fd, &probe, 1, MSG_PEEK | MSG_DONTWAIT) < 0 && detail::would_block(errno);
    }

    pool_limits limits_;
    std::map<endpoint, std::vector<idle>> idle_;
};

}  // namespace llmgate::net
