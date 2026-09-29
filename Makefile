BUILD_DIR := build
CC := gcc
LD := ld
AS := nasm

CFLAGS := -m64 -ffreestanding -fno-pie -fno-stack-protector -mno-red-zone -mno-sse -mno-mmx -Wall -Wextra -Werror -O2
LDFLAGS := -nostdlib -z max-page-size=0x1000

.PHONY: all iso mkprosenjitfs clean run

all: iso

mkprosenjitfs:
	mkdir -p $(BUILD_DIR)
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -Iinclude tools/mkprosenjitfs.c -o $(BUILD_DIR)/mkprosenjitfs

iso:
	chmod +x scripts/build.sh
	./scripts/build.sh

clean:
	rm -rf $(BUILD_DIR)

run: iso
	qemu-system-x86_64 -cdrom $(BUILD_DIR)/prosenjitOS.iso
