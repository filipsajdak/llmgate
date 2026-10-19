# Security

llmgate holds a credential for an upstream service (a GitHub Copilot login or an API key) and attaches it to requests, so access to the gateway is access to that credential.

## What it does about that

- It listens on loopback by default and refuses a non-loopback address at start-up unless a client key is configured.
- It checks the `Host` header against the listen address (and `allowed_hosts`), which stops DNS rebinding.
- A configured client key is compared in constant time and is never forwarded upstream.
- The Copilot token file is written atomically with mode `0600` (`O_EXCL`, `O_NOFOLLOW`, then rename) and its directory with `0700`.
- `llmgate doctor` and the logs print no secret; the real-run script keeps its token and profile in a temporary directory and deletes them on exit.

The details are in the README, section "Who may use the gateway". Code that parses network input (HTTP, SSE, JSON) is covered by sanitizers, fuzzing and compile-time checks; see the README sections on sanitizers and fuzzing.

## What it does not do

It is a local tool for one user or a small trusted group. It is not hardened for exposure to the internet, does not terminate TLS for clients, and does not isolate users from each other.

## Reporting a vulnerability

Use GitHub's private vulnerability reporting on this repository (Security tab, "Report a vulnerability"). Please do not open a public issue for a security problem. Include the version (`llmgate --version`), the configuration keys involved (without secrets) and the smallest request or input that shows the problem.
