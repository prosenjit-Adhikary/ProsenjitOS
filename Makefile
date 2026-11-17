BUILD_DIR := build
CC := gcc
LD := ld
AS := nasm

CFLAGS := -m64 -march=x86-64 -mtune=generic -mno-bmi2 -ffreestanding -fno-pie -fno-stack-protector -mno-red-zone -mno-sse -mno-mmx -Wall -Wextra -Werror -O2
LDFLAGS := -nostdlib -z max-page-size=0x1000

.PHONY: all iso clean run desktop-unit-test security-service-test boot-protocol-abi-test

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

# Host-side tests for the isolated Security Control Center dispatcher.
security-service-test:
	bash scripts/test_security_control_center_service.sh

# Host-side compile-time/layout checks for the versioned boot handoff ABI.
boot-protocol-abi-test:
	bash scripts/test_boot_protocol_abi.sh
