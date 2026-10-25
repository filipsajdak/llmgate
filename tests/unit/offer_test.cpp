#include <boost/ut.hpp>

import llmgate.config_edit;
import llmgate.discovery;
import llmgate.gateway;
import llmgate.jsonedit;
import llmgate.model_list;
import llmgate.offer;
import std;

namespace {

using namespace boost::ut;
namespace gw = llmgate::gateway;
namespace ml = llmgate::model_list;
namespace of = llmgate::offer;

constexpr std::uint64_t window = 400000;
constexpr std::uint64_t prompt_limit = 272000;
constexpr std::uint64_t reply_limit = 128000;

ml::entry listed(std::string id, std::vector<std::string> endpoints, ml::access const policy = ml::access::enabled) {
    return {.id = std::move(id), .context = window, .prompt = prompt_limit, .output = reply_limit, .policy = policy, .endpoints = std::move(endpoints)};
}

std::vector<ml::entry> seat() {
    return {listed("claude-sonnet-5.5", {"/chat/completions", "/v1/messages"}), listed("gpt-6-luna", {"/responses", "ws:/responses"}), listed("gpt-5.6-sol", {"/responses"}, ml::access::disabled),
            listed("old-chat", {"/chat/completions"}), {.id = "bare"}};
}

gw::provider_config copilot_seat() {
    return {.base_url = "https://api.githubcopilot.com", .preset = gw::preset_kind::copilot};
}

std::string applied(std::vector<llmgate::config_edit::change> const& edits) {
    std::string text = R"({ "listen": "127.0.0.1:1", "providers": { "work": { "base_url": "https://api.githubcopilot.com", "preset": "copilot" } } })";
    for (auto const& step : edits) {
        auto const done = llmgate::config_edit::apply(text, step);
        if (!done) {
            return done.error();
        }
        text = done->text;
    }
    return text;
}

suite<"offer"> tests = [] {
    "capability_a_model_that_answers_only_on_responses_gets_that_protocol_and_one_that_answers_on_messages_keeps_the_providers"_test = [] {
        expect(of::wire_of(listed("m", {"/responses"}), gw::wire_protocol::anthropic) == of::wire::responses);
        expect(of::wire_of(listed("m", {"/chat/completions", "/v1/messages"}), gw::wire_protocol::openai_responses) == of::wire::messages);
        expect(of::wire_of(listed("m", {"/chat/completions", "/responses"}), gw::wire_protocol::anthropic) == of::wire::responses);
        expect(of::wire_of(listed("m", {"/chat/completions"}), gw::wire_protocol::anthropic) == of::wire::unsupported);
        expect(of::wire_of(listed("m", {}), gw::wire_protocol::openai_responses) == of::wire::responses);
        expect(of::wire_of(listed("m", {}), gw::wire_protocol::anthropic) == of::wire::messages);
    };

    "capability_the_model_list_is_read_from_the_path_each_kind_of_provider_serves_it_on"_test = [] {
        expect(llmgate::discovery::models_target_of(copilot_seat()) == "/models");
        expect(llmgate::discovery::models_target_of({.base_url = "https://api.anthropic.com"}) == "/v1/models");
        expect(llmgate::discovery::models_target_of({.base_url = "http://127.0.0.1:9080/anthropic", .messages_path = "/v1/messages"}) == "/v1/models");
        expect(llmgate::discovery::models_target_of({.base_url = "https://api.openai.com/v1", .protocol = gw::wire_protocol::openai_responses}) == "/models");
    };

    "capability_the_recommendation_skips_disabled_hidden_and_untranslatable_models"_test = [] {
        auto const all = seat();

        expect(of::recommended(all.at(0), gw::wire_protocol::anthropic) && of::recommended(all.at(1), gw::wire_protocol::anthropic));
        expect(!of::recommended(all.at(2), gw::wire_protocol::anthropic) && !of::recommended(all.at(3), gw::wire_protocol::anthropic));
        auto hidden = listed("h", {"/responses"});
        hidden.hidden = true;
        expect(!of::recommended(hidden, gw::wire_protocol::anthropic));
    };

    "capability_offering_a_claude_and_a_gpt_model_writes_the_offer_the_limits_and_only_the_differing_protocol"_test = [] {
        auto const planned = of::plan("work", copilot_seat(), seat(), {"claude-sonnet-5.5", "gpt-6-luna"});

        expect(planned.has_value());
        auto const text = applied(*planned);
        expect(text.contains(R"("offer": ["claude-sonnet-5.5","gpt-6-luna"])")) << text;
        expect(text.contains("context_tokens") && text.contains("400000") && text.contains("272000") && text.contains("128000")) << text;
        expect(text.contains("model_protocols") && text.contains("gpt-6-luna") && text.contains("openai_responses")) << text;
        expect(!text.contains(R"("claude-sonnet-5.5": "anthropic")") && !text.contains(R"("claude-sonnet-5.5":"anthropic")")) << text;
    };

    "limit_a_disabled_a_chat_only_or_an_unlisted_model_is_refused_by_name_and_nothing_is_planned"_test = [] {
        auto const disabled = of::plan("work", copilot_seat(), seat(), {"gpt-5.6-sol"});
        auto const chat_only = of::plan("work", copilot_seat(), seat(), {"old-chat"});
        auto const unlisted = of::plan("work", copilot_seat(), seat(), {"nope"});

        expect(!disabled.has_value() && disabled.error().contains("gpt-5.6-sol") && disabled.error().contains("policy"));
        expect(!chat_only.has_value() && chat_only.error().contains("old-chat") && chat_only.error().contains("/chat/completions"));
        expect(!unlisted.has_value() && unlisted.error().contains("nope"));
    };

    "capability_a_model_with_no_reported_limits_gets_no_limits_entry"_test = [] {
        auto const planned = of::plan("work", copilot_seat(), seat(), {"bare"});

        expect(planned.has_value());
        expect(!applied(*planned).contains("context_tokens"));
    };
};

}  // namespace

int main() {}
