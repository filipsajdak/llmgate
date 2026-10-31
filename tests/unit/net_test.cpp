#include "test_io.h"

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
    loop.spawn(accept_and_echo(server));
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
    loop.spawn([](net::listener& listening) -> async::task<void> {
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
    loop.spawn([](net::listener& listening) -> async::task<void> {
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
        test_io::open_pair(fds.data());
        for (auto const fd : fds) {
            test_io::make_nonblocking(fd);
        }
    }
    ~socket_pair() {
        test_io::close_fd(fds[0]);
        test_io::close_fd(fds[1]);
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
    loop.spawn(connect_then_disconnect(loop, server.port(), !disconnects));
    auto accepted = co_await server.accept();
    socket_pair upstream_pair;
    if (upstream_replies) {
        test_io::write_fd(upstream_pair.fds[1], "z", 1);
    }
    net::tcp_stream upstream{loop, net::adopt(test_io::duplicate_fd(upstream_pair.fds[0]))};
    std::array<std::byte, 1> buffer{};
    auto const started = std::chrono::steady_clock::now();
    auto const received = co_await upstream.read_some(buffer, {.idle = long_wait, .watch = accepted->descriptor()});
    co_return watched_read{.error = received ? std::error_code{} : received.error(),
                           .waited = std::chrono::steady_clock::now() - started};
}

struct connected_pair {
    net::tcp_stream client;
    net::tcp_stream accepted;
};

async::task<connected_pair> connect_pair(async::reactor& loop, net::listener& server) {
    async::task<std::expected<net::tcp_stream, std::error_code>> accepting = server.accept();
    auto client = co_await net::tcp_stream::connect(loop, std::string{loopback}, server.port());
    auto accepted = co_await std::move(accepting);
    co_return connected_pair{.client = std::move(*client), .accepted = std::move(*accepted)};
}

constexpr net::endpoint upstream{.host = "api.example", .port = 443};

async::task<void> until_readable(async::reactor& loop, int const fd) {
    (void)co_await loop.readable(fd);
}

const suite net_suite = [] {
    "capability_both_ends_of_a_connection_disable_nagle"_test = [] {
        async::reactor loop;
        auto server = net::listener::bind(loop, std::string{loopback}, 0);
        auto pair = loop.run(connect_pair(loop, *server));

        auto const nodelay = [](int const fd) { return test_io::nodelay_enabled(fd); };

        expect(nodelay(pair.client.descriptor()));
        expect(nodelay(pair.accepted.descriptor()));
    };

    "capability_a_returned_connection_is_taken_again"_test = [] {
        async::reactor loop;
        auto server = net::listener::bind(loop, std::string{loopback}, 0);
        auto pair = loop.run(connect_pair(loop, *server));
        net::connection_pool<net::tcp_stream> pool;

        pool.give_back(upstream, std::move(pair.client));
        auto const taken = pool.take(upstream);

        expect(taken.has_value());
        expect(pool.idle_count(upstream) == 0_u);
    };

    "limit_a_connection_older_than_the_idle_age_is_dropped"_test = [] {
        async::reactor loop;
        auto server = net::listener::bind(loop, std::string{loopback}, 0);
        auto pair = loop.run(connect_pair(loop, *server));
        net::connection_pool<net::tcp_stream> pool{{.max_idle_per_endpoint = 8, .max_idle_age = async::duration{100}}};
        auto const start = std::chrono::steady_clock::now();

        pool.give_back(upstream, std::move(pair.client), start);

        expect(!pool.take(upstream, start + std::chrono::milliseconds{101}).has_value());
    };

    "limit_a_connection_the_peer_closed_is_dropped"_test = [] {
        async::reactor loop;
        auto server = net::listener::bind(loop, std::string{loopback}, 0);
        auto pair = loop.run(connect_pair(loop, *server));
        net::connection_pool<net::tcp_stream> pool;
        auto const watched = pair.client.descriptor();
        pool.give_back(upstream, std::move(pair.client));

        { auto closing = std::move(pair.accepted); }
        loop.run(until_readable(loop, watched));

        expect(!pool.take(upstream).has_value());
    };

    "limit_a_connection_with_unread_bytes_is_dropped"_test = [] {
        async::reactor loop;
        auto server = net::listener::bind(loop, std::string{loopback}, 0);
        auto pair = loop.run(connect_pair(loop, *server));
        net::connection_pool<net::tcp_stream> pool;
        std::string const stray = "late";
        loop.run([](net::tcp_stream& peer, std::string const& text) -> async::task<void> {
            (void)co_await peer.write_all(std::as_bytes(std::span{text}));
        }(pair.accepted, stray));
        auto const watched = pair.client.descriptor();
        pool.give_back(upstream, std::move(pair.client));
        loop.run(until_readable(loop, watched));

        expect(!pool.take(upstream).has_value());
    };

    "limit_idle_connections_per_endpoint_are_bounded_oldest_first"_test = [] {
        async::reactor loop;
        auto server = net::listener::bind(loop, std::string{loopback}, 0);
        net::connection_pool<net::tcp_stream> pool{{.max_idle_per_endpoint = 2, .max_idle_age = async::duration{90000}}};
        std::vector<net::tcp_stream> keep_open;

        for (int index = 0; index < 3; ++index) {
            auto pair = loop.run(connect_pair(loop, *server));
            pool.give_back(upstream, std::move(pair.client));
            keep_open.push_back(std::move(pair.accepted));
        }

        expect(pool.idle_count(upstream) == 2_u);
    };

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

int main() {
    return boost::ut::cfg<>.run() ? 1 : 0;
}
