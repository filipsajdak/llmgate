#include <cstdio>
#include <cstdlib>

import std;
import llmgate.async;
import llmgate.copilot;
import llmgate.gateway;
import llmgate.httpio;
import llmgate.json;
import llmgate.net;
import llmgate.routing;
import llmgate.tls;

namespace {

using namespace llmgate;

constexpr std::string_view version_text = "llmgate " LLMGATE_VERSION;
constexpr std::string_view default_config = "~/.config/llmgate/config.jsonc";
constexpr int exit_usage = 2;
constexpr int exit_config = 3;
constexpr int exit_runtime = 1;

struct arguments {
    std::string command = "serve";
    std::string config_path = std::string{default_config};
    bool check_only = false;
    bool show_version = false;
};

std::optional<arguments> parse_arguments(std::span<char* const> const args) {
    arguments parsed;
    for (std::size_t index = 1; index < args.size(); ++index) {
        std::string_view const item{args[index]};
        if (item == "--version") {
            parsed.show_version = true;
        } else if (item == "--check-config") {
            parsed.check_only = true;
        } else if (item == "--config" && index + 1 < args.size()) {
            parsed.config_path = args[++index];
        } else if (item == "serve" || item == "login") {
            parsed.command = item;
        } else {
            return std::nullopt;
        }
    }
    return parsed;
}

std::string expand_home(std::string_view const path) {
    if (!path.starts_with("~/")) {
        return std::string{path};
    }
    auto const* home = std::getenv("HOME");
    return (home == nullptr ? std::string{} : std::string{home}) + std::string{path.substr(1)};
}

std::expected<gateway::config, std::string> load_config(std::string_view const path) {
    std::ifstream in{expand_home(path)};
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

std::optional<std::pair<std::string, std::uint16_t>> split_listen(std::string_view const listen) {
    auto const colon = listen.rfind(':');
    std::uint16_t port = 0;
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    auto const digits = listen.substr(colon + 1);
    if (std::from_chars(digits.data(), digits.data() + digits.size(), port).ec != std::errc{}) {
        return std::nullopt;
    }
    return std::pair{std::string{listen.substr(0, colon)}, port};
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
    auto hosts = routing::default_allowed_hosts(host, port);
    hosts.insert(hosts.end(), settings.allowed_hosts.begin(), settings.allowed_hosts.end());
    return gateway::access_policy{.client_key = key == nullptr ? std::string{} : std::string{key}, .allowed_hosts = std::move(hosts)};
}

int serve(gateway::config settings) {
    auto const address = split_listen(settings.listen);
    if (!address) {
        std::println(stderr, "llmgate: listen must look like 127.0.0.1:9480");
        return exit_config;
    }
    async::reactor loop;
    auto tls_context = tls::client_context::make(settings.tls_groups);
    auto listening = net::listener::bind(loop, address->first, address->second);
    if (!tls_context || !listening) {
        std::println(stderr, "llmgate: cannot start: {}", !tls_context ? tls_context.error().message() : listening.error().message());
        return exit_runtime;
    }
    auto policy = make_policy(settings, address->first, listening->port());
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
    std::println("llmgate listening on {}:{}", address->first, listening->port());
    std::fflush(stdout);
    loop.run(httpio::serve(*listening, proxy, httpio::limits{}));
    return exit_runtime;
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

}  // namespace

int main(int argc, char** argv) {
    auto const parsed = parse_arguments(std::span<char* const>{argv, static_cast<std::size_t>(argc)});
    if (!parsed) {
        std::println(stderr, "usage: llmgate [--config PATH] [serve | login] | --check-config | --version");
        return exit_usage;
    }
    if (parsed->show_version) {
        std::println("{}", version_text);
        return 0;
    }
    auto settings = load_config(parsed->config_path);
    if (!settings) {
        std::println(stderr, "llmgate: {}", settings.error());
        return exit_config;
    }
    if (parsed->check_only) {
        std::println("configuration is valid");
        return 0;
    }
    return parsed->command == "login" ? login(*settings) : serve(std::move(*settings));
}
