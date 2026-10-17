export module llmgate.arguments;

import std;
import llmgate.json;
import llmgate.statusline;

export namespace llmgate::arguments {

enum class command { serve, login, claude, env, setup, statusline, status, stop };

struct options {
    command which = command::serve;
    std::string config_path = "~/.config/llmgate/config.jsonc";
    bool check_config = false;
    bool show_version = false;
    bool as_json = false;
    bool as_vscode = false;
    bool service = false;
    bool dry_run = false;
    std::string vscode_file{};
    std::vector<std::string> passthrough{};
};

struct gateway_address {
    std::string host;
    std::uint16_t port;
};

constexpr auto command_names = std::to_array<std::pair<std::string_view, command>>({
    {"serve", command::serve},
    {"login", command::login},
    {"claude", command::claude},
    {"env", command::env},
    {"setup", command::setup},
    {"statusline", command::statusline},
    {"status", command::status},
    {"stop", command::stop},
});

constexpr std::optional<command> command_named(std::string_view const word) {
    auto const found = std::ranges::find(command_names, word, &std::pair<std::string_view, command>::first);
    return found == command_names.end() ? std::nullopt : std::optional{found->second};
}

constexpr std::expected<options, std::string> parse(std::span<std::string const> const args) {
    options parsed;
    bool chosen = false;
    for (std::size_t index = 0; index < args.size(); ++index) {
        std::string_view const word = args[index];
        if (chosen && parsed.which == command::claude) {
            parsed.passthrough.emplace_back(word);
        } else if (word == "--version") {
            parsed.show_version = true;
        } else if (word == "--check-config") {
            parsed.check_config = true;
        } else if (word == "--config" && index + 1 < args.size()) {
            parsed.config_path = args[++index];
        } else if (word == "--json") {
            parsed.as_json = true;
        } else if (word == "--service") {
            parsed.service = true;
        } else if (word == "--dry-run") {
            parsed.dry_run = true;
        } else if (word == "--vscode") {
            parsed.as_vscode = true;
            if (index + 1 < args.size() && !args[index + 1].starts_with("-") && !command_named(args[index + 1])) {
                parsed.vscode_file = args[++index];
            }
        } else if (auto const named = command_named(word); named && !chosen) {
            parsed.which = *named;
            chosen = true;
        } else {
            return std::unexpected{std::string{"unknown argument: "} + std::string{word}};
        }
    }
    return parsed;
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

constexpr json::value const* path_value(json::value const& root, std::initializer_list<std::string_view> const path) {
    json::value const* current = &root;
    for (auto const key : path) {
        auto const* members = std::get_if<json::object>(&current->data);
        if (members == nullptr) {
            return nullptr;
        }
        auto const found = std::ranges::find(*members, key, &json::object::value_type::first);
        if (found == members->end()) {
            return nullptr;
        }
        current = &found->second;
    }
    return current;
}

constexpr std::string text_at(json::value const& root, std::initializer_list<std::string_view> const path) {
    auto const* found = path_value(root, path);
    auto const* text = found == nullptr ? nullptr : std::get_if<std::string>(&found->data);
    return text == nullptr ? std::string{} : *text;
}

constexpr int percent_at(json::value const& root, std::initializer_list<std::string_view> const path) {
    auto const* found = path_value(root, path);
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
    now.model = text_at(*parsed, {"model", "display_name"});
    if (now.model.empty()) {
        now.model = text_at(*parsed, {"model", "id"});
    }
    now.directory = text_at(*parsed, {"workspace", "current_dir"});
    if (now.directory.empty()) {
        now.directory = text_at(*parsed, {"cwd"});
    }
    now.context_percent = percent_at(*parsed, {"context_window", "used_percentage"});
    return now;
}

constexpr std::pair<std::uint64_t, std::uint64_t> counters_from(std::string_view const body) {
    auto const parsed = json::parse(body, {.jsonc = false});
    if (!parsed) {
        return {0, 0};
    }
    auto const count = [&](std::initializer_list<std::string_view> const path) {
        auto const* found = path_value(*parsed, path);
        auto const* whole = found == nullptr ? nullptr : std::get_if<std::int64_t>(&found->data);
        return whole == nullptr || *whole < 0 ? std::uint64_t{0} : static_cast<std::uint64_t>(*whole);
    };
    std::uint64_t repairs = 0;
    if (auto const* group = path_value(*parsed, {"repairs"})) {
        if (auto const* members = std::get_if<json::object>(&group->data)) {
            for (auto const& [name, value] : *members) {
                if (auto const* whole = std::get_if<std::int64_t>(&value.data); whole != nullptr && *whole > 0) {
                    repairs += static_cast<std::uint64_t>(*whole);
                }
            }
        }
    }
    return {count({"requests"}), repairs};
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
static_assert(base_url({.host = "127.0.0.1", .port = 9480}) == "http://127.0.0.1:9480/anthropic");
static_assert(statusline_input(R"({"model":{"display_name":"Opus"},"workspace":{"current_dir":"/w/p"},"context_window":{"used_percentage":41}})").context_percent == 41);
static_assert(counters_from(R"({"requests":7,"repairs":{"a":2,"b":1}})") == std::pair<std::uint64_t, std::uint64_t>{7, 3});

}  // namespace llmgate::arguments
