# A tour of the C++26 in llmgate

llmgate is written to show what C++26 makes easy to build and to keep right: the types are the only copy of every name, flag and field list, the compiler checks what a test would have to, and the toolchain is a single GCC 16.2 with named modules. This page is a map. Every link below is checked by `tools/check_tour.py` (run by `ctest -L lint` and in CI): the file must exist and the symbol next to it must occur in it, so the page cannot drift from the code.

Contents: [reading order](#reading-order), [what the code uses today](#what-the-code-uses-today), [what GCC 16.2 cannot do yet](#what-gcc-162-cannot-do-yet), [possible improvements](#possible-improvements). Rules the examples follow are in [CLAUDE.md](../CLAUDE.md); the toolchain findings are in the [README](../README.md#toolchain-findings-gcc-1620).

## Reading order

1. Start at an annotated struct: [src/gateway/config.cppm](../src/gateway/config.cppm) `struct config`. One declaration holds the key, the type, the default, the documentation and the example of every setting.
2. See how the same declaration is read, written and documented: [src/json/json.cppm](../src/json/json.cppm) `to_value`, then [src/reflect/enum.cppm](../src/reflect/enum.cppm) `enumerator_table`.
3. See reflection drive a command line: [src/app/arguments.cppm](../src/app/arguments.cppm) `struct options`, where each flag is an annotation on a member.
4. See dispatch by annotation and a compile-time completeness check: [src/responses/stream.cppm](../src/responses/stream.cppm) `rules_cover_events`.
5. See modules, `std::expected` and ranges in a translator: [src/bedrock/converse_request.cppm](../src/bedrock/converse_request.cppm) `translate_request`.
6. See constexpr proven against published vectors: [src/bedrock/sigv4.cppm](../src/bedrock/sigv4.cppm) `static_assert(sign(`, at the end of the module.
7. See coroutines on a hand-written reactor: [src/async/async.cppm](../src/async/async.cppm) `class task`.

## What the code uses today

### Annotations are the schema

A setting is one member with annotations: the JSON key, the documentation, an example. The reader, the writer, `config.example.jsonc`, the README table and the JSON schema are all derived from it, so there is no second list to forget.

Without it (C++20): a struct, a hand-written `from_json` that names every key again, a `to_json` that names them a third time, and a documentation table that is edited by hand.

- Code: [src/gateway/config.cppm](../src/gateway/config.cppm) `reflect::doc`
- Code: [src/json/json.cppm](../src/json/json.cppm) `aggregate_target`
- Test: [tests/unit/json_test.cpp](../tests/unit/json_test.cpp) `a_config_is_read_into_the_struct_by_member_name`

### Enumerators know their names

`reflect::enum_name` and `enum_from_name` read the enumerator list from the type, so adding an enumerator needs no switch, no table and no parser change.

Without it (C++20): a `switch` returning string literals and a second `if` chain parsing them back, kept in step by code review.

- Code: [src/reflect/enum.cppm](../src/reflect/enum.cppm) `enumerator_table`
- Test: [tests/unit/reflect_enum_test.cpp](../tests/unit/reflect_enum_test.cpp) `enum_from_name_round_trips_every_enumerator`

### `template for` walks members and enumerators

A `template for` over `std::meta::enumerators_of` or `nonstatic_data_members_of` replaces index sequences and fold expressions. The init wizard's menu is built from the `kind` enum this way, including each entry's documentation.

Without it (C++20): `std::index_sequence`, a helper lambda per member and a table of strings.

- Code: [src/app/wizard.cppm](../src/app/wizard.cppm) `summary_of`
- Test: [tests/unit/wizard_test.cpp](../tests/unit/wizard_test.cpp) `the_menu_lists_every_kind_with_its_summary_from_the_enum`

### Dispatch by annotation, complete at compile time

A handler for an upstream event is a member marked `[[=handles{event}]]`. A `static_assert` fails the build when an event has no handler and is not listed as deliberately ignored.

Without it (C++20): a `switch` that falls through to `default:` and silently ignores the event nobody remembered.

- Code: [src/responses/stream.cppm](../src/responses/stream.cppm) `rules_cover_events`
- Test: [tests/unit/responses_stream_test.cpp](../tests/unit/responses_stream_test.cpp) `translated_streams_pass_the_guard_with_no_repair_at_all`

### `current_function()` as a layer guard

A function that only one layer may call takes `template <std::meta::info Caller = std::meta::current_function()>` and a `static_assert` on the caller's namespace. The architecture rule is enforced by the compiler, not by a diagram.

Without it (C++20): a comment saying "only call this from commands" and a lint that greps for it.

- Code: [src/core/process.cppm](../src/core/process.cppm) `current_function`
- Test: [tests/constexpr/capabilities/supported/caller_namespace_guard.cpp](../tests/constexpr/capabilities/supported/caller_namespace_guard.cpp) `current_function`

### `#embed` puts files in the binary

The Claude Code mod ships inside the `llmgate` executable: its files are embedded at build time, hashed, and written out on first use. One file to install, nothing to get out of step.

Without it (C++20): `xxd -i` run from CMake, a generated source file and a build dependency on `xxd`.

- Code: [src/claude/mod_bundle.cppm](../src/claude/mod_bundle.cppm) `#embed`
- Test: [tests/constexpr/capabilities/supported/embed_into_constexpr_array.cpp](../tests/constexpr/capabilities/supported/embed_into_constexpr_array.cpp) `#embed`

### Contracts at API boundaries

Preconditions are written as `pre(...)` on the declaration and `contract_assert` inside a body, so the requirement is part of the interface rather than a comment above it.

Without it (C++20): `assert()` that vanishes in release builds, or an `if` that throws.

- Code: [src/async/async.cppm](../src/async/async.cppm) `pre(op != nullptr)`
- Code: [src/async/reactor_kqueue.cpp](../src/async/reactor_kqueue.cpp) `contract_assert`
- Test: [tests/unit/async_test.cpp](../tests/unit/async_test.cpp) `run_returns_the_value_of_a_task`

### constexpr by default, with the proof beside it

The SigV4 signer is constexpr. `static_assert`s at the end of the module check it against the published AWS test-suite signature and key derivation, so a wrong constant fails the build, not a deployment.

Without it (C++20): a runtime unit test that has to be run, and a vector pasted into it.

- Code: [src/bedrock/sigv4.cppm](../src/bedrock/sigv4.cppm) `5fa00fa31553b73ebf1942676e86291e8372ff2a2260956d9b8aae1d763fbf31`
- Test: [tests/unit/sigv4_test.cpp](../tests/unit/sigv4_test.cpp) `a_colon_in_the_path_is_signed_encoded_twice_and_a_plain_path_once`

### Named modules and `import std`

Every `.cppm` starts with `import std;`. There are no standard headers beside it, no include guards and no header/source pairs to keep equal. CMake builds the dependency order.

Without it (C++20): headers, include-what-you-use discipline and precompiled headers.

- Code: [src/bedrock/converse_request.cppm](../src/bedrock/converse_request.cppm) `import std;`
- Code: [CMakeLists.txt](../CMakeLists.txt) `CMAKE_EXPERIMENTAL_CXX_IMPORT_STD`
- Test: [tests/unit/converse_test.cpp](../tests/unit/converse_test.cpp) `text_system_and_limits_map_to_the_converse_shape`

### `std::expected` for failures that are expected

A request that cannot be translated is a value (`std::expected<translated, refusal>`), not an exception, so the caller must look at it and the reason travels with it.

Without it (C++20): exceptions across a coroutine boundary, or an out-parameter and a bool.

- Code: [src/bedrock/converse_request.cppm](../src/bedrock/converse_request.cppm) `std::expected<translated, refusal>`
- Test: [tests/unit/converse_test.cpp](../tests/unit/converse_test.cpp) `a_body_that_is_not_an_object_or_has_no_messages_is_refused`

### Coroutines on a reactor of our own

The gateway's I/O is `co_await` over a small kqueue/epoll reactor, so a streamed answer is straight-line code instead of a chain of callbacks. The reactor destroys unfinished tasks when it goes away, which LeakSanitizer once told us it had to.

Without it (C++20): callbacks or a thread per connection.

- Code: [src/async/async.cppm](../src/async/async.cppm) `class task`
- Test: [tests/unit/async_test.cpp](../tests/unit/async_test.cpp) `awaiting_tasks_chains_their_results`

### Ranges instead of index loops

Splitting, filtering and collecting are `std::views::split`, `std::views::filter` and `std::ranges::to`. Parsers read as what they do.

Without it (C++20): `find` and `substr` in a loop with manual index arithmetic.

- Code: [src/bedrock/aws_credentials.cppm](../src/bedrock/aws_credentials.cppm) `std::views::split`
- Test: [tests/unit/aws_credentials_test.cpp](../tests/unit/aws_credentials_test.cpp) `ini_sections_keys_and_comments_are_read`

### `std::format` and `std::print`

All text is `std::format`, `std::print` or `std::format_to`; there is no iostream and no `printf`. At run time that is type-safe, and in constexpr code `text::append_all` fills the gap.

Without it (C++20): `std::ostringstream` and `snprintf`.

- Code: [src/app/doctor.cppm](../src/app/doctor.cppm) `std::format`
- Code: [app/llmgate.cpp](../app/llmgate.cpp) `std::println`
- Test: [tests/unit/doctor_test.cpp](../tests/unit/doctor_test.cpp) `the_aws_check_names_where_credentials_come_from_fails_with_a_remedy_and_is_absent_without_a_signing_provider`

### `define_static_string` and `define_aggregate`

`define_static_string` turns a reflected name into a pointer that lives for the whole program, so `enum_name` can return a `string_view` at run time. `define_aggregate` generates a struct from a list of members.

Without it (C++20): a name table in a `.cpp` file, and a hand-written counter struct per set of repairs.

- Code: [src/reflect/enum.cppm](../src/reflect/enum.cppm) `define_static_string`
- Code: [src/gateway/gateway.cppm](../src/gateway/gateway.cppm) `define_aggregate`
- Test: [tests/constexpr/capabilities/supported/define_aggregate_from_consteval_block.cpp](../tests/constexpr/capabilities/supported/define_aggregate_from_consteval_block.cpp) `define_aggregate`

### `std::function_ref` for a callback that is only borrowed

A parser hands each event to a callback that lives for the call. `std::function_ref` says so and costs no allocation, where `std::function` would copy the callable.

Without it (C++20): `std::function`, or a template parameter that leaks into every signature.

- Code: [src/http/http.cppm](../src/http/http.cppm) `std::function_ref`
- Test: [tests/unit/http_test.cpp](../tests/unit/http_test.cpp) `chunked_response_is_decoded`

## What GCC 16.2 cannot do yet

Each row has a probe in `tests/constexpr/capabilities/absent` that must fail to compile today. The day GCC gains the facility the probe compiles, its test fails, and this row is the one to update.

| Facility | Workaround in the code | Probe |
|---|---|---|
| `std::hive` | not needed yet | [tests/constexpr/capabilities/absent/hive.cpp](../tests/constexpr/capabilities/absent/hive.cpp) |
| `<linalg>` | not needed | [tests/constexpr/capabilities/absent/linalg.cpp](../tests/constexpr/capabilities/absent/linalg.cpp) |
| `std::is_trivially_relocatable` | moves | [tests/constexpr/capabilities/absent/trivial_relocatability.cpp](../tests/constexpr/capabilities/absent/trivial_relocatability.cpp) |
| `std::is_within_lifetime` | not needed | [tests/constexpr/capabilities/absent/is_within_lifetime.cpp](../tests/constexpr/capabilities/absent/is_within_lifetime.cpp) |
| `std::add_sat` and the other saturating operations | a three-line local `usage::add_sat` | [tests/constexpr/capabilities/absent/saturating_arithmetic.cpp](../tests/constexpr/capabilities/absent/saturating_arithmetic.cpp) |
| `std::meta::annotation_of_type` | `annotations_of_with_type` | [tests/constexpr/capabilities/absent/annotation_of_type.cpp](../tests/constexpr/capabilities/absent/annotation_of_type.cpp) |

`<simd>` is a case where the toolchains differ: it compiles with the Linux GCC 16.2 and does not with Homebrew's GCC 16.2 on macOS, so there is no single probe for it and the project does not use it. Other limits are findings about this compiler rather than missing facilities; they are in the README's [toolchain findings](../README.md#toolchain-findings-gcc-1620) (stdexec with modules, contracts on exported inline functions, and so on).

## Possible improvements

These are facilities GCC 16.2 already supports (each has a probe in `tests/constexpr/capabilities/supported` that compiles today) which the code does not use yet. They are options to weigh, not commitments.

| Idea | Where it would change something | Verdict | Probe |
|---|---|---|---|
| Generate the legacy provider mirror with `define_aggregate` instead of copying members by hand and checking with a `static_assert` | [src/gateway/config.cppm](../src/gateway/config.cppm) `struct upstream_config` | try | [tests/constexpr/capabilities/supported/define_aggregate_from_consteval_block.cpp](../tests/constexpr/capabilities/supported/define_aggregate_from_consteval_block.cpp) |
| Generate shell completions and a man page from the annotated flags | [src/app/arguments.cppm](../src/app/arguments.cppm) `struct options` | try | [tests/constexpr/capabilities/supported/annotations_on_members.cpp](../tests/constexpr/capabilities/supported/annotations_on_members.cpp) |
| `std::inplace_vector` for lists with a hard cap, so the cap is in the type | [src/gateway/gateway.cppm](../src/gateway/gateway.cppm) `most_rate_limit_headers` | try | [tests/constexpr/capabilities/supported/inplace_vector.cpp](../tests/constexpr/capabilities/supported/inplace_vector.cpp) |
| `std::flat_map` for the small counter maps, which are read far more than written | [src/gateway/gateway.cppm](../src/gateway/gateway.cppm) `dropped_fields` | measure first | [tests/constexpr/capabilities/supported/flat_map.cpp](../tests/constexpr/capabilities/supported/flat_map.cpp) |
| `std::optional<T&>` instead of a pointer for a lookup that may find nothing | [src/json/json.cppm](../src/json/json.cppm) `constexpr value const* at(` | try | [tests/constexpr/capabilities/supported/optional_reference.cpp](../tests/constexpr/capabilities/supported/optional_reference.cpp) |
| `std::views::enumerate` and `stride` where a loop carries an index only to read an element | [src/bedrock/converse_request.cppm](../src/bedrock/converse_request.cppm) `convert_blocks` | try | [tests/constexpr/capabilities/supported/ranges_enumerate_stride.cpp](../tests/constexpr/capabilities/supported/ranges_enumerate_stride.cpp) |
| `std::copyable_function` for stored callbacks that are copied today through `std::function` | [src/gateway/gateway.cppm](../src/gateway/gateway.cppm) `using reload_source` | try | [tests/constexpr/capabilities/supported/copyable_function.cpp](../tests/constexpr/capabilities/supported/copyable_function.cpp) |
| `= delete("reason")` so a forbidden overload says why in the error | [src/async/shutdown.cppm](../src/async/shutdown.cppm) `stop_signal(stop_signal const&) = delete` | try | [tests/constexpr/capabilities/supported/deleted_with_reason.cpp](../tests/constexpr/capabilities/supported/deleted_with_reason.cpp) |
| `std::generator` for a pull-style frame reader (the event-stream decoder is push-style today) | [src/bedrock/eventstream.cppm](../src/bedrock/eventstream.cppm) `class decoder` | defer: the stream is fed in pieces, which suits a sink | [tests/constexpr/capabilities/supported/generator.cpp](../tests/constexpr/capabilities/supported/generator.cpp) |
| constexpr exceptions to drop `expected` plumbing in the parsers | [src/json/json.cppm](../src/json/json.cppm) `parse_error` | defer: an `expected` makes the failure visible to the caller | [tests/constexpr/capabilities/supported/constexpr_exception.cpp](../tests/constexpr/capabilities/supported/constexpr_exception.cpp) |
| Pack indexing (`Ts...[0]`) in the small variadic helpers | [src/core/text.cppm](../src/core/text.cppm) `append_all` | try | [tests/constexpr/capabilities/supported/pack_indexing.cpp](../tests/constexpr/capabilities/supported/pack_indexing.cpp) |
