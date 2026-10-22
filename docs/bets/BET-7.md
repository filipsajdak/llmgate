# BET-7: contexts and orchestration across accounts

Pending entry in the ScudoAI Planner (its create_bet needs a program and an owner id); delete this file once it is there.

| Field | Value |
|---|---|
| name | llmgate: private, Employer and customer work stay apart, and two Claude subscriptions work together without llmgate holding either login |
| need | Use a private Claude and ChatGPT account, a Employer Claude Team account, and a customer's Copilot and Bedrock models from one place, keep customer data away from the other parties, and let work run on whichever account has room |
| riskiest_assumption | A headless `claude -p` worker per account, driven over stream-json and pointed at the gateway, can take a task, do plan then execute as two turns and report cost per account, while each login stays inside an unmodified Claude Code process |
| smallest_test | A main session in the Customer context cannot reach a private provider (refused with a named reason), and a delegated task runs on the other Claude account's worker and returns its result and cost with that account's label |
| success_signal | The guard refusal and the blocked handover pass in CI; the real run shows two accounts' rows in the ledger; a handover moves only text the user saw and confirmed; no secret or customer data appears in a pane, the overview or the repo |
| test_start | 2026-11-03 |
| decision_date | 2026-12-18 |

The design is in [docs/llmgate-contexts.md](../llmgate-contexts.md). Slices carry `Bet: BET-7`.
