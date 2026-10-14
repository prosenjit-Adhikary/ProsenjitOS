# ProsenjitOS Desktop Implementation Plan and Handoff

## Architecture boundary

The supported target remains:

Firmware -> bootloader -> kernel/memory/security/process/syscall -> ELF loader
-> userspace init/service manager -> userspace graphics/session service
-> existing Xorg/X11 or Wayland -> XFCE -> applications.

Xorg, Wayland, XFCE, shells, file managers, terminal emulators and application
policy must remain in userspace. No custom X server is planned.

## Current source status (2026-10-14)

Status labels below distinguish source changes from runtime evidence.

### IMPLEMENTED — source code exists

- Kernel ELF loader and filesystem-oriented init loader entry point exist:
  `kernel/elf_loader.c`, `kernel/init.c`, `kernel/include/init.h`.
- Kernel service lifecycle API exists in `kernel/service_manager.c` and
  `kernel/include/service_manager.h`; it provides bounded registration,
  dependency startup, retry limits, lifecycle states and a self-test.
- Graphics/display/compositor/input primitives and a PS/2 input-poll path exist
  across the display, graphics/compositor, input/syscall ABI and boot-test code
  represented by the open desktop PRs. Their presence on a PR branch does not
  mean they are integrated into `main`.
- The normal kernel boot path invokes the service-manager lifecycle self-test once and resets the registry afterward, so synthetic test callbacks do not leak into later service registration. A source cross-check found a second invocation immediately before CPL3 handoff; that duplicate was removed and replaced with an explicit registry reset, because the self-test intentionally leaves its synthetic registrations in place on success. This is a kernel self-test integration only; it does not launch userspace services.
- This branch hardens service registration so the manager owns bounded copies of
  registered service names and dependency names, rather than retaining pointers
  to caller-owned (potentially stack-lifetime) strings. It rejects duplicate
  dependencies, bounds name comparisons to the fixed limit, and tests an
  unterminated lookup buffer. The service-manager self-test rejects duplicate dependencies
  and mutates caller-owned service/dependency name buffers after registration to
  verify the manager's copied names remain usable.
- `kernel/init.c` now clears the ELF load result before attempting a load, initializes local file/admission records, distinguishes a missing `/init` from other open failures, and maps ELF bounds/overflow and permission/conflict failures to explicit init errors. This reduces stale-output and diagnostic ambiguity on failure; it does not connect the helper to normal boot. The init-loader self-test now seeds a nonzero result and checks that an inactive-mount failure clears all result fields.

### SOURCE-REVIEWED — not runtime tested

- On `main` at start of this work, `kernel/init.c` validates a supplied mount
  is active, opens `/init`, checks vnode type and image size, reads the complete
  image, invokes VFS executable admission and security execution admission,
  validates ELF, then loads it. This helper is not proof that boot discovers,
  mounts and uses a real root filesystem.
- `kernel/service_manager.c` has bounded service count/dependency/retry limits
  and detects dependency cycles while starting. Its callbacks are kernel-side
  lifecycle hooks; they are not a userspace PID 1, process-spawn facility or
  graphical-session manager.
- The repository's `main` branch and open desktop PRs were inspected through
  repository metadata and file contents. Open PRs are not silently merged here.

### RUNTIME-UNVERIFIED — awaiting local Debian/QEMU

- No compiler, host test, ISO build, native BIOS image build, QEMU boot, hardware
  input or framebuffer-pixel test was run for this change.
- Do not treat GitHub-hosted Actions as required verification; prefer the local
  Debian environment/self-hosted runner when available.

### BLOCKED — concrete missing integration

- Normal boot still uses an embedded CPL3 test ELF; the real
  device -> partition -> mounted ProsenjitFS -> admitted `/init` path is not
  established as the normal boot path. Storage discovery, a valid formatted
  root volume and mount activation must be connected and verified before the
  embedded test can be retired.
- There is no demonstrated public userspace file-open/read ABI plus process
  exec/spawn lifecycle sufficient to launch arbitrary user services.
- The kernel service manager does not provide a userspace PID 1 or capability-
  authorized userspace service IPC/session orchestration.
- Graphics/compositor primitives do not by themselves create a userspace display
  server. A validated userspace display-buffer/event ABI and process/IPC path
  remain prerequisites for a real session service.
- PS/2 keyboard polling is not mouse support, IRQ-driven input, keymap/text
  translation or proof of physical hardware coverage.
- Xorg/Wayland, their runtime libraries/ABI and XFCE are not integrated into a
  bootable ProsenjitOS userspace. No desktop stack is claimed to run.

## Remaining gates

1. Connect normal boot to a real supported root mount and filesystem-backed
   `/init`, retaining the embedded CPL3 program only as a named test path.
2. Define and implement the minimum safe userspace process-launch and exit
   contract supported by the actual syscall/address-space implementation.
3. Add a userspace init/session supervisor with bounded service metadata,
   dependency ordering, explicit failures and resource cleanup. Do not put
   desktop policy into the kernel service manager.
4. Implement the userspace graphics/session service over validated display,
   input and IPC contracts; keep device access behind kernel boundaries.
5. Bring up an existing Xorg/X11 or Wayland stack only after its ABI, runtime,
   libraries and process model are available; then evaluate XFCE.
6. Add source-only regression coverage where it can execute without privileged
   I/O, followed by local warnings-as-errors builds, both boot paths and real
   input/display interaction.

## Local verification handoff

Run from the repository root on Debian when it is online:

```sh
git status --short
git diff --check
./scripts/test_input_host.sh
make clean
make iso
```

Inspect the actual `scripts/` directory and `Makefile` before running targets
that may differ from this list. If the native BIOS image target is available,
run its documented build command separately; do not infer it from ISO success.

For QEMU, use the repository's documented ISO and native-BIOS targets, capture
serial output to separate log files, and retain the exact exit status. Check for
the existing self-test markers, including where applicable:

- `ProsenjitOS: keyboard input decoder/ring self-test passed`
- `ProsenjitOS: CPL3 user-mode and IPC syscall test passed`

A missing marker, timeout, reset/triple-fault, build warning/error, or nonzero
test result is a failure requiring investigation. These markers prove only the
specific tests that emit them; they do not prove a working graphical desktop.
Also inspect the boot log for root-mount/`/init` admission diagnostics, and
test actual framebuffer output and physical keyboard/mouse separately. Do not
claim runtime success from source symbols or script-generated reports.

## Verification policy

Documentation is not implementation. A component is complete only when its
code is integrated and its required checks have evidence. Preserve unrelated
kernel/filesystem/network/driver work, do not weaken warnings-as-errors, do not
depend on paid GitHub-hosted runners, and do not merge this work automatically.
