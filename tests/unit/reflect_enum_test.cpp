#include <boost/ut.hpp>

import llmgate.reflect;
import std;

namespace {

using namespace boost::ut;
using namespace llmgate::reflect;

enum class stop_reason { end_turn, max_tokens, tool_use };
enum class coded { first = 1, second };
enum class aliased { a, b, c = b };

const suite enum_names = [] {
    "capability_enum_name_returns_the_enumerator_identifier"_test = [] {
        expect(enum_name(stop_reason::tool_use) == "tool_use");
        expect(enum_name(stop_reason::end_turn) == "end_turn");
    };

    "capability_enum_from_name_round_trips_every_enumerator"_test = [] {
        for (auto const reason : {stop_reason::end_turn, stop_reason::max_tokens, stop_reason::tool_use}) {
            expect(enum_from_name<stop_reason>(enum_name(reason)) == reason);
        }
    };

    "limit_enum_from_name_rejects_unknown_and_empty_names"_test = [] {
        expect(!enum_from_name<stop_reason>("stop_sequence").has_value());
        expect(!enum_from_name<stop_reason>("").has_value());
    };

    "capability_enum_count_and_values_list_every_enumerator_in_declaration_order"_test = [] {
        static_assert(enum_count<stop_reason> == 3);
        expect(std::ranges::equal(enum_values<stop_reason>, std::array{stop_reason::end_turn, stop_reason::max_tokens, stop_reason::tool_use}));
    };

    "defect_a_duplicate_value_is_reported_as_an_alias"_test = [] {
        static_assert(unique_values<stop_reason>() && starts_at_one<coded>() && !starts_at_one<stop_reason>());
        static_assert(!unique_values<aliased>());
        expect(unique_values<stop_reason>() && !unique_values<aliased>());
    };

    "capability_enum_string_copies_the_name_by_value_at_compile_time"_test = [] {
        static_assert(enum_string(stop_reason::tool_use) == std::string_view{"tool_use"});
        expect(enum_string(stop_reason::end_turn) == "end_turn");
    };

    "capability_enum_name_is_usable_at_compile_time"_test = [] {
        static_assert(enum_name(stop_reason::max_tokens) == "max_tokens");
    };
};

}  // namespace

int main() {
    return boost::ut::cfg<>.run() ? 1 : 0;
}
