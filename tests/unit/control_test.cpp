#include <boost/ut.hpp>

import llmgate.control;
import std;

namespace {

using namespace boost::ut;
using namespace llmgate::control;

const std::vector<offer> offers{{.provider = "copilot", .models = {"claude-sonnet-5.5", "claude-opus-5.5"}},
                                {.provider = "anthropic", .models = {"claude-sonnet-5-5"}},
                                {.provider = "thor", .models = {"mistral-small-3.1"}}};
const std::vector<std::string> config_order{"copilot", "anthropic"};

std::vector<target> targets() { return catalogue(offers); }

plan resolved(std::string_view const requested, std::string_view const role, std::string_view const session, overrides const& pins = {}) {
    return resolve({.requested = requested, .role = role, .session = session}, targets(), pins, config_order);
}

const suite control_suite = [] {
    "capability_every_target_id_contains_claude_so_the_model_picker_keeps_it"_test = [] {
        for (auto const& item : targets()) {
            expect(item.id.contains("claude")) << item.id;
        }
    };

    "capability_a_claude_model_keeps_its_name_and_a_foreign_model_gets_the_prefix"_test = [] {
        expect(id_of("claude-sonnet-5.5", "copilot") == "claude-sonnet-5.5@copilot");
        expect(id_of("mistral-small-3.1", "thor") == "claude-mistral-small-3.1@thor");
        expect(id_of("Anthropic-Thing", "x") == "Anthropic-Thing@x");
    };

    "capability_the_catalogue_lists_each_offered_model_once_with_provider_and_display_name"_test = [] {
        auto const list = targets();

        expect(list.size() == 4_u);
        expect(list.front().provider == "copilot" && list.front().upstream == "claude-sonnet-5.5");
        expect(list.back().display_name == "mistral-small-3.1 (thor)");
    };

    "limit_a_model_offered_twice_by_one_provider_is_listed_once"_test = [] {
        std::vector<offer> const twice{{.provider = "p", .models = {"m", "m"}}};

        expect(catalogue(twice).size() == 1_u);
    };

    "capability_without_pins_or_a_pick_the_configured_order_decides"_test = [] {
        auto const result = resolved("claude-sonnet-5-5", "sonnet", "s1");

        expect(result.source == via::config);
        expect(result.steps.size() == 2_u && result.steps.front().provider == "copilot" && !result.steps.front().upstream);
    };

    "capability_a_picked_target_id_is_exactly_one_step_with_its_upstream_model"_test = [] {
        auto const result = resolved("claude-mistral-small-3.1@thor", "other", "s1");

        expect(result.source == via::picked);
        expect(result.steps.size() == 1_u);
        expect(result.steps.front().provider == "thor" && result.steps.front().upstream == "mistral-small-3.1");
    };

    "capability_a_global_remap_sends_the_role_to_the_pinned_provider_and_model"_test = [] {
        overrides pins;
        pins.global["sonnet"] = {.provider = "anthropic", .upstream = "claude-sonnet-5-5"};

        auto const result = resolved("claude-sonnet-5-5", "sonnet", "s1", pins);

        expect(result.source == via::global_remap && result.steps.size() == 1_u);
        expect(result.steps.front().provider == "anthropic" && result.steps.front().upstream == "claude-sonnet-5-5");
        expect(resolved("claude-opus-5-5", "opus", "s1", pins).source == via::config);
    };

    "capability_a_session_remap_beats_the_global_remap_for_that_session_only"_test = [] {
        overrides pins;
        pins.global["sonnet"] = {.provider = "anthropic", .upstream = "claude-sonnet-5-5"};
        pins.session["s1"]["sonnet"] = {.provider = "copilot", .upstream = "claude-sonnet-5.5"};

        auto const mine = resolved("claude-sonnet-5-5", "sonnet", "s1", pins);
        auto const other = resolved("claude-sonnet-5-5", "sonnet", "s2", pins);

        expect(mine.source == via::session_remap && mine.steps.front().provider == "copilot");
        expect(other.source == via::global_remap && other.steps.front().provider == "anthropic");
    };

    "capability_an_explicit_pick_beats_every_remap"_test = [] {
        overrides pins;
        pins.global["sonnet"] = {.provider = "anthropic", .upstream = "claude-sonnet-5-5"};
        pins.session["s1"]["sonnet"] = {.provider = "anthropic", .upstream = "claude-sonnet-5-5"};

        auto const result = resolved("claude-sonnet-5.5@copilot", "sonnet", "s1", pins);

        expect(result.source == via::picked && result.steps.front().provider == "copilot");
    };

    "limit_a_session_without_an_id_never_matches_a_session_remap"_test = [] {
        overrides pins;
        pins.session[""]["sonnet"] = {.provider = "copilot", .upstream = "claude-sonnet-5.5"};

        expect(resolved("claude-sonnet-5-5", "sonnet", "", pins).source == via::config);
    };

    "limit_a_pin_must_name_a_listed_target"_test = [] {
        expect(pin_problem({.provider = "copilot", .upstream = "claude-sonnet-5.5"}, targets()).empty());
        expect(pin_problem({.provider = "copilot", .upstream = "nope"}, targets()).contains("nope"));
        expect(pin_problem({.provider = "ghost", .upstream = "claude-sonnet-5.5"}, targets()).contains("ghost"));
    };
};

}  // namespace

int main() {
    return boost::ut::cfg<>.run() ? 1 : 0;
}
