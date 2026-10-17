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

std::unique_ptr<gateway::credential> make_credential(async::reactor& loop, tls::client_context const& tls_context,
                                                     gateway::config const& settings) {
    if (settings.copilot) {
        return std::make_unique<copilot::copilot_credential>(loop, tls_context, *settings.copilot);
    }
    auto const* token = settings.static_token_env.empty() ? nullptr : std::getenv(settings.static_token_env.c_str());
    return std::make_unique<gateway::static_credential>(token == nullptr ? std::string{} : std::string{token});
}

std::expected<gateway::access_policy, std::string> make_policy(gateway::config const& settings, std::string const& host,
                                                               std::uint16_t const port) {
    auto const* key = settings.client_key_env.empty() ? nullptr : std::getenv(settings.client_key_env.c_str());
    if (!settings.client_key_env.empty() && (key == nullptr || *key == '\0')) {
        return std::unexpected{std::format("client_key_env names {}, which is not set", settings.client_key_env)};
    }
    if (!routing::is_loopback_host(host) && key == nullptr) {
        return std::unexpected{std::format("{} is not a loopback address: set client_key_env to require a client key", host)};
    }
    auto identity = commands::identity_key();
    if (!identity) {
        return std::unexpected{identity.error()};
    }
    auto hosts = routing::default_allowed_hosts(host, port);
    hosts.insert(hosts.end(), settings.allowed_hosts.begin(), settings.allowed_hosts.end());
    return gateway::access_policy{.client_key = key == nullptr ? std::string{} : std::string{key}, .allowed_hosts = std::move(hosts), .identity_key = std::move(*identity),
                                  .identity_scope = std::format("{}:{}", host, port)};
}

int serve(gateway::config settings) {
    auto const address = arguments::split_listen(settings.listen);
    if (!address) {
        std::println(stderr, "llmgate: listen must look like 127.0.0.1:9480");
        return exit_config;
    }
    async::reactor loop;
    auto tls_context = tls::client_context::make(settings.tls_groups);
    auto listening = net::listener::bind(loop, address->host, address->port);
    if (!tls_context || !listening) {
        std::println(stderr, "llmgate: cannot start: {}", !tls_context ? tls_context.error().message() : listening.error().message());
        return exit_runtime;
    }
    auto policy = make_policy(settings, address->host, listening->port());
    if (!policy) {
        std::println(stderr, "llmgate: {}", policy.error());
        return exit_config;
    }
    auto credential = make_credential(loop, *tls_context, settings);
    gateway::proxy proxy{loop, settings, std::move(*policy), *credential, *tls_context};
    if (!proxy.configured()) {
        std::println(stderr, "llmgate: upstream.base_url is not a valid http or https URL");
        return exit_config;
    }
    if (auto const sink = commands::ledger_sink(settings.usage.ledger_file)) {
        auto history = commands::read_ledger(settings.usage.ledger_file);
        if (history.skipped != 0) {
            std::println(stderr, "llmgate: {} unreadable usage ledger lines were skipped", history.skipped);
        }
        proxy.use_ledger(std::move(history.loaded));
        proxy.persist_usage_with(sink);
    }
    auto stop = shutdown::stop_signal::make();
    if (!stop || stop->install()) {
        std::println(stderr, "llmgate: cannot install the stop signal handlers");
        return exit_runtime;
    }
    std::println("llmgate listening on {}:{}", address->host, listening->port());
    std::fflush(stdout);
    auto const abandoned = loop.run(httpio::serve_until(*listening, proxy, httpio::limits{}, stop->descriptor(), settings.time.drain));
    if (abandoned != 0) {
        std::println(stderr, "llmgate: stopped with {} requests still running after the {} s drain", abandoned,
                     std::chrono::duration_cast<std::chrono::seconds>(settings.time.drain).count());
    }
    commands::release_pid_file();
    std::println("llmgate stopped");
    return 0;
}

int login(gateway::config const& settings) {
    if (!settings.copilot || settings.copilot->token_file.empty()) {
        std::println(stderr, "llmgate: set copilot.token_file in the configuration first");
        return exit_config;
    }
    async::reactor loop;
    auto tls_context = tls::client_context::make(settings.tls_groups);
    if (!tls_context) {
        return exit_runtime;
    }
    auto const code = loop.run(copilot::request_device_code(loop, *tls_context, *settings.copilot));
    if (!code) {
        std::println(stderr, "llmgate: cannot reach GitHub: {}", code.error().message());
        return exit_runtime;
    }
    std::println("Open {} and enter the code {}", code->verification_uri, code->user_code);
    std::println("Waiting for you to authorise...");
    std::fflush(stdout);
    auto const token = loop.run(copilot::wait_for_authorisation(loop, *tls_context, *settings.copilot, *code));
    if (!token) {
        std::println(stderr, "llmgate: login failed: {}", token.error().message());
        return exit_runtime;
    }
    if (auto const saved = copilot::save_oauth_token(settings.copilot->token_file, *token); !saved) {
        std::println(stderr, "llmgate: cannot save the token: {}", saved.error().message());
        return exit_runtime;
    }
    std::println("Logged in. The token is stored in {}", settings.copilot->token_file);
    return 0;
}

int dispatch(commands::context const& where) {
    switch (where.options.which) {
        case arguments::command::serve:
            return serve(where.settings);
        case arguments::command::login:
            return login(where.settings);
        case arguments::command::claude:
            return commands::run_claude(where);
        case arguments::command::env:
            return commands::run_env(where);
        case arguments::command::setup:
            return commands::run_setup(where);
        case arguments::command::status:
            return commands::run_status(where);
        case arguments::command::stop:
            return commands::run_stop(where);
        case arguments::command::usage:
            return commands::run_usage(where);
        case arguments::command::doctor:
            return commands::run_doctor(where);
        case arguments::command::quota:
            return commands::run_quota(where);
        case arguments::command::statusline:
            break;
    }
    return commands::run_statusline(where, std::string{std::istreambuf_iterator<char>{std::cin}, std::istreambuf_iterator<char>{}});
}

}  // namespace

int main(int argc, char** argv) {
    auto const parsed = arguments::parse(std::vector<std::string>(argv + 1, argv + argc));
    if (!parsed) {
        std::println(stderr, "llmgate: {}\nusage: llmgate [--config PATH] [serve | login | claude ARGS... | env [--json|--vscode] | setup [--service] [--dry-run] [--vscode FILE] | status | stop | usage [--days N] [--json] | doctor [--offline] [--json] | quota [--json] | statusline] | --check-config | --version", parsed.error());
        return exit_usage;
    }
    if (parsed->show_version) {
        std::println("{}", version_text);
        return 0;
    }
    auto settings = load_config(parsed->config_path);
    auto const tolerant = parsed->which == arguments::command::statusline || parsed->which == arguments::command::stop || parsed->which == arguments::command::doctor;
    if (!settings && !tolerant) {
        std::println(stderr, "llmgate: {}", settings.error());
        return exit_config;
    }
    if (parsed->check_config) {
        std::println("configuration is valid");
        return 0;
    }
    commands::context where{.options = *parsed,
                            .settings = settings ? std::move(*settings) : gateway::config{},
                            .binary = process::current_executable().value_or(std::filesystem::path{argv[0]}),
                            .config_error = settings ? std::string{} : settings.error()};
    return dispatch(where);
}
