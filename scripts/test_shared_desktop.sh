#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
cc -std=c11 -Wall -Wextra -Werror -pedantic -I"$root/user/desktop" -I"$root/include" "$root/user/desktop/shared_desktop.c" "$root/tests/shared_desktop_test.c" -o "$tmp/shared-desktop-test"
"$tmp/shared-desktop-test"
