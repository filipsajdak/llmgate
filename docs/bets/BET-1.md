# BET-1: llmgate, a Claude Code gateway without the per-user kit

Pending entry in the ScudoAI Planner. The MCP server `planner` was unreachable from the agent that
prepared this text (`fetch failed` at http://127.0.0.1:3010), so nothing was created there. Paste the
fields below into `create_bet` (or `/bet`), then delete this file: bets live in the Planner, not in files.

| Field | Value |
|---|---|
| name | llmgate: a lighter, faster, protocol-correct Claude Code gateway |
| need | A lighter, faster, protocol-correct Claude Code gateway that replaces an earlier Go gateway and removes the per-user setup kit (profile directory, wrapper scripts, hooks, log scraping) |
| riskiest_assumption | The C++26 reflection and modules toolchain (GCC 16.2) is workable for a production gateway (already proven by a green build, sanitizers and fuzzing), and a gateway that owns the setup (`llmgate setup`, `llmgate claude`) removes the per-user kit without losing a feature Claude Code users rely on |
| smallest_test | A real Claude Code session through llmgate to the Copilot Claude models, using the work account |
| success signal | A full Claude Code turn with tool use through llmgate, with zero stream repairs that cannot be explained |
| test_start | 2026-10-17 |
| decision_date | 2026-11-06 |
| decide | persevere, pivot or stop, with the signals; an overdue bet blocks new slices under it |

Slices carry the trailer `Bet: BET-1`.

## Evidence so far (2026-10-19)

| Signal | Result |
|---|---|
| 1. A full Claude Code turn with tool use through llmgate to the Copilot Claude models, work account, no unexplained repair | Met in the real run: 5 assistant turns, Read x2, Edit, Bash, the invented sample project fixed, 3 streamed requests all 200, 0 upstream failures, circuit closed, connection reused. The only repair was a `data: [DONE]` terminator after each stream, dropped by design (now not counted). The run found and fixed two defects the fakes could not: Copilot rejects a request without `Editor-Version` (the gateway now sends the VS Code headers by default for a Copilot provider), and stream repairs could not be explained from the statistics (`repair_examples`) |
| 2. Perf bar A: one stream adds under 5 ms at p99, no leak over a soak run | Met on macOS and Linux: about 0.1 ms (2 KB) and 0.5 ms (100 KB) at p50; soak 72,692 requests on the host and 30,308 under LeakSanitizer with flat memory and descriptors back to idle (README, Performance) |

## More evidence (2026-10-21, Claude Code 2.1.304, `tests/compat`)

| Item | Result |
|---|---|
| Second model family | Met: one turn each on opus and haiku through the Copilot work account and through the Claude login; the transcripts show the requested family was served |
| Plan mode, thinking block | Met on both backends (plan mode answers with a plan and changes no file; a thinking block is returned and the session continues) |
| Long session | Met: a twelve-turn resumed session on both backends still knew a code word from turn 1 |
| Compaction | Met on both: a headless `/compact` emits the compact boundary and the session keeps its context |
| The public CI | Green on the Linux plain, ASan and TSan jobs, both architectures, macOS host and ASan, e2e, lint and every fuzz job (the nightly constexpr-deep job ran out of memory and now builds one compiler at a time) |
| Soak | 35,577 requests in six minutes (8 workers, hostile mix): memory ended at 8 MB, file descriptors bounded, exit 0, no sanitizer report |

Evidence for every signal is in. Decision on 2026-11-06.

