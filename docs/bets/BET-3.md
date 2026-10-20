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

## Policy gate (before the ChatGPT credential is built)

Read OpenAI's current text on third-party tools and the ChatGPT plan, and record the result and the date
here. If a gateway may not use a ChatGPT sign-in, stop that slice and keep the translator for an API key.
The account to use is a ChatGPT Plus subscription (device-code login must be enabled in its security
settings).
