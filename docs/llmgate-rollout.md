# Rollout: a colleague installs it, on any OS, with the providers a company has

Status: design (company rollout, three phases). It follows [docs/llmgate-providers.md](llmgate-providers.md) (BET-8) and comes
before contexts and the two-account orchestrator ([docs/llmgate-contexts.md](llmgate-contexts.md), BET-7).

## Need

A person who knows Claude Code but not llmgate installs it on their own machine, signs in to the providers their
company gives them, and works in a normal Claude Code session. The company can ship its settings as one file. Nobody
builds anything, and nobody needs a particular compiler.

## What a company rollout needs

| Capability | Today | Gap |
|---|---|---|
| Anthropic Messages upstream, Copilot (device flow, enterprise host), ChatGPT plan, Responses translation | exists | none |
| OpenAI chat-completions upstream (Azure OpenAI, other OpenAI-compatible servers, Copilot models that answer only there) with per-server quirk flags | `openai_chat` is refused as not built | new (phase 1 slice 5) |
| AWS Bedrock with a Bedrock API key or SigV4, its event-stream framing, prompt-cache points, tool names over 64 characters, thinking, images and PDF | not built | new (phase 1 slice 6) |
| Streams that start blocks once, keep them in order and send `message_stop` only after usage is known | guard and repair for Messages and Responses | the same for both new translators |
| A binary that runs on a clean machine | the macOS build links three Homebrew libraries (OpenSSL, llhttp, libstdc++) | new (phase 1 X1, slice 1) |
| One-line install, idempotent re-run, `--dry-run`, uninstall and purge, a log | build from source with GCC 16.2 | new (phase 1 slice 2) |
| Terminal first run: which providers, sign in, pick a recipe | the pane's Setup tab, which needs a working gateway first | new (`llmgate init`, slice 3) |
| Company settings as one file: allowed providers, deny rules, announcement, skill marketplace, default recipe, seeded onboarding | deny rules only | new (org pack, slice 7; full pack in phase 2) |
| Corporate proxy and a custom certificate authority | none | new (slice 4) |
| Native Windows | kqueue and epoll reactors, POSIX process handling, 0600 modes, launchd and systemd | new (slice 8, behind X2) |
| Plan on one provider and execute on another, an advisor with a per-session cap, a plan-mode write guard, a planner brief | role routes, a circuit breaker, agent types, spawn rules | phase 2 |
| VS Code settings from the same source | `llmgate env --vscode`, `setup --vscode` | phase 2 |
| Reports by project and week in two currencies; budgets per session, day, week and month | ledger, daily and monthly budgets | phase 2 |
| Doctor with a fix per failure and `--json` | about 17 checks | phase 2 |
| OpenAI-compatible inbound endpoints, OTLP export, gateway-run web search | none | phase 3 |

## Decisions

| # | Decision |
|---|---|
| L1 | Day-one providers: Claude Team login, Copilot seats, Azure OpenAI and other OpenAI-compatible servers, AWS Bedrock. |
| L2 | Distribution: GitHub Releases with prebuilt binaries (macOS arm64 and x86-64, Linux x86-64 and arm64, Windows x86-64), a one-line installer that verifies a checksum, and a Homebrew tap. |
| L3 | Platforms: macOS, Linux, WSL and native Windows. Windows starts with a toolchain experiment (X2). If GCC 16.2 cannot build llmgate there, the answer is WSL and the finding is recorded. If it can, an IOCP reactor sits beside kqueue and epoll. |
| L4 | C++26 is used where it helps the new code, and `docs/cpp26-tour.md` maps each feature to real code, with a status line for each feature GCC 16.2 cannot do yet. There is no showcase-only target. |
| L5 | Kit features, all four, in phase 2: planner and executor with an advisor; the full org pack; VS Code; reports and budgets. |
| L6 | Gateway extras, all four, in phase 3: inbound OpenAI endpoints, OTLP export, web search. Proxy and certificate authority are phase 1. |
| L7 | Three phases before contexts and the orchestrator (BET-7): phase 1 "a colleague installs it", phase 2 "kit parity", phase 3 "llmgate as a hub". |
| L8 | Bedrock authentication: a Bedrock API key (bearer); SigV4 from the environment or a named AWS profile; `credential_process` and `aws configure export-credentials` for SSO. llmgate does not read SSO caches. |
| L9 | Web search runs on a provider that has a native search tool and feeds the result back; an optional search-API key is a second source. No HTML scraping. |
| L10 | Org pack: the public repo defines a JSONC format validated by reflection and ships an example pack. `llmgate org install <git-url or path>` applies one, and the installer accepts one as an argument. A company keeps its real pack in its own repository. |
| L11 | First run: `llmgate init` asks in the terminal which providers the person has, signs in to each, picks a recipe, and writes the config, the profile and the mod. `llmgate claude` then opens with the pane for later changes. |
| L12 | Signing: SHA256 sums and GitHub build-provenance attestations now. Apple notarization and Windows Authenticode when a certificate exists. |
| L13 | Acceptance: a colleague who does not know llmgate installs from a release on their own machine in under 10 minutes and finishes a tool turn on two providers, unaided. One macOS and one Windows colleague, named before the test. |
| L14 | Bedrock per model family: Claude models use the invoke-with-response-stream operation with the Messages body, which keeps thinking signatures, `cache_control` and images; other models use Converse, translated. Both share one event-stream decoder and one SigV4 signer. |
| L15 | This work is not tracked as bets: no bet texts, decision dates or `Bet:` trailers. The phases are an order, not a commitment date. |
| L16 | Pivot rule for phase 1: if Windows is not passing when the colleague test is due, the test runs on macOS, Linux and WSL, and Windows becomes its own phase after phase 3. |

## Requirements the translators must meet (each gets a test)

| # | Requirement |
|---|---|
| T1 | A content block starts once, in index order, whatever order the upstream's events arrive in. |
| T2 | `message_delta` and `message_stop` follow the last block and are sent only after the upstream's usage is known; if usage never arrives within the idle timeout the stream ends with an error event, not a hang. |
| T3 | A tool name the upstream cannot take (over 64 characters on Bedrock) is replaced by a stable hash-suffixed name and mapped back in every response. |
| T4 | Prompt-cache points are placed after the system prompt, after the tools and on the second-to-last message for upstreams that need them; cache read and write tokens map to Anthropic's usage fields. |
| T5 | Thinking: a budget is derived from the request, `max_tokens` is raised above it when needed, and signatures are kept only where the upstream issues them. Reasoning from another provider is dropped. |
| T6 | Images and PDFs travel as the upstream's native blocks, or the request is refused with a named reason. |
| T7 | Tool results keep their order and `is_error`; parallel tool calls stay parallel; a trailing assistant turn is repaired the way the upstream requires. |
| T8 | A server tool is never silently dropped; it is stripped up front with a note, or refused. |
| T9 | Per-server quirks (`max_completion_tokens`, no stop sequences, no reasoning effort with tools, a dummy thought signature) are annotated members of one struct, not branches in the translator. |

## Experiments before building

| # | Question | Answer |
|---|---|---|
| X1 | Can GCC 16.2 with modules, reflection and `import std` produce a relocatable binary: on macOS with static libstdc++, libgcc, OpenSSL 3.5 and llhttp so `otool -L` lists only `/usr/lib`; on Linux a static or old-glibc build? | **macOS: Yes** (2026-10-26, arm64, GCC 16.2.0, OpenSSL 3.6.4 archives from Homebrew). `cmake --preset release-macos` links OpenSSL and llhttp statically and libstdc++ from its archive (`-nostdlib++`: GCC's `-static-libstdc++` has no effect on Darwin); `otool -L` lists only `/usr/lib/libSystem.B.dylib`, the binary is 8 MB and `doctor --offline` runs. It was found that `CMakeLists.txt` set llhttp's static option under the wrong name, so the shared library was always built. Not yet run on a machine without Homebrew; Linux and x86-64 macOS open (due 2026-10-30) |
| X2 | Does GCC 16.2 (MSYS2 UCRT64, or a mingw-w64 cross build) compile `llmgate_reflect`, `llmgate_json` and an `import std` module with `-freflection`? | Open (go or no-go 2026-11-02) |
| X3 | What do Azure OpenAI and Bedrock accept: tools, images, stream usage, SigV4 against a Bedrock API key; and what does each reject? One announced request each, invented prompts, the owner's or Siili's own accounts. | Open (due 2026-11-02) |
| X4 | Do CONNECT through `HTTPS_PROXY`, `NO_PROXY` and a TLS-intercepting proxy with a custom certificate authority work as designed? | Open (due 2026-11-02) |

## Phase 1 slices

| Slice | Content | Gate (fails today) |
|---|---|---|
| 0 | This spec and the bet texts | the experiments table has no empty cell |
| 1 | Release pipeline on a `v*` tag: the X1 binaries, the Windows binary if X2 passed, checksums, attestations, the tap formula | a tag on a fork produces assets; `brew install` works on a clean Mac |
| 2 | `install.sh` and `install.ps1`: download, verify, install, offer Claude Code's installer, run `llmgate init`; idempotent; `--dry-run`, `--uninstall`, `--purge`, `--org`; a log | smoke tests in a throwaway HOME on three OSes in CI |
| 3 | `llmgate init`: prompts over `provider add`, `login`, `models offer`, `recipe apply`; a hidden secret prompt; seeded onboarding; `--yes` with flags | e2e with fake providers and scripted stdin |
| 4 | Proxy and certificate authority: `network.proxy`, `NO_PROXY`, `network.ca_file`, the OS trust store | unit; e2e through a CONNECT proxy and a test CA |
| 5 | Chat-completions translator and the Azure preset; Copilot chat-only models become offerable | corpus, exhaustive and fuzz targets, `fake_chat.py` e2e |
| 6 | Bedrock provider (L14): SigV4, event-stream, invoke passthrough, Converse translator | AWS SigV4 test vectors, `fake_bedrock.py` e2e, a live request |
| 7 | Minimal org pack and `llmgate org install\|show\|remove` | unit, golden diffs, e2e |
| 8 | Windows (if X2 passed): IOCP reactor, process, ACLs, directories, Task Scheduler entry, PowerShell launcher, CI job | ctest on `windows-latest`; installer smoke test |
| 9 | `docs/cpp26-tour.md`, the colleague runs, the decision | two colleague runs recorded |

## Seams a Windows port touches

| Seam | Today | Windows |
|---|---|---|
| Event loop | `src/async/reactor_kqueue.cpp`, `reactor_epoll.cpp` | an IOCP reactor behind the same concept |
| Processes | `src/core/process.cppm` (posix_spawn, detach, signals) | CreateProcess, detached flags, console control events |
| File modes | 0600 on secrets, state and the ledger | ACLs that grant the current user only |
| Directories | XDG and `~/.local/state` | `%APPDATA%` and `%LOCALAPPDATA%` |
| Service | launchd, systemd user unit | a Task Scheduler entry |
| Launcher | sh | PowerShell |
| CI | macOS and Linux | `windows-latest` |
