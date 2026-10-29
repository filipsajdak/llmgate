#include <boost/ut.hpp>

import llmgate.bedrock_request;
import llmgate.eventstream;
import llmgate.json;
import std;

namespace {

using namespace boost::ut;
namespace br = llmgate::bedrock;
namespace es = llmgate::eventstream;
namespace js = llmgate::json;

js::value parsed(std::string_view const text) {
    return std::move(*js::parse(text, {.jsonc = false}));
}

std::string text_of(js::value const& root, std::initializer_list<std::string_view> path) {
    return js::text_at(root, path);
}

br::translated translated_of(std::string_view const body, std::string_view const betas = "", br::options const& chosen = {}) {
    auto const done = br::translate_request(body, betas, chosen);
    return done ? *done : br::translated{};
}

std::string chunk_frame(std::string_view const event_json) {
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    for (std::size_t at = 0; at < event_json.size(); at += 3) {
        auto const left = std::min<std::size_t>(3, event_json.size() - at);
        std::uint32_t block = 0;
        for (std::size_t offset = 0; offset < 3; ++offset) {
            block = block << 8 | (offset < left ? static_cast<unsigned char>(event_json[at + offset]) : 0U);
        }
        for (std::size_t digit = 0; digit < 4; ++digit) {
            encoded.push_back(digit <= left ? alphabet[block >> (18 - 6 * digit) & 0x3F] : '=');
        }
    }
    std::vector<std::pair<std::string, std::string>> const headers{{":event-type", "chunk"}, {":content-type", "application/json"}, {":message-type", "event"}};
    return es::encode(headers, R"({"bytes":")" + encoded + R"("})");
}

std::string exception_frame(std::string_view const kind, std::string_view const message) {
    std::vector<std::pair<std::string, std::string>> const headers{{":exception-type", std::string{kind}}, {":content-type", "application/json"}, {":message-type", "exception"}};
    return es::encode(headers, R"({"message":")" + std::string{message} + R"("})");
}

std::vector<es::frame> decode_in_pieces(std::string const& wire, std::size_t const piece, es::failure* result = nullptr) {
    es::decoder reader;
    std::vector<es::frame> frames;
    es::failure last = es::failure::none;
    for (std::size_t at = 0; at < wire.size() && last == es::failure::none; at += piece) {
        last = reader.feed(std::string_view{wire}.substr(at, piece), [&frames](es::frame const& each) { frames.push_back(each); });
    }
    if (result != nullptr) {
        *result = last;
    }
    return frames;
}

suite<"bedrock_request"> request_tests = [] {
    "capability_the_body_loses_model_and_stream_gains_the_version_and_keeps_everything_else"_test = [] {
        auto const done = translated_of(R"({"model":"m","stream":true,"anthropic_version":"2023-06-01","max_tokens":9,"thinking":{"type":"adaptive"},"messages":[{"role":"user","content":"hi"}]})");
        auto const out = parsed(done.body);

        expect(done.client_streams && js::at(out, {"model"}) == nullptr && js::at(out, {"stream"}) == nullptr);
        expect(text_of(out, {"anthropic_version"}) == "bedrock-2023-05-31" && js::count_at(out, {"max_tokens"}) == std::optional<std::uint64_t>{9});
        expect(text_of(out, {"thinking", "type"}) == "adaptive" && text_of(out, {"messages"}).empty() && js::at(out, {"messages"}) != nullptr);
    };

    "capability_beta_tokens_move_into_the_body_and_only_allowed_ones_stay"_test = [] {
        auto const done = translated_of(R"({"model":"m","stream":true,"messages":[]})", "interleaved-thinking-2025-05-14, brand-new-2099-01-01 ,context-management-2025-06-27");
        auto const out = parsed(done.body);
        auto const& kept = std::get<js::array>(js::at(out, {"anthropic_beta"})->data);

        expect(kept.size() == 2_u && std::get<std::string>(kept.at(0).data) == "interleaved-thinking-2025-05-14");
        expect(done.dropped_betas == std::vector<std::string>{"brand-new-2099-01-01"});
    };

    "capability_a_configured_beta_list_replaces_the_built_in_one"_test = [] {
        auto const done = translated_of(R"({"model":"m","stream":true,"messages":[]})", "interleaved-thinking-2025-05-14,custom-1", {.betas = {"custom-1"}});

        expect(done.dropped_betas == std::vector<std::string>{"interleaved-thinking-2025-05-14"} && done.body.contains("custom-1"));
    };

    "capability_no_beta_header_means_no_beta_field"_test = [] {
        expect(!translated_of(R"({"model":"m","stream":true,"messages":[]})").body.contains("anthropic_beta"));
    };

    "capability_a_system_message_after_a_user_turn_joins_that_turn_and_one_after_an_assistant_turn_becomes_a_user_turn"_test = [] {
        auto const joined = parsed(translated_of(R"({"model":"m","stream":true,"messages":[{"role":"user","content":"a"},{"role":"system","content":[{"type":"text","text":"b","cache_control":{"type":"ephemeral"}}]}]})").body);
        auto const after_assistant = parsed(translated_of(R"({"model":"m","stream":true,"messages":[{"role":"user","content":"a"},{"role":"assistant","content":"x"},{"role":"system","content":"c"}]})").body);
        auto const& turns = std::get<js::array>(js::at(joined, {"messages"})->data);
        auto const& blocks = std::get<js::array>(js::at(turns.front(), {"content"})->data);
        auto const& later = std::get<js::array>(js::at(after_assistant, {"messages"})->data);

        expect(turns.size() == 1_u && blocks.size() == 2_u && text_of(blocks.at(0), {"text"}) == "a" && text_of(blocks.at(1), {"text"}) == "b" && js::at(blocks.at(1), {"cache_control"}) != nullptr);
        expect(later.size() == 3_u && text_of(later.at(2), {"role"}) == "user" && text_of(std::get<js::array>(js::at(later.at(2), {"content"})->data).front(), {"text"}) == "c");
    };

    "capability_a_leading_system_message_joins_the_system_prompt_whatever_its_shape"_test = [] {
        auto const with_prompt = parsed(translated_of(R"({"model":"m","stream":true,"system":"base","messages":[{"role":"system","content":"extra"},{"role":"user","content":"a"}]})").body);
        auto const without = parsed(translated_of(R"({"model":"m","stream":true,"messages":[{"role":"system","content":"extra"},{"role":"user","content":"a"}]})").body);
        auto const& merged = std::get<js::array>(js::at(with_prompt, {"system"})->data);

        expect(merged.size() == 2_u && text_of(merged.at(0), {"text"}) == "base" && text_of(merged.at(1), {"text"}) == "extra");
        expect(std::get<js::array>(js::at(without, {"system"})->data).size() == 1_u && std::get<js::array>(js::at(without, {"messages"})->data).size() == 1_u);
    };

    "limit_a_body_that_is_not_a_json_object_is_refused"_test = [] {
        expect(!br::translate_request("[]", "", {}).has_value() && !br::translate_request("{", "", {}).has_value());
    };
};

suite<"eventstream"> stream_tests = [] {
    "capability_the_crc_matches_the_standard_check_value"_test = [] {
        expect(es::crc32("123456789") == 0xCBF43926U);
    };

    "capability_frames_arrive_whole_or_a_byte_at_a_time_with_the_same_result"_test = [] {
        auto const wire = chunk_frame(R"({"type":"message_start"})") + chunk_frame(R"({"type":"content_block_delta","index":0})");
        auto const whole = decode_in_pieces(wire, wire.size());
        auto const bytewise = decode_in_pieces(wire, 1);

        expect(whole.size() == 2_u && bytewise.size() == 2_u && whole.at(0).payload == bytewise.at(0).payload);
        expect(whole.at(0).message_type == "event" && whole.at(0).event_type == "chunk");
    };

    "capability_a_chunk_becomes_the_messages_event_it_carries_under_its_own_type"_test = [] {
        auto const frames = decode_in_pieces(chunk_frame(R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"hi"}})"), 7);
        auto const made = es::event_of(frames.front());

        expect(made.has_value() && made->name == "content_block_delta" && made->data == R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"hi"}})");
    };

    "capability_an_exception_frame_becomes_an_error_event_of_the_matching_type"_test = [] {
        auto const throttled = es::event_of(decode_in_pieces(exception_frame("throttlingException", "slow down"), 5).front());
        auto const other = es::event_of(decode_in_pieces(exception_frame("modelStreamErrorException", "boom"), 5).front());

        expect(throttled->name == "error" && parsed(throttled->data).data.index() == parsed(R"({})").data.index());
        expect(text_of(parsed(throttled->data), {"error", "type"}) == "rate_limit_error" && text_of(parsed(throttled->data), {"error", "message"}) == "slow down");
        expect(text_of(parsed(other->data), {"error", "type"}) == "api_error");
    };

    "limit_a_corrupt_frame_stops_the_stream_and_is_named"_test = [] {
        auto wire = chunk_frame(R"({"type":"message_start"})");
        auto flipped_prelude = wire;
        flipped_prelude[9] = static_cast<char>(flipped_prelude[9] ^ 0x01);
        auto flipped_body = wire;
        flipped_body[20] = static_cast<char>(flipped_body[20] ^ 0x01);
        es::failure prelude = es::failure::none;
        es::failure message = es::failure::none;

        expect(decode_in_pieces(flipped_prelude, 4, &prelude).empty() && prelude == es::failure::prelude_crc);
        expect(decode_in_pieces(flipped_body, 4, &message).empty() && message == es::failure::message_crc);
    };

    "limit_an_oversized_frame_is_refused_before_it_is_buffered_and_a_truncated_one_is_reported_incomplete"_test = [] {
        std::string prelude;
        for (auto const value : {0x7FFFFFFFU, 0U}) {
            for (int shift = 24; shift >= 0; shift -= 8) {
                prelude.push_back(static_cast<char>((value >> shift) & 0xFFU));
            }
        }
        auto const check = es::crc32(prelude);
        for (int shift = 24; shift >= 0; shift -= 8) {
            prelude.push_back(static_cast<char>((check >> shift) & 0xFFU));
        }
        es::failure oversized = es::failure::none;
        decode_in_pieces(prelude, prelude.size(), &oversized);
        es::decoder cut;
        auto const wire = chunk_frame(R"({"type":"message_start"})");
        cut.feed(std::string_view{wire}.substr(0, wire.size() - 3), [](es::frame const&) {});

        expect(oversized == es::failure::too_large && cut.incomplete());
    };

    "capability_events_that_are_not_chunks_are_ignored"_test = [] {
        std::vector<std::pair<std::string, std::string>> const headers{{":event-type", "initial-response"}, {":message-type", "event"}};
        auto const frames = decode_in_pieces(es::encode(headers, "{}"), 3);

        expect(frames.size() == 1_u && !es::event_of(frames.front()).has_value());
    };
};

}  // namespace

int main() {}
