BUILD_DIR := build
CC := gcc
LD := ld
AS := nasm

CFLAGS := -m64 -ffreestanding -fno-pie -fno-stack-protector -mno-red-zone -mno-sse -mno-mmx -Wall -Wextra -Werror -O2
LDFLAGS := -nostdlib -z max-page-size=0x1000

.PHONY: all iso clean run desktop-unit-test security-protocol-test security-service-test driver-telemetry-test

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

# Host-side parser tests for the read-only Security Control Center wire proposal.
security-protocol-test:
	bash scripts/test_security_control_center_protocol.sh

# Host-side guard for internal driver telemetry code namespaces.
driver-telemetry-test:
	@tmp=$$(mktemp); trap 'rm -f "$$tmp"' EXIT; \
	$(CC) -std=c11 -Wall -Wextra -Werror -pedantic -Ikernel/include \
	  tests/driver_telemetry_codes_test.c -o "$$tmp"; \
	"$$tmp"

# Host-side dispatcher tests; sanitizers exercise bounded response construction.
security-service-test:
	bash scripts/test_security_control_center_service.sh
