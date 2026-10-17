#include <cstdio>
#include <cstdlib>

import std;
import llmgate.arguments;
import llmgate.async;
import llmgate.commands;
import llmgate.copilot;
import llmgate.gateway;
import llmgate.httpio;
import llmgate.json;
import llmgate.net;
import llmgate.process;
import llmgate.routing;
import llmgate.shutdown;
import llmgate.tls;

namespace {

using namespace llmgate;

constexpr std::string_view version_text = "llmgate " LLMGATE_VERSION;
constexpr std::string_view usage_text =
    "usage: llmgate [--config PATH] [serve | login | claude ARGS... | env [--json|--vscode] | setup [--service] [--dry-run] [--vscode FILE] | status | stop | "
    "usage [--days N] [--json] | doctor [--offline] [--json] | quota [--json] | statusline] | --check-config | --version";
constexpr int exit_usage = 2;
constexpr int exit_config = 3;
constexpr int exit_runtime = 1;

std::expected<gateway::config, std::string> load_config(std::string_view const path) {
    std::ifstream in{process::expand_home(path)};
    if (!in) {
        return std::unexpected{std::format("cannot read {}", path)};
    }
    std::string const text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    auto loaded = json::read_text<gateway::config>(text);
    if (!loaded) {
        return std::unexpected{std::format("{}: {} ({})", path, loaded.error().message, loaded.error().path)};
    }
    return std::move(*loaded);
}

using credential_store = std::map<std::string, std::unique_ptr<gateway::credential>>;

std::unique_ptr<gateway::credential> make_credential(async::reactor& loop, tls::client_context const& tls_context, gateway::provider_config const& provider) {
    if (provider.copilot) {
        return std::make_unique<copilot::copilot_credential>(loop, tls_context, *provider.copilot);
    }
    auto const* token = provider.static_token_env.empty() ? nullptr : std::getenv(provider.static_token_env.c_str());
    return std::make_unique<gateway::static_credential>(token == nullptr ? std::string{} : std::string{token});
}

credential_store make_credentials(async::reactor& loop, tls::client_context const& tls_context, gateway::config const& settings) {
    credential_store store;
    for (auto const& [name, provider] : gateway::effective_providers(settings)) {
        store.emplace(name, make_credential(loop, tls_context, provider));
    }
    return store;
}

gateway::credential_map borrowed(credential_store const& store) {
    gateway::credential_map all;
    for (auto const& [name, owned] : store) {
        all.emplace(name, owned.get());
    }
    return all;
}

int fail(int const code, std::string_view const message) {
    std::println(stderr, "llmgate: {}", message);
    return code;
}

void attach_ledger(gateway::proxy& proxy, gateway::config const& settings) {
    auto const sink = commands::ledger_sink(settings.usage.ledger_file);
    if (!sink) {
        return;
    }
    auto history = commands::read_ledger(settings.usage.ledger_file);
    if (history.skipped != 0) {
        std::println(stderr, "llmgate: {} unreadable usage ledger lines were skipped", history.skipped);
    }
    proxy.use_ledger(std::move(history.loaded));
    proxy.persist_usage_with(sink);
}

void report_abandoned(std::size_t const abandoned, std::chrono::milliseconds const drain) {
    if (abandoned != 0) {
        std::println(stderr, "llmgate: stopped with {} requests still running after the {} s drain", abandoned,
                     std::chrono::duration_cast<std::chrono::seconds>(drain).count());
    }
}

int serve_on(async::reactor& loop, gateway::config const& settings, std::string const& host, tls::client_context const& tls_context,
             net::listener& listening) {
    auto policy = commands::make_policy(settings, host, listening.port());
    if (!policy) {
        return fail(exit_config, policy.error());
    }
    if (auto const problem = gateway::config_problem(settings); !problem.empty()) {
        return fail(exit_config, problem);
    }
    auto credentials = make_credentials(loop, tls_context, settings);
    gateway::proxy proxy{loop, settings, std::move(*policy), borrowed(credentials), tls_context};
    attach_ledger(proxy, settings);
    auto stop = shutdown::stop_signal::make();
    if (!stop || stop->install()) {
        return fail(exit_runtime, "cannot install the stop signal handlers");
    }
    std::println("llmgate listening on {}:{}", host, listening.port());
    std::fflush(stdout);
    report_abandoned(loop.run(httpio::serve_until(listening, proxy, httpio::limits{}, stop->descriptor(), settings.time.drain)), settings.time.drain);
    commands::release_pid_file();
    std::println("llmgate stopped");
    return 0;
}

int serve(gateway::config settings) {
    auto const address = arguments::split_listen(settings.listen);
    if (!address) {
        return fail(exit_config, "listen must look like 127.0.0.1:9480");
    }
    async::reactor loop;
    auto tls_context = tls::client_context::make(settings.tls_groups);
    if (!tls_context) {
        return fail(exit_runtime, std::format("cannot start: {}", tls_context.error().message()));
    }
    auto listening = net::listener::bind(loop, address->host, address->port);
    if (!listening) {
        return fail(exit_runtime, std::format("cannot start: {}", listening.error().message()));
    }
    return serve_on(loop, settings, address->host, *tls_context, *listening);
}

int save_login(gateway::copilot_config const& target, std::string const& token) {
    if (auto const saved = copilot::save_oauth_token(target.token_file, token); !saved) {
        return fail(exit_runtime, std::format("cannot save the token: {}", saved.error().message()));
    }
    std::println("Logged in. The token is stored in {}", target.token_file);
    return 0;
}

int await_login(async::reactor& loop, tls::client_context const& tls_context, gateway::copilot_config const& target, copilot::device_code const& code) {
    std::println("Open {} and enter the code {}", code.verification_uri, code.user_code);
    std::println("Waiting for you to authorise...");
    std::fflush(stdout);
    auto const token = loop.run(copilot::wait_for_authorisation(loop, tls_context, target, code));
    if (!token) {
        return fail(exit_runtime, std::format("login failed: {}", token.error().message()));
    }
    return save_login(target, *token);
}

int request_login(async::reactor& loop, tls::client_context const& tls_context, gateway::copilot_config const& target) {
    auto const code = loop.run(copilot::request_device_code(loop, tls_context, target));
    if (!code) {
        return fail(exit_runtime, std::format("cannot reach GitHub: {}", code.error().message()));
    }
    return await_login(loop, tls_context, target, *code);
}

int login_with(gateway::config const& settings, gateway::copilot_config const& target) {
    async::reactor loop;
    auto tls_context = tls::client_context::make(settings.tls_groups);
    if (!tls_context) {
        return exit_runtime;
    }
    return request_login(loop, *tls_context, target);
}

int login(gateway::config const& settings) {
    auto const target = commands::copilot_login_target(settings);
    if (!target) {
        return fail(exit_config, "set copilot.token_file (or a provider's copilot.token_file) in the configuration first");
    }
    return login_with(settings, *target);
}

int serve_command(commands::context const& where) {
    return serve(where.settings);
}

int login_command(commands::context const& where) {
    return login(where.settings);
}

int statusline_command(commands::context const& where) {
    return commands::run_statusline(where, std::string{std::istreambuf_iterator<char>{std::cin}, std::istreambuf_iterator<char>{}});
}

using command_handler = int (*)(commands::context const&);

constexpr auto handlers = std::to_array<std::pair<arguments::command, command_handler>>({
    {arguments::command::serve, &serve_command},
    {arguments::command::login, &login_command},
    {arguments::command::claude, &commands::run_claude},
    {arguments::command::env, &commands::run_env},
    {arguments::command::setup, &commands::run_setup},
    {arguments::command::status, &commands::run_status},
    {arguments::command::stop, &commands::run_stop},
    {arguments::command::usage, &commands::run_usage},
    {arguments::command::doctor, &commands::run_doctor},
    {arguments::command::quota, &commands::run_quota},
    {arguments::command::statusline, &statusline_command},
});

int dispatch(commands::context const& where) {
    return std::ranges::find(handlers, where.options.which, &std::pair<arguments::command, command_handler>::first)->second(where);
}

bool tolerates_missing_config(arguments::command const which) {
    return which == arguments::command::statusline || which == arguments::command::stop || which == arguments::command::doctor;
}

int check_config(std::expected<gateway::config, std::string> const& settings) {
    auto const problem = settings ? gateway::config_problem(*settings) : std::string{};
    if (!problem.empty()) {
        return fail(exit_config, problem);
    }
    std::println("configuration is valid");
    return 0;
}

commands::context make_context(arguments::options const& parsed, std::expected<gateway::config, std::string> settings, char const* const program) {
    return {.options = parsed,
            .settings = settings ? std::move(*settings) : gateway::config{},
            .binary = process::current_executable().value_or(std::filesystem::path{program}),
            .config_error = settings ? std::string{} : settings.error()};
}

int run(arguments::options const& parsed, char const* const program) {
    auto settings = load_config(parsed.config_path);
    if (!settings && !tolerates_missing_config(parsed.which)) {
        return fail(exit_config, settings.error());
    }
    if (parsed.check_config) {
        return check_config(settings);
    }
    return dispatch(make_context(parsed, std::move(settings), program));
}

}  // namespace

int main(int argc, char** argv) {
    auto const parsed = arguments::parse(std::vector<std::string>(argv + 1, argv + argc));
    if (!parsed) {
        std::println(stderr, "llmgate: {}\n{}", parsed.error(), usage_text);
        return exit_usage;
    }
    if (parsed->show_version) {
        std::println("{}", version_text);
        return 0;
    }
    return run(*parsed, argv[0]);
}
