# llmgate

A gateway that lets Claude Code talk to Copilot, OpenAI-compatible and Anthropic backends without losing anything silently. C++26 with static reflection, Apache-2.0.

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
| `upstream.headers` | map of string | `{}` | Extra headers sent to the upstream on every request. |
| `models` | map of string | `{}` | Claude Code expands aliases such as sonnet to full model names before it calls the gateway, so map full names: client model name to upstream model name. Unknown names go through unchanged. |
| `providers` | map of object | `{}` | More than one backend: name each one here. A provider with a copilot object uses the Copilot login, one with static_token_env uses that variable. The keys upstream, models, copilot and static_token_env stay valid and act as one provider named default. |
| `providers.<name>.base_url` | string | required | Base URL of the provider. |
| `providers.<name>.messages_path` | string | `"/v1/messages"` | Path of the Messages endpoint on the provider. |
| `providers.<name>.headers` | map of string | `{}` | Extra headers sent to the provider on every request. |
| `providers.<name>.static_token_env` | string | `""` | Name of the environment variable that holds the API key sent as the bearer token. |
| `providers.<name>.copilot` | object | none | GitHub Copilot login of this provider; used instead of static_token_env. |
| `providers.<name>.copilot.token_file` | string | `""` | File that holds the GitHub OAuth token; run llmgate login once to write it. |
| `providers.<name>.copilot.client_id` | string | `"Iv1.b507a08c87ecfe98"` | OAuth client id of the device login; the default is the one the VS Code Copilot extension uses. |
| `providers.<name>.copilot.github_url` | string | `"https://github.com"` | GitHub web address used by the device login. |
| `providers.<name>.copilot.api_url` | string | `"https://api.github.com"` | GitHub API address that exchanges the OAuth token for a Copilot token. |
| `providers.<name>.copilot.poll_interval` | duration | none | Time between polls of the device login; absent means the interval GitHub names. |
| `providers.<name>.models` | map of string | `{}` | Model names this provider expects: the name Claude Code sends maps to the id here; unknown names pass through unchanged. |
| `routes` | map of array of string | `{}` | Per role (opus, sonnet, haiku, other) the providers to try in order. A role without a list uses the other list, then every provider. |
| `fallback` | object | - | A provider that fails before any response byte reaches Claude Code (connect error, no response head in time, 5xx, 429) is retried on the next one of the role's list; after failures consecutive failures it is skipped for cooldown. |
| `fallback.failures` | integer | `3` | Consecutive failures after which a provider is skipped. |
| `fallback.cooldown` | duration | `"30s"` | How long a skipped provider stays skipped; then one request probes it. |
| `time` | object | - | Time limits of the gateway. |
| `time.connect` | duration | `"10s"` | How long a connect to a provider may take. A duration is a number with the unit ms, s, m or h. |
| `time.response_head` | duration | `"2m"` | How long a provider may take to send the response head before the attempt counts as failed and the next provider is tried. |
| `time.stream_idle` | duration | `"10m"` | How long a streaming response may stay silent before the gateway cuts it. |
| `time.drain` | duration | `"10s"` | How long a stop (SIGINT or SIGTERM) lets running requests finish; a second signal exits at once. |
| `tls_groups` | one of `post_quantum_required`, `post_quantum_preferred`, `classical_only` | `"post_quantum_preferred"` | TLS key exchange groups offered to the providers. |
| `copilot` | object | none | GitHub Copilot device login of the default provider; run llmgate login once. |
| `copilot.token_file` | string | `""` | File that holds the GitHub OAuth token; run llmgate login once to write it. |
| `copilot.client_id` | string | `"Iv1.b507a08c87ecfe98"` | OAuth client id of the device login; the default is the one the VS Code Copilot extension uses. |
| `copilot.github_url` | string | `"https://github.com"` | GitHub web address used by the device login. |
| `copilot.api_url` | string | `"https://api.github.com"` | GitHub API address that exchanges the OAuth token for a Copilot token. |
| `copilot.poll_interval` | duration | none | Time between polls of the device login; absent means the interval GitHub names. |
| `static_token_env` | string | `""` | Name of the environment variable that holds the API key of the default provider. |
| `client_key_env` | string | `""` | Name of the environment variable that holds the key clients must send; required when listen is not a loopback address. |
| `allowed_hosts` | array of string | `[]` | Host names, besides the listen address, accepted in the Host header of a request. |
| `claude` | object | - | What llmgate claude and llmgate env give Claude Code: model names per role, context window and profile. |
| `claude.opus` | string | `"claude-opus-5-5"` | Model name Claude Code uses for the opus role. |
| `claude.sonnet` | string | `"claude-sonnet-5-5"` | Model name Claude Code uses for the sonnet role. |
| `claude.haiku` | string | `"claude-haiku-5-5"` | Model name Claude Code uses for the haiku role. |
| `claude.model` | string | `"opusplan"` | Model Claude Code starts with: a model name or an alias such as opusplan. |
| `claude.context_tokens` | integer | `180000` | Context window Claude Code assumes, in tokens; it also sets the auto-compact window. |
| `claude.profile_dir` | string | `"~/.claude-llmgate"` | Directory of the separate Claude Code profile that llmgate claude uses. |
| `claude.effort` | string | `"medium"` | Effort level written to the profile when it has none. |
| `claude.telemetry` | boolean | `false` | Whether Claude Code keeps its telemetry on; off by default. |
| `usage` | object | - | Usage ledger and token budgets. |
| `usage.ledger_file` | string | `"~/.local/state/llmgate/usage.jsonl"` | File that receives one line per request; the gateway reads it back at start-up. |
| `usage.budget` | object | - | Token budgets; 0 means no limit, and counted tokens are input + output + cache writes. |
| `usage.budget.daily_tokens` | integer | `0` | Tokens allowed per UTC day; 0 means no limit. |
| `usage.budget.monthly_tokens` | integer | `0` | Tokens allowed per UTC month; 0 means no limit. |
| `usage.budget.warn_percent` | integer | `80` | Percent of a budget after which the status line turns to high. |
<!-- config-table:end -->

## Usage ledger and budgets

Every request that reaches the point of calling the upstream is written as one JSON line to `usage.ledger_file` (default `~/.local/state/llmgate/usage.jsonl`, created `0600` in a `0700` directory; an empty value turns the ledger off). A line holds the time, the model Claude Code asked for and the one the upstream answered with, the role (`opus`, `sonnet`, `haiku` or `other`), the provider that answered, the session id Claude Code sends in the request metadata, the four token counts (`input`, `output`, `cache_read`, `cache_creation`), the duration, the HTTP status and how many stream repairs were needed. It never holds prompt or response text.

The counts are the upstream's own: the usage in the reply, or in `message_start` and the final `message_delta` of a stream, so they are authoritative rather than estimated. A request that fails still gets a line, with its status and zero tokens. At startup the gateway reads the file back; lines it cannot read are skipped and counted.

```jsonc
"usage": { "budget": { "daily_tokens": 1000000, "monthly_tokens": 20000000, "warn_percent": 80 } }
```

A budget counts `input + output + cache_creation` tokens. Cache reads are shown in the report but not counted, because Claude Code re-reads its whole context from the cache on every turn and counting that would use a budget up in a few prompts. Days and months are UTC, so the daily budget resets at 00:00 UTC (01:00 or 02:00 in Poland). Past `warn_percent` the status line turns to `high`; at 100 % the gateway answers new requests with `429 rate_limit_error` ("llmgate budget used up: daily 1.2M of 1.0M tokens; resets at 00:00 UTC"), which Claude Code shows as the error. It is a soft limit: requests already running finish, so a few concurrent requests can overshoot it.

`/llmgate/stats` carries today's and the month's totals and the budget standing, `llmgate usage` prints the report, and the status line reads the same numbers. Appending a line is one small blocking `write` per request on the gateway's thread.

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

**Limits.**
- Plan mode (`opusplan`) is not a separate route: Claude Code sends no marker for it, only the model (an opus model while planning, a sonnet model otherwise), so the role routes already cover it.
- Failure counting is per provider across all roles, and the cooldown clock is the monotonic clock, not the wall clock.
- Only one health signal exists: an HTTP error before the head. A provider that answers 200 with a bad stream is not failed over (the stream is repaired or ended with an error as before).
- Retrying a request whose connection was reused from the pool and turns out dead counts as that provider's failure and moves on to the next provider; it does not retry the same one on a fresh connection.
- `llmgate login` logs in with the top-level `copilot` key, else with the first provider (alphabetically) that has a `copilot.token_file`; two Copilot providers with different token files need a login each, by listing them one at a time.

The selection logic (circuit breaker, role route order) is a constexpr module, `src/gateway/selection.cppm`, with unit tests, a corpus replay, a bounded exhaustive check and an AFL++ harness like the other pure modules.

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
| `tools/cpp26_lint.py --strict` | Unconstrained templates whose parameter is called, awaited or used as a sink; standard `#include`s beside `import std`; iostream and `printf`; runs of three `+=` or `push_back` statements in non-constexpr code; and every comment except a line starting `// Exception: <reason>` and a `}  // namespace x` closer. The `cpp26-idioms` skill and the project `CLAUDE.md` carry the replacements; allowances live in `tools/cpp26_lint_allow.txt` with the same `pending:` convention | `tools/` |
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

## Publishing

There is no remote yet. Nothing below needs a secret: the workflow uses only the automatic `GITHUB_TOKEN` (read-only `contents`), fetches ut and llhttp from public URLs, and never calls Copilot or Anthropic.

```bash
gh repo create filipsajdak/llmgate --private --description "Claude Code gateway, C++26" --source . --remote origin   # or an organisation instead of filipsajdak
git push -u origin main
```

Then check the first run: the macOS jobs need `/opt/homebrew/bin/g++-16`, which the `gcc` formula provides only while it is GCC 16, and `ubuntu-24.04-arm` runners are free for public repositories only. Make the repository public only after reading `config.example.jsonc` and the history for anything private. Add branch protection requiring `linux`, `macos`, `e2e`, `lint` and `crap` once `crap` is green.
