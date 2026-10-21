# Robustness for mixed providers: requirements from another gateway's field report

Source: a report from another gateway implementation (2026-10-21), seen on a Bedrock Opus 4.6 route. The
setup there was `opusplan` with two providers: Opus 5.5 on Copilot for planning and Opus 4.6 on Bedrock
for execution (the sonnet slot). The report may not hold for the Anthropic or Copilot models llmgate
serves today, so every item below is either a fix that is right whatever the model, a rule for translating
backends, or something to measure on our own providers before building.

In that setup every plan-to-execute change is a provider switch. Thinking blocks signed by one provider
reach the other, and the prompt cache starts cold each time.

## Requirements and decisions

| ID | Requirement | State | Decision (2026-10-21) |
|---|---|---|---|
| R1 | Never send Claude Code a tool call whose arguments are cut off; an empty result is an Anthropic error that names the output limit | the Responses translator closed a half-finished call as a valid block (bug) | fix now |
| R2 | A request-id header and field on every error the gateway makes | upstream ids pass through; gateway-made errors have none | fix now |
| R3 | A 200 reply that is not a Message becomes a 502 Anthropic error, never relayed | relayed as is | fix now |
| R4 | A stalled stream ends with an explicit error fast enough for the client to retry or fall back | idle timeout 10 minutes, error event synthesized | default lowered to 120 seconds, still configurable |
| R5 | Thinking blocks do not cross a provider switch | left to Claude Code's recovery, untested with thinking | strip when the provider changed (see below) |
| R6 | Per-model limits: clamp `max_tokens`, a clear 4xx for an oversized prompt, limits published in `/v1/models` | none | config limits, clamp, advertise (BET-4) |
| R7 | Ledger carries stop reason, latency, error text and request class | status, tokens, repairs, session, provider, model | add the four fields (BET-4) |
| R8 | An opt-in capture of request and response bodies for debugging | none | separate file outside the repository, mode 0600, size-capped, keys and tokens redacted, off by default (BET-4) |
| R9 | A health probe that reaches the backends | `/healthz` answers for the process | `/healthz?deep=1` and a doctor line (BET-4) |
| R10 | An unknown model gets a clear error that lists the valid ids | passed through unchanged | opt-in strict mode (BET-4) |
| R11 | Auto-mode classifier requests are detected and counted; unparseable verdicts are counted per provider and model; replies are never rewritten | nothing | measure first (BET-4) |
| R12 | A classifier request can be routed to a named model | roles and pins route by model slot only | after R11 shows a need |
| R13 | Mid-conversation system messages are kept in order | pass through for Messages providers, mapped to `developer` for Responses | already met, covered by the compatibility suite |
| R14 | Billing header text is not sent | the launcher sets `CLAUDE_CODE_ATTRIBUTION_HEADER=0` | already met |
| R15 | Duplicate `message_start` and other stream defects are repaired | the guard repairs and counts them | already met |
| R16 | Fall back only before the first byte; surface 429 and quota errors | breaker and fallback chain, 429 relayed with `retry-after` | already met |
| R17 | A provider's own error wording can trigger a fallback ("unable to process" for a model it lists) | not supported | later, when a provider needs it |
| R18 | Per-provider removal of `anthropic-beta` tokens or thinking-mode mapping | tokens pass through | later, when a provider needs it |

## Thinking across a provider switch (R5)

The session registry knows which provider answered a session last. When the next request goes to a
different provider, the gateway removes `thinking` and `redacted_thinking` blocks from the history before
sending, counts the removal in the statistics, and keeps them when the provider is the same.

Measured on the work Copilot account (2026-10-21, a crafted tool-loop request through the gateway):

| Request | Result |
|---|---|
| tool loop, no thinking block, adaptive thinking | HTTP 200 |
| tool loop, a thinking block with another provider's signature, adaptive thinking | HTTP 400 `Invalid signature in thinking block` |
| the budget form (`thinking.type = enabled`) on the sonnet and opus models | HTTP 400, only adaptive is supported |

So the strip is necessary (a foreign signature is refused) and safe (a tool loop without a thinking block is
accepted). Some backends validate more strictly with the budget form of thinking, which requires the last
tool-loop turn to start with a thinking block; the answer there is to map or drop the setting (R18), and it
is built only when a backend needs it. The compatibility suite gets a switch scenario with thinking in the
history to run on both real backends.

## Auto-mode classifier (R11, R12)

Observed elsewhere: the classifier's replies on one model did not follow the `<block>` format, so Claude
Code treated every tool call as blocked. Rewriting a verdict changes a permission decision, so llmgate does
not rewrite replies. It detects classifier requests (a `</block>` stop sequence or the "responding with
<block>" instruction), counts requests and unparseable verdicts per provider and model, and records them in
the ledger and the capture file. A scripted `--permission-mode auto` run through llmgate sent no classifier
request, so the real request is captured from an interactive session with `defaultMode: auto` (the
capture file, R8) before any routing is designed.

## Backends

Bedrock is not a planned provider. A short evaluation (its request format, streaming framing, limits and
cost) decides whether it becomes one; the translators' rules above (R1, R3, R5) are written so they apply.
