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

    "capability_a_numeric_member_is_replaced_only_at_the_top_level"_test = [] {
        auto const swapped = replace_number_member(R"({"max_tokens":128000,"note":"max_tokens":1,"nested":{"max_tokens":5}})", "max_tokens", 64000);
        auto const clean = replace_number_member(R"({"max_tokens":128000,"nested":{"max_tokens":5},"text":"{\"max_tokens\":1}"})", "max_tokens", 64000);

        expect(!swapped.has_value());
        expect(clean.has_value() && *clean == R"({"max_tokens":64000,"nested":{"max_tokens":5},"text":"{\"max_tokens\":1}"})");
    };

    "limit_a_numeric_replacement_refuses_a_missing_or_non_numeric_member"_test = [] {
        expect(replace_number_member(R"({"a":1})", "max_tokens", 5).error() == errc::member_missing);
        expect(replace_number_member(R"({"max_tokens":"big"})", "max_tokens", 5).error() == errc::member_not_a_number);
        expect(replace_number_member(R"({"max_tokens":null})", "max_tokens", 5).error() == errc::member_not_a_number);
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

    "capability_a_unicode_escape_is_encoded_with_the_utf8_length_of_its_code_point"_test = [] {
        expect(decode_string(R"(A)") == "A");
        expect(decode_string(R"(\u007f)") == "\x7f");
        expect(decode_string(R"(\u0080)") == "\xc2\x80");
        expect(decode_string(R"(߿)") == "\xdf\xbf");
        expect(decode_string(R"(ࠀ)") == "\xe0\xa0\x80");
        expect(decode_string(R"(￿)") == "\xef\xbf\xbf");
        expect(decode_string(R"(𐀀)") == "\xf0\x90\x80\x80");
        expect(decode_string(R"(􏿿)") == "\xf4\x8f\xbf\xbf");
    };

    "limit_a_lone_or_mismatched_surrogate_is_encoded_as_it_stands"_test = [] {
        expect(decode_string(R"(\uD800)") == "\xed\xa0\x80");
        expect(decode_string(R"(\uDC00)") == "\xed\xb0\x80");
        expect(decode_string(R"(\uD800x)") == "\xed\xa0\x80x");
        expect(decode_string(R"(\uD800\n)") == "\xed\xa0\x80\n");
        expect(decode_string(R"(\uD800A)") == "\xed\xa0\x80" "A");
        expect(decode_string(R"(\uD800\uZZZZ)") == "\xed\xa0\x80uZZZZ");
        expect(decode_string(R"(\uD800\u)") == "\xed\xa0\x80u");
    };

    "limit_an_incomplete_unicode_escape_decodes_to_the_letter_u"_test = [] {
        expect(decode_string(R"(\u12)") == "u12");
        expect(decode_string(R"(\uZZZZ)") == "uZZZZ");
        expect(decode_string(R"(\u123)") == "u123");
    };

    "limit_a_lone_trailing_backslash_and_unknown_escapes_are_kept_readable"_test = [] {
        expect(decode_string("a\\") == "a\\");
        expect(decode_string(R"(\q\/\"\\)") == "q/\"\\");
        expect(decode_string(R"(\b\f\r\t)") == "\b\f\r\t");
    };

    "capability_encode_escapes_quotes_control_characters_and_leaves_the_rest"_test = [] {
        expect(encode_string("a\"b\\c\n\r\t") == R"("a\"b\\c\n\r\t")");
        expect(encode_string(std::string_view{"\x01\x1f\x7f \xc3\xa9", 6}) == "\"\\u0001\\u001f\x7f \xc3\xa9\"");
        expect(encode_string("") == "\"\"");
    };

    "limit_container_values_report_their_kind_and_mismatched_closers_are_refused"_test = [] {
        auto const members = top_level_members(R"({"a":{"x":[1]},"b":[],"c":-1,"d":null,"e":"s"})");

        expect(members.has_value());
        expect((*members)[0].kind == value_kind::object);
        expect((*members)[1].kind == value_kind::array);
        expect((*members)[2].kind == value_kind::number);
        expect((*members)[3].kind == value_kind::literal);
        expect((*members)[4].kind == value_kind::string);
        for (std::string_view const text : {R"({"a":[}})", R"({"a":{]})", R"({"a":})", R"({"a":[)", R"({"a":["x)"}) {
            expect(top_level_members(text).error() == errc::malformed) << text;
        }
        expect(top_level_members(R"({"a":[1,2]})").has_value());
        expect(top_level_members(R"({"a":[1 2]})").has_value());
    };

    "limit_member_syntax_errors_and_trailing_bytes_are_told_apart"_test = [] {
        expect(top_level_members(R"({"a":1} x)").error() == errc::trailing_bytes);
        expect(top_level_members("{} x").error() == errc::trailing_bytes);
        expect(top_level_members(R"([])").error() == errc::not_an_object);
        expect(top_level_members(R"({a:1})").error() == errc::malformed);
        expect(top_level_members(R"({"a" 1})").error() == errc::malformed);
        expect(top_level_members(R"({"a":1 "b":2})").error() == errc::malformed);
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
