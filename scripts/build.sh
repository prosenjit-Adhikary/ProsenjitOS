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

gcc -m64 -ffreestanding -fno-pie -fno-stack-protector -mno-red-zone -mno-sse -mno-mmx     -I"$ROOT/kernel/include" -I"$ROOT/include" -Wall -Wextra -Werror -O2 -c "$ROOT/kernel/kernel.c" -o "$BUILD/obj/kernel.o"
gcc -m64 -ffreestanding -fno-pie -fno-stack-protector -mno-red-zone -mno-sse -mno-mmx     -I"$ROOT/kernel/include" -Wall -Wextra -Werror -O2 -c "$ROOT/kernel/memory.c" -o "$BUILD/obj/memory.o"
gcc -m64 -ffreestanding -fno-pie -fno-stack-protector -mno-red-zone -mno-sse -mno-mmx     -I"$ROOT/kernel/include" -Wall -Wextra -Werror -O2 -c "$ROOT/kernel/paging.c" -o "$BUILD/obj/paging.o"
gcc -m64 -ffreestanding -fno-pie -fno-stack-protector -mno-red-zone -mno-sse -mno-mmx     -I"$ROOT/kernel/include" -Wall -Wextra -Werror -O2 -c "$ROOT/kernel/heap.c" -o "$BUILD/obj/heap.o"
gcc -m64 -ffreestanding -fno-pie -fno-stack-protector -mno-red-zone -mno-sse -mno-mmx     -I"$ROOT/kernel/include" -I"$ROOT/include" -Wall -Wextra -Werror -O2 -c "$ROOT/kernel/address_space.c" -o "$BUILD/obj/address_space.o"
gcc -m64 -ffreestanding -fno-pie -fno-stack-protector -mno-red-zone -mno-sse -mno-mmx     -I"$ROOT/kernel/include" -I"$ROOT/include" -Wall -Wextra -Werror -O2 -c "$ROOT/kernel/syscall.c" -o "$BUILD/obj/syscall.o"
gcc -m64 -ffreestanding -fno-pie -fno-stack-protector -mno-red-zone -mno-sse -mno-mmx     -I"$ROOT/kernel/include" -I"$ROOT/include" -Wall -Wextra -Werror -O2 -c "$ROOT/kernel/object.c" -o "$BUILD/obj/object.o"
gcc -m64 -ffreestanding -fno-pie -fno-stack-protector -mno-red-zone -mno-sse -mno-mmx     -I"$ROOT/kernel/include" -I"$ROOT/include" -Wall -Wextra -Werror -O2 -c "$ROOT/kernel/usercopy.c" -o "$BUILD/obj/usercopy.o"
gcc -m64 -ffreestanding -fno-pie -fno-stack-protector -mno-red-zone -mno-sse -mno-mmx     -I"$ROOT/kernel/include" -I"$ROOT/include" -Wall -Wextra -Werror -O2 -c "$ROOT/kernel/ipc.c" -o "$BUILD/obj/ipc.o"
gcc -m64 -ffreestanding -fno-pie -fno-stack-protector -mno-red-zone -mno-sse -mno-mmx     -I"$ROOT/kernel/include" -I"$ROOT/include" -Wall -Wextra -Werror -O2 -c "$ROOT/kernel/process.c" -o "$BUILD/obj/process.o"
gcc -m64 -ffreestanding -fno-pie -fno-stack-protector -mno-red-zone -mno-sse -mno-mmx \
    -I"$ROOT/kernel/include" -I"$ROOT/include" -Wall -Wextra -Werror -O2 \
    -c -o "$BUILD/obj/thread.o" "$ROOT/kernel/thread.c"
gcc -m64 -ffreestanding -fno-pie -fno-stack-protector -mno-red-zone -mno-sse -mno-mmx     -I"$ROOT/kernel/include" -I"$ROOT/include" -Wall -Wextra -Werror -O2 -c "$ROOT/kernel/timer.c" -o "$BUILD/obj/timer.o"
gcc -m64 -ffreestanding -fno-pie -fno-stack-protector -mno-red-zone -mno-sse -mno-mmx     -I"$ROOT/kernel/include" -I"$ROOT/include" -Wall -Wextra -Werror -O2 -c "$ROOT/kernel/intent.c" -o "$BUILD/obj/intent.o"
gcc -m64 -ffreestanding -fno-pie -fno-stack-protector -mno-red-zone -mno-sse -mno-mmx -I"$ROOT/kernel/include" -I"$ROOT/include" -Wall -Wextra -Werror -O2 -c "$ROOT/kernel/pic.c" -o "$BUILD/obj/pic.o"

ld -m elf_x86_64 -T "$ROOT/kernel/linker.ld" -nostdlib    -o "$BUILD/prosenjit.elf"    "$BUILD/obj/multiboot2_header.o" "$BUILD/obj/entry.o" "$BUILD/obj/idt.o" "$BUILD/obj/context_switch.o" "$BUILD/obj/kernel.o" "$BUILD/obj/memory.o" "$BUILD/obj/paging.o" "$BUILD/obj/heap.o" "$BUILD/obj/address_space.o" "$BUILD/obj/syscall.o" "$BUILD/obj/object.o" "$BUILD/obj/usercopy.o" "$BUILD/obj/usercopy_asm.o" "$BUILD/obj/ipc.o" "$BUILD/obj/process.o" "$BUILD/obj/thread.o" "$BUILD/obj/timer.o" "$BUILD/obj/intent.o" "$BUILD/obj/pic.o"

grub-file --is-x86-multiboot2 "$BUILD/prosenjit.elf"
cp "$BUILD/prosenjit.elf" "$BUILD/isodir/boot/prosenjit.elf"
cp "$ROOT/boot/grub.cfg" "$BUILD/isodir/boot/grub/grub.cfg"
grub-mkrescue -o "$ISO" "$BUILD/isodir" >/dev/null

echo "Built: $ISO"
