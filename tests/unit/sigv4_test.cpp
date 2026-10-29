#include <boost/ut.hpp>

import llmgate.sigv4;
import std;

namespace {

using namespace boost::ut;
namespace v4 = llmgate::sigv4;

constexpr std::string_view stamp = "20150830T123600Z";

v4::credentials const keys{.access_key = "AKIDEXAMPLE", .secret_key = "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY"};
v4::scope const where{.region = "us-east-1", .service = "bedrock"};

std::string proof_of(v4::signature const& made) {
    return made.authorization.substr(made.authorization.find("Signature=") + 10);
}

suite<"sigv4"> tests = [] {
    "capability_the_stamp_is_the_basic_iso_form_in_utc"_test = [] {
        expect(v4::stamp_of(std::chrono::sys_days{std::chrono::year{2015} / 8 / 30} + std::chrono::hours{12} + std::chrono::minutes{36}) == stamp);
    };

    "capability_a_colon_in_the_path_is_signed_encoded_twice_and_a_plain_path_once"_test = [] {
        auto const once = v4::sign({.method = "POST", .target = "/model/a.b/invoke", .host = "h"}, keys, where, stamp);
        auto const twice = v4::sign({.method = "POST", .target = "/model/a.b%3A0/invoke", .host = "h"}, keys, where, stamp);
        auto const raw = v4::sign({.method = "POST", .target = "/model/a.b:0/invoke", .host = "h"}, keys, where, stamp);

        expect(proof_of(once) != proof_of(twice) && proof_of(twice) != proof_of(raw));
        expect(v4::detail::encode_path("/model/a.b%3A0/invoke") == "/model/a.b%253A0/invoke" && v4::detail::encode_path("/model/a.b:0/invoke") == "/model/a.b%3A0/invoke");
    };

    "capability_a_session_token_is_signed_and_handed_back_for_its_header"_test = [] {
        auto const with_token = v4::sign({.method = "POST", .target = "/x", .host = "h", .body = "{}"}, {.access_key = "A", .secret_key = "S", .session_token = "T"}, where, stamp);
        auto const without = v4::sign({.method = "POST", .target = "/x", .host = "h", .body = "{}"}, {.access_key = "A", .secret_key = "S"}, where, stamp);

        expect(with_token.security_token == "T" && with_token.authorization.contains("SignedHeaders=host;x-amz-date;x-amz-security-token,"));
        expect(without.security_token.empty() && without.authorization.contains("SignedHeaders=host;x-amz-date,"));
    };

    "capability_the_body_changes_the_signature_and_the_credential_scope_names_date_region_and_service"_test = [] {
        auto const first = v4::sign({.method = "POST", .target = "/x", .host = "h", .body = "a"}, keys, where, stamp);
        auto const second = v4::sign({.method = "POST", .target = "/x", .host = "h", .body = "b"}, keys, where, stamp);

        expect(proof_of(first) != proof_of(second));
        expect(first.authorization.contains("Credential=AKIDEXAMPLE/20150830/us-east-1/bedrock/aws4_request"));
    };

    "capability_query_pairs_are_sorted_and_a_bare_key_gets_an_empty_value"_test = [] {
        expect(v4::detail::sorted_query("b=2&a=1&c") == "a=1&b=2&c=");
        auto const left = v4::sign({.method = "GET", .target = "/p?b=2&a=1", .host = "h"}, keys, where, stamp);
        auto const right = v4::sign({.method = "GET", .target = "/p?a=1&b=2", .host = "h"}, keys, where, stamp);
        expect(proof_of(left) == proof_of(right));
    };

    "capability_a_key_longer_than_a_block_is_hashed_first"_test = [] {
        expect(v4::hex(v4::hmac_sha256(std::string(80, 'k'), "m")) == v4::hex(v4::hmac_sha256(std::string{v4::view_of(v4::sha256(std::string(80, 'k')))}, "m")));
    };
};

}  // namespace

int main() {}
