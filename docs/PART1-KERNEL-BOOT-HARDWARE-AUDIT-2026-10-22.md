# Part 1 — Kernel, Boot & Hardware Foundation Audit

Date: 2026-10-22
Audit branch: `audit/part1-kernel-boot-foundation-20261022`
Audit commit: `4affca7da947b096147a274f9008eb8d6b70dc07`
PR merge-test commit: `05c4f1ee184a2a485f65dbe5eb6b36c01b1b935c`

## Scope and evidence boundary

This is a remote source audit supplemented by actual GitHub Actions logs. CI evidence below is for the PR merge-test commit, not the standalone branch head. No physical hardware or UEFI test was performed. Source presence is not proof that all feature paths are production-ready.

## Findings

| Area | Status | Evidence / remaining work |
|---|---|---|
| Repository/build baseline | COMPLETE for inspected CI snapshot | Default branch is main. `Makefile` calls `scripts/build.sh`; freestanding x86-64 GCC/NASM build uses warnings-as-errors. ISO build passed in workflow #7792. |
| Boot paths | PARTIAL | `boot/entry.asm` records Multiboot2 EAX/EBX, builds identity maps for the first 1 GiB using 2 MiB pages, enables PAE/long mode/paging and transfers to 64-bit code. ISO and native BIOS QEMU marker gates passed on merge-test commit. Full loader/linker/source inventory and boot-protocol audit remain. Native UEFI remains unverified. |
| Physical memory | PARTIAL | `kernel/memory.c` provides physical allocation/free and user-mappable page tracking; Multiboot2 mmap parsing has bounds checks. Physical-memory invariant self-test and page allocation/free markers passed in ISO QEMU. Malformed/overlapping memory map and concurrency edge cases still need dedicated tests. |
| Virtual memory | PARTIAL | `kernel/paging.c` has four-level x86-64 page tables, canonical-address checks, map/unmap and kernel-root switching. Paging deep invariant and address-space isolation tests passed in ISO QEMU. Complete ownership/rollback/concurrency audit remains. |
| Kernel heap | COMPLETE for current CI test; historical cause unknown | `kernel/heap.c` exposes `heap_init`, `kmalloc`, `kfree`, `heap_used_bytes`, `heap_selftest`. Both heap allocation/free and deep invariant markers passed in ISO QEMU. This run did not reproduce the previously reported failure; it does not establish the historical root cause. Keep the self-test enabled. |
| Interrupt/CPU setup | PARTIAL | `kernel/idt.asm` implements exception, syscall and timer entry paths; `kernel/kernel.c` integrates IDT/TSS setup. The QEMU logs show TSS load and CPL3 syscall/IPC tests passed. Still audit IST stack lifetime, frame layouts, privilege transitions, timer preemption and error paths function by function. |
| Scheduler/process | PARTIAL | Process/thread lifecycle, run queue, interrupt-frame scheduling and teardown paths exist. QEMU markers passed for thread lifecycle/run queue, IPC wait-queue block/wakeup, scheduler frame save/restore, and address-space switch/reap. This does not prove SMP safety or all race cases. |
| Syscall ABI/IPC | PARTIAL | `include/abi/syscall.h`, `kernel/syscall.c`, and IPC implementation are existing interfaces. QEMU CPL3 syscall/IPC and security runtime markers passed. Preserve syscall numbers, calling convention, capability checks and frame layout; add deeper negative/race tests before declaring complete. |
| PCI/device enumeration | PARTIAL | PCI enumeration and device broker self-tests passed in QEMU; six PCI devices were enumerated in the recorded ISO run. This does not establish safe DMA or production drivers for all devices. |
| DMA/IOMMU | BLOCKED / NOT ESTABLISHED | Available source/docs do not establish a complete safe DMA allocator/IOMMU path. Do not claim DMA support from headers alone. Define address width, pinning, ownership, coherency, interrupt routing and teardown before implementing bus-mastering drivers. |
| Host unit tests | COMPLETE for the workflow's desktop-manager target | Workflow #7792 reports “desktop manager tests passed”. Other host-test coverage must be inventoried separately. |
| ISO QEMU | COMPLETE for required workflow markers | Workflow #7792 exited QEMU with status 124 at the configured 20-second timeout, then validated required serial markers; job passed. Required markers included physical memory, paging, heap, address-space isolation, syscall/CPL3 IPC, scheduler, storage and device tests. |
| Native BIOS QEMU | COMPLETE for required workflow markers | Same workflow: timeout status 124, followed by successful checks for native handoff, ATA PIO block path, LBA boundary diagnostic and ATA -> MBR -> ProsenjitFS persistent mount. |
| Security architecture contract | COMPLETE for CI contract check | Workflow #7328 passed the repository's security-document marker/contract check. This is not a full security proof. |
| Physical hardware / UEFI | NOT TESTED | No physical device test or UEFI boot was performed. |
| Full source/dependency audit | PARTIAL | The following files were inspected: `Makefile`, `scripts/build.sh`, `boot/entry.asm`, `kernel/kernel.c`, `kernel/memory.c`, `kernel/paging.c`, `kernel/heap.c`, `kernel/idt.asm`, `docs/ROADMAP.md`, `docs/DEBUG_LOG.md`, `docs/REGRESSION-MATRIX.md`. A complete declaration-by-declaration dependency and symbol map is still needed. |

## Exact CI evidence

- Build and boot-test workflow: [run #7792](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39775661349), runner `Office1` (Linux), job conclusion success.
- Security architecture workflow: [run #7328](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39775661242), job conclusion success.
- Tested merge commit: `05c4f1ee184a2a485f65dbe5eb6b36c01b1b935c`.
- ISO QEMU exit status: 124 due to configured timeout; required serial markers were checked and passed before the workflow succeeded.
- Native BIOS QEMU exit status: 124 due to configured timeout; required native boot/storage serial markers were checked and passed before the workflow succeeded.

## Interfaces and dependencies to preserve

- Build entry: `make iso` -> `scripts/build.sh`; clean target: `make clean`.
- Keep the freestanding x86-64 calling conventions and avoid libc/runtime assumptions.
- Preserve both Multiboot2 and native BIOS boot paths.
- Preserve `memory.h` / `paging.h` ownership and user-mappable-page rules.
- Preserve `include/abi/syscall.h`, syscall numbers/arguments, capability checks and user/kernel frame layout unless a documented migration is tested.
- Scheduler, ISR frames, `process.h`, `thread.h` and `ipc.h` lifetimes/wakeup semantics are coupled.
- Keep ATA PIO -> partition -> ProsenjitFS regression markers.

## Remaining work and acceptance

1. Complete a full-tree symbol and dependency map, including linker scripts, native BIOS loader, interrupt/exception definitions, scheduler, IPC and DMA-related files.
2. Add focused deterministic tests for malformed Multiboot2 memory maps, overlapping memory ranges, allocation ownership, page-table rollback and allocator failure paths.
3. Audit interrupt stack/frame invariants, IST use, syscall privilege transitions, timer scheduling, IPC wait/wakeup ordering and teardown races.
4. Establish safe DMA/IOMMU and device resource ownership before claiming DMA support.
5. Re-run the required build, host tests, ISO QEMU, native BIOS QEMU and security checks after any source change; record the resulting exact SHA and logs.
6. Validate native UEFI and physical hardware separately when supported.

- Audit report: COMPLETE for remote review plus recorded CI evidence; source inventory remains partial.
- Build/host target/ISO QEMU/native BIOS QEMU/security contract: COMPLETE for merge-test commit above.
- Kernel source fix: no kernel source was changed in this documentation-only PR. The historical heap failure was not reproduced in this run; its root cause remains unknown.
- Interrupt/scheduler/IPC/memory edge-case audit: PARTIAL.
- DMA/IOMMU: BLOCKED / not established.
- Physical hardware/UEFI: NOT TESTED.
- Overall Part 1 acceptance: NOT COMPLETE.
