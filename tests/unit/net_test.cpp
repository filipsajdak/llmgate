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

async::task<void> echo_once(net::tcp_stream stream) {
    std::array<std::byte, echo_buffer_bytes> buffer{};
    auto const received = co_await stream.read_some(buffer, std::nullopt);
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
    auto const received = co_await client->read_some(std::as_writable_bytes(std::span{reply}), std::nullopt);
    reply.resize(received.value_or(0));
    co_return reply;
}

async::task<std::error_code> read_from_silent_peer(async::reactor& loop, net::listener& server) {
    async::spawn([](net::listener& listening) -> async::task<void> {
        auto accepted = co_await listening.accept();
        std::array<std::byte, 1> hold{};
        if (accepted) {
            (void)co_await accepted->read_some(hold, std::nullopt);
        }
    }(server));
    auto client = co_await net::tcp_stream::connect(loop, std::string{loopback}, server.port());
    std::array<std::byte, 1> buffer{};
    auto const received = co_await client->read_some(buffer, short_wait);
    co_return received ? std::error_code{} : received.error();
}

async::task<std::size_t> read_after_peer_closes(async::reactor& loop, net::listener& server) {
    async::spawn([](net::listener& listening) -> async::task<void> {
        auto accepted = co_await listening.accept();
        (void)accepted;
    }(server));
    auto client = co_await net::tcp_stream::connect(loop, std::string{loopback}, server.port());
    std::array<std::byte, 1> buffer{};
    auto const received = co_await client->read_some(buffer, std::nullopt);
    co_return received.value_or(std::numeric_limits<std::size_t>::max());
}

const suite net_suite = [] {
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
