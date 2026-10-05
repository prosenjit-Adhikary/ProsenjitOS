#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
ISO="$BUILD/prosenjitOS.iso"

rm -rf "$BUILD"
mkdir -p "$BUILD/obj" "$BUILD/isodir/boot/grub"
nasm -f elf64 "$ROOT/boot/multiboot2_header.asm" -o "$BUILD/obj/multiboot2_header.o"
nasm -f elf64 "$ROOT/boot/entry.asm" -o "$BUILD/obj/entry.o"
nasm -f elf64 "$ROOT/kernel/usercopy.asm" -o "$BUILD/obj/usercopy_asm.o"
nasm -f elf64 "$ROOT/kernel/idt.asm" -o "$BUILD/obj/idt.o"
nasm -f elf64 "$ROOT/kernel/context_switch.asm" -o "$BUILD/obj/context_switch.o"

CFLAGS=(-m64 -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns -fno-pie -fno-stack-protector -mno-red-zone -mno-sse -mno-mmx -I"$ROOT/kernel/include" -I"$ROOT/include" -Wall -Wextra -Werror -O2)

gcc "${CFLAGS[@]}" -c "$ROOT/kernel/kernel.c" -o "$BUILD/obj/kernel.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/memory.c" -o "$BUILD/obj/memory.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/paging.c" -o "$BUILD/obj/paging.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/heap.c" -o "$BUILD/obj/heap.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/address_space.c" -o "$BUILD/obj/address_space.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/syscall.c" -o "$BUILD/obj/syscall.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/object.c" -o "$BUILD/obj/object.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/usercopy.c" -o "$BUILD/obj/usercopy.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/ipc.c" -o "$BUILD/obj/ipc.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/process.c" -o "$BUILD/obj/process.o"
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
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/pic.c" -o "$BUILD/obj/pic.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/block.c" -o "$BUILD/obj/block.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/vfs.c" -o "$BUILD/obj/vfs.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/prosenjitfs.c" -o "$BUILD/obj/prosenjitfs.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/partition.c" -o "$BUILD/obj/partition.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/storage.c" -o "$BUILD/obj/storage.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/pci.c" -o "$BUILD/obj/pci.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/driver.c" -o "$BUILD/obj/driver.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/health.c" -o "$BUILD/obj/health.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/crash_record.c" -o "$BUILD/obj/crash_record.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/log.c" -o "$BUILD/obj/log.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/log_persist.c" -o "$BUILD/obj/log_persist.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/driver_class.c" -o "$BUILD/obj/driver_class.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/compositor.c" -o "$BUILD/obj/compositor.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/driver_bundle.c" -o "$BUILD/obj/driver_bundle.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/device_broker.c" -o "$BUILD/obj/device_broker.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/driver_compat.c" -o "$BUILD/obj/driver_compat.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/nt_compat.c" -o "$BUILD/obj/nt_compat.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/pe_inspect.c" -o "$BUILD/obj/pe_inspect.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/ata_pio.c" -o "$BUILD/obj/ata_pio.o"

ld -m elf_x86_64 -T "$ROOT/kernel/linker.ld" -nostdlib -o "$BUILD/prosenjit.elf"   "$BUILD/obj/multiboot2_header.o" "$BUILD/obj/entry.o" "$BUILD/obj/idt.o"   "$BUILD/obj/context_switch.o" "$BUILD/obj/kernel.o" "$BUILD/obj/memory.o"   "$BUILD/obj/paging.o" "$BUILD/obj/heap.o" "$BUILD/obj/address_space.o"   "$BUILD/obj/syscall.o" "$BUILD/obj/object.o" "$BUILD/obj/usercopy.o"   "$BUILD/obj/usercopy_asm.o" "$BUILD/obj/ipc.o" "$BUILD/obj/process.o"   "$BUILD/obj/thread.o" "$BUILD/obj/timer.o" "$BUILD/obj/intent.o"   "$BUILD/obj/security_file.o" "$BUILD/obj/sha512.o" "$BUILD/obj/ed25519_field.o" "$BUILD/obj/ed25519_point.o" "$BUILD/obj/ed25519_scalar.o" "$BUILD/obj/security_provider.o" "$BUILD/obj/security_signature.o" "$BUILD/obj/security_admission.o"   "$BUILD/obj/security_quarantine.o" "$BUILD/obj/security_exec.o" "$BUILD/obj/pic.o"   "$BUILD/obj/block.o" "$BUILD/obj/vfs.o" "$BUILD/obj/prosenjitfs.o" "$BUILD/obj/partition.o" "$BUILD/obj/storage.o" "$BUILD/obj/pci.o" "$BUILD/obj/driver.o" "$BUILD/obj/health.o" "$BUILD/obj/crash_record.o" "$BUILD/obj/log.o" "$BUILD/obj/log_persist.o" "$BUILD/obj/driver_class.o" "$BUILD/obj/compositor.o" "$BUILD/obj/driver_bundle.o" "$BUILD/obj/device_broker.o" "$BUILD/obj/driver_compat.o" "$BUILD/obj/nt_compat.o" "$BUILD/obj/pe_inspect.o" "$BUILD/obj/ata_pio.o"

grub-file --is-x86-multiboot2 "$BUILD/prosenjit.elf"
cp "$BUILD/prosenjit.elf" "$BUILD/isodir/boot/prosenjit.elf"
cp "$ROOT/boot/grub.cfg" "$BUILD/isodir/boot/grub/grub.cfg"
grub-mkrescue -o "$ISO" "$BUILD/isodir" >/dev/null

echo "Built: $ISO"

# Native BIOS boot artifact. This path deliberately does not depend on GRUB,
# MBR partition parsing, or a filesystem: the stage-1/2 loader reads the
# kernel from a fixed LBA layout. Partition/filesystem discovery remains a
# later storage concern.
NATIVE_DISK="$BUILD/prosenjit-native-bios.img"
NATIVE_STAGE2_SECTORS=32
NATIVE_KERNEL_LBA=$((1 + NATIVE_STAGE2_SECTORS))
NATIVE_PFS_START_LBA=2048
NATIVE_PFS_SECTORS=512
objcopy -O binary "$BUILD/prosenjit.elf" "$BUILD/prosenjit.bin"
KERNEL_BYTES="$(stat -c%s "$BUILD/prosenjit.bin")"
NATIVE_KERNEL_SECTORS=$(( (KERNEL_BYTES + 511) / 512 ))
if (( NATIVE_KERNEL_SECTORS == 0 || NATIVE_KERNEL_SECTORS > 1536 )); then
    echo "Native bootloader kernel size is outside the supported 768 KiB staging window" >&2
    exit 1
fi

nasm -f bin -dSTAGE2_SECTORS="$NATIVE_STAGE2_SECTORS" \
    -dPFS_START_LBA="$NATIVE_PFS_START_LBA" -dPFS_SECTORS="$NATIVE_PFS_SECTORS" \
    "$ROOT/boot/native_stage1.asm" -o "$BUILD/native_stage1.bin"
nasm -f bin -dKERNEL_LBA="$NATIVE_KERNEL_LBA" -dKERNEL_SECTORS="$NATIVE_KERNEL_SECTORS" \
    "$ROOT/boot/native_stage2.asm" -o "$BUILD/native_stage2.bin"

if (( $(stat -c%s "$BUILD/native_stage1.bin") != 512 )); then
    echo "Native stage-1 is not exactly one boot sector" >&2
    exit 1
fi
if (( $(stat -c%s "$BUILD/native_stage2.bin") > NATIVE_STAGE2_SECTORS * 512 )); then
    echo "Native stage-2 exceeds its reserved boot area" >&2
    exit 1
fi

NATIVE_KERNEL_END=$((NATIVE_KERNEL_LBA + NATIVE_KERNEL_SECTORS))
NATIVE_PFS_END=$((NATIVE_PFS_START_LBA + NATIVE_PFS_SECTORS))
NATIVE_TOTAL_SECTORS=$((NATIVE_KERNEL_END > NATIVE_PFS_END ? NATIVE_KERNEL_END : NATIVE_PFS_END))
truncate -s $((NATIVE_TOTAL_SECTORS * 512)) "$NATIVE_DISK"
dd if="$BUILD/native_stage1.bin" of="$NATIVE_DISK" bs=512 seek=0 conv=notrunc status=none
dd if="$BUILD/native_stage2.bin" of="$NATIVE_DISK" bs=512 seek=1 conv=notrunc status=none
dd if="$BUILD/prosenjit.bin" of="$NATIVE_DISK" bs=512 seek="$NATIVE_KERNEL_LBA" conv=notrunc status=none

echo "Built native BIOS disk: $NATIVE_DISK"
echo "Native kernel sectors: $NATIVE_KERNEL_SECTORS"
echo "Native ProsenjitFS partition: LBA $NATIVE_PFS_START_LBA + $NATIVE_PFS_SECTORS sectors"
