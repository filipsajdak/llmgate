# Set up providers and how their models cooperate, from inside Claude Code

Status: design (BET-5, mod; BET-4, gateway). Decisions are the owner's answers of 2026-10-22. It extends
[docs/llmgate-mod.md](llmgate-mod.md) decisions M8 (config edits) and M10 (Health and Config) and
[docs/provider-presets.md](provider-presets.md) slice L4.

## Need

The pane today shows and switches the route. It cannot add a provider, and it cannot say how models of different
providers work together: who answers each role, who takes over when one fails, which model plans and which
executes, which one advises and which serves subagents. All of that is edited by hand in the config file.

## Decisions

| # | Decision |
|---|---|
| S1 | **Cooperation covers four things:** role routes with fallback order; the model each role means on each provider (the `models` map); the plan-and-execute pair (`opusplan`); the advisor and the subagent and background models. |
| S2 | **Adding a provider is a guided form with a preview.** The pane asks for a preset (copilot, anthropic, chatgpt), a name and labels, then runs `llmgate provider add` as a dry run and shows the discovered models, the proposed role map, any config problem and the diff; Apply writes it. No field-by-field editor in v1. |
| S3 | **Sign-in is shown in the pane:** the verification link and code, then a wait and a result. Provider add never writes a token itself; it calls the provider's login. |
| S4 | **Apply reloads the running gateway.** The CLI writes the file, then asks the gateway to reload over the control channel (HMAC, as `use`); a config that fails validation leaves the old one running and says why. Session overrides and the ledger survive a reload. |
| S5 | **Edits are surgical and previewed.** `llmgate provider add\|refresh\|remove`, `llmgate routes set ROLE a,b`, `llmgate models set PROVIDER ROLE UPSTREAM`, `llmgate config set PATH VALUE`: each prints a unified diff, changes only the named keys, keeps comments, validates the whole config and writes only with `--yes`. The mod shows the diff and runs the same command with `--yes` on Apply. |
| S6 | **Plan and execute** are the `opus` and `sonnet` roles (what `opusplan` selects); the pane shows them as one pair and edits both rows. |
| S7 | **Subagents and background work** (compaction, classifier, titles) get their own route by request class once the gateway can tell them apart (Claude Code sends hint headers); until then the pane does not offer them. |
| S8 | **Advisor** follows the emulation of docs/provider-presets.md P9 and waits for its own spec; the pane gets a row for it when that exists. |

## What the gateway does not have yet (checked 2026-10-22)

| Missing | Needed by | Slice |
|---|---|---|
| Config reload while running | S4 | C1 |
| Any `llmgate provider` command | S2, S5 | C2 |
| Comment-keeping edit of the JSONC config for objects and arrays (only a number edit exists in `json/scan`) | S5 | C2 |
| Routing by request class (the `x-claude-code-*` hint headers are not read) | S7 | C5 |
| A way for the mod to show a login link while `llmgate login` is running (`$.process.run` returns at exit) | S3 | open question 1 |

## Slices (each carries `Bet: BET-5`, gateway slices `Bet: BET-4`)

| Slice | Content | Gate |
|---|---|---|
| C1 | `llmgate reload` and the control action `reload`: re-read, validate, swap providers, routes and catalogue, keep overrides and ledger, answer the problems | unit, e2e with a changed config |
| C2 | The edit engine and `provider add\|refresh\|remove`, `routes set`, `models set`, `config set` with diff and `--yes`; discovery and the cheap probe of P10 | unit, golden diffs, e2e against the fakes |
| C3 | Login handoff (answer to question 1) and the pane's Add provider flow: preset, name, labels, login, preview, Apply | `claude plugin test`, real run on a throwaway config |
| C4 | Pane Cooperation tab: per role the ordered providers and their upstream model, move up and down, add or drop a provider from a route, the plan and execute pair, preview and Apply | `claude plugin test` |
| C5 | Request-class routing and the subagent and background rows; advisor row after its spec | unit, compat scenario |

## Open questions

1. How the mod shows a login link: `llmgate login --json` printing the link and code as the first line and then continuing (needs a mod API that reads a running process's output; to be checked in the plugin types), or a two-step `login start` and `login finish`.
2. Whether `models set` should accept `ROLE=UPSTREAM` pairs for several providers in one call.
3. Whether a reload may change the listen address (no: it is refused with a message that asks for a restart).
