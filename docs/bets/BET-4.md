# BET-4: robustness for mixed providers

Pending entry in the ScudoAI Planner; delete this file once it is there.

| Field | Value |
|---|---|
| name | llmgate: stay correct when the plan model and the execution model are different providers |
| need | Run opusplan with one provider planning and another executing (for example Copilot Opus 5.5 and Bedrock Opus 4.6) without lost tool calls, rejected signatures, blocked auto-mode calls or silent stalls, and see why when something fails |
| riskiest_assumption | The failures another gateway saw on a Bedrock route also occur on the providers llmgate serves, and a gateway can handle them without rewriting anything that decides a permission |
| smallest_test | A recorded interactive session with defaultMode auto, planning on one provider and executing on another, with the capture file on: classifier requests counted, no unparseable verdict unexplained, no failed request at a switch |
| success signal | The capture shows the classifier traffic and its parse rate per model; switches cost no failed request; limits are advertised per model; a stall ends in an error within the configured idle time |
| decision_date | 2026-11-20 |

The requirements, their state and the decisions are in `docs/gateway-robustness.md`. The cheap fixes (R1 to
R5) go in under BET-2; this bet holds R6 to R12 and the measurement. Slices carry `Bet: BET-4`.
