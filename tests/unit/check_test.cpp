#include <boost/ut.hpp>

import llmgate.check;
import llmgate.gateway;
import llmgate.json;
import std;

namespace {

using namespace boost::ut;
namespace gw = llmgate::gateway;
namespace ck = llmgate::check;

gw::config providers() {
    gw::config built;
    built.providers["anthropic"] = {.base_url = "https://api.anthropic.com", .preset = gw::preset_kind::anthropic, .forward_credentials = true};
    built.providers["codex"] = {.base_url = "https://api.openai.com/v1", .preset = gw::preset_kind::chatgpt,
                                .models = {{"claude-opus-5-5", "gpt-6.1-sol"}, {"claude-haiku-5-5", "gpt-6-luna"}}};
    built.providers["bare"] = {.base_url = "http://127.0.0.1:1"};
    built.providers["partner"] = {.base_url = "http://127.0.0.1:2", .offer = {"gpt-sol"}};
    built.agents["reviewer"] = {.target = "claude-gpt-6-sol@codex", .description = "d", .prompt = "p"};
    return built;
}

const suite check_suite = [] {
    "capability_the_check_target_of_a_provider_is_its_haiku_model_else_its_first_target_else_none"_test = [] {
        auto const built = providers();

        expect(ck::target_of(built, "codex").value() == "claude-gpt-6-luna@codex");
        expect(ck::target_of(built, "partner").value() == "claude-gpt-sol@partner");
        expect(!ck::target_of(built, "bare").has_value() && !ck::target_of(built, "missing").has_value());
    };

    "capability_a_forwarding_provider_is_skipped_because_the_check_has_no_claude_login"_test = [] {
        auto const planned = ck::plan(providers(), "");

        auto const anthropic = std::ranges::find(planned, std::string{"anthropic"}, &ck::job::provider);
        expect(anthropic != planned.end() && anthropic->skipped.contains("forwards your Claude login"));
        auto const bare = std::ranges::find(planned, std::string{"bare"}, &ck::job::provider);
        expect(bare != planned.end() && bare->skipped.contains("no model"));
        auto const codex = std::ranges::find(planned, std::string{"codex"}, &ck::job::provider);
        expect(codex != planned.end() && codex->skipped.empty() && codex->target == "claude-gpt-6-luna@codex");
        expect(ck::plan(providers(), "codex").size() == 1_u);
        expect(ck::plan(providers(), "nope").empty());
    };

    "capability_each_step_sends_a_streaming_request_naming_the_target_and_the_tool_step_forces_a_tool"_test = [] {
        auto const ping = ck::request_body(ck::step::ping, "claude-x@a");
        auto const tool = ck::request_body(ck::step::tool, "claude-x@a");
        auto const long_body = ck::request_body(ck::step::context, "claude-x@a");
        auto const agent = ck::request_body(ck::step::agent, "claude-x@a");

        expect(ping.contains(R"("model":"claude-x@a")") && ping.contains(R"("stream":true)") && !ping.contains("tools"));
        expect(tool.contains(R"("tool_choice":{"type":"tool","name":"echo"})") && tool.contains(R"("input_schema")"));
        expect(long_body.size() > 60000 && long_body.contains("3"));
        expect(agent.contains(R"("model":"claude-x@a")"));
        expect(ck::headers_of(ck::step::agent).contains("x-claude-code-request-class"));
        expect(!ck::headers_of(ck::step::ping).contains("x-claude-code-request-class"));
    };

    "capability_a_reply_is_judged_by_status_and_the_events_the_step_needs"_test = [] {
        constexpr std::string_view text = R"(event: content_block_delta
data: {"type":"content_block_delta","delta":{"type":"text_delta","text":"ok"}}

event: message_stop
data: {"type":"message_stop"}
)";
        constexpr std::string_view tool = R"(data: {"type":"content_block_start","content_block":{"type":"tool_use","name":"echo"}}
data: {"type":"message_stop"})";

        expect(ck::judge(ck::step::ping, 200, text).empty());
        expect(ck::judge(ck::step::ping, 401, "{}").contains("401"));
        expect(ck::judge(ck::step::ping, 200, "garbage").contains("message"));
        expect(ck::judge(ck::step::tool, 200, tool).empty());
        expect(ck::judge(ck::step::tool, 200, text).contains("tool"));
        expect(ck::judge(ck::step::context, 200, text).empty());
        expect(ck::judge(ck::step::ping, 400, R"({"error":{"message":"prompt is too long"}})").contains("prompt is too long"));
    };

    "capability_the_result_table_and_json_name_provider_step_result_and_time"_test = [] {
        std::vector<ck::outcome> const rows{{.provider = "codex", .step = "ping", .detail = "", .milliseconds = 1200}, {.provider = "codex", .step = "tool", .detail = "no tool call", .milliseconds = 800},
                                            {.provider = "anthropic", .step = "all", .skipped = true, .detail = "forwards your Claude login", .milliseconds = 0}};

        auto const table = ck::render(rows);
        auto const json = ck::to_json(rows);

        expect(table.contains("codex") && table.contains("pass") && table.contains("FAIL") && table.contains("skip") && table.contains("1.2 s"));
        expect(json.contains(R"("result":"pass")") && json.contains(R"("result":"fail")") && json.contains(R"("result":"skipped")")) << json;
        expect(ck::all_passed({rows.front()}) && !ck::all_passed(rows));
    };
};

}  // namespace

int main() {
    return boost::ut::cfg<>.run() ? 1 : 0;
}
