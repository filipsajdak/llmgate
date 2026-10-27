#include <boost/ut.hpp>

import llmgate.config_edit;
import llmgate.gateway;
import llmgate.jsonedit;
import llmgate.wizard;
import std;

namespace {

using namespace boost::ut;
namespace wz = llmgate::wizard;
namespace ce = llmgate::config_edit;

std::string applied(std::vector<ce::change> const& edits) {
    std::string text{wz::minimal_config};
    for (auto const& step : edits) {
        auto const done = ce::apply(text, step);
        if (!done) {
            return done.error();
        }
        text = done->text;
    }
    return text;
}

std::string with_provider(wz::answers const& given) {
    auto const planned = ce::plan(wz::provider_request(given));
    if (!planned) {
        return planned.error();
    }
    std::vector<ce::change> edits{*planned};
    if (auto const credential = wz::credential_edit(given.which)) {
        edits.push_back(*credential);
    }
    auto text = applied(edits);
    std::erase_if(text, [](char const c) { return c == ' ' || c == '\n'; });
    return text;
}

suite<"wizard"> tests = [] {
    "capability_the_menu_lists_every_kind_with_its_summary_from_the_enum"_test = [] {
        auto const text = wz::menu();

        expect(text.contains("1) claude") && text.contains("2) copilot") && text.contains("3) chatgpt") && text.contains("4) gateway") && text.contains("5) local")) << text;
        expect(text.contains("GitHub Copilot seat") && text.contains("ChatGPT plan"));
        expect(std::ranges::count(text, '\n') == static_cast<std::ptrdiff_t>(wz::kind_count));
        expect(!wz::summary_of(wz::kind::local).empty());
    };

    "capability_a_choice_takes_numbers_separated_by_commas_or_spaces_without_repeats"_test = [] {
        auto const chosen = wz::choose("2, 3 2,1");

        expect(chosen.has_value() && chosen->size() == 3_u);
        expect(chosen->at(0) == wz::kind::copilot && chosen->at(1) == wz::kind::chatgpt && chosen->at(2) == wz::kind::claude);
    };

    "limit_an_empty_out_of_range_or_non_numeric_choice_is_refused_by_name"_test = [] {
        expect(!wz::choose("").has_value());
        expect(!wz::choose("0").has_value() && !wz::choose("6").has_value());
        auto const word = wz::choose("1,two");
        expect(!word.has_value() && word.error().contains("'two'"));
        expect(!wz::choose("1x").has_value());
    };

    "capability_a_yes_or_no_answer_is_read_case_blind_and_a_blank_takes_the_default"_test = [] {
        expect(wz::yes_no("Y", false) == std::optional{true} && wz::yes_no("no", true) == std::optional{false});
        expect(wz::yes_no("", true) == std::optional{true} && wz::yes_no("", false) == std::optional{false});
        expect(!wz::yes_no("maybe", true).has_value());
    };

    "capability_each_kind_becomes_the_provider_edit_the_command_line_would_make"_test = [] {
        auto const claude = with_provider({.which = wz::kind::claude, .name = "claude"});
        auto const copilot = with_provider({.which = wz::kind::copilot, .name = "work", .host = "example.ghe.com"});
        auto const chatgpt = with_provider({.which = wz::kind::chatgpt, .name = "chatgpt"});
        auto const gateway = with_provider({.which = wz::kind::gateway, .name = "partner", .base_url = "https://gw.example/anthropic", .auth = "header:api-key", .secret = "file:~/.config/llmgate/secrets/partner.key"});
        auto const local = with_provider({.which = wz::kind::local, .name = "local", .base_url = "http://127.0.0.1:8000", .auth = "none"});

        expect(claude.contains("\"preset\":\"anthropic\"") && claude.contains("forward_credentials") && claude.contains("\"credential\":\"login\"")) << claude;
        expect(copilot.contains("\"preset\":\"copilot\"") && copilot.contains("https://example.ghe.com") && copilot.contains("https://api.example.ghe.com")) << copilot;
        expect(chatgpt.contains("\"preset\":\"chatgpt\"") && !chatgpt.contains("credential")) << chatgpt;
        expect(gateway.contains("https://gw.example/anthropic") && gateway.contains("api-key") && gateway.contains("partner.key")) << gateway;
        expect(local.contains("http://127.0.0.1:8000") && local.contains("\"kind\":\"none\"")) << local;
    };

    "capability_the_preset_of_a_kind_comes_from_its_annotation"_test = [] {
        expect(bool{wz::preset_of(wz::kind::claude) == std::optional{llmgate::gateway::preset_kind::anthropic}});
        expect(bool{wz::preset_of(wz::kind::copilot) == std::optional{llmgate::gateway::preset_kind::copilot}});
        expect(bool{wz::preset_of(wz::kind::chatgpt) == std::optional{llmgate::gateway::preset_kind::chatgpt}});
        expect(!wz::preset_of(wz::kind::gateway) && !wz::preset_of(wz::kind::local));
    };

    "capability_the_menu_summaries_start_in_one_column"_test = [] {
        auto const text = wz::menu();
        std::optional<std::size_t> column;
        for (auto const line : std::views::split(std::string_view{text}, '\n') | std::views::filter([](auto part) { return !std::ranges::empty(part); })) {
            auto const row = std::string_view{line.begin(), line.end()};
            auto const summary = std::min({row.find("Your Claude"), row.find("A GitHub"), row.find("Your ChatGPT"), row.find("Another gateway"), row.find("A model served")});
            column = column.value_or(summary);
            expect(summary == *column) << row;
        }
    };

    "capability_the_questions_follow_the_kind"_test = [] {
        expect(wz::signs_in(wz::kind::copilot) && wz::signs_in(wz::kind::chatgpt) && !wz::signs_in(wz::kind::claude) && !wz::signs_in(wz::kind::local));
        expect(!wz::can_list_models(wz::kind::claude) && wz::can_list_models(wz::kind::gateway));
        expect(wz::asks_for_url(wz::kind::gateway) && wz::asks_for_url(wz::kind::local) && !wz::asks_for_url(wz::kind::copilot));
        expect(wz::takes_a_key({.which = wz::kind::gateway, .auth = "bearer"}) && !wz::takes_a_key({.which = wz::kind::gateway, .auth = "none"}) && !wz::takes_a_key({.which = wz::kind::local}));
        expect(wz::default_name(wz::kind::gateway) == "gateway");
    };

    "limit_a_provider_with_a_name_the_config_cannot_hold_is_refused"_test = [] {
        auto const planned = ce::plan(wz::provider_request({.which = wz::kind::copilot, .name = "has space"}));

        expect(!planned.has_value());
    };
};

}  // namespace

int main() {}
