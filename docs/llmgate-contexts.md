# Contexts and orchestration across accounts

Status: design (BET-7), written 2026-10-22 from the owner's answers. Provider kinds and auth are slices A1 to A4 of
[docs/llmgate-guided-setup.md](llmgate-guided-setup.md).

## The owner's accounts

| Party | Accounts | Route |
|---|---|---|
| Private | Claude subscription, ChatGPT/Codex plan | Claude Code login forwarded; `chatgpt` preset |
| Employer | Claude Team subscription | Claude Code login forwarded |
| Customer | Copilot on the customer's GHE host; Bedrock Claude and GPT behind the customer's gateway "partner" | `copilot` preset with a GHE host; partner through the owner's local gateway for now |

## What Anthropic's terms allow (read 2026-10-22)

OAuth subscription login is for the person signed in to the unmodified Claude Code. Developers may not "collect,
store, or intermediate Claude.ai credentials or session tokens", and the advertised limits "assume ordinary,
individual usage". llmgate therefore never holds a Claude login and never picks between two. Each Claude account is
a Claude Code profile and process that the owner signed in; the orchestrator below works between processes.

## Decisions

| # | Decision |
|---|---|
| H1 | Two Claude accounts are combined by an orchestrator: one Claude Code process per account (own `CLAUDE_CONFIG_DIR` and login, base URL on the gateway, an account label header), exchanging work with them. |
| H2 | Transport order: headless `claude -p` workers over stream-json; cross-session messaging as an experiment and optional front end; Channels last. One orchestrator interface sits over all three. |
| H3 | Plan and execute are two turns on a worker (`opusplan` is not available headless). |
| H4 | A context is a named bundle: a Claude profile (or none), the providers it may use, a label, a default recipe, a guard group. Private, Employer and Customer first. |
| H5 | The launcher sets the start context (`llmgate claude --context NAME`, a menu without one); the pane can move to another context and warns on leaving; a Claude-account context needs a new session. |
| H6 | Data flow between contexts is blocked by default. A handover needs the user's explicit request, anonymised text from the source session, the exact text shown and a confirmation; enforced in llmgate. |
| H7 | The pane's Sessions view lists each worker (account, state, task, tokens, quota) with buttons to open its transcript or resume it in its profile. |
| H8 | Partner through the local gateway now (as an Anthropic-protocol provider with its aliases); native Bedrock Converse and chat-completions providers and an import of its `config.yaml` later. |
| H9 | Auth kinds: Claude login (profile), device or PKCE sign-in, API key (any header, from env, a 0600 file or the Keychain), none, forward. The user picks the store; the mod recommends the most secure; it never shows or stores a value in config. |
| H10 | Order: provider kinds and auth, then contexts and the guard, then the orchestrator. BET-6 stays (2026-11-27); this bet decides 2026-12-18. |

## Experiments before building (slice X0; each answered Yes or No with the Claude Code version)

| # | Question | Finding |
|---|---|---|
| 1 | Does `ANTHROPIC_CUSTOM_HEADERS` reach the gateway, so a worker can carry an account label? | **Yes** (Claude Code 2.1.308, scripted upstream, throwaway profile). `ANTHROPIC_CUSTOM_HEADERS='x-llmgate-account: employer'` reached the gateway on every request, subagent requests included |
| 2 | Does a `claude -p --input-format stream-json --output-format stream-json` worker in a second profile, with `claude.credential: login`, forward its login through the gateway and answer two tasks in one process? | **Yes** for the transport: one `claude -p --input-format stream-json --output-format stream-json --verbose` process in its own profile answered two tasks and reported a `result` event for each, through the gateway. Forwarding a real login is checked in the real run |
| 3 | Plan then execute: a first turn with `--permission-mode plan` on the planning model, then a second turn on the executor in the same process or with `--resume`? | **Yes.** A worker started with `--permission-mode plan --model opus` did the planning turn on opus; control requests `set_model` (`sonnet`) and `set_permission_mode` (`default`) were accepted between turns, and the execution turn reached the gateway as sonnet. No `--resume` is needed |
| 4 | Do rate-limit headers captured per worker identify the account (by label)? | |
| 5 | Cross-session messaging between two interactive sessions in different profiles: reachable, `crossSessionInbound` behaviour, hold dialogs | |
| 6 | Can a mod in a session read `CLAUDE_CODE_MESSAGING_SOCKET` and `CLAUDE_CODE_MESSAGING_TOKEN` and report them to llmgate? | |

## Slices (each carries `Bet: BET-7`)

| Slice | Content | Gate |
|---|---|---|
| X0 | The six experiments | findings filled |
| B1 | `contexts` in the config, `llmgate claude --context`, guard enforcement in the gateway, ledger and overview by context | unit, e2e |
| B2 | Mod context picker with a warning on leaving; status entry names the context | `claude plugin test` |
| B3 | Handover API: anonymised text plus an approval token minted by the pane after confirmation; default deny; audit line | unit, `claude plugin test` |
| C1 | Orchestrator daemon: worker registry per context, stream-json protocol, plan then execute, `delegate(context, task)` | unit with a scripted worker, e2e |
| C2 | Sessions view, transcript and resume buttons, per-account quota keyed by the account label | `claude plugin test` |
| C3 | Cross-session messaging front end if X0 allows; a Channels prototype | experiments |

## Open questions

1. Which sign-in works for the customer GHE Copilot account on this machine.
2. A Linux secret store (the first cut is macOS Keychain, a 0600 file and env).
3. How anonymisation is checked: the source session's model rewrites the text; the user sees the result and decides.
