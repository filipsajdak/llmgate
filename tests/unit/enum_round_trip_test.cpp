#include <boost/ut.hpp>

import llmgate.anthropic;
import llmgate.arguments;
import llmgate.copilot;
import llmgate.doctor;
import llmgate.http;
import llmgate.jsonscan;
import llmgate.reflect;
import llmgate.role;
import llmgate.selection;
import llmgate.tls;
import llmgate.usage;
import std;

namespace {

using namespace boost::ut;
using namespace llmgate;

template <class E>
    requires std::is_enum_v<E>
constexpr bool names_round_trip() {
    return reflect::unique_values<E>() && std::ranges::all_of(reflect::enum_values<E>, [](E const value) {
               return !reflect::enum_name(value).empty() && reflect::enum_from_name<E>(reflect::enum_name(value)) == value;
           });
}

template <class... Es>
constexpr bool all_round_trip() {
    return (names_round_trip<Es>() && ...);
}

const suite enum_round_trips = [] {
    "capability_every_enumerator_of_every_named_enum_round_trips_through_its_name"_test = [] {
        static_assert(all_round_trip<arguments::command, anthropic::repair_kind, anthropic::event_type, usage::level, selection::circuit, roles::role>());
        static_assert(all_round_trip<doctor::status, doctor::check, doctor::port_state, doctor::settings_state, doctor::ledger_state, doctor::exchange_state, doctor::quota_state>());
        static_assert(all_round_trip<http::errc, tls::errc, jsonscan::errc, copilot::errc>());
        expect(true);
    };

    "capability_the_role_enum_drives_matching_and_the_validation_message"_test = [] {
        expect(roles::list_text() == "opus, sonnet, haiku or other");
        expect(roles::of("claude-OPUS-5-5") == roles::role::opus && roles::of("gpt-4o") == roles::role::other);
        expect(selection::route_problem(std::vector<selection::role_route>{{.role = "gpt", .providers = {"a"}}}, std::vector<std::string>{"a"}) ==
               "routes.gpt is not a role (opus, sonnet, haiku or other)");
    };
};

}  // namespace

int main() {
    return boost::ut::cfg<>.run() ? 1 : 0;
}
