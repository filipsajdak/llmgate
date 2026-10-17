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
| `{"input", number(s.input)}, {"output", number(s.output)}, ...` | walk the aggregate's members; the key is `std::meta::identifier_of(member)` (the generic writer is a pending item, see below) |
| a `static_assert(sizeof...)` or a count kept beside an enum | `enumerators_of(^^E).size()` |

## How to apply it

1. Make the wire name equal the identifier when the format is yours to choose. Name enumerators and members exactly as they appear on the wire (`message_start`, `cache_read`).
2. When the wire name cannot be a C++ identifier (`--dry-run`, `x-api-key`), put it in an annotation on the member, not in a second table.
3. Keep the function `constexpr` so the compile-time property checks still cover it.
4. Add a test that adds nothing by hand: iterate `enumerators_of` and assert the round trip, so a new enumerator is covered without editing the test.

## When a hand-written table is right

Only when the text is **not** an identifier of the type: an HTTP status phrase, a header name the protocol fixes, a value that differs on purpose from the enumerator. Then add a line to `tools/reflection_lint_allow.txt` with the reason. A line that starts `pending:` is debt to remove, not an exception.

## Checks

- `python3 tools/reflection_lint.py --strict src app` fails on a new hand-written mirror and on a stale allowance.
- A hook runs it on each `src/` or `app/` file you edit and returns the findings as context.
- GCC 16.2 notes: `std::format` is not constexpr here (`__cpp_lib_constexpr_format` is undefined), so in constexpr code build text with `text::append_all`; `std::meta::define_static_string` is how a reflected name becomes a `string_view`.

## Pending

`json::write` takes a `json::value`. A reflection-based `json::to_value<T>(aggregate)` is the missing sibling of `json::read<T>`; until it exists the `key-member-block` findings in `tools/reflection_lint_allow.txt` stay as debt.
