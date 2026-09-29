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
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/security_provider.c" -o "$BUILD/obj/security_provider.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/security_admission.c" -o "$BUILD/obj/security_admission.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/security_quarantine.c" -o "$BUILD/obj/security_quarantine.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/security_exec.c" -o "$BUILD/obj/security_exec.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/pic.c" -o "$BUILD/obj/pic.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/block.c" -o "$BUILD/obj/block.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/vfs.c" -o "$BUILD/obj/vfs.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/fat32.c" -o "$BUILD/obj/fat32.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/exfat.c" -o "$BUILD/obj/exfat.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/ntfs.c" -o "$BUILD/obj/ntfs.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/partition.c" -o "$BUILD/obj/partition.o"
gcc "${CFLAGS[@]}" -c "$ROOT/kernel/pci.c" -o "$BUILD/obj/pci.o"

ld -m elf_x86_64 -T "$ROOT/kernel/linker.ld" -nostdlib -o "$BUILD/prosenjit.elf"   "$BUILD/obj/multiboot2_header.o" "$BUILD/obj/entry.o" "$BUILD/obj/idt.o"   "$BUILD/obj/context_switch.o" "$BUILD/obj/kernel.o" "$BUILD/obj/memory.o"   "$BUILD/obj/paging.o" "$BUILD/obj/heap.o" "$BUILD/obj/address_space.o"   "$BUILD/obj/syscall.o" "$BUILD/obj/object.o" "$BUILD/obj/usercopy.o"   "$BUILD/obj/usercopy_asm.o" "$BUILD/obj/ipc.o" "$BUILD/obj/process.o"   "$BUILD/obj/thread.o" "$BUILD/obj/timer.o" "$BUILD/obj/intent.o"   "$BUILD/obj/security_file.o" "$BUILD/obj/security_provider.o" "$BUILD/obj/security_admission.o"   "$BUILD/obj/security_quarantine.o" "$BUILD/obj/security_exec.o" "$BUILD/obj/pic.o"   "$BUILD/obj/block.o" "$BUILD/obj/vfs.o" "$BUILD/obj/fat32.o" "$BUILD/obj/exfat.o" "$BUILD/obj/ntfs.o" "$BUILD/obj/partition.o" "$BUILD/obj/pci.o"

grub-file --is-x86-multiboot2 "$BUILD/prosenjit.elf"
cp "$BUILD/prosenjit.elf" "$BUILD/isodir/boot/prosenjit.elf"
cp "$ROOT/boot/grub.cfg" "$BUILD/isodir/boot/grub/grub.cfg"
grub-mkrescue -o "$ISO" "$BUILD/isodir" >/dev/null

echo "Built: $ISO"
