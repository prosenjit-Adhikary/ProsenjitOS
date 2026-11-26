#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd -P)"
AGENT="$ROOT/prosenjitos-agent"
INSTALL_DIR="${HOME}/.local/bin"
TARGET="$INSTALL_DIR/prosenjitos-agent"

[ -f "$AGENT" ] || { echo "ERROR: $AGENT not found" >&2; exit 2; }
command -v git >/dev/null 2>&1 || { echo "ERROR: git is required" >&2; exit 2; }
git -C "$ROOT" rev-parse --show-toplevel >/dev/null 2>&1 ||
  { echo "ERROR: not a Git repository" >&2; exit 2; }
[ "$(basename "$ROOT")" = "ProsenjitOS" ] ||
  { echo "ERROR: repository must be named ProsenjitOS" >&2; exit 2; }

REMOTE="$(git -C "$ROOT" remote get-url origin 2>/dev/null || true)"
case "$REMOTE" in
  *github.com/prosenjit-Adhikary/ProsenjitOS.git|*github.com/prosenjit-Adhikary/ProsenjitOS) ;;
  *) echo "ERROR: unexpected origin: ${REMOTE:-<none>}" >&2; exit 2;;
esac

mkdir -p "$INSTALL_DIR"
chmod 0755 "$AGENT"
ln -sfn "$AGENT" "$TARGET"

cat <<INFO
prosenjitos-agent installed.

Installation:
  $TARGET -> $AGENT
Repository:
  $ROOT
Permissions:
  agent 0755; no root privileges requested

Run:
  PROSENJITOS_REPO="$ROOT" prosenjitos-agent diagnose
  PROSENJITOS_REPO="$ROOT" prosenjitos-agent handoff

Security restrictions:
  - local execution only; no listener, HTTP shell, SSH/reverse shell, or network server
  - repository root and Git remote are verified before operation
  - diagnose uses fixed Git/build/QEMU/system-information commands
  - no sudo, reset --hard, clean -fd, force-push, branch deletion, device formatting,
    network/firewall changes, shutdown/reboot, or arbitrary command execution
  - source-changing fixes require a named allowlisted operation and interactive approval
  - AI text is never executed as shell commands

If ~/.local/bin is not on PATH:
  export PATH="$HOME/.local/bin:$PATH"
INFO
