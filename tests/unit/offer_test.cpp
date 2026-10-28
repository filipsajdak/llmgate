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
            listed("old-chat", {"/chat/completions"}), listed("embedder", {"/embeddings"}), {.id = "bare"}};
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
        expect(of::wire_of(listed("m", {"/chat/completions"}), gw::wire_protocol::anthropic) == of::wire::chat);
        expect(of::wire_of(listed("m", {"/embeddings"}), gw::wire_protocol::anthropic) == of::wire::unsupported);
        expect(of::wire_of(listed("m", {}), gw::wire_protocol::openai_chat) == of::wire::chat);
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
        expect(!of::recommended(all.at(2), gw::wire_protocol::anthropic) && of::recommended(all.at(3), gw::wire_protocol::anthropic) && !of::recommended(all.at(4), gw::wire_protocol::anthropic));
        auto hidden = listed("h", {"/responses"});
        hidden.hidden = true;
        expect(!of::recommended(hidden, gw::wire_protocol::anthropic));
    };

    "capability_the_candidates_list_every_model_with_its_wire_policy_and_whether_it_is_already_offered"_test = [] {
        auto provider = copilot_seat();
        provider.offer = {"gpt-6-luna"};
        provider.models = {{"claude-sonnet-5-5", "claude-sonnet-5.5"}};

        auto const rows = of::candidates(provider, seat());

        expect(rows.size() == 6_u);
        expect(rows.at(0).id == "claude-sonnet-5.5" && rows.at(0).protocol == "anthropic" && rows.at(0).recommended && rows.at(0).offered);
        expect(rows.at(1).id == "gpt-6-luna" && rows.at(1).protocol == "openai_responses" && rows.at(1).recommended && rows.at(1).offered);
        expect(rows.at(2).policy == "disabled" && !rows.at(2).recommended && !rows.at(2).offered);
        expect(rows.at(3).protocol == "openai_chat" && rows.at(3).recommended && rows.at(4).protocol == "unsupported" && !rows.at(4).recommended);
        expect(rows.at(5).policy == "unknown" && rows.at(5).context == 0_u && rows.at(5).recommended);
        auto const text = of::candidates_json(rows);
        expect(text.starts_with("[{") && text.contains(R"("id":"gpt-6-luna")") && text.contains(R"("recommended":true)") && text.contains(R"("context":400000)")) << text;
    };

    "limit_a_model_id_that_could_be_read_as_an_option_or_holds_odd_characters_is_never_listed_or_offered"_test = [] {
        auto bad = seat();
        bad.push_back({.id = "--config=/tmp/x", .context = window});
        bad.push_back({.id = "has space"});
        bad.push_back({.id = "esc\x1b[31m"});

        auto const rows = of::candidates(copilot_seat(), bad);
        auto const planned = of::plan("work", copilot_seat(), bad, {"--config=/tmp/x"});

        expect(rows.size() == 6_u);
        expect(std::ranges::none_of(rows, [](of::candidate const& row) { return row.id.starts_with('-') || row.id.contains(' ') || row.id.contains('\x1b'); }));
        expect(!planned.has_value() && planned.error().contains("not a plain model name"));
        expect(of::valid_id("claude-sonnet-5.5") && of::valid_id("gpt-6-luna@v2:free/x") && !of::valid_id("-x") && !of::valid_id("") && !of::valid_id("a b"));
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
        auto const chat_only = of::plan("work", copilot_seat(), seat(), {"embedder"});
        auto const unlisted = of::plan("work", copilot_seat(), seat(), {"nope"});

        expect(!disabled.has_value() && disabled.error().contains("gpt-5.6-sol") && disabled.error().contains("policy"));
        expect(!chat_only.has_value() && chat_only.error().contains("embedder") && chat_only.error().contains("/embeddings"));
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
