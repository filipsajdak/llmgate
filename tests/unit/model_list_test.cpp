#include <boost/ut.hpp>

import llmgate.model_list;
import std;

namespace {

using namespace boost::ut;
using namespace llmgate;

constexpr std::string_view copilot_list = R"({"object":"list","data":[
{"id":"claude-sonnet-9","capabilities":{"family":"claude-sonnet-9","tokenizer":"o200k_base","type":"chat","limits":{"max_context_window_tokens":300000,"max_prompt_tokens":200000,"max_output_tokens":100000}},"policy":{"state":"disabled","terms":"x"},"supported_endpoints":["/v1/messages","/chat/completions"],"extra":1},
{"id":"claude-haiku-9","capabilities":{"family":"claude-haiku-9","limits":{"max_context_window_tokens":200000,"max_prompt_tokens":100000,"max_output_tokens":50000}},"policy":{"state":"enabled"}},
{"id":"embedder","capabilities":{"family":"embedder"}}]})";

constexpr std::string_view plan_list = R"({"models":[
{"slug":"gpt-x","display_name":"GPT-X","context_window":272000,"max_context_window":400000,"visibility":"list"},
{"slug":"gpt-hidden","context_window":1000,"visibility":"hide"}]})";

constexpr std::string_view anthropic_list = R"({"data":[{"id":"claude-a","type":"model","display_name":"A","max_input_tokens":200000,"max_tokens":64000}],"has_more":false})";

const suite model_list_suite = [] {
    "capability_a_copilot_list_gives_the_limits_the_policy_and_the_tokenizer"_test = [] {
        auto const found = model_list::parse(copilot_list);

        expect(found.has_value() && found->size() == 3_u);
        auto const& sonnet = found->at(0);
        expect(sonnet.id == "claude-sonnet-9" && sonnet.family == "claude-sonnet-9" && sonnet.tokenizer == "o200k_base");
        expect(sonnet.context == 300000_u && sonnet.prompt == 200000_u && sonnet.output == 100000_u);
        expect(sonnet.policy == model_list::access::disabled && found->at(1).policy == model_list::access::enabled && found->at(2).policy == model_list::access::unknown);
        expect(!sonnet.hidden && sonnet.endpoints.size() == 2_u && sonnet.endpoints.at(0) == "/v1/messages");
    };

    "capability_a_plan_route_list_uses_slug_context_window_and_visibility"_test = [] {
        auto const found = model_list::parse(plan_list);

        expect(found.has_value() && found->size() == 2_u);
        expect(found->at(0).id == "gpt-x" && found->at(0).context == 272000_u && found->at(0).prompt == 0_u && !found->at(0).hidden);
        expect(found->at(1).id == "gpt-hidden" && found->at(1).hidden);
    };

    "capability_an_anthropic_list_gives_input_and_output_limits"_test = [] {
        auto const found = model_list::parse(anthropic_list);

        expect(found.has_value() && found->size() == 1_u);
        expect(found->at(0).id == "claude-a" && found->at(0).prompt == 200000_u && found->at(0).output == 64000_u && found->at(0).context == 0_u);
    };

    "limit_a_body_that_is_not_a_model_list_is_named_not_guessed"_test = [] {
        expect(!model_list::parse("<html>").has_value());
        expect(!model_list::parse(R"({"error":"nope"})").has_value());
        expect(model_list::parse(R"({"data":[]})").value().empty());
        expect(model_list::parse(R"({"data":[{"no_id":1},{"id":"x"}]})").value().size() == 1_u);
    };

    "capability_a_model_is_usable_unless_its_policy_disables_it"_test = [] {
        auto const found = model_list::parse(copilot_list);

        expect(!model_list::usable(found->at(0)) && model_list::usable(found->at(1)) && model_list::usable(found->at(2)));
    };

    "limit_provider_text_never_puts_control_bytes_on_the_terminal"_test = [] {
        auto const found = model_list::parse(R"({"data":[{"id":"evil\u001b[31m-model\n"}]})");

        auto const table = model_list::render(*found);

        expect(!table.contains('\x1b') && table.contains("evil?[31m-model?"));
        expect(model_list::printable("a\x07" "b", 8) == "a?b" && model_list::printable("abcdef", 3) == "abc");
    };

    "capability_the_table_lists_each_model_with_its_limits_and_marks_a_disabled_one"_test = [] {
        auto const found = model_list::parse(copilot_list);

        auto const table = model_list::render(*found);

        expect(table.contains("claude-sonnet-9") && table.contains("300000") && table.contains("200000") && table.contains("100000"));
        expect(table.contains("disabled by the account's policy"));
        expect(!model_list::render({}).contains("disabled"));
    };
};

}  // namespace

int main() {
    return boost::ut::cfg<>.run() ? 1 : 0;
}
