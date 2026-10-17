export module llmgate.properties;

import std;
import llmgate.anthropic;
import llmgate.json;
import llmgate.jsonscan;
import llmgate.sse;

export namespace llmgate::properties {

constexpr std::size_t piece_whole = std::numeric_limits<std::size_t>::max();

struct sse_record {
    std::string name;
    std::string data;
    std::string id;
    constexpr bool operator==(sse_record const&) const = default;
};

constexpr std::vector<sse_record> parse_sse(std::string_view const text, std::size_t const piece) {
    sse::parser reader;
    std::vector<sse_record> records;
    auto collect = [&records](sse::event const& item) {
        records.push_back({.name = std::string{item.name}, .data = std::string{item.data}, .id = std::string{item.id}});
    };
    for (std::size_t at = 0; at < text.size(); at += std::min(piece, text.size() - at)) {
        reader.feed(text.substr(at, std::min(piece, text.size() - at)), collect);
    }
    reader.finish(collect);
    return records;
}

constexpr bool sse_holds(std::string_view const text) {
    auto const whole = parse_sse(text, piece_whole);
    if (!std::ranges::equal(whole, parse_sse(text, 1))) {
        return false;
    }
    return std::ranges::all_of(whole, [](sse_record const& record) {
        std::string wire;
        sse::write_event(wire, record.name, record.data);
        auto const again = parse_sse(wire, piece_whole);
        return again.size() == 1 && again.front().name == record.name && again.front().data == record.data;
    });
}

constexpr bool jsonscan_holds(std::string_view const text) {
    auto const quoted = jsonscan::encode_string(text);
    if (jsonscan::decode_string(std::string_view{quoted}.substr(1, quoted.size() - 2)) != text) {
        return false;
    }
    (void)jsonscan::decode_string(text);
    auto const members = jsonscan::top_level_members(text);
    if (!members) {
        return true;
    }
    for (auto const& item : *members) {
        if (item.value_begin > item.value_end || item.value_end > text.size()) {
            return false;
        }
    }
    auto const replaced = jsonscan::replace_string_member(text, "model", "REPLACED");
    if (!replaced) {
        return true;
    }
    auto const again = jsonscan::top_level_members(*replaced);
    if (!again || again->size() != members->size()) {
        return false;
    }
    auto const targets = jsonscan::members_named(*again, "model");
    return targets.size() == jsonscan::members_named(*members, "model").size() &&
           std::ranges::all_of(targets, [&replaced](jsonscan::member const& item) {
               return std::string_view{*replaced}.substr(item.value_begin, item.value_end - item.value_begin) == "\"REPLACED\"";
           });
}

constexpr bool same(json::value const& left, json::value const& right);

constexpr bool same_members(json::object const& left, json::object const& right) {
    return std::ranges::equal(left, right, [](auto const& a, auto const& b) { return a.first == b.first && same(a.second, b.second); });
}

constexpr bool same(json::value const& left, json::value const& right) {
    if (left.data.index() != right.data.index()) {
        return false;
    }
    if (auto const* a = std::get_if<json::array>(&left.data)) {
        auto const& b = std::get<json::array>(right.data);
        return std::ranges::equal(*a, b, [](auto const& x, auto const& y) { return same(x, y); });
    }
    if (auto const* a = std::get_if<json::object>(&left.data)) {
        return same_members(*a, std::get<json::object>(right.data));
    }
    return std::visit(
        [&right](auto const& scalar) -> bool {
            using type = std::decay_t<decltype(scalar)>;
            if constexpr (std::is_same_v<type, json::array> || std::is_same_v<type, json::object>) {
                return false;
            } else {
                return scalar == std::get<type>(right.data);
            }
        },
        left.data);
}

struct sample_config {
    std::string name{};
    std::int32_t count = 0;
    bool flag = false;
    std::vector<std::string> tags{};
    std::optional<std::int64_t> limit{};
};

constexpr bool json_holds(std::string_view const text) {
    auto const lenient = json::parse(text);
    auto const strict = json::parse(text, {.jsonc = false});
    if (strict && (!lenient || !same(*strict, *lenient))) {
        return false;
    }
    if (lenient) {
        (void)json::read<sample_config>(*lenient);
    }
    return true;
}

constexpr bool guard_holds_for(std::string_view const text) {
    sse::parser reader;
    anthropic::stream_guard guard;
    std::vector<anthropic::output_event> out;
    auto emit = [&out](anthropic::output_event const& item) { out.push_back(item); };
    auto ignore = [](anthropic::repair const&) {};
    auto feed = [&](sse::event const& item) { guard.on_event(item, emit, ignore); };
    reader.feed(text, feed);
    reader.finish(feed);
    guard.on_end(emit, ignore);
    return guard.complete() && anthropic::validate_grammar(out).has_value();
}

enum class token : std::uint8_t {
    start,
    block_start_0,
    block_start_1,
    delta_0,
    delta_1,
    stop_0,
    stop_1,
    message_delta,
    message_delta_without_usage,
    message_stop,
    ping,
    error,
    junk,
    unknown,
};

constexpr std::string_view text_of(token const kind) {
    switch (kind) {
        case token::start: return "event: message_start\ndata: {\"type\":\"message_start\"}\n\n";
        case token::block_start_0: return "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":0}\n\n";
        case token::block_start_1: return "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":1}\n\n";
        case token::delta_0: return "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0}\n\n";
        case token::delta_1: return "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":1}\n\n";
        case token::stop_0: return "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":0}\n\n";
        case token::stop_1: return "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":1}\n\n";
        case token::message_delta: return "event: message_delta\ndata: {\"type\":\"message_delta\",\"usage\":{}}\n\n";
        case token::message_delta_without_usage: return "event: message_delta\ndata: {\"type\":\"message_delta\"}\n\n";
        case token::message_stop: return "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n";
        case token::ping: return "event: ping\ndata: {\"type\":\"ping\"}\n\n";
        case token::error: return "event: error\ndata: {\"type\":\"error\"}\n\n";
        case token::junk: return "event: message_delta\ndata: {not json\n\n";
        case token::unknown: break;
    }
    return "event: x_future\ndata: {\"type\":\"x_future\"}\n\n";
}

constexpr std::size_t token_count = static_cast<std::size_t>(token::unknown) + 1;

constexpr bool guard_holds_for_tokens(std::span<std::size_t const> const digits) {
    std::string wire;
    for (auto const digit : digits) {
        wire += text_of(static_cast<token>(digit));
    }
    return guard_holds_for(wire);
}

template <class Property>
constexpr bool for_all_sequences(std::size_t const alphabet, std::size_t const longest, Property&& property) {
    std::vector<std::size_t> digits;
    for (std::size_t length = 0; length <= longest; ++length) {
        digits.assign(length, 0);
        for (;;) {
            if (!property(std::span<std::size_t const>{digits})) {
                return false;
            }
            std::size_t place = 0;
            while (place < length && ++digits[place] == alphabet) {
                digits[place++] = 0;
            }
            if (place == length) {
                break;
            }
        }
    }
    return true;
}

constexpr bool for_all_texts(std::string_view const alphabet, std::size_t const longest, bool (*property)(std::string_view)) {
    return for_all_sequences(alphabet.size(), longest, [&](std::span<std::size_t const> const digits) {
        std::string text;
        for (auto const digit : digits) {
            text.push_back(alphabet[digit]);
        }
        return property(text);
    });
}

}  // namespace llmgate::properties
