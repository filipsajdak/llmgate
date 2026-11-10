# llmgate

Clean-room C++26 gateway (GCC 16.2, named modules, `import std`, `-freflection`). The type is the only copy of every name, flag, field list and role. Overview, commands and design decisions are in [README.md](README.md); this file holds only the rules a session must follow.

## C++26 rules

- **Reflection, not mirrors.** No enum-to-string switch, name table, flag `if` chain, `{"key", x.key}` block, `std::format` JSON template, `{Enum::x, &fn}` table or `n % 5` over an enum. Use `reflect::enum_name`, `reflect::enum_from_name`, annotated members and `json::read`/`json::to_value`. Skill: `reflect-dont-hand-write`.
- **Annotations are the schema.** JSON: `json::key{"wire"}`, `flatten`, `skip`, `redact`, `required`; flags: `flag`, `operand`, `applies_to`, `tolerant`; routes: `endpoint`. Read with `json::read<T>` (strict) or `lenient::read<T>`, write with `json::to_value`. Helpers: `reflect::enum_count`, `enum_values`, `unique_values`, `starts_at_one`, `enum_string`, `annotation_of`, `has_annotation`.
- **Dispatch by annotation.** A per-enumerator handler is a member marked `[[=handles{E::x}]]` and found with `template for`, with a `static_assert` that every enumerator is handled once or listed as ruleless (`anthropic::detail::rules_cover_events`). `app/llmgate.cpp` keeps its handler table, justified in the allowance file by `handlers_cover_every_command`.
- **Layers.** A function only one layer may call takes `template <std::meta::info Caller = std::meta::current_function()>` and a `static_assert` on the caller's enclosing namespaces (`process::start_detached`, callable from `llmgate::commands` only).
- **Concepts.** A template parameter that is called, awaited or used as a stream or sink is constrained (`std::invocable`, `net::byte_stream`, a `requires` clause).
- **Modules only.** `import std;` in every `.cppm` and `.cpp`; the global module fragment holds C, POSIX, OpenSSL, `<meta>` and `<contracts>` headers only.
- **Text.** `std::format`, `std::print`, `std::println` at run time; `text::append_all` in constexpr code. No iostream, no `printf`, no chains of `+=` or `push_back`, no `std::string(1, c)` or `a + b + c` temporaries: append each part once (`text::append_all`, `format_to`, or a `std::views::split` that appends each segment). A name/value pair is `text::name_value`, never `std::pair<std::string, std::string>`.
- **constexpr by default**, with a compile-time check beside it (`*_holds`, corpus and exhaustive targets). **Structural `static_assert`s** state each type property once. **Contracts** guard API boundaries; `std::expected` carries expected failures.
- **Braced construction** (designated initializers, initializer lists), views and moves instead of copies, named constants instead of magic numbers, `std::chrono` for durations (mp-units adoption is still open, so do not introduce it piecemeal).
- Skill `cpp26-idioms` has the replacements.

## Comments

None, except a line that starts `// Exception: <reason>` and the closer `}  // namespace x`. The reason a rule holds lives in a named test, a requirement or the commit message. `tools/cpp26_lint.py` fails any other comment (it reads strings, raw strings and char literals, so JSONC inside a test string is fine).

## Copilot review

Copilot reviews every pull request. A reply to one of its review comments starts with `@copilot` and asks it to recheck against the fixing push (a hook enforces it); work can be delegated to it from GitHub with a PR comment that asks for a separate pull request. Skill: `copilot-collaboration`.

## Claude in the cloud

A comment that contains `@claude`, written by the owner, a member or a collaborator on an issue or a pull request that one of them also wrote (an open pull request from this repository, never a fork), starts `.github/workflows/claude.yml`. It has two jobs. The agent job runs the Claude Code action (pinned by commit) with the job's own token only: `contents: read`, and write for comments. It sees only comments by `filipsajdak`, loads user settings only (hooks off), edits inside the checkout and not under `.git/`, `.github/`, `.claude/`, `tools/`, `ops/`, `cmake/`, `tests/lint/`, `tests/release/`, `CMakeLists.txt`, `CMakePresets.json`, `CLAUDE.md` or `AGENTS.md`, cannot `git rm`, and runs nothing that lives in the repository. It cannot push. The commits it makes are bundled and handed to the publish job, which has no agent: `tools/claude_publish.sh` allows changes only under `src/`, `app/`, `docs/`, `examples/`, `mods/`, `tests/{unit,e2e,constexpr,fuzz,network,support,compat}/`, `README.md` and `config.example.jsonc` (an allowlist: anything else is refused, `install/`, the build files and every dotfile among it), refuses a path with a character outside `A-Za-z0-9._/-` (git would quote it), a submodule or a symlink (any mode but 100644 and 100755) in any commit, a merge commit, text that looks like a token anywhere in the history (messages, author fields, text and binary content), a bundle that does not build on the base, and more than 20 commits (the bundle's objects are fsck-checked). Before any git command runs after the agent, the job replaces `.git/config` with a four-line safe one and removes `.git/hooks` and `.git/info/attributes`, so a hook, an include or a driver the agent's tool calls wrote cannot run, and the action's token URL is gone from the config. The agent job runs the same verifier, from a copy of the default branch's script made before the agent started (a pull request that edits the script cannot supply it), before it uploads the bundle, so a refused bundle is never uploaded (a public repository's artifacts are readable by anyone). Then it pushes `claude/run-N-M` and comments a compare link. You open the pull request from that link; the gates run when you open it. The only secret is `CLAUDE_CODE_OAUTH_TOKEN` (no GitHub App); `CLAUDE_CODE_SUBPROCESS_ENV_SCRUB=1` keeps it, and the job token, out of the environment of every command the agent runs (a best-effort scrub by the action, which it applies by default only for `allowed_non_write_users`). Known limit: a prompt injection that reaches the agent can read the job token from `.git/config` and put it in a commit message, but that token is read-only for contents, ends with the job, and the publisher refuses token-looking text.

## Checks

```bash
python3 tools/reflection_lint.py --strict src app tests
python3 tools/cpp26_lint.py --strict
python3 tools/coroutine_lint.py --strict src
python3 -m unittest discover -s tests/lint
ctest --preset host -L lint
ctest --preset host -L capability      # what GCC 16.2 supports, one probe per feature
```

Both lints read `path|rule|text|reason` allowance files (`tools/reflection_lint_allow.txt`, `tools/cpp26_lint_allow.txt`). `pending:` is debt that names the tier that pays it; any other reason is a justified exception; a stale line fails. Hooks: `tools/cpp26_precheck.py` (before a write, advisory) and `tools/reflection_hook.py` (after a write). They never block.

## Review pass before opening a pull request

Gates catch what a rule can state; a review pass catches the rest. Before a pull request, review your own diff on four counts and fix or answer what you find, so Copilot's review is the second opinion and not the first:

- **Correctness:** run `/code-review` on the branch; read the failure paths and the platform branches.
- **Security:** run `/security-review`; this is a public repo that handles credentials, so look for secrets in text or logs, files created without a private mode, shell built from input and unpinned actions.
- **Performance:** a change on the request path (`src/gateway`, `src/http`, `src/json`, translators) is compared with `ops/bench/baseline.json` (`ops/bench/ci.sh bench`), and `/simplify` looks for needless copies, repeated scans and allocations.
- **Consistency:** the behaviour matches its requirement in `docs/llmgate-rollout.md` and the spec is updated first for new behaviour; `tools/update_config_docs.sh --check readme` and `--check example` are clean; names and messages match their neighbours; every platform branch agrees.

## Gates before reporting done

`cmake --preset host && cmake --build --preset host && ctest --preset host -j1`, the lints above, and for a change to source `tools/coverage.sh` (no function above CRAP 8). Details: README sections [Build](README.md#build), [CRAP gate](README.md#crap-gate), [Sanitizers and checks](README.md#sanitizers-and-checks), [Continuous integration](README.md#continuous-integration).

## Does not work on GCC 16.2

Under the ASan flags a `std::string` built from a reflected name in a constant expression does not compile: constexpr targets use `-fno-sanitize=all` or `reflect::enum_string`. A defaulted `operator==` on an exported struct can crash GCC if its first use is in an importing file: write it where the struct is tested. `define_aggregate` members have no default initialisers. A constexpr function returning `std::optional<T>` or `expected<T,...>` of a module type needs an explicit instantiation (`json::read` callers).

Also missing: `annotation_of_type` (use `annotations_of_with_type`); `define_static_string(char const*)` (takes a string_view or range); `std::format` in constexpr code; `string_view` or `char const*` annotation members (use `char[N]` or a consteval-built pointer); a standard `#include` beside `import std`; stdexec with modules; `std::add_sat` through `import std`; contracts on an inline function exported from a module; a non-template coroutine with its body in an interface. The full table is [Toolchain findings](README.md#toolchain-findings-gcc-1620); the reflection part is also in the `reflect-dont-hand-write` skill.
