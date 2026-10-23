# Guided setup: several providers, one person who does not want to edit a config

Status: design (BET-6). Contexts, the guard, handover and the orchestrator across Claude accounts are BET-7 ([docs/llmgate-contexts.md](llmgate-contexts.md)); provider kinds and auth are the slices A1 to A4 below. Decisions are the owner's answers of 2026-10-22; facts are marked documented, reported or
measured. It builds on [docs/llmgate-mod.md](llmgate-mod.md) (the mod), [docs/llmgate-setup.md](llmgate-setup.md)
(edit commands and reload, whose slices C3 to C5 move here) and [docs/labels.md](labels.md).

## Need

A person who knows Claude Code, not llmgate, has a Claude login, a ChatGPT plan and a Copilot seat. They should
reach a setup that uses each for what it is good at, understand what they chose, bind subagents to models of other
providers, and learn from real use whether it was worth it, from inside the session.

## Decisions

| # | Decision |
|---|---|
| G1 | New bet BET-6, decision 2026-11-27. BET-5 stays the control mod (2026-11-20). Slice 0 is live experiments. |
| G2 | Audience: knows Claude Code, not llmgate. Role, fallback and subagent are explained in plain words; one recommended choice per screen. |
| G3 | A first-run wizard (which providers do you have, sign in, test), then recipes with pros and cons. The same screens later act as the editor. |
| G4 | Binding layers, all supported: roles through the pinned `ANTHROPIC_DEFAULT_*_MODEL` and the gateway role map; mod-made agent types bound to full catalogue ids (`$.agent.register`); spawn rules in the mod (`agent.spawn`, keyed on `subagentType`); `CLAUDE_CODE_SUBAGENT_MODEL` (never `_FORCE`) as the default. `turn.step` only for fallback. |
| G5 | The launcher sets `CLAUDE_CODE_GATEWAY_HINT_HEADERS=1`. The gateway reads request class and agent type, records them in the ledger, routes compaction, titles and classifier requests to a cheap provider by default (configurable) and counts subagent usage separately. |
| G6 | Six recipes, each with pros, cons, what must be signed in and a token multiplier (below). |
| G7 | The council is delivered as mod-registered agent types dispatched through Claude Code's own Agent tool, first; a `/llmgate panel` fan-out of headless children comes later. Advanced only, capped at N agents and a token budget, scored in the scorecard. |
| G8 | Evaluation: a live scorecard from the ledger and a quick check at setup time. The owner's benchmark harness (frozen task suite, paired statistics) is a later repo tool. |
| G9 | Meters in v1: Anthropic from headers (built), Copilot credits, the ChatGPT window from the route's headers if it sends them, and the ledger's own token count where nothing is reported. |
| G10 | Strengths: research whether `image_generation` works on the ChatGPT-plan route; only then a tool or agent type bound to it. A recipe claims a strength only when it is proven. |
| G11 | Acceptance: a scripted journey in the mod test kit against a fake gateway, then a real run in a throwaway profile with the owner's three logins, recorded as a GIF. |
| G12 | This document comes before slice 1. |

## What Claude Code does with a model choice (documented; 2.1.306 types and the sub-agents and model-config pages)

| Rank on the wire | Mechanism | Notes |
|---|---|---|
| 1 | `turn.step` hook rewrite | per request; after spawn-time resolution; the order against `_FORCE` is an inference, tested in slice 0 |
| 2 | `CLAUDE_CODE_SUBAGENT_MODEL_FORCE=1` with `CLAUDE_CODE_SUBAGENT_MODEL` | overrides everything below, a hook included; the mod never sets it and `doctor` warns when it is set |
| 3 | `agent.spawn` hook sets `model` | outranks the Agent tool's parameter; not re-run on a SendMessage resume |
| 4 | Agent tool `model` parameter | Claude's choice; takes only `sonnet`, `opus`, `haiku`, `fable` |
| 5 | agent definition `model` (user, project, plugin file, `--agents`, `$.agent.register`) | an alias, a full id or `inherit`; a user agent named `Explore` replaces the built-in |
| 6 | `CLAUDE_CODE_SUBAGENT_MODEL` without force | a default only |
| 7 | the main conversation's model | forks always inherit |

An alias in rank 4 or 5 resolves to the main model when the main model is in that family, so a provider-specific
choice always uses a full id. Skills and commands with `model:` change the model for the rest of the turn.
`ANTHROPIC_DEFAULT_{OPUS,SONNET,HAIKU,FABLE}_MODEL` fix what each alias means at start-up; Haiku also covers
background work, so it is pinned to a cheap role.

What the gateway can tell apart: `x-claude-code-session-id` always; `x-claude-code-agent-id` (random per spawn);
with `CLAUDE_CODE_GATEWAY_HINT_HEADERS=1` also the request class (main, subagent, workflow, compaction, auxiliary)
and the built-in agent type (`custom` for the rest). A user-chosen agent name is never sent, so routing by a named
agent travels in `model`.

## Slice 0: findings (to fill; each line is Yes or No with the Claude Code version and the evidence)

| # | Question | Finding |
|---|---|---|
| 1 | Does `claude-gpt-6-sol@codex` reach the gateway verbatim from an agent file, from `--agents` and from an `agent.spawn` hook? | **Yes** (Claude Code 2.1.308, scripted upstream, throwaway profile). A project agent file with `model: claude-gpt-6-sol@fake` reaches the gateway as `claude-gpt-6-sol@fake`; the gateway resolves it to the catalogue target and the provider sees `gpt-6-sol`. Claude Code logs `unrecognized_model` for it and sends it anyway. `--agents` and an `agent.spawn` hook are still to test |
| 2 | Is a non-`claude` id accepted in an agent definition? | **Yes.** `model: gpt-6-sol` in an agent file is accepted and sent verbatim; the gateway treats it as role `other` |
| 3 | `_FORCE` against a hook model, for a plain subagent and a workflow agent | **Yes, `_FORCE` wins** over the agent file's model (`CLAUDE_CODE_SUBAGENT_MODEL=claude-gpt-6-luna@fake` with `_FORCE=1` put the agent on luna). Without `_FORCE` the agent file's model wins over the env default, as documented. **An `agent.spawn` hook beats the agent file** (hook model luna over frontmatter haiku) **and `_FORCE` beats the hook** (sol), as documented |
| 4 | `turn.step` rewrite with `_FORCE` on | **Yes, `turn.step` wins over `_FORCE`**: the rewrite (luna) reached the gateway while `_FORCE` named sol. The hook is an `async function*` and runs for subagent steps (`e.agentId` set). Rank 1 holds |
| 5 | Do `/compact`, titles and the classifier fire `turn.step` and `agent.spawn`? | |
| 6 | `ANTHROPIC_DEFAULT_SONNET_MODEL` set to a catalogue id, main model another sonnet id, subagent `model: sonnet` | |
| 7 | Does a hook-set model persist when a subagent is resumed with SendMessage? | |
| 8 | Does a skill's `model:` revert on the next prompt? | |
| 9 | Does a user agent named `Explore` with a catalogue id replace the built-in? | **Yes.** A project agent named `Explore` with `model: claude-gpt-6-luna@fake` replaced the built-in model choice for an Explore spawn |
| 10 | Do split-pane teammates inherit the base URL and fire `agent.spawn`? | |
| 11 | The values of `x-claude-code-request-class` and `-agent-type` for each kind of request | With `CLAUDE_CODE_GATEWAY_HINT_HEADERS=1`: `x-claude-code-request-class` is `main` or `subagent`, `x-claude-code-agent-type` is `custom` for a project agent, `x-claude-code-agent-id` is present on subagent requests only, `x-claude-code-prompt-id` on all. Without the variable the class and type headers are absent and `x-claude-code-agent-id` is still sent. A subagent request has `max_tokens` 32000 against 128000 for main |
| 12 | The request body for a catalogue id: which fields must the translation drop (adaptive thinking, effort, `context_management`, system-role messages)? | A subagent request to a catalogue id carries `thinking: adaptive`, `output_config.effort`, `context_management`, `max_tokens` 128000 and 8 tools; the Responses translation already handles these for Codex (compat run), the Messages upstreams receive them unchanged |

Slice 0 status (Claude Code 2.1.308, scripted upstream, throwaway profile, host build): rows 1, 2, 3, 4, 9, 11 and
12 are answered; 5 (compaction and titles), 6, 7 (SendMessage resume), 8 (skill model revert) and 10 (teammates)
need a longer session or the real-user run of slice 7 and stay open. The experiment scripts live outside the repo
and are small enough to rewrite: a scripted Anthropic upstream that answers an `EXP:spawn:NAME` prompt with an
`Agent` tool call, project agents with different `model:` lines, and a throwaway mod with `agent.spawn` and
`turn.step` hooks.

## Recipes

Providers used below: Claude (the Claude Code login, forwarded), ChatGPT (the plan, Codex models), Copilot (a seat,
work or private). Pros and cons are the text the pane shows; evidence is marked and every task-fit claim stays
"reported" until the scorecard confirms it.

| Recipe | Roles and fallback | Pros | Cons | Needs | Tokens |
|---|---|---|---|---|---|
| Quality first with fallback | every role on Claude; Copilot, then ChatGPT only when Claude is limited or down | one vendor's behaviour, best-known results with Claude Code | spends Claude quota fastest; the fallback answers differ in style | Claude, and one more for the fallback | 1x |
| Spend subscriptions evenly | opus on Claude, sonnet on Copilot, haiku, subagents and background on ChatGPT | each quota lasts longer; the cheap tier carries the bulk | quality varies by role; the cache is cold at each switch; three sign-ins | all three | 1x |
| Second opinion | main roles on Claude; a `reviewer` agent type on ChatGPT or Copilot | a different vendor catches different mistakes (reported: errors correlate within a family) | the review costs extra tokens and time | Claude and one other | about 1.3x |
| Work and private apart | work labels only on the company Copilot, private labels only on the person's own accounts; the guard blocks crossing | company data never leaves the company route | fewer fallbacks; needs labels | a work and a private provider | 1x |
| Best model per job | planner on the strongest model, a cheaper executor, haiku and background on the cheap tier, an `illustrator` agent on ChatGPT where image generation is proven, long reads on the longest window | each job gets the model that fits it; planner with cheaper executor has the clearest published gains | most moving parts; claims are reported until measured | all three | 1x to 1.5x |
| Advanced: hard problems | `council-<provider>` agent types, one per provider, dispatched in parallel, and a `chair` that merges | many views on one hard problem | evidence is mixed (debate often only matches majority voting at 2 to 3 times the tokens; a weak member can lower the result); expensive | all three | 3x to 5x, capped |

Defaults come from what is signed in: with one provider only Quality first without fallback is offered; with two,
Second opinion is the recommendation; with three, Best model per job. The Advanced recipe is never the default.

## Evaluation

The live scorecard shows per provider, per request class (main, subagent, compaction) and per agent type: requests,
tokens, cost (from the pricing table), time, fallbacks, errors and the share of work. The quick check at setup time
runs a fixed smoke set through every provider of the recipe (a tool turn, a subagent, a long prompt, an image where
bound) and shows pass or fail and time per provider. Neither judges answer quality. The owner's benchmark harness
comes later: a frozen set of 10 to 20 private tasks with hidden checks, three seeds per task, pass@1 and pass@3,
cost and time per solved task, paired bootstrap, a judge from a provider absent from the pair for open tasks.

## Slices (each carries `Bet: BET-6`)

| Slice | Content | Gate |
|---|---|---|
| 0 | The twelve experiments above, run through a recording gateway with a throwaway mod; findings in the table | the table has no empty cell |
| 1 | Hint headers and class routing: launcher env, the gateway reads class, agent type and agent id, ledger fields `class` and `agent_type`, `routes_by_class`, usage by session and by class in the overview | unit and golden overview |
| 2 | Recipes as data and `llmgate recipe list\|show\|apply NAME --yes` on the edit engine and reload; `doctor` checks sign-in per recipe | unit, golden diffs, e2e |
| 3 | The wizard and plain-words help in the pane, sign-in link and code, test, recipe cards with preview and Apply | `claude plugin test`, scripted journey |
| 4 | Recipe agent types and spawn rules, the "who runs what" view, `doctor` on `_FORCE` (built: `agents` and `spawn_rules` in the config with `llmgate agents` and `rules`, the second-opinion recipe's reviewer, the mod's registration, spawn hook and Agents view, the doctor warning; the real-session check that the model reaches the gateway is open) | `claude plugin test`, real session |
| 5 | Scorecard, meters and the quick check (built: the ledger's per-provider, class, agent and label aggregate, the overview's scorecard, `quota --provider`, the Scorecard tab and its meters, `llmgate check` and the tab's Quick and Full check; the check proves each provider and each agent's target through the gateway, the real subagent turn needs a live session) | unit, e2e, `claude plugin test` |
| 6 | Image tool if proven, the council and its caps (council built: the `hard-problems` recipe installs read-only `council-<vendor>` agents capped at 8 turns, the mod rewrites a `council:` prompt so the session is the chair and dispatches them in parallel, one round, at most three; a subagent cannot spawn subagents, so the chair is the main session, not an agent. The image tool waits for the real run: reports say the Responses `image_generation` tool works on the plan route, undocumented) | live run |
| 7 | The real-user run (G11) and the decision | GIF, findings |
| A1 | Auth kinds in `provider_config` (`bearer`, `header` with a name and scheme, `none`, `forward`) and secret stores (env, a 0600 file, macOS Keychain); `doctor` checks them without printing a value | unit, e2e with a fake that requires `api-key` |
| A2 | `llmgate provider add` learns the kinds, the GHE host of Copilot, the local gateway preset, a local no-auth model, and a `secret set NAME` command | unit, golden diffs, e2e |
| A3 | The wizard for provider kinds: guided form, secret prompt that recommends the most secure store, sign-in link and code through `$.process.spawn`, test | `claude plugin test`, scripted journey |
| A4 | `llmgate provider import mic`: the local-gateway `config.yaml` as a reference for the mic provider's model list | unit |

## Finding: the pane's Input has no mask

`InputProps` of the 2.1.306 types has no password or mask property, so a secret typed into a pane field would be drawn
in clear. The wizard therefore never asks for a key in the pane: it writes the provider with `--secret
file:~/.config/llmgate/secrets/NAME.key` and tells the person to type `! llmgate secret set NAME`, which reads the
value hidden in the terminal and stores it at mode 0600.

## Finding: no quota headers on the ChatGPT plan route

The capture of the owner's Codex gateway (2026-10-23) shows reply headers `x-codex-turn-state` and `x-codex-safety-*` only,
and no rate-limit or window header, so the ChatGPT meter is the ledger's own token count and the pane says so.

## Open questions

1. Which endpoint, if any, reports Copilot credit use for the stored token (G9).
2. Whether the ChatGPT route sends window headers the gateway already captures as `rate_limits`.
3. How the pane shows a sign-in link while `llmgate login` runs (`$.process.run` returns at exit; `$.process.spawn` is the candidate).
4. The cheap default for compaction and titles per preset.
