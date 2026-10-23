BUILD_DIR := build
CC := gcc
LD := ld
AS := nasm

CFLAGS := -m64 -ffreestanding -fno-pie -fno-stack-protector -mno-red-zone -mno-sse -mno-mmx -Wall -Wextra -Werror -O2
LDFLAGS := -nostdlib -z max-page-size=0x1000

.PHONY: all iso clean run desktop-unit-test desktop-abi-test desktop-tests hw-stack-test ps2-keyboard-test ps2-mouse-test input-service-test keymap-test pci-test

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

# Host-side tests for the versioned shared desktop message contract.
desktop-abi-test:
	bash scripts/test_desktop_abi.sh

# Run all isolated desktop contract suites; neither target boots the OS.
desktop-tests: desktop-unit-test desktop-abi-test

# Host-side tests for native device discovery and endpoint lifecycle contracts.
hw-stack-test:
	bash scripts/test_hw_stack.sh

# Host-side tests for legacy PS/2 set-1 scan-code decoding.
ps2-keyboard-test:
	bash scripts/test_ps2_keyboard.sh

# Hosted tests for standard three-byte PS/2 mouse movement packet decoding.
# This validates a pure decoder only; it does not enable a physical mouse.
ps2-mouse-test:
	bash scripts/test_ps2_mouse.sh

# Hosted test for the kernel input queue and PS/2 endpoint delivery.
input-service-test:
	bash scripts/test_input_service.sh

# Host-side test for the user-space US keyboard layout translator.
keymap-test:
	bash scripts/test_keymap.sh

# Hosted tests for read-only PCI BAR decoding and malformed descriptors.
pci-test:
	bash scripts/test_pci.sh
