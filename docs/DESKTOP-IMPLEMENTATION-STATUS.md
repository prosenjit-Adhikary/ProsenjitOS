# ProsenjitOS Desktop Integration Status

Audit date: 2026-10-13  
Base inspected: `main` at `496a084a526607c020804783a2b152a4f0c8700a`  
Working branch: `work/desktop-integration-gate-fix`

## Findings

### Implemented foundations (not a completed desktop)

- `kernel/display.c` provides a driver-bound display backend contract, information validation, presentation dispatch, and a mock-backend self-test.
- `kernel/include/display.h`, `include/abi/display.h`, and `include/abi/input.h` define display/input interface structures.
- `kernel/elf_loader.c`, process/address-space/syscall sources, `kernel/init.c`, and `kernel/service_manager.c` provide kernel-side foundations.
- The desktop architecture plan correctly keeps XFCE, Xorg/X11, Wayland, the shell, and applications in userspace.

### Blocking gaps

- `user/` contains a README but no actual C/assembly userspace program or desktop/session launcher source.
- `kernel/input.c` now polls the legacy PS/2 controller with a bounded budget, decodes set-1 keyboard make/break events, queues them in a bounded FIFO, and exposes one event through permission-checked `PROSENJIT_SYS_INPUT_POLL` with user-buffer validation/copy. The boot self-test covers scan-code decoding and FIFO ordering. This is keyboard-only polling, not mouse support or a complete desktop input stack.
- The display backend's mock self-test does not establish a real hardware/framebuffer backend or a userspace graphics interface.
- A kernel ELF loader and a kernel service manager do not establish a booted userspace init process or a running service/session lifecycle.
- No working Xorg/Wayland port, XFCE session, required POSIX/system-call/library compatibility, or GUI application launch has been demonstrated.
- A successful kernel ISO/QEMU boot is only a prerequisite test, not a desktop completion test.

## Changes in this branch

- Hardened `scripts/desktop_complete.sh` so it checks for real userspace sources, display/input dispatch, session-launcher sources, and window-system integration evidence instead of treating kernel-side symbols as proof of a desktop.
- Corrected the QEMU smoke-test interpretation: a 20-second timeout can be normal for a kernel that keeps running; required serial boot markers must still be present.
- Added `desktop-input-validation.yml` to attempt the same build and both QEMU boot paths on `ubuntu-latest`, with serial-log assertions and artifact upload. Its jobs currently cannot obtain a runner in this repository context.
- Added workflow concurrency groups so superseded queued runs are cancelled instead of accumulating indefinitely.
- Added `scripts/test_input_host.sh`, a host-side warnings-as-errors compile/run test for the actual kernel input module. It checks event ABI layout with compile-time assertions and exercises scan-code decoding, Pause-sequence handling, FIFO ordering, capacity, and overflow without executing privileged I/O instructions. The hosted CI workflow now runs this test before the full kernel build.
- Hardened `PROSENJIT_SYS_INPUT_POLL` to require a CPL3-origin interrupt frame before any PS/2 I/O and added a dispatcher regression check that expects CPL0 calls to return `EPERM`.
- Expanded build-tool preflight and made the generated report explicitly distinguish kernel boot health from desktop completion.

## Verification state

- Source inspection completed for the desktop plan, user-mode README, display ABI/backend, input ABI, init/service manager, Makefile, build script, and build workflow.
- The new input module was separately compiled with `-Wall -Wextra -Werror` in an isolated local harness, and its decoder/FIFO self-test returned `0` (pass). This does not replace compiling the exact integrated kernel image or booting it under QEMU.
- The desktop gate script and kernel input changes are committed on this branch, but they have **not** been executed on the user's Debian host in this session.
- The latest self-hosted build/security workflow runs remain queued. The new GitHub-hosted validation jobs fail before any step starts; the job API reports `runner_id: 0`, an empty runner name, and no steps. This is runner/workflow execution infrastructure failure, not evidence of a source compilation failure. No build or QEMU test actually ran, so no build, keyboard runtime, display, or GUI pass is claimed.

## Required next implementation milestones

1. Implement and test a real kernel display device/backend and bounded userspace presentation interface.
2. Extend input from legacy PS/2 keyboard polling to robust IRQ-driven keyboard and mouse support, then add a real userspace consumer.
3. Complete executable userspace startup and service lifecycle, including a minimal test program that can run from the filesystem.
4. Add the OS compatibility prerequisites and build/port path for an existing userspace window system.
5. Bring up XFCE and verify input, rendering, app launch, session shutdown/restart, and QEMU/local regression.
6. Update this log with command output and test evidence at each milestone. Do not mark the desktop complete based on documentation, symbol searches, mock backends, or kernel boot alone.
