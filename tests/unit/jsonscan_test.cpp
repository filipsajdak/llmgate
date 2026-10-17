#include <boost/ut.hpp>

import llmgate.jsonscan;
import std;

namespace {

using namespace boost::ut;
using namespace llmgate::jsonscan;

constexpr std::string_view request =
    R"({"model":"claude-opus","max_tokens":10,"stream":true,"messages":[{"role":"user","content":"hi {\"model\":\"x\"}"}]})";

std::string value_of(std::string_view const text, member const& item) {
    return std::string{text.substr(item.value_begin, item.value_end - item.value_begin)};
}

const suite scan_suite = [] {
    "capability_top_level_members_report_their_value_ranges"_test = [] {
        auto const members = top_level_members(request);

        expect(members.has_value());
        expect(members->size() == 4_u);
        expect(value_of(request, (*members)[0]) == "\"claude-opus\"");
        expect(value_of(request, (*members)[2]) == "true");
        expect((*members)[3].kind == value_kind::array);
    };

    "capability_a_nested_model_key_is_not_a_top_level_member"_test = [] {
        auto const members = top_level_members(request);

        expect(members_named(*members, "model").size() == 1_u);
    };

    "capability_replace_swaps_only_the_top_level_model"_test = [] {
        auto const swapped = replace_string_member(request, "model", "gpt-5");

        expect(swapped.has_value());
        expect(swapped->starts_with(R"({"model":"gpt-5","max_tokens":10)"));
        expect(swapped->contains(R"(hi {\"model\":\"x\"})"));
    };

    "limit_an_escaped_spelling_of_the_key_is_still_the_model_key"_test = [] {
        auto const swapped = replace_string_member(R"({"model":"a","x":1})", "model", "b");

        expect(swapped.has_value());
        expect(*swapped == R"({"model":"b","x":1})");
    };

    "limit_every_duplicate_of_the_key_is_replaced"_test = [] {
        auto const swapped = replace_string_member(R"({"model":"a","model":"b"})", "model", "c");

        expect(*swapped == R"({"model":"c","model":"c"})");
    };

    "limit_the_replacement_is_escaped_so_it_cannot_close_the_string"_test = [] {
        auto const swapped = replace_string_member(R"({"model":"a"})", "model", R"(x","admin":"yes)");
        auto const members = top_level_members(*swapped);

        expect(members->size() == 1_u);
        expect(decode_string(std::string_view{*swapped}.substr((*members)[0].value_begin + 1, (*members)[0].value_end - (*members)[0].value_begin - 2)) == R"(x","admin":"yes)");
    };

    "limit_a_missing_or_non_string_model_is_reported"_test = [] {
        expect(replace_string_member(R"({"a":1})", "model", "b").error() == errc::member_missing);
        expect(replace_string_member(R"({"model":7})", "model", "b").error() == errc::member_not_a_string);
    };

    "limit_malformed_input_is_rejected_without_reading_past_the_end"_test = [] {
        for (std::string_view const text : {"", "{", "[1]", R"({"a")", R"({"a":})", R"({"a":"x)", R"({"a":1,})", R"({"a":[1,2})",
                                            R"({"a":1} trailing)", R"({"a":"\x"})", R"({"a":"\u12"})"}) {
            expect(!top_level_members(text).has_value()) << text;
        }
    };

    "limit_nesting_beyond_the_depth_limit_is_refused"_test = [] {
        std::string deep = R"({"a":)" + std::string(max_depth + 1, '[') + std::string(max_depth + 1, ']') + "}";

        expect(top_level_members(deep).error() == errc::too_deep);
    };

    "capability_decode_handles_escapes_and_surrogate_pairs"_test = [] {
        expect(decode_string(R"(a\nbé😀)") == "a\nb\xc3\xa9\xf0\x9f\x98\x80");
    };

    "capability_the_scanner_accepts_whitespace_and_empty_objects"_test = [] {
        expect(top_level_members(" { } ").has_value());
        expect(top_level_members("{ \"a\" : [ 1 , { \"b\" : null } ] }")->size() == 1_u);
    };
};

}  // namespace

int main() {
    return boost::ut::cfg<>.run() ? 1 : 0;
}
