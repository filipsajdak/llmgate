# BET-6: a guided multi-provider setup inside Claude Code

Pending entry in the ScudoAI Planner (its create_bet needs a program and an owner id); delete this file once it is there.

| Field | Value |
|---|---|
| name | llmgate mod: a person who knows Claude Code but not llmgate reaches a good multi-provider setup from inside the session |
| need | Sign in to ChatGPT, Claude and Copilot, choose which model does which job (main work, planning, execution, review, background, images), bind subagents to models of other providers, and see whether the setup was worth it, without editing a config file |
| riskiest_assumption | Claude Code lets a named agent run on a model of another provider (a full catalogue id in an agent definition, `$.agent.register` or an `agent.spawn` hook reaches the gateway verbatim and is accepted), and a setup judged from the usage ledger tells a user something true |
| smallest_test | In one real session on a throwaway profile with the owner's three logins: the wizard signs in, a recipe is applied, a subagent whose model is `claude-gpt-6-sol@codex` produces a ledger row for provider `codex` with class `subagent`, and compaction is routed by class |
| success_signal | The scripted journey passes in the mod test kit; the real run shows the subagent and compaction rows; the scorecard names cost, time and fallbacks per provider; no secret, account id or company host appears in the pane, the overview or the repo |
| test_start | 2026-10-23 |
| decision_date | 2026-11-27 |

The design, decisions and slices are in [docs/llmgate-guided-setup.md](../llmgate-guided-setup.md). Slices carry `Bet: BET-6`.
