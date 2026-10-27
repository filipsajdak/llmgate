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
| X1 | Can GCC 16.2 with modules, reflection and `import std` produce a relocatable binary: on macOS with static libstdc++, libgcc, OpenSSL 3.5 and llhttp so `otool -L` lists only `/usr/lib`; on Linux a static or old-glibc build? | **macOS: Yes** (2026-10-26, arm64, GCC 16.2.0, OpenSSL 3.6.4 archives from Homebrew). `cmake --preset release-macos` links OpenSSL and llhttp statically and libstdc++ from its archive (`-nostdlib++`: GCC's `-static-libstdc++` has no effect on Darwin); `otool -L` lists only `/usr/lib/libSystem.B.dylib`, the binary is 8 MB and `doctor --offline` runs. It was found that `CMakeLists.txt` set llhttp's static option under the wrong name, so the shared library was always built. Not yet run on a machine without Homebrew. x86-64 macOS: Yes in CI (2026-10-27, `macos-15-intel`), after `-static-libgcc` was added because Intel linked Homebrew's `libgcc_s` dylib. **Linux: Yes with a fully static glibc build** (2026-10-26, aarch64 only, `gcc:16` image with glibc 2.41 and OpenSSL 3.5.7 static archives; `-DOPENSSL_USE_STATIC_LIBS=ON -DLLMGATE_STATIC_RUNTIME=ON -DCMAKE_EXE_LINKER_FLAGS=-static`; about 15 minutes per build). The result is a static ELF of 13 MB (10.5 MB stripped) with no `NEEDED` entries; `--version` and `doctor --offline` run in bare `debian:trixie-slim`, `debian:bookworm-slim`, `ubuntu:22.04`, `ubuntu:20.04` and `alpine`, and `login` reached github.com (DNS and TLS) on Alpine and, with `ca-certificates` installed, on the others. The linker warns that static `getaddrinfo` wants a matching glibc at run time; DNS worked on glibc 2.31 and on musl, and a host whose `nsswitch.conf` needs a non-builtin module (LDAP, mdns) was not tested. The variant with only libstdc++ and libgcc static is not relocatable: it needs `GLIBC_2.38` and `libzstd.so.1` (from Debian's `libcrypto.a`). Without a system CA bundle (bare `trixie-slim`) both fail with `github_unreachable`, so the binary needs a bundled or discovered trust store (see X4). x86-64 Linux: Yes in CI (2026-10-27, `ubuntu-24.04`): the same static build ran on `debian:bookworm-slim` and `ubuntu:20.04` |
| X2 | Does GCC 16.2 (MSYS2 UCRT64, or a mingw-w64 cross build) compile `llmgate_reflect`, `llmgate_json` and an `import std` module with `-freflection`? | **Yes, on a real Windows runner** (2026-10-27, [run 40418285880](https://github.com/filipsajdak/llmgate/actions/runs/40418285880), `windows-latest`, MSYS2 UCRT64 GCC 16.2.0 Rev6, OpenSSL 3.6.5). The preinstalled MSYS2 plus `msys2/setup-msys2` works; the CMake configure with the `import std` support succeeded unchanged and the five unit tests of the modules that need no operating-system layer (`reflect_enum_test`, `json_test`, `json_writer_test`, `lenient_test`, `jsonscan_test`) built and passed. Still open: the OS-bound modules (reactor, process, TLS sockets, HTTP) and the rest of the suite; slice 8. Earlier cross-build finding, 2026-10-26, nothing run on Windows then: A stock x86_64-w64-mingw32 GCC 16.2.0 cross compiler (binutils 2.45, mingw-w64 v13, UCRT, winpthreads) built in Docker compiles with `-std=c++26 -freflection -fcontracts -Wall -Wextra -Werror -fmodules`: a `<meta>` reflection test with `static_assert`; `import std` from its `bits/std.cc` (libstdc++ ships `std.cc`, `std.compat.cc` and `libstdc++.modules.json`); and the modules `require`, `text`, `reflect`, `members`, `jsonscan`, `json`, `lenient`, `doc`, `role` plus `contract_violation.cpp`, with no warnings and no mingw workaround; an exe importing `llmgate.reflect` and `llmgate.json` linked. `reflect` and `json` include only `<meta>` in their global fragments. Packages: MSYS2's `mingw-w64-gcc` base package is 16.2.0-6 (UCRT64 build likely, unconfirmed), winlibs has GCC 16.2.0 with mingw-w64 14 UCRT (r3, 2026-10-18); Debian trixie's mingw is 14.2. Not verified: running any exe; a native MSYS2 build; the CMake path (`CMAKE_CXX_MODULE_STD` with Ninja module scanning, replaced by manual compiles here); the larger modules, OpenSSL and llhttp on MSYS2; whether `windows-latest` has MSYS2. One MSYS2 UCRT64 CI job settles those, and needs a pushed branch. The reactor, process, TLS sockets and HTTP modules still need the Windows port (slice 8) |
| X3 | What do Azure OpenAI and Bedrock accept: tools, images, stream usage, SigV4 against a Bedrock API key; and what does each reject? One announced request each, invented prompts, the owner's or Siili's own accounts. | Open (due 2026-11-02) |
| X4 | Do CONNECT through `HTTPS_PROXY`, `NO_PROXY` and a TLS-intercepting proxy with a custom certificate authority work as designed? | **Partly** (2026-10-26, local harness: CONNECT proxy, Basic-auth proxy, TLS-intercepting proxy with a second CA, a test origin). llmgate today ignores `HTTPS_PROXY`, `ALL_PROXY` and `NO_PROXY` (`src/gateway/fetch.cpp` connects directly) and does not read the macOS keychain (the binary links no Security framework). It honours `SSL_CERT_FILE` and `SSL_CERT_DIR` through `SSL_CTX_set_default_verify_paths`, then tries three fixed bundle paths (`src/tls/tls.cppm:114-117`); `client_context::make` already takes an `extra_trusted_pem` argument that no caller feeds. The compiled-in OpenSSL directory is the build host's Homebrew one, so a release binary on a Mac without Homebrew has only `/etc/ssl/cert.pem` (128 roots, against 198 under Homebrew). An untrusted issuer reports only `handshake_failed`; curl names the issuer problem. With curl all four proxy cases behave as expected (plain tunnel, 407 without credentials, 200 with them, interception only when the corporate CA is trusted). Not tested: llmgate through a proxy, because it sends no CONNECT. Decisions that follow: `network.proxy` from env by default with `NO_PROXY` (domain suffix, CIDR, `*`), `network.ca_file` feeding `extra_trusted_pem`, a macOS keychain export (`security find-certificate -a -p`) and the Windows ROOT store as trust sources, and a handshake error that names the verification result. |

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

## Installer (phase 1 slice 2)

`install/install.sh` is POSIX sh and runs the same under `curl | sh` and from a clone. `install.ps1` follows with the Windows binary (slice 8); until then Windows uses WSL.

| # | Behaviour |
|---|---|
| I1 | It detects the system and architecture, resolves the latest release (or `--version`), downloads the archive and `SHA256SUMS.txt`, and refuses to install anything whose digest does not match. |
| I2 | It installs to `~/.local/bin` (or `--prefix`) by writing a new file beside the old one and renaming it, keeps the previous binary as `llmgate.previous`, and runs the new binary's `--version` before replacing anything. |
| I3 | A re-run with the same version changes nothing; `--force` reinstalls. |
| I4 | `--dry-run` prints every step and writes nothing, not even a log. |
| I5 | It never edits a shell profile; when the prefix is not on `PATH` it prints the line to add. |
| I6 | If `claude` is missing it says so and offers Claude Code's official installer; it runs it only after a yes at the terminal or with `--with-claude`, never with `--yes` alone. |
| I7 | It then runs `llmgate init` when the binary has it, else `llmgate setup`, reading answers from the terminal even under `curl | sh`; with no terminal it prints the command instead. `--no-init` skips this. |
| I8 | A log of every step goes to `~/.local/state/llmgate/install-TIMESTAMP.log`. |
| I9 | `--uninstall` stops the gateway and removes the binary and `llmgate.previous`. `--purge` also lists, then removes after a yes (or `--yes`), `~/.config/llmgate`, `~/.local/state/llmgate` and `~/.claude-llmgate*`. It never touches `~/.claude`. |
| I10 | Versions and prefixes are checked before use: a version must be `MAJOR.MINOR.PATCH`, and nothing from the network is run except the archive's binary after its digest matched. |

The shell test `tests/e2e/install_flow.sh` serves a fake release from a local server and checks I1 to I5, I8 to I10.

## First run: `llmgate init` (phase 1 slice 3)

| # | Behaviour |
|---|---|
| W1 | `init` creates the configuration file when it is missing (only `listen`), never overwrites one that exists, and works when the configuration does not validate yet. Running it again is safe: a provider name that already exists is skipped with a message. |
| W2 | Interactively it shows a numbered menu of the kinds of provider a person may have and takes a list such as `1,3`. The kinds are: a Claude login, a GitHub Copilot seat (github.com or an enterprise host), a ChatGPT plan, another gateway reached with a key, and a local server. The menu text comes from annotations on the kind enum. |
| W3 | For each kind it asks only what is needed (a name, a host or URL, how the key is sent), then makes the same edit as `provider add` and prints the diff, so the person sees what changed. |
| W4 | A key is read hidden from the terminal, stored under `~/.config/llmgate/secrets/NAME.key` with mode 0600, and never printed or put in the configuration. |
| W5 | Copilot and ChatGPT sign in during `init` with the same device-code or browser flow as `llmgate login`. A sign-in that fails leaves the provider in place and says how to retry. |
| W6 | A Claude login sets `claude.credential` to `login` and tells the person to type `/login` once in Claude Code. |
| W7 | After a provider answers, `init` offers its recommended models (the same filter as `models offer all`) and applies them on a yes. A provider that cannot list models is not an error; `init` says how to offer models later. |
| W8 | Recipes whose needs the providers meet are listed with a one-line summary; a blank answer takes the first and 0 keeps things as they are. |
| W9 | `init` then writes the profile as `setup` does. Whenever the profile is written (`setup` and `init` alike) and `claude.credential` is `gateway`, it also marks onboarding as done in the profile's `.claude.json` (only the one missing key, never overwriting anything), so there is no login screen; with `login` it leaves onboarding alone. |
| W10 | `init --yes` never prompts and never signs in: it creates the configuration if missing, writes the profile, applies `--recipe ID` when the providers already meet its needs, and prints what is still to do. |
| W11 | Input that ends early stops `init` with the steps already written intact and a message; nothing is half-written because each step is one atomic edit. |
| W12 | The mod is not installed by this slice. Shipping it inside the binary and loading it from `llmgate claude` is the next slice. |

The unit tests drive the menu and answer parsing; `tests/e2e/init_flow.sh` feeds a scripted terminal to the real binary against fake providers.

## The mod inside the binary (phase 1 slice 4)

| # | Behaviour |
|---|---|
| B1 | The binary contains the mod's runtime files (`.claude-plugin/plugin.json`, `hooks/hooks.json` and the hook sources), embedded at build time with `#embed`. A test fails when a runtime file under `mods/llmgate` is not bundled. Its tests and type files are not. |
| B2 | `llmgate claude` writes them to `$XDG_DATA_HOME/llmgate/mod/DIGEST` (default `~/.local/share/llmgate/mod/DIGEST`), where DIGEST is 16 hex digits over the file names and bytes. Only files that differ are written, with mode 0600 in 0700 directories. Nothing under `~/.claude` is touched. |
| B3 | It starts Claude Code with `--plugin-dir DIR` before the person's own arguments and exports `LLMGATE_BIN` and `LLMGATE_CONFIG` (absolute paths), which the mod needs to find the binary and the configuration. |
| B4 | With `claude.mod` set to `false` nothing is written, no flag is added and the two variables are not exported. |
| B5 | `LLMGATE_MOD_DIR` names a mod directory to load instead, for developing the mod; nothing is written then. |
| B6 | If the files cannot be written, `llmgate claude` warns and starts Claude Code without the mod. |
| B7 | Directories of older digests stay, because a running session may be reading one; each is about 70 KB. |
| B8 | `setup` and the VS Code settings do not load the mod: it is a launch argument of the terminal launcher. VS Code and the desktop app are checked later. |

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

## Built (phase 1)

| Slice | What exists |
|---|---|
| X1, X2, X4 | answers above; the `windows` workflow builds and tests the reflect and json modules on a Windows runner |
| 1 (trial) | `release.yml` builds four binaries (macOS arm64 and x86-64, Linux x86-64 and arm64), checks each for outside dependencies, runs the Linux ones on bare images, packages `llmgate-VERSION-OS-ARCH.tar.gz`, writes `SHA256SUMS.txt` and `llmgate.rb`, and attests provenance. A branch run ([run 40469846567](https://github.com/filipsajdak/llmgate/actions/runs/40469846567)) passed and kept the files as a trial artifact; `gh attestation verify` accepts an archive. Not yet done: a tag release, the tap repository and `brew install` on a clean Mac |
