export module llmgate.anthropic;

import std;
import llmgate.jsonscan;
import llmgate.reflect;
import llmgate.sse;

export namespace llmgate::anthropic {

enum class repair_kind {
    duplicate_message_start,
    missing_message_start,
    orphan_block_event,
    unclosed_block,
    duplicate_message_delta,
    missing_message_delta,
    missing_usage,
    event_after_stop,
    unparseable_event,
    truncated_stream,
};
static_assert(reflect::unique_values<repair_kind>());

struct repair {
    repair_kind kind;
    std::string detail;
};

struct output_event {
    std::string name;
    std::string data;
};

enum class event_type { message_start, content_block_start, content_block_delta, content_block_stop, message_delta, message_stop, ping, error, other };
static_assert(reflect::unique_values<event_type>());

}  // namespace llmgate::anthropic

namespace llmgate::anthropic::detail {

constexpr std::string_view synthesized_message_start =
    R"({"type":"message_start","message":{"id":"msg_llmgate","type":"message","role":"assistant","model":"","content":[],"stop_reason":null,"stop_sequence":null,"usage":{"input_tokens":0,"output_tokens":0}}})";
constexpr std::string_view synthesized_message_delta =
    R"({"type":"message_delta","delta":{"stop_reason":"end_turn","stop_sequence":null},"usage":{"output_tokens":0}})";
constexpr std::string_view synthesized_message_stop = R"({"type":"message_stop"})";
constexpr std::string_view synthesized_error =
    R"({"type":"error","error":{"type":"api_error","message":"upstream stream ended before message_stop"}})";

struct parsed {
    event_type type = event_type::other;
    std::optional<int> index;
    bool has_usage = false;
};

constexpr event_type type_from(std::string_view const name) {
    return reflect::enum_from_name<event_type>(name).value_or(event_type::other);
}

constexpr std::string decimal(int const value) {
    std::array<char, 16> digits{};
    auto const end = std::to_chars(digits.data(), digits.data() + digits.size(), value).ptr;
    return std::string{digits.data(), end};
}

class index_set {
public:
    constexpr bool contains(int const index) const { return std::ranges::contains(items_, index); }
    constexpr bool empty() const noexcept { return items_.empty(); }
    constexpr int first() const { return items_.front(); }

    constexpr bool insert(int const index) {
        auto const place = std::ranges::lower_bound(items_, index);
        if (place != items_.end() && *place == index) {
            return false;
        }
        items_.insert(place, index);
        return true;
    }

    constexpr std::size_t erase(int const index) {
        auto const place = std::ranges::lower_bound(items_, index);
        if (place == items_.end() || *place != index) {
            return 0;
        }
        items_.erase(place);
        return 1;
    }

private:
    std::vector<int> items_;
};

constexpr std::optional<int> index_from(std::string_view const text) {
    int value = 0;
    auto const [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value < 0) {
        return std::nullopt;
    }
    return value;
}

constexpr std::optional<parsed> classify(std::string_view const data) {
    auto const members = jsonscan::top_level_members(data);
    if (!members) {
        return std::nullopt;
    }
    parsed result;
    for (auto const& item : jsonscan::members_named(*members, "type")) {
        if (item.kind == jsonscan::value_kind::string) {
            result.type = type_from(jsonscan::decode_string(data.substr(item.value_begin + 1, item.value_end - item.value_begin - 2)));
        }
    }
    for (auto const& item : jsonscan::members_named(*members, "index")) {
        result.index = index_from(data.substr(item.value_begin, item.value_end - item.value_begin));
    }
    result.has_usage = !jsonscan::members_named(*members, "usage").empty();
    return result;
}

constexpr bool is_standalone(event_type const type) noexcept {
    constexpr auto standalone = std::to_array({event_type::ping, event_type::other, event_type::error, event_type::message_start});
    return std::ranges::contains(standalone, type);
}

constexpr std::string name_of(event_type const type) {
    return type == event_type::other ? std::string{} : std::string{reflect::enum_name(type)};
}

constexpr std::string with_usage(std::string_view const data) {
    auto const close = data.rfind('}');
    std::string patched{data.substr(0, close)};
    patched += R"(,"usage":{"output_tokens":0}})";
    return patched;
}

using verdict = std::optional<std::string_view>;

struct grammar_state {
    index_set open;
    bool started = false;
    bool delta = false;
    bool stopped = false;

    constexpr verdict on_error(parsed const&) {
        stopped = true;
        return std::nullopt;
    }

    constexpr verdict on_message_start(parsed const&) {
        if (std::exchange(started, true)) {
            return "second message_start";
        }
        return std::nullopt;
    }

    constexpr verdict on_block_start(parsed const& event) {
        if (!started || !event.index || !open.insert(*event.index) || delta) {
            return "bad content_block_start";
        }
        return std::nullopt;
    }

    constexpr verdict on_block_delta(parsed const& event) {
        if (!event.index || !open.contains(*event.index)) {
            return "delta outside an open block";
        }
        return std::nullopt;
    }

    constexpr verdict on_block_stop(parsed const& event) {
        if (!event.index || open.erase(*event.index) == 0) {
            return "stop for a block that is not open";
        }
        return std::nullopt;
    }

    constexpr verdict on_message_delta(parsed const& event) {
        if (!started || !open.empty() || std::exchange(delta, true)) {
            return "bad message_delta";
        }
        if (!event.has_usage) {
            return "message_delta without usage";
        }
        return std::nullopt;
    }

    constexpr verdict on_message_stop(parsed const&) {
        if (!delta) {
            return "message_stop before message_delta";
        }
        stopped = true;
        return std::nullopt;
    }

    constexpr verdict accept(parsed const& event);
};

using grammar_rule = verdict (grammar_state::*)(parsed const&);

constexpr auto grammar_rules = std::to_array<std::pair<event_type, grammar_rule>>({
    {event_type::error, &grammar_state::on_error},
    {event_type::message_start, &grammar_state::on_message_start},
    {event_type::content_block_start, &grammar_state::on_block_start},
    {event_type::content_block_delta, &grammar_state::on_block_delta},
    {event_type::content_block_stop, &grammar_state::on_block_stop},
    {event_type::message_delta, &grammar_state::on_message_delta},
    {event_type::message_stop, &grammar_state::on_message_stop},
});

constexpr verdict grammar_state::accept(parsed const& event) {
    if (stopped) {
        return "event after the stream ended";
    }
    auto const rule = std::ranges::find(grammar_rules, event.type, &std::pair<event_type, grammar_rule>::first);
    return rule == grammar_rules.end() ? std::nullopt : (this->*rule->second)(event);
}

}  // namespace llmgate::anthropic::detail

export namespace llmgate::anthropic {

template <class Sink>
concept output_sink = std::invocable<Sink&, output_event>;

template <class Sink>
concept repair_sink = std::invocable<Sink&, repair>;

class stream_guard {
public:
    template <output_sink Out, repair_sink Report>
    constexpr void on_event(sse::event const& incoming, Out&& out, Report&& report) {
        auto const parsed = detail::classify(incoming.data);
        if (!parsed) {
            report(repair{.kind = repair_kind::unparseable_event, .detail = std::string{incoming.name}});
            return;
        }
        if (stopped_) {
            report(repair{.kind = repair_kind::event_after_stop, .detail = std::string{incoming.name}});
            return;
        }
        handle(*parsed, incoming, out, report);
    }

    template <output_sink Out, repair_sink Report>
    constexpr void on_end(Out&& out, Report&& report) {
        if (stopped_) {
            return;
        }
        stopped_ = true;
        report(repair{.kind = repair_kind::truncated_stream, .detail = {}});
        out(output_event{.name = "error", .data = std::string{detail::synthesized_error}});
    }

    constexpr bool complete() const noexcept { return stopped_; }

private:
    template <output_sink Out, repair_sink Report>
    constexpr void handle(detail::parsed const& parsed, sse::event const& incoming, Out& out, Report& report) {
        if (detail::is_standalone(parsed.type)) {
            handle_standalone(parsed, incoming, out, report);
            return;
        }
        ensure_started(out, report);
        handle_in_message(parsed, incoming, out, report);
    }

    template <output_sink Out, repair_sink Report>
    constexpr void handle_standalone(detail::parsed const& parsed, sse::event const& incoming, Out& out, Report& report) {
        if (parsed.type == event_type::message_start) {
            start_message(parsed, incoming, out, report);
            return;
        }
        stopped_ = parsed.type == event_type::error;
        forward(parsed, incoming, out);
    }

    template <output_sink Out, repair_sink Report>
    constexpr void handle_in_message(detail::parsed const& parsed, sse::event const& incoming, Out& out, Report& report) {
        using enum event_type;
        switch (parsed.type) {
            case content_block_start: open_block(parsed, incoming, out, report); break;
            case content_block_delta: continue_block(parsed, incoming, out, report); break;
            case content_block_stop: close_block(parsed, incoming, out, report); break;
            case message_delta: finish_delta(parsed, incoming, out, report); break;
            default: finish_message(incoming, out, report); break;
        }
    }

    template <output_sink Out>
    static constexpr void forward(detail::parsed const& parsed, sse::event const& incoming, Out& out) {
        auto const name = incoming.name.empty() ? detail::name_of(parsed.type) : std::string{incoming.name};
        out(output_event{.name = name, .data = std::string{incoming.data}});
    }

    template <output_sink Out, repair_sink Report>
    constexpr void start_message(detail::parsed const& parsed, sse::event const& incoming, Out& out, Report& report) {
        if (started_) {
            report(repair{.kind = repair_kind::duplicate_message_start, .detail = {}});
            return;
        }
        started_ = true;
        forward(parsed, incoming, out);
    }

    template <output_sink Out, repair_sink Report>
    constexpr void ensure_started(Out& out, Report& report) {
        if (!std::exchange(started_, true)) {
            report(repair{.kind = repair_kind::missing_message_start, .detail = {}});
            out(output_event{.name = "message_start", .data = std::string{detail::synthesized_message_start}});
        }
    }

    template <output_sink Out, repair_sink Report>
    constexpr void open_block(detail::parsed const& parsed, sse::event const& incoming, Out& out, Report& report) {
        if (!parsed.index || delta_seen_) {
            report(repair{.kind = repair_kind::orphan_block_event, .detail = "content_block_start outside the content phase"});
            return;
        }
        if (open_.contains(*parsed.index)) {
            close_index(*parsed.index, out, report);
        }
        open_.insert(*parsed.index);
        forward(parsed, incoming, out);
    }

    template <output_sink Out, repair_sink Report>
    constexpr void continue_block(detail::parsed const& parsed, sse::event const& incoming, Out& out, Report& report) {
        if (!parsed.index || !open_.contains(*parsed.index)) {
            report(repair{.kind = repair_kind::orphan_block_event, .detail = "content_block_delta"});
            return;
        }
        forward(parsed, incoming, out);
    }

    template <output_sink Out, repair_sink Report>
    constexpr void close_block(detail::parsed const& parsed, sse::event const& incoming, Out& out, Report& report) {
        if (!parsed.index || open_.erase(*parsed.index) == 0) {
            report(repair{.kind = repair_kind::orphan_block_event, .detail = "content_block_stop"});
            return;
        }
        forward(parsed, incoming, out);
    }

    template <output_sink Out, repair_sink Report>
    constexpr void close_index(int const index, Out& out, Report& report) {
        open_.erase(index);
        report(repair{.kind = repair_kind::unclosed_block, .detail = detail::decimal(index)});
        out(output_event{.name = "content_block_stop", .data = R"({"type":"content_block_stop","index":)" + detail::decimal(index) + "}"});
    }

    template <output_sink Out, repair_sink Report>
    constexpr void close_all(Out& out, Report& report) {
        while (!open_.empty()) {
            close_index(open_.first(), out, report);
        }
    }

    template <output_sink Out, repair_sink Report>
    constexpr void finish_delta(detail::parsed const& parsed, sse::event const& incoming, Out& out, Report& report) {
        if (delta_seen_) {
            report(repair{.kind = repair_kind::duplicate_message_delta, .detail = {}});
            return;
        }
        close_all(out, report);
        delta_seen_ = true;
        if (parsed.has_usage) {
            forward(parsed, incoming, out);
            return;
        }
        report(repair{.kind = repair_kind::missing_usage, .detail = {}});
        out(output_event{.name = "message_delta", .data = detail::with_usage(incoming.data)});
    }

    template <output_sink Out, repair_sink Report>
    constexpr void finish_message(sse::event const& incoming, Out& out, Report& report) {
        close_all(out, report);
        if (!std::exchange(delta_seen_, true)) {
            report(repair{.kind = repair_kind::missing_message_delta, .detail = {}});
            out(output_event{.name = "message_delta", .data = std::string{detail::synthesized_message_delta}});
        }
        stopped_ = true;
        out(output_event{.name = "message_stop", .data = std::string{incoming.data}});
    }

    detail::index_set open_;
    bool started_ = false;
    bool delta_seen_ = false;
    bool stopped_ = false;
};

constexpr std::expected<void, std::string> validate_grammar(std::span<output_event const> events) {
    detail::grammar_state state;
    for (auto const& item : events) {
        auto const parsed = detail::classify(item.data);
        if (!parsed) {
            return std::unexpected{"unparseable event data"};
        }
        if (auto const problem = state.accept(*parsed)) {
            return std::unexpected{std::string{*problem}};
        }
    }
    return {};
}

}  // namespace llmgate::anthropic
