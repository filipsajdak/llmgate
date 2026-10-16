# llmgate

A gateway that lets Claude Code talk to Copilot, OpenAI-compatible and Anthropic backends without losing anything silently. C++26 with static reflection, Apache-2.0.

Status: walking-skeleton phase (BET-1, decision on 2026-11-06). Nothing here serves traffic yet.

## Build

```bash
cmake --preset host
cmake --build --preset host
ctest --preset host
```

Requires GCC 16.2 (`/opt/homebrew/bin/g++-16`), CMake 4.4+, Ninja. Clang cannot compile `<meta>`.

## Design decisions

| Topic | Decision |
|---|---|
| Canonical model | Anthropic Messages is the lossless hub; same-protocol traffic is passthrough |
| JSON | Own `std::meta` codec; unknown fields are kept in a raw bag |
| Async | stdexec senders, gated on `__cpp_lib_senders` (libstdc++ 16.2 has none) |
| I/O | Own reactor: kqueue on macOS, io_uring on Linux |
| HTTP and TLS | llhttp parser; OpenSSL over memory BIOs |
| Errors | `std::expected` on I/O and parse paths; contracts for programmer errors |
| Config | JSON with comments, read by the same codec |
| Units | mp-units for durations, sizes, token counts |
| Comments | None, except where a rule is broken or an exception is made |
| Modules | `import std` and named modules everywhere except translation units that include stdexec |

## Toolchain findings (GCC 16.2.0)

| Finding | Consequence |
|---|---|
| Any translation unit that includes stdexec crashes the compiler (`__query.hpp:177`) or fails (`__meta.hpp:288`) when modules are enabled | Reactor, HTTP, TLS and backends are non-module code; pure logic is modules |
| CMake's module scanning adds `-fmodules-ts` and triggers that crash | Scanning is off by default and on per module target |
| An inline function with contracts exported from a module leaves `__tu_has_violation` undefined at link time | Put contracts on the non-inline declaration and the body in an implementation unit |
| libstdc++ 16.2 has no default `handle_contract_violation` | `src/core/contract_violation.cpp` defines it |
| boost-ext/ut uses `constructor(101)`, unsupported on Mach-O | `cmake/patch_ut.cmake` disables that block on Apple |
| `import std` through CMake needs the `CxxImportStd` experimental gate | Set before `project()` in `CMakeLists.txt` |
