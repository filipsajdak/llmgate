# BET-5: control llmgate from inside Claude Code

Pending entry in the ScudoAI Planner (its create_bet needs a program and an owner id; the Planner was unreachable
when the other bets were written); delete this file once it is there.

| Field | Value |
|---|---|
| name | llmgate mod: see and change the route, models, usage and config without leaving Claude Code |
| need | Switch provider, account and model live, see which provider answers and what each provider has cost and has left, diagnose a refused model, add a provider and choose which models of it serve which role, all from inside the session, with no second terminal |
| riskiest_assumption | A mod can drive llmgate safely and fast enough for daily use on the owner's terminal, and its three hard pieces work as documented: a live pane that polls the gateway, a model-callable tool whose call is held until a button is pressed, and a status entry that follows the route |
| smallest_test | One real session in which the pane shows the live route, a button switches the session's sonnet slot to another provider and the next reply comes from it, the status entry follows, and a model-proposed switch is held until Apply is pressed |
| success_signal | The pane, the switch button and the held proposal work in the terminal for a week of real use; a refused proposal (Dismiss, Esc, timeout, no pane) changes nothing; usage per provider matches the ledger; no secret and no company host appears in the pane, the overview or the repo |
| test_start | 2026-10-22 |
| decision_date | 2026-11-20 |

The design, decisions, slices and risks are in [docs/llmgate-mod.md](../llmgate-mod.md). Slices carry `Bet: BET-5`.
