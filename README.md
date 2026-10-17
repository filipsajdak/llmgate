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
| `addrinfo` member order differs between libcs, and GCC rejects omitted members under `-Werror` | The one marked exception to designated initializers, in `net.cppm` |

## TLS and post-quantum

OpenSSL 3.5 or newer is required. The default policy offers `X25519MLKEM768` and `SecP256r1MLKEM768` first, then classical groups. Measured 2026-10-17: api.anthropic.com negotiates `X25519MLKEM768`; api.githubcopilot.com negotiates `x25519` and rejects a handshake that offers only post-quantum groups.
