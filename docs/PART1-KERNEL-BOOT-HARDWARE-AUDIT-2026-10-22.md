# Part 1 — Kernel, Boot & Hardware Foundation Audit

Date: 2026-10-22
Audit base: `cf67eea520ae9ff4d48b0ce5c62f8d5a847ca519` (reported latest repository commit at audit start)
Audit branch: `audit/part1-kernel-boot-foundation-20261022`

## Scope and evidence boundary

This is a source-backed remote audit record, not a claim that a local compiler, self-hosted runner, or QEMU guest was executed in this session. Repository files and open PR metadata were inspected through GitHub. No shell access to the user's Debian runner was available here, so no build/test exit code or fresh boot log is claimed. The report is intentionally docs-only until the current kernel boot failure can be reproduced on the exact head.

## Findings

| Area | Status | Evidence / next action |
|---|---|---|
| Repository baseline | COMPLETE (inspection) | Audit base SHA recorded above; default branch is `main`. |
| Existing architecture docs | PARTIAL | Read `README.md`, `docs/ROADMAP.md`, `docs/DEBUG_LOG.md`, and `docs/REGRESSION-MATRIX.md`. These document x86-64 boot, memory, syscall/process/security foundations, native BIOS loader, and QEMU regression gates. |
| Boot protocol and loader | PARTIAL | `boot/entry.asm` is referenced by the debug log for early boot/security labels; `scripts/build.sh` is invoked by the top-level `Makefile`. Roadmap marks native legacy BIOS loader hardening and its QEMU regression implemented, but stable native boot protocol, native UEFI loader, boot-device discovery and kernel/module validation remain unchecked. Exact boot-source inventory still needs full tree traversal. |
| Link/build configuration | PARTIAL | `Makefile` uses `gcc`, `ld`, `nasm`; C flags include `-m64 -ffreestanding -fno-pie -fno-stack-protector -mno-red-zone -mno-sse -mno-mmx -Wall -Wextra -Werror -O2`; link flags include `-nostdlib -z max-page-size=0x1000`. `make iso` calls `scripts/build.sh`. Build not executed in this session. |
| Physical memory | PARTIAL | `kernel/memory.c`: `physical_memory_init`, `physical_page_alloc`, `physical_page_alloc_user`, `physical_page_is_allocated`, `physical_page_is_user_mappable`, `physical_page_free`. Multiboot2 mmap parser checks overall size, tag bounds and entry size; available pages are bounded by `PROSENJIT_PHYS_LIMIT`; low memory, kernel and Multiboot info are reserved. Needs current-head build/guest regression and complete edge-case audit (malformed mmap iteration, overlapping available/reserved ranges, allocator concurrency assumptions). |
| Virtual memory | PARTIAL | `kernel/paging.c`: `paging_map_page`, `paging_unmap_page`, `paging_switch_kernel`, `paging_kernel_root`, `ensure_table`; four-level x86-64 page tables, canonical VA and physical-limit checks, duplicate-map rejection and rollback logic are present. Mapping/allocator ownership and concurrent mutation still need a full function-by-function audit; source presence is not runtime proof. |
| Kernel heap | BLOCKED (boot gate) | `kernel/heap.c`: `heap_init`, `heap_grow`, `kmalloc`, `kfree`, `heap_used_bytes`, `heap_selftest`. `kernel/kernel.c` explicitly halts on `heap_selftest() != 0` with `ProsenjitOS: ERROR kernel heap deep self-test failed`. Historical conversation also records this boot failure. Root cause is not established by the inspected source alone; do not change allocator semantics speculatively. Reproduce on exact HEAD, add stage-specific diagnostic return logging, then fix the demonstrated invariant violation and rerun all gates. |
| Interrupt/CPU setup | PARTIAL | `kernel/kernel.c` contains IDT setup and exception/IRQ gates; symbols include `idt_init`, `idt_set_gate`, `isr128`, `isr32`, `cpu_load_tss`, and `enter_user_mode`. IDT uses dedicated IST for double fault, page fault and syscall entry. `docs/DEBUG_LOG.md` notes open risks around shared IST1 syscall-frame lifetime. Full ISR assembly, GDT/TSS, PIC/PIT and exception-frame audit remains. |
| Scheduler/process | PARTIAL | `kernel/process.c`: process lifecycle/registration/teardown; `kernel/thread.c`: `thread_manager_init`, `thread_init`, run queue and process-thread termination. Source comments enforce that blocked/quarantined processes do not run. Debug log flags scheduler eligibility, no-successor handling and IPC wait/wakeup synchronization as open risks. |
| Syscall ABI and IPC | PARTIAL | `kernel/syscall.c`, `include/abi/syscall.h`, and `kernel/kernel.c` provide the syscall dispatcher and CPL3 tests. Dispatch applies per-process syscall allowlists and capability checks. Prior debug log reports an IPC test fix and successful earlier QEMU run, but that does not prove current-head boot. Shared IST1 frame lifetime and IPC wait/wakeup synchronization remain audit items. |
| DMA and device enumeration | PARTIAL | Roadmap reports PCI enumeration foundation implemented; `kernel/kernel.c` includes `pci.h`, `driver.h`, `device_broker.h`, `ata_pio.h`, `partition.h`, and `storage.h`. Part 2 PR #171 explicitly says DMA allocator/IOMMU path, IRQ routing and device ownership arbitration are outstanding. Do not claim DMA support. |
| Open PR dependencies | PARTIAL | PR #169 is the Part 1 shared desktop ABI foundation, while PR #171 is stacked on it. These are not substitutes for the kernel/boot foundation audit or a current-head boot pass. Preserve unrelated branches and do not merge either PR as part of this task. |
| Host tests | NOT TESTED | No host test command was executed in this session. Existing self-tests and Makefile targets must be inspected and run on the exact candidate head. |
| ISO/QEMU and native BIOS QEMU | NOT TESTED | No guest was started in this session. Prior CI descriptions are commit-specific and must not be reused as evidence for this audit head. |
| Physical hardware / UEFI | NOT TESTED | No physical device or UEFI loader test was performed; roadmap still marks native UEFI pending. |

## Known dependencies and interfaces to preserve

- Build entry point: `make iso` -> `scripts/build.sh`; clean target: `make clean`.
- Architecture: freestanding x86-64 kernel; avoid libc/libgcc assumptions and preserve current calling conventions.
- Boot inputs: Multiboot2 path and native BIOS handoff coexist. Do not silently replace either path.
- Memory: `memory.h` / `paging.h` interfaces are consumed by heap, address-space and process code. Preserve page ownership and user-mappable-page rules.
- Privilege boundary: `include/abi/syscall.h`, syscall number/argument conventions, per-process syscall allowlist and capability checks must remain stable absent a documented migration.
- Scheduler/IPC: `thread.h`, `process.h`, `ipc.h` lifetimes and wait/wakeup semantics are coupled; do not patch a single side without regression tests.
- Native storage boot tests depend on ATA PIO -> partition -> ProsenjitFS; retain those markers and tests while diagnosing earlier boot failures.

## Required next verification sequence

1. On the configured Linux x64 self-hosted runner, record `git rev-parse HEAD`, `git status --short`, and runner service/job identity before running anything. Do not assume that a configured service means a workflow executed.
2. Run `make clean` and `make iso`; capture complete logs and exact exit codes.
3. Run the repository's focused host tests and security architecture workflow on this exact commit.
4. Run ISO QEMU and native BIOS disk-image QEMU separately. For each, capture serial output and require the workflow's expected markers; timeout exit 124 is acceptable only if the test script verifies every required marker before treating it as a pass.
5. Diagnose the heap self-test by reporting the exact failing return stage, without weakening or skipping the self-test. Fix only a demonstrated cause.
6. Rerun all host, build, ISO QEMU, native BIOS QEMU and security checks after the fix. Record resulting commit SHA and workflow run URLs.
7. Continue the interrupt/scheduler/IPC/DMA audit only after the earliest boot gate is green, so later failures are not masked by the heap halt.

## Change and acceptance status

This PR adds only this audit report. It deliberately makes no kernel source changes because the reported heap failure has not been reproduced against a fresh exact-head log in this execution environment. That restraint preserves working components and avoids an unsupported allocator redesign.

- Audit report: COMPLETE (source/document review; limited by remote-only inspection).
- Kernel source fix: BLOCKED (heap self-test root cause not yet proven).
- Build/tests/QEMU: NOT TESTED in this session.
- Physical hardware/UEFI: NOT TESTED.
- Overall Part 1 acceptance: NOT COMPLETE.
