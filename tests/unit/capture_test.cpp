#include <boost/ut.hpp>

import llmgate.capture;
import llmgate.http;
import llmgate.json;
import std;

namespace {

using namespace boost::ut;
using namespace llmgate;

const suite capture_suite = [] {
    "capability_credential_headers_are_redacted_whatever_their_case_and_the_rest_are_kept"_test = [] {
        http::header_map fields;
        fields.add("Authorization", "Bearer secret-token");
        fields.add("X-Api-Key", "k-123");
        fields.add("cookie", "a=b");
        fields.add("Set-Cookie", "c=d");
        fields.add("Proxy-Authorization", "Basic xyz");
        fields.add("anthropic-beta", "tools-2024");
        fields.add("Content-Type", "application/json");

        auto const shown = capture::redacted_headers(fields);

        expect(shown.at("Authorization") == "<redacted>" && shown.at("X-Api-Key") == "<redacted>" && shown.at("cookie") == "<redacted>");
        expect(shown.at("Set-Cookie") == "<redacted>" && shown.at("Proxy-Authorization") == "<redacted>");
        expect(shown.at("anthropic-beta") == "tools-2024" && shown.at("Content-Type") == "application/json");
    };

    "limit_a_body_over_the_limit_is_cut_and_says_so"_test = [] {
        bool long_cut = false;
        auto const kept = capture::bounded("abcdefghij", 4, long_cut);
        bool short_cut = true;
        auto const all = capture::bounded("abc", 4, short_cut);

        expect(kept == "abcd" && long_cut);
        expect(all == "abc" && !short_cut);
    };

    "capability_an_exchange_is_one_json_line_with_every_part"_test = [] {
        capture::exchange seen{.time = 5, .session = "s1", .provider = "p", .requested = "opus", .kind = "classifier", .verdict = "allow", .status = 200,
                               .request_headers = {{"x", "1"}}, .request_body = "{\"a\":1}", .reply_headers = {{"content-type", "text/event-stream"}}, .reply_body = "event: ping\n\n"};

        auto const line = capture::line_of(seen);

        expect(line.ends_with("}\n") && std::ranges::count(line, '\n') == 1);
        auto const parsed = json::parse(line, {.jsonc = false});
        expect(parsed.has_value());
        expect(json::text_at(*parsed, {"session"}) == "s1" && json::text_at(*parsed, {"request_body"}) == "{\"a\":1}" && json::text_at(*parsed, {"reply_body"}) == "event: ping\n\n");
        expect(json::text_at(*parsed, {"request_headers", "x"}) == "1" && json::text_at(*parsed, {"verdict"}) == "allow");
    };

    "limit_a_body_with_control_characters_stays_on_one_line"_test = [] {
        capture::exchange seen{.request_body = "line one\nline two\u0001", .reply_body = "\r\n\t"};

        auto const line = capture::line_of(seen);

        expect(std::ranges::count(line, '\n') == 1);
        auto const parsed = json::parse(line, {.jsonc = false});
        expect(parsed && json::text_at(*parsed, {"request_body"}) == "line one\nline two\u0001");
    };
};

}  // namespace

int main() {
    return boost::ut::cfg<>.run() ? 1 : 0;
}
