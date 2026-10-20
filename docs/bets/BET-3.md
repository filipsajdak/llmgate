# BET-3: other protocols (Codex, local models, later Grok and Gemini)

Pending entry in the ScudoAI Planner; delete this file once it is there.

| Field | Value |
|---|---|
| name | llmgate: Claude Code on ChatGPT/Codex and local models |
| need | Run Claude Code on a ChatGPT subscription (Plus), on local models on Thor, and later on Grok and Gemini |
| riskiest_assumption | Claude Code's tool loop, subagents, hooks, plan mode and thinking survive a Messages-to-Responses or Messages-to-chat translation, and the ChatGPT-login route stays usable for a third-party gateway |
| smallest_test | One real tool turn through Codex |
| success signal | That turn passes with no unexplained repair and the matrix has a Codex column; local-model and Gemini columns follow |
| decision_date | 2026-12-04 |

## Policy gate (read 2026-10-21; secondary sources, no binding text found)

Question: may a third-party gateway use a ChatGPT Plus sign-in to reach Codex models?

| Finding | Source |
|---|---|
| OpenAI offers an official "Sign in with ChatGPT" route that spends a Plus or Pro allowance in partner apps (OpenCode, Warp, Amp and others), with a per-app spending cap; at launch it covers open-source projects, personal projects that run locally and selected private apps, while a paid or hosted app fills in an interest form first | news reports of the DevDay announcement |
| A Codex lead is reported to have said that Sign in With ChatGPT is not approved for ad-hoc third-party Codex clients: pure open-source clients and existing partners are supported, others must contact OpenAI first | one secondary report (AICoder) of a post on X; not checked against the post |
| A Codex maintainer wrote that forking is welcome, that the terms are "quite permissive", that open-source projects such as OpenCode do similar things, and that he is not a lawyer | GitHub discussion openai/codex#8338, Dec 2025 and Feb 2026 |
| Tools that reuse the Codex login work by presenting a Codex-specific system prompt, so they can break whenever OpenAI changes that check | community write-ups |

Reading: llmgate is open source (Apache-2.0) and runs locally for one person, which is the group the reports
call supported, but no binding OpenAI text says so and the position is reported inconsistently. The route is
fragile and carries an account risk the user must accept.

Decision needed from the user before the ChatGPT credential slice (S8) starts: (1) proceed with the sign-in
for the user's own Plus account, local and personal only, accepting that OpenAI may change or block it; or
(2) build the translator against an OpenAI API key only. The translator slices (S5 to S7) do not depend on
this and go first.
