module;

export module llmgate.config;

import std;
import llmgate.claude;
import llmgate.json;
import llmgate.selection;
import llmgate.shutdown;
import llmgate.tls;
import llmgate.usage;

export namespace llmgate::gateway {

using std::chrono::milliseconds;

struct timeouts {
    milliseconds connect = std::chrono::seconds{10};
    milliseconds response_head = std::chrono::minutes{2};
    milliseconds stream_idle = std::chrono::minutes{10};
    milliseconds drain = shutdown::default_drain;
};

struct upstream_config {
    [[= json::required]] std::string base_url{};
    std::string messages_path = "/v1/messages";
    std::map<std::string, std::string> headers{};
};

constexpr std::string_view vscode_copilot_client_id = "Iv1.b507a08c87ecfe98";

struct copilot_config {
    std::string token_file{};
    std::string client_id = std::string{vscode_copilot_client_id};
    std::string github_url = "https://github.com";
    std::string api_url = "https://api.github.com";
    std::optional<milliseconds> poll_interval{};
};

struct provider_config {
    [[= json::required]] std::string base_url{};
    std::string messages_path = "/v1/messages";
    std::map<std::string, std::string> headers{};
    std::string static_token_env{};
    std::optional<copilot_config> copilot{};
    std::map<std::string, std::string> models{};
};

struct fallback_config {
    unsigned failures = selection::default_failure_threshold;
    milliseconds cooldown = milliseconds{selection::default_cooldown_ms};
};

constexpr std::string_view implicit_provider = "default";

struct usage_config {
    std::string ledger_file = "~/.local/state/llmgate/usage.jsonl";
    usage::budget budget{};
};

struct config {
    [[= json::required]] std::string listen{};
    upstream_config upstream{};
    std::map<std::string, std::string> models{};
    std::map<std::string, provider_config> providers{};
    std::map<std::string, std::vector<std::string>> routes{};
    fallback_config fallback{};
    timeouts time{};
    tls::group_policy tls_groups = tls::group_policy::post_quantum_preferred;
    std::optional<copilot_config> copilot{};
    std::string static_token_env{};
    std::string client_key_env{};
    std::vector<std::string> allowed_hosts{};
    claude::launch_config claude{};
    usage_config usage{};
};

}  // namespace llmgate::gateway
