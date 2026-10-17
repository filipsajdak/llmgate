export module llmgate.anthropic;

import std;
import llmgate.jsonscan;
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

struct repair {
    repair_kind kind;
    std::string detail;
};

struct output_event {
    std::string name;
    std::string data;
};

enum class event_type { message_start, content_block_start, content_block_delta, content_block_stop, message_delta, message_stop, ping, error, other };

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

constexpr auto known_types = std::to_array<std::pair<std::string_view, event_type>>({
    {"message_start", event_type::message_start},
    {"content_block_start", event_type::content_block_start},
    {"content_block_delta", event_type::content_block_delta},
    {"content_block_stop", event_type::content_block_stop},
    {"message_delta", event_type::message_delta},
    {"message_stop", event_type::message_stop},
    {"ping", event_type::ping},
    {"error", event_type::error},
});

constexpr event_type type_from(std::string_view const name) {
    auto const match = std::ranges::find(known_types, name, &std::pair<std::string_view, event_type>::first);
    return match == known_types.end() ? event_type::other : match->second;
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

constexpr std::string name_of(event_type const type) {
    switch (type) {
        case event_type::message_start: return "message_start";
        case event_type::content_block_start: return "content_block_start";
        case event_type::content_block_delta: return "content_block_delta";
        case event_type::content_block_stop: return "content_block_stop";
        case event_type::message_delta: return "message_delta";
        case event_type::message_stop: return "message_stop";
        case event_type::ping: return "ping";
        case event_type::error: return "error";
        case event_type::other: break;
    }
    return "";
}

constexpr std::string with_usage(std::string_view const data) {
    auto const close = data.rfind('}');
    std::string patched{data.substr(0, close)};
    patched += R"(,"usage":{"output_tokens":0}})";
    return patched;
}

}  // namespace llmgate::anthropic::detail

export namespace llmgate::anthropic {

class stream_guard {
public:
    template <std::invocable<output_event const&> Out, std::invocable<repair const&> Report>
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

    template <std::invocable<output_event const&> Out, std::invocable<repair const&> Report>
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
    template <class Out, class Report>
    constexpr void handle(detail::parsed const& parsed, sse::event const& incoming, Out& out, Report& report) {
        using enum event_type;
        if (parsed.type == ping || parsed.type == other) {
            forward(parsed, incoming, out);
            return;
        }
        if (parsed.type == error) {
            stopped_ = true;
            forward(parsed, incoming, out);
            return;
        }
        if (parsed.type == message_start) {
            start_message(parsed, incoming, out, report);
            return;
        }
        ensure_started(out, report);
        switch (parsed.type) {
            case content_block_start: open_block(parsed, incoming, out, report); break;
            case content_block_delta: continue_block(parsed, incoming, out, report); break;
            case content_block_stop: close_block(parsed, incoming, out, report); break;
            case message_delta: finish_delta(parsed, incoming, out, report); break;
            default: finish_message(incoming, out, report); break;
        }
    }

    template <class Out>
    static constexpr void forward(detail::parsed const& parsed, sse::event const& incoming, Out& out) {
        auto const name = incoming.name.empty() ? detail::name_of(parsed.type) : std::string{incoming.name};
        out(output_event{.name = name, .data = std::string{incoming.data}});
    }

    template <class Out, class Report>
    constexpr void start_message(detail::parsed const& parsed, sse::event const& incoming, Out& out, Report& report) {
        if (started_) {
            report(repair{.kind = repair_kind::duplicate_message_start, .detail = {}});
            return;
        }
        started_ = true;
        forward(parsed, incoming, out);
    }

    template <class Out, class Report>
    constexpr void ensure_started(Out& out, Report& report) {
        if (!std::exchange(started_, true)) {
            report(repair{.kind = repair_kind::missing_message_start, .detail = {}});
            out(output_event{.name = "message_start", .data = std::string{detail::synthesized_message_start}});
        }
    }

    template <class Out, class Report>
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

    template <class Out, class Report>
    constexpr void continue_block(detail::parsed const& parsed, sse::event const& incoming, Out& out, Report& report) {
        if (!parsed.index || !open_.contains(*parsed.index)) {
            report(repair{.kind = repair_kind::orphan_block_event, .detail = "content_block_delta"});
            return;
        }
        forward(parsed, incoming, out);
    }

    template <class Out, class Report>
    constexpr void close_block(detail::parsed const& parsed, sse::event const& incoming, Out& out, Report& report) {
        if (!parsed.index || open_.erase(*parsed.index) == 0) {
            report(repair{.kind = repair_kind::orphan_block_event, .detail = "content_block_stop"});
            return;
        }
        forward(parsed, incoming, out);
    }

    template <class Out, class Report>
    constexpr void close_index(int const index, Out& out, Report& report) {
        open_.erase(index);
        report(repair{.kind = repair_kind::unclosed_block, .detail = detail::decimal(index)});
        out(output_event{.name = "content_block_stop", .data = R"({"type":"content_block_stop","index":)" + detail::decimal(index) + "}"});
    }

    template <class Out, class Report>
    constexpr void close_all(Out& out, Report& report) {
        while (!open_.empty()) {
            close_index(open_.first(), out, report);
        }
    }

    template <class Out, class Report>
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

    template <class Out, class Report>
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
    detail::index_set open;
    bool started = false;
    bool delta = false;
    bool stopped = false;
    for (auto const& item : events) {
        auto const parsed = detail::classify(item.data);
        if (!parsed) {
            return std::unexpected{"unparseable event data"};
        }
        if (stopped) {
            return std::unexpected{"event after the stream ended"};
        }
        using enum event_type;
        switch (parsed->type) {
            case ping: case other: break;
            case error: stopped = true; break;
            case message_start:
                if (std::exchange(started, true)) return std::unexpected{"second message_start"};
                break;
            case content_block_start:
                if (!started || !parsed->index || !open.insert(*parsed->index) || delta) return std::unexpected{"bad content_block_start"};
                break;
            case content_block_delta:
                if (!parsed->index || !open.contains(*parsed->index)) return std::unexpected{"delta outside an open block"};
                break;
            case content_block_stop:
                if (!parsed->index || open.erase(*parsed->index) == 0) return std::unexpected{"stop for a block that is not open"};
                break;
            case message_delta:
                if (!started || !open.empty() || std::exchange(delta, true)) return std::unexpected{"bad message_delta"};
                if (!parsed->has_usage) return std::unexpected{"message_delta without usage"};
                break;
            case message_stop:
                if (!delta) return std::unexpected{"message_stop before message_delta"};
                stopped = true;
                break;
        }
    }
    return {};
}

}  // namespace llmgate::anthropic
