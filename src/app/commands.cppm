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
import llmgate.proof;
import llmgate.routing;
import llmgate.statusline;
import llmgate.usage;

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
constexpr std::chrono::milliseconds stop_margin = std::chrono::seconds{5};
constexpr std::chrono::milliseconds stop_step{50};
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
    process::header_list headers;
    if (auto key = client_key(settings); !key.empty()) {
        headers.emplace_back("x-api-key", std::move(key));
    }
    return headers;
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

std::filesystem::path identity_file() {
    return state_directory() / "identity";
}

constexpr std::size_t identity_bytes = 32;
constexpr std::size_t challenge_bytes = 16;
constexpr std::string_view proof_marker = R"("proof":")";

std::string proof_in(std::string_view const body) {
    auto const at = body.find(proof_marker);
    if (at == std::string_view::npos) {
        return {};
    }
    auto const rest = body.substr(at + proof_marker.size());
    return std::string{rest.substr(0, rest.find('"'))};
}

int run_words(std::filesystem::path const& program, std::initializer_list<std::string_view> const words) {
    std::vector<std::string> arguments;
    for (auto const word : words) {
        arguments.emplace_back(word);
    }
    return process::run_and_wait(program, arguments);
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
    (void)run_words("/bin/launchctl", {"bootout", domain, unit.string()});
    if (run_words("/bin/launchctl", {"bootstrap", domain, unit.string()}) != 0) {
        return std::unexpected{"launchctl could not load the service; load it with: launchctl bootstrap " + domain + " " + unit.string()};
    }
#else
    auto const systemctl = process::find_in_path("systemctl");
    if (!systemctl || run_words(*systemctl, {"--user", "daemon-reload"}) != 0 ||
        run_words(*systemctl, {"--user", "enable", "--now", "llmgate.service"}) != 0) {
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

inline std::expected<std::string, std::string> identity_key() {
    constexpr mode_t key_mode = 0600;
    auto const path = detail::identity_file();
    if (auto const held = detail::read_file(path); held && held->size() == detail::identity_bytes * 2) {
        return *held;
    }
    auto fresh = proof::random_hex(detail::identity_bytes);
    if (!fresh) {
        return std::unexpected{"cannot generate the gateway identity: " + fresh.error().message()};
    }
    if (auto const written = process::write_file_atomic(path, *fresh, key_mode, true); !written) {
        return std::unexpected{path.string() + ": " + written.error().message()};
    }
    auto const settled = detail::read_file(path);
    return settled ? *settled : std::move(*fresh);
}

inline bool gateway_verified(arguments::gateway_address const& address) {
    auto const key = identity_key();
    auto const challenge = proof::random_hex(detail::challenge_bytes);
    if (!key || !challenge) {
        return false;
    }
    auto const answer = process::http_get(address.host, address.port, "/healthz?challenge=" + *challenge, {}, statusline_limit * 5);
    return answer && answer->status == 200 && routing::secrets_equal(detail::proof_in(answer->body), proof::hmac_sha256_hex(*key, proof::identity_message(std::format("{}:{}", address.host, address.port), *challenge)));
}

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
        if (gateway_verified(*address)) {
            return {};
        }
        return std::unexpected{std::format("something on {}:{} answers but is not this user's llmgate gateway (or is an older one); "
                                           "nothing was sent to it. Stop it and retry",
                                           address->host, address->port)};
    }
    auto const log = detail::state_directory() / std::string{state_log_name};
    std::vector<std::string> serve_arguments;
    serve_arguments.emplace_back("--config");
    serve_arguments.push_back(process::expand_home(where.options.config_path));
    serve_arguments.emplace_back("serve");
    auto const started = process::start_detached(where.binary, serve_arguments, log);
    if (!started) {
        return std::unexpected{"cannot start the gateway: " + started.error().message()};
    }
    (void)process::write_file_atomic(detail::pid_file(), std::to_string(*started) + "\n", 0600, true);
    auto const up = process::wait_until([&address] { return gateway_verified(*address); }, startup_limit);
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
    auto const stats = gateway_verified(*address) ? process::http_get(address->host, address->port, "/llmgate/stats", detail::key_headers(where.settings), statusline_limit)
                                                 : std::nullopt;
    if (!stats || stats->status != 200) {
        std::println("gateway: down ({}:{})", address->host, address->port);
        return exit_failure;
    }
    std::println("gateway: up ({}:{})\n{}", address->host, address->port, stats->body);
    return 0;
}

inline void release_pid_file() {
    if (auto const recorded = detail::recorded_pid(); recorded && *recorded == ::getpid()) {
        std::error_code ignored;
        std::filesystem::remove(detail::pid_file(), ignored);
    }
}

inline int run_stop(context const& where) {
    auto const pid = detail::recorded_pid();
    if (!pid) {
        std::println("no gateway started by llmgate is recorded; a service is stopped with launchctl or systemctl");
        return exit_failure;
    }
    std::error_code ignored;
    if (::kill(*pid, SIGTERM) != 0) {
        std::filesystem::remove(detail::pid_file(), ignored);
        std::println("the recorded gateway was not running");
        return exit_failure;
    }
    auto const limit = where.settings.time.drain + stop_margin;
    if (!process::wait_until([&pid] { return !process::process_running(*pid); }, limit, stop_step)) {
        std::println("the gateway has not stopped after {} s; it is still finishing requests (a second SIGTERM stops it at once)",
                     std::chrono::duration_cast<std::chrono::seconds>(limit).count());
        return exit_failure;
    }
    std::filesystem::remove(detail::pid_file(), ignored);
    std::println("gateway stopped");
    return 0;
}

inline usage::load_result read_ledger(std::string const& configured_path) {
    if (configured_path.empty()) {
        return {};
    }
    auto const text = detail::read_file(process::expand_home(configured_path));
    return text ? usage::load(*text) : usage::load_result{};
}

inline std::function<void(std::string const&)> ledger_sink(std::string const& configured_path) {
    if (configured_path.empty()) {
        return {};
    }
    return [path = std::filesystem::path{process::expand_home(configured_path)}, warned = false](std::string const& line) mutable {
        constexpr mode_t ledger_mode = 0600;
        if (auto const written = process::append_file(path, line, ledger_mode, true); !written && !std::exchange(warned, true)) {
            std::println(stderr, "llmgate: cannot append to the usage ledger {}: {}", path.string(), written.error().message());
        }
    };
}

inline int run_usage(context const& where) {
    auto const& configured = where.settings.usage.ledger_file;
    if (configured.empty()) {
        std::println("the usage ledger is disabled (usage.ledger_file is empty)");
        return 0;
    }
    auto const loaded = read_ledger(configured);
    auto const now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    auto const last = usage::day_of(now);
    auto const first = last - static_cast<std::int64_t>(where.options.days) + 1;
    if (where.options.as_json) {
        std::println("{}", usage::report_json(loaded.loaded, first, last));
        return 0;
    }
    std::print("{}", usage::report(loaded.loaded, first, last));
    auto const today = loaded.loaded.on_day(last).used.counted();
    auto const month = loaded.loaded.in_month(usage::month_of(last)).used.counted();
    if (auto const now_verdict = usage::evaluate(where.settings.usage.budget, today, month); now_verdict.limit != 0) {
        std::println("budget {}: {} of {} tokens ({}%) - {}", now_verdict.window, usage::compact(now_verdict.used), usage::compact(now_verdict.limit),
                     now_verdict.percent, usage::level_name(now_verdict.state));
    }
    if (loaded.skipped != 0) {
        std::println("{} unreadable ledger lines were skipped", loaded.skipped);
    }
    return 0;
}

inline int run_statusline(context const& where, std::string_view const input) {
    auto now = arguments::statusline_input(input);
    if (auto const address = detail::address_of(where.settings)) {
        auto const stats = gateway_verified(*address) ? process::http_get(address->host, address->port, "/llmgate/stats", detail::key_headers(where.settings), statusline_limit)
                                                     : std::nullopt;
        now.gateway_up = stats && stats->status == 200;
        if (now.gateway_up) {
            std::tie(now.requests, now.repairs) = arguments::counters_from(stats->body);
            auto const head = usage::headline_from(stats->body);
            now.usage_known = true;
            now.today_tokens = head.today;
            now.budget_limit = head.limit;
            now.budget_percent = static_cast<int>(head.percent);
            now.budget_window = head.window;
            now.budget_state = head.state;
        }
    }
    if (!now.directory.empty()) {
        now.branch = detail::branch_of(now.directory);
    }
    std::print("{}", statusline::render(now));
    return 0;
}

}  // namespace llmgate::commands
