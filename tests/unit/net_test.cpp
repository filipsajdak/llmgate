#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <boost/ut.hpp>

import llmgate.async;
import llmgate.net;
import std;

namespace {

using namespace boost::ut;
using namespace llmgate;

constexpr std::string_view loopback = "127.0.0.1";
constexpr std::size_t echo_buffer_bytes = 64;
constexpr async::duration short_wait{30};
constexpr async::duration long_wait{5000};
constexpr async::duration disconnect_delay{30};

async::task<void> echo_once(net::tcp_stream stream) {
    std::array<std::byte, echo_buffer_bytes> buffer{};
    auto const received = co_await stream.read_some(buffer, {});
    if (received && *received > 0) {
        (void)co_await stream.write_all(std::span<std::byte const>{buffer}.first(*received));
    }
}

async::task<void> accept_and_echo(net::listener& server) {
    auto accepted = co_await server.accept();
    if (accepted) {
        co_await echo_once(std::move(*accepted));
    }
}

async::task<std::string> round_trip(async::reactor& loop, net::listener& server, std::string message) {
    async::spawn(accept_and_echo(server));
    auto client = co_await net::tcp_stream::connect(loop, std::string{loopback}, server.port());
    if (!client) {
        co_return "connect failed";
    }
    (void)co_await client->write_all(std::as_bytes(std::span{message}));
    std::string reply(message.size(), '\0');
    auto const received = co_await client->read_some(std::as_writable_bytes(std::span{reply}), {});
    reply.resize(received.value_or(0));
    co_return reply;
}

async::task<std::error_code> read_from_silent_peer(async::reactor& loop, net::listener& server) {
    async::spawn([](net::listener& listening) -> async::task<void> {
        auto accepted = co_await listening.accept();
        std::array<std::byte, 1> hold{};
        if (accepted) {
            (void)co_await accepted->read_some(hold, {});
        }
    }(server));
    auto client = co_await net::tcp_stream::connect(loop, std::string{loopback}, server.port());
    std::array<std::byte, 1> buffer{};
    auto const received = co_await client->read_some(buffer, {.idle = short_wait});
    co_return received ? std::error_code{} : received.error();
}

async::task<std::size_t> read_after_peer_closes(async::reactor& loop, net::listener& server) {
    async::spawn([](net::listener& listening) -> async::task<void> {
        auto accepted = co_await listening.accept();
        (void)accepted;
    }(server));
    auto client = co_await net::tcp_stream::connect(loop, std::string{loopback}, server.port());
    std::array<std::byte, 1> buffer{};
    auto const received = co_await client->read_some(buffer, {});
    co_return received.value_or(std::numeric_limits<std::size_t>::max());
}

struct socket_pair {
    std::array<int, 2> fds{};
    socket_pair() {
        ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data());
        for (auto const fd : fds) {
            ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        }
    }
    ~socket_pair() {
        ::close(fds[0]);
        ::close(fds[1]);
    }
};

async::task<void> connect_then_disconnect(async::reactor& loop, std::uint16_t const port, bool const send_first) {
    auto client = co_await net::tcp_stream::connect(loop, std::string{loopback}, port);
    if (send_first) {
        std::string const extra = "more";
        (void)co_await client->write_all(std::as_bytes(std::span{extra}));
    }
    co_await loop.sleep(disconnect_delay);
}

struct watched_read {
    std::error_code error;
    std::chrono::steady_clock::duration waited;
};

async::task<watched_read> read_while_watching_client(async::reactor& loop, net::listener& server, bool const disconnects,
                                                     bool const upstream_replies) {
    async::spawn(connect_then_disconnect(loop, server.port(), !disconnects));
    auto accepted = co_await server.accept();
    socket_pair upstream_pair;
    if (upstream_replies) {
        ::write(upstream_pair.fds[1], "z", 1);
    }
    net::tcp_stream upstream{loop, net::unique_fd{::dup(upstream_pair.fds[0])}};
    std::array<std::byte, 1> buffer{};
    auto const started = std::chrono::steady_clock::now();
    auto const received = co_await upstream.read_some(buffer, {.idle = long_wait, .watch = accepted->descriptor()});
    co_return watched_read{.error = received ? std::error_code{} : received.error(),
                           .waited = std::chrono::steady_clock::now() - started};
}

const suite net_suite = [] {
    "capability_a_client_disconnect_cancels_a_pending_upstream_read"_test = [] {
        async::reactor loop;
        auto server = net::listener::bind(loop, std::string{loopback}, 0);

        auto const outcome = loop.run(read_while_watching_client(loop, *server, true, false));

        expect(outcome.error == std::errc::connection_aborted);
        expect(outcome.waited < long_wait);
    };

    "capability_a_watched_read_still_returns_upstream_data"_test = [] {
        async::reactor loop;
        auto server = net::listener::bind(loop, std::string{loopback}, 0);

        auto const outcome = loop.run(read_while_watching_client(loop, *server, false, true));

        expect(!outcome.error);
    };

    "capability_listener_binds_an_ephemeral_port"_test = [] {
        async::reactor loop;

        auto server = net::listener::bind(loop, std::string{loopback}, 0);

        expect(server.has_value());
        expect(server->port() > 0_u);
    };

    "limit_listener_rejects_an_address_that_is_not_ipv4"_test = [] {
        async::reactor loop;

        auto server = net::listener::bind(loop, "not-an-address", 0);

        expect(!server.has_value());
    };

    "capability_loopback_echo_returns_the_bytes_sent"_test = [] {
        async::reactor loop;
        auto server = net::listener::bind(loop, std::string{loopback}, 0);

        expect(loop.run(round_trip(loop, *server, "hello gateway")) == "hello gateway");
    };

    "limit_read_from_a_silent_peer_times_out_at_the_idle_limit"_test = [] {
        async::reactor loop;
        auto server = net::listener::bind(loop, std::string{loopback}, 0);

        auto const outcome = loop.run(read_from_silent_peer(loop, *server));

        expect(outcome == std::errc::timed_out);
    };

    "capability_read_after_the_peer_closes_reports_end_of_stream"_test = [] {
        async::reactor loop;
        auto server = net::listener::bind(loop, std::string{loopback}, 0);

        expect(loop.run(read_after_peer_closes(loop, *server)) == 0_u);
    };

    "limit_connect_to_a_closed_port_fails"_test = [] {
        async::reactor loop;
        auto const closed_port = net::listener::bind(loop, std::string{loopback}, 0)->port();

        auto result = loop.run(net::tcp_stream::connect(loop, std::string{loopback}, closed_port));

        expect(!result.has_value());
    };
};

}  // namespace

int main() {}
