#include <boost/ut.hpp>

import llmgate.chat_request;
import llmgate.chat_stream;
import llmgate.json;
import llmgate.sse;
import std;

namespace {

using namespace boost::ut;
namespace ch = llmgate::chat;
namespace js = llmgate::json;

js::value parsed(std::string_view const text) {
    return std::move(*js::parse(text, {.jsonc = false}));
}

ch::translated translated_of(std::string_view const body, ch::quirks const& chosen = {}) {
    auto const done = ch::translate_request(body, chosen);
    return done ? *done : ch::translated{};
}

std::string text_of(js::value const& root, std::initializer_list<std::string_view> path) {
    return js::text_at(root, path);
}

js::value const& message_at(js::value const& root, std::size_t const index) {
    return std::get<js::array>(js::at(root, {"messages"})->data).at(index);
}

constexpr std::string_view conversation = R"({
  "model": "gpt-x", "max_tokens": 100, "stream": true, "temperature": 0.2, "top_p": 0.9, "stop_sequences": ["END"],
  "system": [{"type": "text", "text": "Be brief.", "cache_control": {"type": "ephemeral"}}, {"type": "text", "text": "Use tools."}],
  "thinking": {"type": "enabled", "budget_tokens": 5000}, "metadata": {"user_id": "u"}, "top_k": 5,
  "tools": [{"name": "Bash", "description": "run it", "input_schema": {"type": "object", "properties": {"command": {"type": "string"}}}}],
  "tool_choice": {"type": "any", "disable_parallel_tool_use": true},
  "messages": [
    {"role": "user", "content": [{"type": "text", "text": "list files"}, {"type": "image", "source": {"type": "base64", "media_type": "image/png", "data": "QUJD"}}]},
    {"role": "assistant", "content": [{"type": "thinking", "thinking": "hm", "signature": "s"}, {"type": "text", "text": "Running."},
                                      {"type": "tool_use", "id": "call_1", "name": "Bash", "input": {"command": "ls"}}]},
    {"role": "user", "content": [{"type": "tool_result", "tool_use_id": "call_1", "is_error": true, "content": [{"type": "text", "text": "denied"},
                                  {"type": "image", "source": {"type": "url", "url": "https://x.test/a.png"}}]}, {"type": "text", "text": "try again"}]}
  ]
})";

struct event_log {
    std::vector<std::pair<std::string, js::value>> events{};

    std::vector<std::string> names() const {
        return events | std::views::transform([](auto const& each) { return each.first; }) | std::ranges::to<std::vector>();
    }
};

event_log run_stream(std::vector<std::string> const& chunks, std::vector<ch::renamed_name> renamed = {}, bool finish_at_end = false) {
    ch::stream_translator translator{"claude-model", std::move(renamed)};
    event_log log;
    auto sink = [&log](llmgate::sse::event const& item) { log.events.emplace_back(std::string{item.name}, parsed(item.data)); };
    for (auto const& data : chunks) {
        translator.on_event({.name = {}, .data = data, .id = {}}, sink);
    }
    if (finish_at_end) {
        translator.finish(sink);
    }
    return log;
}

std::string chunk(std::string_view const delta, std::string_view const reason = "null", std::string_view const usage = "null") {
    js::object members{{"id", js::make_string("chatcmpl-1")}};
    members.emplace_back("choices", js::value{js::array{js::make_object({{"index", js::value{std::int64_t{0}}}, {"delta", parsed(delta)}, {"finish_reason", parsed(reason)}})}});
    members.emplace_back("usage", parsed(usage));
    return js::write(js::value{std::move(members)});
}

constexpr std::string_view usage_chunk = R"({"id":"chatcmpl-1","choices":[],"usage":{"prompt_tokens":120,"completion_tokens":7,"total_tokens":127,"prompt_tokens_details":{"cached_tokens":20}}})";

suite<"chat_request"> request_tests = [] {
    "capability_a_conversation_becomes_system_user_assistant_and_tool_messages_in_order"_test = [] {
        auto const out = parsed(translated_of(conversation).body);

        expect(text_of(out, {"model"}) == "gpt-x" && js::at(out, {"stream"}) != nullptr && text_of(out, {"stream_options", "include_usage"}).empty());
        expect(text_of(message_at(out, 0), {"role"}) == "system" && text_of(message_at(out, 0), {"content"}) == "Be brief.\n\nUse tools.");
        expect(text_of(message_at(out, 1), {"role"}) == "user");
        expect(text_of(message_at(out, 2), {"role"}) == "assistant" && text_of(message_at(out, 2), {"content"}) == "Running.");
        expect(text_of(message_at(out, 2), {"tool_calls"}).empty());
        expect(text_of(message_at(out, 3), {"role"}) == "tool" && text_of(message_at(out, 3), {"tool_call_id"}) == "call_1" && text_of(message_at(out, 3), {"content"}) == "Error: denied");
        expect(text_of(message_at(out, 4), {"role"}) == "user" && text_of(message_at(out, 5), {"role"}) == "user");
    };

    "capability_an_image_becomes_an_image_url_part_and_text_alone_becomes_a_plain_string"_test = [] {
        auto const out = parsed(translated_of(conversation).body);
        auto const& parts = std::get<js::array>(js::at(message_at(out, 1), {"content"})->data);

        expect(parts.size() == 2_u && text_of(parts.at(0), {"type"}) == "text" && text_of(parts.at(1), {"type"}) == "image_url");
        expect(text_of(parts.at(1), {"image_url", "url"}) == "data:image/png;base64,QUJD");
        expect(text_of(message_at(out, 5), {"content"}) == "try again");
        auto const& pictures = std::get<js::array>(js::at(message_at(out, 4), {"content"})->data);
        expect(pictures.size() == 1_u && text_of(pictures.front(), {"image_url", "url"}) == "https://x.test/a.png");
    };

    "capability_the_assistant_turn_keeps_its_tool_calls_with_json_arguments_and_drops_thinking"_test = [] {
        auto const out = parsed(translated_of(conversation).body);
        auto const& calls = std::get<js::array>(js::at(message_at(out, 2), {"tool_calls"})->data);

        expect(calls.size() == 1_u && text_of(calls.front(), {"id"}) == "call_1" && text_of(calls.front(), {"type"}) == "function");
        expect(text_of(calls.front(), {"function", "name"}) == "Bash" && text_of(calls.front(), {"function", "arguments"}) == R"({"command":"ls"})");
        expect(!translated_of(conversation).body.contains("hm") && !translated_of(conversation).body.contains("cache_control"));
    };

    "capability_limits_stop_sampling_effort_tools_and_choice_are_mapped"_test = [] {
        auto const out = parsed(translated_of(conversation).body);

        expect(text_of(out, {"max_tokens"}) == "" && js::count_at(out, {"max_tokens"}) == std::optional<std::uint64_t>{100});
        expect(text_of(out, {"stop"}).empty() && std::get<js::array>(js::at(out, {"stop"})->data).size() == 1_u);
        expect(js::at(out, {"temperature"}) != nullptr && js::at(out, {"top_p"}) != nullptr);
        expect(text_of(out, {"reasoning_effort"}) == "medium" && text_of(out, {"tool_choice"}) == "required");
        expect(std::get<bool>(js::at(out, {"parallel_tool_calls"})->data) == false);
        auto const& tool = std::get<js::array>(js::at(out, {"tools"})->data).front();
        expect(text_of(tool, {"type"}) == "function" && text_of(tool, {"function", "name"}) == "Bash" && text_of(tool, {"function", "description"}) == "run it");
    };

    "limit_fields_the_provider_has_no_place_for_are_reported_as_dropped"_test = [] {
        auto const done = translated_of(conversation);

        expect(done.client_streams && done.dropped == std::vector<std::string>{"metadata", "top_k"});
    };

    "capability_each_quirk_changes_only_its_own_field"_test = [] {
        auto const completion = parsed(translated_of(conversation, {.use_max_completion_tokens = true}).body);
        auto const no_stop = parsed(translated_of(conversation, {.drop_stop_sequences = true}).body);
        auto const no_sampling = parsed(translated_of(conversation, {.drop_sampling = true}).body);
        auto const no_effort = parsed(translated_of(conversation, {.drop_reasoning_effort = true}).body);

        expect(js::at(completion, {"max_completion_tokens"}) != nullptr && js::at(completion, {"max_tokens"}) == nullptr);
        expect(js::at(no_stop, {"stop"}) == nullptr && js::at(no_stop, {"temperature"}) != nullptr && js::at(no_stop, {"reasoning_effort"}) != nullptr);
        expect(js::at(no_sampling, {"temperature"}) == nullptr && js::at(no_sampling, {"top_p"}) == nullptr && js::at(no_sampling, {"stop"}) != nullptr);
        expect(js::at(no_effort, {"reasoning_effort"}) == nullptr && js::at(no_effort, {"temperature"}) != nullptr);
    };

    "capability_the_with_tools_quirk_drops_the_effort_only_when_the_request_has_tools"_test = [] {
        constexpr std::string_view with_tools = R"({"model":"m","thinking":{"type":"adaptive"},"tools":[{"name":"a","input_schema":{}}],"messages":[{"role":"user","content":"hi"}]})";
        constexpr std::string_view without = R"({"model":"m","thinking":{"type":"adaptive"},"messages":[{"role":"user","content":"hi"}]})";
        ch::quirks const quirk{.drop_reasoning_effort_with_tools = true};

        expect(js::at(parsed(translated_of(with_tools, quirk).body), {"reasoning_effort"}) == nullptr);
        expect(js::at(parsed(translated_of(without, quirk).body), {"reasoning_effort"}) != nullptr);
        expect(js::at(parsed(translated_of(with_tools).body), {"reasoning_effort"}) != nullptr);
    };

    "capability_thinking_disabled_or_absent_sends_no_effort_and_a_named_effort_wins_over_the_budget"_test = [] {
        auto const off = parsed(translated_of(R"({"model":"m","thinking":{"type":"disabled"},"messages":[{"role":"user","content":"hi"}]})").body);
        auto const none = parsed(translated_of(R"({"model":"m","messages":[{"role":"user","content":"hi"}]})").body);
        auto const named = parsed(translated_of(R"({"model":"m","thinking":{"type":"adaptive"},"output_config":{"effort":"max"},"messages":[{"role":"user","content":"hi"}]})").body);

        expect(js::at(off, {"reasoning_effort"}) == nullptr && js::at(none, {"reasoning_effort"}) == nullptr);
        expect(text_of(named, {"reasoning_effort"}) == "high");
    };

    "capability_a_tool_name_the_server_cannot_take_is_replaced_and_remembered_for_the_answer"_test = [] {
        std::string const long_name(80, 'x');
        auto const body = std::format(R"({{"model":"m","tools":[{{"name":"{}","input_schema":{{}}}},{{"name":"ok-name","input_schema":{{}}}}],"tool_choice":{{"type":"tool","name":"{}"}},
            "messages":[{{"role":"assistant","content":[{{"type":"tool_use","id":"c","name":"a b","input":{{}}}}]}}]}})", long_name, long_name);
        auto const done = translated_of(body);
        auto const out = parsed(done.body);
        auto const used_for = [&done](std::string_view const original) {
            auto const found = std::ranges::find(done.renamed, original, &ch::renamed_name::original);
            return found == done.renamed.end() ? std::string{} : found->used;
        };

        expect(done.renamed.size() == 2_u && used_for(long_name).size() == ch::longest_name && used_for("a b").starts_with("a_b_"));
        auto const& tool = std::get<js::array>(js::at(out, {"tools"})->data);
        expect(text_of(tool.at(0), {"function", "name"}) == used_for(long_name) && text_of(tool.at(1), {"function", "name"}) == "ok-name");
        expect(text_of(out, {"tool_choice", "function", "name"}) == used_for(long_name));
        expect(text_of(std::get<js::array>(js::at(message_at(out, 0), {"tool_calls"})->data).front(), {"function", "name"}) == used_for("a b"));
    };

    "limit_requests_the_server_cannot_serve_are_refused_by_name"_test = [] {
        auto const server_tool = ch::translate_request(R"({"model":"m","tools":[{"type":"advisor_20260301","name":"a"}],"messages":[]})", {});
        auto const user_call = ch::translate_request(R"({"model":"m","messages":[{"role":"user","content":[{"type":"tool_use","id":"c","name":"n","input":{}}]}]})", {});
        auto const bad_image = ch::translate_request(R"({"model":"m","messages":[{"role":"user","content":[{"type":"image","source":{"type":"file"}}]}]})", {});
        auto const no_model = ch::translate_request(R"({"messages":[]})", {});
        auto const broken = ch::translate_request("{", {});

        expect(!server_tool && server_tool.error().message.contains("advisor_20260301"));
        expect(!user_call && user_call.error().message.contains("messages.0.content.0"));
        expect(!bad_image && bad_image.error().message.contains("base64 or url"));
        expect(!no_model && no_model.error().message.contains("model is required"));
        expect(!broken && broken.error().message.contains("not valid JSON"));
    };
};

suite<"chat_stream"> stream_tests = [] {
    "capability_text_chunks_become_one_text_block_and_the_end_waits_for_the_usage_chunk"_test = [] {
        auto const log = run_stream({chunk(R"({"role":"assistant","content":""})"), chunk(R"({"content":"Hel"})"), chunk(R"({"content":"lo"})"), chunk("{}", R"("stop")")});

        expect(log.names() == std::vector<std::string>{"message_start", "ping", "content_block_start", "content_block_delta", "content_block_delta", "ping"});
        auto const done = run_stream({chunk(R"({"content":"Hi"})"), chunk("{}", R"("stop")"), std::string{usage_chunk}, "[DONE]"});
        expect(done.names() == std::vector<std::string>{"message_start", "content_block_start", "content_block_delta", "ping", "content_block_stop", "message_delta", "message_stop"});
        auto const& delta = done.events.at(5).second;
        expect(text_of(delta, {"delta", "stop_reason"}) == "end_turn");
        expect(js::count_at(delta, {"usage", "input_tokens"}) == std::optional<std::uint64_t>{100} && js::count_at(delta, {"usage", "cache_read_input_tokens"}) == std::optional<std::uint64_t>{20});
        expect(js::count_at(delta, {"usage", "output_tokens"}) == std::optional<std::uint64_t>{7});
    };

    "capability_the_text_deltas_carry_the_text_in_order"_test = [] {
        auto const log = run_stream({chunk(R"({"content":"Hel"})"), chunk(R"({"content":"lo"})"), chunk("{}", R"("stop")", R"({"prompt_tokens":3,"completion_tokens":2})")});

        expect(text_of(log.events.at(2).second, {"delta", "text"}) == "Hel" && text_of(log.events.at(3).second, {"delta", "text"}) == "lo");
        expect(log.names().back() == "message_stop");
    };

    "capability_tool_call_fragments_are_joined_into_one_tool_use_with_the_original_name"_test = [] {
        std::string const used = "short_name_abcd1234";
        auto const log = run_stream({chunk(R"({"tool_calls":[{"index":0,"id":"call_9","type":"function","function":{"name":")" + used + R"(","arguments":""}}]})"),
                                     chunk(R"({"tool_calls":[{"index":0,"function":{"arguments":"{\"comm"}}]})"), chunk(R"({"tool_calls":[{"index":0,"function":{"arguments":"and\":\"ls\"}"}}]})"),
                                     chunk("{}", R"("tool_calls")", R"({"prompt_tokens":4,"completion_tokens":5})")},
                                    {{.used = used, .original = "a very long original tool name"}});

        auto const names = log.names();
        expect(names.at(names.size() - 5) == "content_block_start" && names.at(names.size() - 4) == "content_block_delta" && names.at(names.size() - 3) == "content_block_stop");
        auto const& start = log.events.at(names.size() - 5).second;
        expect(text_of(start, {"content_block", "type"}) == "tool_use" && text_of(start, {"content_block", "id"}) == "call_9" && text_of(start, {"content_block", "name"}) == "a very long original tool name");
        expect(text_of(log.events.at(names.size() - 4).second, {"delta", "partial_json"}) == R"({"command":"ls"})");
        expect(text_of(log.events.at(names.size() - 2).second, {"delta", "stop_reason"}) == "tool_use");
    };

    "capability_text_then_two_tool_calls_keep_their_order_and_a_stop_reason_of_tool_use_even_when_the_server_says_stop"_test = [] {
        auto const log = run_stream({chunk(R"({"content":"Let me"})"),
                                     chunk(R"({"tool_calls":[{"index":1,"id":"b","function":{"name":"two","arguments":"{}"}},{"index":0,"id":"a","function":{"name":"one","arguments":"{}"}}]})"),
                                     chunk("{}", R"("stop")", R"({"prompt_tokens":1,"completion_tokens":1})")});

        std::vector<std::string> starts;
        for (auto const& [name, body] : log.events) {
            if (name == "content_block_start") {
                starts.push_back(text_of(body, {"content_block", "type"}) + ":" + text_of(body, {"content_block", "name"}));
            }
        }
        expect(starts == std::vector<std::string>{"text:", "tool_use:one", "tool_use:two"}) << starts.size();
        expect(text_of(log.events.at(log.events.size() - 2).second, {"delta", "stop_reason"}) == "tool_use");
    };

    "capability_the_end_marker_without_a_usage_chunk_ends_the_message_with_zero_usage"_test = [] {
        auto const log = run_stream({chunk(R"({"content":"x"})"), chunk("{}", R"("length")"), "[DONE]"});

        auto const& delta = log.events.at(log.events.size() - 2).second;
        expect(text_of(delta, {"delta", "stop_reason"}) == "max_tokens" && js::count_at(delta, {"usage", "output_tokens"}) == std::optional<std::uint64_t>{0});
    };

    "capability_finish_closes_a_stream_that_ended_after_its_finish_reason_but_not_one_cut_short"_test = [] {
        auto const closed = run_stream({chunk(R"({"content":"x"})"), chunk("{}", R"("stop")")}, {}, true);
        auto const cut = run_stream({chunk(R"({"content":"x"})")}, {}, true);

        expect(closed.names().back() == "message_stop" && cut.names().back() != "message_stop");
    };

    "limit_an_error_chunk_an_unparsable_chunk_and_an_empty_answer_are_handled_by_name"_test = [] {
        auto const failed = run_stream({R"({"error":{"message":"overloaded","code":"rate_limit_exceeded"}})"});
        auto const empty = run_stream({chunk("{}", R"("stop")", R"({"prompt_tokens":1,"completion_tokens":0})")});
        ch::stream_translator garbage{"m", {}};
        std::vector<std::string> seen;
        garbage.on_event({.name = {}, .data = "not json", .id = {}}, [&seen](llmgate::sse::event const& item) { seen.emplace_back(item.name); });

        expect(failed.names().back() == "error" && text_of(failed.events.back().second, {"error", "type"}) == "rate_limit_error" && text_of(failed.events.back().second, {"error", "message"}) == "overloaded");
        expect(empty.names().back() == "error" && text_of(empty.events.back().second, {"error", "message"}).contains("no output"));
        expect(garbage.ignored().at("unparsed_chunk") == 1_u && seen.empty());
    };

    "capability_a_tool_call_whose_arguments_are_not_a_json_object_is_dropped_and_counted"_test = [] {
        ch::stream_translator translator{"m", {}};
        std::vector<std::string> names;
        auto sink = [&names](llmgate::sse::event const& item) { names.emplace_back(item.name); };
        translator.on_event({.name = {}, .data = chunk(R"({"content":"ok","tool_calls":[{"index":0,"id":"c","function":{"name":"n","arguments":"{\"broken"}}]})"), .id = {}}, sink);
        translator.on_event({.name = {}, .data = chunk("{}", R"("tool_calls")", R"({"prompt_tokens":1,"completion_tokens":1})"), .id = {}}, sink);

        expect(translator.ignored().at("dropped_incomplete_tool_call") == 1_u);
        expect(std::ranges::count(names, std::string{"content_block_start"}) == 1 && names.back() == "message_stop");
    };

    "capability_an_upstream_error_status_becomes_an_anthropic_error_with_the_upstream_message"_test = [] {
        auto const limited = parsed(ch::error_body(429, R"({"error":{"message":"slow down"}})"));
        auto const bare = parsed(ch::error_body(500, "oops"));

        expect(text_of(limited, {"type"}) == "error" && text_of(limited, {"error", "type"}) == "rate_limit_error" && text_of(limited, {"error", "message"}) == "slow down");
        expect(text_of(bare, {"error", "type"}) == "api_error" && text_of(bare, {"error", "message"}) == "the upstream answered 500");
    };
};

}  // namespace

int main() {}
