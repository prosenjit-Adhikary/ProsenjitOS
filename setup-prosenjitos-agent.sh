#!/usr/bin/env bash
# Install prosenjitos-agent for the current user only.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd -P)"
SOURCE="$SCRIPT_DIR/tools/prosenjitos-agent"
INSTALL_DIR="${XDG_BIN_HOME:-$HOME/.local/bin}"
CONFIG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/prosenjitos-agent"
CONFIG_FILE="$CONFIG_DIR/config"

[[ -f "$SOURCE" ]] || { echo "ERROR: missing $SOURCE" >&2; exit 1; }
command -v git >/dev/null 2>&1 || { echo "ERROR: git is required" >&2; exit 1; }

mkdir -p "$INSTALL_DIR" "$CONFIG_DIR"
install -m 0755 "$SOURCE" "$INSTALL_DIR/prosenjitos-agent"

repo_default="$HOME/ProsenjitOS"
if top="$(git rev-parse --show-toplevel 2>/dev/null)"; then
    top="$(cd "$top" && pwd -P)"
    if [[ -f "$top/kernel/prosenjitfs.c" && -f "$top/scripts/build.sh" ]]; then
        repo_default="$top"
    fi
fi
printf 'PROSENJITOS_REPO_CONFIG=%q\n' "$repo_default" > "$CONFIG_FILE"
chmod 0600 "$CONFIG_FILE"

cat <<OUT
prosenjitos-agent installed.

Installation: $INSTALL_DIR/prosenjitos-agent
Configured repository: $repo_default
Config: $CONFIG_FILE
Permissions: user-only executable; config mode 0600; no root required.

Allowed diagnostic operations are fixed Git/build/QEMU/inspection commands.
No arbitrary command executor, SSH/reverse shell, public HTTP shell, or AI-to-shell path is installed.
Source-changing mode is only: fix apply-patch <patch-file>, with git --check and interactive local approval.

If $INSTALL_DIR is not in PATH, open a new shell or run:
  export PATH="$INSTALL_DIR:\$PATH"

Run:
  prosenjitos-agent diagnose
  prosenjitos-agent handoff
OUT
