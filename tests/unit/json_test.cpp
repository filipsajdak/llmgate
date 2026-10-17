#include <boost/ut.hpp>

import llmgate.json;
import std;

namespace {

using namespace boost::ut;
using namespace llmgate::json;
using std::chrono::milliseconds;

enum class level { quiet, loud };

struct limits {
    milliseconds idle = std::chrono::seconds{60};
    std::int32_t max_messages = 100;
};

struct settings {
    [[= required]] std::string listen{};
    bool verbose = false;
    level mode = level::quiet;
    limits timeouts{};
    std::vector<std::string> tags{};
    std::map<std::string, std::string> models{};
    std::optional<double> ratio{};
};

constexpr std::string_view full_config = R"(
{
  // where to listen
  "listen": "127.0.0.1:9480",
  "verbose": true,
  "mode": "loud",
  "timeouts": { "idle": "90s", "max_messages": 7, },   /* trailing comma */
  "tags": ["a", "b"],
  "models": { "opus": "claude-opus-4.5", "sonnet": "claude-sonnet-4.5" },
  "ratio": 0.5
}
)";

const suite json_suite = [] {
    "capability_path_helpers_read_nested_text_and_counts_and_ignore_the_wrong_type"_test = [] {
        auto const parsed = parse(R"({"a":{"b":"text","n":5,"f":2.5,"neg":-1,"s":"x"},"list":[1]})");

        expect(text_at(*parsed, {"a", "b"}) == "text");
        expect(text_at(*parsed, {"a", "n"}).empty());
        expect(count_at(*parsed, {"a", "n"}) == std::optional<std::uint64_t>{5});
        expect(count_at(*parsed, {"a", "f"}) == std::optional<std::uint64_t>{2});
        expect(!count_at(*parsed, {"a", "neg"}).has_value());
        expect(!count_at(*parsed, {"a", "s"}).has_value());
        expect(at(*parsed, {"a", "missing"}) == nullptr);
        expect(at(*parsed, {"list", "x"}) == nullptr);
        expect(at(*parsed, {}) == &*parsed);
    };

    "capability_a_config_is_read_into_the_struct_by_member_name"_test = [] {
        auto const loaded = read_text<settings>(full_config);

        expect(loaded.has_value());
        expect(loaded->listen == "127.0.0.1:9480");
        expect(loaded->verbose);
        expect(loaded->mode == level::loud);
        expect(loaded->timeouts.idle == std::chrono::seconds{90});
        expect(loaded->timeouts.max_messages == 7);
        expect(loaded->tags == std::vector<std::string>{"a", "b"});
        expect(loaded->models.at("opus") == "claude-opus-4.5");
        expect(loaded->ratio == 0.5);
    };

    "capability_missing_optional_members_keep_their_defaults"_test = [] {
        auto const loaded = read_text<settings>(R"({"listen":"x"})");

        expect(loaded.has_value());
        expect(!loaded->verbose);
        expect(loaded->timeouts.idle == std::chrono::seconds{60});
        expect(loaded->timeouts.max_messages == 100);
        expect(!loaded->ratio.has_value());
    };

    "limit_a_missing_required_member_is_reported_with_its_path"_test = [] {
        auto const loaded = read_text<settings>("{}");

        expect(!loaded.has_value());
        expect(loaded.error().path == "listen");
        expect(loaded.error().message == "required");
    };

    "capability_unknown_keys_can_be_allowed_for_third_party_replies"_test = [] {
        auto const loaded = read_text<settings>(R"({"listen":"x","extra":{"deep":[1]}})", {}, {.allow_unknown_keys = true});

        expect(loaded.has_value());
        expect(loaded->listen == "x");
    };

    "limit_an_unknown_key_is_rejected_so_typos_cannot_hide"_test = [] {
        auto const loaded = read_text<settings>(R"({"listen":"x","verbos":true})");

        expect(!loaded.has_value());
        expect(loaded.error().path == "verbos");
        expect(loaded.error().message == "unknown key");
    };

    "limit_a_wrong_type_deep_in_the_tree_names_the_whole_path"_test = [] {
        auto const loaded = read_text<settings>(R"({"listen":"x","timeouts":{"max_messages":"many"}})");

        expect(loaded.error().path == "timeouts.max_messages");
        expect(loaded.error().message == "expected a whole number");
    };

    "limit_array_elements_are_addressed_by_index"_test = [] {
        auto const loaded = read_text<settings>(R"({"listen":"x","tags":["a",2]})");

        expect(loaded.error().path == "tags[1]");
    };

    "limit_a_number_outside_the_member_type_is_rejected"_test = [] {
        auto const loaded = read_text<settings>(R"({"listen":"x","timeouts":{"max_messages":99999999999}})");

        expect(loaded.error().message == "number is out of range");
    };

    "limit_bad_durations_and_enum_names_are_rejected"_test = [] {
        expect(read_text<settings>(R"({"listen":"x","timeouts":{"idle":"soon"}})").error().path == "timeouts.idle");
        expect(read_text<settings>(R"({"listen":"x","timeouts":{"idle":"5d"}})").error().message == "unknown duration unit");
        expect(read_text<settings>(R"({"listen":"x","mode":"deafening"})").error().path == "mode");
    };

    "capability_durations_accept_every_unit"_test = [] {
        auto const loaded = read_text<limits>(R"({"idle":"250ms"})");
        auto const minutes = read_text<limits>(R"({"idle":"2m"})");
        auto const hours = read_text<limits>(R"({"idle":"1h"})");

        expect(loaded->idle == milliseconds{250});
        expect(minutes->idle == std::chrono::minutes{2});
        expect(hours->idle == std::chrono::hours{1});
    };

    "limit_malformed_json_reports_an_offset"_test = [] {
        for (std::string_view const text : {"", "{", R"({"a":})", R"({"a" 1})", R"({"a":1,,})", "[1,", R"({"a":"x)", "{} extra"}) {
            expect(!parse(text).has_value()) << text;
        }
    };

    "limit_comments_are_refused_when_strict_json_is_requested"_test = [] {
        expect(!parse("{} // note", {.jsonc = false}).has_value());
        expect(parse("{} // note").has_value());
    };

    "limit_deeply_nested_input_is_refused"_test = [] {
        std::string deep(100, '[');
        deep += std::string(100, ']');

        expect(!parse(deep).has_value());
    };

    "capability_written_json_parses_back_to_the_same_document"_test = [] {
        auto const parsed = parse(R"({"a":[1,{"b":null,"c":"x\ny"}],"d":true,"e":-3,"f":[],"g":{}})");

        auto const compact = write(*parsed);

        expect(compact == R"({"a":[1,{"b":null,"c":"x\ny"}],"d":true,"e":-3,"f":[],"g":{}})");
        expect(write(*parse(compact)) == compact);
    };

    "capability_pretty_output_indents_nested_values_and_ends_with_a_newline"_test = [] {
        auto const parsed = parse(R"({"a":[1,2],"b":{"c":true},"d":[]})");

        auto const text = write(*parsed, 2);

        expect(text == "{\n  \"a\": [\n    1,\n    2\n  ],\n  \"b\": {\n    \"c\": true\n  },\n  \"d\": []\n}\n");
    };

    "limit_control_characters_and_quotes_in_strings_are_escaped_on_write"_test = [] {
        auto const value = make_object({{"k\"", make_string("a\"b\\c\x01")}});

        auto const text = write(value);

        expect(text == R"({"k\"":"a\"b\\c\u0001"})");
        expect(std::get<std::string>(std::get<object>(parse(text)->data)[0].second.data) == "a\"b\\c\x01");
    };

    "capability_scalars_and_escapes_round_trip_through_the_dom"_test = [] {
        auto const parsed = parse(R"({"n":-12,"f":1.5e2,"s":"a\nbé","t":true,"z":null})");

        expect(parsed.has_value());
        auto const& members = std::get<object>(parsed->data);
        expect(std::get<std::int64_t>(members[0].second.data) == -12);
        expect(std::get<double>(members[1].second.data) == 150.0);
        expect(std::get<std::string>(members[2].second.data) == "a\nb\xc3\xa9");
    };

    "capability_every_string_escape_decodes_to_its_character"_test = [] {
        auto const parsed = parse(R"(["\n\t\r\b\f\"\\\/\q","A","é","€"])");

        expect(parsed.has_value());
        auto const& items = std::get<array>(parsed->data);
        expect(std::get<std::string>(items[0].data) == "\n\t\r\b\f\"\\/q");
        expect(std::get<std::string>(items[1].data) == "A");
        expect(std::get<std::string>(items[2].data) == "\xc3\xa9");
        expect(std::get<std::string>(items[3].data) == "\xe2\x82\xac");
    };

    "limit_a_truncated_unicode_escape_decodes_to_nothing_and_a_lone_backslash_is_unterminated"_test = [] {
        auto const parsed = parse(R"(["\u12"])");

        expect(!parsed.has_value());
        expect(!parse(R"("\)").has_value());
        expect(!parse("\"a\x01\"").has_value());
    };

    "limit_comments_end_at_the_line_or_the_closing_marker_and_a_lone_slash_is_not_one"_test = [] {
        expect(parse("[1] // trailing, no newline").has_value());
        expect(parse("/* lead */ [1] /* trail */").has_value());
        expect(parse("[1] /* never closed").has_value());
        expect(!parse("[1] / 2").has_value());
        expect(!parse("[1] /").has_value());
        expect(parse("[1, // one\n 2]").has_value());
    };

    "capability_keywords_and_numbers_are_recognised_and_other_tokens_refused"_test = [] {
        auto const parsed = parse("[false,true,null,-0,2.5]");

        expect(parsed.has_value());
        auto const& items = std::get<array>(parsed->data);
        expect(std::get<bool>(items[0].data) == false);
        expect(std::get<bool>(items[1].data) == true);
        expect(std::holds_alternative<std::nullptr_t>(items[2].data));
        expect(std::get<double>(items[4].data) == 2.5);
        for (std::string_view const text : {"[nul]", "[1x]", "[+]", "[--1]", "[tru e]"}) {
            expect(!parse(text).has_value()) << text;
        }
    };

    "limit_container_syntax_errors_report_where_they_stop"_test = [] {
        expect(parse("[1 2]").error().message == "expected ',' or ']'");
        expect(parse(R"({"a":1 "b":2})").error().message == "expected ',' or '}'");
        expect(parse("{1:2}").error().message == "expected a string key");
        expect(parse(R"({"a" 2})").error().message == "expected ':'");
        expect(parse("[").error().message == "unexpected end of input");
        expect(parse("{").error().message == "expected a string key");
    };

    "limit_every_read_target_names_its_expected_type_when_the_json_has_another"_test = [] {
        struct wrong {
            bool flag = false;
            std::string text{};
            std::int32_t count = 0;
            unsigned small = 0;
            double real = 0;
            level mode = level::quiet;
            milliseconds span{};
            std::vector<int> list{};
            std::map<std::string, int> table{};
            std::optional<int> maybe{};
        };
        auto const message_for = [](std::string_view const text) { return read_text<wrong>(text).error().message; };

        expect(message_for(R"({"flag":1})") == "expected true or false");
        expect(message_for(R"({"text":1})") == "expected a string");
        expect(message_for(R"({"count":"x"})") == "expected a whole number");
        expect(message_for(R"({"small":-1})") == "number is out of range");
        expect(message_for(R"({"real":"x"})") == "expected a number");
        expect(message_for(R"({"mode":1})") == "not one of the allowed names");
        expect(message_for(R"({"span":1})") == "expected a duration string");
        expect(message_for(R"({"list":1})") == "expected an array");
        expect(message_for(R"({"table":1})") == "expected an object");
        expect(message_for(R"({"maybe":"x"})") == "expected a whole number");
        expect(message_for(R"({"list":[1,"x"]})") == "expected a whole number");
        expect(message_for(R"({"table":{"k":"x"}})") == "expected a whole number");
        expect(message_for("[]") == "expected an object");
        expect(read_text<wrong>(R"({"maybe":null,"real":2,"list":[1],"table":{"k":2},"small":3})").has_value());
    };

    "capability_written_numbers_and_flags_round_trip"_test = [] {
        auto const parsed = parse("[1.5,false,true,0]");

        expect(write(*parsed) == "[1.5,false,true,0]");
    };
};

}  // namespace

int main() {
    return boost::ut::cfg<>.run() ? 1 : 0;
}
