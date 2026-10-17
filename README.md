# llmgate

A gateway that lets Claude Code talk to Copilot, OpenAI-compatible and Anthropic backends without losing anything silently. C++26 with static reflection, Apache-2.0.

Status: walking-skeleton phase (BET-1, decision on 2026-11-06). The Anthropic passthrough works end to end with the real Claude Code CLI against a scripted upstream (`tests/e2e/claude_through_gateway.sh`); the first run against GitHub Copilot needs your device login.

## Quick start

```bash
cp config.example.jsonc ~/.config/llmgate/config.jsonc
build/host/llmgate login            # prints a code; authorise it at github.com/login/device
build/host/llmgate                  # serves on 127.0.0.1:9480
ANTHROPIC_BASE_URL=http://127.0.0.1:9480/anthropic ANTHROPIC_API_KEY=any claude
```

## Who may use the gateway

The gateway attaches your upstream credential to every request, so it checks who is asking:

| Check | Rule |
|---|---|
| Host header | must match the listen address (`127.0.0.1:PORT`, `localhost:PORT` and `[::1]:PORT` for a loopback listen), compared case-insensitively; anything else gets 403, which stops DNS rebinding. Extra names go in `allowed_hosts` |
| Client key | set `client_key_env` to the name of an environment variable holding a secret; requests must then carry it in `x-api-key` or as a bearer token (compared in constant time), otherwise 401. Claude Code sends `ANTHROPIC_API_KEY` as `x-api-key`. `/healthz` stays open |
| Listen address | a non-loopback address is refused at start-up unless a client key is configured |
| Token file | written atomically (`0600`, `O_EXCL|O_NOFOLLOW`, rename), its directory `0700` when created |

The client key is never forwarded upstream.

`llmgate --check-config` validates a configuration (unknown keys and wrong types are reported with their path), `llmgate --version` prints the version.

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

`tools/coverage.sh` builds with coverage, runs the unit tests and fails when any function has CRAP above 8 (CRAP = CC^2 * (1 - coverage)^3 + CC, complexity from lizard, line coverage from gcovr, a line counts as covered when any template instantiation ran it). Last run: 215 functions, 0 above 8. The epoll backend is not instrumented by the macOS run, so it is held to complexity 8 alone.

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
| `import std` through CMake needs the `CxxImportStd` experimental gate | Set before `project()` in `CMakeLists.txt` |
| `SSL_get0_group_name` before the handshake finishes dereferences a null session | Guarded in `tls::connection::negotiated_group`, covered by a named test |
| LeakSanitizer found spawned server loops still suspended when a reactor was destroyed | `reactor::spawn` registers each task and the reactor destroys unfinished ones |
| `addrinfo` member order differs between libcs, and GCC rejects omitted members under `-Werror` | The one marked exception to designated initializers, in `net.cppm` |

## TLS and post-quantum

OpenSSL 3.5 or newer is required. The default policy offers `X25519MLKEM768` and `SecP256r1MLKEM768` first, then classical groups. Measured 2026-10-17: api.anthropic.com negotiates `X25519MLKEM768`; api.githubcopilot.com negotiates `x25519` and rejects a handshake that offers only post-quantum groups.
