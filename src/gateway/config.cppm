module;

export module llmgate.config;

import std;
import llmgate.claude;
import llmgate.doc;
import llmgate.json;
import llmgate.reflect;
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
    [[= reflect::doc{"How long a streaming response may stay silent before the gateway cuts it with an error event, so that the client can retry. Anthropic servers send a ping every few seconds, so a silence of two minutes is a stall; raise it for a provider that streams nothing while it reasons."}]]
    milliseconds stream_idle = std::chrono::minutes{2};
    [[= reflect::doc{"How long a stop (SIGINT or SIGTERM) lets running requests finish; a second signal exits at once."}]]
    milliseconds drain = shutdown::default_drain;
};

constexpr std::uint16_t default_callback_port = 1455;

struct chatgpt_config {
    [[= reflect::doc{"File that holds the ChatGPT sign-in tokens (created with mode 0600); run llmgate login --provider NAME once to write it."}]]
    [[= reflect::example{R"("~/.config/llmgate/chatgpt-token.json")"}]] std::string token_file{};
    [[= reflect::doc{"Address of the OpenAI sign-in service."}]] std::string issuer = "https://auth.openai.com";
    [[= reflect::doc{"Port on 127.0.0.1 that receives the browser's callback during the sign-in."}]] std::uint16_t callback_port = default_callback_port;
    [[= reflect::doc{"Name OpenAI shows the user when this tool is registered at the first sign-in."}]] std::string agent_name = "llmgate";
};

enum class wire_protocol { anthropic, openai_responses };
static_assert(reflect::unique_values<wire_protocol>());

constexpr std::size_t preset_url_capacity = 40;

struct preset_defaults {
    char base_url[preset_url_capacity]{};
    wire_protocol protocol = wire_protocol::anthropic;

    consteval preset_defaults(std::string_view const url, wire_protocol const wire) : protocol(wire) {
        if (url.size() >= preset_url_capacity) {
            throw "gateway::preset_defaults: the address does not fit preset_url_capacity";
        }
        std::ranges::copy(url, base_url);
    }
};

enum class preset_kind {
    none,
    copilot [[= preset_defaults{"https://api.githubcopilot.com", wire_protocol::anthropic}]],
    anthropic [[= preset_defaults{"https://api.anthropic.com", wire_protocol::anthropic}]],
    chatgpt [[= preset_defaults{"https://api.openai.com/v1", wire_protocol::openai_responses}]],
    openai_chat
};
static_assert(reflect::unique_values<preset_kind>());

constexpr std::string_view default_messages_path = "/v1/messages";
constexpr std::string_view default_responses_path = "/responses";

enum class auth_kind { bearer, header, none };
static_assert(reflect::unique_values<auth_kind>());

struct auth_config {
    [[= reflect::doc{"How the provider is authenticated: bearer (Authorization: Bearer SECRET, the default), header (SECRET in the header named by header, after scheme when one is set) or none (no credential, for a local model)."}]]
    [[= reflect::example{R"("header")"}]] [[= reflect::commented]] auth_kind kind = auth_kind::bearer;
    [[= reflect::doc{"Header that carries the secret when kind is header."}]] [[= reflect::example{R"("api-key")"}]] [[= reflect::commented]] std::string header = "api-key";
    [[= reflect::doc{"Text put before the secret in that header, such as Bearer; empty sends the secret alone."}]] [[= reflect::commented]] std::string scheme{};
    [[= reflect::doc{"Name of the environment variable that holds the secret."}]] [[= reflect::example{R"("PARTNER_PROD_API_KEY")"}]] [[= reflect::commented]] std::string env{};
    [[= reflect::doc{"File that holds the secret (one line); keep it readable by you only. It is read before the environment variable."}]] [[= reflect::example{R"("~/.config/llmgate/partner.key")"}]] [[= reflect::commented]] std::string file{};
};

struct model_limits {
    [[= reflect::doc{"Context window of the model in tokens; 0 means unknown."}]] std::uint64_t context_tokens = 0;
    [[= reflect::doc{"Largest prompt the model accepts, in tokens; 0 means unknown. Published as max_input_tokens in the model list."}]] std::uint64_t prompt_tokens = 0;
    [[= reflect::doc{"Largest reply the model produces, in tokens; 0 means unknown. A request with a larger max_tokens is clamped to it, and the model list publishes it as max_tokens."}]] std::uint64_t output_tokens = 0;
};

struct upstream_config {
    [[= json::required]] [[= reflect::doc{"Base URL of the single upstream; together with the other top-level keys it acts as one provider named default."}]]
    [[= reflect::example{R"("https://api.githubcopilot.com")"}]] std::string base_url{};
    [[= reflect::doc{"Path of the Messages endpoint on the upstream."}]] std::string messages_path = "/v1/messages";
    [[= reflect::doc{"A working default for this kind of provider: copilot, anthropic, chatgpt (openai_chat is not built yet). It fills base_url when that is empty and the protocol when it is left at anthropic; whatever you set yourself wins."}]]
    [[= reflect::example{R"("chatgpt")"}]] [[= reflect::commented]] preset_kind preset = preset_kind::none;
    [[= reflect::doc{"Wire protocol of the provider: anthropic (the Messages API, the default) or openai_responses (the OpenAI Responses API; Claude Code requests are translated, see docs/translation-responses.md). With openai_responses the messages_path default becomes /responses."}]]
    wire_protocol protocol = wire_protocol::anthropic;
    [[= reflect::doc{"Extra headers sent to the upstream on every request. With a copilot login the gateway also sends Editor-Version and Copilot-Integration-Id (the VS Code values) unless they are set here."}]]
    [[= reflect::example{R"({ "Editor-Version": "vscode/1.99.0", "Copilot-Integration-Id": "vscode-chat" })"}]]
    std::map<std::string, std::string> headers{};
    [[= reflect::doc{"Limits per upstream model id, as the provider reports them (llmgate models lists them). max_tokens above output_tokens is clamped; the model list carries prompt_tokens and output_tokens."}]]
    [[= reflect::example{R"({ "claude-sonnet-5.5": { "context_tokens": 328000, "prompt_tokens": 200000, "output_tokens": 128000 } })"}]] [[= reflect::commented]] std::map<std::string, model_limits> limits{};
    [[= reflect::doc{"Wire protocol of single models, keyed by upstream model id; a model not listed uses the provider's protocol. A provider whose list gives supported_endpoints (a Copilot seat serves Claude models on /v1/messages and GPT models on /responses) needs one entry per model that differs."}]]
    [[= reflect::example{R"({ "gpt-6-luna": "openai_responses" })"}]] [[= reflect::commented]] std::map<std::string, wire_protocol> model_protocols{};
    [[= reflect::doc{"Forward the client's own Authorization or x-api-key header and every anthropic-* header unchanged to this provider (a Claude login or an API key held by Claude Code itself) instead of attaching a gateway credential. For an Anthropic provider only; refused together with client_key_env or a non-loopback listen."}]] [[= reflect::commented]] bool forward_credentials = false;
    [[= reflect::doc{"Models of this provider to list in Claude Code's /model picker besides the values of models; each appears as claude-NAME@PROVIDER."}]] [[= reflect::example{R"(["claude-sonnet-5-5"])"}]] [[= reflect::commented]] std::vector<std::string> offer{};
    [[= reflect::doc{"Labels of this provider, shown beside every model it serves in the pane, the /model picker and the CLI, and used to group usage and filter targets. Each is 1 to 24 letters, digits, - or _."}]] [[= reflect::example{R"(["work", "acme"])"}]] [[= reflect::commented]] std::vector<std::string> labels{};
    [[= reflect::doc{"Labels added to single models, keyed by upstream model id; a model carries its provider's labels first, at most 8 in all."}]] [[= reflect::example{R"({ "claude-opus-5.5": ["big"] })"}]] [[= reflect::commented]] std::map<std::string, std::vector<std::string>> model_labels{};
    [[= reflect::doc{"Authentication of this provider: how the secret is sent (kind, header, scheme) and where it is read from (file, env). A provider with a login (copilot, chatgpt) or forward_credentials needs none; static_token_env still works as the bearer secret's variable."}]] [[= reflect::commented]] auth_config auth{};
};

constexpr std::string_view vscode_copilot_client_id = "Iv1.b507a08c87ecfe98";
constexpr std::string_view vscode_editor_version = "vscode/1.99.0";
constexpr std::string_view vscode_integration_id = "vscode-chat";

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
    [[= reflect::doc{"Base URL of the provider; a preset supplies it when this is empty."}]] [[= reflect::example{R"("https://api.anthropic.com")"}]] std::string base_url{};
    [[= reflect::doc{"Path of the Messages endpoint on the provider."}]] std::string messages_path = "/v1/messages";
    [[= reflect::doc{"A working default for this kind of provider: copilot, anthropic, chatgpt (openai_chat is not built yet). It fills base_url when that is empty and the protocol when it is left at anthropic; whatever you set yourself wins."}]]
    [[= reflect::example{R"("chatgpt")"}]] [[= reflect::commented]] preset_kind preset = preset_kind::none;
    [[= reflect::doc{"Wire protocol of the provider: anthropic (the Messages API, the default) or openai_responses (the OpenAI Responses API; Claude Code requests are translated, see docs/translation-responses.md). With openai_responses the messages_path default becomes /responses."}]]
    wire_protocol protocol = wire_protocol::anthropic;
    [[= reflect::doc{"Extra headers sent to the provider on every request. With a copilot login the gateway also sends Editor-Version and Copilot-Integration-Id (the VS Code values) unless they are set here."}]] std::map<std::string, std::string> headers{};
    [[= reflect::doc{"Limits per upstream model id, as the provider reports them (llmgate models lists them). max_tokens above output_tokens is clamped; the model list carries prompt_tokens and output_tokens."}]]
    [[= reflect::example{R"({ "claude-sonnet-5.5": { "context_tokens": 328000, "prompt_tokens": 200000, "output_tokens": 128000 } })"}]] [[= reflect::commented]] std::map<std::string, model_limits> limits{};
    [[= reflect::doc{"Wire protocol of single models, keyed by upstream model id; a model not listed uses the provider's protocol. A provider whose list gives supported_endpoints (a Copilot seat serves Claude models on /v1/messages and GPT models on /responses) needs one entry per model that differs."}]]
    [[= reflect::example{R"({ "gpt-6-luna": "openai_responses" })"}]] [[= reflect::commented]] std::map<std::string, wire_protocol> model_protocols{};
    [[= reflect::doc{"Forward the client's own Authorization or x-api-key header and every anthropic-* header unchanged to this provider (a Claude login or an API key held by Claude Code itself) instead of attaching a gateway credential. For an Anthropic provider only; refused together with client_key_env or a non-loopback listen."}]] [[= reflect::commented]] bool forward_credentials = false;
    [[= reflect::doc{"Models of this provider to list in Claude Code's /model picker besides the values of models; each appears as claude-NAME@PROVIDER."}]] [[= reflect::example{R"(["claude-sonnet-5-5"])"}]] [[= reflect::commented]] std::vector<std::string> offer{};
    [[= reflect::doc{"Labels of this provider, shown beside every model it serves in the pane, the /model picker and the CLI, and used to group usage and filter targets. Each is 1 to 24 letters, digits, - or _."}]] [[= reflect::example{R"(["work", "acme"])"}]] [[= reflect::commented]] std::vector<std::string> labels{};
    [[= reflect::doc{"Labels added to single models, keyed by upstream model id; a model carries its provider's labels first, at most 8 in all."}]] [[= reflect::example{R"({ "claude-opus-5.5": ["big"] })"}]] [[= reflect::commented]] std::map<std::string, std::vector<std::string>> model_labels{};
    [[= reflect::doc{"Authentication of this provider: how the secret is sent (kind, header, scheme) and where it is read from (file, env). A provider with a login (copilot, chatgpt) or forward_credentials needs none; static_token_env still works as the bearer secret's variable."}]] [[= reflect::commented]] auth_config auth{};
    [[= reflect::doc{"Name of the environment variable that holds the API key sent as the bearer token."}]]
    [[= reflect::example{R"("ANTHROPIC_UPSTREAM_KEY")"}]] std::string static_token_env{};
    [[= reflect::doc{"GitHub Copilot login of this provider; used instead of static_token_env."}]] std::optional<copilot_config> copilot{};
    [[= reflect::doc{"ChatGPT plan sign-in of this provider (OpenAI's documented token sharing for open-source tools); used instead of static_token_env, with protocol openai_responses."}]] std::optional<chatgpt_config> chatgpt{};
    [[= reflect::doc{"Model names this provider expects: the name Claude Code sends maps to the id here; unknown names pass through unchanged."}]]
    std::map<std::string, std::string> models{};
};

constexpr std::size_t longest_agent_prompt = 4000;
constexpr std::size_t longest_agent_description = 240;
constexpr std::size_t most_agent_tools = 24;

struct agent_config {
    [[= reflect::doc{"Target id (claude-NAME@PROVIDER) the agent runs on (llmgate targets lists them); empty means the session's own model."}]] [[= reflect::example{R"("claude-gpt-6-sol@codex")"}]] std::string target{};
    [[= reflect::doc{"When Claude should hand work to this agent: the line Claude reads in its list of agent types."}]] std::string description{};
    [[= reflect::doc{"The agent's system prompt."}]] std::string prompt{};
    [[= reflect::doc{"Tools the agent may call, by name; empty means every tool the session has."}]] [[= reflect::example{R"(["Read", "Grep", "Glob"])"}]] [[= reflect::commented]] std::vector<std::string> tools{};
    [[= reflect::doc{"Most turns the agent takes before it stops; 0 leaves Claude Code's default."}]] [[= reflect::example{"8"}]] [[= reflect::commented]] unsigned max_turns = 0;
};

constexpr unsigned most_agent_turns = 100;

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

struct debug_config {
    [[= reflect::doc{"File that receives one JSON line per request with the request and reply bodies and the headers (credentials replaced by <redacted>), for finding out what a misbehaving model sent. The file holds your conversations: it is created with mode 0600, is off while this is empty, and stops growing at capture_limit_mib."}]]
    [[= reflect::example{R"("~/.local/state/llmgate/capture.jsonl")"}]] [[= reflect::commented]] std::string capture_file{};
    [[= reflect::doc{"Size in MiB at which the capture file stops growing."}]] std::uint64_t capture_limit_mib = 256;
};

struct config {
    [[= json::required]] [[= reflect::doc{"Address the gateway listens on; point Claude Code at it with ANTHROPIC_BASE_URL=http://ADDRESS/anthropic."}]]
    [[= reflect::example{R"("127.0.0.1:9480")"}]] std::string listen{};
    [[= reflect::doc{"The single backend of an older configuration; it keeps working and acts as one provider named default."}]] upstream_config upstream{};
    [[= reflect::doc{"Claude Code expands aliases such as sonnet to full model names before it calls the gateway, so map full names: client model name to upstream model name. Unknown names go through unchanged."}]]
    [[= reflect::example{R"({
  "claude-opus-5-5": "claude-opus-5.5",
  "claude-sonnet-5-5": "claude-sonnet-5.5",
  "claude-haiku-5-5": "claude-haiku-5.5",
  "claude-haiku-4-5": "claude-haiku-4.5"
})"}]]
    std::map<std::string, std::string> models{};
    [[= reflect::doc{"More than one backend: name each one here. A provider with a copilot object uses the Copilot login, one with static_token_env uses that variable. The keys upstream, models, copilot and static_token_env stay valid and act as one provider named default."}]]
    [[= reflect::example{R"({
  "copilot": { "base_url": "https://api.githubcopilot.com", "copilot": { "token_file": "~/.config/llmgate/copilot-token.json" },
               "headers": { "Editor-Version": "vscode/1.99.0", "Copilot-Integration-Id": "vscode-chat" },
               "models": { "claude-opus-5-5": "claude-opus-5.5" } },
  "anthropic": { "base_url": "https://api.anthropic.com", "static_token_env": "ANTHROPIC_UPSTREAM_KEY" }
})"}]]
    [[= reflect::commented]] std::map<std::string, provider_config> providers{};
    [[= reflect::doc{"Prices in US dollars per million tokens, by the model name the usage ledger records (the model of the reply). The overview prices the spend of a model that is listed here; any other model's cost is left unknown, never guessed."}]]
    [[= reflect::example{R"({ "claude-sonnet-5.5": { "input": 3.0, "output": 15.0, "cache_read": 0.3, "cache_creation": 3.75 } })"}]] [[= reflect::commented]] std::map<std::string, usage::price> pricing{};
    [[= reflect::doc{"Per role (opus, sonnet, haiku, fable, other) the providers to try in order. A role without a list uses the other list, then every provider."}]]
    [[= reflect::example{R"({ "opus": ["anthropic", "copilot"], "sonnet": ["copilot", "anthropic"], "other": ["copilot"] })"}]]
    [[= reflect::commented]] std::map<std::string, std::vector<std::string>> routes{};
    [[= reflect::doc{"Per request class (subagent, workflow, compaction, auxiliary) the providers or target ids (claude-NAME@PROVIDER) to try in order, instead of the role's list. A target id names the model too. Claude Code sends the class with CLAUDE_CODE_GATEWAY_HINT_HEADERS=1, which llmgate claude sets. Pins and ids picked in /model still win."}]]
    [[= reflect::example{R"({ "compaction": ["claude-gpt-6-luna@codex", "copilot"] })"}]] [[= reflect::commented]] std::map<std::string, std::vector<std::string>> routes_by_class{};
    [[= reflect::doc{"Subagent types the llmgate mod registers in Claude Code, each bound to a target on any provider (Claude can then delegate to it by name)."}]]
    [[= reflect::example{R"({ "reviewer": { "target": "claude-gpt-6-sol@codex", "description": "Reviews a change as a second opinion", "prompt": "Review the change and list defects.", "tools": ["Read", "Grep"], "max_turns": 8 } })"}]]
    [[= reflect::commented]] std::map<std::string, agent_config> agents{};
    [[= reflect::doc{"Per subagent type (built-in such as Explore, or your own) the target it runs on; the mod applies it when the subagent starts and it outranks the model Claude names."}]]
    [[= reflect::example{R"({ "Explore": "claude-gpt-6-luna@codex" })"}]] [[= reflect::commented]] std::map<std::string, std::string> spawn_rules{};
    [[= reflect::doc{"A provider that fails before any response byte reaches Claude Code (connect error, no response head in time, 5xx, 429) is retried on the next one of the role's list; after failures consecutive failures it is skipped for cooldown."}]]
    fallback_config fallback{};
    [[= reflect::doc{"Time limits of the gateway."}]] timeouts time{};
    [[= reflect::doc{"TLS key exchange groups offered to the providers."}]] tls::group_policy tls_groups = tls::group_policy::post_quantum_preferred;
    [[= reflect::doc{"GitHub Copilot device login of the default provider; run llmgate login once."}]] std::optional<copilot_config> copilot{};
    [[= reflect::doc{"ChatGPT plan sign-in of this provider (OpenAI's documented token sharing for open-source tools); used instead of static_token_env, with protocol openai_responses."}]] std::optional<chatgpt_config> chatgpt{};
    [[= reflect::doc{"Name of the environment variable that holds the API key of the default provider."}]]
    [[= reflect::example{R"("ANTHROPIC_UPSTREAM_KEY")"}]] [[= reflect::commented]] std::string static_token_env{};
    [[= reflect::doc{"Name of the environment variable that holds the key clients must send; required when listen is not a loopback address."}]]
    [[= reflect::example{R"("LLMGATE_CLIENT_KEY")"}]] [[= reflect::commented]] std::string client_key_env{};
    [[= reflect::doc{"Host names, besides the listen address, accepted in the Host header of a request."}]]
    [[= reflect::example{R"(["gateway.internal:9480"])"}]] [[= reflect::commented]] std::vector<std::string> allowed_hosts{};
    [[= reflect::doc{"What llmgate claude and llmgate env give Claude Code: model names per role, context window and profile."}]] claude::launch_config claude{};
    [[= reflect::doc{"Usage ledger and token budgets."}]] usage_config usage{};
    [[= reflect::doc{"Debugging aids that are off by default."}]] debug_config debug{};
};

}  // namespace llmgate::gateway
