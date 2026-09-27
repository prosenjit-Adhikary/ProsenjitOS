#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
ISO="$BUILD/prosenjitOS.iso"

rm -rf "$BUILD"
mkdir -p "$BUILD/obj" "$BUILD/isodir/boot/grub"

nasm -f elf64 "$ROOT/boot/multiboot2_header.asm" -o "$BUILD/obj/multiboot2_header.o"
nasm -f elf64 "$ROOT/boot/entry.asm" -o "$BUILD/obj/entry.o"
nasm -f elf64 "$ROOT/kernel/idt.asm" -o "$BUILD/obj/idt.o"

gcc -m64 -ffreestanding -fno-pie -fno-stack-protector -mno-red-zone -mno-sse -mno-mmx     -I"$ROOT/kernel/include" -Wall -Wextra -Werror -O2 -c "$ROOT/kernel/kernel.c" -o "$BUILD/obj/kernel.o"
gcc -m64 -ffreestanding -fno-pie -fno-stack-protector -mno-red-zone -mno-sse -mno-mmx     -I"$ROOT/kernel/include" -Wall -Wextra -Werror -O2 -c "$ROOT/kernel/memory.c" -o "$BUILD/obj/memory.o"
gcc -m64 -ffreestanding -fno-pie -fno-stack-protector -mno-red-zone -mno-sse -mno-mmx     -I"$ROOT/kernel/include" -Wall -Wextra -Werror -O2 -c "$ROOT/kernel/paging.c" -o "$BUILD/obj/paging.o"

ld -m elf_x86_64 -T "$ROOT/kernel/linker.ld" -nostdlib    -o "$BUILD/prosenjit.elf"    "$BUILD/obj/multiboot2_header.o" "$BUILD/obj/entry.o" "$BUILD/obj/idt.o" "$BUILD/obj/kernel.o" "$BUILD/obj/memory.o" "$BUILD/obj/paging.o"

grub-file --is-x86-multiboot2 "$BUILD/prosenjit.elf"
cp "$BUILD/prosenjit.elf" "$BUILD/isodir/boot/prosenjit.elf"
cp "$ROOT/boot/grub.cfg" "$BUILD/isodir/boot/grub/grub.cfg"
grub-mkrescue -o "$ISO" "$BUILD/isodir" >/dev/null

echo "Built: $ISO"
