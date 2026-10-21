#include <boost/ut.hpp>

import llmgate.json;
import llmgate.responses_request;
import std;

namespace {

using namespace boost::ut;
using namespace llmgate;

json::value parsed(std::string_view const text) {
    auto const out = json::parse(text, {.jsonc = false});
    return out ? *out : json::value{};
}

std::string field(responses::translated const& done, std::initializer_list<std::string_view> const path) {
    auto const root = parsed(done.body);
    auto const* found = json::at(root, path);
    return found == nullptr ? std::string{"<missing>"} : json::write(*found);
}

responses::translated ok(std::string_view const body, responses::request_options const& options = {}) {
    auto const result = responses::translate_request(body, options);
    expect(result.has_value()) << (result ? "" : result.error().message);
    return result ? *result : responses::translated{};
}

std::string refused(std::string_view const body) {
    auto const result = responses::translate_request(body, {});
    return result ? std::string{"<translated>"} : result.error().message;
}

constexpr std::string_view user_hi = R"({"model":"gpt","stream":true,"messages":[{"role":"user","content":"hi"}]})";

const suite request_suite = [] {
    "capability_a_plain_turn_becomes_one_user_message_with_the_fixed_route_settings"_test = [] {
        auto const done = ok(user_hi);

        expect(field(done, {"model"}) == R"("gpt")");
        expect(field(done, {"input"}) == R"([{"type":"message","role":"user","content":[{"type":"input_text","text":"hi"}]}])");
        expect(field(done, {"store"}) == "false" && field(done, {"stream"}) == "true");
        expect(field(done, {"include"}) == R"(["reasoning.encrypted_content"])");
        expect(done.client_streams);
    };

    "capability_the_system_prompt_as_a_string_or_as_blocks_becomes_instructions"_test = [] {
        auto const text = ok(R"({"model":"m","system":"be brief","messages":[]})");
        auto const blocks = ok(R"({"model":"m","system":[{"type":"text","text":"one","cache_control":{"type":"ephemeral"}},{"type":"text","text":"two"}],"messages":[]})");

        expect(field(text, {"instructions"}) == R"("be brief")");
        expect(field(blocks, {"instructions"}) == R"("one\n\ntwo")");
        expect(field(ok(user_hi), {"instructions"}) == "<missing>");
    };

    "capability_a_tool_loop_keeps_order_call_ids_and_arguments"_test = [] {
        auto const done = ok(R"({"model":"m","messages":[
          {"role":"user","content":"fix it"},
          {"role":"assistant","content":[{"type":"text","text":"looking"},{"type":"tool_use","id":"toolu_1","name":"Read","input":{"path":"a.py"}},{"type":"tool_use","id":"toolu_2","name":"Bash","input":{"command":"ls"}}]},
          {"role":"user","content":[{"type":"tool_result","tool_use_id":"toolu_1","content":"def f(): pass"},{"type":"tool_result","tool_use_id":"toolu_2","content":[{"type":"text","text":"a.py"},{"type":"text","text":"b.py"}]},{"type":"text","text":"go on"}]}]})");

        expect(field(done, {"input"}) ==
               R"([{"type":"message","role":"user","content":[{"type":"input_text","text":"fix it"}]},)"
               R"({"type":"message","role":"assistant","content":[{"type":"output_text","text":"looking"}]},)"
               R"({"type":"function_call","call_id":"toolu_1","name":"Read","arguments":"{\"path\":\"a.py\"}"},)"
               R"({"type":"function_call","call_id":"toolu_2","name":"Bash","arguments":"{\"command\":\"ls\"}"},)"
               R"({"type":"function_call_output","call_id":"toolu_1","output":"def f(): pass"},)"
               R"({"type":"function_call_output","call_id":"toolu_2","output":"a.py\nb.py"},)"
               R"({"type":"message","role":"user","content":[{"type":"input_text","text":"go on"}]}])");
    };

    "capability_an_error_result_is_prefixed_and_an_image_in_a_result_follows_as_a_user_image"_test = [] {
        auto const done = ok(R"({"model":"m","messages":[{"role":"user","content":[
          {"type":"tool_result","tool_use_id":"t1","is_error":true,"content":"no such file"},
          {"type":"tool_result","tool_use_id":"t2","content":[{"type":"text","text":"see"},{"type":"image","source":{"type":"base64","media_type":"image/png","data":"AAAA"}}]}]}]})");

        expect(field(done, {"input"}) ==
               R"([{"type":"function_call_output","call_id":"t1","output":"Error: no such file"},)"
               R"({"type":"function_call_output","call_id":"t2","output":"see"},)"
               R"({"type":"message","role":"user","content":[{"type":"input_image","image_url":"data:image/png;base64,AAAA"}]}])");
    };

    "capability_user_images_as_base64_or_url_become_input_images"_test = [] {
        auto const done = ok(R"({"model":"m","messages":[{"role":"user","content":[
          {"type":"image","source":{"type":"base64","media_type":"image/jpeg","data":"QQ=="}},
          {"type":"image","source":{"type":"url","url":"https://example.invalid/a.png"}}]}]})");

        expect(field(done, {"input"}) ==
               R"([{"type":"message","role":"user","content":[{"type":"input_image","image_url":"data:image/jpeg;base64,QQ=="},)"
               R"({"type":"input_image","image_url":"https://example.invalid/a.png"}]}])");
    };

    "capability_a_mid_conversation_system_message_becomes_a_developer_message"_test = [] {
        auto const done = ok(R"({"model":"m","messages":[{"role":"user","content":"a"},{"role":"system","content":"rule change"}]})");

        expect(field(done, {"input", "1"}) == "<missing>");
        expect(parsed(done.body).data.index() != 0);
        expect(done.body.contains(R"({"type":"message","role":"developer","content":[{"type":"input_text","text":"rule change"}]})"));
    };

    "capability_custom_tools_become_functions_with_their_schema_unchanged"_test = [] {
        auto const done = ok(R"({"model":"m","messages":[],"tools":[
          {"name":"Bash","description":"run","input_schema":{"type":"object","properties":{"command":{"type":"string"}},"required":["command"]}},
          {"type":"custom","name":"Plain"}]})");

        expect(field(done, {"tools"}) ==
               R"([{"type":"function","name":"Bash","description":"run","parameters":{"type":"object","properties":{"command":{"type":"string"}},"required":["command"]}},)"
               R"({"type":"function","name":"Plain","parameters":{"type":"object","properties":{}}}])");
    };

    "capability_tool_choice_maps_and_disabling_parallel_calls_is_carried"_test = [] {
        auto const tools = R"("tools":[{"name":"T","input_schema":{}}])";
        auto const choice = [&](std::string_view const text) { return ok(std::format(R"({{"model":"m","messages":[],{},"tool_choice":{}}})", tools, text)); };

        expect(field(choice(R"({"type":"auto"})"), {"tool_choice"}) == R"("auto")");
        expect(field(choice(R"({"type":"any"})"), {"tool_choice"}) == R"("required")");
        expect(field(choice(R"({"type":"none"})"), {"tool_choice"}) == R"("none")");
        expect(field(choice(R"({"type":"tool","name":"T"})"), {"tool_choice"}) == R"({"type":"function","name":"T"})");
        expect(field(choice(R"({"type":"auto","disable_parallel_tool_use":true})"), {"parallel_tool_calls"}) == "false");
        expect(field(choice(R"({"type":"auto"})"), {"parallel_tool_calls"}) == "<missing>");
        expect(field(ok(R"({"model":"m","messages":[],"tool_choice":{"type":"any"}})"), {"tool_choice"}) == "<missing>");
    };

    "capability_effort_and_thinking_budget_map_to_a_reasoning_effort"_test = [] {
        auto const effort = [](std::string_view const extra) { return field(ok(std::format(R"({{"model":"m","messages":[],{}}})", extra)), {"reasoning", "effort"}); };

        expect(effort(R"("output_config":{"effort":"low"})") == R"("low")");
        expect(effort(R"("output_config":{"effort":"medium"})") == R"("medium")");
        expect(effort(R"("output_config":{"effort":"high"})") == R"("high")");
        expect(effort(R"("output_config":{"effort":"xhigh"})") == R"("high")");
        expect(effort(R"("thinking":{"type":"enabled","budget_tokens":2000})") == R"("low")");
        expect(effort(R"("thinking":{"type":"enabled","budget_tokens":8000})") == R"("medium")");
        expect(effort(R"("thinking":{"type":"enabled","budget_tokens":32000})") == R"("high")");
        expect(effort(R"("thinking":{"type":"adaptive"})") == R"("medium")");
        expect(effort(R"("thinking":{"type":"disabled"},"output_config":{"effort":"high"})") == "<missing>");
        expect(effort(R"("stream":true)") == "<missing>");
        expect(field(ok(R"({"model":"m","messages":[],"thinking":{"type":"adaptive"}})"), {"reasoning", "summary"}) == R"("auto")");
    };

    "capability_a_thinking_block_written_by_this_gateway_returns_as_a_reasoning_item"_test = [] {
        auto const signature = std::string{responses::signature_prefix} + R"({"type":"reasoning","id":"rs_1","encrypted_content":"abc","summary":[]})";
        auto const body = std::format(R"({{"model":"m","messages":[{{"role":"assistant","content":[{{"type":"thinking","thinking":"hm","signature":{}}},{{"type":"text","text":"ok"}}]}}]}})", json::write(json::make_string(signature)));

        auto const done = ok(body);

        expect(field(done, {"input"}) ==
               R"([{"type":"reasoning","id":"rs_1","encrypted_content":"abc","summary":[]},{"type":"message","role":"assistant","content":[{"type":"output_text","text":"ok"}]}])");
    };

    "limit_thinking_without_this_gateways_signature_and_redacted_thinking_are_dropped"_test = [] {
        auto const done = ok(R"({"model":"m","messages":[{"role":"assistant","content":[
          {"type":"thinking","thinking":"hm","signature":"anthropic-opaque"},{"type":"thinking","thinking":"no signature"},{"type":"redacted_thinking","data":"x"},{"type":"text","text":"ok"}]}]})");

        expect(field(done, {"input"}) == R"([{"type":"message","role":"assistant","content":[{"type":"output_text","text":"ok"}]}])");
    };

    "limit_a_damaged_gateway_signature_is_dropped_not_sent"_test = [] {
        auto const done = ok(std::format(R"({{"model":"m","messages":[{{"role":"assistant","content":[{{"type":"thinking","thinking":"h","signature":"{}{{not json"}},{{"type":"text","text":"ok"}}]}}]}})", responses::signature_prefix));

        expect(field(done, {"input"}) == R"([{"type":"message","role":"assistant","content":[{"type":"output_text","text":"ok"}]}])");
    };

    "capability_fields_the_route_has_no_equivalent_for_are_dropped_and_named"_test = [] {
        auto const done = ok(R"({"model":"m","messages":[],"max_tokens":9,"temperature":0.2,"top_p":1,"top_k":3,"stop_sequences":["x"],"metadata":{"user_id":"u"},"context_management":{},"safeguards":{},"mystery":1})");

        expect(done.dropped == std::vector<std::string>{"max_tokens", "temperature", "top_p", "top_k", "stop_sequences", "metadata", "context_management", "safeguards", "mystery"});
        for (auto const name : responses::rejected_fields) {
            expect(field(done, {name}) == "<missing>") << name;
        }
    };

    "capability_the_translated_request_never_carries_a_field_the_route_rejects"_test = [] {
        auto const done = ok(R"({"model":"m","system":"s","max_tokens":9,"stream":true,"temperature":1,"metadata":{"a":1},
          "tools":[{"name":"T","input_schema":{}}],"tool_choice":{"type":"auto"},"thinking":{"type":"adaptive"},"messages":[{"role":"user","content":"x"}]})",
                             {.prompt_cache_key = "k"});
        auto const root = parsed(done.body);
        auto const* members = std::get_if<json::object>(&root.data);

        expect(members != nullptr);
        for (auto const& [name, ignored] : *members) {
            expect(!std::ranges::contains(responses::rejected_fields, std::string_view{name})) << name;
        }
    };

    "capability_the_session_hash_becomes_the_prompt_cache_key_when_given"_test = [] {
        expect(field(ok(user_hi, {.prompt_cache_key = "abc123"}), {"prompt_cache_key"}) == R"("abc123")");
        expect(field(ok(user_hi), {"prompt_cache_key"}) == "<missing>");
    };

    "limit_a_request_that_is_not_streamed_says_so"_test = [] {
        expect(!ok(R"({"model":"m","messages":[]})").client_streams);
        expect(!ok(R"({"model":"m","stream":false,"messages":[]})").client_streams);
    };

    "capability_a_server_tool_is_refused_with_the_wording_claude_codes_recovery_recognises"_test = [] {
        expect(refused(R"({"model":"m","messages":[],"tools":[{"name":"ok","input_schema":{}},{"type":"advisor_20260301","name":"advisor"}]})") ==
               "tools.1: Input tag 'advisor_20260301' found using 'type' does not match any of the expected tags");
        expect(refused(R"({"model":"m","messages":[],"tools":[{"type":"web_search_20250305","name":"web_search"}]})").contains("web_search_20250305"));
    };

    "capability_a_block_the_route_cannot_carry_is_refused_naming_its_position"_test = [] {
        expect(refused(R"({"model":"m","messages":[{"role":"user","content":"a"},{"role":"user","content":[{"type":"text","text":"a"},{"type":"document","source":{}}]}]})") ==
               "messages.1.content.1: unsupported content block type 'document' for this provider");
        expect(refused(R"({"model":"m","messages":[{"role":"user","content":[{"type":"tool_reference","tool_name":"x"}]}]})").contains("tool_reference"));
    };

    "limit_malformed_requests_are_refused_with_a_reason"_test = [] {
        expect(refused("not json").contains("JSON"));
        expect(refused("[]").contains("object"));
        expect(refused(R"({"messages":[]})").contains("model"));
        expect(refused(R"({"model":"m"})").contains("messages"));
        expect(refused(R"({"model":"m","messages":[{"role":"user","content":[{"type":"text"}]}]})").contains("text"));
        expect(refused(R"({"model":"m","messages":[{"role":"user","content":[{"type":"tool_use","name":"x","input":{}}]}]})").contains("id"));
    };
};

}  // namespace

int main() {
    return boost::ut::cfg<>.run() ? 1 : 0;
}
