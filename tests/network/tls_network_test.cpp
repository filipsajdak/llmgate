#include <boost/ut.hpp>

import llmgate.async;
import llmgate.net;
import llmgate.tls;
import std;

namespace {

using namespace boost::ut;
using namespace llmgate;

constexpr std::uint16_t https_port = 443;
constexpr std::string_view status_line_prefix = "HTTP/1.1 ";
constexpr std::size_t response_head_bytes = 64;

struct exchange {
    std::string first_bytes{};
    std::string group{};
    std::string version{};
    bool post_quantum{};
    std::error_code error{};
};

async::task<exchange> get_root(async::reactor& loop, tls::group_policy const policy, std::string host) {
    auto context = tls::client_context::make(policy);
    if (!context) {
        co_return exchange{.error = context.error()};
    }
    auto stream = co_await net::tls_stream::connect(loop, *context, host, https_port);
    if (!stream) {
        co_return exchange{.error = stream.error()};
    }
    auto const request = std::format("GET / HTTP/1.1\r\nHost: {}\r\nUser-Agent: llmgate-s0\r\nConnection: close\r\n\r\n", host);
    if (auto const sent = co_await stream->write_all(std::as_bytes(std::span{request})); !sent) {
        co_return exchange{.error = sent.error()};
    }
    std::string head(response_head_bytes, '\0');
    auto const received = co_await stream->read_some(std::as_writable_bytes(std::span{head}), {});
    if (!received) {
        co_return exchange{.error = received.error()};
    }
    head.resize(*received);
    auto const& session = stream->session();
    co_return exchange{.first_bytes = std::move(head),
                       .group = std::string{session.negotiated_group()},
                       .version = std::string{session.version()},
                       .post_quantum = session.post_quantum()};
}

exchange fetch(tls::group_policy const policy, std::string host) {
    async::reactor loop;
    return loop.run(get_root(loop, policy, std::move(host)));
}

const suite network_suite = [] {
    "capability_anthropic_negotiates_a_post_quantum_group"_test = [] {
        auto const result = fetch(tls::group_policy::post_quantum_required, "api.anthropic.com");

        std::println("anthropic: {} {} pq={}", result.version, result.group, result.post_quantum);
        expect(!result.error) << result.error.message();
        expect(result.post_quantum);
        expect(result.first_bytes.starts_with(status_line_prefix));
    };

    "capability_copilot_answers_with_the_preferred_policy"_test = [] {
        auto const result = fetch(tls::group_policy::post_quantum_preferred, "api.githubcopilot.com");

        std::println("copilot: {} {} pq={}", result.version, result.group, result.post_quantum);
        expect(!result.error) << result.error.message();
        expect(result.first_bytes.starts_with(status_line_prefix));
    };

    "limit_classical_only_policy_never_negotiates_a_post_quantum_group"_test = [] {
        auto const result = fetch(tls::group_policy::classical_only, "api.anthropic.com");

        expect(!result.error) << result.error.message();
        expect(!result.post_quantum);
    };

    "limit_required_policy_fails_against_a_server_without_the_group"_test = [] {
        auto const result = fetch(tls::group_policy::post_quantum_required, "api.githubcopilot.com");

        expect(static_cast<bool>(result.error));
    };
};

}  // namespace

int main() {}
