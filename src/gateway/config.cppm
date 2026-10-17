module;

export module llmgate.config;

import std;
import llmgate.claude;
import llmgate.doc;
import llmgate.json;
import llmgate.selection;
import llmgate.shutdown;
import llmgate.tls;
import llmgate.usage;

export namespace llmgate::gateway {

using std::chrono::milliseconds;

struct timeouts {
    [[= reflect::doc{"How long a connect to a provider may take. A duration is a number with the unit ms, s, m or h."}]]
    milliseconds connect = std::chrono::seconds{10};
    [[= reflect::doc{"How long a provider may take to send the response head before the attempt counts as failed and the next provider is tried."}]]
    milliseconds response_head = std::chrono::minutes{2};
    [[= reflect::doc{"How long a streaming response may stay silent before the gateway cuts it."}]]
    milliseconds stream_idle = std::chrono::minutes{10};
    [[= reflect::doc{"How long a stop (SIGINT or SIGTERM) lets running requests finish; a second signal exits at once."}]]
    milliseconds drain = shutdown::default_drain;
};

struct upstream_config {
    [[= json::required]] [[= reflect::doc{"Base URL of the single upstream; together with the other top-level keys it acts as one provider named default."}]]
    [[= reflect::example{R"("https://api.githubcopilot.com")"}]] std::string base_url{};
    [[= reflect::doc{"Path of the Messages endpoint on the upstream."}]] std::string messages_path = "/v1/messages";
    [[= reflect::doc{"Extra headers sent to the upstream on every request."}]]
    [[= reflect::example{R"({ "Editor-Version": "vscode/1.99.0", "Copilot-Integration-Id": "vscode-chat" })"}]]
    std::map<std::string, std::string> headers{};
};

constexpr std::string_view vscode_copilot_client_id = "Iv1.b507a08c87ecfe98";

struct copilot_config {
    [[= reflect::doc{"File that holds the GitHub OAuth token; run llmgate login once to write it."}]]
    [[= reflect::example{R"("~/.config/llmgate/copilot-token.json")"}]] std::string token_file{};
    [[= reflect::doc{"OAuth client id of the device login; the default is the one the VS Code Copilot extension uses."}]]
    std::string client_id = std::string{vscode_copilot_client_id};
    [[= reflect::doc{"GitHub web address used by the device login."}]] std::string github_url = "https://github.com";
    [[= reflect::doc{"GitHub API address that exchanges the OAuth token for a Copilot token."}]] std::string api_url = "https://api.github.com";
    [[= reflect::doc{"Time between polls of the device login; absent means the interval GitHub names."}]] [[= reflect::example{R"("5s")"}]]
    [[= reflect::commented]] std::optional<milliseconds> poll_interval{};
};

struct provider_config {
    [[= json::required]] [[= reflect::doc{"Base URL of the provider."}]] [[= reflect::example{R"("https://api.anthropic.com")"}]] std::string base_url{};
    [[= reflect::doc{"Path of the Messages endpoint on the provider."}]] std::string messages_path = "/v1/messages";
    [[= reflect::doc{"Extra headers sent to the provider on every request."}]] std::map<std::string, std::string> headers{};
    [[= reflect::doc{"Name of the environment variable that holds the API key sent as the bearer token."}]]
    [[= reflect::example{R"("ANTHROPIC_UPSTREAM_KEY")"}]] std::string static_token_env{};
    [[= reflect::doc{"GitHub Copilot login of this provider; used instead of static_token_env."}]] std::optional<copilot_config> copilot{};
    [[= reflect::doc{"Model names this provider expects: the name Claude Code sends maps to the id here; unknown names pass through unchanged."}]]
    std::map<std::string, std::string> models{};
};

struct fallback_config {
    [[= reflect::doc{"Consecutive failures after which a provider is skipped."}]] unsigned failures = selection::default_failure_threshold;
    [[= reflect::doc{"How long a skipped provider stays skipped; then one request probes it."}]]
    milliseconds cooldown = milliseconds{selection::default_cooldown_ms};
};

constexpr std::string_view implicit_provider = "default";

struct usage_config {
    [[= reflect::doc{"File that receives one line per request; the gateway reads it back at start-up."}]]
    std::string ledger_file = "~/.local/state/llmgate/usage.jsonl";
    [[= reflect::doc{"Token budgets; 0 means no limit, and counted tokens are input + output + cache writes."}]] usage::budget budget{};
};

struct config {
    [[= json::required]] [[= reflect::doc{"Address the gateway listens on; point Claude Code at it with ANTHROPIC_BASE_URL=http://ADDRESS/anthropic."}]]
    [[= reflect::example{R"("127.0.0.1:9480")"}]] std::string listen{};
    [[= reflect::doc{"The single backend of an older configuration; it keeps working and acts as one provider named default."}]] upstream_config upstream{};
    [[= reflect::doc{"Claude Code expands aliases such as sonnet to full model names before it calls the gateway, so map full names: client model name to upstream model name. Unknown names go through unchanged."}]]
    [[= reflect::example{R"({ "claude-opus-5-5": "claude-opus-5.5", "claude-sonnet-5-5": "claude-sonnet-5.5", "claude-haiku-5-5": "claude-haiku-5.5", "claude-haiku-4-5": "claude-haiku-4.5" })"}]]
    std::map<std::string, std::string> models{};
    [[= reflect::doc{"More than one backend: name each one here. A provider with a copilot object uses the Copilot login, one with static_token_env uses that variable. The keys upstream, models, copilot and static_token_env stay valid and act as one provider named default."}]]
    [[= reflect::example{R"({
  "copilot": { "base_url": "https://api.githubcopilot.com", "copilot": { "token_file": "~/.config/llmgate/copilot-token.json" },
               "headers": { "Editor-Version": "vscode/1.99.0", "Copilot-Integration-Id": "vscode-chat" },
               "models": { "claude-opus-5-5": "claude-opus-5.5" } },
  "anthropic": { "base_url": "https://api.anthropic.com", "static_token_env": "ANTHROPIC_UPSTREAM_KEY" }
})"}]]
    [[= reflect::commented]] std::map<std::string, provider_config> providers{};
    [[= reflect::doc{"Per role (opus, sonnet, haiku, other) the providers to try in order. A role without a list uses the other list, then every provider."}]]
    [[= reflect::example{R"({ "opus": ["anthropic", "copilot"], "sonnet": ["copilot", "anthropic"], "other": ["copilot"] })"}]]
    [[= reflect::commented]] std::map<std::string, std::vector<std::string>> routes{};
    [[= reflect::doc{"A provider that fails before any response byte reaches Claude Code (connect error, no response head in time, 5xx, 429) is retried on the next one of the role's list; after failures consecutive failures it is skipped for cooldown."}]]
    fallback_config fallback{};
    [[= reflect::doc{"Time limits of the gateway."}]] timeouts time{};
    [[= reflect::doc{"TLS key exchange groups offered to the providers."}]] tls::group_policy tls_groups = tls::group_policy::post_quantum_preferred;
    [[= reflect::doc{"GitHub Copilot device login of the default provider; run llmgate login once."}]] std::optional<copilot_config> copilot{};
    [[= reflect::doc{"Name of the environment variable that holds the API key of the default provider."}]]
    [[= reflect::example{R"("ANTHROPIC_UPSTREAM_KEY")"}]] [[= reflect::commented]] std::string static_token_env{};
    [[= reflect::doc{"Name of the environment variable that holds the key clients must send; required when listen is not a loopback address."}]]
    [[= reflect::example{R"("LLMGATE_CLIENT_KEY")"}]] [[= reflect::commented]] std::string client_key_env{};
    [[= reflect::doc{"Host names, besides the listen address, accepted in the Host header of a request."}]]
    [[= reflect::example{R"(["gateway.internal:9480"])"}]] [[= reflect::commented]] std::vector<std::string> allowed_hosts{};
    [[= reflect::doc{"What llmgate claude and llmgate env give Claude Code: model names per role, context window and profile."}]] claude::launch_config claude{};
    [[= reflect::doc{"Usage ledger and token budgets."}]] usage_config usage{};
};

}  // namespace llmgate::gateway
