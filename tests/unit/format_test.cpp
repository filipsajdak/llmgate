#include <boost/ut.hpp>

import llmgate.format;
import llmgate.json;
import llmgate.ut_print;
import std;

namespace {

using namespace boost::ut;
namespace json = llmgate::json;

enum class tier { free, paid };

struct inner {
    int depth;
    std::string label;
};

struct account {
    std::string name;
    [[= json::redact]] std::string token;
    [[= json::skip]] int internal;
    tier level;
    std::optional<int> limit;
    inner nested;
    std::vector<int> ids;
    std::map<std::string, int> counts;
};

struct own_text {
    int x;
};

struct plain {
    int x;
    bool on;
};

}  // namespace

template <>
struct std::formatter<own_text> {
    constexpr auto parse(std::format_parse_context& context) { return context.begin(); }
    auto format(own_text const& value, std::format_context& context) const { return std::format_to(context.out(), "own#{}", value.x); }
};

namespace {

account const sample{.name = "ann",
                     .token = "s3cret",
                     .internal = 7,
                     .level = tier::paid,
                     .limit = std::nullopt,
                     .nested = {.depth = 2, .label = "x"},
                     .ids = {1, 2},
                     .counts = {{"a", 1}}};

const suite format_suite = [] {
    "capability_aggregate_prints_members_in_declaration_order"_test = [] {
        expect(std::format("{}", plain{.x = 3, .on = true}) == "{x=3, on=true}");
    };

    "capability_redacted_member_prints_a_placeholder_and_skipped_member_is_absent"_test = [] {
        auto const text = std::format("{}", sample);
        expect(text.find("s3cret") == std::string::npos);
        expect(text.find("internal") == std::string::npos);
        expect(text.find("token=<redacted>") != std::string::npos);
    };

    "capability_nested_optional_enum_vector_and_map_members_print_inline"_test = [] {
        expect(std::format("{}", sample) == R"({name="ann", token=<redacted>, level=paid, limit=null, nested={depth=2, label="x"}, ids=[1, 2], counts={"a": 1}})");
    };

    "capability_present_optional_prints_its_value"_test = [] {
        auto filled = sample;
        filled.limit = 5;
        expect(std::format("{}", filled).find("limit=5,") != std::string::npos);
    };

    "limit_enum_value_without_an_enumerator_prints_its_number"_test = [] {
        auto odd = sample;
        odd.level = static_cast<tier>(9);
        expect(std::format("{}", odd).find("level=9,") != std::string::npos);
    };

    "capability_a_type_with_its_own_formatter_is_untouched"_test = [] {
        expect(std::format("{}", own_text{.x = 4}) == "own#4");
    };

    "capability_ut_printer_shows_an_aggregate_through_the_formatter"_test = [] {
        printer out;
        out << plain{.x = 1, .on = false};
        expect(out.str() == "{x=1, on=false}");
    };
};

}  // namespace

int main() {
    return boost::ut::cfg<>.run() ? 1 : 0;
}
