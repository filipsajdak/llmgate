# Providers from the pane: discovery, sign-in and switching

Status: design (BET-8). It builds on [docs/llmgate-guided-setup.md](llmgate-guided-setup.md) (wizard, recipes, agent
types, scorecard) and comes before contexts and the two-account orchestrator ([docs/llmgate-contexts.md](llmgate-contexts.md), BET-7).

## Need

A person adds a provider they have (a Copilot seat, a ChatGPT plan, a company gateway, a local model) from inside
Claude Code without knowing its protocol or model names, and chooses which model answers a task while they work.

## What the first real run showed

| Observation | Gap |
|---|---|
| A personal Copilot seat had every Claude model disabled by policy; its GPT models answered only on `/responses`, so the provider needed `protocol: openai_responses` set by hand | The protocol belongs to the model, and the provider's own list says which |
| The company gateway needed the path prefix `/anthropic` | Discovery and the check must find the prefix or say so |
| Sign-in ran outside the pane (`! llmgate login`) | The pane should run the device flow |
| Claude Code treats a non-Claude id as a 200k-token model with adaptive thinking | Limits must reach Claude Code and the translator must drop what the model lacks |
| Switching works by role only | A per-task choice and an automatic choice are missing |

## Decisions

| # | Decision |
|---|---|
| K1 | Order: onboarding, then switching (both BET-8), then contexts and the guard (BET-7 B), then the two-account orchestrator (BET-7 C). |
| K2 | One provider entry; each model's protocol comes from the discovered `supported_endpoints` (`/v1/messages` gives anthropic, only `/responses` gives openai_responses), with a per-model override in the config. |
| K3 | Sign-in from the pane: the pane starts the device flow, shows the link and the code with a copy button, waits, then runs `llmgate check --quick` for the provider. The token goes to a 0600 file and never passes through the pane. |
| K4 | Discovery, the user chooses: the pane lists each provider's discovered models with context window, policy state and a recommendation, and the user ticks what to offer. A refresh warns when an offered model disappears or is disabled. |
| K5 | Publish real limits: discovered limits go into `model_limits`, the profile's picker rows and whatever Claude Code reads for the context window (E2). The gateway drops or maps request features the target model lacks. `doctor` warns when an offered model's limits are unknown. |
| K6 | Switching, four ways: a pane Targets picker (this session's role, or the default); a prompt prefix `@target: task` that hands the task to a one-off subagent on that target; auto by task kind from signals only (no classifier call) with the reason shown per request; `/llmgate use` with completion and a short form. |
| K7 | Image generation: one announced experiment on the ChatGPT plan route; if it works, an `illustrator` agent type bound to ChatGPT that saves the PNG into the project. |
| K8 | Tracked as BET-8, decision 2026-11-20; BET-6 decides on 2026-11-27; BET-7 follows. |
| K9 | Acceptance: a scripted pane journey against fake providers, then a recorded live run on the owner's accounts. |

## Experiments before building (slice 0; each answered Yes or No with the Claude Code version)

| # | Question | Answer |
|---|---|---|
| E1 | Does the ChatGPT plan route list models with limits, and does the company gateway's `/anthropic/v1/models` carry limits? | |
| E2 | Does Claude Code take a context limit for a non-Claude id from a `modelPicker.options` field, from `/v1/models` discovery fields, or only from `CLAUDE_CODE_MAX_CONTEXT_TOKENS`? Does it then compact at the right point? | |
| E3 | Is a one-off `$.agent.register` mid-session visible to the next turn, or must prefix agents be registered at start? | |
| E4 | Which signals reach the gateway or the mod per request: plan permission mode, image blocks, prompt size, a read-only tool set? | |
| E5 | Does `image_generation` work on the ChatGPT plan route (one hand-built request, announced first)? | |

## Slices (each carries `Bet: BET-8`)

| Slice | Content | Gate |
|---|---|---|
| 0 | This spec, the bet text, experiments E1 to E5 | the table has no empty cell |
| 1 | Per-model protocol in `provider_config`; the translator is chosen per model; the Copilot preset fills it from the endpoints | unit, e2e with a fake serving both |
| 2 | `llmgate models offer` writes `offer`, `model_limits` and protocols; a refresh reports drift in the overview | unit, golden diff, e2e |
| 3 | Limits reach Claude Code; the translator drops what the model lacks; `doctor` on unknown limits | unit, e2e |
| 4 | `llmgate login --json-events`; the pane runs the device flow and shows link and code | unit, `claude plugin test` |
| 5 | The pane's model list with ticks, context, policy and recommendation; a drift banner | `claude plugin test` |
| 6 | Targets picker, `@target:` prefix, `/llmgate use` completion | `claude plugin test`, unit |
| 7 | `routes_by_signal` and the reason per request | unit, golden overview |
| 8 | `illustrator` agent (only if E5 is yes) | unit, live run |
| 9 | Journey test, live run with a GIF, the BET-8 decision | GIF, findings |
