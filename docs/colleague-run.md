# Colleague run sheet

A colleague who has never seen llmgate installs it from a release and finishes a tool turn on two providers, unaided, in under ten minutes. This sheet is for the person watching. The result becomes one row of the table in [the rollout spec](llmgate-rollout.md#c26-tour-and-colleague-runs-phase-1-slice-9), without names or company details.

## Before the run

- A machine, or a fresh user account, with no llmgate and no Homebrew `gcc`. Claude Code may be installed or not; the installer offers it.
- A released version of llmgate (`v0.1.0` or later) and its install command from the README.
- Access to two providers for the colleague, ready to use: for example a Claude login and a GitHub Copilot seat, or a company Bedrock endpoint. If the company has an organisation pack, have its source and tag ready and add `--org SOURCE --org-ref TAG` to the install command.
- A stopwatch, and a text file open for notes.

## What you say

Only this, and nothing else until the end: "Install llmgate with this command, connect two of your providers, and get Claude Code to run `echo hello` with its Bash tool on each. Say out loud what you are doing."

## What the colleague does

1. Runs the install command from the README in a terminal.
2. Answers the questions of `llmgate init` (which providers they have, signing in, a recipe).
3. Runs `llmgate claude` and asks for `echo hello` with the Bash tool.
4. Switches to the second provider (the pane's target picker, or `llmgate use`) and asks again.

## Stopwatch points

| Point | When |
|---|---|
| T0 | the colleague is told to start |
| T1 | `llmgate --version` prints a version |
| T2 | `llmgate init` has finished |
| T3 | the first tool turn has finished |
| T4 | the tool turn on the second provider has finished |

## The rules for the observer

- Do not type, point or explain. If the colleague is stuck for a full minute, write down the exact text on the screen and what they tried; you may then give one hint, and you note that you did.
- Pass: T4 - T0 is under ten minutes and no hint was needed. A hint, or more than ten minutes, is a fail that still teaches us something.
- Write down every moment of hesitation, every message the colleague did not understand and every question they asked aloud.

## After the run

Add one row to the results table in the spec: the date, the system (operating system and architecture), the minutes from T0 to T3, the providers used, what blocked the colleague, and the change that fixed it (a commit or an issue). Keep it free of names and company details. Then fix what blocked them, and run again with a different colleague when the fix is released.
