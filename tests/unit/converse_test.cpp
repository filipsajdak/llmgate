#include <boost/ut.hpp>

import llmgate.bedrock_options;
import llmgate.converse_request;
import llmgate.converse_stream;
import llmgate.json;
import llmgate.sse;
import std;

namespace {

using namespace boost::ut;
namespace cv = llmgate::converse;
namespace js = llmgate::json;

js::value parsed(std::string_view const text) {
    return std::move(*js::parse(text, {.jsonc = false}));
}

cv::translated translated_of(std::string_view const body, llmgate::bedrock::converse_options const& chosen = {}) {
    auto const done = cv::translate_request(body, chosen);
    return done ? *done : cv::translated{};
}

std::string refusal_of(std::string_view const body) {
    auto const done = cv::translate_request(body, {});
    return done ? std::string{} : done.error().message;
}

std::string text_of(js::value const& root, std::initializer_list<std::string_view> path) {
    return js::text_at(root, path);
}

js::array const& array_at(js::value const& root, std::initializer_list<std::string_view> path) {
    return std::get<js::array>(js::at(root, path)->data);
}

struct sink_result {
    std::vector<std::pair<std::string, std::string>> events{};
};

sink_result run(cv::stream_translator& translator, std::vector<std::pair<std::string, std::string>> const& frames, bool const finish = false) {
    sink_result out;
    auto sink = [&out](llmgate::sse::event const& each) { out.events.emplace_back(each.name, each.data); };
    for (auto const& [type, payload] : frames) {
        translator.on_event(type, payload, sink);
    }
    if (finish) {
        translator.finish(sink);
    }
    return out;
}

std::vector<std::string> names_of(sink_result const& result) {
    std::vector<std::string> names;
    for (auto const& [name, data] : result.events) {
        names.push_back(name);
    }
    return names;
}

constexpr std::string_view PNG = "iVBORw0KGgo=";

suite<"converse_request"> request_tests = [] {
    "capability_text_system_and_limits_map_to_the_converse_shape"_test = [] {
        auto const done = translated_of(R"({"model":"m","stream":true,"system":"Be brief.","max_tokens":9,"temperature":0.5,"top_p":0.9,"stop_sequences":["x"],"messages":[{"role":"user","content":"hi"}]})");
        auto const out = parsed(done.body);

        expect(done.client_streams && js::at(out, {"model"}) == nullptr && js::at(out, {"stream"}) == nullptr);
        expect(text_of(out, {"system"}).empty() && text_of(array_at(out, {"system"}).front(), {"text"}) == "Be brief.");
        expect(text_of(array_at(out, {"messages"}).front(), {"role"}) == "user" && text_of(array_at(out, {"messages"}).front(), {"content"}).empty());
        expect(js::count_at(out, {"inferenceConfig", "maxTokens"}) == std::optional<std::uint64_t>{9} && js::at(out, {"inferenceConfig", "stopSequences"}) != nullptr);
        expect(js::at(out, {"inferenceConfig", "topP"}) != nullptr && js::at(out, {"inferenceConfig", "temperature"}) != nullptr);
    };

    "capability_images_pdf_and_media_become_bytes_blocks_with_a_format"_test = [] {
        auto const body = std::format(R"({{"model":"m","messages":[{{"role":"user","content":[
            {{"type":"image","source":{{"type":"base64","media_type":"image/jpg","data":"{0}"}}}},
            {{"type":"document","source":{{"type":"base64","media_type":"application/pdf","data":"{0}"}}}},
            {{"type":"audio","source":{{"type":"base64","media_type":"audio/mpeg","data":"{0}"}}}},
            {{"type":"video","source":{{"type":"base64","media_type":"video/quicktime","data":"{0}"}}}}]}}]}})", PNG);
        auto const out = parsed(translated_of(body).body);
        auto const& blocks = array_at(array_at(out, {"messages"}).front(), {"content"});

        expect(blocks.size() == 4_u && text_of(blocks.at(0), {"image", "format"}) == "jpeg" && text_of(blocks.at(0), {"image", "source", "bytes"}) == PNG);
        expect(text_of(blocks.at(1), {"document", "format"}) == "pdf" && text_of(blocks.at(1), {"document", "name"}) == "document-1");
        expect(text_of(blocks.at(2), {"audio", "format"}) == "mp3" && text_of(blocks.at(3), {"video", "format"}) == "mov");
    };

    "limit_media_without_base64_data_and_an_unknown_document_type_are_refused"_test = [] {
        expect(refusal_of(R"({"model":"m","messages":[{"role":"user","content":[{"type":"image","source":{"type":"url","url":"x"}}]}]})").contains("image blocks must carry base64"));
        expect(refusal_of(R"({"model":"m","messages":[{"role":"user","content":[{"type":"document","source":{"type":"base64","media_type":"application/zip","data":"QQ=="}}]}]})").contains("document blocks"));
        expect(refusal_of(R"({"model":"m","messages":[{"role":"user","content":[{"type":"server_tool_use"}]}]})").contains("messages.0.content.0"));
    };

    "capability_tool_use_and_results_round_trip_with_status_and_empty_text_is_dropped"_test = [] {
        auto const out = parsed(translated_of(R"({"model":"m","messages":[
            {"role":"user","content":"run it"},
            {"role":"assistant","content":[{"type":"text","text":"  "},{"type":"tool_use","id":"t1","name":"Bash","input":{"command":"ls"}}]},
            {"role":"user","content":[{"type":"tool_result","tool_use_id":"t1","is_error":true,"content":"boom"},{"type":"tool_result","tool_use_id":"t2","content":[]}]}]})").body);
        auto const& turns = array_at(out, {"messages"});
        auto const& assistant = array_at(turns.at(1), {"content"});
        auto const& results = array_at(turns.at(2), {"content"});

        expect(turns.size() == 3_u && assistant.size() == 1_u && text_of(assistant.front(), {"toolUse", "name"}) == "Bash" && text_of(assistant.front(), {"toolUse", "toolUseId"}) == "t1");
        expect(text_of(results.at(0), {"toolResult", "status"}) == "error" && text_of(results.at(1), {"toolResult", "status"}) == "success");
        expect(text_of(array_at(results.at(1), {"toolResult", "content"}).front(), {"text"}) == "(empty)");
    };

    "capability_tools_become_tool_specs_with_the_choice_mapped_and_none_removes_them"_test = [] {
        auto const base = R"("model":"m","messages":[{"role":"user","content":"x"}],"tools":[{"name":"Bash","description":"d","input_schema":{"type":"object","properties":{"c":{"type":"string"}}}}])";
        auto const any = parsed(translated_of(std::format("{{{},\"tool_choice\":{{\"type\":\"any\"}}}}", base)).body);
        auto const pinned = parsed(translated_of(std::format("{{{},\"tool_choice\":{{\"type\":\"tool\",\"name\":\"Bash\"}}}}", base)).body);
        auto const none = parsed(translated_of(std::format("{{{},\"tool_choice\":{{\"type\":\"none\"}}}}", base)).body);

        expect(text_of(array_at(any, {"toolConfig", "tools"}).front(), {"toolSpec", "name"}) == "Bash" && js::at(any, {"toolConfig", "toolChoice", "any"}) != nullptr);
        expect(text_of(pinned, {"toolConfig", "toolChoice", "tool", "name"}) == "Bash" && js::at(none, {"toolConfig"}) == nullptr);
        expect(js::at(any, {"toolConfig", "tools"}) != nullptr && refusal_of(R"({"model":"m","messages":[],"tools":[{"type":"advisor_20260301","name":"a"}]})").contains("advisor_20260301"));
    };

    "capability_a_tool_name_over_64_characters_is_hashed_and_recorded_for_the_way_back"_test = [] {
        auto const name = std::string(80, 'a');
        auto const done = translated_of(std::format(R"({{"model":"m","messages":[{{"role":"user","content":"x"}}],"tools":[{{"name":"{}","input_schema":{{}}}}]}})", name));
        auto const out = parsed(done.body);

        expect(done.renamed.size() == 1_u && done.renamed.front().original == name && done.renamed.front().used.size() == 64_u);
        expect(text_of(array_at(out, {"toolConfig", "tools"}).front(), {"toolSpec", "name"}) == done.renamed.front().used);
    };

    "capability_cache_control_becomes_a_cache_point_after_the_block_and_can_be_dropped"_test = [] {
        constexpr std::string_view body = R"({"model":"m","system":[{"type":"text","text":"s","cache_control":{"type":"ephemeral"}}],"messages":[{"role":"user","content":[{"type":"text","text":"u","cache_control":{"type":"ephemeral"}}]}],"tools":[{"name":"T","input_schema":{},"cache_control":{"type":"ephemeral"}}]})";
        auto const kept = parsed(translated_of(body).body);
        auto const dropped = parsed(translated_of(body, {.drop_cache_points = true}).body);

        expect(array_at(kept, {"system"}).size() == 2_u && js::at(array_at(kept, {"system"}).at(1), {"cachePoint"}) != nullptr);
        expect(array_at(array_at(kept, {"messages"}).front(), {"content"}).size() == 2_u && array_at(kept, {"toolConfig", "tools"}).size() == 2_u);
        expect(array_at(dropped, {"system"}).size() == 1_u && array_at(dropped, {"toolConfig", "tools"}).size() == 1_u);
    };

    "capability_thinking_is_passed_only_for_a_listed_model_and_otherwise_dropped_and_named"_test = [] {
        constexpr std::string_view body = R"({"model":"claude-x","messages":[{"role":"user","content":"x"},{"role":"assistant","content":[{"type":"thinking","thinking":"t","signature":"s"},{"type":"text","text":"a"}]}],"thinking":{"type":"enabled","budget_tokens":2000},"output_config":{"effort":"high"},"context_management":{},"metadata":{}})";
        auto const listed = translated_of(body, {.thinking_models = {"claude-x"}});
        auto const plain = translated_of(body);
        auto const with = parsed(listed.body);
        auto const without = parsed(plain.body);

        expect(text_of(with, {"additionalModelRequestFields", "thinking", "type"}) == "enabled" && listed.dropped == std::vector<std::string>{"output_config", "context_management", "metadata"});
        expect(text_of(array_at(array_at(with, {"messages"}).at(1), {"content"}).front(), {"reasoningContent", "reasoningText", "signature"}) == "s");
        expect(js::at(without, {"additionalModelRequestFields"}) == nullptr && array_at(array_at(without, {"messages"}).at(1), {"content"}).size() == 1_u);
        expect(plain.dropped == std::vector<std::string>{"thinking", "output_config", "context_management", "metadata"});
    };

    "capability_system_messages_in_the_conversation_merge_into_neighbouring_turns"_test = [] {
        auto const out = parsed(translated_of(R"({"model":"m","messages":[{"role":"system","content":"lead"},{"role":"user","content":"a"},{"role":"system","content":"b"},{"role":"assistant","content":"x"},{"role":"system","content":"c"}]})").body);
        auto const& turns = array_at(out, {"messages"});

        expect(array_at(out, {"system"}).size() == 1_u && turns.size() == 3_u);
        expect(array_at(turns.at(0), {"content"}).size() == 2_u && text_of(turns.at(2), {"role"}) == "user");
    };

    "limit_a_body_that_is_not_an_object_or_has_no_messages_is_refused"_test = [] {
        expect(!refusal_of("[]").empty() && !refusal_of("{").empty() && refusal_of(R"({"model":"m"})").contains("messages is required"));
    };
};

suite<"converse_stream"> stream_tests = [] {
    "capability_a_text_turn_streams_with_usage_held_until_the_metadata_event"_test = [] {
        cv::stream_translator translator{"nova", {}};
        auto const before = run(translator, {{"messageStart", R"({"role":"assistant"})"}, {"contentBlockDelta", R"({"contentBlockIndex":0,"delta":{"text":"Hel"}})"},
                                             {"contentBlockDelta", R"({"contentBlockIndex":0,"delta":{"text":"lo"}})"}, {"contentBlockStop", R"({"contentBlockIndex":0})"},
                                             {"messageStop", R"({"stopReason":"end_turn"})"}});
        auto const after = run(translator, {{"metadata", R"({"usage":{"inputTokens":10,"outputTokens":3,"cacheReadInputTokens":100,"cacheWriteInputTokens":7},"metrics":{}})"}});

        expect(names_of(before) == std::vector<std::string>{"message_start", "content_block_start", "content_block_delta", "content_block_delta", "content_block_stop"});
        expect(names_of(after) == std::vector<std::string>{"message_delta", "message_stop"});
        auto const usage = parsed(after.events.front().second);
        expect(text_of(usage, {"delta", "stop_reason"}) == "end_turn" && js::count_at(usage, {"usage", "cache_read_input_tokens"}) == std::optional<std::uint64_t>{100});
        expect(js::count_at(usage, {"usage", "cache_creation_input_tokens"}) == std::optional<std::uint64_t>{7} && js::count_at(usage, {"usage", "input_tokens"}) == std::optional<std::uint64_t>{10});
    };

    "capability_a_tool_call_streams_its_fragments_and_gets_its_original_name_back"_test = [] {
        auto const hashed = std::string(60, 'a') + "_deadbeef";
        cv::stream_translator translator{"m", {{.used = hashed, .original = std::string(80, 'a')}}};
        auto const out = run(translator, {{"messageStart", "{}"},
                                          {"contentBlockStart", std::format(R"({{"contentBlockIndex":1,"start":{{"toolUse":{{"toolUseId":"t1","name":"{}"}}}}}})", hashed)},
                                          {"contentBlockDelta", R"({"contentBlockIndex":1,"delta":{"toolUse":{"input":"{\"c\":"}}})"},
                                          {"contentBlockDelta", R"({"contentBlockIndex":1,"delta":{"toolUse":{"input":"1}"}}})"}, {"contentBlockStop", R"({"contentBlockIndex":1})"},
                                          {"messageStop", R"({"stopReason":"tool_use"})"}, {"metadata", R"({"usage":{"inputTokens":1,"outputTokens":1}})"}});
        auto const start = parsed(out.events.at(1).second);

        expect(text_of(start, {"content_block", "name"}) == std::string(80, 'a') && text_of(start, {"content_block", "id"}) == "t1" && js::count_at(start, {"index"}) == std::optional<std::uint64_t>{1});
        expect(names_of(out).back() == "message_stop" && text_of(parsed(out.events.at(out.events.size() - 2).second), {"delta", "stop_reason"}) == "tool_use");
    };

    "capability_a_tool_call_without_input_gets_empty_object_arguments"_test = [] {
        cv::stream_translator translator{"m", {}};
        auto const out = run(translator, {{"contentBlockStart", R"({"contentBlockIndex":0,"start":{"toolUse":{"toolUseId":"t","name":"N"}}})"}, {"contentBlockStop", R"({"contentBlockIndex":0})"}});

        expect(out.events.size() == 4_u && text_of(parsed(out.events.at(2).second), {"delta", "partial_json"}) == "{}");
    };

    "capability_reasoning_becomes_a_thinking_block_with_signature"_test = [] {
        cv::stream_translator translator{"m", {}};
        auto const out = run(translator, {{"contentBlockDelta", R"({"contentBlockIndex":0,"delta":{"reasoningContent":{"text":"hm"}}})"},
                                          {"contentBlockDelta", R"({"contentBlockIndex":0,"delta":{"reasoningContent":{"signature":"sig"}}})"}});

        expect(text_of(parsed(out.events.at(1).second), {"content_block", "type"}) == "thinking" && text_of(parsed(out.events.at(2).second), {"delta", "type"}) == "thinking_delta");
        expect(text_of(parsed(out.events.at(3).second), {"delta", "signature"}) == "sig");
    };

    "capability_every_stop_reason_maps_and_the_stream_ends_without_usage_when_metadata_never_comes"_test = [] {
        auto const stop = [](std::string_view const reason) {
            cv::stream_translator translator{"m", {}};
            auto const out = run(translator, {{"contentBlockDelta", R"({"contentBlockIndex":0,"delta":{"text":"x"}})"}, {"messageStop", std::format(R"({{"stopReason":"{}"}})", reason)}}, true);
            return text_of(parsed(out.events.at(out.events.size() - 2).second), {"delta", "stop_reason"});
        };

        expect(stop("end_turn") == "end_turn" && stop("max_tokens") == "max_tokens" && stop("stop_sequence") == "stop_sequence");
        expect(stop("guardrail_intervened") == "refusal" && stop("content_filtered") == "refusal" && stop("tool_use") == "tool_use");
    };

    "limit_an_empty_answer_is_an_error_and_odd_events_are_counted_not_fatal"_test = [] {
        cv::stream_translator empty{"m", {}};
        auto const out = run(empty, {{"messageStart", "{}"}, {"messageStop", R"({"stopReason":"end_turn"})"}, {"metadata", "{}"}});
        cv::stream_translator odd{"m", {}};
        run(odd, {{"somethingNew", "{}"}, {"contentBlockDelta", "not json"}});

        expect(names_of(out).back() == "error" && text_of(parsed(out.events.back().second), {"error", "message"}).contains("no output"));
        expect(odd.ignored().at("unknown_event") == 1_u && odd.ignored().at("unparsed_event") == 1_u);
    };
};

}  // namespace

int main() {}
