# Translating Claude Code's Messages API to the OpenAI Responses API

Status: design (BET-3, slice S5). Nothing here is built; the README claims no OpenAI support until a real
Claude Code turn passes through it. The translator is one provider protocol, `openai_responses`; a second
one for chat-completions backends (local models, Grok) follows the same shape.

## What is translated

Claude Code sends `POST /anthropic/v1/messages` (Anthropic Messages, streamed). The upstream is
`POST https://api.openai.com/v1/responses` with the user's ChatGPT plan token (the documented open-source
sign-in, `docs/bets/BET-3.md`) or an API key. The gateway translates the request, then translates the
upstream event stream back into Anthropic events, and passes the result through the existing
`anthropic::stream_guard`, so the guard's grammar holds for translated streams too.

Every request is sent with `store: false` and `stream: true` (the plan-token route requires both). The
gateway keeps no conversation state: each request carries the whole history in `input`.

## Request mapping

| Messages field | Responses field | Rule |
|---|---|---|
| `model` | `model` | the provider's `models` map; the default role map is in "Models" below |
| `system` (string or blocks) | `instructions` | text blocks joined by a blank line; `cache_control` dropped |
| `messages[]` with a `system` role (mid-conversation system messages) | an `input` item `{type: message, role: developer}` | explicit `system` role items are rejected by the route |
| user text | `{type: message, role: user, content: [{type: input_text}]}` | |
| user image (`base64` or `url`) | `input_image` with a `data:` URL or the URL | |
| assistant text | `{type: message, role: assistant, content: [{type: output_text}]}` | |
| assistant `tool_use` | `{type: function_call, call_id, name, arguments}` | `arguments` is the JSON text of `input`; `call_id` is the `tool_use` id |
| user `tool_result` | `{type: function_call_output, call_id, output}` | text blocks joined; an image in a result is sent as an `input_image` item in the same turn (checked on the first real run); `is_error` becomes an `Error:` prefix |
| assistant `thinking` | a `reasoning` item restored from the signature | see "Thinking" |
| `tools[]` custom tools | `tools[]` `{type: function, name, description, parameters}` | `input_schema` becomes `parameters` unchanged |
| `tool_choice` `auto`, `any`, `none`, `{tool, name}` | `auto`, `required`, `none`, `{type: function, name}` | `disable_parallel_tool_use: true` gives `parallel_tool_calls: false` |
| `thinking` and `output_config.effort` | `reasoning: {effort, summary: auto}` | `low` and `medium` map to themselves, `high` and `xhigh` to `high`; `thinking.budget_tokens` below 4,000 gives `low`, below 16,000 `medium`, otherwise `high`; thinking off gives no `reasoning` object |
| `max_tokens` | omitted | `max_output_tokens` is not accepted on the plan route; the cap cannot be enforced and is counted in stats |
| `stream_options`, `temperature`, `top_p`, `top_k`, `stop_sequences`, `metadata`, `context_management`, `safeguards`, `container`, `service_tier` | omitted | none has an equivalent on the route; counted, never silent |
| request `anthropic-beta` header | dropped | tokens are listed in stats; none changes the translation |
| session id (`x-claude-code-session-id`) | `prompt_cache_key` | a hash of it, to keep the cache warm across turns (to be confirmed on the first real run; dropped if the route rejects it) |

Fields the plan route rejects (`background`, `conversation`, `max_output_tokens`, `max_tool_calls`,
`metadata`, `moderation`, `multi_agent`, `prompt`, `prompt_cache_retention`, `safety_identifier`,
`temperature`, `top_logprobs`, `top_p`, `truncation`, `user`, `previous_response_id` over HTTP) are never
emitted: a unit test lists them and fails if the translator can produce one.

## Capability policy

A server tool, an unsupported block or a missing capability is never dropped silently (decision D20). The
gateway either strips it up front where Claude Code does not depend on it, or answers with the wording
Claude Code's own recovery recognises.

| Case | Behaviour |
|---|---|
| tool types other than custom (`advisor_20260301`, `web_search_*`, `web_fetch_*`, `code_execution_*`, `computer_*`, `text_editor_*`, `bash_*`, `memory_*`) | HTTP 400, `invalid_request_error`, message `tools.N: Input tag 'TYPE' found using 'type' does not match any of the expected tags`; Claude Code drops the tool and retries (the advisor case is exercised by the compatibility suite) |
| `document` blocks, `search_result`, `container_upload`, `tool_reference` (tool search) | HTTP 400 naming the block type; `tool_reference` is the one Claude Code must not send, so the gateway lists deferred tools by name in the tool list instead |
| `redacted_thinking` | dropped from the history (it is Anthropic's own opaque text) |
| `cache_control` anywhere | dropped; the usage block reports what the upstream says it cached |
| `count_tokens` | answered by the existing local estimate; never sent upstream |
| unsupported Responses tool outcomes (image generation, file search, code interpreter, hosted MCP, `tool_search`) | never requested: they are not in the translated `tools` |

## Stream mapping (Responses events to Messages events)

The translator is a state machine over the Responses stream. It emits `message_start` first, then one
content block per output item, `message_delta` with the stop reason and usage, `message_stop`, and its own
`ping` every 10 seconds while the upstream is silent (a translating gateway must emit its own pings).

| Responses event | Messages events |
|---|---|
| `response.created` | `message_start` (id `msg_` plus the response id, `model` as the client asked, usage zero) |
| `response.output_item.added` message | `content_block_start` text |
| `response.output_text.delta` | `content_block_delta` `text_delta` |
| `response.output_item.done` message | `content_block_stop` |
| `response.output_item.added` function_call | `content_block_start` `tool_use` with `id` = `call_id`, `name`, empty `input` |
| `response.function_call_arguments.delta` | `content_block_delta` `input_json_delta` |
| `response.function_call_arguments.done` / `output_item.done` | `content_block_stop` |
| `response.output_item.added` reasoning | `content_block_start` `thinking` |
| `response.reasoning_summary_text.delta` | `content_block_delta` `thinking_delta` |
| `response.output_item.done` reasoning | `content_block_delta` `signature_delta` (the signature, below), then `content_block_stop` |
| `response.completed` | `message_delta` (stop reason, usage), `message_stop` |
| `response.incomplete` | `message_delta` with `max_tokens` (or `refusal` for a content filter), `message_stop` |
| `response.failed`, `error` | an Anthropic `error` event (see "Errors"), then close |
| any other event | ignored and counted by type, so a new event type is visible in stats |

Block indexes are assigned in order of arrival, one per output item, so parallel tool calls become several
`tool_use` blocks in one message (Claude Code sends 10 to 16 percent of its assistant messages this way).
Argument text for a tool call is forwarded as it arrives; the gateway never buffers a whole call.

Stop reason: any `function_call` item gives `tool_use`; otherwise `end_turn`; `incomplete` for length gives
`max_tokens`.

Usage: `input_tokens` minus `input_tokens_details.cached_tokens` becomes `input_tokens`, the cached count
becomes `cache_read_input_tokens`, `cache_creation_input_tokens` is 0, `output_tokens` is passed through
(it includes reasoning tokens, which are also reported in stats). The usage ledger needs no change.

## Thinking

Responses reasoning is an opaque item the upstream wants back on the next turn. With `store: false` the item
returns only through `include: ["reasoning.encrypted_content"]`. The translator requests it, shows the
reasoning summary as a `thinking` block, and puts the item (id and encrypted content) into the block's
`signature`, as the item's JSON text behind the prefix `llmgate-r1:`. On the next request a `thinking` block with a signature written by this gateway
is restored to a `reasoning` item; a block with no signature, or one this gateway did not write, is dropped
(Claude Code treats the signature as opaque, so nothing else needs to change). A history that switched
provider therefore never sends one provider's reasoning to another.

## Models

The provider's `models` map decides the upstream model; the defaults a ChatGPT-plan provider documents are
the opus and sonnet roles on GPT-5.6 Terra and the haiku role on GPT-5.6 Luna. They are suggestions: the
list of models the account can use comes from the upstream's model catalogue, which is not an entitlement
check, and only a completed turn confirms access.

## Credentials and configuration

```jsonc
"providers": {
  "codex": {
    "base_url": "https://api.openai.com/v1",
    "protocol": "openai_responses",
    "chatgpt": { "token_file": "~/.config/llmgate/chatgpt-token.json" },
    "models": { "claude-sonnet-5-5": "gpt-5.6-terra", "claude-haiku-5-5": "gpt-5.6-luna" }
  }
}
```

`protocol` defaults to `anthropic`. `chatgpt` selects the documented sign-in (`llmgate login --provider
codex`): the system browser opens `auth.openai.com` with `client_id=dynamic_agent_client`, PKCE (S256), the
scope `openid profile email offline_access resource.invoke chatgpt.tokens.use.direct` and a loopback
redirect on `127.0.0.1`; the gateway validates `state`, the ID token (issuer, audience, expiry, nonce,
signature) and the granted scope before it stores anything, writes the token file atomically with mode
0600, saves the `client_id` the callback returns, and refreshes the rotating refresh token before expiry.
A static `static_token_env` (an API key) replaces `chatgpt` for the API route. A provider with `chatgpt`
refuses to start on a non-loopback listen address and never forwards the client's own credentials.

## Errors

| Upstream | Client sees |
|---|---|
| 401 | one refresh and retry; then `authentication_error` telling the user to run `llmgate login --provider NAME` |
| 403 or "plan use is not enabled" | `permission_error` with the upstream's reason, not retried |
| 429 | `rate_limit_error`, with `retry-after` passed through |
| 400 | `invalid_request_error` with the upstream message |
| 5xx, `response.failed`, connection loss | `api_error`; counts toward the circuit breaker and triggers fallback as for any provider |
| unknown event or field | counted in stats, never fatal |

A refusal, a rate limit or an account warning from the upstream is reported as it arrives; the gateway never
retries around it with another credential.

## How it is tested

1. A pure translator module (`src/responses/`), constexpr where the language allows, with unit tests per row
   of the tables above and a `static_assert` that every Responses event type the guard knows is handled once
   or listed as ignored (as `anthropic::detail::rules_cover_events` does).
2. Properties: a translated stream, run through `anthropic::stream_guard`, satisfies `validate_grammar` for
   every sequence the exhaustive generator builds from the event alphabet; request translation never emits a
   field of the rejected list; translating a history and back keeps tool call ids and order.
3. A scripted `fake_responses.py` backend for the compatibility suite and end-to-end tests, with recorded
   event sequences as the fuzz corpus (`tests/fuzz/responses/`).
4. Real runs, opt-in, after the user's sign-in: the compatibility scenarios of `tests/compat` on the new
   column, with the first column cells expected to differ marked as expected-unsupported with the reason.
