module llmgate.discovery;

import std;
import llmgate.async;
import llmgate.fetch;
import llmgate.gateway;
import llmgate.http;
import llmgate.httpio;
import llmgate.tls;

namespace llmgate::discovery {

namespace {

constexpr std::chrono::milliseconds list_limit = std::chrono::seconds{30};
constexpr int http_not_found = 404;
constexpr std::string_view models_target = "/models";
constexpr std::string_view messages_suffix = "/messages";
constexpr std::string_view anthropic_version = "2023-06-01";
constexpr std::string_view user_agent = "llmgate/0.0";

void add_missing(http::header_map& fields, std::string_view const name, std::string_view const value) {
    if (!fields.find(name)) {
        fields.add(std::string{name}, std::string{value});
    }
}

void add_authentication(http::header_map& fields, gateway::auth_config const& auth, std::string_view const token) {
    if (auth.kind == gateway::auth_kind::bearer) {
        fields.add("Authorization", std::format("Bearer {}", token));
    } else if (auth.kind == gateway::auth_kind::header) {
        fields.add(auth.header, auth.scheme.empty() ? std::string{token} : std::format("{} {}", auth.scheme, token));
    }
}

}  // namespace

std::string models_target_of(gateway::provider_config const& provider) {
    bool const openai_style = provider.copilot || provider.chatgpt || provider.preset == gateway::preset_kind::copilot || provider.preset == gateway::preset_kind::chatgpt ||
                              provider.protocol == gateway::wire_protocol::openai_responses;
    if (!openai_style && provider.messages_path.ends_with(messages_suffix)) {
        return provider.messages_path.substr(0, provider.messages_path.size() - messages_suffix.size()) + "/models";
    }
    return std::string{models_target};
}

namespace {

http::header_map headers_for(gateway::provider_config const& provider, std::string_view const token) {
    http::header_map fields;
    for (auto const& [name, value] : provider.headers) {
        fields.add(name, value);
    }
    if (provider.copilot) {
        add_missing(fields, "Editor-Version", gateway::vscode_editor_version);
        add_missing(fields, "Copilot-Integration-Id", gateway::vscode_integration_id);
    }
    add_authentication(fields, provider.auth, token);
    if (provider.protocol == gateway::wire_protocol::anthropic && !provider.copilot) {
        add_missing(fields, "anthropic-version", anthropic_version);
    }
    fields.add("Accept", "application/json");
    fields.add("User-Agent", std::string{user_agent});
    return fields;
}

bool needs_secret(gateway::provider_config const& provider) {
    return provider.auth.kind == gateway::auth_kind::bearer || provider.auth.kind == gateway::auth_kind::header;
}

}  // namespace

async::task<std::expected<fetch::reply, std::string>> fetch_models(async::reactor& loop, tls::client_context const& tls_context, gateway::provider_config const& provider,
                                                                     gateway::credential& credential) {
    if (provider.forward_credentials) {
        co_return std::unexpected{std::string{"this provider forwards the client's own login, so llmgate holds no credential to list its models with"}};
    }
    std::string token;
    if (needs_secret(provider)) {
        auto bearer = co_await credential.bearer();
        if (!bearer) {
            co_return std::unexpected{std::format("no upstream credential: {}", bearer.error().message())};
        }
        token = std::move(*bearer);
    }
    auto const target = models_target_of(provider);
    auto answer = co_await fetch::request(loop, tls_context, provider.base_url, {.method = "GET", .target = target, .host = {}, .fields = headers_for(provider, token)}, list_limit);
    if (answer && answer->status == http_not_found && target != models_target) {
        answer = co_await fetch::request(loop, tls_context, provider.base_url, {.method = "GET", .target = std::string{models_target}, .host = {}, .fields = headers_for(provider, token)}, list_limit);
    }
    if (!answer) {
        co_return std::unexpected{std::format("the provider did not answer: {}", answer.error().message())};
    }
    co_return std::move(*answer);
}

}  // namespace llmgate::discovery
