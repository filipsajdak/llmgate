module;

#include <meta>

export module llmgate.arguments;

import std;
import llmgate.json;
import llmgate.reflect;
import llmgate.statusline;

export namespace llmgate::arguments {

enum class command { serve, login, claude, env, setup, statusline, status, stop, usage, doctor, quota };
static_assert(reflect::unique_values<command>());

constexpr unsigned default_days = 7;
constexpr unsigned longest_report_days = 3660;

constexpr std::size_t flag_capacity = 16;

struct flag {
    char word[flag_capacity];
};

struct limits {
    unsigned lowest = 0;
    unsigned highest = 0;
};

struct options {
    command which = command::serve;
    [[= flag{"--config"}]] std::string config_path = "~/.config/llmgate/config.jsonc";
    [[= flag{"--check-config"}]] bool check_config = false;
    [[= flag{"--version"}]] bool show_version = false;
    [[= flag{"--json"}]] bool as_json = false;
    bool as_vscode = false;
    [[= flag{"--service"}]] bool service = false;
    [[= flag{"--dry-run"}]] bool dry_run = false;
    [[= flag{"--offline"}]] bool offline = false;
    std::string vscode_file{};
    [[= flag{"--days"}]] [[= limits{1, longest_report_days}]] unsigned days = default_days;
    std::vector<std::string> passthrough{};
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

constexpr std::string_view vscode_word = "--vscode";

constexpr step vscode_flag(parse_state& state) {
    state.parsed.as_vscode = true;
    auto const given = following_word(state);
    if (given && !given->starts_with("-") && !command_named(*given)) {
        state.parsed.vscode_file = *given;
        ++state.index;
    }
    return {};
}

consteval flag flag_of(std::meta::info const member) {
    for (auto const annotation : std::meta::annotations_of(member)) {
        if (std::meta::remove_cvref(std::meta::type_of(annotation)) == ^^flag) {
            return std::meta::extract<flag>(annotation);
        }
    }
    return {};
}

consteval limits limits_of(std::meta::info const member) {
    for (auto const annotation : std::meta::annotations_of(member)) {
        if (std::meta::remove_cvref(std::meta::type_of(annotation)) == ^^limits) {
            return std::meta::extract<limits>(annotation);
        }
    }
    return {};
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

constexpr std::optional<step> apply_flag(parse_state& state, std::string_view const word) {
    static constexpr auto members = std::define_static_array(std::meta::nonstatic_data_members_of(^^options, std::meta::access_context::current()));
    std::optional<step> outcome;
    template for (constexpr auto member : members) {
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

constexpr step consume(parse_state& state) {
    std::string_view const word = state.args[state.index];
    if (state.chosen && state.parsed.which == command::claude) {
        state.parsed.passthrough.emplace_back(word);
        return {};
    }
    if (word == vscode_word) {
        return vscode_flag(state);
    }
    if (auto const applied = apply_flag(state, word)) {
        return *applied;
    }
    return choose_command(state, word);
}

}  // namespace llmgate::arguments::detail

export namespace llmgate::arguments {

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

constexpr int percent_at(json::value const& root, std::initializer_list<std::string_view> const path) {
    auto const* found = json::at(root, path);
    if (found == nullptr) {
        return 0;
    }
    if (auto const* whole = std::get_if<std::int64_t>(&found->data)) {
        return static_cast<int>(std::clamp<std::int64_t>(*whole, 0, 1000));
    }
    if (auto const* real = std::get_if<double>(&found->data)) {
        return static_cast<int>(std::clamp(*real, 0.0, 1000.0));
    }
    return 0;
}

constexpr statusline::snapshot statusline_input(std::string_view const text) {
    auto const parsed = json::parse(text, {.jsonc = false});
    if (!parsed) {
        return {};
    }
    statusline::snapshot now;
    now.model = json::text_at(*parsed, {"model", "display_name"});
    if (now.model.empty()) {
        now.model = json::text_at(*parsed, {"model", "id"});
    }
    now.directory = json::text_at(*parsed, {"workspace", "current_dir"});
    if (now.directory.empty()) {
        now.directory = json::text_at(*parsed, {"cwd"});
    }
    now.context_percent = percent_at(*parsed, {"context_window", "used_percentage"});
    return now;
}

constexpr std::pair<std::uint64_t, std::uint64_t> counters_from(std::string_view const body) {
    auto const parsed = json::parse(body, {.jsonc = false});
    if (!parsed) {
        return {0, 0};
    }
    std::uint64_t repairs = 0;
    if (auto const* group = json::at(*parsed, {"repairs"})) {
        if (auto const* members = std::get_if<json::object>(&group->data)) {
            for (auto const& [name, value] : *members) {
                if (auto const* whole = std::get_if<std::int64_t>(&value.data); whole != nullptr && *whole > 0) {
                    repairs += static_cast<std::uint64_t>(*whole);
                }
            }
        }
    }
    return {json::count_at(*parsed, {"requests"}).value_or(0), repairs};
}

constexpr std::vector<std::string> as_strings(std::initializer_list<std::string_view> const words) {
    return std::vector<std::string>(words.begin(), words.end());
}

static_assert(parse(as_strings({"claude", "--model", "opus", "-p", "hi"})).value().passthrough == as_strings({"--model", "opus", "-p", "hi"}));
static_assert(parse(as_strings({"--config", "x.jsonc", "setup", "--service", "--dry-run"})).value().service);
static_assert(parse(as_strings({"env", "--json"})).value().as_json && parse(as_strings({})).value().which == command::serve);
static_assert(!parse(as_strings({"bogus"})).has_value() && !parse(as_strings({"setup", "serve"})).has_value());
static_assert(parse(as_strings({"setup", "--vscode", "/s.json"})).value().vscode_file == "/s.json");
static_assert(parse(as_strings({"env", "--vscode"})).value().vscode_file.empty());
static_assert(split_listen("127.0.0.1:9480")->port == 9480 && !split_listen("nocolon") && !split_listen(":80") && !split_listen("h:99999x"));
static_assert(parse(as_strings({"usage", "--days", "30", "--json"})).value().days == 30 && parse(as_strings({"usage"})).value().days == default_days);
static_assert(!parse(as_strings({"usage", "--days"})).has_value() && !parse(as_strings({"usage", "--days", "0"})).has_value() &&
              !parse(as_strings({"usage", "--days", "9x"})).has_value() && !parse(as_strings({"usage", "--days", "99999"})).has_value());
static_assert(parse(as_strings({"doctor", "--offline", "--json"})).value().offline && parse(as_strings({"quota"})).value().which == command::quota);
static_assert(!parse(as_strings({"doctor"})).value().offline && parse(as_strings({"doctor"})).value().which == command::doctor);
static_assert(base_url({.host = "127.0.0.1", .port = 9480}) == "http://127.0.0.1:9480/anthropic");
static_assert(statusline_input(R"({"model":{"display_name":"Opus"},"workspace":{"current_dir":"/w/p"},"context_window":{"used_percentage":41}})").context_percent == 41);
static_assert(counters_from(R"({"requests":7,"repairs":{"a":2,"b":1}})") == std::pair<std::uint64_t, std::uint64_t>{7, 3});

}  // namespace llmgate::arguments

template class std::optional<llmgate::arguments::gateway_address>;
