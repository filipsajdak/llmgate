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

## Policy gate (read 2026-10-21)

Question: may a third-party gateway use a ChatGPT Plus sign-in to reach OpenAI models?

Not read: the help-center article "Using Codex with your ChatGPT plan" (help.openai.com, article 11369540),
which returned HTTP 403 to an automated fetch. Search snippets of it say that signing in to Codex uses the
plan's usage, that partner apps can share a plan's usage after the user enables subscription sharing, and
that a developer app can send Responses API requests with an OAuth token authorized for the user's plan.

Read: OpenAI's developer documentation, "Sign in with ChatGPT", section "Open-source token sharing"
(developers.openai.com/siwc/token-sharing-open-source and its subpages).

| Finding | Where |
|---|---|
| A documented route lets open-source and locally hosted apps use a user's ChatGPT plan; a paid or remotely hosted app completes an interest form instead | overview page |
| The flow needs no client secret and no partner key: the app opens the system browser to auth.openai.com with `client_id=dynamic_agent_client`, PKCE (S256), a loopback redirect on 127.0.0.1 and the scope `chatgpt.tokens.use.direct`; the callback returns a `client_id` bound to the user and workspace, saved for later sign-ins | sign-in page |
| Inference is `POST https://api.openai.com/v1/responses` with `Authorization: Bearer <access token>`, `store: false`, `stream: true`; the grant does not give access to the user's conversations | models and inference, overview |
| The preview omits some fields (`max_output_tokens`, `temperature`, `top_p`, `metadata`, `user`, `previous_response_id`, ...), system-role input items, image generation, file search, code interpreter, hosted MCP and `tool_search` | preview limitations |
| "A valid ID token alone does not authorize ChatGPT plan usage": a plan may lack access, and the docs have a section for "ChatGPT plan use isn't enabled" (not read) | sign-in page |
| A separate "Request a client ID" page says the feature is offered to a select group of commercial partners with a waitlist; it does not say how that relates to the open-source flow | request-client-id page |
| Reports that ad-hoc third-party Codex clients are not approved concern reusing the Codex CLI login, which this route does not do | AICoder report, GitHub discussion openai/codex#8338 |

Reading: this replaces the earlier plan to reuse the undocumented Codex backend. llmgate is open source
and runs locally, which is the documented category, so the route is official and not a gray area. Open
points: whether a Plus plan is enabled for it (found out at the first sign-in), the exact refresh request
(not documented on the pages read), and the conflicting client-ID page. The credential is a browser
sign-in on a loopback port, not a device-code login.

Effect on the plan: S5 (translation spec) must obey the preview limitations above; S8 builds the
documented sign-in and refresh, with `store: false` and `stream: true`. Decision for the user before S8:
proceed with the documented sign-in for their own Plus account (the recommendation), or build the
translator against an API key only.
