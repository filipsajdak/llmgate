module;

#include <meta>

export module llmgate.responses_stream;

import std;
import llmgate.json;
import llmgate.members;
import llmgate.reflect;
import llmgate.responses_request;
import llmgate.sse;
import llmgate.text;

namespace llmgate::responses::detail {

enum class upstream_event {
    created,
    in_progress,
    output_item_added,
    output_item_done,
    content_part_added,
    content_part_done,
    output_text_delta,
    output_text_done,
    function_call_arguments_delta,
    function_call_arguments_done,
    reasoning_summary_part_added,
    reasoning_summary_part_done,
    reasoning_summary_text_delta,
    reasoning_summary_text_done,
    completed,
    incomplete,
    failed,
    error
};
static_assert(reflect::unique_values<upstream_event>());

enum class item_kind { message, function_call, reasoning };
static_assert(reflect::unique_values<item_kind>());

enum class block_type { text, thinking };

constexpr std::string_view upstream_prefix = "response.";
constexpr std::string_view fallback_message_id = "llmgate";
constexpr std::string_view empty_arguments = "{}";

constexpr std::string normalized(std::string_view name) {
    if (name.starts_with(upstream_prefix)) {
        name.remove_prefix(upstream_prefix.size());
    }
    std::string out{name};
    std::ranges::replace(out, '.', '_');
    return out;
}

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

constexpr std::string_view error_type_of(std::string_view const code) {
    if (code == "rate_limit_exceeded") {
        return "rate_limit_error";
    }
    return code == "context_length_exceeded" || code == "invalid_prompt" || code == "invalid_request_error" ? "invalid_request_error" : "api_error";
}

constexpr std::string_view stop_reason_of(std::string_view const reason) {
    if (reason == "max_output_tokens") {
        return "max_tokens";
    }
    return reason == "content_filter" ? "refusal" : "end_turn";
}

constexpr json::value usage_of(json::value const& response) {
    auto const input = json::count_at(response, {"usage", "input_tokens"}).value_or(0);
    auto const cached = json::count_at(response, {"usage", "input_tokens_details", "cached_tokens"}).value_or(0);
    auto const output = json::count_at(response, {"usage", "output_tokens"}).value_or(0);
    return json::make_object({{"input_tokens", count_value(input > cached ? input - cached : 0)},
                              {"output_tokens", count_value(output)},
                              {"cache_read_input_tokens", count_value(cached)},
                              {"cache_creation_input_tokens", count_value(0)}});
}

struct open_block {
    std::size_t index = 0;
    block_type kind = block_type::text;
};

struct pending_call {
    std::string call_id{};
    std::string name{};
    std::string arguments{};
};

constexpr std::string_view dropped_call_label = "dropped_incomplete_tool_call";
constexpr std::string_view output_limit_message = "the upstream stopped at its output limit before producing any output";
constexpr std::string_view no_output_message = "the upstream returned no output";

struct handles {
    upstream_event event;
};

struct produced_event {
    std::string name{};
    std::string data{};
};

constexpr auto status_error_types = std::to_array<std::pair<int, std::string_view>>(
    {{400, "invalid_request_error"}, {401, "authentication_error"}, {403, "permission_error"}, {404, "not_found_error"}, {429, "rate_limit_error"}});

constexpr std::string_view error_type_for_status(int const status) {
    auto const found = std::ranges::find(status_error_types, status, &std::pair<int, std::string_view>::first);
    return found == status_error_types.end() ? "api_error" : found->second;
}

struct plan_error {
    std::string_view code;
    std::string_view type;
    std::string_view next_step;
    bool names_param;
};

constexpr auto plan_errors = std::to_array<plan_error>(
    {{"subscription_sharing_usage_limit_exceeded", "rate_limit_error", "the ChatGPT plan's usage limit is reached: check ChatGPT settings, then Usage, for when it resets", false},
     {"subscription_sharing_user_not_eligible", "permission_error", "this ChatGPT account is not eligible to share its plan with tools (a region or policy restriction)", false},
     {"subscription_sharing_invalid_user", "authentication_error", "the ChatGPT sign-in is no longer valid: run llmgate login --provider NAME again", false},
     {"subscription_sharing_usage_unavailable", "api_error", "ChatGPT plan usage is temporarily unavailable: try again shortly", false},
     {"subscription_sharing_user_unavailable", "api_error", "the ChatGPT account is temporarily unavailable: try again shortly", false},
     {"subscription_sharing_route_not_supported", "permission_error", "the plan route does not support this endpoint or method", false},
     {"subscription_sharing_unsupported_capability", "invalid_request_error", "the plan route does not support something in this request, remove it", true}});

constexpr std::optional<plan_error> plan_error_for(std::string_view const code) {
    auto const found = std::ranges::find(plan_errors, code, &plan_error::code);
    return found == plan_errors.end() ? std::nullopt : std::optional<plan_error>{*found};
}

}  // namespace llmgate::responses::detail

export namespace llmgate::responses {

constexpr std::string error_body(int const status, std::string_view const upstream_body) {
    auto const parsed = json::parse(upstream_body, {.jsonc = false});
    std::string message;
    std::string_view type = detail::error_type_for_status(status);
    if (parsed) {
        message = json::text_at(*parsed, {"error", "message"});
        if (auto const known = detail::plan_error_for(json::text_at(*parsed, {"error", "code"}))) {
            type = known->type;
            message = known->next_step;
            if (auto const param = json::text_at(*parsed, {"error", "param"}); known->names_param && !param.empty()) {
                text::append_all(message, " (", param, ")");
            }
        }
    }
    if (message.empty()) {
        text::append_all(message, "the upstream answered ", text::decimal(status));
    }
    return json::write(detail::event_of("error", {{"error", json::make_object({{"type", detail::text_value(type)}, {"message", detail::text_value(message)}})}}));
}

class stream_translator {
public:
    explicit stream_translator(std::string model) : model_{std::move(model)} {}

    template <sse::event_sink Sink>
    void on_event(sse::event const& incoming, Sink&& out) {
        accept(incoming);
        for (auto const& item : produced_) {
            out(sse::event{.name = item.name, .data = item.data, .id = {}});
        }
        produced_.clear();
    }

    std::map<std::string, std::uint64_t> const& ignored() const noexcept { return ignored_; }

private:
    constexpr void accept(sse::event const& incoming);
    constexpr void dispatch(detail::upstream_event kind, json::value const& data);

    void put(std::string_view const name, json::value const& body) { produced_.push_back({.name = std::string{name}, .data = json::write(body)}); }

    void ping() { put("ping", detail::event_of("ping")); }

    static std::uint64_t output_index(json::value const& data) { return json::count_at(data, {"output_index"}).value_or(0); }

    void begin(std::string_view const response_id = {}) {
        if (started_) {
            return;
        }
        started_ = true;
        std::string id{"msg_"};
        id += response_id.empty() ? detail::fallback_message_id : response_id;
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

    void open(std::uint64_t const at, detail::block_type const kind, json::value start) {
        begin();
        blocks_[at] = {.index = next_index_++, .kind = kind};
        put("content_block_start", detail::block_event("content_block_start", blocks_[at].index, std::move(start), "content_block"));
    }

    void close(std::uint64_t const at) {
        if (auto const found = blocks_.find(at); found != blocks_.end()) {
            put("content_block_stop", detail::event_of("content_block_stop", {{"index", detail::count_value(found->second.index)}}));
            blocks_.erase(found);
        }
    }

    void remember_call(std::uint64_t const at, json::value const& data) {
        auto id = json::text_at(data, {"item", "call_id"});
        if (id.empty()) {
            id = json::text_at(data, {"item", "id"});
        }
        calls_[at] = {.call_id = std::move(id), .name = json::text_at(data, {"item", "name"}), .arguments = json::text_at(data, {"item", "arguments"})};
    }

    void open_thinking(std::uint64_t const at) {
        open(at, detail::block_type::thinking, json::make_object({{"type", detail::text_value("thinking")}, {"thinking", detail::text_value("")}, {"signature", detail::text_value("")}}));
    }

    void drop_call(std::uint64_t const at) {
        calls_.erase(at);
        ++ignored_[std::string{detail::dropped_call_label}];
    }

    void emit_call(detail::pending_call const& call) {
        auto const index = next_index_++;
        saw_call_ = true;
        put("content_block_start", detail::block_event("content_block_start", index,
                                                       json::make_object({{"type", detail::text_value("tool_use")}, {"id", detail::text_value(call.call_id)},
                                                                          {"name", detail::text_value(call.name)}, {"input", json::make_object({})}}),
                                                       "content_block"));
        put("content_block_delta", detail::delta_event(index, "input_json_delta", "partial_json", call.arguments));
        put("content_block_stop", detail::event_of("content_block_stop", {{"index", detail::count_value(index)}}));
    }

    void finish_tool(std::uint64_t const at, json::value const& data) {
        begin();
        if (!calls_.contains(at)) {
            remember_call(at, data);
        }
        auto& call = calls_[at];
        if (auto const final_text = json::text_at(data, {"item", "arguments"}); !final_text.empty()) {
            call.arguments = final_text;
        }
        if (call.arguments.empty()) {
            call.arguments = detail::empty_arguments;
        }
        auto const parsed = json::parse(call.arguments, {.jsonc = false});
        if (!parsed || std::get_if<json::object>(&parsed->data) == nullptr) {
            drop_call(at);
            return;
        }
        emit_call(call);
        calls_.erase(at);
    }

    void finish_reasoning(std::uint64_t const at, json::value const& data) {
        if (!blocks_.contains(at)) {
            open_thinking(at);
        }
        std::string signature{signature_prefix};
        if (auto const* item = json::at(data, {"item"})) {
            signature += json::write(*item);
        }
        put("content_block_delta", detail::delta_event(blocks_[at].index, "signature_delta", "signature", signature));
        close(at);
    }

    void end_message(json::value const& data, std::string_view const reason) {
        begin();
        while (!blocks_.empty()) {
            close(blocks_.begin()->first);
        }
        if (!calls_.empty()) {
            ignored_[std::string{detail::dropped_call_label}] += calls_.size();
            calls_.clear();
        }
        if (next_index_ == 0) {
            fail("", reason == "max_tokens" ? detail::output_limit_message : detail::no_output_message);
            return;
        }
        auto const* response = json::at(data, {"response"});
        auto const usage = response == nullptr ? detail::usage_of(json::value{}) : detail::usage_of(*response);
        put("message_delta",
            detail::event_of("message_delta", {{"delta", json::make_object({{"stop_reason", detail::text_value(saw_call_ ? "tool_use" : reason)}, {"stop_sequence", json::value{nullptr}}})},
                                               {"usage", usage}}));
        put("message_stop", detail::event_of("message_stop"));
        finished_ = true;
    }

    void fail(std::string_view const code, std::string_view const message) {
        put("error", detail::event_of("error", {{"error", json::make_object({{"type", detail::text_value(detail::error_type_of(code))}, {"message", detail::text_value(message)}})}}));
        finished_ = true;
    }

    [[=detail::handles{detail::upstream_event::created}]] void on_created(json::value const& data) { begin(json::text_at(data, {"response", "id"})); }

    [[=detail::handles{detail::upstream_event::in_progress}]] void on_in_progress(json::value const& data) {
        begin(json::text_at(data, {"response", "id"}));
        ping();
    }

    [[=detail::handles{detail::upstream_event::output_item_added}]] void on_item_added(json::value const& data) {
        begin();
        auto const kind = reflect::enum_from_name<detail::item_kind>(json::text_at(data, {"item", "type"}));
        if (kind == detail::item_kind::function_call) {
            remember_call(output_index(data), data);
        } else if (kind == detail::item_kind::reasoning) {
            open_thinking(output_index(data));
        }
    }

    [[=detail::handles{detail::upstream_event::output_item_done}]] void on_item_done(json::value const& data) {
        auto const at = output_index(data);
        switch (reflect::enum_from_name<detail::item_kind>(json::text_at(data, {"item", "type"})).value_or(detail::item_kind::message)) {
            case detail::item_kind::function_call: finish_tool(at, data); break;
            case detail::item_kind::reasoning: finish_reasoning(at, data); break;
            case detail::item_kind::message: close(at); break;
        }
    }

    [[=detail::handles{detail::upstream_event::output_text_delta}]] void on_text_delta(json::value const& data) {
        auto const at = output_index(data);
        if (!blocks_.contains(at)) {
            open(at, detail::block_type::text, json::make_object({{"type", detail::text_value("text")}, {"text", detail::text_value("")}}));
        }
        put("content_block_delta", detail::delta_event(blocks_[at].index, "text_delta", "text", json::text_at(data, {"delta"})));
    }

    [[=detail::handles{detail::upstream_event::function_call_arguments_delta}]] void on_arguments_delta(json::value const& data) {
        if (auto const found = calls_.find(output_index(data)); found != calls_.end()) {
            found->second.arguments += json::text_at(data, {"delta"});
        }
    }

    [[=detail::handles{detail::upstream_event::reasoning_summary_text_delta}]] void on_summary_delta(json::value const& data) {
        auto const at = output_index(data);
        if (!blocks_.contains(at)) {
            open_thinking(at);
        }
        put("content_block_delta", detail::delta_event(blocks_[at].index, "thinking_delta", "thinking", json::text_at(data, {"delta"})));
    }

    [[=detail::handles{detail::upstream_event::completed}]] void on_completed(json::value const& data) { end_message(data, detail::stop_reason_of({})); }

    [[=detail::handles{detail::upstream_event::incomplete}]] void on_incomplete(json::value const& data) {
        end_message(data, detail::stop_reason_of(json::text_at(data, {"response", "incomplete_details", "reason"})));
    }

    [[=detail::handles{detail::upstream_event::failed}]] void on_failed(json::value const& data) {
        fail(json::text_at(data, {"response", "error", "code"}), json::text_at(data, {"response", "error", "message"}));
    }

    [[=detail::handles{detail::upstream_event::error}]] void on_error(json::value const& data) { fail(json::text_at(data, {"code"}), json::text_at(data, {"message"})); }

    std::string model_;
    bool started_ = false;
    bool finished_ = false;
    bool saw_call_ = false;
    std::size_t next_index_ = 0;
    std::map<std::uint64_t, detail::open_block> blocks_{};
    std::map<std::uint64_t, detail::pending_call> calls_{};
    std::map<std::string, std::uint64_t> ignored_{};
    std::vector<detail::produced_event> produced_{};
};

}  // namespace llmgate::responses

namespace llmgate::responses {

consteval std::span<std::meta::info const> rule_members() {
    return std::define_static_array(std::meta::members_of(^^stream_translator, std::meta::access_context::unchecked()) |
                                    std::views::filter([](std::meta::info const member) { return std::meta::is_function(member) && std::meta::has_identifier(member) && reflect::has_annotation<detail::handles>(member); }));
}

constexpr auto ruleless_events =
    std::to_array({detail::upstream_event::content_part_added, detail::upstream_event::content_part_done, detail::upstream_event::output_text_done,
                   detail::upstream_event::function_call_arguments_done, detail::upstream_event::reasoning_summary_part_added,
                   detail::upstream_event::reasoning_summary_part_done, detail::upstream_event::reasoning_summary_text_done});

consteval bool rules_cover_events() {
    auto const handled = rule_members() | std::views::transform([](std::meta::info const member) { return reflect::annotation_of<detail::handles>(member)->event; });
    return std::ranges::all_of(reflect::enum_values<detail::upstream_event>, [&](detail::upstream_event const type) {
        return std::ranges::count(handled, type) + std::ranges::count(ruleless_events, type) == 1;
    });
}
static_assert(rules_cover_events());

constexpr void stream_translator::dispatch(detail::upstream_event const kind, json::value const& data) {
    template for (constexpr auto member : rule_members()) {
        if (reflect::annotation_of<detail::handles>(member)->event == kind) {
            this->[:member:](data);
        }
    }
}

constexpr void stream_translator::accept(sse::event const& incoming) {
    if (finished_) {
        return;
    }
    auto const data = json::parse(incoming.data, {.jsonc = false});
    auto const type = data ? json::text_at(*data, {"type"}) : std::string{};
    auto const name = type.empty() ? std::string{incoming.name} : type;
    auto const kind = reflect::enum_from_name<detail::upstream_event>(detail::normalized(name));
    if (!data || !kind) {
        ++ignored_[name];
        ping();
        return;
    }
    dispatch(*kind, *data);
}

}  // namespace llmgate::responses
