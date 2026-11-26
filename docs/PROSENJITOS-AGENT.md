# ProsenjitOS local diagnostic agent

## Commands

Run from the repository after setup:

```text
prosenjitos-agent diagnose
prosenjitos-agent handoff
prosenjitos-agent fix apply-local-patch /path/inside/repo/change.patch
```

Set `PROSENJITOS_REPO` when the checkout is not `~/ProsenjitOS`.

## Safety model

The agent is a local, repository-confined diagnostic runner. It does not listen on a network socket and does not provide SSH, HTTP, reverse-shell, or arbitrary command execution.

Before every operation it verifies:

1. `git rev-parse --show-toplevel`
2. repository basename is `ProsenjitOS`
3. `origin` points to `prosenjit-Adhikary/ProsenjitOS`

Diagnostic commands are fixed by the agent. AI output is never passed to a shell.

The source-changing `fix` path currently accepts only the named operation `apply-local-patch`. The patch must be inside the verified repository, have a `.patch` or `.diff` suffix, pass `git apply --check`, and receive an interactive `APPLY-PATCH` confirmation.

The agent never performs `git reset --hard`, `git clean -fd`, force-push, branch deletion, device formatting, network/firewall changes, shutdown/reboot, or arbitrary sudo commands.

## Diagnostic scope

The agent records the current HEAD, branch, working tree, recent history, build output, QEMU serial/debug output, host resource information, and current ProsenjitFS diagnostic source definitions.

The current repository build procedure is `./scripts/build.sh`. The current CI workflow also defines both GRUB ISO and native BIOS QEMU boot tests; the agent follows those two QEMU image paths locally.

ProsenjitFS diagnostics are read from the implementation at runtime/build time rather than assuming a historical interface. The report preserves the observed `mkdir_stage`, `mkdir_rc`, `tx_stage`, and `tx_count` values and the current source assignments.

## AI handoff

`handoff` creates `reports/ai-handoff.txt` from the latest report. The handoff explicitly tells an AI to use current HEAD evidence, separate QEMU failures from filesystem failures, fix root causes, rebuild, rerun tests, and never weaken or mask tests.

A user may manually pipe that handoff to an AI CLI. There is deliberately no automatic AI-to-shell execution path.

## Chatblade

Chatblade was investigated as an optional interface. Its upstream repository is now archived, so this agent does not depend on it. If a user has Chatblade installed, it can remain a presentation/AI-query layer around `reports/ai-handoff.txt`; it is not granted execution authority by this agent.

The supported architecture remains:

```text
ProsenjitOS agent
      |
      v
diagnostic report
      |
      v
AI CLI / Chat UI
      |
      v
named structured operation
      |
      v
local allowlist + human approval
      |
      v
agent operation
      |
      v
result/report
```

## Reports

Each diagnosis writes:

- `reports/latest.txt`
- `reports/latest.json`
- `reports/ai-handoff.txt`
- timestamped text/JSON reports
- a timestamped private diagnostic evidence directory under `reports/.agent-*`

The evidence directory is kept inside the repository so the agent does not wander into unrelated parts of the host filesystem.
