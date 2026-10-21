#include <boost/ut.hpp>

import llmgate.anthropic;
import llmgate.json;
import llmgate.responses_request;
import llmgate.responses_stream;
import llmgate.sse;
import std;

namespace {

using namespace boost::ut;
using namespace llmgate;

struct wire_event {
    std::string name;
    std::string data;
};

std::string stream_text(std::vector<wire_event> const& events) {
    std::string out;
    for (auto const& item : events) {
        sse::write_event(out, item.name, item.data);
    }
    return out;
}

std::vector<wire_event> translate(std::vector<wire_event> const& upstream, std::size_t const chunk = 0, responses::stream_translator* use = nullptr) {
    responses::stream_translator local{"claude-sonnet-5-5"};
    auto& translator = use == nullptr ? local : *use;
    std::vector<wire_event> out;
    sse::parser parser;
    auto const wire = stream_text(upstream);
    auto const feed = [&](std::string_view const piece) {
        parser.feed(piece, [&](sse::event const& incoming) {
            translator.on_event(incoming, [&](sse::event const& produced) { out.push_back({std::string{produced.name}, std::string{produced.data}}); });
        });
    };
    if (chunk == 0) {
        feed(wire);
    } else {
        for (std::size_t at = 0; at < wire.size(); at += chunk) {
            feed(std::string_view{wire}.substr(at, chunk));
        }
    }
    return out;
}

std::string names(std::vector<wire_event> const& events) {
    std::string out;
    for (auto const& item : events) {
        out += out.empty() ? "" : " ";
        out += item.name;
    }
    return out;
}

wire_event event(std::string_view const type, std::string_view const rest = "") {
    return {std::string{type}, std::format(R"({{"type":"{}"{}}})", type, rest)};
}

constexpr std::string_view created = R"(,"response":{"id":"resp_1","model":"gpt-5.6-terra","status":"in_progress"})";
constexpr std::string_view usage_120_100_9 = R"(,"response":{"id":"resp_1","status":"completed","usage":{"input_tokens":120,"input_tokens_details":{"cached_tokens":100},"output_tokens":9,"output_tokens_details":{"reasoning_tokens":4},"total_tokens":129}})";

std::vector<wire_event> text_stream() {
    return {event("response.created", created),
            event("response.in_progress", created),
            event("response.output_item.added", R"(,"output_index":0,"item":{"type":"message","id":"msg_a","role":"assistant","content":[]})"),
            event("response.content_part.added", R"(,"output_index":0,"content_index":0,"part":{"type":"output_text","text":""})"),
            event("response.output_text.delta", R"(,"output_index":0,"content_index":0,"delta":"Hel")"),
            event("response.output_text.delta", R"(,"output_index":0,"content_index":0,"delta":"lo")"),
            event("response.output_text.done", R"(,"output_index":0,"content_index":0,"text":"Hello")"),
            event("response.content_part.done", R"(,"output_index":0,"content_index":0)"),
            event("response.output_item.done", R"(,"output_index":0,"item":{"type":"message","id":"msg_a"})"),
            event("response.completed", usage_120_100_9)};
}

std::vector<wire_event> tool_stream() {
    return {event("response.created", created),
            event("response.output_item.added", R"(,"output_index":0,"item":{"type":"function_call","id":"fc_1","call_id":"call_1","name":"Bash","arguments":""})"),
            event("response.function_call_arguments.delta", R"(,"output_index":0,"item_id":"fc_1","delta":"{\"command\":")"),
            event("response.function_call_arguments.delta", R"(,"output_index":0,"item_id":"fc_1","delta":"\"ls\"}")"),
            event("response.function_call_arguments.done", R"(,"output_index":0,"item_id":"fc_1","arguments":"{\"command\":\"ls\"}")"),
            event("response.output_item.done", R"(,"output_index":0,"item":{"type":"function_call","id":"fc_1","call_id":"call_1","name":"Bash","arguments":"{\"command\":\"ls\"}"})"),
            event("response.completed", usage_120_100_9)};
}

void grammar_holds(std::vector<wire_event> const& produced) {
    std::vector<anthropic::output_event> events;
    for (auto const& item : produced) {
        events.push_back({.name = item.name, .data = item.data});
    }
    auto const verdict = anthropic::validate_grammar(events);
    expect(verdict.has_value()) << (verdict ? "" : verdict.error());
}

json::value parsed(std::string_view const text) {
    auto const out = json::parse(text, {.jsonc = false});
    return out ? *out : json::value{};
}

const suite stream_suite = [] {
    "capability_translated_streams_pass_the_guard_with_no_repair_at_all"_test = [] {
        auto reasoning = std::vector<wire_event>{
            event("response.created", created),
            event("response.output_item.added", R"(,"output_index":0,"item":{"type":"reasoning","id":"rs_1","summary":[]})"),
            event("response.reasoning_summary_text.delta", R"(,"output_index":0,"delta":"x")"),
            event("response.output_item.done", R"(,"output_index":0,"item":{"type":"reasoning","id":"rs_1","encrypted_content":"E"})"),
            event("response.output_item.added", R"(,"output_index":1,"item":{"type":"message","id":"m"})"),
            event("response.output_text.delta", R"(,"output_index":1,"delta":"hi")"),
            event("response.output_item.done", R"(,"output_index":1,"item":{"type":"message","id":"m"})"),
            event("response.completed", usage_120_100_9)};

        for (auto const& upstream : {text_stream(), tool_stream(), reasoning}) {
            anthropic::stream_guard guard;
            std::size_t repairs = 0;
            std::size_t forwarded = 0;
            for (auto const& item : translate(upstream)) {
                guard.on_event(sse::event{.name = item.name, .data = item.data, .id = {}}, [&](auto const&) { ++forwarded; }, [&](auto const&) { ++repairs; });
            }
            guard.on_end([&](auto const&) { ++forwarded; }, [&](auto const&) { ++repairs; });

            expect(repairs == 0_u && forwarded > 0_u);
        }
    };

    "capability_a_text_stream_becomes_blocks_with_the_usage_split_into_input_and_cache_read"_test = [] {
        auto const out = translate(text_stream());

        expect(names(out) == "message_start ping content_block_start content_block_delta content_block_delta content_block_stop message_delta message_stop") << names(out);
        expect(out[0].data.contains(R"("model":"claude-sonnet-5-5")") && out[0].data.contains(R"("id":"msg_resp_1")"));
        expect(out[2].data == R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})");
        expect(out[3].data == R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"Hel"}})");
        expect(out[6].data == R"({"type":"message_delta","delta":{"stop_reason":"end_turn","stop_sequence":null},"usage":{"input_tokens":20,"output_tokens":9,"cache_read_input_tokens":100,"cache_creation_input_tokens":0}})")
            << out[6].data;
        grammar_holds(out);
    };

    "capability_a_tool_call_streams_its_arguments_and_ends_the_turn_with_tool_use"_test = [] {
        auto const out = translate(tool_stream());

        expect(names(out) == "message_start content_block_start content_block_delta content_block_delta content_block_stop message_delta message_stop") << names(out);
        expect(out[1].data == R"({"type":"content_block_start","index":0,"content_block":{"type":"tool_use","id":"call_1","name":"Bash","input":{}}})");
        expect(out[2].data == R"({"type":"content_block_delta","index":0,"delta":{"type":"input_json_delta","partial_json":"{\"command\":"}})");
        expect(out[5].data.contains(R"("stop_reason":"tool_use")"));
        grammar_holds(out);
    };

    "capability_two_parallel_calls_and_text_before_them_get_consecutive_block_indexes"_test = [] {
        auto upstream = text_stream();
        upstream.pop_back();
        for (auto const index : {1, 2}) {
            auto const call = std::format(R"("output_index":{},"item":{{"type":"function_call","id":"fc_{}","call_id":"call_{}","name":"T","arguments":"{{}}"}})", index, index, index);
            upstream.push_back(event("response.output_item.added", "," + call));
            upstream.push_back(event("response.output_item.done", "," + call));
        }
        upstream.push_back(event("response.completed", usage_120_100_9));

        auto const out = translate(upstream);
        std::vector<std::string> starts;
        for (auto const& item : out) {
            if (item.name == "content_block_start") {
                starts.push_back(item.data.substr(0, 45));
            }
        }

        expect(starts.size() == 3_u);
        expect(starts[0].contains(R"("index":0)") && starts[1].contains(R"("index":1)") && starts[2].contains(R"("index":2)"));
        grammar_holds(out);
    };

    "capability_a_call_whose_arguments_come_only_with_the_done_event_still_sends_them"_test = [] {
        auto upstream = tool_stream();
        std::erase_if(upstream, [](wire_event const& item) { return item.name == "response.function_call_arguments.delta" || item.name == "response.function_call_arguments.done"; });

        auto const out = translate(upstream);

        expect(names(out) == "message_start content_block_start content_block_delta content_block_stop message_delta message_stop") << names(out);
        expect(out[2].data.contains(R"("partial_json":"{\"command\":\"ls\"}")"));
        grammar_holds(out);
    };

    "capability_reasoning_becomes_a_thinking_block_whose_signature_restores_the_item"_test = [] {
        std::vector<wire_event> const upstream{
            event("response.created", created),
            event("response.output_item.added", R"(,"output_index":0,"item":{"type":"reasoning","id":"rs_1","summary":[]})"),
            event("response.reasoning_summary_text.delta", R"(,"output_index":0,"summary_index":0,"delta":"weigh ")"),
            event("response.reasoning_summary_text.delta", R"(,"output_index":0,"summary_index":0,"delta":"options")"),
            event("response.output_item.done", R"(,"output_index":0,"item":{"type":"reasoning","id":"rs_1","encrypted_content":"ENC","summary":[{"type":"summary_text","text":"weigh options"}]})"),
            event("response.completed", usage_120_100_9)};

        auto const out = translate(upstream);

        expect(names(out) == "message_start content_block_start content_block_delta content_block_delta content_block_delta content_block_stop message_delta message_stop") << names(out);
        expect(out[1].data == R"({"type":"content_block_start","index":0,"content_block":{"type":"thinking","thinking":"","signature":""}})");
        expect(out[2].data.contains(R"("thinking_delta")") && out[2].data.contains("weigh "));
        auto const signature = json::text_at(parsed(out[4].data), {"delta", "signature"});
        expect(signature.starts_with(responses::signature_prefix));
        grammar_holds(out);

        auto const history = std::format(R"({{"model":"m","messages":[{{"role":"assistant","content":[{{"type":"thinking","thinking":"x","signature":{}}}]}}]}})", json::write(json::make_string(signature)));
        auto const back = responses::translate_request(history, {});
        expect(back.has_value() && back->body.contains(R"("encrypted_content":"ENC")") && back->body.contains(R"("type":"reasoning")"));
    };

    "capability_an_incomplete_response_maps_its_reason_to_a_stop_reason"_test = [] {
        auto const stop = [](std::string_view const reason) {
            auto upstream = text_stream();
            upstream.back() = event("response.incomplete", std::format(R"(,"response":{{"incomplete_details":{{"reason":"{}"}},"usage":{{"input_tokens":5,"output_tokens":2}}}})", reason));
            auto const out = translate(upstream);
            grammar_holds(out);
            return json::text_at(parsed(out[out.size() - 2].data), {"delta", "stop_reason"});
        };

        expect(stop("max_output_tokens") == "max_tokens");
        expect(stop("content_filter") == "refusal");
        expect(stop("something_new") == "end_turn");
    };

    "limit_a_failed_response_and_an_error_event_end_the_stream_with_an_anthropic_error"_test = [] {
        auto upstream = text_stream();
        upstream.resize(5);
        upstream.push_back(event("response.failed", R"(,"response":{"error":{"code":"rate_limit_exceeded","message":"slow down"}})"));

        auto const failed = translate(upstream);
        auto const plain = translate({event("error", R"(,"code":"server_error","message":"boom")")});

        expect(failed.back().name == "error");
        expect(failed.back().data == R"({"type":"error","error":{"type":"rate_limit_error","message":"slow down"}})") << failed.back().data;
        expect(plain.back().data == R"({"type":"error","error":{"type":"api_error","message":"boom"}})") << plain.back().data;
    };

    "limit_nothing_follows_the_end_of_a_response"_test = [] {
        auto upstream = text_stream();
        upstream.push_back(event("response.output_text.delta", R"(,"output_index":0,"delta":"late")"));

        auto const out = translate(upstream);

        expect(out.back().name == "message_stop");
    };

    "capability_a_stream_that_starts_without_response_created_still_opens_with_message_start"_test = [] {
        auto upstream = text_stream();
        upstream.erase(upstream.begin(), upstream.begin() + 2);

        auto const out = translate(upstream);

        expect(out.front().name == "message_start" && out.front().data.contains(R"("id":"msg_llmgate")"));
        grammar_holds(out);
    };

    "capability_unknown_events_become_pings_and_are_counted_by_type"_test = [] {
        responses::stream_translator translator{"m"};
        auto upstream = text_stream();
        upstream.insert(upstream.begin() + 3, event("response.future_thing.delta"));
        upstream.insert(upstream.begin() + 3, event("response.future_thing.delta"));

        auto const out = translate(upstream, 0, &translator);

        expect(translator.ignored().at("response.future_thing.delta") == 2_u);
        expect(std::ranges::count(out, std::string{"ping"}, &wire_event::name) >= 3);
        grammar_holds(out);
    };

    "capability_the_result_does_not_depend_on_how_the_bytes_are_chunked"_test = [] {
        auto const whole = translate(tool_stream());

        for (auto const chunk : {1uz, 3uz, 7uz, 50uz}) {
            auto const pieces = translate(tool_stream(), chunk);
            expect(names(pieces) == names(whole)) << chunk;
            expect(pieces.size() == whole.size() && pieces.back().data == whole.back().data) << chunk;
        }
    };

    "limit_missing_usage_counts_as_zero_and_missing_cache_details_as_no_cache"_test = [] {
        auto upstream = text_stream();
        upstream.back() = event("response.completed", R"(,"response":{"usage":{"input_tokens":7,"output_tokens":3}})");

        auto const out = translate(upstream);

        expect(out[out.size() - 2].data.contains(R"("input_tokens":7,"output_tokens":3,"cache_read_input_tokens":0)")) << out[out.size() - 2].data;
        upstream.back() = event("response.completed");
        expect(translate(upstream)[translate(upstream).size() - 2].data.contains(R"("input_tokens":0,"output_tokens":0)"));
    };

    "capability_an_upstream_error_status_and_body_become_an_anthropic_error"_test = [] {
        auto const body = R"({"error":{"message":"Rate limit reached","type":"rate_limit_error","code":"rate_limit_exceeded"}})";

        expect(responses::error_body(429, body) == R"({"type":"error","error":{"type":"rate_limit_error","message":"Rate limit reached"}})");
        expect(responses::error_body(401, R"({"error":{"message":"bad token"}})") == R"({"type":"error","error":{"type":"authentication_error","message":"bad token"}})");
        expect(responses::error_body(403, R"({"error":{"message":"no plan"}})").contains("permission_error"));
        expect(responses::error_body(400, R"({"error":{"message":"bad"}})").contains("invalid_request_error"));
        expect(responses::error_body(404, "{}").contains("not_found_error"));
        expect(responses::error_body(503, "<html>").contains(R"("type":"api_error")"));
        expect(responses::error_body(500, "").contains("the upstream answered 500"));
    };

    "limit_a_cached_count_above_the_input_count_never_makes_input_negative"_test = [] {
        auto upstream = text_stream();
        upstream.back() = event("response.completed", R"(,"response":{"usage":{"input_tokens":5,"input_tokens_details":{"cached_tokens":9},"output_tokens":3}})");

        auto const out = translate(upstream);

        expect(out[out.size() - 2].data.contains(R"("input_tokens":0,"output_tokens":3,"cache_read_input_tokens":9)")) << out[out.size() - 2].data;
    };
};

}  // namespace

int main() {
    return boost::ut::cfg<>.run() ? 1 : 0;
}
