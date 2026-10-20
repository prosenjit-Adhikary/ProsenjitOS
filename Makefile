BUILD_DIR := build
CC := gcc
LD := ld
AS := nasm

CFLAGS := -m64 -ffreestanding -fno-pie -fno-stack-protector -mno-red-zone -mno-sse -mno-mmx -Wall -Wextra -Werror -O2
LDFLAGS := -nostdlib -z max-page-size=0x1000

.PHONY: all iso clean run desktop-unit-test paint-unit-test

all: iso

iso:
	chmod +x scripts/build.sh
	./scripts/build.sh

clean:
	rm -rf $(BUILD_DIR)

run: iso
	qemu-system-x86_64 -cdrom $(BUILD_DIR)/prosenjitOS.iso

# Host-side unit tests for the userspace DML registry; does not boot the OS.
desktop-unit-test:
	bash scripts/test_desktop_manager.sh

# Host-side raster drawing, frontend-controller, and BMP regression tests; no kernel GUI code.
paint-unit-test:
	bash scripts/test_paint_core.sh
