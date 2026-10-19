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

    "capability_a_use_command_pins_the_role_globally_or_for_one_session"_test = [] {
        overrides pins;

        expect(apply(pins, R"({"action":"use","role":"sonnet","provider":"anthropic","upstream":"claude-sonnet-5-5"})", targets()).empty());
        expect(apply(pins, R"({"action":"use","role":"opus","provider":"copilot","upstream":"claude-opus-5.5","session":"s9"})", targets()).empty());

        expect(pins.global.at("sonnet").provider == "anthropic");
        expect(pins.session.at("s9").at("opus").upstream == "claude-opus-5.5" && !pins.global.contains("opus"));
    };

    "capability_a_use_command_may_name_a_catalogue_target_id_instead_of_provider_and_model"_test = [] {
        overrides pins;

        expect(apply(pins, R"({"action":"use","role":"opus","target":"claude-mistral-small-3.1@thor"})", targets()).empty());
        expect(pins.global.at("opus") == pin{.provider = "thor", .upstream = "mistral-small-3.1"});
        expect(apply(pins, R"({"action":"use","role":"opus","target":"claude-nope@thor"})", targets()).contains("claude-nope@thor"));
    };

    "capability_a_clear_command_removes_one_pin_or_all_of_a_scope"_test = [] {
        overrides pins;
        pins.global["sonnet"] = {.provider = "anthropic", .upstream = "claude-sonnet-5-5"};
        pins.global["opus"] = {.provider = "copilot", .upstream = "claude-opus-5.5"};

        expect(apply(pins, R"({"action":"clear","role":"sonnet"})", targets()).empty());
        expect(!pins.global.contains("sonnet") && pins.global.contains("opus"));
        expect(apply(pins, R"({"action":"clear"})", targets()).empty());
        expect(pins.global.empty());
    };

    "limit_a_command_naming_an_unknown_role_target_or_action_changes_nothing"_test = [] {
        overrides pins;

        expect(apply(pins, R"({"action":"use","role":"gpt","provider":"copilot","upstream":"claude-opus-5.5"})", targets()).contains("gpt"));
        expect(apply(pins, R"({"action":"use","role":"opus","provider":"ghost","upstream":"x"})", targets()).contains("ghost"));
        expect(apply(pins, R"({"action":"dance"})", targets()).contains("dance"));
        expect(apply(pins, "not json", targets()).contains("JSON"));
        expect(pins.global.empty() && pins.session.empty());
    };

    "limit_the_session_pins_stay_within_their_bound_dropping_the_oldest"_test = [] {
        overrides pins;
        for (std::size_t n = 0; n < max_sessions + 3; ++n) {
            expect(apply(pins, std::format(R"({{"action":"use","role":"opus","provider":"copilot","upstream":"claude-opus-5.5","session":"s{}"}})", n), targets()).empty());
        }

        expect(pins.session.size() == max_sessions);
        expect(!pins.session.contains("s0") && pins.session.contains(std::format("s{}", max_sessions + 2)));
    };

    "capability_global_pins_survive_a_save_and_load_and_sessions_are_not_saved"_test = [] {
        overrides pins;
        pins.global["sonnet"] = {.provider = "anthropic", .upstream = "claude-sonnet-5-5"};
        pins.global["opus"] = {.provider = "copilot", .upstream = "claude-opus-5.5"};
        pins.session["s1"]["opus"] = {.provider = "thor", .upstream = "mistral-small-3.1"};

        auto const loaded = load_pins(save_pins(pins));

        expect(loaded.has_value() && loaded->global == pins.global && loaded->session.empty());
    };

    "limit_a_damaged_overrides_file_is_reported_not_trusted"_test = [] {
        expect(!load_pins("{").has_value());
        expect(!load_pins(R"({"pins":[{"role":"opus","provider":3}]})").has_value());
        expect(load_pins("").has_value() && load_pins("")->global.empty());
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
