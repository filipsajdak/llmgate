# BET-2: switching and compatibility

Pending entry in the ScudoAI Planner (the MCP server `planner` was unreachable on 2026-10-19). Paste
the fields into `create_bet`, then delete this file: bets live in the Planner, not in files.

| Field | Value |
|---|---|
| name | llmgate: switch provider, model and account live, and know what works where |
| need | Use Claude Code through whichever provider and account I choose, switch it live, always see which one answers, and know which Claude Code features work on each |
| riskiest_assumption | A gateway-side switch (per-session remap, role remap, per-target ids) is invisible to Claude Code's own features: caching, thinking signatures, subagents, plan mode and compaction survive a switch with at most the documented recoveries |
| smallest_test | In one real Claude Code session, switch from Copilot to an Anthropic account with `llmgate use` and finish the turn |
| success signal | The scripted switching scenario passes in CI; real Copilot and Anthropic runs fill the committed matrix with explained results; the status line names the provider of the last reply |
| test_start | 2026-10-19 |
| decision_date | 2026-11-13 |

Slices carry the trailer `Bet: BET-2`.

## Evidence so far (2026-10-20)

| Signal | Result |
|---|---|
| Smallest test: a real session switched from Copilot to Anthropic mid-conversation | Met: `switch-mid-session` passed (first reply from Copilot, `llmgate use sonnet ... --session ID`, resumed session answered from Anthropic) |
| Matrix | 11 feature scenarios pass on the Claude login and on a Copilot account (Claude Code 2.1.304): text, tool loop, file write, plan mode, subagent, thinking, skill, slash command, hook, MCP, image |
| Switching, status line | Unit and end-to-end tests: picked target, role and session remaps, signed control endpoint, saved remaps, status line `via PROVIDER/MODEL`, `FALLBACK`, `switched` |
| Cost at real request sizes | About 3 ms per MB of request body for one stream (README, Performance) |

Also met (2026-10-21): a real fallback run (dead first provider, Claude login second: the session view names
the second provider and marks the reply as a fallback); a twelve-turn session and a compaction on both real
backends; opus and haiku turns; the soak (35,577 requests, no leak).

Not yet covered: advisor, prompt-cache reads across a switch, long context at 1M, two Anthropic profiles side
by side, and a real Claude Code run on a Codex or local-model backend (BET-3). Decision on 2026-11-13.
