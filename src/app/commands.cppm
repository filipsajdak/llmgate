module;

#include <meta>

#include <csignal>
#include <cstdio>
#include <cstdlib>

#include <sys/stat.h>
#include <unistd.h>

export module llmgate.commands;

import std;
import llmgate.arguments;
import llmgate.async;
import llmgate.claude;
import llmgate.control;
import llmgate.copilot;
import llmgate.doctor;
import llmgate.gateway;
import llmgate.json;
import llmgate.process;
import llmgate.proof;
import llmgate.quota;
import llmgate.routing;
import llmgate.statusline;
import llmgate.tls;
import llmgate.usage;

export namespace llmgate::commands {

using outcome = std::expected<void, std::string>;

using program_runner = int (*)(std::filesystem::path const&, std::vector<std::string> const&);

struct context {
    arguments::options options;
    gateway::config settings;
    std::filesystem::path binary;
    std::string config_error{};
    program_runner run_program = &process::run_and_wait;
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
    return std::format("{} --config {} statusline", claude::shell_quote(where.binary.string()),
                       claude::shell_quote(process::expand_home(where.options.config_path)));
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

void announce_write(std::filesystem::path const& path, bool const dry_run) {
    std::println("{} {}", dry_run ? "would write" : "writing    ", path.string());
}

outcome write_text(std::filesystem::path const& path, std::string const& text, bool const dry_run) {
    constexpr mode_t file_mode = 0600;
    if (auto const current = read_file(path); current && *current == text) {
        std::println("unchanged  {}", path.string());
        return {};
    }
    announce_write(path, dry_run);
    if (dry_run) {
        return {};
    }
    if (auto const written = process::write_file_atomic(path, text, file_mode, true); !written) {
        return std::unexpected{path.string() + ": " + written.error().message()};
    }
    return {};
}

outcome write_json(std::filesystem::path const& path, json::value const& document, bool const dry_run, bool const private_parent) {
    constexpr mode_t file_mode = 0600;
    auto const text = json::write(document, 2);
    auto const current = read_file(path);
    if (current && *current == text) {
        std::println("unchanged  {}", path.string());
        return {};
    }
    announce_write(path, dry_run);
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

int run_words(context const& where, std::filesystem::path const& program, std::initializer_list<std::string_view> const words) {
    std::vector<std::string> arguments;
    for (auto const word : words) {
        arguments.emplace_back(word);
    }
    return where.run_program(program, arguments);
}

struct service_unit {
    std::filesystem::path path;
    std::string text;
};

#if defined(__APPLE__)
service_unit service_unit_for(context const& where) {
    auto const log = (state_directory() / std::string{state_log_name}).string();
    return {.path = std::filesystem::path{process::expand_home("~/Library/LaunchAgents")} / (std::string{service_label} + ".plist"),
            .text = claude::launchd_plist(service_label, where.binary.string(), process::expand_home(where.options.config_path), log)};
}

outcome load_service(context const& where, std::filesystem::path const& unit) {
    auto const domain = std::format("gui/{}", ::getuid());
    (void)run_words(where, "/bin/launchctl", {"bootout", domain, unit.string()});
    if (run_words(where, "/bin/launchctl", {"bootstrap", domain, unit.string()}) != 0) {
        return std::unexpected{"launchctl could not load the service; load it with: launchctl bootstrap " + domain + " " + unit.string()};
    }
    return {};
}
#else
service_unit service_unit_for(context const& where) {
    return {.path = config_directory() / "systemd" / "user" / "llmgate.service",
            .text = claude::systemd_unit(where.binary.string(), process::expand_home(where.options.config_path))};
}

outcome load_service(context const& where, std::filesystem::path const&) {
    auto const systemctl = process::find_in_path("systemctl").value_or("systemctl");
    bool const started = run_words(where, systemctl, {"--user", "daemon-reload"}) == 0 &&
                         run_words(where, systemctl, {"--user", "enable", "--now", "llmgate.service"}) == 0;
    if (!started) {
        return std::unexpected{"systemctl could not start the service; start it with: systemctl --user enable --now llmgate.service"};
    }
    return {};
}
#endif

outcome write_unit(service_unit const& unit) {
    constexpr mode_t unit_mode = 0644;
    if (auto const written = process::write_file_atomic(unit.path, unit.text, unit_mode, false); !written) {
        return std::unexpected{unit.path.string() + ": " + written.error().message()};
    }
    return {};
}

outcome install_service(context const& where, bool const dry_run) {
    auto const unit = service_unit_for(where);
    announce_write(unit.path, dry_run);
    if (dry_run) {
        return {};
    }
    if (auto const written = write_unit(unit); !written) {
        return written;
    }
    if (auto const loaded = load_service(where, unit.path); !loaded) {
        return loaded;
    }
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
    if (auto written = detail::write_json(settings_path, merged, dry_run, true); !written) {
        return written;
    }
    return detail::write_text(directory / "commands" / "gateway.md",
                              claude::gateway_command_file(where.binary.string(), process::expand_home(where.options.config_path)), dry_run);
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
    if (where.options.vscode) {
        std::print("{}", json::write(claude::vscode_snippet(*list), 2));
    } else if (where.options.as_json) {
        std::print("{}", json::write(claude::environment_object(*list), 2));
    } else {
        std::print("{}", claude::shell_exports(*list));
    }
    return 0;
}

}  // namespace llmgate::commands

namespace llmgate::commands::detail {

outcome write_vscode_settings(context const& where, claude::environment_list const& list) {
    if (!where.options.vscode || where.options.vscode->empty()) {
        return {};
    }
    auto const path = std::filesystem::path{process::expand_home(*where.options.vscode)};
    auto existing = existing_json(path);
    if (!existing) {
        return std::unexpected{existing.error()};
    }
    return write_json(path, claude::merge_vscode(std::move(*existing), list), where.options.dry_run, false);
}

outcome install_requested_service(context const& where) {
    if (!where.options.service) {
        return {};
    }
    return install_service(where, where.options.dry_run);
}

outcome run_setup_steps(context const& where, claude::environment_list const& list) {
    if (auto const profile = ensure_profile(where, where.options.dry_run); !profile) {
        return profile;
    }
    if (auto const vscode = write_vscode_settings(where, list); !vscode) {
        return vscode;
    }
    return install_requested_service(where);
}

}  // namespace llmgate::commands::detail

export namespace llmgate::commands {

inline int run_setup(context const& where) {
    auto const list = detail::environment_for(where.settings);
    if (!list) {
        std::println(stderr, "llmgate: {}", list.error());
        return exit_failure;
    }
    if (auto const done = detail::run_setup_steps(where, *list); !done) {
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

}  // namespace llmgate::commands

namespace llmgate::commands::detail {
doctor::file_fact file_fact_of(std::filesystem::path const& path) {
    struct stat info{};
    if (::stat(path.c_str(), &info) != 0) {
        return {.path = path.string()};
    }
    return {.path = path.string(), .exists = true, .mode = static_cast<unsigned>(info.st_mode & 0777)};
}

bool can_write(std::filesystem::path path) {
    std::error_code ignored;
    while (!path.empty() && !std::filesystem::exists(path, ignored)) {
        path = path.parent_path();
    }
    return !path.empty() && ::access(path.c_str(), W_OK) == 0;
}

doctor::settings_state profile_state(context const& where) {
    std::error_code ignored;
    auto const directory = std::filesystem::path{process::expand_home(where.settings.claude.profile_dir)};
    auto const settings_path = directory / "settings.json";
    if (!std::filesystem::exists(directory, ignored)) {
        return doctor::settings_state::no_profile;
    }
    if (!std::filesystem::exists(settings_path, ignored)) {
        return doctor::settings_state::no_settings;
    }
    auto existing = existing_json(settings_path);
    if (!existing) {
        return doctor::settings_state::invalid;
    }
    auto const current = json::write(*existing, 2);
    auto merged = claude::merge_settings(std::move(*existing), where.settings.claude.effort, status_line_command(where));
    return json::write(merged, 2) == current ? doctor::settings_state::current : doctor::settings_state::outdated;
}

doctor::exchange_state exchange_of(std::error_code const error) {
    if (!error) {
        return doctor::exchange_state::accepted;
    }
    if (error == copilot::errc::login_required) {
        return doctor::exchange_state::login_required;
    }
    return error == copilot::errc::github_unreachable ? doctor::exchange_state::unreachable : doctor::exchange_state::rejected;
}

std::string failure_text(std::error_code const error) {
    if (error == copilot::errc::login_required) {
        return "there is no valid Copilot login; run llmgate login";
    }
    return error.message();
}

std::expected<quota::summary, std::string> fetch_quota(async::reactor& loop, tls::client_context const& tls_context, gateway::copilot_config const& copilot_settings) {
    auto const body = loop.run(copilot::fetch_user(loop, tls_context, copilot_settings));
    if (!body) {
        return std::unexpected{failure_text(body.error())};
    }
    auto parsed = quota::parse(*body);
    if (!parsed) {
        return std::unexpected{std::string{quota::unavailable_message}};
    }
    return std::move(*parsed);
}

bool has_token_file(gateway::provider_config const& provider) {
    return provider.copilot && !provider.copilot->token_file.empty();
}

std::optional<gateway::copilot_config> copilot_target(gateway::config const& settings) {
    for (auto const& entry : gateway::effective_providers(settings)) {
        if (has_token_file(entry.second)) {
            return entry.second.copilot;
        }
    }
    return std::nullopt;
}

void probe_network(gateway::config const& settings, doctor::facts& seen) {
    async::reactor loop;
    auto tls_context = tls::client_context::make(settings.tls_groups);
    if (!tls_context) {
        seen.exchange = doctor::exchange_state::unreachable;
        return;
    }
    auto const target = copilot_target(settings);
    if (!target) {
        return;
    }
    seen.exchange = exchange_of(loop.run(copilot::check_login(loop, *tls_context, *target)));
    if (seen.exchange != doctor::exchange_state::accepted) {
        return;
    }
    auto found = fetch_quota(loop, *tls_context, *target);
    seen.quota = found ? doctor::quota_state::read : doctor::quota_state::unavailable;
    seen.quota_summary = found ? std::optional{std::move(*found)} : std::nullopt;
}

std::optional<doctor::port_state> port_state_of(context const& where, bool const identity_exists) {
    auto const address = address_of(where.settings);
    if (!address || address->port == 0) {
        return std::nullopt;
    }
    if (process::gateway_alive(address->host, address->port)) {
        return identity_exists && gateway_verified(*address) ? doctor::port_state::ours : doctor::port_state::foreign;
    }
    return process::tcp_connectable(address->host, address->port, statusline_limit) ? doctor::port_state::foreign : doctor::port_state::free;
}

bool claude_on_path() {
    auto const named = variable("LLMGATE_CLAUDE").transform([](std::string const& name) { return process::find_in_path(name); }).value_or(process::find_in_path("claude"));
    return named.has_value();
}

void describe_credential(context const& where, doctor::facts& seen) {
    auto const copilot_settings = copilot_target(where.settings);
    seen.copilot_configured = copilot_settings.has_value();
    if (seen.copilot_configured) {
        auto const path = process::expand_home(copilot_settings->token_file);
        seen.token = file_fact_of(path);
        seen.token_valid = seen.token.exists && copilot::load_oauth_token(path).has_value();
    }
    seen.static_token_variable = where.settings.static_token_env;
    seen.static_token_set = !where.settings.static_token_env.empty() && variable(where.settings.static_token_env).has_value();
}

void describe_ledger(gateway::config const& settings, doctor::facts& seen) {
    seen.ledger_configured = !settings.usage.ledger_file.empty();
    if (!seen.ledger_configured) {
        return;
    }
    auto const path = std::filesystem::path{process::expand_home(settings.usage.ledger_file)};
    seen.ledger_file = file_fact_of(path);
    seen.ledger = can_write(path) ? doctor::ledger_state::writable : doctor::ledger_state::not_writable;
}

template <class Owner>
consteval std::meta::info member_named(std::string_view const name) {
    for (auto const candidate : std::meta::nonstatic_data_members_of(^^Owner, std::meta::access_context::current())) {
        if (std::meta::identifier_of(candidate) == name) {
            return candidate;
        }
    }
    throw "commands::copy_matching_members: the target has no member with that name";
}

template <class From, class To>
    requires std::is_class_v<From> && std::is_class_v<To>
constexpr void copy_matching_members(From const& from, To& to) {
    template for (constexpr auto source : std::define_static_array(std::meta::nonstatic_data_members_of(^^From, std::meta::access_context::current()))) {
        to.[:member_named<To>(std::meta::identifier_of(source)):] = from.[:source:];
    }
}

void copy_budget(usage::budget const& from, doctor::facts& seen) {
    copy_matching_members(from, seen);
}

doctor::facts gather_facts(context const& where) {
    doctor::facts seen;
    seen.config_error = where.config_error;
    seen.identity = file_fact_of(identity_file());
    seen.claude_found = claude_on_path();
    if (!seen.config_error.empty()) {
        return seen;
    }
    seen.listen = where.settings.listen;
    seen.client_key_variable = where.settings.client_key_env;
    seen.client_key_set = !where.settings.client_key_env.empty() && !client_key(where.settings).empty();
    seen.port = port_state_of(where, seen.identity.exists);
    seen.profile_dir = process::expand_home(where.settings.claude.profile_dir);
    seen.settings = profile_state(where);
    describe_credential(where, seen);
    describe_ledger(where.settings, seen);
    copy_budget(where.settings.usage.budget, seen);
    if (!where.options.offline && seen.copilot_configured && seen.token_valid) {
        probe_network(where.settings, seen);
    }
    return seen;
}

}  // namespace llmgate::commands::detail

export namespace llmgate::commands {

inline int run_doctor(context const& where) {
    auto const found = doctor::diagnose(detail::gather_facts(where));
    if (where.options.as_json) {
        std::print("{}", doctor::render_json(found));
    } else {
        std::print("{}", doctor::render_text(found));
        if (where.options.offline) {
            std::println("network checks were skipped (--offline)");
        }
    }
    return doctor::exit_code(found) == 0 ? 0 : exit_failure;
}

inline int run_quota(context const& where) {
    auto const copilot_settings = detail::copilot_target(where.settings);
    if (!copilot_settings) {
        std::println(stderr, "llmgate: quota needs a Copilot login: a copilot block with token_file, at the top level or under a provider");
        return exit_failure;
    }
    async::reactor loop;
    auto tls_context = tls::client_context::make(where.settings.tls_groups);
    if (!tls_context) {
        std::println(stderr, "llmgate: cannot start TLS: {}", tls_context.error().message());
        return exit_failure;
    }
    auto const found = detail::fetch_quota(loop, *tls_context, *copilot_settings);
    if (!found) {
        std::println(stderr, "llmgate: Copilot quota is unavailable: {}", found.error());
        return exit_failure;
    }
    std::print("{}", where.options.as_json ? quota::to_json(*found) : quota::render(*found));
    return 0;
}

inline std::filesystem::path overrides_file() {
    return detail::state_directory() / "overrides.json";
}

inline control::overrides read_overrides() {
    auto loaded = control::load_pins(detail::read_file(overrides_file()).value_or(std::string{}));
    if (!loaded) {
        std::println(stderr, "llmgate: ignoring the damaged overrides file {}: {}", overrides_file().string(), loaded.error());
        return {};
    }
    return std::move(*loaded);
}

inline std::function<void(control::overrides const&)> overrides_sink() {
    return [](control::overrides const& book) {
        constexpr mode_t overrides_mode = 0600;
        if (auto const written = process::write_file_atomic(overrides_file(), control::save_pins(book), overrides_mode, true); !written) {
            std::println(stderr, "llmgate: cannot save the overrides {}: {}", overrides_file().string(), written.error().message());
        }
    };
}

inline std::expected<std::string, std::string> control_body(std::vector<std::string> const& words, std::string_view const session) {
    constexpr std::string_view clear_word = "clear";
    control::command what{.session = std::string{session}};
    if (words.size() == 1 && words.front() == clear_word) {
        what.action = clear_word;
    } else if (words.size() == 2 && words.back() == clear_word) {
        what = {.action = std::string{clear_word}, .role = words.front(), .session = std::string{session}};
    } else if (words.size() == 2) {
        what = {.action = "use", .role = words.front(), .target = words.back(), .session = std::string{session}};
    } else {
        return std::unexpected{std::string{"use needs ROLE TARGET, ROLE clear, or clear"}};
    }
    return json::write(json::to_value(what));
}

struct control_call {
    arguments::gateway_address address;
    process::header_list headers;
    std::string body;
};

inline std::expected<control_call, std::string> prepare_control_call(context const& where) {
    auto const address = detail::address_of(where.settings);
    if (!address) {
        return std::unexpected{address.error()};
    }
    auto body = control_body(where.options.passthrough, where.options.session);
    if (!body) {
        return std::unexpected{std::move(body.error())};
    }
    auto const key = identity_key();
    if (!key) {
        return std::unexpected{key.error()};
    }
    auto headers = detail::key_headers(where.settings);
    headers.emplace_back("x-llmgate-proof", proof::hmac_sha256_hex(*key, proof::identity_message(std::format("{}:{}/control", address->host, address->port), *body)));
    return control_call{.address = *address, .headers = std::move(headers), .body = std::move(*body)};
}

inline int run_use(context const& where) {
    auto const call = prepare_control_call(where);
    if (!call) {
        std::println(stderr, "llmgate: {}", call.error());
        return exit_failure;
    }
    auto const answer = gateway_verified(call->address) ? process::http_post(call->address.host, call->address.port, "/llmgate/control", call->headers, call->body, statusline_limit * 5) : std::nullopt;
    if (!answer || answer->status != 200) {
        std::println(stderr, "llmgate: the gateway refused or did not answer: {}", answer ? answer->body : std::string{"down"});
        return exit_failure;
    }
    std::println("ok");
    return 0;
}

inline int show_from_gateway(context const& where, std::string const& target) {
    auto const address = detail::address_of(where.settings);
    if (!address) {
        std::println(stderr, "llmgate: {}", address.error());
        return exit_failure;
    }
    auto const answer = gateway_verified(*address) ? process::http_get(address->host, address->port, target, detail::key_headers(where.settings), statusline_limit * 5) : std::nullopt;
    if (!answer || answer->status != 200) {
        std::println(stderr, "llmgate: the gateway is down ({}:{})", address->host, address->port);
        return exit_failure;
    }
    std::println("{}", answer->body);
    return 0;
}

inline int run_targets(context const& where) {
    return show_from_gateway(where, "/anthropic/v1/models");
}

inline int run_route(context const& where) {
    auto const& words = where.options.passthrough;
    std::string target = "/llmgate/route?requested=";
    target += words.empty() ? std::string{} : words.front();
    target += "&session=" + where.options.session;
    return show_from_gateway(where, target);
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

inline statusline::snapshot fetch_served(arguments::gateway_address const& address, context const& where, statusline::snapshot now) {
    if (now.session_id.empty()) {
        return now;
    }
    auto const view = process::http_get(address.host, address.port, "/llmgate/session?id=" + now.session_id, detail::key_headers(where.settings), statusline_limit);
    if (view && view->status == 200) {
        now = arguments::served_into(std::move(now), view->body);
    }
    now.profile = statusline::profile_of(detail::variable("CLAUDE_CONFIG_DIR").value_or(std::string{}));
    return now;
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
            now = fetch_served(*address, where, std::move(now));
        }
    }
    if (!now.directory.empty()) {
        now.branch = detail::branch_of(now.directory);
    }
    std::print("{}", statusline::render(now));
    return 0;
}

}  // namespace llmgate::commands

namespace llmgate::commands::detail {

std::expected<std::optional<std::string>, std::string> configured_client_key(gateway::config const& settings) {
    if (settings.client_key_env.empty()) {
        return std::nullopt;
    }
    auto key = variable(settings.client_key_env);
    if (!key) {
        return std::unexpected{std::format("client_key_env names {}, which is not set", settings.client_key_env)};
    }
    return key;
}

std::optional<std::string> forwarding_conflict(gateway::config const& settings, std::string const& host, bool const has_key) {
    auto const providers = gateway::effective_providers(settings);
    auto const forwarding = std::ranges::find_if(providers, [](auto const& entry) { return entry.second.forward_credentials; });
    if (forwarding == providers.end()) {
        return std::nullopt;
    }
    if (has_key) {
        return std::format("provider {} forwards the client's credentials, which cannot also carry the gateway key: remove client_key_env", forwarding->first);
    }
    if (!routing::is_loopback_host(host)) {
        return std::format("provider {} forwards the client's credentials, so the gateway must listen on a loopback address, not {}", forwarding->first, host);
    }
    return std::nullopt;
}

outcome require_key_off_loopback(std::string const& host, bool const has_key) {
    if (!routing::is_loopback_host(host) && !has_key) {
        return std::unexpected{std::format("{} is not a loopback address: set client_key_env to require a client key", host)};
    }
    return {};
}

std::vector<std::string> allowed_hosts_for(gateway::config const& settings, std::string const& host, std::uint16_t const port) {
    auto hosts = routing::default_allowed_hosts(host, port);
    hosts.insert(hosts.end(), settings.allowed_hosts.begin(), settings.allowed_hosts.end());
    return hosts;
}

}  // namespace llmgate::commands::detail

export namespace llmgate::commands {

inline std::expected<gateway::access_policy, std::string> make_policy(gateway::config const& settings, std::string const& host,
                                                                      std::uint16_t const port) {
    auto const key = detail::configured_client_key(settings);
    if (!key) {
        return std::unexpected{key.error()};
    }
    if (auto const conflict = detail::forwarding_conflict(settings, host, key->has_value())) {
        return std::unexpected{*conflict};
    }
    if (auto const open = detail::require_key_off_loopback(host, key->has_value()); !open) {
        return std::unexpected{open.error()};
    }
    auto identity = identity_key();
    if (!identity) {
        return std::unexpected{identity.error()};
    }
    return gateway::access_policy{.client_key = key->value_or(std::string{}),
                                  .allowed_hosts = detail::allowed_hosts_for(settings, host, port),
                                  .identity_key = std::move(*identity),
                                  .identity_scope = std::format("{}:{}", host, port)};
}

inline std::optional<gateway::copilot_config> copilot_login_target(gateway::config const& settings) {
    return detail::copilot_target(settings);
}

}  // namespace llmgate::commands
