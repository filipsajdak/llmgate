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
| `llmgate status` / `llmgate stop` | Shows whether the gateway is up and its counters; stops a gateway that `llmgate claude` started |
| `llmgate usage [--days N] [--json]` | Per-day, per-model token report from the usage ledger, with the budget standing; works without the gateway running |
| `llmgate statusline` | The status line command: model, folder, git branch, a context bar, gateway health, request and repair counts, and today's tokens against the budget |
| `llmgate login` | GitHub Copilot device login |
| `llmgate --check-config` | Validates the configuration; unknown keys and wrong types are reported with their path |

The first time Claude Code sees the dummy `ANTHROPIC_API_KEY` it asks once whether to use it; answer yes (VS Code skips the question through `claudeCode.disableLoginPrompt`).

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
| quota, doctor, fallback and role routing | not built yet; see the planned slices below |

`llmgate --version` prints the version.

## Usage ledger and budgets

Every request that reaches the point of calling the upstream is written as one JSON line to `usage.ledger_file` (default `~/.local/state/llmgate/usage.jsonl`, created `0600` in a `0700` directory; an empty value turns the ledger off). A line holds the time, the model Claude Code asked for and the one the upstream answered with, the role (`opus`, `sonnet`, `haiku` or `other`), the session id Claude Code sends in the request metadata, the four token counts (`input`, `output`, `cache_read`, `cache_creation`), the duration, the HTTP status and how many stream repairs were needed. It never holds prompt or response text.

The counts are the upstream's own: the usage in the reply, or in `message_start` and the final `message_delta` of a stream, so they are authoritative rather than estimated. A request that fails still gets a line, with its status and zero tokens. At startup the gateway reads the file back; lines it cannot read are skipped and counted.

```jsonc
"usage": { "budget": { "daily_tokens": 1000000, "monthly_tokens": 20000000, "warn_percent": 80 } }
```

A budget counts `input + output + cache_creation` tokens. Cache reads are shown in the report but not counted, because Claude Code re-reads its whole context from the cache on every turn and counting that would use a budget up in a few prompts. Days and months are UTC, so the daily budget resets at 00:00 UTC (01:00 or 02:00 in Poland). Past `warn_percent` the status line turns to `high`; at 100 % the gateway answers new requests with `429 rate_limit_error` ("llmgate budget used up: daily 1.2M of 1.0M tokens; resets at 00:00 UTC"), which Claude Code shows as the error. It is a soft limit: requests already running finish, so a few concurrent requests can overshoot it.

`/llmgate/stats` carries today's and the month's totals and the budget standing, `llmgate usage` prints the report, and the status line reads the same numbers. Appending a line is one small blocking `write` per request on the gateway's thread.

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

Constant evaluation rejects undefined behaviour: an out-of-bounds read, signed overflow, a dangling pointer or an uninitialised read is a compile error instead of a lucky run. The pure text modules (`jsonscan`, `sse`, `anthropic`) are therefore `constexpr`, and `tests/support/properties.cppm` holds each property check once for three consumers:

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
| `lint` | `tools/coroutine_lint.py --strict src` |
| `crap` | `tools/coverage.sh`: CRAP above 8 fails |
| `fuzz` | AFL++ with ASan and UBSan for 60 seconds on each of the ten harnesses |

A nightly schedule (02:17 UTC) and manual dispatch add `constexpr-deep` (`-DLLMGATE_CONSTEXPR_DEPTH=deep` through `CMAKE_ARGS`), the mutation run on `usage` and `statusline` (advisory, summary on the job page), and fuzzing for 15 minutes per harness. `tests/e2e/claude_through_gateway.sh` needs the real `claude` CLI and a built `build/host/llmgate`, so it stays a manual check.

## Publishing

There is no remote yet. Nothing below needs a secret: the workflow uses only the automatic `GITHUB_TOKEN` (read-only `contents`), fetches ut and llhttp from public URLs, and never calls Copilot or Anthropic.

```bash
gh repo create filipsajdak/llmgate --private --description "Claude Code gateway, C++26" --source . --remote origin   # or an organisation instead of filipsajdak
git push -u origin main
```

Then check the first run: the macOS jobs need `/opt/homebrew/bin/g++-16`, which the `gcc` formula provides only while it is GCC 16, and `ubuntu-24.04-arm` runners are free for public repositories only. Make the repository public only after reading `config.example.jsonc` and the history for anything private. Add branch protection requiring `linux`, `macos`, `e2e`, `lint` and `crap` once `crap` is green.
