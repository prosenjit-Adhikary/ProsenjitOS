#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/input_host_test.c" <<'EOF'
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "input.h"

/* The self-test never calls the privileged PS/2 port reader. */
uint64_t timer_ticks(void) { return 7u; }

_Static_assert(sizeof(struct pros_input_event) == 32u,
               "input event ABI size changed");
_Static_assert(offsetof(struct pros_input_event, timestamp) == 8u,
               "input timestamp ABI offset changed");
_Static_assert(offsetof(struct pros_input_event, value0) == 16u,
               "input value0 ABI offset changed");
_Static_assert(offsetof(struct pros_input_event, flags) == 24u,
               "input flags ABI offset changed");

int main(void)
{
    int rc = pros_input_selftest();
    if (rc != PROS_INPUT_OK) {
        fprintf(stderr, "input self-test failed: %d\n", rc);
        return 1;
    }
    puts("input host self-test passed");
    return 0;
}
EOF

"${CC:-gcc}" -std=c11 -Wall -Wextra -Werror -O2 \
  -I"$ROOT/kernel/include" -I"$ROOT/include" \
  "$ROOT/kernel/input.c" "$TMP/input_host_test.c" \
  -o "$TMP/input_host_test"
"$TMP/input_host_test"
