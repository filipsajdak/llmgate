# Provider presets: a working default per provider, discovery, limits and token counting

Status: design (BET-4, requirement R6 and its follow-ups). Built so far: slice L0 (`llmgate models`, the doctor drift check). Decisions are the user's
answers of 2026-10-21; evidence is marked as measured, documented or reported.

## Problem

Adding a provider means hand-writing the model map, the limits, the translation settings and the header
rules, and a wrong id fails late (Copilot answered `The requested model is not supported` for
`claude-sonnet-5.5` on 2026-10-21 while its old map still pointed there). The gateway should start every
provider from a default that works with Claude Code, let the user change any part of it, and learn the facts
that change (model ids, limits) from the provider instead of from a table that goes stale.

## Decisions

| # | Decision |
|---|---|
| P1 | **Built-in presets with overrides.** A provider says `preset: "copilot"` and states only what differs. A provider without a preset gets plain defaults. Presets are compiled in, typed by reflection like the rest of the config. |
| P2 | **Discover, then pin.** `llmgate provider add NAME` lists the provider's models, proposes a role map and limits from what it finds, and writes them into the config for the user to edit. A preset holds the rules for reading a provider's model list, not concrete ids. |
| P3 | **Over-limit requests: map upstream errors, estimate only to warn.** Each provider's own "context too long" error is translated to Anthropic's wording (`prompt is too long: N tokens > M maximum`), which Claude Code recovers from by compacting. The local estimate never rejects. |
| P4 | **Token counting: one local estimator everywhere; native counters are a later opt-in.** The estimator answers `count_tokens` and feeds a ledger warning. This round writes the survey below and builds the estimator only. |
| P5 | **First presets:** `copilot`, `anthropic`, `chatgpt`, and a placeholder `openai-chat` (vLLM on a Thor) that declares the shape and the discovery now and waits for its translator (BET-3, S7). |
| P6 | **Onboarding is a command with a probe** (`llmgate provider add`), which writes nothing without confirmation. |
| P7 | **Translation knobs exposed per provider, with preset defaults:** reasoning effort map, beta-token policy, unsupported-capability policy, output verbosity and tool-call style. |
| P8 | **Claude Code features are part of a preset's contract:** skills, plugins, hooks, mods, rules, the `/advisor` feature where it is possible, and `opusplan`. Each preset states which of them it supports, and the compatibility suite proves it. |
| P9 | **`/advisor` on a non-Anthropic provider is emulated through a function tool.** The gateway shows the model an ordinary `advisor` function; a call is answered by a configured model (any provider) and returned as the tool result. On Anthropic the native server tool stays. |
| P10 | **The probe is cheap by default:** list models, one one-token request per chosen model; `--deep` adds a tool call, a stream and a thinking turn. It shows what it will send before sending. |
| P11 | **Discovery refreshes on demand:** `llmgate provider refresh NAME` shows a diff; `doctor` warns when a pinned model is no longer listed (the Copilot case) and names the fix. |
| P12 | **Order:** Copilot discovery first (unblocks the real switching run), then limits, error mapping, the estimator. |

## What a preset holds

| Part | Content | Customised by |
|---|---|---|
| protocol | `anthropic` or `openai_responses` (later `openai_chat`) | provider |
| auth | which credential kinds apply (Copilot login, ChatGPT sign-in, pass-through, API key) | provider |
| headers | the fixed client identification a route requires | provider `headers` |
| discovery | the model-list path and the field names for id, context, prompt and output limits, visibility | not customised |
| role rule | how to propose opus, sonnet, haiku from a list (rank by family and size, skip hidden models) | the written `models` map |
| limits | context, prompt and output tokens per model, pinned from discovery, overridable | provider `limits` |
| translation | effort map, beta-token policy, unsupported-capability policy, verbosity, parallel tool calls, strict schemas | provider `translation` |
| features | a statement per Claude Code feature (below) | read only; proved by compat |

### Discovery sources (measured unless noted)

| Provider | Path | Limit fields |
|---|---|---|
| chatgpt | `GET /models` on the plan route (measured 2026-10-21: nine models, each with `context_window`, `max_context_window`, `visibility`, `supported_reasoning_levels`, `default_reasoning_level`, `input_modalities`) | `context_window` is the usable window, `max_context_window` the ceiling |
| copilot | `GET /models` (measured 2026-10-21: 53 entries) | `capabilities.limits.max_context_window_tokens`, `max_prompt_tokens`, `max_output_tokens` (the prompt limit is smaller than the window), `capabilities.tokenizer` (`o200k_base` on the Claude entries), `supported_endpoints`, and `policy.state` (`enabled` or `disabled`); a model with `policy.state: disabled` is refused with `The requested model is not supported` |
| anthropic | `GET /v1/models` (documented) | `max_input_tokens`, `max_tokens` |
| openai-chat | `GET /v1/models` on the vLLM server | `max_model_len` |

The published model list (`/v1/models` of llmgate) then carries these limits for Claude Code's picker.

## Over-limit handling (P3)

1. The pinned limits are advisory. `max_tokens` above the pinned output limit is clamped to it (BET-4 R6),
   counted in the statistics.
2. A provider's context-length error is recognised by a per-preset list of wordings and codes and returned as
   Anthropic's `invalid_request_error` with `prompt is too long: N tokens > M maximum`, taking N and M from
   the upstream message when it states them. This is the text Claude Code's recovery matches.
3. The estimator compares its figure with the pinned prompt limit and writes a ledger warning field when it
   is over; the request is still sent.

To prove with a real run before relying on it: that Claude Code compacts on the mapped error for each
provider. A crafted oversized request goes through the compatibility suite as a scenario (`oversize`).

## Token counting survey (P4)

Claude Code treats `count_tokens` as optional: when the route is missing it estimates locally (documented in
its gateway protocol page). llmgate already answers it with a local estimate and never sends it upstream.

| Counter | Request | Notes | Verdict |
|---|---|---|---|
| Anthropic `POST /v1/messages/count_tokens` | same body as a message | documented: counts system, tools, images and PDFs (base64 only); refuses most server tools, the MCP connector and URL or file sources; past thinking blocks are ignored; the figure may include system-added tokens that are not billed | usable as an opt-in for the `anthropic` preset; needs the client's login or a key |
| OpenAI `POST /v1/responses/input_tokens` | same body as a Responses request | documented: includes tools and formatting tokens; **measured 2026-10-21: the ChatGPT plan route answers 401 `hardened_oauth_rule_missing`** | not available on the plan route; possible with an API key |
| Bedrock `CountTokens` | InvokeModel or Converse shape | documented: no charge, matches billing, some models unsupported | later, with a Bedrock provider (not planned) |
| Gemini `models.countTokens` | `contents` | documented for text; tools and system instruction support not confirmed | later, with a Gemini provider |
| vLLM `POST /tokenize` (outside `/v1`) | `prompt` or `messages` | reported: applies the chat template, text only, returns count and `max_model_len` | opt-in for the `openai-chat` preset |
| Copilot | none known | no endpoint found | estimator |
| Local tokenizer (tiktoken family) | n/a | exact for OpenAI families, a large dependency for this build | rejected for now |
| Estimator | bytes of the serialised request, with a ratio per family | published ratios: about 4.0 characters per token for GPT, 3.5 for Claude, code and JSON denser than prose, 10 to 15 percent error on prose and more on code | the default; ratios start conservative and are calibrated from the usage the providers report |

Open: the estimator's ratios and margin are set from measured data. The ledger already records input tokens
per reply; a script compares them with the estimate of the same request to fix the ratio per preset.

## Claude Code features per preset (P8)

| Feature | anthropic | copilot | chatgpt | openai-chat |
|---|---|---|---|---|
| skills, plugins, hooks, mods, rules | client side; no gateway work | same | same | same |
| `opusplan` | role routing; plan and execution may be different providers (thinking blocks stripped across a switch, R5) | same | same | same |
| `/advisor` | native server tool | refused with the recognised wording; Claude Code drops it | same | same |
| adaptive thinking, effort | pass through | pass through (adaptive only, measured) | mapped to reasoning effort | mapped |
| images | pass through | pass through | mapped to `input_image` | model dependent |

Skills, plugins, hooks, rules and `opusplan` are covered by compatibility scenarios today (skill, hooks,
slash-command, mcp, plan-mode; the plugin, rule and mod scenarios are missing and are added). `/advisor` on a
non-Anthropic provider is emulated by the gateway (P9); its design is open question 1.

## Open questions

1. Advisor emulation (P9): which model answers by default and how it is configured (`advisor.model` as a
   target id), what history the advisor sees (the whole conversation, as the native tool does, or a
   summary), how its call is shown to Claude Code (the request never carries the server tool block, so the
   function must be injected in the request and its result consumed inside the gateway), and the cost bound
   (calls per turn). Needs its own spec before building.
2. The probe's request counts per mode, to be stated on screen before it sends anything.

## Slices

| Slice | Content | Gate |
|---|---|---|
| L0 | Copilot discovery: read `/models`, pin ids and limits, `doctor` drift line, fix the maintainer's map | unit, e2e against a fake, real run |
| L1 | `limits` per model in the config, clamp of `max_tokens`, limits in `/v1/models` | unit, e2e |
| L2 | context-length error mapping per preset, `oversize` compat scenario | unit, real run on copilot and chatgpt |
| L3 | preset type and the four presets, `translation` knobs | unit, config docs generated |
| L4 | `llmgate provider add` with discovery and probe | e2e against the fakes |
| L5 | local estimator, ledger warning, calibration script | unit, mutation |
