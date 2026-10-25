# BET-8: any provider in two minutes, switch in one click

Pending entry in the ScudoAI Planner (its create_bet needs a program and an owner id); delete this file once it is there.

| Field | Value |
|---|---|
| name | llmgate mod: add any provider from inside Claude Code without knowing its protocol or model names, and pick which model does a task while working |
| need | Add a Copilot seat, a ChatGPT plan, a company gateway or a local model from the pane, and choose per task which one answers, without editing a config file |
| riskiest_assumption | A provider's own model list is reliable enough to set protocol and limits automatically, and Claude Code accepts a context limit for a non-Claude id from the gateway's side |
| smallest_test | In the pane, add the customer's Copilot GHE seat: sign in, tick two discovered models (one Claude, one GPT), and finish a tool turn on each with no manual config edit |
| success_signal | The scripted pane journey passes; the live run adds two providers from the pane in under two minutes each, runs one `@target:` turn and one auto-routed plan request, and the ledger rows match |
| test_start | 2026-10-25 |
| decision_date | 2026-11-20 |

The design, decisions and slices are in [docs/llmgate-providers.md](../llmgate-providers.md). Slices carry `Bet: BET-8`.
