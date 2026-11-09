# Instructions for Copilot (code review and the coding agent)

llmgate is a clean-room C++26 gateway (GCC 16.2, named modules, `import std`, reflection, contracts) that lets Claude Code use other providers. `CLAUDE.md` is the source of truth for the rules; this file is the short version for reviews.

## What to look for in a review

- **Correctness first:** wrong results, lost errors, resource leaks, races in the coroutine reactor, a path that works on POSIX but not on Windows (the build has a platform layer under `src/platform`).
- **Security:** this repo is public and handles credentials. Flag a token, key or customer name in any file, text or commit; a secret printed or logged; a file created without a private mode; a shell built from input; unpinned third-party GitHub Actions.
- **Repo rules** (flag a violation, do not rewrite style):
  - No comments, except a line starting `// Exception: <reason>` and the closer `}  // namespace x`.
  - Reflection, not hand-written mirrors: no enum-to-string switch, no name tables, no hand-built JSON for a type that has annotations (`json::to_value`, `json::read`, `reflect::enum_name`).
  - Braced construction (designated initializers, initializer lists) instead of build-then-fill; views and moves instead of copies; named constants instead of magic numbers; `std::chrono` for durations.
  - `import std;` in every module file, no standard `#include` beside it.
  - Every function you touch stays at or under CRAP 8 (`tools/coverage.sh`); coverage comes from the end-to-end flows of the `llmgate` binary, so a new branch needs an end-to-end case.
  - Test names say what they prove: `capability_`, `limit_`, `defect_`.
  - Commit messages are ASCII only, conventional, atomic.
- **Clean room:** nothing from a customer's internal tools, hosts or names may appear in the repo.
- **Performance:** the gateway sits on the request path of every model call, so look for work done per request or per byte that could be done once: a copy of a body or buffer that a view or a move would avoid, a body scanned more than once, an allocation or a `std::format` in a hot loop, a lookup that is linear where a map or a `switch` over a reflected enum is available, a blocking call inside a coroutine, a lock held across an `co_await`, an unbounded buffer or map (the soak test runs under LeakSanitizer). Say what the cost is and where; do not flag micro-optimisations in code that runs once at start-up. The benchmark is `ops/bench/ci.sh bench` against `ops/bench/baseline.json`.
- **Consistency:** the change agrees with the rest of the repo. The behaviour matches the numbered requirement in `docs/llmgate-rollout.md` (and the spec is updated when behaviour is added); the README and `config.example.jsonc` match the annotated types (they are generated); a new option, command, error message or log line is named and worded like its neighbours; every platform branch (macOS, Linux, Windows) does the same thing or says why not; a new branch has a test with a name that says what it proves.

## How to report

Lead with the finding that matters most. Give severity (high: wrong result, data loss, secret exposure or a crash; medium: a bypass, a race, a leak or a rule violation; low: wording or small cleanups) and a one-line failure scenario with concrete inputs. Say nothing about a file you found no problem in.

## Do not flag

- Generated text: the config table in `README.md` and `config.example.jsonc` come from `tools/update_config_docs.sh`.
- The `Windows` flows that are skipped on purpose (listed in `docs/llmgate-rollout.md`, WIN13) and the POSIX-only scenarios in `tests/e2e`.
- Style the compiler or `tools/*_lint.py` already enforces.

## Building and testing

CI builds in a container, so do the same:

```sh
ops/ci/linux.sh -L unit            # plain Linux build and the unit tests
PRESET=linux-asan ops/ci/linux.sh  # AddressSanitizer
python3 tools/cpp26_lint.py --strict && python3 tools/reflection_lint.py --strict src app tests && python3 tools/coroutine_lint.py --strict src
python3 -m unittest discover -s tests/lint
```

The `host` preset and `build/host` are for the maintainer's macOS machine.

## Where the specification is

`docs/llmgate-rollout.md` holds the numbered requirements (V1..., C1..., O1..., WIN1..., NT1...). A change that adds behaviour updates it first.

## When you open a pull request

Open a separate pull request, never push to a branch a person is working on; describe the change with a table or `Visual: none, because ...`; end the verification with a `Mutation:` line.
