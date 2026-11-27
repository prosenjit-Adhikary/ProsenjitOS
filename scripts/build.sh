#!/usr/bin/env bash
set -euo pipefail
trap 'status=$?; printf "Build failed at line %s: %s (exit %s)\\n" "${BASH_LINENO[0]:-unknown}" "$BASH_COMMAND" "$status" >&2; exit "$status"' ERR

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_NAME="${PROSENJITOS_BUILD_NAME:-build}"
BUILD="$ROOT/$BUILD_NAME"
CRASH_RECORD_QEMU_TEST="${PROSENJITOS_CRASH_RECORD_QEMU_TEST:-0}"
ISO="$BUILD/prosenjitOS.iso"

# Fail before deleting any previous build output when the host toolchain is
# incomplete. Keep this list aligned with every external command used below.
BUILD_ISO="${PROSENJITOS_BUILD_ISO:-1}"
BUILD_NATIVE_BIOS="${PROSENJITOS_BUILD_NATIVE_BIOS:-1}"
for value in "$BUILD_ISO" "$BUILD_NATIVE_BIOS" "$CRASH_RECORD_QEMU_TEST"; do
    if [[ "$value" != "0" && "$value" != "1" ]]; then
        echo "Build mode flags must be 0 or 1" >&2
        exit 2
    fi
done

required_tools=(
    awk bash cat cmp cp dd gcc grep ld ln mkdir nasm nm objcopy python3 rm sha256sum stat truncate
)
if [[ "$BUILD_ISO" == "1" ]]; then
    required_tools+=(grub-file grub-mkrescue grub-script-check mcopy mformat xorriso)
fi
if [[ "$BUILD_NATIVE_BIOS" == "1" ]]; then
    required_tools+=(mkfifo)
fi
for tool in "${required_tools[@]}"; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "Missing required build tool: $tool" >&2
        exit 127
    fi
done

if [[ -z "$ROOT" || ( "$BUILD_NAME" != "build" && "$BUILD_NAME" != "build-crash-test" ) || "$BUILD" == "/" ]]; then
    echo "Refusing unsafe build directory: $BUILD" >&2
    exit 2
fi
if [[ "$CRASH_RECORD_QEMU_TEST" == "1" && "$BUILD_NAME" != "build-crash-test" ]]; then
    echo "Crash-record QEMU test mode requires PROSENJITOS_BUILD_NAME=build-crash-test" >&2
    exit 2
fi

rm -rf -- "$BUILD"
mkdir -p "$BUILD/obj"

# Exercise the shared Boot Protocol v1 parser with adversarial host fixtures.
gcc -std=c11 -Wall -Wextra -Werror -I"$ROOT/include" \
    "$ROOT/kernel/boot_protocol.c" "$ROOT/tests/boot_protocol_test.c" \
    -o "$BUILD/boot_protocol_test"
"$BUILD/boot_protocol_test"

gcc -std=c11 -Wall -Wextra -Werror -DPROSENJIT_PMM_HOST_TEST -ffunction-sections -fdata-sections \
    -I"$ROOT/include" -I"$ROOT/kernel/include" \
    "$ROOT/kernel/boot_protocol.c" "$ROOT/kernel/memory.c" \
    "$ROOT/tests/boot_v1_pmm_test.c" \
    -Wl,--gc-sections -Wl,--defsym=__kernel_start=0x100000 \
    -Wl,--defsym=__kernel_end=0x110000 \
    -o "$BUILD/boot_v1_pmm_test"
"$BUILD/boot_v1_pmm_test"

nasm -f elf64 "$ROOT/boot/multiboot2_header.asm" -o "$BUILD/obj/multiboot2_header.o"
nasm -f elf64 "$ROOT/boot/entry.asm" -o "$BUILD/obj/entry.o"
nasm -f elf64 "$ROOT/kernel/usercopy.asm" -o "$BUILD/obj/usercopy_asm.o"
nasm -f elf64 "$ROOT/kernel/idt.asm" -o "$BUILD/obj/idt.o"
nasm -f elf64 "$ROOT/kernel/context_switch.asm" -o "$BUILD/obj/context_switch.o"
nasm -f elf64 "$ROOT/kernel/panic_handoff.asm" -o "$BUILD/obj/panic_handoff.o"

CFLAGS=(-m64 -march=x86-64 -mtune=generic -mno-bmi2 -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns -fno-pie -fno-stack-protector -mno-red-zone -mno-sse -mno-mmx -I"$ROOT/kernel/include" -I"$ROOT/include" -I"$ROOT/third_party/fatfs" -I"$ROOT/third_party/fatfs/include" -Wall -Wextra -Werror -O2)
if [[ "$CRASH_RECORD_QEMU_TEST" == "1" ]]; then
    CFLAGS+=(-DPROS_CRASH_RECORD_QEMU_TEST=1)
fi

gcc "${CFLAGS[@]}" -c "$ROOT/kernel/kernel.c" -o "$BUILD/obj/kernel.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/panic_handoff.c" -o "$BUILD/obj/panic_handoff_c.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/memory.c" -o "$BUILD/obj/memory.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/boot_protocol.c" -o "$BUILD/obj/boot_protocol.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/paging.c" -o "$BUILD/obj/paging.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/heap.c" -o "$BUILD/obj/heap.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/address_space.c" -o "$BUILD/obj/address_space.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/elf_loader.c" -o "$BUILD/obj/elf_loader.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/syscall.c" -o "$BUILD/obj/syscall.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/object.c" -o "$BUILD/obj/object.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/usercopy.c" -o "$BUILD/obj/usercopy.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/ipc.c" -o "$BUILD/obj/ipc.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/process.c" -o "$BUILD/obj/process.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/init.c" -o "$BUILD/obj/init.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/service_manager.c" -o "$BUILD/obj/service_manager.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/thread.c" -o "$BUILD/obj/thread.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/timer.c" -o "$BUILD/obj/timer.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/intent.c" -o "$BUILD/obj/intent.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/security_file.c" -o "$BUILD/obj/security_file.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/sha512.c" -o "$BUILD/obj/sha512.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/ed25519_field.c" -o "$BUILD/obj/ed25519_field.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/ed25519_point.c" -o "$BUILD/obj/ed25519_point.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/ed25519_scalar.c" -o "$BUILD/obj/ed25519_scalar.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/security_provider.c" -o "$BUILD/obj/security_provider.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/security_signature.c" -o "$BUILD/obj/security_signature.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/security_admission.c" -o "$BUILD/obj/security_admission.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/security_quarantine.c" -o "$BUILD/obj/security_quarantine.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/security_exec.c" -o "$BUILD/obj/security_exec.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/security_monitor.c" -o "$BUILD/obj/security_monitor.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/security_hardening.c" -o "$BUILD/obj/security_hardening.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/pic.c" -o "$BUILD/obj/pic.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/block.c" -o "$BUILD/obj/block.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/fatfs_disk.c" -o "$BUILD/obj/fatfs_disk.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/fatfs_provider.c" -o "$BUILD/obj/fatfs_provider.o"
gcc "${CFLAGS[@]}" -c "$ROOT/third_party/fatfs/ff.c" -o "$BUILD/obj/fatfs_ff.o"
gcc "${CFLAGS[@]}" -c "$ROOT/third_party/fatfs/option/ffunicode.c" -o "$BUILD/obj/fatfs_unicode.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/vfs.c" -o "$BUILD/obj/vfs.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/vfs_provider.c" -o "$BUILD/obj/vfs_provider.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/prosenjitfs.c" -o "$BUILD/obj/prosenjitfs.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/partition.c" -o "$BUILD/obj/partition.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/storage.c" -o "$BUILD/obj/storage.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/fatfs_libc.c" -o "$BUILD/obj/fatfs_libc.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/pci.c" -o "$BUILD/obj/pci.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/usb_msc.c" -o "$BUILD/obj/usb_msc.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/usb_dma.c" -o "$BUILD/obj/usb_dma.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/usb_xhci.c" -o "$BUILD/obj/usb_xhci.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/usb_xhci_ring.c" -o "$BUILD/obj/usb_xhci_ring.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/usb_desc.c" -o "$BUILD/obj/usb_desc.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/driver.c" -o "$BUILD/obj/driver.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/health.c" -o "$BUILD/obj/health.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/crash_record.c" -o "$BUILD/obj/crash_record.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/log.c" -o "$BUILD/obj/log.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/log_persist.c" -o "$BUILD/obj/log_persist.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/driver_class.c" -o "$BUILD/obj/driver_class.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/network.c" -o "$BUILD/obj/network.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/driver_bundle.c" -o "$BUILD/obj/driver_bundle.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/device_broker.c" -o "$BUILD/obj/device_broker.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/display.c" -o "$BUILD/obj/display.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/driver_compat.c" -o "$BUILD/obj/driver_compat.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/nt_compat.c" -o "$BUILD/obj/nt_compat.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/pe_inspect.c" -o "$BUILD/obj/pe_inspect.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/ata_pio.c" -o "$BUILD/obj/ata_pio.o"

ld -m elf_x86_64 -T "$ROOT/kernel/linker.ld" -nostdlib -o "$BUILD/prosenjit.elf"   "$BUILD/obj/multiboot2_header.o" "$BUILD/obj/entry.o" "$BUILD/obj/idt.o"   "$BUILD/obj/context_switch.o" "$BUILD/obj/panic_handoff.o" "$BUILD/obj/panic_handoff_c.o" "$BUILD/obj/kernel.o" "$BUILD/obj/memory.o" "$BUILD/obj/boot_protocol.o"   "$BUILD/obj/paging.o" "$BUILD/obj/heap.o" "$BUILD/obj/address_space.o" "$BUILD/obj/elf_loader.o"   "$BUILD/obj/syscall.o" "$BUILD/obj/object.o" "$BUILD/obj/usercopy.o"   "$BUILD/obj/usercopy_asm.o" "$BUILD/obj/ipc.o" "$BUILD/obj/process.o" "$BUILD/obj/init.o" "$BUILD/obj/service_manager.o"   "$BUILD/obj/thread.o" "$BUILD/obj/timer.o" "$BUILD/obj/intent.o"   "$BUILD/obj/security_file.o" "$BUILD/obj/sha512.o" "$BUILD/obj/ed25519_field.o" "$BUILD/obj/ed25519_point.o" "$BUILD/obj/ed25519_scalar.o" "$BUILD/obj/security_provider.o" "$BUILD/obj/security_signature.o" "$BUILD/obj/security_admission.o"   "$BUILD/obj/security_quarantine.o" "$BUILD/obj/security_exec.o" "$BUILD/obj/security_monitor.o" "$BUILD/obj/security_hardening.o" "$BUILD/obj/pic.o"   "$BUILD/obj/block.o" "$BUILD/obj/fatfs_disk.o" "$BUILD/obj/fatfs_provider.o" "$BUILD/obj/fatfs_ff.o" "$BUILD/obj/fatfs_unicode.o" "$BUILD/obj/vfs.o" "$BUILD/obj/vfs_provider.o" "$BUILD/obj/prosenjitfs.o" "$BUILD/obj/partition.o" "$BUILD/obj/storage.o" "$BUILD/obj/fatfs_libc.o" "$BUILD/obj/pci.o" "$BUILD/obj/usb_msc.o" "$BUILD/obj/usb_dma.o" "$BUILD/obj/usb_xhci.o" "$BUILD/obj/usb_xhci_ring.o" "$BUILD/obj/usb_desc.o" "$BUILD/obj/driver.o" "$BUILD/obj/health.o" "$BUILD/obj/crash_record.o" "$BUILD/obj/log.o" "$BUILD/obj/log_persist.o" "$BUILD/obj/driver_class.o" "$BUILD/obj/network.o" "$BUILD/obj/driver_bundle.o" "$BUILD/obj/device_broker.o" "$BUILD/obj/display.o" "$BUILD/obj/driver_compat.o" "$BUILD/obj/nt_compat.o" "$BUILD/obj/pe_inspect.o" "$BUILD/obj/ata_pio.o"

# Verify the exact Multiboot2 request-tag sequence in the linked image.
objcopy --dump-section .text="$BUILD/text-header-check.bin" "$BUILD/prosenjit.elf"
python3 - "$BUILD/text-header-check.bin" <<'PY'
import pathlib
import struct
import sys

section = pathlib.Path(sys.argv[1]).read_bytes()
if len(section) < 40:
    raise SystemExit("linked .text is too short for the Multiboot2 header")
blob = section[:40]
magic, arch, length, checksum = struct.unpack_from("<4I", blob, 0)
if (magic, arch, length) != (0xE85250D6, 0, 40):
    raise SystemExit("Multiboot2 fixed header fields are invalid")
if (magic + arch + length + checksum) & 0xFFFFFFFF:
    raise SystemExit("Multiboot2 header checksum is invalid")
if struct.unpack_from("<HHII", blob, 16) != (1, 0, 12, 6):
    raise SystemExit("required Multiboot2 mmap information-request tag is missing")
if blob[28:32] != bytes(4) or struct.unpack_from("<HHI", blob, 32) != (0, 0, 8):
    raise SystemExit("Multiboot2 end tag or alignment padding is malformed")
print("Multiboot2 required mmap request and end tag verified")
PY


if [[ "$BUILD_ISO" == "1" ]]; then
    mkdir -p "$BUILD/isodir/boot/grub"
    grub-script-check "$ROOT/boot/grub.cfg"
    grub-file --is-x86-multiboot2 "$BUILD/prosenjit.elf"
cp "$BUILD/prosenjit.elf" "$BUILD/isodir/boot/prosenjit.elf"
cp "$ROOT/boot/grub.cfg" "$BUILD/isodir/boot/grub/grub.cfg"
grub-mkrescue -o "$ISO" "$BUILD/isodir" >/dev/null

# Validate the produced ISO as an artifact, not merely the exit status of
# grub-mkrescue. Confirm a BIOS El Torito entry and byte-identical boot files.
if [[ ! -s "$ISO" ]]; then
    echo "GRUB ISO creation produced an empty or missing image: $ISO" >&2
    exit 1
fi
ISO_BYTES="$(stat -c%s "$ISO")"
if (( ISO_BYTES % 2048 != 0 )); then
    echo "ISO image size is not aligned to 2048-byte ISO9660 blocks: $ISO_BYTES" >&2
    exit 1
fi
mkdir -p "$BUILD/iso-verify"
if ! xorriso -indev "$ISO" -report_el_torito plain \
    >"$BUILD/iso-verify/el-torito-report.txt" 2>&1; then
    cat "$BUILD/iso-verify/el-torito-report.txt" >&2
    echo "Unable to inspect ISO El Torito boot catalog" >&2
    exit 1
fi
if ! grep -Eiq 'BIOS' "$BUILD/iso-verify/el-torito-report.txt"; then
    cat "$BUILD/iso-verify/el-torito-report.txt" >&2
    echo "ISO does not advertise a BIOS El Torito boot entry" >&2
    exit 1
fi
if ! xorriso -osirrox on -indev "$ISO" \
    -extract /boot/prosenjit.elf "$BUILD/iso-verify/prosenjit.elf" \
    -extract /boot/grub/grub.cfg "$BUILD/iso-verify/grub.cfg" \
    >"$BUILD/iso-verify/extract.log" 2>&1; then
    cat "$BUILD/iso-verify/extract.log" >&2
    echo "Unable to extract required kernel/GRUB files from ISO" >&2
    exit 1
fi
cmp -s "$BUILD/prosenjit.elf" "$BUILD/iso-verify/prosenjit.elf" || {
    echo "Kernel embedded in ISO differs from the linked kernel ELF" >&2
    exit 1
}
cmp -s "$ROOT/boot/grub.cfg" "$BUILD/iso-verify/grub.cfg" || {
    echo "GRUB configuration embedded in ISO differs from source" >&2
    exit 1
}
grub-file --is-x86-multiboot2 "$BUILD/iso-verify/prosenjit.elf" || {
    echo "Kernel extracted from ISO is not a valid Multiboot2 image" >&2
    exit 1
}
echo "Validated bootable BIOS ISO: $ISO ($ISO_BYTES bytes)"

echo "Built: $ISO"
fi

if [[ "$BUILD_NATIVE_BIOS" == "1" ]]; then
# Native BIOS boot artifact. This path deliberately does not depend on GRUB,
# MBR partition parsing, or a filesystem: the stage-1/2 loader reads the
# kernel from a fixed LBA layout. Partition/filesystem discovery remains a
# later storage concern.
NATIVE_DISK="$BUILD/prosenjit-native-bios.img"
NATIVE_STAGE2_SECTORS=24
NATIVE_STAGE2_LOAD_ADDRESS=0x8000
NATIVE_KERNEL_LOAD_BUFFER=0x10000
NATIVE_KERNEL_LOAD_LIMIT=0x18000
NATIVE_STAGE2_LOAD_END=$((NATIVE_STAGE2_LOAD_ADDRESS + NATIVE_STAGE2_SECTORS * 512))
if (( NATIVE_STAGE2_LOAD_END > NATIVE_KERNEL_LOAD_BUFFER )); then
    echo "Native stage-two image overlaps the kernel bounce buffer" >&2
    exit 1
fi
NATIVE_KERNEL_LBA=$((1 + NATIVE_STAGE2_SECTORS))
# Native disk-image ABI: two sectors are reserved for crash-record storage.
# They are not part of ProsenjitFS and must never be covered by the kernel.
NATIVE_CRASH_RECORD_LBA=2046
NATIVE_CRASH_RECORD_SECTORS=2
NATIVE_CRASH_RECORD_END=$((NATIVE_CRASH_RECORD_LBA + NATIVE_CRASH_RECORD_SECTORS))
NATIVE_PFS_START_LBA=2048
NATIVE_PFS_SECTORS=512
objcopy -O binary "$BUILD/prosenjit.elf" "$BUILD/prosenjit.bin"
KERNEL_BYTES="$(stat -c%s "$BUILD/prosenjit.bin")"

# The Multiboot2 header is not guaranteed to be immediately followed by
# _start: the linker must honor the input .text alignment. Resolve the
# actual linked entry symbol instead of assuming a fixed 24-byte offset.
KERNEL_ENTRY_HEX="$(nm -n "$BUILD/prosenjit.elf" | awk '$3 == "_start" { print $1; exit }')"
if [[ ! "$KERNEL_ENTRY_HEX" =~ ^[0-9A-Fa-f]+$ ]]; then
    echo "Unable to resolve the linked native kernel entry symbol (_start)" >&2
    exit 1
fi
NATIVE_KERNEL_ENTRY_OFFSET=$((16#$KERNEL_ENTRY_HEX - 0x100000))
if (( NATIVE_KERNEL_ENTRY_OFFSET < 0 || NATIVE_KERNEL_ENTRY_OFFSET >= KERNEL_BYTES )); then
    echo "Linked native kernel entry is outside the flat kernel image" >&2
    exit 1
fi
echo "Native kernel entry offset: $NATIVE_KERNEL_ENTRY_OFFSET"

if (( KERNEL_BYTES == 0 )); then
    echo "Native kernel flat image is empty" >&2
    exit 1
fi
NATIVE_KERNEL_SECTORS=$(( (KERNEL_BYTES + 511) / 512 ))
if (( NATIVE_KERNEL_SECTORS == 0 )); then
    echo "Native bootloader kernel sector count is invalid" >&2
    exit 1
fi
if (( NATIVE_KERNEL_LBA < 1 + NATIVE_STAGE2_SECTORS )); then
    echo "Native kernel LBA overlaps the reserved stage-1/stage-2 boot area" >&2
    exit 1
fi
if (( NATIVE_KERNEL_LBA + NATIVE_KERNEL_SECTORS > NATIVE_CRASH_RECORD_LBA )); then
    echo "Native kernel image overlaps the reserved crash-record extent (LBA $NATIVE_CRASH_RECORD_LBA-$((NATIVE_CRASH_RECORD_END - 1)))" >&2
    exit 1
fi
if (( NATIVE_CRASH_RECORD_END > NATIVE_PFS_START_LBA )); then
    echo "Native crash-record extent overlaps the ProsenjitFS partition" >&2
    exit 1
fi

nasm -f bin -dSTAGE2_SECTORS="$NATIVE_STAGE2_SECTORS" \
    -dPFS_START_LBA="$NATIVE_PFS_START_LBA" -dPFS_SECTORS="$NATIVE_PFS_SECTORS" \
    "$ROOT/boot/native_stage1.asm" -o "$BUILD/native_stage1.bin"
nasm -f bin -l "$BUILD/native_stage2.lst" \
    -dKERNEL_LBA="$NATIVE_KERNEL_LBA" -dKERNEL_SECTORS="$NATIVE_KERNEL_SECTORS" \
    -dKERNEL_ENTRY_OFFSET="$NATIVE_KERNEL_ENTRY_OFFSET" \
    "$ROOT/boot/native_stage2.asm" -o "$BUILD/native_stage2.bin"

echo "===== Native stage-2 assembled far-return listing ====="
grep -n -B 12 -A 55 "copy_chunk_pm" "$BUILD/native_stage2.lst"

# NASM's listing addresses are section-relative and show pre-relocation bytes.
# Verify both the 32->16-bit protected-mode jump and the 16-bit real-mode jump
# in the final flat binary, including the ORG-adjusted physical code offsets.
python3 - "$BUILD/native_stage2.bin" "$BUILD/native_stage2.lst" <<'PY'
import pathlib
import re
import struct
import sys

binary_path, listing_path = sys.argv[1:]
image = pathlib.Path(binary_path).read_bytes()
listing = pathlib.Path(listing_path).read_text()
pm16_jump = None
real_jump = None
for line in listing.splitlines():
    match = re.search(r"\b([0-9A-Fa-f]{8})\s+[0-9A-Fa-f]+\s+", line)
    if not match:
        continue
    offset = int(match.group(1), 16)
    if "db 0x66, 0xEA" in line:
        pm16_jump = offset
    elif line.rstrip().endswith("db 0xEA"):
        real_jump = offset
if pm16_jump is None or real_jump is None:
    raise SystemExit("Unable to locate both native protected/real-mode far jumps")
if pm16_jump + 6 > len(image) or real_jump + 5 > len(image):
    raise SystemExit("Native far-jump pointer extends beyond stage-2 image")

pm16_target = struct.unpack_from("<HH", image, pm16_jump + 2)
expected_pm16 = 0x8000 + pm16_jump + 6
real_target = struct.unpack_from("<HH", image, real_jump + 1)
expected_real = 0x8000 + real_jump + 5
print(f"Native PM32->PM16 far jump: {image[pm16_jump:pm16_jump + 6].hex(' ')}")
print(f"Native PM16->real far jump: {image[real_jump:real_jump + 5].hex(' ')}")
print(f"Native PM32->PM16 target: selector {pm16_target[1]:#06x}, offset {pm16_target[0]:#06x}; expected selector 0x0018, offset {expected_pm16:#06x}")
print(f"Native PM16->real target: selector {real_target[1]:#06x}, offset {real_target[0]:#06x}; expected selector 0x0000, offset {expected_real:#06x}")
if image[pm16_jump:pm16_jump + 2] != bytes((0x66, 0xEA)) or pm16_target != (expected_pm16, 0x18):
    raise SystemExit("Native PM32->PM16 far-jump encoding/target is invalid")
if image[real_jump] != 0xEA or real_target != (expected_real, 0):
    raise SystemExit("Native PM16->real far-jump encoding/target is invalid")
print("Native protected/real-mode far-jump binary validation passed.")
PY
if (( $(stat -c%s "$BUILD/native_stage1.bin") != 512 )); then
    echo "Native stage-1 is not exactly one boot sector" >&2
    exit 1
fi
NATIVE_STAGE2_BYTES="$(stat -c%s "$BUILD/native_stage2.bin")"
if (( NATIVE_STAGE2_BYTES != NATIVE_STAGE2_SECTORS * 512 )); then
    echo "Native stage-2 does not exactly fill its reserved boot area" >&2
    exit 1
fi

NATIVE_KERNEL_END=$((NATIVE_KERNEL_LBA + NATIVE_KERNEL_SECTORS))
NATIVE_PFS_END=$((NATIVE_PFS_START_LBA + NATIVE_PFS_SECTORS))
NATIVE_TOTAL_SECTORS=$((NATIVE_KERNEL_END > NATIVE_PFS_END ? NATIVE_KERNEL_END : NATIVE_PFS_END))
truncate -s $((NATIVE_TOTAL_SECTORS * 512)) "$NATIVE_DISK"
dd if="$BUILD/native_stage1.bin" of="$NATIVE_DISK" bs=512 seek=0 conv=notrunc status=none
dd if="$BUILD/native_stage2.bin" of="$NATIVE_DISK" bs=512 seek=1 conv=notrunc status=none
dd if="$BUILD/prosenjit.bin" of="$NATIVE_DISK" bs=512 seek="$NATIVE_KERNEL_LBA" conv=notrunc status=none

# Read the complete image back and verify the boot signature, exact stage
# placement, kernel payload, MBR partition descriptor, and image geometry.
python3 - "$NATIVE_DISK" "$BUILD/native_stage1.bin" "$BUILD/native_stage2.bin" \
    "$BUILD/prosenjit.bin" "$NATIVE_STAGE2_SECTORS" "$NATIVE_KERNEL_LBA" \
    "$NATIVE_CRASH_RECORD_LBA" "$NATIVE_CRASH_RECORD_SECTORS" \
    "$NATIVE_PFS_START_LBA" "$NATIVE_PFS_SECTORS" "$NATIVE_TOTAL_SECTORS" <<'PY'
import pathlib
import struct
import sys

(image_path, stage1_path, stage2_path, kernel_path, stage2_sectors,
 kernel_lba, crash_lba, crash_sectors, pfs_lba, pfs_sectors,
 total_sectors) = sys.argv[1:]
stage2_sectors = int(stage2_sectors)
kernel_lba = int(kernel_lba)
crash_lba = int(crash_lba)
crash_sectors = int(crash_sectors)
pfs_lba = int(pfs_lba)
pfs_sectors = int(pfs_sectors)
total_sectors = int(total_sectors)

image = pathlib.Path(image_path).read_bytes()
stage1 = pathlib.Path(stage1_path).read_bytes()
stage2 = pathlib.Path(stage2_path).read_bytes()
kernel = pathlib.Path(kernel_path).read_bytes()

def require(condition, message):
    if not condition:
        raise SystemExit("Native disk image verification failed: " + message)

require(len(image) == total_sectors * 512, "unexpected total image size")
require(len(image) % 512 == 0, "image is not sector aligned")
require(len(stage1) == 512, "stage-1 is not one sector")
require(image[:512] == stage1, "stage-1 readback mismatch")
require(image[510:512] == b"\x55\xaa", "missing BIOS boot signature")
require(len(stage2) == stage2_sectors * 512, "stage-2 size does not match configured sectors")
require(image[512:(1 + stage2_sectors) * 512] == stage2, "stage-2 readback mismatch")
kernel_start = kernel_lba * 512
kernel_end = kernel_start + len(kernel)
require(kernel_end <= len(image), "kernel payload exceeds image")
require(image[kernel_start:kernel_end] == kernel, "kernel payload readback mismatch")
kernel_sectors = (len(kernel) + 511) // 512
crash_end_lba = crash_lba + crash_sectors
pfs_end_lba = pfs_lba + pfs_sectors

# Treat every occupied extent as a half-open LBA interval and reject any
# overlap, including the reserved crash-record sectors.
def overlaps(left, right):
    return left[0] < right[1] and right[0] < left[1]

extents = [
    ("stage-1", (0, 1)),
    ("stage-2", (1, 1 + stage2_sectors)),
    ("kernel", (kernel_lba, kernel_lba + kernel_sectors)),
    ("crash-record reservation", (crash_lba, crash_end_lba)),
    ("ProsenjitFS", (pfs_lba, pfs_end_lba)),
]
for index, (left_name, left_range) in enumerate(extents):
    for right_name, right_range in extents[index + 1:]:
        require(not overlaps(left_range, right_range),
                f"{left_name} overlaps {right_name}")

# Exercise the overlap predicate against the exact boundary cases this ABI
# must protect: a kernel entering the reservation and a reservation entering
# the filesystem are rejected; adjacent extents remain valid.
require(overlaps((25, 2047), (2046, 2048)),
        "overlap predicate missed a kernel/crash collision")
require(overlaps((2046, 2048), (2047, 2560)),
        "overlap predicate missed a crash/PFS collision")
require(not overlaps((25, 2046), (2046, 2048)),
        "overlap predicate rejected adjacent kernel/crash extents")
require(not overlaps((2046, 2048), (2048, 2560)),
        "overlap predicate rejected adjacent crash/PFS extents")
require(crash_lba == 2046 and crash_sectors == 2,
        "unexpected native crash-record ABI extent")
require(crash_end_lba <= pfs_lba,
        "crash-record reservation overlaps the ProsenjitFS partition")
require(crash_end_lba * 512 <= len(image),
        "crash-record reservation extends beyond the image")
crash_start = crash_lba * 512
crash_end = crash_end_lba * 512
require(image[crash_start:crash_end] == bytes(crash_sectors * 512),
        "reserved crash-record sectors are not initially zero")
partition = image[446:462]
require(partition[0] == 0x80, "PFS partition is not marked active")
require(partition[4] == 0xDA, "unexpected PFS partition type")
require(struct.unpack_from("<I", partition, 8)[0] == pfs_lba,
        "PFS partition start LBA mismatch")
require(struct.unpack_from("<I", partition, 12)[0] == pfs_sectors,
        "PFS partition size mismatch")
require(pfs_end_lba <= total_sectors,
        "PFS partition extends beyond the image")
print("Native BIOS disk image layout/readback verification passed.")
PY

# Format only this freshly-created disposable test image with the kernel's
# own ProsenjitFS formatter. The running OS never auto-formats real disks.
gcc -std=c11 -D_POSIX_C_SOURCE=200809L -O2 -Wall -Wextra -Werror \
    -ffunction-sections -fdata-sections -fno-builtin \
    -I"$ROOT/kernel/include" -I"$ROOT/include" \
    "$ROOT/tools/mkprosenjitfs.c" "$ROOT/kernel/prosenjitfs.c" \
    -Wl,--gc-sections -o "$BUILD/mkprosenjitfs"
"$BUILD/mkprosenjitfs" "$NATIVE_DISK"
# The formatter must operate strictly inside the PFS partition and preserve
# the separately reserved crash-record sectors byte-for-byte (still zero).
python3 - "$NATIVE_DISK" "$NATIVE_CRASH_RECORD_LBA" "$NATIVE_CRASH_RECORD_SECTORS" <<'PY'
import pathlib
import sys

image = pathlib.Path(sys.argv[1]).read_bytes()
start = int(sys.argv[2]) * 512
size = int(sys.argv[3]) * 512
if image[start:start + size] != bytes(size):
    raise SystemExit("ProsenjitFS formatter modified reserved crash-record sectors")
print("Crash-record reservation preserved by ProsenjitFS formatting.")
PY

# Refuse special files and malformed regular files before any sector writes.
# These tests use /dev/null and disposable files created inside BUILD only.
NATIVE_DISK_SHA_BEFORE="$(sha256sum "$NATIVE_DISK" | awk '{print $1}')"
if "$BUILD/mkprosenjitfs" /dev/null >/dev/null 2>&1; then
    echo "Formatter unexpectedly accepted a non-regular target" >&2
    exit 1
fi
ln -s "$NATIVE_DISK" "$BUILD/native-disk-image-symlink.img"
if "$BUILD/mkprosenjitfs" "$BUILD/native-disk-image-symlink.img" >/dev/null 2>&1; then
    echo "Formatter unexpectedly followed a symlink target" >&2
    exit 1
fi
mkdir "$BUILD/native-disk-image-directory"
if "$BUILD/mkprosenjitfs" "$BUILD/native-disk-image-directory" >/dev/null 2>&1; then
    echo "Formatter unexpectedly accepted a directory target" >&2
    exit 1
fi
mkfifo "$BUILD/native-disk-image-fifo"
if "$BUILD/mkprosenjitfs" "$BUILD/native-disk-image-fifo" >/dev/null 2>&1; then
    echo "Formatter unexpectedly accepted a FIFO target" >&2
    exit 1
fi
# Regular-file type alone is insufficient: require the expected MBR/partition.
truncate -s "$(stat -c%s "$NATIVE_DISK")" "$BUILD/invalid-regular-disk.img"
INVALID_IMAGE_SHA_BEFORE="$(sha256sum "$BUILD/invalid-regular-disk.img" | awk '{print $1}')"
if "$BUILD/mkprosenjitfs" "$BUILD/invalid-regular-disk.img" >/dev/null 2>&1; then
    echo "Formatter unexpectedly accepted a regular file without the expected MBR" >&2
    exit 1
fi
INVALID_IMAGE_SHA_AFTER="$(sha256sum "$BUILD/invalid-regular-disk.img" | awk '{print $1}')"
if [[ "$INVALID_IMAGE_SHA_BEFORE" != "$INVALID_IMAGE_SHA_AFTER" ]]; then
    echo "Formatter modified a regular file rejected for invalid disk layout" >&2
    exit 1
fi

# Preserve the valid PFS entry but add a second partition that overlaps it.
cp "$NATIVE_DISK" "$BUILD/overlapping-partition-disk.img"
python3 - "$BUILD/overlapping-partition-disk.img" "$NATIVE_PFS_START_LBA" "$NATIVE_PFS_SECTORS" <<'PY'
import struct
import sys

path = sys.argv[1]
pfs_start = int(sys.argv[2])
pfs_sectors = int(sys.argv[3])
with open(path, "r+b") as image:
    mbr = bytearray(image.read(512))
    entry = 446 + 16
    mbr[entry] = 0x00
    mbr[entry + 4] = 0x83
    struct.pack_into("<II", mbr, entry + 8, pfs_start + 1,
                     pfs_sectors)
    image.seek(0)
    image.write(mbr)
PY
OVERLAP_SHA_BEFORE="$(sha256sum "$BUILD/overlapping-partition-disk.img" | awk '{print $1}')"
if "$BUILD/mkprosenjitfs" "$BUILD/overlapping-partition-disk.img" >/dev/null 2>&1; then
    echo "Formatter unexpectedly accepted an overlapping MBR partition" >&2
    exit 1
fi
OVERLAP_SHA_AFTER="$(sha256sum "$BUILD/overlapping-partition-disk.img" | awk '{print $1}')"
if [[ "$OVERLAP_SHA_BEFORE" != "$OVERLAP_SHA_AFTER" ]]; then
    echo "Formatter modified an image rejected for overlapping partitions" >&2
    exit 1
fi

# Exercise additional malformed MBR layouts independently. Every rejected
# regular-file fixture must remain byte-for-byte unchanged.
create_bad_mbr_fixture() {
    local name="$1"
    local mode="$2"
    local fixture="$BUILD/$name"
    cp "$NATIVE_DISK" "$fixture"
    python3 - "$fixture" "$mode" "$NATIVE_PFS_START_LBA" "$NATIVE_PFS_SECTORS" <<'PY'
import os
import struct
import sys

path, mode = sys.argv[1], sys.argv[2]
pfs_start, pfs_sectors = int(sys.argv[3]), int(sys.argv[4])
with open(path, "r+b") as image:
    mbr = bytearray(image.read(512))
    total_sectors = os.fstat(image.fileno()).st_size // 512

    def entry(index):
        return 446 + index * 16

    def set_partition(index, status, kind, start, count):
        off = entry(index)
        mbr[off] = status
        mbr[off + 4] = kind
        struct.pack_into("<II", mbr, off + 8, start, count)

    if mode == "duplicate-pfs":
        set_partition(1, 0, 0xDA, pfs_start, pfs_sectors)
    elif mode == "wrong-pfs-start":
        set_partition(0, 0, 0xDA, pfs_start + 1, pfs_sectors)
    elif mode == "wrong-pfs-size":
        set_partition(0, 0, 0xDA, pfs_start, pfs_sectors - 1)
    elif mode == "invalid-status":
        set_partition(1, 0x7F, 0x83, pfs_start + pfs_sectors + 8, 8)
    elif mode == "partition-out-of-image":
        set_partition(1, 0, 0x83, total_sectors - 1, 2)
    else:
        raise SystemExit("unknown malformed-MBR fixture mode: " + mode)

    image.seek(0)
    image.write(mbr)
PY
    local before after
    before="$(sha256sum "$fixture" | awk '{print $1}')"
    if "$BUILD/mkprosenjitfs" "$fixture" >/dev/null 2>&1; then
        echo "Formatter unexpectedly accepted malformed MBR fixture: $mode" >&2
        exit 1
    fi
    after="$(sha256sum "$fixture" | awk '{print $1}')"
    if [[ "$before" != "$after" ]]; then
        echo "Formatter modified rejected malformed MBR fixture: $mode" >&2
        exit 1
    fi
}

create_bad_mbr_fixture "duplicate-pfs-partition.img" "duplicate-pfs"
create_bad_mbr_fixture "wrong-pfs-start.img" "wrong-pfs-start"
create_bad_mbr_fixture "wrong-pfs-size.img" "wrong-pfs-size"
create_bad_mbr_fixture "invalid-mbr-status.img" "invalid-status"
create_bad_mbr_fixture "out-of-image-partition.img" "partition-out-of-image"

NATIVE_DISK_SHA_AFTER="$(sha256sum "$NATIVE_DISK" | awk '{print $1}')"
if [[ "$NATIVE_DISK_SHA_BEFORE" != "$NATIVE_DISK_SHA_AFTER" ]]; then
    echo "Formatter refusal tests unexpectedly modified the valid disk image" >&2
    exit 1
fi

echo "Built native BIOS disk: $NATIVE_DISK"
echo "Native kernel sectors: $NATIVE_KERNEL_SECTORS"
echo "Native crash-record reservation: LBA $NATIVE_CRASH_RECORD_LBA + $NATIVE_CRASH_RECORD_SECTORS sectors (reserved, not yet attached)"
echo "Native ProsenjitFS partition: LBA $NATIVE_PFS_START_LBA + $NATIVE_PFS_SECTORS sectors"
fi
