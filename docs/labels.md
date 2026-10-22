# Labels: tell your providers and models apart

Status: design (BET-5, mod; BET-4, config). Decisions are the owner's answers of 2026-10-22.

## Problem

Several providers and models of one family answer the same role (a work Copilot and a private one, a model kept
for one customer). The pane, `/model` and the usage table name them by provider key and model id only, so the
person has to remember which is which, cannot see what a customer's work cost, and can switch a company session
onto a private account by mistake.

## Decisions

| # | Decision |
|---|---|
| B1 | **Labels are free tags:** a list of short strings (`"work"`, `"acme"`). A label is 1 to 24 characters of letters, digits, `-` and `_`; at most 8 per target; anything else is a config problem named by `doctor`. |
| B2 | **Set on a provider and on a model, merged.** `providers.NAME.labels` applies to every model of the provider; `providers.NAME.model_labels` maps an upstream model name to more labels. A target's labels are the provider's then the model's, without duplicates. |
| B3 | **Shown** in the pane (Targets and Usage tables get a `labels` column), in the `/v1/models` display name read by Claude Code's `/model` picker (`Sonnet 5.5 - Copilot [work, acme]`), in the status entry, and in `llmgate targets`, `llmgate models` and `llmgate route`. The id of a target never changes, so remaps and sessions keep working. |
| B4 | **Usage and cost are grouped by label.** The ledger records the labels in force for each request (as they were then, so a later edit does not rewrite history); `GET /llmgate/overview` gains `spend.by_label`; the Usage view and `/llmgate usage` gain a table per label. A request with several labels counts once under each, so label totals can exceed the total and the table says so. |
| B5 | **Filter by label.** `llmgate targets --label work`, `/llmgate targets work`, a label filter in the pane's Targets view, and `/llmgate use ROLE label:work` when exactly one target carries it. |
| B6 | **A guard warns on crossing.** `label_guard` is a list of groups of labels that must not be mixed in one session (`[["work", "private"]]`). A switch that moves a session from a target with one label of a group to a target with another label of the same group is refused by the CLI without `--yes`, and the pane shows the warning and a second button. A global (`make default`) switch is checked against the previous default the same way. The guard never blocks a fallback the config itself lists. |

## Open questions

1. Whether the guard should also cover a fallback chain that mixes the groups (a `doctor` warning is the cheap form).
2. Whether the ledger's label record needs a new field in the usage file's schema version or can ride in the existing provider column.

## Slices (each carries `Bet: BET-5`, config slices `Bet: BET-4`)

| Slice | Content | Gate |
|---|---|---|
| B-1 | `labels` and `model_labels` in the config with validation, `doctor` finding, generated config docs, labels on catalogue targets, `/v1/models` display name, `llmgate targets/models/route` | unit, e2e |
| B-2 | Ledger records labels; `spend.by_label` in the overview; `describe` tables | unit, e2e |
| B-3 | Mod: labels column, Targets filter, `/llmgate targets LABEL`, `use ROLE label:X`, status entry | `claude plugin test` |
| B-4 | `label_guard`: the CLI check with `--yes`, the pane warning and second button | unit, e2e, `claude plugin test` |
