#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD_DIR="${PROSENJITOS_CRASH_TEST_BUILD_DIR:-build-crash-test}"
IMAGE="$BUILD_DIR/prosenjit-native-bios.img"
ELF="$BUILD_DIR/prosenjit.elf"
LOG_DIR="$BUILD_DIR/two-boot-logs"
GDB_PORT="${PROSENJITOS_CRASH_TEST_GDB_PORT:-12345}"
QEMU_TIMEOUT_SECONDS="${PROSENJITOS_CRASH_TEST_TIMEOUT_SECONDS:-40}"

for file in "$IMAGE" "$ELF"; do
    if [[ ! -s "$file" ]]; then
        echo "Missing crash-test input: $file" >&2
        exit 1
    fi
done
for tool in qemu-system-x86_64 gdb timeout sha256sum; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "Missing required crash-test tool: $tool" >&2
        exit 1
    }
done

mkdir -p "$LOG_DIR"
BOOT1_SERIAL="$LOG_DIR/boot1-serial.log"
BOOT1_DEBUGCON="$LOG_DIR/boot1-debugcon.log"
BOOT1_QEMU="$LOG_DIR/boot1-qemu.log"
GDB_LOG="$LOG_DIR/boot1-gdb.log"
BOOT2_SERIAL="$LOG_DIR/boot2-serial.log"
BOOT2_DEBUGCON="$LOG_DIR/boot2-debugcon.log"
BOOT2_QEMU="$LOG_DIR/boot2-qemu.log"
: > "$BOOT1_SERIAL"
: > "$BOOT1_DEBUGCON"
: > "$BOOT1_QEMU"
: > "$GDB_LOG"
: > "$BOOT2_SERIAL"
: > "$BOOT2_DEBUGCON"
: > "$BOOT2_QEMU"

qemu_pid=""
cleanup() {
    if [[ -n "$qemu_pid" ]] && kill -0 "$qemu_pid" 2>/dev/null; then
        kill -TERM "$qemu_pid" 2>/dev/null || true
        wait "$qemu_pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT

wait_for_marker() {
    local logfile="$1"
    local marker="$2"
    local pid="$3"
    local description="$4"
    local ticks=$((QEMU_TIMEOUT_SECONDS * 10))
    for ((i = 0; i < ticks; ++i)); do
        if grep -Fq "$marker" "$logfile"; then
            return 0
        fi
        if ! kill -0 "$pid" 2>/dev/null; then
            echo "$description QEMU exited before marker: $marker" >&2
            return 1
        fi
        sleep 0.1
    done
    echo "$description timed out waiting for marker: $marker" >&2
    return 1
}

echo "[crash-test] Boot 1: launch isolated image paused with GDB stub."
qemu-system-x86_64 \
    -drive "format=raw,file=$IMAGE,if=ide" \
    -display none \
    -serial "file:$BOOT1_SERIAL" \
    -debugcon "file:$BOOT1_DEBUGCON" \
    -global isa-debugcon.iobase=0xe9 \
    -no-reboot -no-shutdown \
    -gdb "tcp:127.0.0.1:$GDB_PORT" -S \
    -d int -D "$BOOT1_QEMU" \
    >"$LOG_DIR/boot1-qemu-stdout.log" 2>&1 &
qemu_pid=$!
sleep 1

echo "[crash-test] GDB waits for the post-attach test gate, then releases UD2."
set +e
timeout "$QEMU_TIMEOUT_SECONDS" gdb -q -batch "$ELF" \
    -ex "target remote 127.0.0.1:$GDB_PORT" \
    -ex "break pros_crash_record_qemu_test_gate" \
    -ex "continue" \
    -ex "set variable pros_crash_record_qemu_test_trigger = 1" \
    -ex "detach" >"$GDB_LOG" 2>&1
gdb_status=$?
set -e
cat "$GDB_LOG"
if (( gdb_status != 0 )); then
    echo "GDB controller failed (status $gdb_status)." >&2
    exit 1
fi

wait_for_marker "$BOOT1_DEBUGCON" "CRASHREC_WRITE_FLUSH_OK" "$qemu_pid" "Boot 1"
grep -Fq "CRASHREC_WAITING_FOR_GDB" "$BOOT1_DEBUGCON" || {
    echo "Boot 1 never reached the armed post-attach wait point." >&2
    exit 1
}
grep -Fq "CRASHREC_TRIGGER_RELEASED" "$BOOT1_DEBUGCON" || {
    echo "Boot 1 did not confirm GDB trigger release." >&2
    exit 1
}
echo "[crash-test] Boot 1 production ATA sector write + flush confirmed."

# Stop and reap QEMU before reusing the same image in a new process.
kill -TERM "$qemu_pid" 2>/dev/null || true
wait "$qemu_pid" 2>/dev/null || true
qemu_pid=""
BOOT1_IMAGE_SHA="$(sha256sum "$IMAGE" | awk '{print $1}')"
echo "[crash-test] Retained image after Boot 1: $BOOT1_IMAGE_SHA"

echo "[crash-test] Boot 2: new QEMU process, same retained raw image."
qemu-system-x86_64 \
    -drive "format=raw,file=$IMAGE,if=ide" \
    -display none \
    -serial "file:$BOOT2_SERIAL" \
    -debugcon "file:$BOOT2_DEBUGCON" \
    -global isa-debugcon.iobase=0xe9 \
    -no-reboot -no-shutdown \
    -d int -D "$BOOT2_QEMU" \
    >"$LOG_DIR/boot2-qemu-stdout.log" 2>&1 &
qemu_pid=$!

wait_for_marker "$BOOT2_DEBUGCON" "CRASHREC_BOOT2_RECOVERY_OK" "$qemu_pid" "Boot 2"
if grep -Fq "CRASHREC_BOOT2_RECOVERY_FAIL" "$BOOT2_DEBUGCON"; then
    echo "Boot 2 production reader rejected the persisted crash record." >&2
    exit 1
fi

kill -TERM "$qemu_pid" 2>/dev/null || true
wait "$qemu_pid" 2>/dev/null || true
qemu_pid=""
BOOT2_IMAGE_SHA="$(sha256sum "$IMAGE" | awk '{print $1}')"
if [[ "$BOOT1_IMAGE_SHA" != "$BOOT2_IMAGE_SHA" ]]; then
    echo "Disk image changed during Boot 2; same-image recovery test is invalid." >&2
    echo "before=$BOOT1_IMAGE_SHA after=$BOOT2_IMAGE_SHA" >&2
    exit 1
fi

echo "[crash-test] Boot 2 production reader recovered sequence=1, vector=6."
echo "[crash-test] Same-image hash unchanged during Boot 2."
echo "[crash-test] REAL PANIC WRITE+FLUSH AND TWO-BOOT RECOVERY PASS."
