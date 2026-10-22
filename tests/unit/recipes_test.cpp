#include <boost/ut.hpp>

import llmgate.config_edit;
import llmgate.gateway;
import llmgate.json;
import llmgate.recipes;
import std;

namespace {

using namespace boost::ut;
namespace gw = llmgate::gateway;
namespace rc = llmgate::recipes;

gw::config three_providers() {
    gw::config built;
    built.listen = "127.0.0.1:9483";
    built.providers["anthropic"] = {.base_url = "https://api.anthropic.com", .preset = gw::preset_kind::anthropic, .forward_credentials = true};
    built.providers["codex"] = {.base_url = "https://api.openai.com/v1", .preset = gw::preset_kind::chatgpt, .protocol = gw::wire_protocol::openai_responses,
                                .models = {{"claude-opus-5-5", "gpt-6.1-sol"}, {"claude-sonnet-5-5", "gpt-6-sol"}, {"claude-haiku-5-5", "gpt-6-luna"}}};
    built.providers["work"] = {.base_url = "https://api.githubcopilot.com", .preset = gw::preset_kind::copilot};
    return built;
}

std::vector<std::string> route_of(std::vector<rc::edit> const& edits, std::string_view const key) {
    for (auto const& item : edits) {
        if (item.path.size() == 2 && item.path.front() == "routes" && item.path.back() == key) {
            return llmgate::json::read_text<std::vector<std::string>>(*item.value).value_or(std::vector<std::string>{});
        }
    }
    return {};
}

const suite recipes_suite = [] {
    "capability_a_provider_is_classified_by_its_preset_or_login"_test = [] {
        auto const built = three_providers();

        expect(rc::kind_of(built.providers.at("anthropic")) == rc::provider_kind::claude);
        expect(rc::kind_of(built.providers.at("codex")) == rc::provider_kind::chatgpt);
        expect(rc::kind_of(built.providers.at("work")) == rc::provider_kind::copilot);
        expect(rc::kind_of(gw::provider_config{.base_url = "http://x"}) == rc::provider_kind::other);
        expect(rc::kind_of(gw::provider_config{.chatgpt = gw::chatgpt_config{}}) == rc::provider_kind::chatgpt);
    };

    "capability_quality_first_routes_every_role_to_claude_then_copilot_then_chatgpt"_test = [] {
        auto const planned = rc::plan(rc::find("quality-first").value(), three_providers());

        expect(planned.missing.empty());
        for (auto const* role : {"opus", "sonnet", "haiku"}) {
            expect((route_of(planned.edits, role) == std::vector<std::string>{"anthropic", "work", "codex"})) << role;
        }
    };

    "capability_spend_evenly_spreads_roles_and_sends_background_work_to_the_cheap_model_of_chatgpt"_test = [] {
        auto const planned = rc::plan(rc::find("spend-evenly").value(), three_providers());

        expect((route_of(planned.edits, "opus") == std::vector<std::string>{"anthropic", "work", "codex"}));
        expect((route_of(planned.edits, "sonnet") == std::vector<std::string>{"work", "codex", "anthropic"}));
        expect((route_of(planned.edits, "haiku") == std::vector<std::string>{"codex", "work", "anthropic"}));
        auto const compaction = std::ranges::find_if(planned.edits, [](rc::edit const& item) { return item.path.size() == 2 && item.path.front() == "routes_by_class" && item.path.back() == "compaction"; });
        expect(compaction != planned.edits.end() && compaction->value.value() == R"(["claude-gpt-6-luna@codex"])");
    };

    "capability_second_opinion_installs_a_reviewer_agent_on_the_first_other_vendor_target"_test = [] {
        auto const planned = rc::plan(rc::find("second-opinion").value(), three_providers());

        auto const reviewer = std::ranges::find_if(planned.edits, [](rc::edit const& item) { return item.path.size() == 2 && item.path.front() == "agents" && item.path.back() == "reviewer"; });
        expect(planned.missing.empty() && reviewer != planned.edits.end());
        expect(reviewer->value->contains(R"("target":"claude-gpt-6-sol@codex")") && reviewer->value->contains("Review")) << reviewer->value.value_or("");
    };

    "limit_second_opinion_needs_a_non_claude_provider_and_says_which_kinds_would_do"_test = [] {
        gw::config only_claude;
        only_claude.providers["anthropic"] = {.base_url = "https://api.anthropic.com", .preset = gw::preset_kind::anthropic, .forward_credentials = true};

        auto const planned = rc::plan(rc::find("second-opinion").value(), only_claude);

        expect(planned.missing == std::vector<std::string>{"chatgpt or copilot"} && planned.edits.empty());
    };

    "limit_a_recipe_names_the_kinds_it_misses_and_changes_nothing_when_a_required_one_is_absent"_test = [] {
        gw::config only_claude;
        only_claude.providers["anthropic"] = {.base_url = "https://api.anthropic.com", .preset = gw::preset_kind::anthropic, .forward_credentials = true};

        auto const planned = rc::plan(rc::find("spend-evenly").value(), only_claude);

        expect(planned.missing == std::vector<std::string>{"chatgpt", "copilot"});
        expect(planned.edits.empty());
        expect(rc::plan(rc::find("quality-first").value(), only_claude).missing.empty());
    };

    "capability_applying_the_edits_in_order_gives_a_valid_config_with_the_routes"_test = [] {
        auto const built = three_providers();
        auto const planned = rc::plan(rc::find("spend-evenly").value(), built);
        std::string text = R"({ "listen": "127.0.0.1:9483", "providers": {
  "anthropic": { "preset": "anthropic", "forward_credentials": true },
  "codex": { "preset": "chatgpt", "chatgpt": { "token_file": "/t" }, "models": { "claude-haiku-5-5": "gpt-6-luna" } },
  "work": { "preset": "copilot", "copilot": { "token_file": "/c" } }
} })";

        for (auto const& item : planned.edits) {
            auto const done = llmgate::config_edit::apply(text, {.path = item.path, .value = item.value});
            expect(done.has_value()) << (done ? "" : done.error());
            if (done) {
                text = done->text;
            }
        }

        auto const parsed = llmgate::json::read_text<gw::config>(text);
        expect(parsed.has_value() && gw::config_problem(*parsed).empty() && parsed->routes.at("sonnet").front() == "work");
    };

    "capability_the_recipe_list_carries_title_summary_pros_cons_needs_and_the_token_multiplier"_test = [] {
        auto const text = rc::list_json();

        for (auto const* word : {"quality-first", "spend-evenly", "best-per-job", R"("pros")", R"("cons")", R"("needs")", R"("tokens")", R"("title")", R"("summary")"}) {
            expect(text.contains(word)) << word;
        }
        expect(!rc::find("nope").has_value());
    };
};

}  // namespace

int main() {
    return boost::ut::cfg<>.run() ? 1 : 0;
}
