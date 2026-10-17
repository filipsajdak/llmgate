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
