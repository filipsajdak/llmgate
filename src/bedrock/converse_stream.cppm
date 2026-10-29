export module llmgate.converse_stream;

import std;
import llmgate.chat_request;
import llmgate.json;
import llmgate.sse;
import llmgate.text;

namespace llmgate::converse::detail {

constexpr std::string_view fallback_message_id = "llmgate";
constexpr std::string_view no_output_message = "the upstream returned no output";
constexpr std::string_view unparsed_label = "unparsed_event";
constexpr std::string_view unknown_label = "unknown_event";

enum class block_kind { text, tool, thinking };

struct produced_event {
    std::string name{};
    std::string data{};
};

struct counts {
    std::uint64_t input = 0;
    std::uint64_t output = 0;
    std::uint64_t cache_read = 0;
    std::uint64_t cache_write = 0;
};

constexpr json::value text_value(std::string_view const text) {
    return json::make_string(std::string{text});
}

constexpr json::value count_value(std::uint64_t const count) {
    return json::value{static_cast<std::int64_t>(count)};
}

constexpr json::value event_of(std::string_view const type, std::initializer_list<std::pair<std::string, json::value>> const rest = {}) {
    json::object members{{"type", text_value(type)}};
    members.append_range(rest);
    return json::value{std::move(members)};
}

constexpr json::value block_event(std::string_view const type, std::size_t const index, json::value body, std::string_view const key) {
    return event_of(type, {{"index", count_value(index)}, {std::string{key}, std::move(body)}});
}

constexpr json::value delta_event(std::size_t const index, std::string_view const kind, std::string_view const field, std::string_view const text) {
    return block_event("content_block_delta", index, json::make_object({{"type", text_value(kind)}, {std::string{field}, text_value(text)}}), "delta");
}

constexpr std::string_view stop_reason_of(std::string_view const reason) {
    constexpr std::array<std::pair<std::string_view, std::string_view>, 5> known{
        {{"tool_use", "tool_use"}, {"max_tokens", "max_tokens"}, {"stop_sequence", "stop_sequence"}, {"guardrail_intervened", "refusal"}, {"content_filtered", "refusal"}}};
    auto const found = std::ranges::find(known, reason, &std::pair<std::string_view, std::string_view>::first);
    return found == known.end() ? "end_turn" : found->second;
}

constexpr counts counts_of(json::value const& metadata) {
    return {.input = json::count_at(metadata, {"usage", "inputTokens"}).value_or(0),
            .output = json::count_at(metadata, {"usage", "outputTokens"}).value_or(0),
            .cache_read = json::count_at(metadata, {"usage", "cacheReadInputTokens"}).value_or(0),
            .cache_write = json::count_at(metadata, {"usage", "cacheWriteInputTokens"}).value_or(0)};
}

constexpr json::value usage_json(counts const& seen) {
    return json::make_object({{"input_tokens", count_value(seen.input)},
                              {"output_tokens", count_value(seen.output)},
                              {"cache_read_input_tokens", count_value(seen.cache_read)},
                              {"cache_creation_input_tokens", count_value(seen.cache_write)}});
}

}  // namespace llmgate::converse::detail

export namespace llmgate::converse {

class stream_translator {
public:
    stream_translator(std::string model, std::vector<chat::renamed_name> renamed) : model_{std::move(model)}, renamed_{std::move(renamed)} {}

    template <sse::event_sink Sink>
    void on_event(std::string_view const event_type, std::string_view const payload, Sink&& out) {
        accept(event_type, payload);
        flush(out);
    }

    template <sse::event_sink Sink>
    void finish(Sink&& out) {
        if (!finished_ && started_ && !reason_.empty()) {
            conclude();
        }
        flush(out);
    }

    std::map<std::string, std::uint64_t> const& ignored() const noexcept { return ignored_; }

private:
    using handler = void (stream_translator::*)(json::value const&);

    template <sse::event_sink Sink>
    void flush(Sink& out) {
        for (auto const& item : produced_) {
            out(sse::event{.name = item.name, .data = item.data, .id = {}});
        }
        produced_.clear();
    }

    void put(std::string_view const name, json::value const& body) { produced_.push_back({.name = std::string{name}, .data = json::write(body)}); }

    void begin() {
        if (started_) {
            return;
        }
        started_ = true;
        std::string id{"msg_"};
        id += detail::fallback_message_id;
        auto const usage = json::make_object({{"input_tokens", detail::count_value(0)}, {"output_tokens", detail::count_value(0)}});
        put("message_start", detail::event_of("message_start", {{"message", json::make_object({{"id", detail::text_value(id)},
                                                                                              {"type", detail::text_value("message")},
                                                                                              {"role", detail::text_value("assistant")},
                                                                                              {"model", detail::text_value(model_)},
                                                                                              {"content", json::value{json::array{}}},
                                                                                              {"stop_reason", json::value{nullptr}},
                                                                                              {"stop_sequence", json::value{nullptr}},
                                                                                              {"usage", usage}})}}));
    }

    std::string original_name(std::string_view const used) const {
        auto const found = std::ranges::find(renamed_, used, &chat::renamed_name::used);
        return found == renamed_.end() ? std::string{used} : found->original;
    }

    void open(std::size_t const index, detail::block_kind const kind, json::value body) {
        begin();
        opened_any_ = true;
        open_[index] = {.kind = kind, .fragments = false};
        put("content_block_start", detail::block_event("content_block_start", index, std::move(body), "content_block"));
    }

    void ensure(std::size_t const index, detail::block_kind const kind) {
        if (open_.contains(index)) {
            return;
        }
        auto const thinking = kind == detail::block_kind::thinking;
        open(index, kind,
             thinking ? json::make_object({{"type", detail::text_value("thinking")}, {"thinking", detail::text_value("")}, {"signature", detail::text_value("")}})
                      : json::make_object({{"type", detail::text_value("text")}, {"text", detail::text_value("")}}));
    }

    void close(std::size_t const index) {
        auto const found = open_.find(index);
        if (found == open_.end()) {
            return;
        }
        if (found->second.kind == detail::block_kind::tool && !found->second.fragments) {
            put("content_block_delta", detail::delta_event(index, "input_json_delta", "partial_json", "{}"));
        }
        put("content_block_stop", detail::event_of("content_block_stop", {{"index", detail::count_value(index)}}));
        open_.erase(found);
    }

    void on_message_start(json::value const&) { begin(); }

    void on_block_start(json::value const& body) {
        auto const index = json::count_at(body, {"contentBlockIndex"}).value_or(0);
        auto const* tool = json::at(body, {"start", "toolUse"});
        if (tool == nullptr) {
            return;
        }
        open(index, detail::block_kind::tool,
             json::make_object({{"type", detail::text_value("tool_use")},
                                {"id", detail::text_value(json::text_at(*tool, {"toolUseId"}))},
                                {"name", detail::text_value(original_name(json::text_at(*tool, {"name"})))},
                                {"input", json::make_object({})}}));
    }

    void reasoning_delta(std::size_t const index, json::value const& reasoning) {
        ensure(index, detail::block_kind::thinking);
        if (auto const text = json::text_at(reasoning, {"text"}); !text.empty()) {
            put("content_block_delta", detail::delta_event(index, "thinking_delta", "thinking", text));
        }
        if (auto const signature = json::text_at(reasoning, {"signature"}); !signature.empty()) {
            put("content_block_delta", detail::delta_event(index, "signature_delta", "signature", signature));
        }
    }

    void on_block_delta(json::value const& body) {
        auto const index = json::count_at(body, {"contentBlockIndex"}).value_or(0);
        if (auto const text = json::text_at(body, {"delta", "text"}); !text.empty()) {
            ensure(index, detail::block_kind::text);
            put("content_block_delta", detail::delta_event(index, "text_delta", "text", text));
        } else if (auto const* reasoning = json::at(body, {"delta", "reasoningContent"})) {
            reasoning_delta(index, *reasoning);
        } else if (auto const fragment = json::text_at(body, {"delta", "toolUse", "input"}); open_.contains(index) && !fragment.empty()) {
            open_[index].fragments = true;
            put("content_block_delta", detail::delta_event(index, "input_json_delta", "partial_json", fragment));
        }
    }

    void on_block_stop(json::value const& body) { close(json::count_at(body, {"contentBlockIndex"}).value_or(0)); }

    void on_message_stop(json::value const& body) { reason_ = json::text_at(body, {"stopReason"}); }

    void on_metadata(json::value const& body) {
        seen_ = detail::counts_of(body);
        conclude();
    }

    void conclude() {
        begin();
        while (!open_.empty()) {
            close(open_.begin()->first);
        }
        if (!opened_any_) {
            put("error", detail::event_of("error", {{"error", json::make_object({{"type", detail::text_value("api_error")}, {"message", detail::text_value(detail::no_output_message)}})}}));
            finished_ = true;
            return;
        }
        put("message_delta", detail::event_of("message_delta", {{"delta", json::make_object({{"stop_reason", detail::text_value(detail::stop_reason_of(reason_))}, {"stop_sequence", json::value{nullptr}}})},
                                                                {"usage", detail::usage_json(seen_)}}));
        put("message_stop", detail::event_of("message_stop"));
        finished_ = true;
    }

    static constexpr std::array<std::pair<std::string_view, handler>, 6> handlers{{{"messageStart", &stream_translator::on_message_start},
                                                                                  {"contentBlockStart", &stream_translator::on_block_start},
                                                                                  {"contentBlockDelta", &stream_translator::on_block_delta},
                                                                                  {"contentBlockStop", &stream_translator::on_block_stop},
                                                                                  {"messageStop", &stream_translator::on_message_stop},
                                                                                  {"metadata", &stream_translator::on_metadata}}};

    void accept(std::string_view const event_type, std::string_view const payload) {
        if (finished_) {
            return;
        }
        auto const body = json::parse(payload, {.jsonc = false});
        if (!body || std::get_if<json::object>(&body->data) == nullptr) {
            ++ignored_[std::string{detail::unparsed_label}];
            return;
        }
        auto const found = std::ranges::find(handlers, event_type, &std::pair<std::string_view, handler>::first);
        if (found == handlers.end()) {
            ++ignored_[std::string{detail::unknown_label}];
            return;
        }
        (this->*found->second)(*body);
    }

    struct open_block {
        detail::block_kind kind = detail::block_kind::text;
        bool fragments = false;
    };

    std::string model_;
    std::vector<chat::renamed_name> renamed_;
    bool started_ = false;
    bool finished_ = false;
    bool opened_any_ = false;
    std::string reason_{};
    detail::counts seen_{};
    std::map<std::size_t, open_block> open_{};
    std::map<std::string, std::uint64_t> ignored_{};
    std::vector<detail::produced_event> produced_{};
};

}  // namespace llmgate::converse
