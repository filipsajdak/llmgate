---
name: reflect-dont-hand-write
description: |
  Use when writing or editing C++ in this repo that maps enum values to names, names to enum values, parses command-line flags, or builds or reads a JSON object field by field.
  Replaces text that must be kept in sync with a type definition with code derived from the type by C++26 reflection.
  Trigger phrases: "switch returning strings", "name table", "enum to string", "parse flags", "serialize struct", "add a field", "add a command", "add an enumerator".
allowed-tools: Bash, Read, Grep, Edit
---

# Reflect, do not hand-write what a definition already says

A string that repeats an enumerator or a member name is a second copy that nothing keeps in step. Add an enumerator, forget the switch, and the compiler stays quiet. Reflection makes the type the only copy.

## What to use instead

| Hand-written | Derived |
|---|---|
| `switch (e) { case E::a: return "a"; ... }` or a ternary chain | `reflect::enum_name(e)` (`src/reflect/enum.cppm`, constexpr, `std::meta`) |
| `{"a", E::a}, {"b", E::b}, ...` and a lookup over it | `reflect::enum_from_name<E>(word)` |
| `if (word == "--json") ... else if (word == "--dry-run") ...` | annotate the members of the options struct (`[[= flag{"--json"}]] bool as_json;`) and walk them with `template for` over `std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current())` and `std::meta::annotations_of(member)`, as `json::read_aggregate` does with `json::required` |
| `{"input", number(s.input)}, {"output", number(s.output)}, ...` | walk the aggregate's members; the key is `std::meta::identifier_of(member)` (`json::to_value`; read with `json::read<T>` or `lenient::read<T>`) |
| a `static_assert(sizeof...)` or a count kept beside an enum | `enumerators_of(^^E).size()` |

## Annotation catalog and helpers

| Where | Annotations |
|---|---|
| JSON members (`json.cppm`) | `json::key{"wire"}`, `json::flatten`, `json::skip`, `json::redact`, `json::required` |
| Options structs (`arguments.cppm`) | `flag{"--x"}`, `operand`, `applies_to{...}`, `tolerant` |
| Routes (`routing.cppm`) | `endpoint` |
| Per-enumerator handlers | a `handles{E::x}` annotation on a member function, dispatched by `template for` over `members_of`, with a `static_assert` that each enumerator is handled once or listed as ruleless (`guard.cppm`) |

Helpers: `reflect::enum_count`, `enum_values`, `unique_values`, `starts_at_one`, `enum_name`, `enum_string` (a `std::string`, safe under the sanitizers), `enum_from_name`; `reflect::annotation_of<A>(member)` and `has_annotation<A>(member)` (`members.cppm`); `json::to_value`, `json::read`, `lenient::read`. Call a spliced member function as `this->[:member:](args)`; `(this->*[:member:])(args)` is rejected ("cannot implicitly reference a class member").

Traps: under the ASan flags a `std::string` built from a reflected name inside a constant expression does not compile, so constexpr targets are sanitizer-free; a defaulted `operator==` on an exported struct can crash GCC when its first use is in an importing file; `define_aggregate` members take no default initialisers; callers of `json::read` for a module type need `template class std::optional<T>;` or an explicit `expected` instantiation at the end of the defining module.

## How to apply it

1. Make the wire name equal the identifier when the format is yours to choose. Name enumerators and members exactly as they appear on the wire (`message_start`, `cache_read`).
2. When the wire name cannot be a C++ identifier (`--dry-run`, `x-api-key`), put it in an annotation on the member, not in a second table.
3. Keep the function `constexpr` so the compile-time property checks still cover it.
4. Add a test that adds nothing by hand: iterate `enumerators_of` and assert the round trip, so a new enumerator is covered without editing the test.

## When a hand-written table is right

Only when the text is **not** an identifier of the type, or the table is a function-pointer table the language cannot annotate (`app/llmgate.cpp`, guarded by `handlers_cover_every_command`): an HTTP status phrase, a header name the protocol fixes, a value that differs on purpose from the enumerator. Then add a line to `tools/reflection_lint_allow.txt` with the reason. A line that starts `pending:` is debt to remove, not an exception.

## Checks

- `python3 tools/reflection_lint.py --strict src app tests` fails on a new hand-written mirror and on a stale allowance. It also reports `{Enum::x, &fn}` and `{"GET", "/p", route::x}` tables, raw-string `std::format` JSON templates, `static_cast<Enum>(n % N)` and `std::to_array<std::string_view>` lists that equal an enum.
- A PreToolUse hook (`tools/cpp26_precheck.py`) scans the new text before it is written and a PostToolUse hook (`tools/reflection_hook.py`) the whole file afterwards; both return the findings as context and never block.

## GCC 16.2 facts (verified by `ctest -L capability`, probes in `tests/constexpr/capabilities`)

The reflection posts are mostly written for the Bloomberg clang fork. On GCC 16.2.0 with `-freflection`:

| Do | Do not |
|---|---|
| `std::meta::annotations_of_with_type(member, ^^A)` and test `.empty()` or take `[0]` | `annotation_of_type` does not exist (the must-fail probe `absent/annotation_of_type.cpp` fails the day a compiler adds it) |
| Annotation payloads that hold text as `char text[N]` filled by a consteval factory, as `flag` does in `arguments.cppm` | a `std::string_view` member in an annotation |
| A `char const*` payload only when a consteval factory builds it with `std::define_static_string(std::string_view)` | `define_static_string(char const*)`: it takes a string_view or range |
| `std::define_static_array(...)` around `enumerators_of` and `nonstatic_data_members_of` before `template for` | iterating the raw `std::vector<info>` in `template for`: it is not a constant range |
| `std::remove_cvref_t` before using `type_of(m)` as a key or comparing it | using `type_of(m)` of a `const` or reference member as a key without stripping it |
| `std::meta::identifier_of(e)` (null-terminated: `n.data()[n.size()] == 0`) as a `string_view` | assuming `display_string_of(^^T)` text: it is implementation-defined, never parse or compare it |
| `std::meta::subobjects_of` to include bases; `nonstatic_data_members_of` skips them | `namespace_members_of`, `info_pack` and the `reflect_*` libraries from the posts |
| `access_context::current()` in the type's own scope, `unchecked()` to read private members, `unprivileged()` for public only | reading private members under `unprivileged()` |
| `enumerators_of(^^E).size()` as a constant, and a pairwise `extract<E>` comparison to detect alias enumerators | trusting a hand-kept count |
| `define_aggregate` from a `consteval {}` block in a class template (probe `define_aggregate_from_consteval_block`) | (not probed: other contexts) |
| An aggregate `std::formatter<T>` built with `template for` and `std::format_to` | (not probed: types with bases) |
| `std::meta::current_function()` as a template default argument, walked with `parent_of` through members and lambdas (probe `caller_namespace_guard`) | `requires` on the caller (unreadable diagnostic) |
| `this->[:member:](x)` over annotated member functions (probe `annotated_member_function_dispatch`) | `(this->*[:member:])(x)` |
| `#embed` into a `constexpr char[]` and parse it at compile time | `std::format` or `std::to_string` in constexpr code: `__cpp_lib_constexpr_format` is undefined, so build text with `text::append_all` |
| `std::string{reflect::enum_name(v)}` at run time, also under the sanitizers; corpus targets compile with `-fno-sanitize=all` | an `#include` of a standard header next to `import std` in a module: measured 2.2x slower and `__mbstate_t` conflicts; only `<meta>` and `<contracts>` sit in the global module fragment today |

Probes are plain translation units, so module-specific behaviour (explicit instantiation of `std::optional<T>` for module types, coroutines exported from interfaces) is in the README "Toolchain findings" table, not here. Add a probe to `tests/constexpr/capabilities/supported` before relying on a new feature; move it to `absent` when it fails.
