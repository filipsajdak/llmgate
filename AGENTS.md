# Agent instructions

The rules for this repository are in [CLAUDE.md](CLAUDE.md) (C++26 style, comments, checks, gates) and [.github/copilot-instructions.md](.github/copilot-instructions.md) (what to look for in a review). The specification is `docs/llmgate-rollout.md`.

- Build and test the way CI does: `ops/ci/linux.sh` (a container with GCC 16.2); the `host` preset is macOS only.
- Run the lints before reporting done: `python3 tools/cpp26_lint.py --strict`, `python3 tools/reflection_lint.py --strict src app tests`, `python3 tools/coroutine_lint.py --strict src`, `python3 -m unittest discover -s tests/lint`.
- Open a separate pull request instead of pushing to someone else's branch.
