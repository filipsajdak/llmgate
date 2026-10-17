# llmgate

A gateway that lets Claude Code talk to Copilot, OpenAI-compatible and Anthropic backends without losing anything silently. C++26 with static reflection, Apache-2.0.

Status: walking-skeleton phase (BET-1, decision on 2026-11-06). Nothing here serves traffic yet.

## Build

```bash
cmake --preset host
cmake --build --preset host
ctest --preset host            # unit tests
ctest --preset host-network    # real TLS to api.anthropic.com and api.githubcopilot.com
```

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
| `import std` through CMake needs the `CxxImportStd` experimental gate | Set before `project()` in `CMakeLists.txt` |
| `SSL_get0_group_name` before the handshake finishes dereferences a null session | Guarded in `tls::connection::negotiated_group`, covered by a named test |
| LeakSanitizer found spawned server loops still suspended when a reactor was destroyed | `reactor::spawn` registers each task and the reactor destroys unfinished ones |
| `addrinfo` member order differs between libcs, and GCC rejects omitted members under `-Werror` | The one marked exception to designated initializers, in `net.cppm` |

## TLS and post-quantum

OpenSSL 3.5 or newer is required. The default policy offers `X25519MLKEM768` and `SecP256r1MLKEM768` first, then classical groups. Measured 2026-10-17: api.anthropic.com negotiates `X25519MLKEM768`; api.githubcopilot.com negotiates `x25519` and rejects a handshake that offers only post-quantum groups.
