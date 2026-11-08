# Native Spawn Implementation Log

## 2026-10-29 — Part 2 lifecycle gate

- Branch: `part2/native-thread-stack-lifecycle-20261029` (PR #307)
- Starting commit: `dd7dcd5592e98678554f9a2a747b1339ffacb77a` (`main`, Part 1 documentation merged as PR #306)
- Working tree: local source snapshot of the repository; Git metadata is unavailable in this workspace. The source snapshot was fetched from the repository default branch at the starting SHA. Branch publication and PR status are tracked separately in the final report.

### Baseline before code changes

- `make desktop-unit-test security-service-test && make` — **PASS**. Both host suites reported passed; complete ISO and native BIOS build completed.
- QEMU ISO boot, 20-second timeout — **PASS**. Expected timeout (124); all 37 workflow-required serial milestones were present.
- QEMU native BIOS boot, 20-second timeout — **PASS**. Expected timeout (124); all 41 workflow-required serial milestones were present.
- Baseline serial evidence is retained in `build/baseline-native-qemu.log`; the build and workflow scripts define the commands and required markers.

### Inspected interfaces and verified findings

Reviewed Part 1's `docs/FOUNDATION-AUDIT.md` and `docs/FOUNDATION-AUDIT-LOG.md`, `docs/NATIVE-SPAWN-CONTRACT.md`, `docs/PROCESS-LIFECYCLE.md`, process/thread/address-space/ELF/init sources, syscall ABI/dispatcher, paging/user-copy and heap code, tests, Makefile, and `.github/workflows/build.yml`.

- No public general spawn/exec syscall exists. Existing ELF boot test loads and enters a self-authored image through a dedicated CPL3 handoff; this is not a scheduler-created user thread.
- Before this change, `thread_init()` accepted an unchecked caller stack top and wrote a bootstrap frame there. Existing callers used static arrays; thread records were retained in a global registry after process wait, including records whose C storage was block-scoped.
- `process_wait()` already rejects a child with live/current threads and destroys its address space; the retained thread registry records and caller-owned stacks were outside that cleanup path.
- The shared IST1 interrupt path and incomplete per-thread CPL3 return/TSS stack ownership prevent safely implementing a general runnable user-thread spawn path here. No syscall number or competing process subsystem was added.
- The heap allocator is the existing kernel allocation interface. This change relies on its supervisor kernel-heap mapping; it does not assert hardware stack guards or per-thread memory protection.

### Code changes

- `thread_init()` now allocates a 16 KiB kernel stack from `kmalloc`, builds the existing kernel bootstrap frame on it, and records the owned base and size in `struct pros_thread`. Allocation failure returns before registry publication.
- Added `thread_reap_process()`. It preflights that all matching threads are dead, unqueued, and not using the current RSP, then unlinks records and releases their kernel stacks. Thread objects remain caller-owned and must remain alive until reaping.
- Added read-only reaping preflight for live/current/dead/queued/IPC-wait-channel state, and wired final `process_unregister()` to detach the registry records before process storage can disappear, after confirming the process is present in the registry. `process_wait()` preflights the owning reference count and thread records before irreversible cleanup, then copies the saved exit code only after successful unregister. This incorporates the stronger lifecycle constraints identified in open PR #156 without importing unrelated DML changes.
- Removed boot-test static stack arrays and updated the boot regression/workflow markers to require that process wait cleared the owned stack fields.
- Updated the native spawn and process lifecycle contracts to reflect what is implemented and the remaining CPL3 spawn gate.

### Verification

- Baseline results above are from before implementation and are not post-change evidence.
- Post-change host suites and full build: **PASS** (both host suites passed; ISO and native BIOS images produced). Post-change QEMU ISO boot: **PASS**, expected timeout exit 124 after milestones. Post-change native BIOS boot: **PASS**, expected timeout exit 124 after milestones. Workflow marker check: **PASS**, 37/37 ISO and 41/41 native BIOS markers, including the stack-reap regression and existing CPL3/IPC smoke test. Exact-head GitHub Actions for commit `5c1fd34974ddd8a822272e9aafdd0a53d5da2bb6`: Build/boot run #9009 (ID `40927529056`) **PASS**, including ISO and native BIOS QEMU steps; Security architecture run #8504 (ID `40927528759`) **PASS**. The final unregister ordering correction was included in that tested head.

### Limitations and next step

- No general native spawn/exec ABI; no proof of general CPL3 scheduled entry; no PE/Windows `.exe` support.
- Scheduler/registry synchronization is not SMP-safe. Kernel heap stacks have no guard pages, and thread object storage remains caller-owned.
- Next: resolve dedicated per-thread user interrupt/return-frame and TSS RSP0 ownership, establish scheduler synchronization, then design and test a minimal spawn ABI using the existing ELF loader before PE integration.

## Hosted CI refresh (2026-10-29)

- Pull request: [#307](https://github.com/prosenjit-Adhikary/ProsenjitOS/pull/307), ready for review, not merged.
- Exact PR head before this log refresh: `168d6d76600a03fe5837e1ac81ff7af33eb63a25`.
- Build/boot run #9011 (ID `40928122470`): **PASS**. Runner/toolchain, host-test, ISO build, ProsenjitFS diagnostic, ISO QEMU, and native BIOS QEMU steps all succeeded.
- Security architecture run #8506 (ID `40928122325`): **PASS**.
- The repository source work and local verification are complete for this stack-ownership slice. Full Part 2 general spawn/exec remains incomplete because the current architecture does not safely schedule a general CPL3 entry; this blocker and the next prerequisite are documented above.
- Next session: review the final PR diff/status, continue resolving per-thread CPL3 interrupt/return-frame and TSS RSP0 ownership, then implement/test spawn only after that foundation is safe. Do not claim PE/Windows `.exe` execution.


## 2026-11-08 — Per-thread TSS RSP0 ownership foundation (PR #491)

- Starting main commit: `05fdcd8561327ca1122626378297edae4be56565`.
- Added `cpu_set_kernel_stack()` and `cpu_get_kernel_stack()` assembly accessors for the long-mode TSS RSP0 field. The setter rejects null and non-16-byte-aligned values.
- Added scheduler-side validation that a selected thread owns a sufficiently sized, overflow-safe, 16-byte-aligned kernel stack in the canonical high-half kernel-heap range before publishing its top as RSP0.
- Both cooperative and interrupt-frame scheduling paths update RSP0. Address-space-switch failure restores the previous thread's RSP0; idle/detach and current-thread termination restore the bootstrap RSP0.
- `thread_reap_process()` now refuses to release a thread stack while its top remains published in TSS RSP0. Added QEMU regressions for setter validation, cooperative and interrupt-frame scheduler switches, failed-CR3 rollback, and active-TSS-stack reap refusal. Required markers are checked for ISO and native BIOS boots.
- Exact PR head `52d63882899c26698b73f99dcf892b5a306ff243`: Build and boot-test run #9936 (ID `42463797357`) **PASS**, including host tests, timer IRQ assembly invariants, ISO build, ProsenjitFS/exFAT diagnostics, ISO QEMU boot, and native BIOS QEMU boot. The log contains the TSS setter, cooperative switch, interrupt-frame switch, CR3 rollback, and active-stack reap markers for both ISO and native BIOS boots. Security architecture run #9433 (ID `42463797363`) **PASS**.

### Limitations and next step

- This is only TSS RSP0 ownership plumbing. The `INT 0x80` IDT gate still uses shared IST1; syscall frames can still reside on that shared stack.
- `pros_interrupt_frame` still represents the CPL0 three-word IRET tail; user RSP/SS are not yet part of the scheduler frame contract.
- No scheduler-created CPL3 thread, general spawn/exec syscall, CPL3 timer preemption, SMP scheduler locking, or Windows/Linux application compatibility is implemented by this change.
- Next: design and test a stable per-thread syscall/exception entry frame, move user syscall entry from shared IST1 to a per-thread entry stack only after bootstrap handoff is safe, then add CPL3 RSP/SS save/restore and a scheduler-owned first-user-entry frame. Keep CPL3 timer preemption disabled until those invariants pass.
