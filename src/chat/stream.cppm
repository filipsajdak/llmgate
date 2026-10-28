export module llmgate.chat_stream;

import std;
import llmgate.blocks;
import llmgate.chat_request;
import llmgate.json;
import llmgate.sse;
import llmgate.text;

namespace llmgate::chat::detail {

constexpr std::string_view done_marker = "[DONE]";
constexpr std::string_view fallback_message_id = "llmgate";
constexpr std::string_view empty_arguments = "{}";
constexpr std::string_view dropped_call_label = "dropped_incomplete_tool_call";
constexpr std::string_view unparsed_label = "unparsed_chunk";
constexpr std::string_view output_limit_message = "the upstream stopped at its output limit before producing any output";
constexpr std::string_view no_output_message = "the upstream returned no output";

struct produced_event {
    std::string name{};
    std::string data{};
};

struct pending_call {
    std::string call_id{};
    std::string name{};
    std::string arguments{};
};

struct counts {
    std::uint64_t input = 0;
    std::uint64_t cached = 0;
    std::uint64_t output = 0;
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
    if (reason == "tool_calls" || reason == "function_call") {
        return "tool_use";
    }
    if (reason == "length") {
        return "max_tokens";
    }
    return reason == "content_filter" ? "refusal" : "end_turn";
}

constexpr std::optional<counts> counts_of(json::value const& chunk) {
    auto const* usage = json::at(chunk, {"usage"});
    if (usage == nullptr || std::get_if<json::object>(&usage->data) == nullptr) {
        return std::nullopt;
    }
    return counts{.input = json::count_at(*usage, {"prompt_tokens"}).value_or(0),
                  .cached = json::count_at(*usage, {"prompt_tokens_details", "cached_tokens"}).value_or(0),
                  .output = json::count_at(*usage, {"completion_tokens"}).value_or(0)};
}

constexpr json::value usage_json(counts const& seen) {
    return json::make_object({{"input_tokens", count_value(seen.input > seen.cached ? seen.input - seen.cached : 0)},
                              {"output_tokens", count_value(seen.output)},
                              {"cache_read_input_tokens", count_value(seen.cached)},
                              {"cache_creation_input_tokens", count_value(0)}});
}

constexpr auto error_codes = std::to_array<std::pair<std::string_view, std::string_view>>({{"rate_limit_exceeded", "rate_limit_error"},
                                                                                        {"context_length_exceeded", "invalid_request_error"},
                                                                                        {"invalid_prompt", "invalid_request_error"},
                                                                                        {"invalid_request_error", "invalid_request_error"},
                                                                                        {"content_filter", "invalid_request_error"}});

constexpr std::string_view error_type_of(std::string_view const code) {
    auto const found = std::ranges::find(error_codes, code, &std::pair<std::string_view, std::string_view>::first);
    return found == error_codes.end() ? "api_error" : found->second;
}

constexpr json::value const* first_choice(json::value const& chunk) {
    auto const* choices = json::at(chunk, {"choices"});
    auto const* list = choices == nullptr ? nullptr : std::get_if<json::array>(&choices->data);
    return list == nullptr || list->empty() ? nullptr : &list->front();
}

}  // namespace llmgate::chat::detail

export namespace llmgate::chat {

constexpr std::string error_body(int const status, std::string_view const upstream_body) {
    auto const parsed = json::parse(upstream_body, {.jsonc = false});
    auto message = parsed ? json::text_at(*parsed, {"error", "message"}) : std::string{};
    if (message.empty() && parsed) {
        message = json::text_at(*parsed, {"message"});
    }
    if (message.empty()) {
        text::append_all(message, "the upstream answered ", text::decimal(status));
    }
    return json::write(detail::event_of(
        "error", {{"error", json::make_object({{"type", detail::text_value(blocks::error_type_for_status(status))}, {"message", detail::text_value(message)}})}}));
}

class stream_translator {
public:
    stream_translator(std::string model, std::vector<renamed_name> renamed) : model_{std::move(model)}, renamed_{std::move(renamed)} {}

    template <sse::event_sink Sink>
    void on_event(sse::event const& incoming, Sink&& out) {
        accept(incoming);
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
    template <sse::event_sink Sink>
    void flush(Sink& out) {
        for (auto const& item : produced_) {
            out(sse::event{.name = item.name, .data = item.data, .id = {}});
        }
        produced_.clear();
    }

    void put(std::string_view const name, json::value const& body) { produced_.push_back({.name = std::string{name}, .data = json::write(body)}); }

    void begin(std::string_view const chunk_id) {
        if (started_) {
            return;
        }
        started_ = true;
        std::string id{"msg_"};
        id += chunk_id.empty() ? detail::fallback_message_id : chunk_id;
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

    void close_text() {
        if (text_block_) {
            put("content_block_stop", detail::event_of("content_block_stop", {{"index", detail::count_value(*text_block_)}}));
            text_block_.reset();
        }
    }

    void add_text(std::string_view const text) {
        if (!text_block_) {
            text_block_ = next_index_++;
            put("content_block_start", detail::block_event("content_block_start", *text_block_, json::make_object({{"type", detail::text_value("text")}, {"text", detail::text_value("")}}), "content_block"));
        }
        put("content_block_delta", detail::delta_event(*text_block_, "text_delta", "text", text));
    }

    void add_call_fragment(json::value const& fragment, std::size_t const position) {
        close_text();
        auto const at = json::count_at(fragment, {"index"}).value_or(position);
        auto& call = calls_[at];
        if (auto id = json::text_at(fragment, {"id"}); !id.empty()) {
            call.call_id = std::move(id);
        }
        if (auto name = json::text_at(fragment, {"function", "name"}); !name.empty()) {
            call.name = std::move(name);
        }
        call.arguments += json::text_at(fragment, {"function", "arguments"});
    }

    std::string original_name(std::string_view const used) const {
        auto const found = std::ranges::find(renamed_, used, &renamed_name::used);
        return found == renamed_.end() ? std::string{used} : found->original;
    }

    void emit_call(detail::pending_call const& call) {
        auto const index = next_index_++;
        saw_call_ = true;
        put("content_block_start", detail::block_event("content_block_start", index,
                                                       json::make_object({{"type", detail::text_value("tool_use")}, {"id", detail::text_value(call.call_id)},
                                                                          {"name", detail::text_value(original_name(call.name))}, {"input", json::make_object({})}}),
                                                       "content_block"));
        put("content_block_delta", detail::delta_event(index, "input_json_delta", "partial_json", call.arguments));
        put("content_block_stop", detail::event_of("content_block_stop", {{"index", detail::count_value(index)}}));
    }

    void emit_calls() {
        for (auto& [at, call] : calls_) {
            if (call.arguments.empty()) {
                call.arguments = detail::empty_arguments;
            }
            auto const parsed = json::parse(call.arguments, {.jsonc = false});
            if (!parsed || std::get_if<json::object>(&parsed->data) == nullptr || call.name.empty()) {
                ++ignored_[std::string{detail::dropped_call_label}];
                continue;
            }
            emit_call(call);
        }
        calls_.clear();
    }

    void conclude() {
        begin({});
        close_text();
        emit_calls();
        if (next_index_ == 0) {
            fail("", reason_ == "length" ? detail::output_limit_message : detail::no_output_message);
            return;
        }
        put("message_delta", detail::event_of("message_delta", {{"delta", json::make_object({{"stop_reason", detail::text_value(saw_call_ ? "tool_use" : detail::stop_reason_of(reason_))}, {"stop_sequence", json::value{nullptr}}})},
                                                                {"usage", detail::usage_json(seen_)}}));
        put("message_stop", detail::event_of("message_stop"));
        finished_ = true;
    }

    void fail(std::string_view const code, std::string_view const message) {
        put("error", detail::event_of("error", {{"error", json::make_object({{"type", detail::text_value(detail::error_type_of(code))}, {"message", detail::text_value(message)}})}}));
        finished_ = true;
    }

    void absorb_calls(json::value const& choice) {
        auto const* calls = json::at(choice, {"delta", "tool_calls"});
        auto const* list = calls == nullptr ? nullptr : std::get_if<json::array>(&calls->data);
        for (std::size_t position = 0; list != nullptr && position < list->size(); ++position) {
            add_call_fragment((*list)[position], position);
        }
    }

    void absorb(json::value const& chunk) {
        if (auto const found = detail::counts_of(chunk)) {
            seen_ = *found;
            have_usage_ = true;
        }
        auto const* choice = detail::first_choice(chunk);
        if (choice == nullptr) {
            return;
        }
        if (auto const text = json::text_at(*choice, {"delta", "content"}); !text.empty()) {
            add_text(text);
        }
        absorb_calls(*choice);
        if (auto reason = json::text_at(*choice, {"finish_reason"}); !reason.empty()) {
            reason_ = std::move(reason);
        }
    }

    void settle(std::size_t const produced_before) {
        if (!reason_.empty() && have_usage_) {
            conclude();
        } else if (produced_.size() == produced_before) {
            put("ping", detail::event_of("ping"));
        }
    }

    void take(json::value const& chunk) {
        if (json::at(chunk, {"error"}) != nullptr) {
            fail(json::text_at(chunk, {"error", "code"}), json::text_at(chunk, {"error", "message"}));
            return;
        }
        begin(json::text_at(chunk, {"id"}));
        auto const before = produced_.size();
        absorb(chunk);
        settle(before);
    }

    void accept(sse::event const& incoming) {
        if (finished_) {
            return;
        }
        if (incoming.data == detail::done_marker) {
            conclude();
            return;
        }
        auto const chunk = json::parse(incoming.data, {.jsonc = false});
        if (!chunk || std::get_if<json::object>(&chunk->data) == nullptr) {
            ++ignored_[std::string{detail::unparsed_label}];
            return;
        }
        take(*chunk);
    }

    std::string model_;
    std::vector<renamed_name> renamed_;
    bool started_ = false;
    bool finished_ = false;
    bool saw_call_ = false;
    bool have_usage_ = false;
    std::size_t next_index_ = 0;
    std::optional<std::size_t> text_block_{};
    std::map<std::size_t, detail::pending_call> calls_{};
    std::string reason_{};
    detail::counts seen_{};
    std::map<std::string, std::uint64_t> ignored_{};
    std::vector<detail::produced_event> produced_{};
};

}  // namespace llmgate::chat
