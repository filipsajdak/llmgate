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
constexpr std::string_view models_target = "/models";
constexpr std::string_view user_agent = "llmgate/0.0";

void add_missing(http::header_map& fields, std::string_view const name, std::string_view const value) {
    if (!fields.find(name)) {
        fields.add(std::string{name}, std::string{value});
    }
}

}  // namespace

async::task<std::expected<fetch::reply, std::string>> fetch_models(async::reactor& loop, tls::client_context const& tls_context, gateway::provider_config const& provider,
                                                                     gateway::credential& credential) {
    if (provider.forward_credentials) {
        co_return std::unexpected{std::string{"this provider forwards the client's own login, so llmgate holds no credential to list its models with"}};
    }
    auto bearer = co_await credential.bearer();
    if (!bearer) {
        co_return std::unexpected{std::format("no upstream credential: {}", bearer.error().message())};
    }
    http::header_map fields;
    for (auto const& [name, value] : provider.headers) {
        fields.add(name, value);
    }
    if (provider.copilot) {
        add_missing(fields, "Editor-Version", gateway::vscode_editor_version);
        add_missing(fields, "Copilot-Integration-Id", gateway::vscode_integration_id);
    }
    fields.add("Authorization", std::format("Bearer {}", *bearer));
    fields.add("Accept", "application/json");
    fields.add("User-Agent", std::string{user_agent});
    auto answer = co_await fetch::request(loop, tls_context, provider.base_url, {.method = "GET", .target = std::string{models_target}, .host = {}, .fields = std::move(fields)}, list_limit);
    if (!answer) {
        co_return std::unexpected{std::format("the provider did not answer: {}", answer.error().message())};
    }
    co_return std::move(*answer);
}

}  // namespace llmgate::discovery
