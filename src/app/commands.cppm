module;

#include <csignal>
#include <cstdio>
#include <cstdlib>

#include <unistd.h>

export module llmgate.commands;

import std;
import llmgate.arguments;
import llmgate.claude;
import llmgate.gateway;
import llmgate.json;
import llmgate.process;
import llmgate.routing;
import llmgate.statusline;

export namespace llmgate::commands {

using outcome = std::expected<void, std::string>;

struct context {
    arguments::options options;
    gateway::config settings;
    std::filesystem::path binary;
};

constexpr std::string_view state_log_name = "gateway.log";
constexpr std::string_view state_pid_name = "gateway.pid";
constexpr std::string_view service_label = "dev.llmgate";
constexpr std::chrono::milliseconds startup_limit{30000};
constexpr std::chrono::milliseconds statusline_limit{400};
constexpr int exit_failure = 1;
constexpr int exit_missing = 127;

}  // namespace llmgate::commands

namespace llmgate::commands::detail {

std::optional<std::string> variable(std::string_view const name) {
    auto const* value = std::getenv(std::string{name}.c_str());
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }
    return std::string{value};
}

std::filesystem::path state_directory() {
    if (auto const base = variable("XDG_STATE_HOME")) {
        return std::filesystem::path{*base} / "llmgate";
    }
    return std::filesystem::path{process::expand_home("~/.local/state")} / "llmgate";
}

std::filesystem::path config_directory() {
    if (auto const base = variable("XDG_CONFIG_HOME")) {
        return std::filesystem::path{*base};
    }
    return std::filesystem::path{process::expand_home("~/.config")};
}

std::expected<std::string, std::string> read_file(std::filesystem::path const& path) {
    std::ifstream in{path};
    if (!in) {
        return std::unexpected{"cannot read " + path.string()};
    }
    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

std::expected<arguments::gateway_address, std::string> address_of(gateway::config const& settings) {
    auto const address = arguments::split_listen(settings.listen);
    if (!address) {
        return std::unexpected{std::string{"listen must look like 127.0.0.1:9480"}};
    }
    return *address;
}

std::string client_key(gateway::config const& settings) {
    return settings.client_key_env.empty() ? std::string{} : variable(settings.client_key_env).value_or("");
}

process::header_list key_headers(gateway::config const& settings) {
    auto const key = client_key(settings);
    return key.empty() ? process::header_list{} : process::header_list{{"x-api-key", key}};
}

std::expected<claude::environment_list, std::string> environment_for(gateway::config const& settings) {
    auto const address = address_of(settings);
    if (!address) {
        return std::unexpected{address.error()};
    }
    if (!settings.client_key_env.empty() && client_key(settings).empty()) {
        return std::unexpected{"client_key_env names " + settings.client_key_env + ", which is not set"};
    }
    return claude::environment(settings.claude, arguments::base_url(*address), client_key(settings),
                               process::expand_home(settings.claude.profile_dir));
}

std::string status_line_command(context const& where) {
    return claude::shell_quote(where.binary.string()) + " --config " + claude::shell_quote(process::expand_home(where.options.config_path)) +
           " statusline";
}

std::expected<json::value, std::string> existing_json(std::filesystem::path const& path) {
    std::error_code ignored;
    if (!std::filesystem::exists(path, ignored)) {
        return json::value{};
    }
    auto text = read_file(path);
    if (!text) {
        return std::unexpected{text.error()};
    }
    auto parsed = json::parse(*text, {.jsonc = false});
    if (!parsed) {
        return std::unexpected{path.string() + " is not plain JSON (" + parsed.error().message + "); fix or remove it, nothing was changed"};
    }
    return std::move(*parsed);
}

outcome write_json(std::filesystem::path const& path, json::value const& document, bool const dry_run, bool const private_parent) {
    constexpr mode_t file_mode = 0600;
    auto const text = json::write(document, 2);
    auto const current = read_file(path);
    if (current && *current == text) {
        std::println("unchanged  {}", path.string());
        return {};
    }
    std::println("{} {}", dry_run ? "would write" : "writing    ", path.string());
    if (dry_run) {
        return {};
    }
    if (auto const written = process::write_file_atomic(path, text, file_mode, private_parent); !written) {
        return std::unexpected{path.string() + ": " + written.error().message()};
    }
    return {};
}

std::filesystem::path pid_file() {
    return state_directory() / std::string{state_pid_name};
}

std::optional<pid_t> recorded_pid() {
    auto const text = read_file(pid_file());
    int pid = 0;
    if (!text || std::from_chars(text->data(), text->data() + text->size(), pid).ec != std::errc{} || pid <= 0) {
        return std::nullopt;
    }
    return static_cast<pid_t>(pid);
}

outcome install_service(context const& where, bool const dry_run) {
    auto const config_path = process::expand_home(where.options.config_path);
    auto const log = (state_directory() / std::string{state_log_name}).string();
#if defined(__APPLE__)
    auto const unit = std::filesystem::path{process::expand_home("~/Library/LaunchAgents")} / (std::string{service_label} + ".plist");
    auto const text = claude::launchd_plist(service_label, where.binary.string(), config_path, log);
#else
    auto const unit = config_directory() / "systemd" / "user" / "llmgate.service";
    auto const text = claude::systemd_unit(where.binary.string(), config_path);
#endif
    std::println("{} {}", dry_run ? "would write" : "writing    ", unit.string());
    if (dry_run) {
        return {};
    }
    constexpr mode_t unit_mode = 0644;
    if (auto const written = process::write_file_atomic(unit, text, unit_mode, false); !written) {
        return std::unexpected{unit.string() + ": " + written.error().message()};
    }
#if defined(__APPLE__)
    auto const domain = std::format("gui/{}", ::getuid());
    (void)process::run_and_wait("/bin/launchctl", {"bootout", domain, unit.string()});
    if (process::run_and_wait("/bin/launchctl", {"bootstrap", domain, unit.string()}) != 0) {
        return std::unexpected{"launchctl could not load the service; load it with: launchctl bootstrap " + domain + " " + unit.string()};
    }
#else
    auto const systemctl = process::find_in_path("systemctl");
    if (!systemctl || process::run_and_wait(*systemctl, {"--user", "daemon-reload"}) != 0 ||
        process::run_and_wait(*systemctl, {"--user", "enable", "--now", "llmgate.service"}) != 0) {
        return std::unexpected{"systemctl could not start the service; start it with: systemctl --user enable --now llmgate.service"};
    }
#endif
    std::println("service started");
    return {};
}

std::string branch_of(std::filesystem::path directory) {
    for (int depth = 0; depth < 64 && !directory.empty(); ++depth) {
        if (auto const head = read_file(directory / ".git" / "HEAD")) {
            return statusline::branch_from_head(*head);
        }
        auto const parent = directory.parent_path();
        if (parent == directory) {
            break;
        }
        directory = parent;
    }
    return {};
}

}  // namespace llmgate::commands::detail

export namespace llmgate::commands {

inline outcome ensure_profile(context const& where, bool const dry_run) {
    auto const directory = std::filesystem::path{process::expand_home(where.settings.claude.profile_dir)};
    auto const settings_path = directory / "settings.json";
    auto existing = detail::existing_json(settings_path);
    if (!existing) {
        return std::unexpected{existing.error()};
    }
    auto merged = claude::merge_settings(std::move(*existing), where.settings.claude.effort, detail::status_line_command(where));
    return detail::write_json(settings_path, merged, dry_run, true);
}

inline outcome ensure_gateway(context const& where) {
    auto const address = detail::address_of(where.settings);
    if (!address) {
        return std::unexpected{address.error()};
    }
    if (address->port == 0) {
        return std::unexpected{std::string{"listen needs a fixed port to be shared between launches"}};
    }
    if (process::gateway_alive(address->host, address->port)) {
        return {};
    }
    auto const log = detail::state_directory() / std::string{state_log_name};
    auto const started = process::start_detached(where.binary, {"--config", process::expand_home(where.options.config_path), "serve"}, log);
    if (!started) {
        return std::unexpected{"cannot start the gateway: " + started.error().message()};
    }
    (void)process::write_file_atomic(detail::pid_file(), std::to_string(*started) + "\n", 0600, true);
    auto const up = process::wait_until([&address] { return process::gateway_alive(address->host, address->port); }, startup_limit);
    if (!up) {
        return std::unexpected{"the gateway did not come up; see " + log.string()};
    }
    return {};
}

inline int run_env(context const& where) {
    auto const list = detail::environment_for(where.settings);
    if (!list) {
        std::println(stderr, "llmgate: {}", list.error());
        return exit_failure;
    }
    if (where.options.as_vscode) {
        std::print("{}", json::write(claude::vscode_snippet(*list), 2));
    } else if (where.options.as_json) {
        std::print("{}", json::write(claude::environment_object(*list), 2));
    } else {
        std::print("{}", claude::shell_exports(*list));
    }
    return 0;
}

inline int run_setup(context const& where) {
    auto const list = detail::environment_for(where.settings);
    if (!list) {
        std::println(stderr, "llmgate: {}", list.error());
        return exit_failure;
    }
    auto const dry_run = where.options.dry_run;
    outcome done = ensure_profile(where, dry_run);
    if (done && where.options.as_vscode && !where.options.vscode_file.empty()) {
        auto const path = std::filesystem::path{process::expand_home(where.options.vscode_file)};
        auto existing = detail::existing_json(path);
        done = existing ? detail::write_json(path, claude::merge_vscode(std::move(*existing), *list), dry_run, false)
                        : outcome{std::unexpected{existing.error()}};
    }
    if (done && where.options.service) {
        done = detail::install_service(where, dry_run);
    }
    if (!done) {
        std::println(stderr, "llmgate: {}", done.error());
        return exit_failure;
    }
    std::println("Ready. Start Claude Code with: llmgate claude");
    return 0;
}

inline int run_claude(context const& where) {
    auto const list = detail::environment_for(where.settings);
    if (!list) {
        std::println(stderr, "llmgate: {}", list.error());
        return exit_failure;
    }
    if (auto const profile = ensure_profile(where, false); !profile) {
        std::println(stderr, "llmgate: {}", profile.error());
        return exit_failure;
    }
    if (auto const gateway = ensure_gateway(where); !gateway) {
        std::println(stderr, "llmgate: {}", gateway.error());
        return exit_failure;
    }
    auto const program = detail::variable("LLMGATE_CLAUDE").transform([](std::string const& name) { return process::find_in_path(name); })
                             .value_or(process::find_in_path("claude"));
    if (!program) {
        std::println(stderr, "llmgate: the claude command was not found on PATH");
        return exit_missing;
    }
    process::exec_program(*program, where.options.passthrough, *list);
}

inline int run_status(context const& where) {
    auto const address = detail::address_of(where.settings);
    if (!address) {
        std::println(stderr, "llmgate: {}", address.error());
        return exit_failure;
    }
    auto const stats = process::http_get(address->host, address->port, "/llmgate/stats", detail::key_headers(where.settings), statusline_limit);
    if (!stats || stats->status != 200) {
        std::println("gateway: down ({}:{})", address->host, address->port);
        return exit_failure;
    }
    std::println("gateway: up ({}:{})\n{}", address->host, address->port, stats->body);
    return 0;
}

inline int run_stop(context const&) {
    auto const pid = detail::recorded_pid();
    if (!pid) {
        std::println("no gateway started by llmgate is recorded; a service is stopped with launchctl or systemctl");
        return exit_failure;
    }
    auto const killed = ::kill(*pid, SIGTERM) == 0;
    std::filesystem::remove(detail::pid_file());
    std::println("{}", killed ? "gateway stopped" : "the recorded gateway was not running");
    return killed ? 0 : exit_failure;
}

inline int run_statusline(context const& where, std::string_view const input) {
    auto now = arguments::statusline_input(input);
    if (auto const address = detail::address_of(where.settings)) {
        auto const stats = process::http_get(address->host, address->port, "/llmgate/stats", detail::key_headers(where.settings), statusline_limit);
        now.gateway_up = stats && stats->status == 200;
        if (now.gateway_up) {
            std::tie(now.requests, now.repairs) = arguments::counters_from(stats->body);
        }
    }
    if (!now.directory.empty()) {
        now.branch = detail::branch_of(now.directory);
    }
    std::print("{}", statusline::render(now));
    return 0;
}

}  // namespace llmgate::commands
