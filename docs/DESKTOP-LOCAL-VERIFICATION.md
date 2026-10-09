# Desktop integration: local verification runbook

This runbook is for the current `desktop-gate-hardening-20261009` branch. It does not mark the desktop complete; it defines the evidence required before changing that status.

## Preconditions

- Use the project's Debian/Linux development environment or the configured self-hosted Linux x86_64 runner.
- Do not switch to GitHub-hosted paid runners merely to clear a queued status.
- Start from a clean working tree and record the exact commit SHA being tested.
- Keep the existing filesystem, bootloader, network, and driver scopes unchanged unless a shared contract is demonstrably required.

## Static checks

From the repository root:

```sh
git status --short
git rev-parse HEAD
bash -n scripts/desktop_complete.sh
bash -n scripts/build.sh
./scripts/desktop_complete.sh
```

The completion script is a source/build/runtime gate, not a substitute for inspecting failures. Save its generated `reports/desktop-integration-report.md` and review every failure and note. A source-grep match alone is not proof that a behavior works.

## Clean build and boot smoke test

Run in the local Debian VM:

```sh
make clean
make iso
timeout 20s qemu-system-x86_64 -cdrom build/prosenjitOS.iso -display none -serial stdio -no-reboot
```

Capture the full output and exit status. A timeout can be expected for a kernel that intentionally remains running, so inspect the serial log for the explicit boot milestones and errors rather than treating timeout alone as pass/fail. Also run the project's established build/boot-test command if it differs from the smoke-test command above.

## Required evidence before desktop completion

- Clean compilation with `-Werror` and no stale build products.
- ELF loader self-tests pass, including empty argv/envp, invalid argv, oversized-argument rollback, and page permission checks.
- Native boot/QEMU serial logs show the current intended test path passes.
- A real filesystem-backed `/init` is loaded through VFS and execution-security admission in a normal process path; the embedded CPL3 test ELF alone does not satisfy this.
- Public file open/read/write and process exec/spawn contracts are implemented, dispatched, and tested with invalid pointers, permission denial, and resource cleanup.
- Userspace PID 1/service startup works without moving desktop components into kernel code.
- A real display backend and keyboard/pointer event path are implemented and exercised; synthetic display tests alone do not satisfy this.
- Xorg/X11 or Wayland and XFCE are packaged/integrated as userspace components and a graphical session reaches a usable desktop.
- Re-run all relevant security, filesystem, and boot regression tests after cross-subsystem changes.

## Reporting rules

For every test, record command, tested commit SHA, exit status, and relevant log excerpt. Report `queued`, `in progress`, and `not run` separately from `passed`. Do not merge the draft PR while any required gate is unverified or failing.
