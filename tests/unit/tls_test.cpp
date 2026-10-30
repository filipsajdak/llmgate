#include <boost/ut.hpp>

import llmgate.tls;
import std;

namespace {

using namespace boost::ut;
using namespace llmgate::tls;

constexpr std::uint16_t mlkem_group = 0x11EC;
constexpr std::byte tls_handshake_record{0x16};
constexpr std::size_t record_header_size = 5;
constexpr std::size_t handshake_header_size = 4;
constexpr std::size_t version_and_random_size = 2 + 32;
constexpr std::uint16_t supported_groups_extension = 0x000A;
constexpr std::size_t byte_bits = 8;

struct hello_reader {
    std::span<std::byte const> bytes;
    std::size_t at = 0;

    std::size_t number(std::size_t const width) {
        std::size_t value = 0;
        for (std::size_t used = 0; used < width && at < bytes.size(); ++used) {
            value = (value << byte_bits) | std::to_integer<std::size_t>(bytes[at++]);
        }
        return value;
    }

    void skip_vector(std::size_t const length_width) { at += number(length_width); }
};

std::vector<std::uint16_t> offered_groups(std::span<std::byte const> hello) {
    hello_reader in{.bytes = hello, .at = record_header_size + handshake_header_size + version_and_random_size};
    in.skip_vector(1);
    in.skip_vector(2);
    in.skip_vector(1);
    auto const extensions_end = in.number(2) + in.at;
    std::vector<std::uint16_t> groups;
    while (in.at + 4 <= std::min(extensions_end, hello.size())) {
        auto const type = in.number(2);
        auto const length = in.number(2);
        auto const next = in.at + length;
        if (type == supported_groups_extension) {
            auto const listed = in.number(2);
            for (std::size_t used = 0; used < listed; used += 2) {
                groups.push_back(static_cast<std::uint16_t>(in.number(2)));
            }
        }
        in.at = next;
    }
    return groups;
}

bool contains_group(std::span<std::byte const> hello) {
    return std::ranges::contains(offered_groups(hello), mlkem_group);
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
        expect(progress.error() == errc::handshake_failed) << progress.error().message() << " / " << progress.error().category().name() << " / " << progress.error().value();
        expect(!connection->diagnostic().empty());
    };

    "limit_error_codes_carry_their_enumerator_name"_test = [] {
        expect(make_error_code(errc::truncated).message() == "truncated");
        expect(std::string{make_error_code(errc::peer_closed).category().name()} == "llmgate::tls::errc");
    };

    "limit_nothing_is_negotiated_before_the_handshake"_test = [] {
        auto context = client_context::make(group_policy::post_quantum_preferred);
        auto connection = connection::make(*context, "example.com");

        expect(connection->negotiated_group().empty());
        expect(!connection->post_quantum());
    };
};

}  // namespace

int main() {
    return boost::ut::cfg<>.run() ? 1 : 0;
}
