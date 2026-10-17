export module llmgate.properties;

import std;
import llmgate.anthropic;
import llmgate.arguments;
import llmgate.statusline;
import llmgate.usage;
import llmgate.claude;
import llmgate.json;
import llmgate.jsonscan;
import llmgate.routing;
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
        auto const compact = json::write(*lenient);
        auto const pretty = json::write(*lenient, 2);
        auto const again = json::parse(compact, {.jsonc = false});
        auto const again_pretty = json::parse(pretty, {.jsonc = false});
        if (!again || !again_pretty || !same(*lenient, *again) || !same(*lenient, *again_pretty) || json::write(*again) != compact) {
            return false;
        }
    }
    return true;
}

constexpr std::string_view alias_name = "opus";
constexpr std::string_view mapped_name = "mapped";

constexpr std::optional<std::string> sample_lookup(std::string const& requested) {
    return requested == alias_name ? std::optional<std::string>{std::string{mapped_name}} : std::nullopt;
}

constexpr bool routing_holds(std::string_view const text) {
    if (auto const url = routing::parse_url(text)) {
        std::string again = url->secure ? "https://" : "http://";
        again += url->host;
        again += ':';
        std::array<char, 8> digits{};
        again.append(digits.data(), std::to_chars(digits.data(), digits.data() + digits.size(), url->port).ptr);
        again += url->base_path;
        auto const reparsed = routing::parse_url(again);
        if (!reparsed || reparsed->host != url->host || reparsed->port != url->port || reparsed->secure != url->secure ||
            reparsed->base_path != url->base_path) {
            return false;
        }
    }
    (void)routing::route_of("POST", text);
    if (!routing::secrets_equal(text, text) || routing::secrets_equal(text, std::string{text} + "x") ||
        routing::secrets_equal(text, "other") != routing::secrets_equal("other", text)) {
        return false;
    }
    (void)routing::is_loopback_host(text);
    auto const challenge = routing::query_value(text, "challenge");
    if (!challenge.empty() && (!text.contains(challenge) || challenge.contains('&'))) {
        return false;
    }
    (void)routing::valid_challenge(text);
    auto const allowed = routing::default_allowed_hosts(text, 9480);
    if (routing::host_allowed(text, allowed) != std::ranges::contains(allowed, std::string{text}) &&
        !std::ranges::any_of(allowed, [&text](std::string const& candidate) { return candidate.size() == text.size(); })) {
        return false;
    }
    if (routing::path_of(text).contains('?')) {
        return false;
    }
    auto const prepared = routing::prepare(text, &sample_lookup);
    if (!prepared) {
        return true;
    }
    auto const members = jsonscan::top_level_members(prepared->body);
    if (!members) {
        return false;
    }
    auto const models = jsonscan::members_named(*members, "model");
    for (auto const& item : models) {
        auto const raw = std::string_view{prepared->body}.substr(item.value_begin + 1, item.value_end - item.value_begin - 2);
        if (jsonscan::decode_string(raw) == alias_name) {
            return false;
        }
    }
    return true;
}

constexpr bool settings_holds(std::string_view const text) {
    auto const parsed = json::parse(text, {.jsonc = false});
    if (!parsed) {
        return true;
    }
    auto const once = claude::merge_settings(*parsed, "medium", "s");
    auto const twice = claude::merge_settings(once, "medium", "s");
    if (json::write(once) != json::write(twice)) {
        return false;
    }
    auto const* original = std::get_if<json::object>(&parsed->data);
    auto const* merged = std::get_if<json::object>(&once.data);
    if (merged == nullptr) {
        return false;
    }
    if (original != nullptr) {
        for (auto const& [key, ignored] : *original) {
            auto const kept = std::ranges::any_of(*merged, [&key](auto const& entry) { return entry.first == key; });
            if (!kept) {
                return false;
            }
        }
    }
    if (!json::write(once).contains("Read(**/.env)")) {
        return false;
    }
    auto const env = claude::environment({}, "u", "k", "/p");
    auto const vscode_once = claude::merge_vscode(*parsed, env);
    return json::write(vscode_once) == json::write(claude::merge_vscode(vscode_once, env));
}

constexpr std::int64_t seconds_from(std::string_view const text) {
    std::uint64_t bits = 0;
    for (std::size_t index = 0; index < text.size() && index < 8; ++index) {
        bits = (bits << 8) | static_cast<unsigned char>(text[index]);
    }
    if (!text.empty()) {
        bits <<= (8 - std::min<std::size_t>(text.size(), 8)) * 8;
    }
    return static_cast<std::int64_t>(bits);
}

constexpr bool calendar_holds(std::int64_t const seconds) {
    auto const day = usage::day_of(seconds);
    auto const into_day = seconds - day * usage::seconds_per_day;
    auto const date = usage::civil_from_day(day);
    auto const month_slot = ((usage::month_of(day) % 12) + 12) % 12;
    return into_day >= 0 && into_day < usage::seconds_per_day && date.month >= 1 && date.month <= 12 && date.day >= 1 && date.day <= 31 &&
           month_slot == static_cast<std::int64_t>(date.month) - 1 && usage::civil_from_day(day + 1).day != date.day;
}

constexpr bool usage_holds(std::string_view const text) {
    usage::observer seen;
    seen.on_body(text);
    seen.on_event("message_start", text);
    seen.on_event("message_delta", text);
    (void)seen.used().counted();
    (void)usage::session_of(text);
    (void)usage::role_of(text);
    (void)usage::headline_from(text);
    if (auto const entry = usage::from_line(text)) {
        if (usage::from_line(usage::to_line(*entry)) != entry) {
            return false;
        }
        usage::ledger book;
        book.add(*entry);
        if (book.on_day(usage::day_of(entry->time)).requests != 1) {
            return false;
        }
    }
    auto const loaded = usage::load(text);
    auto const limit = static_cast<std::uint64_t>(text.size());
    auto const verdict = usage::evaluate({.daily_tokens = limit, .monthly_tokens = limit * 3, .warn_percent = 80}, limit / 2, limit * 5);
    if (verdict.percent > usage::percent_cap || (verdict.state == usage::level::exhausted) != (verdict.limit != 0 && verdict.percent >= 100)) {
        return false;
    }
    (void)usage::summary_json(loaded.loaded, {.daily_tokens = limit}, seconds_from(text));
    (void)usage::report(loaded.loaded, 0, 1);
    return calendar_holds(seconds_from(text)) && usage::compact(limit * limit).size() <= 24;
}

constexpr bool launcher_holds(std::string_view const text) {
    auto const snapshot = arguments::statusline_input(text);
    if (snapshot.context_percent < 0 || snapshot.context_percent > 1000) {
        return false;
    }
    auto const drawn = statusline::render(snapshot);
    if (!drawn.starts_with("[llmgate] ") || std::ranges::count(drawn, '\n') != 2) {
        return false;
    }
    auto const counters = arguments::counters_from(text);
    (void)counters;
    (void)statusline::branch_from_head(text);
    (void)statusline::base_name(text);
    if (auto const address = arguments::split_listen(text)) {
        auto const url = arguments::base_url(*address);
        auto const again = arguments::split_listen(address->host + ":" + claude::decimal(address->port));
        (void)url;
        return again.has_value() && again->host == address->host && again->port == address->port;
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
