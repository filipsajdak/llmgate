module;

#include <meta>

export module llmgate.arguments;

import std;
import llmgate.json;
import llmgate.members;
import llmgate.reflect;
import llmgate.statusline;
import llmgate.text;

export namespace llmgate::arguments {

struct tolerant {};

enum class command {
    serve,
    login,
    claude,
    env,
    setup,
    statusline [[= tolerant{}]],
    status,
    stop [[= tolerant{}]],
    usage,
    doctor [[= tolerant{}]],
    quota,
    use,
    targets,
    route,
    models,
    reload
};
static_assert(reflect::unique_values<command>());

constexpr unsigned default_days = 7;
constexpr unsigned longest_report_days = 3660;

constexpr std::size_t flag_capacity = 24;

struct flag {
    char word[flag_capacity];
};

constexpr std::size_t operand_capacity = 12;

struct operand {
    char text[operand_capacity];
};

struct applies_to {
    std::uint32_t mask = 0;

    constexpr applies_to() = default;
    constexpr applies_to(std::same_as<command> auto const... which) : mask{(... | (std::uint32_t{1} << std::to_underlying(which)))} {}

    constexpr bool covers(command const which) const {
        return (mask >> std::to_underlying(which) & 1U) != 0;
    }
};

enum class document { example, schema, config_table };
static_assert(reflect::unique_values<document>());

struct prints {
    document which;
};

struct limits {
    unsigned lowest = 0;
    unsigned highest = 0;
};

struct options {
    command which = command::serve;
    [[= flag{"--config"}]] [[= operand{"PATH"}]] std::string config_path = "~/.config/llmgate/config.jsonc";
    [[= flag{"--profile"}]] [[= operand{"NAME"}]] std::string profile{};
    [[= flag{"--check-config"}]] bool check_config = false;
    [[= flag{"--version"}]] bool show_version = false;
    [[= flag{"--print-example"}]] [[= prints{document::example}]] bool print_example = false;
    [[= flag{"--print-schema"}]] [[= prints{document::schema}]] bool print_schema = false;
    [[= flag{"--print-config-table"}]] [[= prints{document::config_table}]] bool print_config_table = false;
    [[= flag{"--json"}]] [[= applies_to{command::env, command::usage, command::doctor, command::quota, command::models}]] bool as_json = false;
    [[= flag{"--vscode"}]] [[= applies_to{command::env, command::setup}]] [[= operand{"FILE"}]] std::optional<std::string> vscode{};
    [[= flag{"--service"}]] [[= applies_to{command::setup}]] bool service = false;
    [[= flag{"--dry-run"}]] [[= applies_to{command::setup}]] bool dry_run = false;
    [[= flag{"--offline"}]] [[= applies_to{command::doctor}]] bool offline = false;
    [[= flag{"--days"}]] [[= operand{"N"}]] [[= applies_to{command::usage}]] [[= limits{1, longest_report_days}]] unsigned days = default_days;
    [[= flag{"--provider"}]] [[= applies_to{command::login, command::models}]] [[= operand{"NAME"}]] std::string provider{};
    [[= flag{"--session"}]] [[= applies_to{command::use, command::route}]] [[= operand{"ID"}]] std::string session{};
    [[= operand{"ARGS..."}]] [[= applies_to{command::claude, command::use, command::route}]] std::vector<std::string> passthrough{};
};

struct gateway_address {
    std::string host;
    std::uint16_t port;
};

constexpr std::optional<command> command_named(std::string_view const word) {
    return reflect::enum_from_name<command>(word);
}

}  // namespace llmgate::arguments

namespace llmgate::arguments::detail {

using step = std::expected<void, std::string>;

struct parse_state {
    std::span<std::string const> args;
    std::size_t index = 0;
    options parsed{};
    bool chosen = false;
};

constexpr std::unexpected<std::string> unknown_argument(std::string_view const word) {
    return std::unexpected{std::string{"unknown argument: "} + std::string{word}};
}

constexpr std::optional<std::string_view> following_word(parse_state const& state) {
    if (state.index + 1 >= state.args.size()) {
        return std::nullopt;
    }
    return std::string_view{state.args[state.index + 1]};
}

constexpr std::optional<unsigned> whole_number(std::string_view const given) {
    unsigned number = 0;
    auto const read = std::from_chars(given.data(), given.data() + given.size(), number);
    if (read.ec != std::errc{} || read.ptr != given.data() + given.size()) {
        return std::nullopt;
    }
    return number;
}

consteval flag flag_of(std::meta::info const member) {
    return reflect::annotation_of<flag>(member).value_or(flag{});
}

consteval operand operand_of(std::meta::info const member) {
    return reflect::annotation_of<operand>(member).value_or(operand{});
}

consteval applies_to applies_to_of(std::meta::info const member) {
    return reflect::annotation_of<applies_to>(member).value_or(applies_to{});
}

consteval limits limits_of(std::meta::info const member) {
    return reflect::annotation_of<limits>(member).value_or(limits{});
}

constexpr std::string number_text(unsigned const number) {
    std::array<char, 12> digits{};
    auto const end = std::to_chars(digits.data(), digits.data() + digits.size(), number).ptr;
    return std::string{digits.data(), end};
}

constexpr step read_operand(bool& slot, parse_state&, std::string_view, limits) {
    slot = true;
    return {};
}

constexpr step read_operand(std::optional<std::string>& slot, parse_state& state, std::string_view, limits) {
    auto const given = following_word(state);
    if (given && !given->starts_with("-") && !command_named(*given)) {
        slot = std::string{*given};
        ++state.index;
    } else {
        slot.emplace();
    }
    return {};
}

constexpr step read_operand(std::string& slot, parse_state& state, std::string_view const word, limits) {
    auto const given = following_word(state);
    if (!given) {
        return unknown_argument(word);
    }
    slot = *given;
    ++state.index;
    return {};
}

constexpr step read_operand(unsigned& slot, parse_state& state, std::string_view const word, limits const range) {
    auto const number = whole_number(following_word(state).value_or(std::string_view{}));
    if (!number || *number < range.lowest || *number > range.highest) {
        return std::unexpected{std::string{word} + " needs a number from " + number_text(range.lowest) + " to " + number_text(range.highest)};
    }
    slot = *number;
    ++state.index;
    return {};
}

inline constexpr auto option_members = std::define_static_array(std::meta::nonstatic_data_members_of(^^options, std::meta::access_context::current()));

constexpr std::optional<step> apply_flag(parse_state& state, std::string_view const word) {
    std::optional<step> outcome;
    template for (constexpr auto member : option_members) {
        constexpr auto marker = flag_of(member);
        if constexpr (marker.word[0] != '\0') {
            if (word == std::string_view{marker.word}) {
                outcome = read_operand(state.parsed.[:member:], state, word, limits_of(member));
            }
        }
    }
    return outcome;
}

constexpr step choose_command(parse_state& state, std::string_view const word) {
    auto const named = command_named(word);
    if (!named || state.chosen) {
        return unknown_argument(word);
    }
    state.parsed.which = *named;
    state.chosen = true;
    return {};
}

constexpr bool takes_words(command const which) {
    return which == command::use || which == command::route;
}

constexpr step consume(parse_state& state) {
    std::string_view const word = state.args[state.index];
    if (state.chosen && state.parsed.which == command::claude) {
        state.parsed.passthrough.emplace_back(word);
        return {};
    }
    if (auto const applied = apply_flag(state, word)) {
        return *applied;
    }
    if (state.chosen && takes_words(state.parsed.which) && !word.starts_with("-")) {
        state.parsed.passthrough.emplace_back(word);
        return {};
    }
    return choose_command(state, word);
}

template <class Slot>
inline constexpr bool optional_slot = false;

template <class Value>
inline constexpr bool optional_slot<std::optional<Value>> = true;

constexpr void append_operand(std::string& out, std::string_view const holder, bool const optional) {
    if (!holder.empty()) {
        text::append_all(out, optional ? " [" : " ", holder, optional ? "]" : "");
    }
}

template <std::meta::info member>
constexpr void append_usage_of(std::string& out, command const which) {
    constexpr auto marker = flag_of(member);
    constexpr auto holder_text = operand_of(member);
    constexpr bool holds_operand = holder_text.text[0] != '\0';
    std::string_view const holder{holder_text.text};
    constexpr auto scope = applies_to_of(member);
    constexpr bool named = marker.word[0] != '\0';
    if constexpr (named && scope.mask != 0) {
        if (scope.covers(which)) {
            text::append_all(out, " [", std::string_view{marker.word});
            append_operand(out, holder, optional_slot<typename [:std::meta::type_of(member):]>);
            text::append_all(out, "]");
        }
    } else if constexpr (!named && holds_operand) {
        if (scope.covers(which)) {
            text::append_all(out, " ", holder);
        }
    }
}

constexpr void append_command_usage(std::string& out, command const which) {
    text::append_all(out, reflect::enum_string(which));
    template for (constexpr auto member : option_members) {
        append_usage_of<member>(out, which);
    }
}

template <std::meta::info member>
constexpr void append_global_usage(std::string& out, bool const with_operand) {
    constexpr auto marker = flag_of(member);
    constexpr auto holder = operand_of(member);
    constexpr bool global = marker.word[0] != '\0' && applies_to_of(member).mask == 0;
    if constexpr (global) {
        if ((holder.text[0] != '\0') == with_operand) {
            text::append_all(out, with_operand ? " [" : " | ", std::string_view{marker.word}, with_operand ? " " : "",
                             std::string_view{holder.text}, with_operand ? "]" : "");
        }
    }
}

constexpr std::string usage_string() {
    std::string out = "usage: llmgate";
    template for (constexpr auto member : option_members) {
        append_global_usage<member>(out, true);
    }
    text::append_all(out, " [");
    for (auto const which : reflect::enum_values<command>) {
        text::append_all(out, which == reflect::enum_values<command>.front() ? "" : " | ");
        append_command_usage(out, which);
    }
    text::append_all(out, "]");
    template for (constexpr auto member : option_members) {
        append_global_usage<member>(out, false);
    }
    return out;
}

}  // namespace llmgate::arguments::detail

export namespace llmgate::arguments {

inline constexpr std::string_view usage_text = std::define_static_string(detail::usage_string());

template <class Marker>
constexpr bool command_marked(command const which) {
    static constexpr auto items = std::define_static_array(std::meta::enumerators_of(^^command));
    bool marked = false;
    template for (constexpr auto item : items) {
        if (which == [:item:]) {
            marked = reflect::has_annotation<Marker>(item);
        }
    }
    return marked;
}

constexpr std::optional<document> requested_document(options const& parsed) {
    std::optional<document> found;
    template for (constexpr auto member : detail::option_members) {
        constexpr auto marker = reflect::annotation_of<prints>(member);
        if constexpr (marker) {
            static_assert(std::same_as<typename [:std::meta::type_of(member):], bool>, "a prints flag is a switch");
            if (parsed.[:member:]) {
                found = found.value_or(marker->which);
            }
        }
    }
    return found;
}

constexpr std::expected<options, std::string> parse(std::span<std::string const> const args) {
    detail::parse_state state{.args = args};
    for (; state.index < args.size(); ++state.index) {
        if (auto const done = detail::consume(state); !done) {
            return std::unexpected{done.error()};
        }
    }
    return std::move(state.parsed);
}

constexpr std::optional<gateway_address> split_listen(std::string_view const listen) {
    auto const colon = listen.rfind(':');
    if (colon == std::string_view::npos || colon == 0) {
        return std::nullopt;
    }
    std::uint16_t port = 0;
    auto const digits = listen.substr(colon + 1);
    auto const parsed = std::from_chars(digits.data(), digits.data() + digits.size(), port);
    if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size()) {
        return std::nullopt;
    }
    return gateway_address{.host = std::string{listen.substr(0, colon)}, .port = port};
}

constexpr std::string base_url(gateway_address const& address) {
    std::array<char, 8> digits{};
    auto const end = std::to_chars(digits.data(), digits.data() + digits.size(), address.port).ptr;
    return "http://" + address.host + ":" + std::string{digits.data(), end} + "/anthropic";
}

constexpr double percent_cap = 1000.0;

struct model_wire {
    std::string display_name{};
    std::string id{};
};

struct workspace_wire {
    std::string current_dir{};
};

struct context_wire {
    double used_percentage = 0.0;
};

template <class Wire>
constexpr Wire lenient_at(json::value const& root, std::initializer_list<std::string_view> const path) {
    auto const* found = json::at(root, path);
    return found == nullptr ? Wire{} : json::read<Wire>(*found, {.allow_unknown_keys = true}).value_or(Wire{});
}

constexpr statusline::snapshot statusline_input(std::string_view const text) {
    auto const parsed = json::parse(text, {.jsonc = false});
    if (!parsed) {
        return {};
    }
    auto const model = lenient_at<model_wire>(*parsed, {"model"});
    auto const workspace = lenient_at<workspace_wire>(*parsed, {"workspace"});
    auto const context = lenient_at<context_wire>(*parsed, {"context_window"});
    auto const directory = workspace.current_dir.empty() ? lenient_at<std::string>(*parsed, {"cwd"}) : workspace.current_dir;
    return {.model = model.display_name.empty() ? model.id : model.display_name,
            .directory = directory,
            .context_percent = static_cast<int>(std::clamp(context.used_percentage, 0.0, percent_cap)),
            .session_id = lenient_at<std::string>(*parsed, {"session_id"})};
}

struct served_wire {
    std::string provider{};
    std::string upstream{};
    bool fallback = false;
    bool switched = false;
};

constexpr statusline::snapshot served_into(statusline::snapshot now, std::string_view const body) {
    auto const parsed = json::parse(body, {.jsonc = false});
    if (!parsed) {
        return now;
    }
    auto const wire = json::read<served_wire>(*parsed, {.allow_unknown_keys = true}).value_or(served_wire{});
    now.provider = wire.provider;
    now.upstream_model = wire.upstream;
    now.fallback = wire.fallback;
    now.switched = wire.switched;
    return now;
}

constexpr std::pair<std::uint64_t, std::uint64_t> counters_from(std::string_view const body) {
    auto const parsed = json::parse(body, {.jsonc = false});
    if (!parsed) {
        return {0, 0};
    }
    std::uint64_t repairs = 0;
    auto const* group = json::at(*parsed, {"repairs"});
    auto const* members = group == nullptr ? nullptr : std::get_if<json::object>(&group->data);
    if (members != nullptr) {
        for (auto const& member : *members) {
            repairs += json::read<std::uint64_t>(member.second).value_or(0);
        }
    }
    return {lenient_at<std::uint64_t>(*parsed, {"requests"}), repairs};
}

constexpr std::vector<std::string> as_strings(std::initializer_list<std::string_view> const words) {
    return std::vector<std::string>(words.begin(), words.end());
}

static_assert(parse(as_strings({"claude", "--model", "opus", "-p", "hi"})).value().passthrough == as_strings({"--model", "opus", "-p", "hi"}));
static_assert(parse(as_strings({"--config", "x.jsonc", "setup", "--service", "--dry-run"})).value().service);
static_assert(parse(as_strings({"env", "--json"})).value().as_json && parse(as_strings({})).value().which == command::serve);
static_assert(!parse(as_strings({"bogus"})).has_value() && !parse(as_strings({"setup", "serve"})).has_value());
static_assert(parse(as_strings({"setup", "--vscode", "/s.json"})).value().vscode == "/s.json");
static_assert(parse(as_strings({"env", "--vscode"})).value().vscode == "");
static_assert(split_listen("127.0.0.1:9480")->port == 9480 && !split_listen("nocolon") && !split_listen(":80") && !split_listen("h:99999x"));
static_assert(parse(as_strings({"usage", "--days", "30", "--json"})).value().days == 30 && parse(as_strings({"usage"})).value().days == default_days);
static_assert(!parse(as_strings({"usage", "--days"})).has_value() && !parse(as_strings({"usage", "--days", "0"})).has_value() &&
              !parse(as_strings({"usage", "--days", "9x"})).has_value() && !parse(as_strings({"usage", "--days", "99999"})).has_value());
static_assert(parse(as_strings({"doctor", "--offline", "--json"})).value().offline && parse(as_strings({"quota"})).value().which == command::quota);
static_assert(!parse(as_strings({"doctor"})).value().offline && parse(as_strings({"doctor"})).value().which == command::doctor);
static_assert(command_marked<tolerant>(command::statusline) && command_marked<tolerant>(command::stop) && !command_marked<tolerant>(command::serve));
static_assert(requested_document(parse(as_strings({"--print-schema"})).value()) == document::schema && !requested_document(options{}));
static_assert(requested_document(parse(as_strings({"--print-example", "--print-config-table"})).value()) == document::example);
static_assert(base_url({.host = "127.0.0.1", .port = 9480}) == "http://127.0.0.1:9480/anthropic");
static_assert(statusline_input(R"({"model":{"display_name":"Opus"},"workspace":{"current_dir":"/w/p"},"context_window":{"used_percentage":41}})").context_percent == 41);
static_assert(counters_from(R"({"requests":7,"repairs":{"a":2,"b":1}})") == std::pair<std::uint64_t, std::uint64_t>{7, 3});

}  // namespace llmgate::arguments

template class std::optional<llmgate::arguments::gateway_address>;
