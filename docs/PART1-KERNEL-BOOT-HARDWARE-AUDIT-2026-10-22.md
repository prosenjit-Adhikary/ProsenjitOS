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
| Physical memory | PARTIAL | `kernel/memory.c` provides physical allocation/free and user-mappable page tracking. Parser validation now rejects truncated/missing-end tags, partial mmap entries, and unsupported entry versions; non-available ranges override overlapping available ranges. Dedicated host regression tests passed in workflow #7814. Allocator concurrency and further malformed-map edge cases still need review. |
| Virtual memory | PARTIAL | `kernel/paging.c` has four-level x86-64 page tables, canonical-address checks, map/unmap and kernel-root switching. Paging deep invariant and address-space isolation tests passed in ISO QEMU. Complete ownership/rollback/concurrency audit remains. |
| Kernel heap | COMPLETE for current CI test; historical cause unknown | `kernel/heap.c` exposes `heap_init`, `kmalloc`, `kfree`, `heap_used_bytes`, `heap_selftest`. Both heap allocation/free and deep invariant markers passed in ISO QEMU. This run did not reproduce the previously reported failure; it does not establish the historical root cause. Keep the self-test enabled. |
| Interrupt/CPU setup | PARTIAL | `kernel/idt.asm` now saves all GPRs across recoverable exceptions and timer IRQs, dynamically aligns C-call stacks, and sends PIC EOI before restoring interrupted registers. TSS, CPL3 syscall/IPC, and scheduler-frame QEMU markers pass in workflow #7814. Dedicated register-sentinel tests, IST lifetime and all privilege/error paths still need further audit. |
| Scheduler/process | PARTIAL | Process/thread lifecycle, run queue, interrupt-frame scheduling and teardown paths exist. QEMU markers passed for thread lifecycle/run queue, IPC wait-queue block/wakeup, scheduler frame save/restore, and address-space switch/reap. This does not prove SMP safety or all race cases. |
| Syscall ABI/IPC | PARTIAL | `include/abi/syscall.h`, `kernel/syscall.c`, and IPC implementation are existing interfaces. QEMU CPL3 syscall/IPC and security runtime markers passed. Preserve syscall numbers, calling convention, capability checks and frame layout; add deeper negative/race tests before declaring complete. |
| PCI/device enumeration | PARTIAL | PCI enumeration and device broker self-tests passed in QEMU; six PCI devices were enumerated in the recorded ISO run. This does not establish safe DMA or production drivers for all devices. |
| DMA/IOMMU | BLOCKED / NOT ESTABLISHED | Available source/docs do not establish a complete safe DMA allocator/IOMMU path. Do not claim DMA support from headers alone. Define address width, pinning, ownership, coherency, interrupt routing and teardown before implementing bus-mastering drivers. |
| Host unit tests | COMPLETE for desktop-manager and Multiboot2 memory-map targets | Workflow #7814 passed `make desktop-unit-test memory-map-unit-test`; the memory-map test covers valid maps, reserved-overlap precedence, first allocation, out-of-bounds tag, partial entry, truncated tag, missing end tag and unsupported entry version. Other host-test coverage must be inventoried separately. |
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
2. Add focused deterministic tests for page-table rollback, allocation failure paths, allocator ownership under stress, and concurrent memory-management operations.
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


## Addendum — CI rerun for audit head (2026-10-22)

- Audit branch head tested: `5d1900fbe41e3aeb3b12ab7f2dd3240785a06ae0`.
- PR merge-test commit used by GitHub Actions: `46e95e7376b3b1b5324cb81f438f3f345563a722`.
- Build and boot-test workflow [run #7796](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39776529908): SUCCESS on Linux self-hosted runner `office`. Desktop manager unit tests, ISO build and ProsenjitFS block-init diagnostic passed. ISO QEMU and native BIOS QEMU required-marker checks passed.
- Native BIOS serial log confirmed Multiboot/native handoff, x86-64 long mode, physical memory allocation/free, paging and heap deep-invariant tests, address-space isolation, syscall/security, process/thread/scheduler/IPC, ProsenjitFS format/mount/journal/recovery, ATA PIO -> MBR -> ProsenjitFS persistent mount, PCI enumeration, and CPL3 syscall/IPC markers.
- Native BIOS QEMU exited with status 124 because the configured timeout terminated QEMU; required serial markers were checked and passed before the workflow concluded success. This is not an unlimited-duration boot or physical-hardware test.
- Security architecture workflow [run #7332](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39776529531): SUCCESS for the audit head.
- These CI results improve confidence in the existing boot/test path but do not close the remaining memory-map edge cases, full interrupt/scheduler/IPC audit, DMA/IOMMU safety, UEFI, or physical-hardware validation. No kernel source change was made.


## Addendum — memory-map hardening and CI verification (2026-10-22)

- Source change: `kernel/memory.c` commit `90dc0130fae6794dfd662efebf2823afbd1c79c8`.
- Changes: Multiboot2 tag bounds use subtract-before-add validation; malformed/missing end tags are rejected; memory-map payload must fit within the parent tag; entry version/stride/payload size are validated; non-available firmware ranges are reserved after available ranges so overlapping descriptions fail closed.
- Build and boot-test workflow [#7804](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39777558176): SUCCESS on the authorized Linux self-hosted runner. Desktop manager unit tests, ISO build, ProsenjitFS block-init diagnostic, ISO QEMU marker checks, and native BIOS QEMU marker checks passed.
- Native BIOS serial evidence includes physical memory deep invariant, page allocation/free, paging and heap deep invariants, address-space isolation, syscall ABI/policy, security runtime W^X/NX, process/thread/scheduler/IPC regressions, ProsenjitFS format/mount/journal/recovery, ATA PIO -> MBR -> persistent ProsenjitFS mount, PCI enumeration (six devices in this QEMU run), and CPL3 syscall/IPC.
- Native BIOS QEMU exit status was 124 at the configured 20-second timeout; all required serial milestones were present and checked by CI. This is a bounded QEMU marker test, not physical-hardware or unlimited-runtime validation.
- Security architecture workflow [#7336](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39777558161): SUCCESS.
- Remaining: add a focused host-side synthetic Multiboot2 parser test (including truncated tag, oversized mmap payload, missing end tag, and overlapping available/reserved ranges); complete function-level interrupt/scheduler/IPC and page-table ownership/concurrency review; DMA/IOMMU, UEFI and physical hardware remain unverified.
- Overall Part 1 acceptance remains NOT COMPLETE until the new parser change has focused malformed-input regression coverage and the remaining hardware/security audit gates are resolved.


## Addendum — memory and interrupt hardening (2026-10-22; supersedes earlier docs-only status)

- Final source commit tested by CI: `ba5a478aa93899805cc812f0b1d337823a7d21d6`.
- `kernel/memory.c`: validate the Multiboot2 tag chain before parsing; reject missing/truncated end tags, malformed entry stride, partial entries, and unsupported mmap entry versions; reserve non-available ranges after available ranges so reserved overlap wins.
- `tests/memory_map_test.c` and `scripts/test_memory_map.sh`: added host-side regression tests for valid map initialization, reserved overlap precedence and accounting, first allocation, out-of-bounds tag, partial mmap entry, truncated tag, missing end tag, and unsupported entry version. The first test attempt exposed a malformed fixture that confused the single Multiboot2 tag `size` field with a separate payload size; the fixture and assertion were corrected. Final tests all passed.
- `kernel/idt.asm`: exception entry now saves/restores every GPR around `exception_dispatch`, keeps the original exception frame pointer, and dynamically aligns the stack before calling C. Timer IRQ entry now dynamically aligns C calls and sends PIC EOI before restoring interrupted GPRs, avoiding C-call clobber of the resumed register state.
- CI [Build and boot-test run #7814](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39779291138): SUCCESS on authorized Linux self-hosted runner `office`. Desktop-manager and memory-map host tests passed; ISO build passed; ISO and native BIOS QEMU required-marker checks passed.
- CI [Security architecture run #7350](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39779291128): SUCCESS.
- Both QEMU invocations exited with status 124 at the configured 20-second timeout. Required boot/runtime markers were present and verified before CI passed. This is not a physical-hardware, UEFI, SMP, or unlimited-runtime test.
- Remaining acceptance blockers: full interrupt/IST/register-sentinel test coverage; page-table rollback/ownership and concurrency review; DMA/IOMMU safety is not established; UEFI and physical hardware are not tested. Overall Part 1 remains PARTIAL until these gates are addressed or explicitly accepted as out-of-scope.


## Addendum — syscall IST alignment and final verification (2026-10-22)

- Latest tested kernel/interrupt source commit: `a9dbc3315701f4ceed42719eee00f8988c5cc185`.
- `isr128` now keeps the saved GPR frame and CPU IRET frame addressed independently of the active stack alignment, dynamically aligns RSP before calling `syscall_dispatch` and `thread_schedule_interrupt`, and restores the original register-frame pointer before returning. This complements the earlier exception and timer entry alignment/register-preservation fixes.
- Build and boot-test workflow [#7815](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39779827776): SUCCESS on Linux self-hosted runner. Host desktop-manager and Multiboot2 memory-map tests passed; ISO build passed; ISO and native BIOS QEMU required-marker gates passed.
- Security architecture workflow [#7351](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39779827869): SUCCESS.
- ISO QEMU and native BIOS QEMU each exited with status 124 at the configured 20-second timeout; the required serial markers (including memory/paging invariants, security runtime, scheduler/IPC, filesystem/storage and CPL3 syscall tests) were present and verified before the job passed.
- Overall Part 1 remains PARTIAL: page-table rollback/ownership and allocator concurrency still need deeper tests; explicit register-sentinel/IST tests remain desirable; DMA/IOMMU safety is not established; UEFI and physical hardware are not tested.


## Addendum — exception error-code frame correction (2026-10-22)

- Source commit: `67c2ee44e87382048a54e7234a797c501d2ec79f`.
- Audit found that exception vectors 21 (#CP), 29 (#VC on AMD SEV-ES), and 30 (#SX) were using no-error-code stubs. CPUs implementing these exceptions push a hardware error code; adding a synthetic zero would shift the common frame and misreport the vector/error/RIP layout. These vectors now use `ISR_WITH_ERROR`, matching vectors 8, 10–14 and 17.
- Added `scripts/test_interrupt_vectors.sh` and wired it into the build workflow to guard the error-code vector list. This is a source-contract regression check; it does not simulate CET/SEV-ES hardware exceptions.
- Latest build/security CI for this exact follow-up commit must pass before treating it as verified.
- Part 1 remains PARTIAL: this static correction does not replace register-sentinel tests, IST stack tests, page-table/allocator concurrency tests, DMA/IOMMU validation, UEFI testing or physical hardware testing.

## Follow-up verification and exception-frame correction (2026-10-22)

This addendum supersedes the older CI snapshot links above for the latest source head. The current branch head at the time of verification was `01a72c908cdd8d09539e23f461eb25aa1056adff`.

- [Build and boot-test run #7837](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39780970730): **SUCCESS** on the Linux `Office1` runner. The workflow ran `bash -n` on the memory-map and interrupt-vector scripts, the exception-vector mapping regression, and the Multiboot2 memory-map regression. The ISO built successfully. ISO and native BIOS QEMU runs reached all required serial markers.
- [Security architecture run](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39780970729): **SUCCESS**.
- QEMU's exit status 124 is the configured 20-second timeout. Passing is based on the required serial markers being present before timeout; it is not a claim of unlimited-runtime stability.
- Exception-vector review corrected vectors 21 (#CP), 29 (#VC), and 30 (#SX) to use the hardware-error-code stub macro. `scripts/test_interrupt_vectors.sh` asserts the error-code classification for vectors 8, 10, 11, 12, 13, 14, 17, 21, 29, and 30. CI output includes: `PASS: all modern x86-64 error-code exception stubs use consistent frames`.

## Explicit remaining gaps

- **Allocator concurrency:** physical allocation/free and bitmap accounting are not protected by locks. Current test evidence does not establish SMP-safe allocator operation; do not enable concurrent allocator callers without a synchronization design and tests.
- **Page-table ownership/rollback:** map failure paths have rollback logic and QEMU address-space self-tests exist, but there is no exhaustive injected-allocation-failure test proving every partial page-table allocation is reclaimed. Treat this as partial, not proven complete.
- **Thread kernel-stack ownership:** thread initialization receives a caller-supplied stack top; the audit has not established a uniform per-thread stack allocation, guard-page, lifetime and teardown contract for multiple concurrent user threads. Dedicated register-sentinel and IST-stack tests remain needed.
- **DMA/IOMMU:** no complete DMA ownership/address-width/IOMMU safety path was established. PCI enumeration is not proof of safe bus mastering.
- **Platform coverage:** no physical-hardware or UEFI boot validation was performed. SMP/race behavior remains unverified.

Overall status remains **PARTIAL / NOT COMPLETE**. This is an evidence-bounded source/CI audit, not a complete kernel correctness or security proof.


## Physical allocator hardening addendum (2026-10-22)

- Branch commit: `3efa3ac1503210e0120b49b466e320a510fe5a6e` (allocator changes are in its ancestors).
- `kernel/memory.c`: boot-tag validation now completes before allocator state is mutated. A malformed map leaves the existing allocator state untouched; after successful initialization, a second valid initialization returns `-6` rather than silently clearing live allocation ownership and counters. If initialization finds no free managed pages, its partially built bitmap/accounting state is cleared before returning `-5`.
- Allocation now fails closed if a free bitmap bit is inconsistent with the managed bitmap, and checks accounting before decrementing. Ordinary allocation clears any stale user classification defensively.
- `tests/memory_map_test.c`: added checks for repeated initialization preserving live free-page accounting, user-page allocation classification, invalid/unaligned user-mapping queries, free clearing classification, and double-free accounting.
- Allocator design: fixed bitmap over the identity-mapped physical range below 1 GiB, 4 KiB pages. The page bitmap marks available pages; the managed bitmap distinguishes allocatable pages from permanently reserved pages; the user bitmap classifies only explicitly user-allocated pages. Page zero, low 1 MiB, kernel image, and Multiboot information are reserved. Non-available firmware entries override overlapping available entries.
- Ownership contract in current API: `physical_page_alloc()` returns an allocated managed page to its caller; `physical_page_alloc_user()` additionally marks it user-mappable; callers must free only pages they own and must not free a page merely because a mapping was removed. `physical_page_free()` rejects unaligned, unmanaged, out-of-range, already-free, and double-free addresses. Address-space and page-table teardown still require the broader caller/rollback audit.
- Synchronization limitation: allocator metadata is not protected by a lock. Existing code and current QEMU coverage do not establish that all calls are single-CPU/non-concurrent, so concurrent and SMP safety remain **NOT ESTABLISHED**. Do not treat this addendum as an SMP guarantee.
- Verification status: GitHub Actions were queued for the final head when this note was authored; their conclusions must be checked against the exact final SHA before declaring the allocator complete. Physical hardware, UEFI, injected page-table allocation failures, and exhaustive exhaustion/boundary tests remain unverified.


## Addendum — physical allocator synchronization and regression coverage (2026-10-22)

- Source change: IRQ-save atomic spin lock now serializes physical allocation, free, user-page classification, counters and initialization. Local maskable interrupts are disabled while the lock is held to prevent same-CPU interrupt re-entry; atomic acquire/release operations serialize CPUs. NMI handlers must not call the allocator. This implementation does not by itself establish that the kernel scheduler, device drivers, or all other memory subsystems are SMP-ready.
- `physical_page_alloc_user` now allocates and marks user-mappability within one critical section, preventing another caller from observing or freeing the page between allocation and classification.
- Query and free-page accounting accessors are synchronized. Initialization is serialized and a second valid initialization is rejected without resetting live allocations. Header comments document allocator scope and caller ownership.
- Host regression tests now exercise concurrent allocation across eight pthread workers, uniqueness while pages remain held, concurrent frees, exact accounting restoration, exhaustion, failed-allocation accounting, invalid frees, and 1,000 repeated allocation/free cycles.
- Host tests compile the same production allocator source with only the privileged interrupt-save instructions omitted under `PROSENJIT_MEMORY_HOST_TEST`; atomic lock semantics and allocator logic remain shared with the kernel build.
- Limitations: this is a spin lock, so allocator critical sections must remain short and must not be called from NMI context. Physical hardware, UEFI, and true multi-core kernel execution have not been tested. CI must pass on the final commit before this change is considered verified.
- Overall Part 1 remains PARTIAL because the wider page-table rollback, stack/IST, DMA/IOMMU, UEFI and physical-hardware acceptance gates are outside this allocator-focused change.


## Follow-up — paging mapping/ownership regression checks

- Commit `e8f93f7a359d3eb40b0183ae6425bd2063b1a99a` strengthens `paging_selftest()`.
- A rejected duplicate mapping now explicitly verifies that the original virtual-to-physical translation remains unchanged and allocator free-page accounting does not drift.
- Unmapping now has an explicit regression assertion that the caller-owned leaf physical page remains allocated; page-table unmapping does not imply ownership transfer.
- These checks run as part of the existing QEMU paging deep-invariant self-test. They do not yet replace deterministic allocation-failure injection for every intermediate page-table allocation branch. Final CI for this follow-up must be checked before claiming verification.


## Follow-up — deterministic page-table allocation-failure injection

- Commit `80dd711223ab1c1a9b0efe46e94c2be1b5573bdc` adds a private, self-test-only allocation fault injector around page-table allocation.
- `paging_selftest()` forces allocation failure after 0, 1, and 2 successful intermediate-table allocations, exercising failure at each of the three levels for an otherwise unmapped kernel virtual-address subtree.
- Each case asserts mapping failure, no resulting translation, and exact restoration of allocator free-page accounting. Fault injection is disabled/reset after each attempt and is not exposed as a public kernel API.
- CI must pass on this latest head before these new cases are considered verified. Physical hardware and actual SMP behavior remain separate validation requirements.


## Follow-up — interrupt entry source-structure regression guards

- Commit `2a58cbfffb2d475ed399ffac7677679ec1dfcc93` expands `scripts/test_interrupt_vectors.sh`.
- In addition to the existing architectural error-code vector checks, the script now checks that the syscall, timer IRQ and common exception entry blocks each contain saves/restores for all 15 general-purpose registers and explicitly align the temporary C-call stack.
- The script also guards restoration of the saved interrupt-frame pointer and checks that the timer entry retains a PIC EOI call.
- This is a static source-structure regression test. It does **not** execute injected register sentinels or prove actual IST delivery, stack bounds, nested exceptions, or scheduler-switch correctness. Those remain open acceptance gates.
- Build/boot and security workflows must pass on the resulting branch head before this follow-up is considered CI-verified. Part 1 remains **PARTIAL / NOT COMPLETE**.


## Verification result — interrupt entry source-structure guards

- Commit `4006900fbcfb229ba69786be3787b6e2a78a6cf8` adds static guards for GPR save/restore instructions, call-stack alignment, saved-frame restoration, error-code vectors, and the timer PIC EOI call.
- Build and boot-test workflow [#39787841682](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39787841682): **SUCCESS**. The workflow completed script/unit-test validation, ISO build, ProsenjitFS block-init diagnostic, ISO QEMU boot test, and native BIOS QEMU boot test.
- Security architecture workflow [#39787841777](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39787841777): **SUCCESS**.
- These CI results verify the current source-structure test and the configured boot milestones in QEMU. They do not prove actual runtime register sentinels, IST guard/bounds, nested exception handling, physical hardware, UEFI, or SMP safety.
- PR #183 remains draft and unmerged. Overall Part 1 remains **PARTIAL / NOT COMPLETE**.


## Cross-check — thread kernel-stack ownership and teardown

Source review of `kernel/thread.c`, `kernel/include/thread.h`, `kernel/context_switch.asm`, and the boot scheduler tests confirms the following boundary:

- `thread_init()` accepts a caller-provided `stack_top` and constructs the initial saved context on that memory. It does not allocate a stack, record its base/size/ownership, create a guard page, or provide a stack-release operation.
- The current boot scheduler test uses statically allocated 4 KiB stacks for its test threads. This proves a test fixture exists; it is not a general per-thread stack allocator.
- `thread_context_switch` stores the outgoing stack pointer in the saved context at the current structure layout's offset and restores the selected thread's stack. This is layout-coupled assembly; changes to `struct pros_thread`/`struct pros_cpu_context` need an explicit offset/layout check.
- `thread_terminate_process()` removes queued threads, cancels waiters, clears saved frame references, and marks threads dead. It does not free kernel stacks because the current thread contract does not own or track them. Any future owned-stack implementation must defer freeing the currently executing stack until execution has switched away from it.
- No source or CI evidence in this audit establishes per-thread stack guard pages, overflow detection, stack lifetime ownership, or safe reclamation after preemption.

**Decision:** do not retrofit dynamic stack allocation as a one-off change in this interrupt-hardening patch. First define a stack descriptor/owner and lifecycle contract, then add allocation-failure, guard-page, switch-away-before-free, repeated create/terminate, and teardown tests. This remains an explicit acceptance gate, not a completed feature.
-

## Addendum — latest audit-head verification (2026-10-23)

- Audit branch: `audit/part1-kernel-boot-foundation-20261022` (existing draft PR #183; this work remains separate from `main`).
- Source-code head previously tested: `4006900fbcfb229ba69786be3787b6e2a78a6cf8`. Subsequent commits `e01777468d16f6e6443c0e407d6f64b76ae6dbcd` and `067596d1edaee2923d30824425d202e0896ae0a2` update audit documentation only, as recorded in the PR description.
- Build and boot-test workflow [run #7877](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39797489341): **SUCCESS** (GitHub Actions API conclusion). Security architecture workflow [run #7413](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39797489339): **SUCCESS**. These runs are associated with documentation-only head `067596d1edaee2923d30824425d202e0896ae0a2`; they do not constitute a fresh compile of changed kernel source.
- Earlier source-changing commit `4006900fbcfb229ba69786be3787b6e2a78a6cf8` has successful build/boot run [#7874](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39787841682) and security run [#7410](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39787841777). The workflow reports include host/script checks, ISO build, and ISO/native-BIOS QEMU required-marker checks. QEMU status 124 is the configured timeout and was accepted only after required markers were validated.
- No workflow result here proves runtime register-sentinel coverage, IST stack safety, all scheduler/IPC races, complete page-table/allocator concurrency safety, DMA/IOMMU correctness, UEFI, physical hardware, or SMP safety.
- **Status:** existing audit PR remains **PARTIAL / NOT COMPLETE**. No new kernel source modification is claimed in this addendum. Remaining gates stay explicit: finish the full-tree dependency/symbol inventory; add runtime interrupt/IST and stronger concurrency/ownership tests; establish a designed and tested DMA/IOMMU/resource-ownership path before claiming DMA support; and test UEFI/physical hardware when available.


## Follow-up — user/kernel virtual-address boundary (2026-10-23)

- Commit `a2adb87c3af8a6cf84539e6ced428f08207604d4` hardens `paging_map_page()`: requests with `PROSENJIT_PAGE_USER` in the canonical high half are rejected before any page-table allocation or mutation.
- `paging_selftest()` now checks that a user mapping at `0xFFFF800000001000` is rejected and remains untranslated.
- Rationale: on four-level x86-64 paging, a user-accessible mapping must not be admitted in the canonical kernel half. This guard complements page-table permission checks; it does not replace the user address-space isolation audit.
- Verification state at this log update: commit exists on the audit branch, diff was re-fetched and reviewed, but the workflow lookup returned no runs for this source-changing commit. **Build/QEMU verification is pending; this change is NOT marked tested or complete.** Re-run the repository's host tests, build, ISO QEMU, native BIOS QEMU, and security workflow against this exact commit before acceptance.


## Follow-up — avoid false assumption in paging regression (2026-10-23)

- Source commit `acbc41742fab399d39fa0ccb2848ba69c7176ceb` removes a fragile assertion that the kernel-root translation of `0xFFFF800000001000` must be absent. That address belongs to the canonical high half and could legitimately already be mapped by a kernel configuration; this test should validate rejection of the attempted USER mapping, not assert that a kernel VA is unmapped.
- The core negative regression remains: `paging_map_page(0xFFFF800000001000, ..., PROSENJIT_PAGE_USER | ...)` must fail.
- CI at the time of this log update: build/boot run #7932 (for the preceding source commit) was in progress at the native BIOS QEMU step; build/boot run #7933 for the latest audit-head update was pending. Security architecture run #7469 succeeded for the prior audit head. These are not yet passing results for this latest source commit.
- Latest source commit build/host tests/ISO QEMU/native BIOS QEMU: **PENDING / NOT VERIFIED**. Overall acceptance remains **PARTIAL**.


## Addendum — current audit-head CI result (2026-10-23)

- Current PR head: `9580e3f3d65026491283575d52ee5603b1d6776d`.
- Build and boot-test workflow [run #7936](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39907555272): **SUCCESS**. The job passed runner/toolchain verification, shell syntax and static interrupt-vector tests, desktop and Multiboot2 memory-map host tests, ISO build, ProsenjitFS block-init diagnostic, ISO QEMU required-marker checks, and native BIOS QEMU required-marker checks.
- Security architecture workflow [run #7472](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39907555224): **SUCCESS**.
- The tested source ancestry includes `acbc41742fab399d39fa0ccb2848ba69c7176ceb`, which rejects USER mappings in the canonical high half and avoids assuming that a kernel high-half VA must be unmapped. The current head adds audit documentation after that source change.
- QEMU gates are bounded marker tests. They do not establish unlimited-runtime stability, physical hardware, UEFI, true SMP safety, runtime register-sentinel/IST correctness, or DMA/IOMMU safety.
- **Updated status:** current PR-head build/host tests/ISO QEMU/native BIOS QEMU/security contract are **COMPLETE for the configured CI gates**. Overall Part 1 remains **PARTIAL / NOT COMPLETE** because the stack ownership/guard/reclamation contract, complete scheduler/IPC race review, full-tree dependency inventory, DMA/IOMMU, UEFI and physical-hardware gates remain open.


## Follow-up — restrict shared kernel-root mapping API (2026-10-23)

- Source commit: `b5c42ae4485a39adb07530931ff8a9948bae6018`.
- Review found that `paging_map_page()` mutates the shared kernel PML4 but accepted `PROSENJIT_PAGE_USER` for lower-half addresses if it could create a new USER-enabled hierarchy. The prior guard rejected only high-half USER mappings. That left an unnecessary way to create user-accessible mappings through the kernel-root API.
- Hardened the API boundary: `paging_map_page()` now rejects `PROSENJIT_PAGE_USER` regardless of virtual address. User mappings must go through `address_space_map_user_page()` in a process-specific root.
- Expanded `paging_selftest()` to assert rejection of both a canonical high-half and a lower-half USER mapping.
- This is a source-level security hardening; do not mark it verified until the exact latest-head build, host tests, ISO QEMU, native BIOS QEMU and security workflow conclude successfully.
- Overall Part 1 remains **PARTIAL / NOT COMPLETE** pending latest-head CI and the previously listed stack/IST, lifecycle/race, DMA/IOMMU, UEFI and physical-hardware gates.


## Verification result — shared kernel-root USER mapping restriction (2026-10-23)

- Source change tested: `b5c42ae4485a39adb07530931ff8a9948bae6018`; tested PR head: `8e52e645d34eb21254dec02bed860e684beb5120`.
- Build and boot-test workflow [run #7942](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39907995238): **SUCCESS**. The exact-head job passed runner/toolchain checks, static interrupt-vector and host tests, ISO build, ProsenjitFS diagnostic, ISO QEMU required markers, and native BIOS QEMU required markers.
- Security architecture workflow [run #7477](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39907995286): **SUCCESS**.
- The new paging self-test's lower-half and upper-half USER-mapping rejection checks were included in the built kernel and passed the configured QEMU marker gates.
- QEMU was terminated by the configured bounded test timeout after the required serial markers were checked. This does not establish unlimited-runtime stability or physical hardware, UEFI, or real SMP correctness.
- **Verified for configured CI gates:** kernel-root USER mapping restriction. **Overall Part 1 remains PARTIAL / NOT COMPLETE**; outstanding gates include full-tree dependency inventory, runtime register-sentinel/IST and lifecycle race tests, thread stack ownership/guard/reclamation, and DMA/IOMMU, UEFI, physical-hardware validation.


## Follow-up — compile-time scheduler/ISR ABI layout assertions (2026-10-23)

- Source commit: `361e5102d9dcd7d0f4682bff7e6ebfef345eb6e9`.
- `kernel/thread.c` now uses compile-time assertions to verify that `pros_thread.context.rsp` remains at byte offset 80, matching `kernel/context_switch.asm`, and that `pros_interrupt_frame.rip` and the complete interrupt-frame size match the assembly's 15-register + 3-word frame contract.
- Purpose: fail the build immediately if a C structure edit silently changes offsets consumed by handwritten assembly. These checks do not prove runtime register preservation, stack safety, or correct IST delivery.
- Verification status: **PENDING** until build/host tests/ISO QEMU/native BIOS QEMU and security checks finish for this source commit or a later head containing it.
- Thread-stack allocation/ownership/guard/reclamation remains a separate unresolved design gate; this change intentionally does not invent a stack allocator or alter lifecycle semantics.


## Follow-up — thread stack-top alignment validation (2026-10-23)

- Source commits: `361e5102d9dcd7d0f4682bff7e6ebfef345eb6e9` (compile-time C/assembly layout assertions), `f35ece2d8d0abd57be320078dfc5b16bed66b799` (reject non-16-byte-aligned stack tops), and `2a825a7243a2e09bf27479420d53423447593523` (boot scheduler regression setup).
- `thread_init()` now rejects stack tops that are not 16-byte aligned, matching the bootstrap trampoline's x86-64 SysV call alignment. The boot scheduler test supplies a deliberately misaligned stack top and requires rejection before normal test-thread registration.
- Compile-time assertions keep the C `pros_thread.context.rsp` offset synchronized with the hard-coded offset in `context_switch.asm`, and assert the interrupt frame's RIP offset and total size.
- These are narrow ABI/validation hardening changes. They do **not** establish stack memory bounds, ownership, guard pages, overflow detection, deferred reclamation, runtime GPR sentinels, or IST correctness.
- Verification for these source changes: **PENDING** exact-head build/host tests/ISO QEMU/native BIOS QEMU/security workflow.
- Overall Part 1 remains **PARTIAL / NOT COMPLETE**.


## Verification result — scheduler/ISR ABI and stack-top checks (2026-10-23)

- Tested PR head: `c99f20d9fd0725957ea8c32989a8276dd3afd518`.
- Build and boot-test workflow [run #7955](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39908712948): **SUCCESS**. Runner/toolchain checks, static interrupt-vector checks, desktop-manager and memory-map host tests, ISO build, ProsenjitFS block-init diagnostic, ISO QEMU required markers, and native BIOS QEMU required markers all passed.
- Security architecture workflow [run #7491](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39908712958): **SUCCESS**.
- The tested head includes the compile-time C/assembly layout assertions, 16-byte stack-top validation in `thread_init()`, the deliberately misaligned-stack rejection regression, and the shared-kernel-root USER mapping restriction.
- Both QEMU stages are bounded marker tests (the workflow validates required serial markers); this does not prove unlimited runtime, real SMP behavior, physical hardware, UEFI, DMA/IOMMU safety, or complete IST/register-sentinel correctness.
- **Verified for configured CI gates:** the latest source head above builds and passes the configured host, ISO-QEMU, native-BIOS-QEMU, and security-contract gates.
- **Overall Part 1 remains PARTIAL / NOT COMPLETE.** Remaining substantive gates include stack ownership/bounds/guard/reclamation, runtime ISR/IST register-sentinel testing, deeper scheduler/IPC lifecycle race coverage, complete source/dependency inventory, safe DMA/IOMMU design, UEFI, and physical hardware.


## Verification result — user-mapping physical-page admission and TLB invalidation (2026-10-23)

- Source head tested: `561544d22074c9660552e070d276532f1b509bad`.
- `kernel/address_space.c`: `address_space_map_user_page_owned()` now applies the same `physical_page_is_user_mappable()` admission check as the non-owned mapping API. Without this, the owned API could admit an allocated kernel-only page into a user mapping and later treat it as owned user memory during teardown.
- `address_space_selftest()` now explicitly requires both owned and non-owned user mapping APIs to reject a generic kernel allocation that is not marked user-mappable. Cleanup paths unmap before freeing if a regression unexpectedly succeeds.
- `address_space_map_user_page()` now executes `invlpg` for the mapped VA after publishing the leaf entry, so an active address space does not retain a stale translation for that VA.
- Build and boot-test workflow [#7958](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39909044629): **SUCCESS**. Static interrupt-vector checks, desktop-manager and Multiboot2 memory-map host tests, ISO build, ProsenjitFS diagnostic, ISO QEMU required-marker checks and native BIOS QEMU required-marker checks all passed.
- Security architecture workflow [#7494](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39909044655): **SUCCESS**.
- The QEMU tests are bounded serial-marker gates, not proof of physical-hardware behavior, UEFI, true SMP concurrency, unlimited runtime, or DMA/IOMMU safety.
- **Verified for configured CI gates:** user-mapping page admission and mapping-path changes in the exact source head above.
- **Overall Part 1 remains PARTIAL / NOT COMPLETE.** Stack ownership/guard/reclamation, runtime GPR/IST and nested-exception tests, deeper scheduler/IPC race coverage, complete dependency inventory, DMA/IOMMU, UEFI and physical-hardware validation remain open.


## Verification result — per-thread user interrupt-frame lifetime (2026-10-23)

- Source commit tested: `d8e7b7571e4e618d038c93e8b60e736e7ea6ad6c`.
- Root cause addressed: user-origin timer interrupts use the shared TSS RSP0 stack, and `INT 0x80` uses shared IST1. Keeping a pointer into either transient stack after switching threads risks resuming a frame that a later interrupt has overwritten.
- `kernel/include/thread.h` now defines a complete 20-word user interrupt frame (15 saved GPRs plus RIP/CS/RFLAGS/RSP/SS) and embeds stable storage in each thread.
- `thread_schedule_interrupt()` snapshots all 20 words when the saved CS indicates CPL3; CPL0 frames continue to reference the interrupted thread's own kernel stack. The common assembly restore layout remains the first 18 words.
- Boot regression coverage checks that the incoming user frame is copied into the current thread's storage, RSP/SS and register sentinels survive, and a later scheduler selection returns the stable saved frame.
- Build and boot-test workflow [#7967](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39914674318): **SUCCESS** for this exact source commit. Runner/toolchain validation, host tests, ISO build, ProsenjitFS diagnostic, ISO QEMU required markers and native BIOS QEMU required markers passed.
- Security architecture workflow [#7503](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39914674395): **SUCCESS** for the same commit.
- These are bounded QEMU/CI gates; they do not establish physical hardware, UEFI, SMP, unlimited runtime, or DMA/IOMMU safety.
- **Remaining acceptance gates:** stack ownership/bounds/guard/reclamation; actual ISR/IST register-sentinel and nested-exception delivery beyond the source-level/scheduler-frame regression; deeper scheduler/IPC race coverage; complete memory/page-table dependency audit; DMA/IOMMU; UEFI and physical hardware.
- **Overall Part 1 remains PARTIAL / NOT COMPLETE.**


## Verification result — guarded bootstrap and IST stacks (2026-10-23)

- Tested source commit: `eb89c7e823df4b30b6d452bc52fb553c08769e40`.
- `boot/entry.asm` now page-aligns the bootstrap stack and IST1/IST2/IST3 stacks, reserving a dedicated page immediately below each stack.
- `kernel/paging.c` removes those four guard pages from the final kernel page table after establishing the identity map, verifies each guard remains unmapped, and verifies the corresponding stack ends remain mapped RW+NX. Guard-page physical memory remains reserved as part of the kernel image and is not freed to the allocator.
- The first guard-page attempt correctly exposed a mismatch in the hardening audit: the BSS verifier expected every BSS page to be mapped. `kernel/security_hardening.c` now explicitly recognizes only these four named guard pages, requires them to remain unmapped, and continues checking all other BSS pages for supervisor RW+NX.
- Build and boot-test workflow [#7975](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39915295717): **SUCCESS**. Host tests, ISO build, ProsenjitFS diagnostic, ISO QEMU and native BIOS QEMU marker gates passed.
- Security architecture workflow [#7511](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39915295611): **SUCCESS** for the same source commit.
- Together with the previous per-thread user-frame fix, the configured boot gates now cover the guard-page checks and the scheduler snapshot/resume regression.
- **Still open:** dynamically allocated per-thread kernel stacks with explicit ownership/reclamation; guard/bounds coverage for arbitrary caller-provided thread stacks; actual ISR/IST GPR-sentinel and nested-exception delivery tests; deeper scheduler/IPC race coverage; complete memory/page-table dependency audit; DMA/IOMMU; UEFI and physical hardware.
- **Overall Part 1 remains PARTIAL / NOT COMPLETE.** These changes improve stack isolation but do not establish a general thread-stack lifecycle.


## Verification result — runtime interrupt register preservation (2026-10-23)

- Source change: `dc6189e9442ace74108b2835e2900d7ef325ca3c` adds runtime GPR sentinel probes for the exception path (recoverable INT3) and timer interrupt path (software INT 0x20), and emits a serial success marker only after both return with all tested GPRs intact.
- CI enforcement change: `1826a022981c3b55f380f7b6ae07429c5164fc66` makes both ISO and native BIOS QEMU runs require the runtime-interrupt marker, in addition to the previously required guard/security milestones.
- Build and boot-test workflow [#7979](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39915850981): runner/toolchain validation, static/host tests, ISO build, ProsenjitFS diagnostic, ISO QEMU, and native BIOS QEMU steps all report **success**. Both QEMU serial logs contain `ProsenjitOS: interrupt/timer GPR preservation runtime tests passed`; each QEMU run ended with expected timeout status 124 after its bounded test window. The workflow API had not yet finalized the overall run status when this report was updated, despite all listed job steps being completed successfully.
- Security architecture workflow [#7515](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39915850890): **SUCCESS**.
- These tests exercise GPR preservation for the returning INT3 and timer entry paths in QEMU. They do not test every hardware exception, IST nesting, all interrupted privilege/stack transitions, true SMP concurrency, or physical hardware.
- **Still open:** dynamically allocated per-thread kernel-stack ownership/bounds/guard/reclamation; IST/nested-exception runtime tests; deeper scheduler/IPC race coverage; complete memory/page-table dependency audit; DMA/IOMMU; UEFI and physical hardware.
- **Overall Part 1 remains PARTIAL / NOT COMPLETE.**
