---
name: cpp26-idioms
description: |
  Use when writing or editing C++ in this repo (src/, app/, tests/): a new function, a new template, a new enum or enumerator, a new struct, formatting or building a string, printing, taking a callback or stream, adding a check or an assertion, or about to write a comment.
  Keeps new code in the C++26 style the tree already follows: concepts, import std, format and print, reflection, structural static_asserts, contracts, constexpr, and no comments.
  Trigger phrases: "write C++", "edit C++", "new function", "new template", "add an enum", "format a string", "build a string", "print", "add a callback", "add a comment", "include a header".
allowed-tools: Bash, Read, Grep, Edit
---

# C++26 idioms for llmgate

`python3 tools/cpp26_lint.py --strict` and `python3 tools/reflection_lint.py --strict src app tests` enforce what follows. A PreToolUse hook reads the text you are about to write and names the rule; its message is the replacement to apply. Names that repeat a type belong to the sibling skill `reflect-dont-hand-write`.

## Rules with the replacement

| Instead of | Write |
|---|---|
| `template <class F>` whose parameter is called, awaited or used as a stream or sink | `template <std::invocable<Args> F>`, a named concept (`net::byte_stream`, `sse::event_sink`, `httpio::request_handler`) or a `requires` clause. A pure type utility (`is_optional<T>`, `T` only in `sizeof` or `std::is_enum_v`) needs none |
| `#include <vector>`, `<string>`, `<format>` in a `.cppm` or `.cpp` | `import std;`. Only C and POSIX headers (`<unistd.h>`, `<openssl/ssl.h>`, `<cstdio>`) and `<meta>`, `<contracts>` go in the global module fragment |
| `out += c == q ? std::string{"'\\''"} : std::string(1, c);` or `out += "a" + name + "b"` (a temporary per character or per `+`) | `text::append_all(out, "a", name, "b")` in constexpr code, `std::format_to(std::back_inserter(out), ...)` at run time; for escaping, `std::views::split` on the special character and append each run once (see `claude::append_shell_quoted`) |
| `std::vector<std::pair<std::string, std::string>>` for names and values | `std::vector<text::name_value>`: an aggregate, so `json::to_value` writes it and `.name`/`.value` read better than `.first`/`.second` |
| `std::cout`, `printf`, `fprintf` | `std::println(stderr, "...", x)` or `std::print`; text with `std::format` |
| `out += a; out += b; out += c;` or three `push_back` calls in a row | run time: `std::format_to(std::back_inserter(out), "{}{}{}", a, b, c)` or one `std::format`; constexpr code: `text::append_all(out, a, b, c)` (`std::format` is not constexpr on GCC 16.2); a container: a braced list or `std::ranges::to` |
| `switch` returning a string per enumerator, `{"word", E::x}` tables, flag `if` chains | `reflect::enum_name`, `reflect::enum_from_name`, annotated members (see `reflect-dont-hand-write`) |
| a JSON object spelled in a `std::format` template, `{"key", x.key}` pairs | `json::to_value(aggregate)`; the member names are the wire names, or `json::key{"wire"}` when they cannot be |
| `static_cast<E>(n % 5)` | `static_cast<E>(n % reflect::enum_count<E>)` |
| a comment | nothing: the name says it, a rule lives in a named test, a requirement or the commit message. The only comments allowed are `// Exception: <reason>` on a line of its own and the `}  // namespace x` closer |

## Habits that no lint can check

1. **Add an enum** with the wire names as enumerators, then `static_assert` what must hold: a round trip over `enumerators_of` and `static_assert(reflect::unique_values<E>())` so that no enumerator is an alias. Derive tables from the enum, never the enum from a table.
2. **Structural `static_assert`s** state a property of a type once: every member is `std::uint64_t`, the handlers table has one entry per enumerator, `launch_config` has one slot per `role`. Walk `nonstatic_data_members_of` rather than listing members.
3. **Contracts at API boundaries.** `pre`/`post` on the non-inline declaration with the body in an implementation unit (an inline function with contracts exported from a module fails to link on GCC 16.2); `contract_assert` inside bodies. Use `std::expected` for failures that callers must handle, contracts for programmer errors.
4. **`constexpr` by default**, with a compile-time property check beside it (the `*_holds` functions plus the corpus and exhaustive targets in `CMakeLists.txt`).
5. **Braced construction**: designated initializers for aggregates, an initializer list for containers; never default-construct and then assign. **No needless copies**: `string_view` and `span` to borrow, sink parameters by value then `std::move`.
6. **No magic numbers**: name each number once (a constant, a `constexpr` in a `detail` namespace), reference the name. Only `0`, `1` and `100%` stand bare.
7. **Units**: durations, sizes and token counts are mp-units quantities, not `int` with a suffix.

## When a rule has a real exception

Write `// Exception: <reason>` above the line, or add a justified line to `tools/cpp26_lint_allow.txt` (`path|rule|text fragment|reason`). A reason that starts `pending:` names the tier that removes the debt (none remain); a stale line fails the gate.

## Layering guard (kept)

`template <std::meta::info Caller = std::meta::current_function()>` on a function plus `static_assert(may_call(Caller), "...")` checks the caller's enclosing namespaces at compile time and costs nothing at run time; the error names the offending caller. The CMake import graph only says who may import a module, not who may call one of its functions: `gateway` imports `process` for `write_file_atomic` and could still call `start_detached`, which the guard forbids outside `llmgate::commands`. Limits: the function becomes a template, so its address cannot be taken (`run_and_wait` stays a plain function for that reason), and the namespace it names must be declared where the guard is written. Probes: `supported/caller_namespace_guard.cpp`, `absent/caller_namespace_guard_rejects_other_layers.cpp`.

## Tried and rejected

| Idea | Why not |
|---|---|
| `requires(may_call(Caller))` instead of the `static_assert` | GCC 16.2 prints the constraint as `std::meta::info'meta_type' not supported by direct_abstract_declarator` and "no matching function"; the `static_assert` message is readable |
| `#include <coroutine>` beside `import std` | not needed: `import std;` alone builds the async layer on GCC 16.2 (macOS); the Linux container is the parent's check |
| `std::println(stderr, ...)` in `contract_violation.cpp` | it formats into a `std::string` first; a violation handler must not allocate, so `fprintf` stays as a justified allowance |
| An annotation naming a member function on the enumerator (`[[=rule{&grammar_state::on_error}]]`) | the class is declared after the enum; annotate the handler with the enumerator instead (`handles{event_type::error}`) and walk the members |

## GCC 16.2 does not do this

See the table in `reflect-dont-hand-write` and the README "Toolchain findings". Short list: `annotation_of_type`, `define_static_string(char const*)`, `std::format` in constexpr, a standard `#include` beside `import std`, stdexec with modules, `std::add_sat` through `import std`, inline contract functions exported from a module, a coroutine exported from an interface as a non-template free function.
