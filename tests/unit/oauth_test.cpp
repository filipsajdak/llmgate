#include <boost/ut.hpp>

import llmgate.json;
import llmgate.oauth;
import llmgate.proof;
import std;

namespace {

using namespace boost::ut;
using namespace llmgate;

std::string token_with(std::string_view const payload) {
    auto const as_bytes = [](std::string_view const text) {
        return std::vector<std::byte>{reinterpret_cast<std::byte const*>(text.data()), reinterpret_cast<std::byte const*>(text.data()) + text.size()};
    };
    return oauth::base64url(as_bytes(R"({"alg":"RS256"})")) + "." + oauth::base64url(as_bytes(payload)) + ".signature";
}

constexpr std::int64_t now = 1'760'000'000;

const suite oauth_suite = [] {
    "capability_base64url_matches_the_rfc_4648_vectors_without_padding"_test = [] {
        auto const encode = [](std::string_view const text) {
            return oauth::base64url(std::span{reinterpret_cast<std::byte const*>(text.data()), text.size()});
        };

        expect(encode("") == "" && encode("f") == "Zg" && encode("fo") == "Zm8" && encode("foo") == "Zm9v" && encode("foob") == "Zm9vYg");
        std::array<std::byte, 2> const odd{std::byte{0xfb}, std::byte{0xff}};
        expect(oauth::base64url(odd) == "-_8");
    };

    "capability_base64url_round_trips_and_refuses_what_is_not_base64url"_test = [] {
        auto const text = std::string{"any \xf0\x9f\x98\x80 text, with +/ bytes?"};
        auto const bytes = std::span{reinterpret_cast<std::byte const*>(text.data()), text.size()};

        auto const back = oauth::from_base64url(oauth::base64url(bytes));

        expect(back.has_value() && std::string_view{reinterpret_cast<char const*>(back->data()), back->size()} == text);
        expect(!oauth::from_base64url("a+b").has_value() && !oauth::from_base64url("a b").has_value() && !oauth::from_base64url("a").has_value());
        expect(oauth::from_base64url("").has_value() && oauth::from_base64url("")->empty());
    };

    "capability_the_pkce_challenge_matches_the_rfc_7636_example"_test = [] {
        expect(oauth::code_challenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk") == "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
    };

    "capability_a_fresh_verifier_has_an_allowed_length_and_alphabet_and_differs_each_time"_test = [] {
        auto const first = oauth::random_token().value_or("");
        auto const second = oauth::random_token().value_or("");

        expect(first.size() >= 43_u && first.size() <= 128_u && first != second);
        expect(std::ranges::all_of(first, [](char const c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }));
    };

    "capability_percent_encoding_keeps_unreserved_bytes_and_encodes_the_rest_as_utf8"_test = [] {
        expect(oauth::percent_encode("AZaz09-._~") == "AZaz09-._~");
        expect(oauth::percent_encode("http://127.0.0.1:1455/auth/callback") == "http%3A%2F%2F127.0.0.1%3A1455%2Fauth%2Fcallback");
        expect(oauth::percent_encode("openid profile") == "openid%20profile");
        expect(oauth::percent_encode("\xc5\x82") == "%C5%82");
    };

    "capability_percent_decoding_reads_escapes_and_plus_and_refuses_broken_ones"_test = [] {
        expect(oauth::percent_decode("a%20b+c%2Fd") == std::optional<std::string>{"a b c/d"});
        expect(!oauth::percent_decode("a%2").has_value() && !oauth::percent_decode("%zz").has_value());
    };

    "capability_the_authorize_url_carries_every_parameter_the_documented_flow_needs"_test = [] {
        oauth::authorization_request const request{.client_id = "dynamic_agent_client", .redirect_uri = "http://127.0.0.1:1455/auth/callback", .state = "st", .nonce = "no",
                                                   .code_challenge = "ch", .agent_name_hint = "llmgate", .host_id = "urn:uuid:11111111-2222-4333-8444-555555555555"};

        auto const url = oauth::authorize_url("https://auth.openai.com", request);

        expect(url == "https://auth.openai.com/api/accounts/authorize?response_type=code&client_id=dynamic_agent_client&redirect_uri=http%3A%2F%2F127.0.0.1%3A1455%2Fauth%2Fcallback"
                      "&scope=openid%20profile%20email%20offline_access%20resource.invoke%20chatgpt.tokens.use.direct&state=st&nonce=no&code_challenge=ch&code_challenge_method=S256"
                      "&resource=https%3A%2F%2Fapi.openai.com%2Fv1&agent_name_hint=llmgate&ext_agent_host_id=urn%3Auuid%3A11111111-2222-4333-8444-555555555555") << url;
    };

    "capability_a_returning_user_has_no_agent_name_hint_in_the_url"_test = [] {
        oauth::authorization_request const request{.client_id = "issued-id", .redirect_uri = "http://127.0.0.1:1455/auth/callback", .state = "s", .nonce = "n", .code_challenge = "c",
                                                   .host_id = "urn:uuid:x"};

        auto const url = oauth::authorize_url("https://auth.openai.com", request);

        expect(!url.contains("agent_name_hint") && url.contains("client_id=issued-id"));
    };

    "capability_the_callback_gives_the_code_state_and_issued_client_id"_test = [] {
        auto const done = oauth::parse_callback("/auth/callback?code=ab%2Bc&state=xyz&client_id=issued_1");

        expect(done.has_value() && done->code == "ab+c" && done->state == "xyz" && done->client_id == "issued_1" && done->error.empty());
    };

    "limit_a_callback_with_an_error_or_without_a_code_is_refused_with_the_reason"_test = [] {
        auto const denied = oauth::parse_callback("/auth/callback?error=access_denied&error_description=user%20said%20no&state=xyz");
        auto const empty = oauth::parse_callback("/auth/callback?state=xyz");
        auto const other = oauth::parse_callback("/somewhere/else?code=1&state=2");

        expect(denied.has_value() && denied->error == "access_denied" && denied->code.empty());
        expect(!empty.has_value() && !other.has_value());
    };

    "capability_a_jwt_payload_is_read_without_trusting_its_signature_and_a_broken_one_is_refused"_test = [] {
        auto const claims = oauth::jwt_claims(token_with(R"({"iss":"https://auth.openai.com","sub":"u1","client_id":"issued_1"})"));

        expect(claims.has_value() && json::text_at(*claims, {"sub"}) == "u1" && json::text_at(*claims, {"client_id"}) == "issued_1");
        expect(!oauth::jwt_claims("not-a-token").has_value() && !oauth::jwt_claims("a.b").has_value() && !oauth::jwt_claims("a.!!!.c").has_value());
    };

    "capability_an_id_token_is_accepted_only_for_the_right_issuer_audience_nonce_and_time"_test = [] {
        auto const text = [](std::string_view const value) { return json::make_string(std::string{value}); };
        auto const make = [&](json::value audience, std::string_view const nonce, std::int64_t const exp, std::string_view const iss = "https://auth.openai.com") {
            return json::make_object({{"iss", text(iss)}, {"aud", std::move(audience)}, {"nonce", text(nonce)}, {"exp", json::value{exp}}, {"sub", text("u")}});
        };
        constexpr std::string_view issuer = "https://auth.openai.com";

        expect(oauth::id_token_problem(make(text("cid"), "n1", now + 60), issuer, "cid", "n1", now).empty());
        expect(oauth::id_token_problem(make(json::value{json::array{text("other"), text("cid")}}, "n1", now + 60), issuer, "cid", "n1", now).empty());
        expect(oauth::id_token_problem(make(text("cid"), "n1", now + 60, "https://evil.example"), issuer, "cid", "n1", now).contains("issuer"));
        expect(oauth::id_token_problem(make(text("someone-else"), "n1", now + 60), issuer, "cid", "n1", now).contains("audience"));
        expect(oauth::id_token_problem(make(text("cid"), "other", now + 60), issuer, "cid", "n1", now).contains("nonce"));
        expect(oauth::id_token_problem(make(text("cid"), "n1", now - 1), issuer, "cid", "n1", now).contains("expired"));
        expect(oauth::id_token_problem(json::value{}, issuer, "cid", "n1", now).contains("issuer"));
    };

    "capability_the_plan_scope_is_found_as_a_whole_word_in_the_granted_scopes"_test = [] {
        expect(oauth::grants_plan_use("openid offline_access chatgpt.tokens.use.direct"));
        expect(!oauth::grants_plan_use("openid offline_access") && !oauth::grants_plan_use("chatgpt.tokens.use.directly") && !oauth::grants_plan_use(""));
    };

    "capability_a_token_reply_is_read_leniently_and_needs_an_access_token"_test = [] {
        auto const done = oauth::parse_token_reply(R"({"access_token":"a","refresh_token":"r","id_token":"i","token_type":"Bearer","expires_in":3600,"scope":"openid","earliest_refresh_at":1760003000,"future":1})");
        auto const bad = oauth::parse_token_reply(R"({"error":"invalid_grant"})");

        expect(done.has_value() && done->access_token == "a" && done->refresh_token == "r" && done->expires_in == 3600_i && done->scope == "openid");
        expect(!bad.has_value() && bad.error() == "invalid_grant");
        expect(!oauth::parse_token_reply("not json").has_value());
    };
};

}  // namespace

int main() {
    return boost::ut::cfg<>.run() ? 1 : 0;
}
