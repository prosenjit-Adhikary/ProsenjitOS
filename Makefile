BUILD_DIR := build
CC := gcc
LD := ld
AS := nasm

CFLAGS := -m64 -ffreestanding -fno-pie -fno-stack-protector -mno-red-zone -Wall -Wextra -O2
LDFLAGS := -nostdlib -z max-page-size=0x1000

.PHONY: all clean run

all:
	@echo "ProsenjitOS: toolchain skeleton created."
	@echo "Bootable image build will be enabled in Phase 1."

clean:
	rm -rf $(BUILD_DIR)

run:
	@echo "Run target is reserved for the bootable image milestone."
