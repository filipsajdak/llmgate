#include <boost/ut.hpp>

#include "support/self_signed.hpp"

import llmgate.async;
import llmgate.net;
import llmgate.tls;
import std;

namespace {

using namespace boost::ut;
using namespace llmgate;

constexpr std::string_view loopback = "127.0.0.1";
constexpr std::string_view localhost = "localhost";
constexpr async::duration short_wait{40};

testing::credentials const& server_credentials() {
    static testing::credentials const credentials = testing::make_self_signed(std::string{localhost});
    return credentials;
}

tls::server_context make_server() {
    return std::move(*tls::server_context::make(tls::group_policy::post_quantum_preferred,
                                                server_credentials().certificate_pem, server_credentials().private_key_pem));
}

tls::client_context trusting_client() {
    return std::move(*tls::client_context::make(tls::group_policy::post_quantum_preferred, server_credentials().certificate_pem));
}

enum class server_behaviour { echo_then_close, echo_then_drop, stay_silent };

async::task<void> serve_once(async::reactor& loop, net::listener& listening, tls::server_context const& context,
                             server_behaviour const behaviour) {
    auto plain = co_await listening.accept();
    auto secure = co_await net::tls_stream::accept(loop, context, std::move(*plain));
    if (!secure) {
        co_return;
    }
    std::array<std::byte, 64> buffer{};
    if (behaviour == server_behaviour::stay_silent) {
        for (auto more = co_await secure->read_some(buffer, {}); more && *more > 0;
             more = co_await secure->read_some(buffer, {})) {
        }
        co_return;
    }
    auto const received = co_await secure->read_some(buffer, {});
    if (received && *received > 0) {
        (void)co_await secure->write_all(std::span<std::byte const>{buffer}.first(*received));
    }
    if (behaviour == server_behaviour::echo_then_close) {
        (void)co_await secure->close();
    }
}

struct exchange {
    std::error_code connect_error;
    std::error_code read_error;
    std::string echoed;
    std::string group;
    bool post_quantum = false;
    std::size_t second_read = std::numeric_limits<std::size_t>::max();
};

async::task<exchange> talk(async::reactor& loop, net::listener& listening, tls::server_context const& server,
                           tls::client_context const& client, std::string host, server_behaviour const behaviour,
                           net::read_options const options) {
    loop.spawn(serve_once(loop, listening, server, behaviour));
    exchange outcome;
    auto stream = co_await net::tls_stream::connect(loop, client, std::move(host), listening.port());
    if (!stream) {
        outcome.connect_error = stream.error();
        co_return outcome;
    }
    outcome.group = std::string{stream->session().negotiated_group()};
    outcome.post_quantum = stream->session().post_quantum();
    std::string const message = "hello over tls";
    (void)co_await stream->write_all(std::as_bytes(std::span{message}));
    std::string reply(message.size(), '\0');
    auto const first = co_await stream->read_some(std::as_writable_bytes(std::span{reply}), options);
    if (!first) {
        outcome.read_error = first.error();
        co_return outcome;
    }
    reply.resize(*first);
    outcome.echoed = reply;
    std::array<std::byte, 8> rest{};
    auto const second = co_await stream->read_some(rest, options);
    if (second) {
        outcome.second_read = *second;
    } else {
        outcome.read_error = second.error();
    }
    co_return outcome;
}

exchange run(std::string host, tls::client_context const& client, server_behaviour const behaviour,
             net::read_options const options = {}) {
    async::reactor loop;
    auto listening = net::listener::bind(loop, std::string{loopback}, 0);
    auto const server = make_server();
    return loop.run(talk(loop, *listening, server, client, std::move(host), behaviour, options));
}

const suite tls_stream_suite = [] {
    "capability_a_trusted_server_echoes_over_a_verified_handshake"_test = [] {
        auto const client = trusting_client();

        auto const result = run(std::string{localhost}, client, server_behaviour::echo_then_close);

        expect(!result.connect_error) << result.connect_error.message();
        expect(result.echoed == "hello over tls");
    };

    "capability_both_ends_negotiate_a_post_quantum_group_by_default"_test = [] {
        auto const client = trusting_client();

        auto const result = run(std::string{localhost}, client, server_behaviour::echo_then_close);

        expect(result.post_quantum) << result.group;
    };

    "capability_a_close_notify_ends_the_stream_cleanly"_test = [] {
        auto const client = trusting_client();

        auto const result = run(std::string{localhost}, client, server_behaviour::echo_then_close);

        expect(!result.read_error) << result.read_error.message();
        expect(result.second_read == 0_u);
    };

    "limit_a_connection_dropped_without_close_notify_is_reported_as_truncated"_test = [] {
        auto const client = trusting_client();

        auto const result = run(std::string{localhost}, client, server_behaviour::echo_then_drop);

        expect(result.read_error == tls::errc::truncated) << result.read_error.message();
    };

    "limit_a_server_certificate_nobody_trusts_is_rejected"_test = [] {
        auto const untrusting = std::move(*tls::client_context::make(tls::group_policy::post_quantum_preferred));

        auto const result = run(std::string{localhost}, untrusting, server_behaviour::echo_then_close);

        expect(static_cast<bool>(result.connect_error));
        expect(result.echoed.empty());
    };

    "limit_a_certificate_for_another_name_is_rejected"_test = [] {
        auto const client = trusting_client();

        auto const result = run(std::string{loopback}, client, server_behaviour::echo_then_close);

        expect(static_cast<bool>(result.connect_error));
    };

    "limit_a_silent_tls_peer_times_out_at_the_idle_limit"_test = [] {
        auto const client = trusting_client();

        auto const result = run(std::string{localhost}, client, server_behaviour::stay_silent, {.idle = short_wait});

        expect(result.read_error == std::errc::timed_out) << result.read_error.message();
    };
};

}  // namespace

int main() {
    return boost::ut::cfg<>.run() ? 1 : 0;
}
