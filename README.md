# llmgate

A gateway that lets Claude Code talk to GitHub Copilot and Anthropic backends without losing anything silently; a ChatGPT plan backend (Codex models) has passed the compatibility scenarios on a real Plus account; other providers (local OpenAI-compatible models, Gemini, Grok) are planned and are not built yet. C++26 with static reflection, Apache-2.0.

Status: walking-skeleton phase (BET-1, decision on 2026-11-06). The Anthropic passthrough works end to end with the real Claude Code CLI against a scripted upstream (`tests/e2e/claude_through_gateway.sh`); the first run against GitHub Copilot needs your device login.

## Quick start

```bash
mkdir -p ~/.config/llmgate && cp config.example.jsonc ~/.config/llmgate/config.jsonc
llmgate login          # prints a code; authorise it at github.com/login/device (GitHub Copilot)
llmgate setup          # creates the dedicated Claude profile ~/.claude-llmgate
llmgate claude         # starts the gateway if it is down, then runs Claude Code through it
```

`llmgate claude ARGS...` passes every argument to `claude` unchanged, so `llmgate claude -p "hello" --model sonnet` works. Your normal `claude` and `~/.claude` are never touched: the launcher points Claude Code at its own profile directory through `CLAUDE_CONFIG_DIR`.

| Command | What it does |
|---|---|
| `llmgate claude [args]` | Sets the environment Claude Code needs, starts the gateway on demand (detached, logging to `~/.local/state/llmgate/gateway.log`, never killing anything) and runs `claude` |
| `llmgate setup [--dry-run]` | Creates or updates the profile: private deny rules for `.env` files, secrets, the gateway config directory and `~/.claude`, `effortLevel` if unset, and the status line. Existing settings are merged, never overwritten; a `settings.json` that is not plain JSON is refused untouched |
| `llmgate setup --service` | Also installs a launchd agent (macOS) or systemd user unit (Linux) that keeps the gateway running |
| `llmgate setup --vscode FILE` | Merges `claudeCode.environmentVariables` and `claudeCode.disableLoginPrompt` into a VS Code settings file (plain JSON only) |
| `llmgate env [--json\|--vscode]` | Prints the environment as shell exports, JSON or a VS Code settings snippet |
| `llmgate status` / `llmgate stop` | Shows whether the gateway is up and its counters; `stop` sends SIGTERM to a gateway that `llmgate claude` started and waits until it has exited |
| `llmgate usage [--days N] [--json]` | Per-day, per-model token report from the usage ledger, with the budget standing; works without the gateway running |
| `llmgate doctor [--offline] [--json]` | Checks the setup and prints PASS, WARN or FAIL per check with a one-line remedy; exits non-zero on any FAIL. No secret is printed. `--offline` skips the GitHub calls |
| `llmgate quota [--json]` | GitHub Copilot plan and quota: remaining of total per allowance and the reset date |
| `llmgate models [--provider NAME] [--json]` | the models a provider lists, with context, prompt and output limits, and a mark on any the account's policy disables; `doctor` runs the same check on every model the config maps |
| `llmgate statusline` | The status line command: model, folder, git branch, a context bar, gateway health, request and repair counts, and today's tokens against the budget |
| `llmgate login` | GitHub Copilot device login |
| `llmgate --check-config` | Validates the configuration; unknown keys and wrong types are reported with their path |

The launcher passes the gateway key (or a placeholder) as `ANTHROPIC_AUTH_TOKEN`, not `ANTHROPIC_API_KEY`: with the latter Claude Code asks once whether to use a custom API key, with the former (checked on `claude` 2.1.302) it does not. The gateway accepts it as a bearer token.

### What replaces a per-user setup kit

| Kit | llmgate |
|---|---|
| `install.sh`, profile directory, `settings.json` | `llmgate setup` |
| wrapper scripts and their environment variables | `llmgate claude`, `llmgate env` |
| a script that starts the gateway if it is down | `llmgate claude` starts it; `setup --service` keeps it running |
| a login helper, log scraping for the device code | `llmgate login` |
| plan-mode hooks, write-size rules, attribution-header and stream-cut workarounds | the gateway keeps system reminders, repairs streams and sets the environment itself |
| VS Code settings script | `llmgate setup --vscode FILE`, `llmgate env --vscode` |
| status line script, cost hook | `llmgate statusline` (third line: today's tokens and budget) |
| usage ledger, budgets | `usage` in the configuration, `llmgate usage` |
| fallback and role routing | `providers`, `routes`, `fallback` (see Providers, role routes and fallback) |
| quota, doctor | `llmgate quota`, `llmgate doctor` |

`llmgate --version` prints the version.

## Configuration reference

Every key is documented once, in the `[[=reflect::doc{...}]]` and `[[=reflect::example{...}]]` annotations on the configuration structs in `src/gateway/config.cppm` (and on `claude::launch_config` and `usage::budget`). Three flags print documents generated from them: `llmgate --print-example` (the commented JSONC that is `config.example.jsonc`), `llmgate --print-config-table` (the table below) and `llmgate --print-schema` (JSON Schema 2020-12). After a change to an annotation run `tools/update_config_docs.sh`; the tests `llmgate_example_config_is_generated` and `llmgate_readme_config_table_is_generated` fail while either file is stale, and `constexpr_example_config` fails to build when the example no longer parses as `config` or leaves a member out.

<!-- config-table:begin -->
| Key | Type | Default | Description |
|---|---|---|---|
| `listen` | string | required | Address the gateway listens on; point Claude Code at it with ANTHROPIC_BASE_URL=http://ADDRESS/anthropic. |
| `upstream` | object | - | The single backend of an older configuration; it keeps working and acts as one provider named default. |
| `upstream.base_url` | string | required | Base URL of the single upstream; together with the other top-level keys it acts as one provider named default. |
| `upstream.messages_path` | string | `"/v1/messages"` | Path of the Messages endpoint on the upstream. |
| `upstream.preset` | one of `none`, `copilot`, `anthropic`, `chatgpt`, `openai_chat` | `"none"` | A working default for this kind of provider: copilot, anthropic, chatgpt (openai_chat is not built yet). It fills base_url when that is empty and the protocol when it is left at anthropic; whatever you set yourself wins. |
| `upstream.protocol` | one of `anthropic`, `openai_responses` | `"anthropic"` | Wire protocol of the provider: anthropic (the Messages API, the default) or openai_responses (the OpenAI Responses API; Claude Code requests are translated, see docs/translation-responses.md). With openai_responses the messages_path default becomes /responses. |
| `upstream.headers` | map of string | `{}` | Extra headers sent to the upstream on every request. With a copilot login the gateway also sends Editor-Version and Copilot-Integration-Id (the VS Code values) unless they are set here. |
| `upstream.limits` | map of object | `{}` | Limits per upstream model id, as the provider reports them (llmgate models lists them). max_tokens above output_tokens is clamped; the model list carries prompt_tokens and output_tokens. |
| `upstream.limits.<name>.context_tokens` | integer | `0` | Context window of the model in tokens; 0 means unknown. |
| `upstream.limits.<name>.prompt_tokens` | integer | `0` | Largest prompt the model accepts, in tokens; 0 means unknown. Published as max_input_tokens in the model list. |
| `upstream.limits.<name>.output_tokens` | integer | `0` | Largest reply the model produces, in tokens; 0 means unknown. A request with a larger max_tokens is clamped to it, and the model list publishes it as max_tokens. |
| `upstream.forward_credentials` | boolean | `false` | Forward the client's own Authorization or x-api-key header and every anthropic-* header unchanged to this provider (a Claude login or an API key held by Claude Code itself) instead of attaching a gateway credential. For an Anthropic provider only; refused together with client_key_env or a non-loopback listen. |
| `upstream.offer` | array of string | `[]` | Models of this provider to list in Claude Code's /model picker besides the values of models; each appears as claude-NAME@PROVIDER. |
| `models` | map of string | `{}` | Claude Code expands aliases such as sonnet to full model names before it calls the gateway, so map full names: client model name to upstream model name. Unknown names go through unchanged. |
| `providers` | map of object | `{}` | More than one backend: name each one here. A provider with a copilot object uses the Copilot login, one with static_token_env uses that variable. The keys upstream, models, copilot and static_token_env stay valid and act as one provider named default. |
| `providers.<name>.base_url` | string | `""` | Base URL of the provider; a preset supplies it when this is empty. |
| `providers.<name>.messages_path` | string | `"/v1/messages"` | Path of the Messages endpoint on the provider. |
| `providers.<name>.preset` | one of `none`, `copilot`, `anthropic`, `chatgpt`, `openai_chat` | `"none"` | A working default for this kind of provider: copilot, anthropic, chatgpt (openai_chat is not built yet). It fills base_url when that is empty and the protocol when it is left at anthropic; whatever you set yourself wins. |
| `providers.<name>.protocol` | one of `anthropic`, `openai_responses` | `"anthropic"` | Wire protocol of the provider: anthropic (the Messages API, the default) or openai_responses (the OpenAI Responses API; Claude Code requests are translated, see docs/translation-responses.md). With openai_responses the messages_path default becomes /responses. |
| `providers.<name>.headers` | map of string | `{}` | Extra headers sent to the provider on every request. With a copilot login the gateway also sends Editor-Version and Copilot-Integration-Id (the VS Code values) unless they are set here. |
| `providers.<name>.limits` | map of object | `{}` | Limits per upstream model id, as the provider reports them (llmgate models lists them). max_tokens above output_tokens is clamped; the model list carries prompt_tokens and output_tokens. |
| `providers.<name>.limits.<name>.context_tokens` | integer | `0` | Context window of the model in tokens; 0 means unknown. |
| `providers.<name>.limits.<name>.prompt_tokens` | integer | `0` | Largest prompt the model accepts, in tokens; 0 means unknown. Published as max_input_tokens in the model list. |
| `providers.<name>.limits.<name>.output_tokens` | integer | `0` | Largest reply the model produces, in tokens; 0 means unknown. A request with a larger max_tokens is clamped to it, and the model list publishes it as max_tokens. |
| `providers.<name>.forward_credentials` | boolean | `false` | Forward the client's own Authorization or x-api-key header and every anthropic-* header unchanged to this provider (a Claude login or an API key held by Claude Code itself) instead of attaching a gateway credential. For an Anthropic provider only; refused together with client_key_env or a non-loopback listen. |
| `providers.<name>.offer` | array of string | `[]` | Models of this provider to list in Claude Code's /model picker besides the values of models; each appears as claude-NAME@PROVIDER. |
| `providers.<name>.static_token_env` | string | `""` | Name of the environment variable that holds the API key sent as the bearer token. |
| `providers.<name>.copilot` | object | none | GitHub Copilot login of this provider; used instead of static_token_env. |
| `providers.<name>.copilot.token_file` | string | `""` | File that holds the GitHub OAuth token; run llmgate login once to write it. |
| `providers.<name>.copilot.client_id` | string | `"Iv1.b507a08c87ecfe98"` | OAuth client id of the device login; the default is the one the VS Code Copilot extension uses. |
| `providers.<name>.copilot.github_url` | string | `"https://github.com"` | GitHub web address used by the device login. |
| `providers.<name>.copilot.api_url` | string | `"https://api.github.com"` | GitHub API address that exchanges the OAuth token for a Copilot token. |
| `providers.<name>.copilot.poll_interval` | duration | none | Time between polls of the device login; absent means the interval GitHub names. |
| `providers.<name>.chatgpt` | object | none | ChatGPT plan sign-in of this provider (OpenAI's documented token sharing for open-source tools); used instead of static_token_env, with protocol openai_responses. |
| `providers.<name>.chatgpt.token_file` | string | `""` | File that holds the ChatGPT sign-in tokens (created with mode 0600); run llmgate login --provider NAME once to write it. |
| `providers.<name>.chatgpt.issuer` | string | `"https://auth.openai.com"` | Address of the OpenAI sign-in service. |
| `providers.<name>.chatgpt.callback_port` | integer | `1455` | Port on 127.0.0.1 that receives the browser's callback during the sign-in. |
| `providers.<name>.chatgpt.agent_name` | string | `"llmgate"` | Name OpenAI shows the user when this tool is registered at the first sign-in. |
| `providers.<name>.models` | map of string | `{}` | Model names this provider expects: the name Claude Code sends maps to the id here; unknown names pass through unchanged. |
| `pricing` | map of object | `{}` | Prices in US dollars per million tokens, by the model name the usage ledger records (the model of the reply). The overview prices the spend of a model that is listed here; any other model's cost is left unknown, never guessed. |
| `pricing.<name>.input` | number | `0` | US dollars per million input tokens. |
| `pricing.<name>.output` | number | `0` | US dollars per million output tokens. |
| `pricing.<name>.cache_read` | number | `0` | US dollars per million cache-read tokens. |
| `pricing.<name>.cache_creation` | number | `0` | US dollars per million cache-write tokens. |
| `routes` | map of array of string | `{}` | Per role (opus, sonnet, haiku, fable, other) the providers to try in order. A role without a list uses the other list, then every provider. |
| `fallback` | object | - | A provider that fails before any response byte reaches Claude Code (connect error, no response head in time, 5xx, 429) is retried on the next one of the role's list; after failures consecutive failures it is skipped for cooldown. |
| `fallback.failures` | integer | `3` | Consecutive failures after which a provider is skipped. |
| `fallback.cooldown` | duration | `"30s"` | How long a skipped provider stays skipped; then one request probes it. |
| `time` | object | - | Time limits of the gateway. |
| `time.connect` | duration | `"10s"` | How long a connect to a provider may take. A duration is a number with the unit ms, s, m or h. |
| `time.response_head` | duration | `"2m"` | How long a provider may take to send the response head before the attempt counts as failed and the next provider is tried. |
| `time.stream_idle` | duration | `"2m"` | How long a streaming response may stay silent before the gateway cuts it with an error event, so that the client can retry. Anthropic servers send a ping every few seconds, so a silence of two minutes is a stall; raise it for a provider that streams nothing while it reasons. |
| `time.drain` | duration | `"10s"` | How long a stop (SIGINT or SIGTERM) lets running requests finish; a second signal exits at once. |
| `tls_groups` | one of `post_quantum_required`, `post_quantum_preferred`, `classical_only` | `"post_quantum_preferred"` | TLS key exchange groups offered to the providers. |
| `copilot` | object | none | GitHub Copilot device login of the default provider; run llmgate login once. |
| `copilot.token_file` | string | `""` | File that holds the GitHub OAuth token; run llmgate login once to write it. |
| `copilot.client_id` | string | `"Iv1.b507a08c87ecfe98"` | OAuth client id of the device login; the default is the one the VS Code Copilot extension uses. |
| `copilot.github_url` | string | `"https://github.com"` | GitHub web address used by the device login. |
| `copilot.api_url` | string | `"https://api.github.com"` | GitHub API address that exchanges the OAuth token for a Copilot token. |
| `copilot.poll_interval` | duration | none | Time between polls of the device login; absent means the interval GitHub names. |
| `chatgpt` | object | none | ChatGPT plan sign-in of this provider (OpenAI's documented token sharing for open-source tools); used instead of static_token_env, with protocol openai_responses. |
| `chatgpt.token_file` | string | `""` | File that holds the ChatGPT sign-in tokens (created with mode 0600); run llmgate login --provider NAME once to write it. |
| `chatgpt.issuer` | string | `"https://auth.openai.com"` | Address of the OpenAI sign-in service. |
| `chatgpt.callback_port` | integer | `1455` | Port on 127.0.0.1 that receives the browser's callback during the sign-in. |
| `chatgpt.agent_name` | string | `"llmgate"` | Name OpenAI shows the user when this tool is registered at the first sign-in. |
| `static_token_env` | string | `""` | Name of the environment variable that holds the API key of the default provider. |
| `client_key_env` | string | `""` | Name of the environment variable that holds the key clients must send; required when listen is not a loopback address. |
| `allowed_hosts` | array of string | `[]` | Host names, besides the listen address, accepted in the Host header of a request. |
| `claude` | object | - | What llmgate claude and llmgate env give Claude Code: model names per role, context window and profile. |
| `claude.opus` | string | `"claude-opus-5-5"` | Model name Claude Code uses for the opus role. |
| `claude.sonnet` | string | `"claude-sonnet-5-5"` | Model name Claude Code uses for the sonnet role. |
| `claude.haiku` | string | `"claude-haiku-5-5"` | Model name Claude Code uses for the haiku role. |
| `claude.fable` | string | `"claude-fable-5-1"` | Model name Claude Code uses for the fable role. |
| `claude.model` | string | `"opusplan"` | Model Claude Code starts with: a model name or an alias such as opusplan. |
| `claude.context_tokens` | integer | `180000` | Context window Claude Code assumes, in tokens; it also sets the auto-compact window. |
| `claude.profile_dir` | string | `"~/.claude-llmgate"` | Directory of the separate Claude Code profile that llmgate claude uses. |
| `claude.effort` | string | `"medium"` | Effort level written to the profile when it has none. |
| `claude.telemetry` | boolean | `false` | Whether Claude Code keeps its telemetry on; off by default. |
| `claude.credential` | one of `gateway`, `login` | `"gateway"` | Credential Claude Code presents to the gateway: gateway sends the gateway key (or a placeholder) as ANTHROPIC_AUTH_TOKEN; login sends none, so Claude Code keeps using its own saved login or API key, which a provider with forward_credentials passes on. |
| `usage` | object | - | Usage ledger and token budgets. |
| `usage.ledger_file` | string | `"~/.local/state/llmgate/usage.jsonl"` | File that receives one line per request; the gateway reads it back at start-up. |
| `usage.budget` | object | - | Token budgets; 0 means no limit, and counted tokens are input + output + cache writes. |
| `usage.budget.daily_tokens` | integer | `0` | Tokens allowed per UTC day; 0 means no limit. |
| `usage.budget.monthly_tokens` | integer | `0` | Tokens allowed per UTC month; 0 means no limit. |
| `usage.budget.warn_percent` | integer | `80` | Percent of a budget after which the status line turns to high. |
| `debug` | object | - | Debugging aids that are off by default. |
| `debug.capture_file` | string | `""` | File that receives one JSON line per request with the request and reply bodies and the headers (credentials replaced by <redacted>), for finding out what a misbehaving model sent. The file holds your conversations: it is created with mode 0600, is off while this is empty, and stops growing at capture_limit_mib. |
| `debug.capture_limit_mib` | integer | `256` | Size in MiB at which the capture file stops growing. |
<!-- config-table:end -->

## Usage ledger and budgets

Every request that reaches the point of calling the upstream is written as one JSON line to `usage.ledger_file` (default `~/.local/state/llmgate/usage.jsonl`, created `0600` in a `0700` directory; an empty value turns the ledger off). A line holds the time, the model Claude Code asked for and the one the upstream answered with, the role (`opus`, `sonnet`, `haiku` or `other`), the provider that answered, the session id Claude Code sends in the request metadata, the four token counts (`input`, `output`, `cache_read`, `cache_creation`), the duration, the HTTP status and how many stream repairs were needed. It never holds prompt or response text.

The counts are the upstream's own: the usage in the reply, or in `message_start` and the final `message_delta` of a stream, so they are authoritative rather than estimated. A request that fails still gets a line, with its status and zero tokens. At startup the gateway reads the file back; lines it cannot read are skipped and counted.

```jsonc
"usage": { "budget": { "daily_tokens": 1000000, "monthly_tokens": 20000000, "warn_percent": 80 } }
```

A budget counts `input + output + cache_creation` tokens. Cache reads are shown in the report but not counted, because Claude Code re-reads its whole context from the cache on every turn and counting that would use a budget up in a few prompts. Days and months are UTC, so the daily budget resets at 00:00 UTC (01:00 or 02:00 in Poland). Past `warn_percent` the status line turns to `high`; at 100 % the gateway answers new requests with `429 rate_limit_error` ("llmgate budget used up: daily 1.2M of 1.0M tokens; resets at 00:00 UTC"), which Claude Code shows as the error. It is a soft limit: requests already running finish, so a few concurrent requests can overshoot it.

`/llmgate/stats` also keeps `repair_examples`: the first detail the gateway saw for each kind of stream repair (for an unparseable event, its name and the first 24 printable characters of its data). A bare `data: [DONE]` event, which Copilot sends at the end of every stream, is dropped without counting as a repair. `/llmgate/stats` carries today's and the month's totals and the budget standing, `llmgate usage` prints the report, and the status line reads the same numbers. Appending a line is one small blocking `write` per request on the gateway's thread.

## Providers, role routes and fallback

One gateway can sit in front of several backends. Name each under `providers`, then say per role which providers to try and in what order:

```jsonc
"providers": {
  "copilot":   { "base_url": "https://api.githubcopilot.com", "copilot": { "token_file": "~/.config/llmgate/copilot-token.json" },
                 "headers": { "Editor-Version": "vscode/1.99.0" }, "models": { "claude-opus-5-5": "claude-opus-5.5" } },
  "anthropic": { "base_url": "https://api.anthropic.com", "static_token_env": "ANTHROPIC_UPSTREAM_KEY" }
},
"routes": { "opus": ["anthropic", "copilot"], "sonnet": ["copilot", "anthropic"], "other": ["copilot"] },
"fallback": { "failures": 3, "cooldown": "30s" }
```

| Key | Meaning |
|---|---|
| `providers.NAME` | `base_url` (required), `messages_path` (default `/v1/messages`), `headers`, a credential (`copilot` with its token file, or `static_token_env` naming an environment variable) and `models` (the name Claude Code sends -> the id this provider expects; unknown names pass through unchanged) |
| `routes.ROLE` | the providers to try, in order, for `opus`, `sonnet`, `haiku` or `other`; the role is the family named in the requested model (`usage::role_of`); the roles are the enumerators of `roles::role` in `src/reflect/role.cppm`, the only list of them. A role without a list uses the `other` list, and without that every provider in alphabetical order |
| `fallback.failures` | consecutive failures that open a provider's circuit (default 3) |
| `fallback.cooldown` | how long an open circuit is skipped (default `30s`) |
| `upstream`, `models`, `copilot`, `static_token_env` | still valid on their own: they act as one provider named `default`, so an existing configuration needs no change |

**What triggers a fallback.** Only a failure before any response byte reached Claude Code: a refused or timed-out connection, a write error, no response head within `time.response_head`, a reply with status 5xx or 429. The next provider of the list is tried once each. A stream that has begun is never retried, and neither is a 4xx other than 429 (it is relayed), so a request is never charged twice. If every provider failed, the last failure is relayed exactly as it came; if every circuit is open the client gets `503` ("every provider for this model is unavailable").

**Circuit breaker.** After `failures` consecutive failures a provider is skipped for `cooldown`. Then exactly one request probes it (half-open): success closes the circuit, failure reopens it for another full cooldown. A failure that is the gateway's own doing, such as a provider whose credential is missing, falls through to the next provider but does not count against the breaker. The breaker is in memory and starts closed on every restart.

**Records.** The ledger line gets a `provider` field (the provider that answered, or the last one tried when all failed; one line per request, however many providers were tried; lines written before the field existed read back with an empty provider). `/llmgate/stats` has a `providers` object with `attempts`, `answered`, `failures`, `skipped` (circuit open) and `circuit` (`closed`, `open`, `half_open`) per provider, and `no_provider` counts the 503s. `/anthropic/v1/models` lists the union of all providers' aliases.

The ledger line also records `stop_reason`, `error` (the upstream's message or error type, at most 200 characters) and `kind`: `classifier` for a request whose `stop_sequences` hold `</block>` (the auto-mode safety classifier, which roughly doubles the request count in auto mode), `severity` for the small side request that stops at `</severity>`, and `main` for the rest. For classifier traffic `verdict` is `block`, `allow` or `unparseable`, read the way the client reads it (`<block>yes` or `<block>no`, after an optional `<thinking>` section), and `/llmgate/stats` counts `classifier_requests` and `classifier_unparseable` per `provider/model`, so a model that does not follow the format is visible at once. The gateway only counts these replies; it never rewrites one. Lines written before these fields existed read back with them empty.

**Capturing a misbehaving exchange.** Set `debug.capture_file` and the gateway appends one JSON line per request with its request headers and body and the upstream's reply headers and body (the raw event stream for a streamed reply), each body cut at 2 MiB, with `Authorization`, `x-api-key`, `Cookie` and similar headers replaced by `<redacted>`. The file holds your conversations, so it is created with mode 0600, is off by default, announces itself on start-up and stops growing at `debug.capture_limit_mib` (256). It is how to see what a model really sent, for example the safety classifier's request and reply in an auto-mode session.

**Rate limits.** A 429 from a provider does not count as a failure of that provider: the request still goes on to the next provider, and the last reply reaches the client unchanged with its own retry timing. Only 5xx replies, timeouts and bodies that are not a Message open a provider's circuit.

**Limits.**
- Plan mode (`opusplan`) is not a separate route: Claude Code sends no marker for it, only the model (an opus model while planning, a sonnet model otherwise), so the role routes already cover it.
- Failure counting is per provider across all roles, and the cooldown clock is the monotonic clock, not the wall clock.
- Only one health signal exists: an HTTP error before the head. A provider that answers 200 with a bad stream is not failed over (the stream is repaired or ended with an error as before).
- Retrying a request whose connection was reused from the pool and turns out dead counts as that provider's failure and moves on to the next provider; it does not retry the same one on a fresh connection.
- `llmgate login` logs in with the top-level `copilot` key, else with the first provider (alphabetically) that has a `copilot.token_file`; two Copilot providers with different token files need a login each, by listing them one at a time.

The selection logic (circuit breaker, role route order) is a constexpr module, `src/gateway/selection.cppm`, with unit tests, a corpus replay, a bounded exhaustive check and an AFL++ harness like the other pure modules.

## Using a Claude subscription or an API key held by Claude Code

Anthropic's gateway guide says that with only `ANTHROPIC_BASE_URL` set, Claude Code keeps using its saved claude.ai login (or API key) and a gateway must forward the credential and the `anthropic-beta` value (which carries the OAuth capability) unchanged. A provider with `forward_credentials` does exactly that, so llmgate never sees, stores or refreshes a subscription token:

```jsonc
"providers": { "anthropic": { "base_url": "https://api.anthropic.com", "forward_credentials": true } },
"claude": { "credential": "login" }
```

- The client's `Authorization` and `x-api-key` headers and every `anthropic-*` header reach that provider unchanged; no gateway token is attached. Every other provider still gets only its own credential, so a fallback from an Anthropic provider to Copilot never hands Copilot your Anthropic login.
- `claude.credential: "login"` makes `llmgate claude` and `llmgate env` leave `ANTHROPIC_AUTH_TOKEN` unset, so Claude Code does not replace its own login with the gateway placeholder. Run `llmgate claude` once and sign in with `/login` inside the dedicated profile (`~/.claude-llmgate`); your normal `~/.claude` is not touched.
- The gateway refuses to start a forwarding provider together with `client_key_env` (the same header cannot carry both the gateway key and the login) or on a non-loopback listen address; `llmgate --check-config` says so.
- One Claude subscription account is one Claude Code profile. Anthropic's terms do not allow a gateway to collect, store or intermediate subscription credentials, so llmgate does not hold subscription tokens and cannot switch between two subscription accounts inside a running session; `/login` in Claude Code does that by hand.

## Switching providers, models and accounts

Every model a provider serves is a target with an id of the form `claude-MODEL@PROVIDER` (the `claude` keeps it in Claude Code's `/model` list). Set `CLAUDE_CODE_ENABLE_GATEWAY_MODEL_DISCOVERY=1` and `/model` lists them next to the aliases; picking one sends the session to exactly that provider and model, with no fallback. A provider's `offer` list adds models that are not in its `models` map.

A request is routed by the first rule that applies: a picked target, a remap for this session, a remap for the role, then `routes` and `fallback`.

| Command | Effect |
|---|---|
| `llmgate targets` | list the targets |
| `llmgate use opus claude-opus-5.5@copilot` | send the opus role to that target, for every session; saved in the state directory and kept across restarts |
| `llmgate use opus clear`, `llmgate use clear` | remove one remap, or all |
| `llmgate use sonnet TARGET --session ID` | the same for one Claude Code session, kept in memory |
| `llmgate route [MODEL] [--session ID]` | show which provider would answer, and why |
| `/gateway use sonnet TARGET` | the same from inside Claude Code, for the current session (`llmgate setup` writes the command) |

The commands are signed with the identity key, so only a process that can read it can change the routing. The status line shows `via PROVIDER/MODEL` for the reply the session last received, `FALLBACK` when a later provider than the first one answered, and `switched` when the provider or model changed since the previous reply. Switching provider in the middle of a conversation starts a cold prompt cache, and Claude Code recovers from a rejected thinking signature on its own.

Several accounts of one provider are several provider entries. A Claude subscription account is a Claude Code profile: `llmgate --profile NAME claude` uses `~/.claude-llmgate-NAME` as `CLAUDE_CONFIG_DIR`, and each profile is logged in once with `/login` (see the credential section above). A running Claude Code cannot change profile; `/login` inside it switches by hand.

## Compatibility with Claude Code features

`tests/compat/` holds one directory per Claude Code feature (`scenario.json` with the prompt, `check.sh` asserting what the run changed, `stub.sh` for the self-test). `python3 tests/compat/run.py --backend NAME [--config PATH]` runs them through `llmgate claude`, writes `build/compat/NAME.json` (status, detail, the Claude Code version, the `anthropic-beta` tokens the gateway saw) and compares the statuses with `tests/compat/expected/NAME.json`. A scenario a backend does not provide is `unsupported` without a run. The self-test (`ctest -L compat-selftest`) runs every scenario against a scripted backend with a stub in place of Claude Code, and again with a stub that does nothing, which must fail every check. The table is generated from the committed expectations; a backend shows `not run` until a real run is recorded.

<!-- compat-matrix:begin -->
| Scenario | GitHub Copilot | Anthropic (Claude login) | ChatGPT plan (Codex models) | Copilot then Anthropic (switch) | Copilot then ChatGPT plan (switch) | Copilot down, then Anthropic (fallback) | Scripted (selftest) |
|---|---|---|---|---|---|---|---|
| The session is compacted and still knows what came before (`compaction`) | pass | pass | pass | - | - | - | unsupported |
| Fall back to the next provider when the first is down (`fallback`) | unsupported | unsupported | unsupported | - | - | pass | pass |
| A PostToolUse hook fires after a tool call (`hooks`) | pass | pass | pass | - | - | - | pass |
| An image is read and understood (`image`) | pass | pass | pass | - | - | - | pass |
| A twelve-turn session keeps its context (`long-session`) | pass | pass | pass | - | - | - | unsupported |
| A tool from an MCP server is listed and called (`mcp`) | pass | pass | pass | - | - | - | pass |
| Plan mode answers with a plan and changes no file (`plan-mode`) | pass | pass | pass | - | - | - | pass |
| A project skill is discovered and used (`skill`) | pass | pass | pass | - | - | - | pass |
| A project slash command runs (`slash-command`) | pass | pass | pass | - | - | - | pass |
| Delegate to a subagent (`subagent`) | pass | pass | pass | - | - | - | pass |
| Switch provider in the middle of one session (`switch-mid-session`) | unsupported | unsupported | unsupported | pass | pass | - | pass |
| Switch provider after a turn with thinking (`switch-with-thinking`) | unsupported | unsupported | unsupported | pass | pass | - | pass |
| One text turn on the haiku model (`text-haiku`) | pass | pass | pass | - | - | - | pass |
| One text turn on the opus model (`text-opus`) | pass | pass | pass | - | - | - | pass |
| One text turn (`text-turn`) | pass | pass | pass | - | - | - | pass |
| A thinking block is returned and accepted back (`thinking`) | pass | pass | pass | - | - | - | pass |
| Fix a bug through a tool loop (read, edit, run) (`tool-loop`) | pass | pass | pass | - | - | - | pass |
| Create a file with the Write tool (`write-file`) | pass | pass | pass | - | - | - | pass |
<!-- compat-matrix:end -->

## ChatGPT plan (Codex models)

A provider with `protocol: "openai_responses"` and a `chatgpt` object sends Claude Code's requests to the
OpenAI Responses API, signed in with your ChatGPT plan through OpenAI's documented sign-in for open-source
tools. The translation (requests, streamed events, tool calls, thinking, usage and errors) is specified in
[docs/translation-responses.md](docs/translation-responses.md) and is covered by unit, property and end-to-end
tests against a scripted upstream, and on 2026-10-21 every non-switching scenario of the compatibility suite
passed through a real ChatGPT Plus plan (column above). The plan route is OpenAI's, so its availability and
model list can change; `llmgate doctor` checks the sign-in.

```jsonc
{ "providers": { "codex": {
    "preset": "chatgpt",
    "chatgpt": { "token_file": "~/.config/llmgate/chatgpt.json" },
    "models": { "claude-opus-5-5": "gpt-6.1-sol", "claude-sonnet-5-5": "gpt-6-sol" } } } }
```

- `preset: "chatgpt"` fills the base URL and the `openai_responses` protocol; `copilot` and `anthropic` presets fill their base URLs, and whatever the provider sets itself wins.
- `llmgate login --provider codex` opens the browser once, registers llmgate with OpenAI, and writes the tokens
  with mode 0600. `LLMGATE_NO_BROWSER=1` prints the address instead of opening it.
- The access token lasts an hour and refreshes itself; the refresh token lasts 30 days and rotates.
- `llmgate doctor` reports whether the sign-in exists and how long it is valid.
- The plan route's own errors (usage limit, account not eligible, sign-in no longer valid) reach Claude Code as
  Anthropic errors with a plain next step.
- On a Plus plan, whether this use is enabled is OpenAI's decision and is only known after the first sign-in.
  Content sent through the plan is subject to the data controls of that ChatGPT account, so test with invented
  data.

A chat-completions translator for local models (for example vLLM on a Thor) follows the same design and is not
built.

## The llmgate mod: the route inside Claude Code

`mods/llmgate/` is a Claude Code [mod](https://code.claude.com/docs/en/plugins/mods/overview) that shows the route
of the session and switches it without a second terminal ([docs/llmgate-mod.md](docs/llmgate-mod.md); BET-5, first
slice). `/llmgate` opens a pane (beside the transcript where the app has room, above the prompt otherwise; under
`claude -p` it prints the routes as text):

- **Now:** for opus, sonnet and haiku the providers and upstream model in force and why (`via`), and the provider
  that answered the last reply, with `fallback` and `switched` marks.
- **Usage:** tokens and requests today and this month, spend per provider and model priced from the `pricing` table of the
  config (a model with no price shows `price unknown`), each provider's circuit state and counters, and the
  `anthropic-ratelimit-unified-*` limits an Anthropic provider last reported (5h and 7d use, and a limit that is not
  `allowed`).
- **Targets:** every model your providers offer (`offer` in the provider), with `use for ROLE` (this session) and
  `make default` (every session) buttons, and `clear session override`.

Start Claude Code with the mod loaded and tell it where llmgate is (the launcher will do both in a later slice):

```
LLMGATE_BIN="$(command -v llmgate)" LLMGATE_CONFIG=~/.config/llmgate/config.jsonc \
  llmgate claude --plugin-dir /path/to/llmgate/mods/llmgate
```

A mod is code that runs with your permissions, so check what this one does first:
`claude plugin validate mods/llmgate`. It reads `ANTHROPIC_BASE_URL`, `ANTHROPIC_AUTH_TOKEN`, `LLMGATE_BIN` and
`LLMGATE_CONFIG`; it fetches only from a loopback gateway (`/anthropic/v1/models`, `/llmgate/overview`), sending the gateway key only there; and its one other action is running the `llmgate` binary by
argument list (`llmgate use ROLE TARGET [--session ID]`) with a target taken from the gateway's own list, never
through a shell. Its tests run with `claude plugin test` from `mods/llmgate`, and `tests/e2e/mod_flow.sh` runs the
command against a real gateway (skipped when `claude` is not installed).

## Stopping, doctor and quota

`SIGINT` or `SIGTERM` makes `llmgate serve` stop accepting and close its listening socket. Requests already running finish for up to `time.drain` (default `10s`); new requests on open keep-alive connections get 503, and idle connections do not hold the drain. Then the gateway removes the pid file it owns (the one `llmgate claude` wrote for it) and exits 0. A second signal exits 130 at once; if the drain times out the gateway says how many requests it abandoned and still exits 0. The signal handler only writes a byte to a self-pipe that the reactor waits on next to the listening socket, so there is no flag polling and no work inside the handler. The usage ledger is appended per request, so there is nothing left to flush.

`llmgate doctor` gathers plain facts (file modes, whether the port answers, whether `settings.json` is merged, the environment) and hands them to a constexpr module that returns the findings, so the verdicts are tested at compile time. Control bytes in any text taken from a file or from GitHub are replaced before printing.

| Check | PASS when | FAIL or WARN |
|---|---|---|
| config | the file parses | FAIL, with the parser's message |
| identity | the identity file is `0600` or not created yet | FAIL when other users can read it |
| claude | `claude` (or `LLMGATE_CLAUDE`) is found | WARN |
| listen | valid, loopback or a client key is set, fixed port | FAIL for a bad address, an open address without a key or a key variable that is not set; WARN for port 0 |
| port | free, or this user's verified gateway | FAIL when something else answers |
| profile | `settings.json` is what `setup` would write | WARN when `setup` has not run or is out of date; FAIL when the file is not plain JSON |
| copilot-login, copilot-exchange | token file present and `0600`, GitHub accepts the exchange | FAIL when missing, readable by others or rejected; WARN when GitHub is unreachable |
| ledger | writable and `0600` | FAIL when it cannot be written; WARN when other users can read it |
| budget | limits are consistent | WARN for `warn_percent` of 0 or 100 and above, or a daily limit above the monthly one |
| quota | no allowance at or under 20 % | WARN when low or when the endpoint does not answer |

`llmgate quota` calls `GET /copilot_internal/user` on `copilot.api_url` with the stored login (no token exchange) and reads `copilot_plan`, `access_type_sku`, `quota_reset_date` and `quota_snapshots` (`entitlement`, `remaining`, `percent_remaining`, `unlimited`, `has_quota` for `chat`, `completions` and `premium_interactions`). Unknown keys are ignored; a reply without `quota_snapshots` is reported as unavailable. A free personal plan has no premium requests, so that line says so.

## Who may use the gateway

The gateway attaches your upstream credential to every request, so it checks who is asking:

| Check | Rule |
|---|---|
| Host header | must match the listen address (`127.0.0.1:PORT`, `localhost:PORT` and `[::1]:PORT` for a loopback listen), compared case-insensitively; anything else gets 403, which stops DNS rebinding. Extra names go in `allowed_hosts` |
| Client key | set `client_key_env` to the name of an environment variable holding a secret; requests must then carry it in `x-api-key` or as a bearer token (compared in constant time), otherwise 401. Claude Code sends `ANTHROPIC_API_KEY` as `x-api-key`. `/healthz` stays open |
| Listen address | a non-loopback address is refused at start-up unless a client key is configured |
| Token file | written atomically (`0600`, `O_EXCL|O_NOFOLLOW`, rename), its directory `0700` when created |

The client key is never forwarded upstream.



## Build

```bash
cmake --preset host
cmake --build --preset host
ctest --preset host            # unit tests
ctest --preset host-network    # real TLS to api.anthropic.com and api.githubcopilot.com
```

## Compile-time undefined-behaviour checks

Constant evaluation rejects undefined behaviour: an out-of-bounds read, signed overflow, a dangling pointer or an uninitialised read is a compile error instead of a lucky run. The pure text modules (`jsonscan`, `sse`, `anthropic`, `selection`) are therefore `constexpr`, and `tests/support/properties.cppm` holds each property check once for three consumers:

| Consumer | What it does |
|---|---|
| AFL++ harnesses (`tests/fuzz`) | run the property on mutated input at run time |
| `constexpr_*_corpus` targets | `#embed` every seed file and `static_assert` the property on it at compile time |
| `constexpr_exhaustive` | `static_assert`s the property over every input up to a bounded length; `-DLLMGATE_CONSTEXPR_DEPTH=deep` raises the bounds for CI |

A planted five-byte over-read in the scanner failed both compile-time targets (`array subscript value '51' is outside the bounds of array 'file_0'`) while the ordinary unit tests passed it.

What cannot be evaluated in GCC 16.2: `std::set` and `std::map` (use a sorted vector), `std::as_bytes` and `reinterpret_cast` (take `string_view` for text protocols), `std::format`, and floating-point `from_chars`. `llmgate::require` is the contract-lite check for inline functions: it aborts at run time and is a compile error during constant evaluation.

## Reentrancy and interleaving checks

TSan and ASan cannot see bugs that need two coroutines to interleave on one thread. These checks do:

| Technique | What it catches | Where |
|---|---|---|
| Seeded chaos reactor (`reactor{seed}`) | Shuffles the order in which ready I/O completions and posted resumptions run, so resume order is an explored dimension and any failure replays from its seed | `src/async/async.cppm` |
| `reactor::post` and `yield` | Nothing resumes another coroutine inline (the mutex hands over through the queue), which removes a class of reentrancy and gives the chaos reactor its hook | `src/async/async.cppm` |
| Refused duplicate waits | A second waiter on the same descriptor and direction gets `device_or_resource_busy` instead of silently replacing the first | `waiter_table` |
| Busy flags and async mutex | Concurrent readers get `operation_in_progress`; concurrent writers are serialised so their bytes never interleave; destroying a stream with a pending operation aborts with a message | `src/net/net.cppm`, `src/http/io.cppm` |
| `chaos::stream` | Random short reads and random yields around every I/O call | `tests/support/chaos.cppm` |
| Seed-sweep scenarios | Concurrent writers, concurrent HTTP clients and a full-duplex TLS echo run under 136 seeds; a failure prints the seed | `tests/unit/schedule_test.cpp` |
| AFL++ schedule harness | Coverage-guided search over seeds and scenarios | `tests/fuzz/schedule` |
| `tools/reflection_lint.py --strict src app tests` | Hand-written copies of a definition: enum-to-string switches (also a `case` and its `return` on separate lines), `{"word", Type::member}` tables, `if (word == ...)` chains, `{"key", x.key}` blocks, raw-string `std::format` templates with three or more `"key":{}` pairs, `{Enum::x, &fn}` and `{"GET", "/p", route::x}` tables, `static_cast<Enum>(n % N)` and `std::to_array<std::string_view>` lists that match an enum's enumerators; use `reflect::enum_name`, `reflect::enum_from_name` and annotated members instead. It reads strings, raw strings and comments with a small tokenizer (`tools/cpp_source.py`). A PreToolUse hook (`tools/cpp26_precheck.py`) scans the new text of each edit before it is written and a PostToolUse hook (`tools/reflection_hook.py`) the whole file afterwards; the `reflect-dont-hand-write` skill (`.claude/skills`) explains the patterns. `tools/reflection_lint_allow.txt` holds justified exceptions and `pending:` debt (each line names the tier that removes it), and a stale line fails the gate | `tools/` |
| `tools/cpp26_lint.py --strict` | Unconstrained templates whose parameter is called, awaited or used as a sink; standard `#include`s beside `import std`; iostream and `printf`; string temporaries (`std::string(1, c)`, a `+=` fed by a chain of `+`) and runs of three `+=` or `push_back` statements in non-constexpr code; and every comment except a line starting `// Exception: <reason>` and a `}  // namespace x` closer. The `cpp26-idioms` skill and the project `CLAUDE.md` carry the replacements; allowances live in `tools/cpp26_lint_allow.txt` with the same `pending:` convention | `tools/` |
| `python3 -m unittest discover -s tests/lint` | The tokenizer and both hooks, driven with piped tool payloads (ctest label `lint`) | `tests/lint` |
| `tools/coroutine_lint.py --strict src` | Coroutines that take references or views (clang-tidy CP.53 cannot parse this code); every allowed case carries its reason in `tools/coroutine_lint_allow.txt` | `tools/` |

The tests were validated by reintroducing the defects they target: an epoll mask that ignores writers hangs `async_test`, and a reactor without the duplicate-wait refusal fails its test.

## CRAP gate

`tools/coverage.sh` builds with coverage, runs the unit tests and fails when any function has CRAP above 8 (CRAP = CC^2 * (1 - coverage)^3 + CC, complexity from lizard, line coverage from gcovr, a line counts as covered when any template instantiation ran it). The gate scores `src/` and `app/`. Last run (2026-10-17, macOS): 542 functions, 44 above 8, so the gate fails until they are split or covered; the coverage preset passes `-Wno-error=coverage-invalid-line-number` because GCC 16.2 emits a bogus line-table warning for defaulted members in module interfaces. The epoll backend is not instrumented by the macOS run, so it is held to complexity 8 alone.

## Sanitizers and checks

| Preset | Where | Checks |
|---|---|---|
| `asan` | macOS | AddressSanitizer, UBSan with float and pointer checks, `_GLIBCXX_ASSERTIONS`, pattern-initialised locals, use-after-scope and use-after-return |
| `linux-asan` | `PRESET=linux-asan ops/ci/linux.sh` | the same plus LeakSanitizer |
| `linux-tsan` | `PRESET=linux-tsan ops/ci/linux.sh` | ThreadSanitizer (the code is single-threaded today, so it guards future threads) |
| `ops/fuzz/run.sh <target> [seconds]` | Linux container | AFL++ with ASan and UBSan on the HTTP and SSE parsers |

MemorySanitizer is not available: it needs clang, which cannot compile `<meta>`. Every sanitizer aborts on the first finding (`-fno-sanitize-recover=all`).

Requires GCC 16.2 (`/opt/homebrew/bin/g++-16`), CMake 4.4+, Ninja. Clang cannot compile `<meta>`.

## Trying it against a real Copilot

`tests/e2e/real_run.sh` runs one Claude Code turn with tool use (read a file, fix a bug, run a test, in an invented Python sample created in a temporary directory) through `llmgate claude` to the Copilot Claude models, and writes `build/realrun/report.md`: exit status, tool calls seen, stream repairs by kind, upstream failures, connection reuse, tokens, the model requested against the model served, and the doctor summary. The report holds no host name, token or response text.

```bash
LLMGATE_REAL=1 LLMGATE_GHE_HOST=your-company.ghe.com tests/e2e/real_run.sh
```

- It refuses to run without `LLMGATE_REAL=1`, because it sends requests to a real service, and without a host. A host ending in `.ghe.com` gets `https://api.HOST` for the API and `https://copilot-api.HOST` as the Copilot base URL until the token exchange names the real one; any other host is treated as GitHub Enterprise Server (`https://HOST/api/v3`). Override with `LLMGATE_GITHUB_URL`, `LLMGATE_GITHUB_API_URL` and `LLMGATE_COPILOT_BASE_URL`.
- The model's tools are scoped: Read at its default (the working directory, the invented sample), edits accepted only inside it, and Bash limited to `python3 test_mathlib.py`. Anything else it tries is denied and shows up as an error in the turn, not as a command on your machine.
- `LLMGATE_TOKEN_FILE=/path/outside/the/repo/token.json` keeps the login between runs (the script then skips `llmgate login` and never deletes the file; it refuses a path inside the repository). `LLMGATE_COPILOT_HEADERS=1` sets `Editor-Version` and `Copilot-Integration-Id` explicitly in the config (the gateway sends them by default for a Copilot provider; this is for checking that path).
- It stops at `llmgate login` until you authorise the printed code in a browser. The token lives in a temporary 0600 file, the profile and state in a temporary home, and everything is deleted on exit; your own `~/.config/llmgate` and `~/.claude-llmgate` are not touched.
- The account needs Copilot with Claude models enabled. `claude-sonnet-5-5` is mapped to `claude-sonnet-5.5` (and likewise for opus and haiku); change the ids in the script if the account serves others.
- `tests/e2e/real_run_rehearsal.sh` runs the same script against a fake GitHub and the scripted tool-use upstream, so the script itself is tested without any account (needs the real `claude` CLI).

## Performance

`python3 ops/bench/bench.py [--paced]` starts a fast fake upstream and the gateway (usage ledger on and off), drives streamed Messages requests at several concurrencies and body sizes, and prints requests per second, time to first byte, time to first delta, total latency (p50, p95, p99), CPU per request, memory and upstream connection reuse next to a direct-to-upstream baseline. `--paced` adds an upstream that sends an event every 20 ms, which shows that the gateway streams instead of buffering. Results are saved as JSON under `build/bench/`.

Measured on one Mac (Apple silicon, GCC 16.2, `RelWithDebInfo`), client and fake upstream on the same machine, plain HTTP, ledger on:

| Body | Concurrent streams | Added latency p50 / p99 | CPU per request | Throughput |
|---|---|---|---|---|
| 2 KB | 1 | 0.10 / 0.13 ms | 0.14 ms | 4,800 req/s |
| 100 KB | 1 | 0.47 / 0.53 ms | 0.42 ms | 1,640 req/s |
| 100 KB | 8 | 2.8 / 3.2 ms | 0.41 ms | 2,450 req/s |
| 100 KB | 32 | 11.5 / 21 ms | 0.41 ms | 2,400 req/s |

The gateway is single-threaded, so added latency under load is queueing behind other requests. For one Claude Code session (a stream or two, bodies of 50 to 200 KB) the gateway adds under a millisecond. `--tls` adds an HTTPS upstream with a self-signed certificate that the gateway verifies (it trusts `SSL_CERT_FILE`), and `--before-binary` compares a second build. A streamed reply that ends cleanly returns its upstream connection to the pool; before that was fixed, every streamed request opened a new connection and a high request rate ran out of ephemeral ports. Over TLS on this machine (2-second runs):

| Streamed request, 2 KB, one at a time | Added latency p50 / p99 | CPU per request | Throughput |
|---|---|---|---|
| Before the pool fix | 1.35 / 1.62 ms | 0.49 ms | 680 req/s |
| After | 0.08 / 0.10 ms | 0.14 ms | 4,790 req/s |

That is the CPU cost of one TLS handshake with the default key-exchange offer, about 1.3 ms. On loopback the network adds nothing; against a real upstream each avoided handshake also saves the TCP and TLS round trips, which depend on the distance to the provider and are not measured here.

`python3 ops/bench/soak.py --minutes 10` runs a hostile mix against the gateway (full streams, non-streamed requests, clients that vanish mid-stream, half-sent requests, malformed bodies and wrong keys), samples memory and open file descriptors every few seconds, and fails if descriptors do not return to the idle count plus the pool's idle limit, if the gateway exits non-zero on SIGTERM, or if a sanitizer reports anything. Measured 2026-10-18: 72,692 requests in 10 minutes on the host (memory flat at 9 to 15 MB, descriptors back to 23 = 15 idle + 8 pooled), and 30,308 requests in 5 minutes under `linux-asan` with LeakSanitizer (memory flat at 49 MB with `ASAN_OPTIONS=quarantine_size_mb=1`; with the default 256 MB quarantine ASan itself holds the freed memory and RSS climbs to 400 MB, which is not a leak). Run it in the container with `--cap-add SYS_PTRACE --security-opt seccomp=unconfined` and `--binary build/linux-asan/llmgate`.

Linux (the `linux` preset in the CI container, 3-second runs, ledger on) shows the same shape: one 2 KB stream adds 0.11 ms at p50 and uses 0.17 ms of CPU, one 100 KB stream adds 0.48 ms. The first Linux run exposed a 40 ms stall on every streamed reply (22 requests per second for one stream): no socket had `TCP_NODELAY`, so the response head and the first chunk, written back to back, waited on the peer's delayed ACK. macOS does not show it. Every TCP socket the gateway creates or accepts now disables Nagle (`capability_both_ends_of_a_connection_disable_nagle`).

`ops/bench/baseline.json` holds those Linux numbers. The nightly `bench` job (and `workflow_dispatch`) runs `ops/bench/ci.sh bench`, which fails on a request error or when a single stream stops reusing upstream connections, and only warns when added latency or CPU per request exceeds three times the baseline plus a floor (`ops/bench/compare.py`; runners are noisy). It then runs `ops/bench/ci.sh soak` for three minutes under `linux-asan`. Replace the baseline with `python3 ops/bench/compare.py RESULT.json --write-baseline` after a deliberate change.

Where the time goes at 100 KB (`sample` on macOS): the request body is scanned three times per request (about 0.09 ms), the usage ledger is opened, written and closed per request (about 0.035 ms), and copying the body to the upstream is the rest. Scanning once and keeping the ledger open are the two cheap wins if a shared gateway ever needs the headroom.

Real Claude Code requests are much larger than the bench default: in a sample of one account's recent sessions the median context was 290k to 470k tokens (1.2 to 1.9 MB of JSON) and the largest about 4 MB. At those sizes (macOS, `--body-kb 2048 4096`) the gateway adds a time proportional to the body, about 3 ms per MB for one stream (6 ms at 2 MB, 12 ms at 4 MB, 0.05 to 0.1 s with eight streams at once) and about 6 ms of CPU per 2 MB request. A model turn at that context takes seconds, so the cost is under 0.5 % of it; scanning the body fewer than three times would only matter for far faster upstreams.

## Design decisions

| Topic | Decision |
|---|---|
| Canonical model | Anthropic Messages is the lossless hub; same-protocol traffic is passthrough |
| JSON | Own `std::meta` codec; unknown fields are kept in a raw bag |
| Async | C++20 coroutines with an own `task<T>` and reactor awaitables, so all code can be modules; `std::execution` joins behind an adapter once `__cpp_lib_senders` exists (libstdc++ 16.2 has none) |
| I/O | Own reactor: kqueue on macOS, io_uring on Linux |
| HTTP and TLS | llhttp parser; OpenSSL over memory BIOs |
| Errors | `std::expected` on I/O and parse paths; contracts for programmer errors |
| Config | JSON with comments, read by the same codec |
| Units | mp-units for durations, sizes, token counts |
| Comments | None, except where a rule is broken or an exception is made |
| Modules | `import std` and named modules everywhere |

## Toolchain findings (GCC 16.2.0)

| Finding | Consequence |
|---|---|
| Any translation unit that includes stdexec crashes the compiler (`__query.hpp`, also on stdexec main) or fails (`__meta.hpp:288`) when modules are enabled | stdexec is not used; the async layer is coroutines |
| An inline function with contracts exported from a module leaves `__tu_has_violation` undefined at link time | Put contracts on the non-inline declaration and the body in an implementation unit |
| libstdc++ 16.2 has no default `handle_contract_violation` | `src/core/contract_violation.cpp` defines it |
| boost-ext/ut uses `constructor(101)`, unsupported on Mach-O | `cmake/patch_ut.cmake` disables that block on Apple |
| An exported non-template free function that is a coroutine and has its body in a module interface makes importers fail with `failed to read compiled module cluster N: Bad file data` | Declare it in the interface and define it in an implementation unit (`fetch.cpp`, `copilot.cpp`); coroutine class members and templates are fine |
| GCC 16.2 at `-O2` reports `-Wfree-nonheap-object` for the destructor of a `vector<pair<string,string>>` built from a braced list in the same function (valid code) | Build such lists with `emplace_back` in tests; the library functions are not affected |
| A constexpr function in one module that is called at run time from another translation unit can leave members of standard-library specialisations it instantiates undefined at link time (`expected<json::value,...>::operator*`, `string operator+`) | The json module explicitly instantiates `expected<value, parse_error>` and builds paths with `append`; if a new link error names a std symbol from a module function, add an explicit instantiation or avoid that operator |
| A constexpr function in one module that returns `std::optional<T>` of a module type, called at run time from another translation unit, can leave `optional<T>::operator*` and `operator->` undefined at link time (`quota_holds`, `launcher_holds`) | Explicit instantiation at the end of the module that defines `T`: `template class std::optional<T>;` |
| `sigemptyset` is a macro on macOS, so `::sigemptyset(...)` does not parse | A value-initialised `struct sigaction` already has an empty mask |
| A constexpr corpus target compiled with the ASan preset failed with `(const char*)(&"ok") == 0 is not a constant expression` once `usage::level_name` returned `reflect::enum_name` (a pointer from `define_static_string`) and the result became a `std::string` | Corpus targets compile with `-fno-sanitize=all` like the exhaustive ones: compile-time evaluation has nothing to sanitise |
| Reflection facilities verified on GCC 16.2 by `ctest -L capability` (one test per feature in `tests/constexpr/capabilities/supported`): annotations on members and enumerators, `annotations_of_with_type`, annotation payloads as `char[N]` or as a pointer built by a consteval factory calling `define_static_string(string_view)`, `define_aggregate` from a `consteval {}` block, an aggregate `std::formatter` with `template for`, `current_function()` as a template default, `#embed` into a constexpr array, `subobjects_of`, `access_context::unprivileged()` | `annotation_of_type` (clang-fork spelling) does not exist: the must-fail test `reflection_capability_absent_annotation_of_type` starts failing when a compiler adds it. Probes are plain translation units, so module-specific behaviour is covered by the module tests, not here |
| `std::meta::display_string_of(^^E)` for an enum declared in a module and carrying an annotation returns `ns::E@module` | `reflect::enum_category` cuts the name at `@`, so the pinned category names stay `llmgate::tls::errc` |
| boost-ut prints operands through `std::ostream` (`out_ << value`) and `expect(a == b)` on a bare bool prints no operands at all | `llmgate.format` is the aggregate `std::formatter` (redaction aware); a test that wants an aggregate in `eq(a, b)` failure output imports `llmgate.ut_print`, whose `operator<<` in `std` delegates to it (`format_test`); no source printed a struct by hand, so the formatter has no production caller yet |
| `#include <coroutine>` was kept beside `import std` in `async.cppm` and `shutdown.cppm` as debt | Removed: `import std;` alone supports the coroutine machinery on GCC 16.2 (host build and all tests pass); the Linux container build confirms it |
| `std::meta::current_function()` as a default template argument gives the caller (members and lambdas included; `parent_of` walks to the namespace) | Kept as the layering guard on `process::start_detached` (callable from `llmgate::commands` or outside `llmgate`): the import graph does not say who may call a function of an imported module, the error names the caller, no run-time cost. A constraint (`requires`) prints an unreadable diagnostic, so the check is a `static_assert`. The function is a template, so it has no address |
| A violation handler that calls `std::println` would format into a `std::string` | `contract_violation.cpp` keeps `std::fprintf`: a handler must not allocate, justified in `tools/cpp26_lint_allow.txt` |
| `(this->*[:member:])(x)` for a spliced member function | rejected with "cannot implicitly reference a class member"; write `this->[:member:](x)` |
| `std::add_sat` (saturating arithmetic) is not exported by `import std` in GCC 16.2 | `usage::add_sat` is a three-line local version; replace it when `import std` provides it |
| `import std` through CMake needs the `CxxImportStd` experimental gate | Set before `project()` in `CMakeLists.txt` |
| `SSL_get0_group_name` before the handshake finishes dereferences a null session | Guarded in `tls::connection::negotiated_group`, covered by a named test |
| LeakSanitizer found spawned server loops still suspended when a reactor was destroyed | `reactor::spawn` registers each task and the reactor destroys unfinished ones |
| `addrinfo` member order differs between libcs, and GCC rejects omitted members under `-Werror` | The one marked exception to designated initializers, in `net.cppm` |

## TLS and post-quantum

OpenSSL 3.5 or newer is required. The default policy offers `X25519MLKEM768` and `SecP256r1MLKEM768` first, then classical groups. Measured 2026-10-17: api.anthropic.com negotiates `X25519MLKEM768`; api.githubcopilot.com negotiates `x25519` and rejects a handshake that offers only post-quantum groups.

## Mutation testing

`tools/mutate.sh <file-or-module> [--changed-lines BASE_REF]` mutates one source with [universalmutator](https://github.com/agroce/universalmutator) (C++ rules; Mull needs clang, which cannot compile `<meta>`), builds and runs the unit tests that link that module for each mutant in `build/mutate`, and prints the line an MR's Verification section needs: `Mutation: universalmutator, changed lines: K killed, S survived (n new tests, e equivalent, a arid)`. `usage` finds `src/*/usage.cppm`; `--tests a,b` overrides the tests; `--sample N` (default 200, `0` for all) takes a seeded random sample because a module has thousands of mutants and each costs a rebuild of about ten seconds; `--new-tests N` fills the summary.

The source is mutated in place and restored after every mutant and on exit or signal, so do not edit it while a run is going. A mutant is `killed` (a test failed), `killed_compile_time` (a `static_assert` in the module failed), `timeout`, `uncompilable` (including warnings, because the build uses `-Werror`) or `survived`. Each survivor is printed with its line, the original and the mutated text, and its key. Record a survivor that no test can or should kill in `tools/mutation_triage.txt` as `equivalent` (no observable change) or `arid` (not product behaviour) with the reason; the summary counts them from there. `tools/mutation_ignore.txt` lists lines that are never mutated. Mutation is advice, never a merge gate; the nightly CI run publishes the summary.

Measured 2026-10-17 on a seeded sample of 250 mutants per file (the compiled ones are scored):

| File | Before | After |
|---|---|---|
| `src/claude/statusline.cppm` | 122 of 138 killed, 16 survived (88 %) | 132 of 138 killed, 6 survived: 5 equivalent, 1 arid (100 % of the killable) |
| `src/usage/usage.cppm` | 90 of 110 killed, 20 survived (82 %) | 98 of 110 killed, 12 survived: 7 equivalent, 5 arid (100 % of the killable) |

Most kills come from the `static_assert`s in the modules; the new tests in `tests/unit` raised the kills by the tests themselves from 8 to 18 (statusline) and from 6 to 14 (usage).

## Continuous integration

`.github/workflows/ci.yml` runs on every pull request and push to `main`:

| Job | What it runs |
|---|---|
| `linux` | `ops/ci/linux.sh` in the `gcc:16.2` container: presets `linux` (x86-64 and arm64), `linux-asan`, `linux-tsan` build and run every unit, constexpr and e2e test |
| `macos` | presets `host` and `asan` with Homebrew GCC 16 |
| `e2e` | `ctest -L e2e` in the container (`tests/e2e/setup_flow.sh` needs python3 and curl, which the image installs) |
| `lint` | `tools/coroutine_lint.py --strict src`, both reflection and cpp26 lints with their self-tests, and `python3 -m unittest discover -s tests/lint` |
| `crap` | `tools/coverage.sh`: CRAP above 8 fails |
| `fuzz` | AFL++ with ASan and UBSan for 60 seconds on each of the ten harnesses |

A nightly schedule (02:17 UTC) and manual dispatch add `newest-gcc` (configures on the `gcc:latest` Docker Hub image, the newest released GCC, with no secrets, then runs `ctest -L capability` and `ctest -L lint`, so a compiler release that adds or drops a reflection feature shows up the next morning; the pinned `gcc:16.2` stays the compiler of record), `constexpr-deep` (`-DLLMGATE_CONSTEXPR_DEPTH=deep` through `CMAKE_ARGS`), the mutation run on `usage` and `statusline` (advisory, summary on the job page), and fuzzing for 15 minutes per harness. `tests/e2e/claude_through_gateway.sh` needs the real `claude` CLI and a built `build/host/llmgate`, so it stays a manual check.
