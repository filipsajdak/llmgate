#include <boost/ut.hpp>

import llmgate.proof;
import std;

namespace {

using namespace boost::ut;
using namespace llmgate::proof;

const suite proof_suite = [] {
    "capability_hmac_sha256_matches_rfc_4231_case_1"_test = [] {
        auto const key = std::string(20, '\x0b');

        expect(hmac_sha256_hex(key, "Hi There") == "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    };

    "capability_hmac_sha256_matches_rfc_4231_case_2"_test = [] {
        expect(hmac_sha256_hex("Jefe", "what do ya want for nothing?") == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    };

    "limit_a_different_key_or_message_gives_a_different_proof"_test = [] {
        auto const base = hmac_sha256_hex("key", "challenge");

        expect(base != hmac_sha256_hex("kez", "challenge"));
        expect(base != hmac_sha256_hex("key", "challengf"));
        expect(base == hmac_sha256_hex("key", "challenge"));
    };

    "capability_the_identity_message_binds_tag_scope_and_challenge"_test = [] {
        expect(identity_message("127.0.0.1:9480", "ab") == "llmgate-identity-v1|127.0.0.1:9480|ab");
        expect(hmac_sha256_hex("k", identity_message("h:1", "ab")) != hmac_sha256_hex("k", identity_message("h:2", "ab")));
    };

    "capability_random_hex_has_the_requested_length_and_differs_between_calls"_test = [] {
        auto const first = random_hex(32);
        auto const second = random_hex(32);

        expect(first.has_value() && second.has_value());
        expect(first->size() == 64_u);
        expect(*first != *second);
        expect(std::ranges::all_of(*first, [](char const c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }));
    };

    "capability_hex_encoding_covers_every_byte_value"_test = [] {
        std::array<unsigned char, 3> bytes{0x00, 0x0f, 0xff};

        expect(to_hex(bytes) == "000fff");
    };
};

}  // namespace

int main() {
    return boost::ut::cfg<>.run() ? 1 : 0;
}
