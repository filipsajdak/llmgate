#include <boost/ut.hpp>

import llmgate.tls;
import std;

namespace {

using namespace boost::ut;
using namespace llmgate::tls;

constexpr std::byte mlkem_group_high{0x11};
constexpr std::byte mlkem_group_low{0xEC};
constexpr std::byte tls_handshake_record{0x16};

bool contains_group(std::span<std::byte const> hello) {
    return std::ranges::contains_subrange(hello, std::array{mlkem_group_high, mlkem_group_low});
}

std::vector<std::byte> client_hello(group_policy const policy) {
    auto context = client_context::make(policy);
    auto connection = connection::make(*context, "example.com");
    (void)connection->handshake();
    return connection->take_outgoing();
}

const suite tls_suite = [] {
    "capability_client_hello_is_a_handshake_record"_test = [] {
        auto const hello = client_hello(group_policy::post_quantum_preferred);

        expect(!hello.empty());
        expect(hello.front() == tls_handshake_record);
    };

    "capability_preferred_policy_offers_the_post_quantum_group"_test = [] {
        expect(contains_group(client_hello(group_policy::post_quantum_preferred)));
    };

    "capability_required_policy_offers_the_post_quantum_group"_test = [] {
        expect(contains_group(client_hello(group_policy::post_quantum_required)));
    };

    "capability_classical_policy_does_not_offer_the_post_quantum_group"_test = [] {
        expect(!contains_group(client_hello(group_policy::classical_only)));
    };

    "capability_handshake_waits_for_the_server_after_the_client_hello"_test = [] {
        auto context = client_context::make(group_policy::post_quantum_preferred);
        auto connection = connection::make(*context, "example.com");

        auto const progress = connection->handshake();

        expect(progress.has_value());
        expect(*progress == want::read);
    };

    "limit_garbage_from_the_server_fails_the_handshake_with_a_diagnostic"_test = [] {
        auto context = client_context::make(group_policy::post_quantum_preferred);
        auto connection = connection::make(*context, "example.com");
        (void)connection->handshake();
        std::string const garbage(64, 'x');

        (void)connection->feed(std::as_bytes(std::span{garbage}));
        auto const progress = connection->handshake();

        expect(!progress.has_value());
        expect(progress.error() == errc::handshake_failed);
        expect(!connection->diagnostic().empty());
    };

    "limit_error_codes_carry_their_enumerator_name"_test = [] {
        expect(make_error_code(errc::truncated).message() == "truncated");
        expect(std::string{make_error_code(errc::peer_closed).category().name()} == "llmgate.tls");
    };

    "limit_nothing_is_negotiated_before_the_handshake"_test = [] {
        auto context = client_context::make(group_policy::post_quantum_preferred);
        auto connection = connection::make(*context, "example.com");

        expect(connection->negotiated_group().empty());
        expect(!connection->post_quantum());
    };
};

}  // namespace

int main() {}
