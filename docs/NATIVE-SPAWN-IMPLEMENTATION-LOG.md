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


## 2026-11-14 — Transactional process wait/reap hardening (PR #642)

- Branch: `phase1/transactional-wait-reap-20261114`.
- The source audit found that the previous wait path could close handles and destroy the address space before final thread-stack/TSS validation. A late failure could leave a ZOMBIE with partially released resources.
- Added full read-only preflight for the user page-table subtree, active CR3, table ownership, leaf-page uniqueness, owned-page list integrity and duplicate ownership records. Temporary physical-page bitmaps detect aliased page-table nodes across different branches and duplicate user-page mappings; local interrupts are disabled while the scratch bitmaps are in use.
- Added complete handle-table preflight for slot metadata, object type/lifetime, rights and per-table reference counts. `handle_table_close_all()` validates before its first close.
- Strengthened thread reaping checks for dead/unqueued/non-waiting state, every guarded kernel stack mapping, TSS RSP0 and current RSP. Stack release occurs before registry unlinking.
- Serialized process exit, wait, unregister and resource release with interrupt-state preservation under the existing single-CPU model. Child self-handles are allowed; external process-object references prevent reaping.
- Added QEMU regressions for external/self process references, active TSS stack, malformed second thread stack, active target CR3, corrupt handle refcount, retry safety, duplicate owned-page metadata and cross-branch page-table aliasing.
- Exact tested head: `081f57b8368c53b3c1abc8341376fcdac305dad4`. Build and boot workflow run ID `43630379873` **PASS**; security architecture workflow run ID `43630379868` **PASS**. ISO build, ISO QEMU boot and native BIOS QEMU boot passed. QEMU watchdog exit status 124 was expected after the required boot/self-test markers were observed. The selected runner was GitHub-hosted fallback, not the local runner.
- This is one Phase 1 hardening slice only. General spawn/exec, scheduler-created CPL3 launch, complete user-frame handoff, CPL3 timer preemption and SMP synchronization remain incomplete.

## 2026-11-15 — SYS_EXIT trampoline ABI alignment hardening (PR #642)

- Source audit found that the `isr128` internal-exit branch called
  `paging_switch_kernel()` directly after restoring the saved user GPRs.
  That point does not provide the SysV x86-64 C-call stack alignment.
- `process_exit()` has already switched the exiting process to the kernel
  address space before it publishes the internal exit sentinel. Removed the
  redundant direct call from the interrupt stub.
- The stub now switches to the 16-byte-aligned bootstrap stack and CALLs the
  assembly `user_mode_exit_trampoline`. That trampoline retains its own
  stack reset and aligned C calls, which are also needed by the exception
  exit path.
- Extended `scripts/test_timer_irq_assembly.sh` to require bootstrap-stack
  activation before the trampoline call and reject a direct unaligned
  `paging_switch_kernel` call in the ISR exit block.
- Source and test-script contents were cross-checked at the latest commit.
  Exact-head assembly/build/QEMU and Security CI are still required; this
  source review does not claim a runtime PASS.

- Follow-up test audit found that the mismatched owned-page unmap regression
  restored its corrupted physical-address field before calling unmap, so it
  did not actually exercise the rejection path. Corrected the test to keep
  the mismatch through the unmap attempt, verify failure/list preservation
  before dereferencing the record again, then restore metadata and verify the
  original mapping and allocation remain intact. Runtime execution remains
  pending exact-head CI.

- The first exact-head QEMU run exposed a remaining regression-fixture mistake:
  the test restored the ownership record after the destroy-preflight probe but
  did not re-corrupt it before the subsequent unmap probe. The runtime therefore
  exercised a matching unmap and correctly failed the expected-rejection check.
  The fixture now reintroduces the mismatched physical address immediately
  before unmap, seeds the output parameter with a sentinel, and verifies both
  the output and ownership list remain unchanged on rejection. Re-run Build/QEMU
  and Security checks on the corrected head before accepting this test.


## 2026-11-16 — Thread-stack release and registry integrity audit (PR #642)

**Scope:** source-only audit of thread-stack allocation/release, page-table walkers, physical-page release, and every thread-registry traversal. No source code was changed in this checkpoint.

### Confirmed source findings

- `thread_kernel_stack_allocate()` reserves a stack-slot bit before allocating/mapping four independently allocated pages. Its rollback calls the void `paging_unmap_page()` and `physical_page_free()` helpers, then clears the slot bit without proving that every mapping/page was actually released.
- `thread_kernel_stack_release()` has the same observability gap: after a one-record validation it calls void unmap/free helpers, clears page metadata as it goes, and returns success without a checked all-pages commit.
- `paging_unmap_page()` silently returns for invalid/missing mappings and does not require the leaf PTE to match the expected physical page. `physical_page_free()` silently ignores invalid, unmanaged, or already-free pages. The existing contiguous-free helper has all-or-nothing validation, but stack pages are independently allocated and need not be contiguous.
- `paging_translate()` and `paging_unmap_page()` dereference intermediate page-table addresses after checking PRESENT, without first proving those pages are allocator-owned, aligned page-table pages. Therefore the current stack validator cannot claim safety against arbitrary corrupted page-table pointers.
- Thread-registry traversals in initialization, live-thread checks, reapability, unpublished abort, process reap, and kernel-thread reap are not consistently bounded or cycle-checked. A corrupted cycle can hang a preflight before it has a chance to reject the registry.
- `thread_manager_init()` currently clears run-queue/current/registry state and the stack-slot bitmap without first proving that no live ownership state exists.
- `thread_init_user()` calls the common initializer, which publishes the NEW thread in the registry, before it fills the CPL3 interrupt frame. The spawn wrapper currently excludes local interrupts, but the public constructor does not itself enforce or document that publication boundary.

These are source-confirmed integrity/observability gaps. They do not establish that the normal boot path currently leaks a page or fails.

### Required next implementation slice

1. Define checked, expected-PA page-unmap preflight/commit operations that authenticate every intermediate page-table page before dereferencing it.
2. Define all-or-nothing non-contiguous physical-page batch-free preflight/commit operations; reject duplicate, unaligned, unmanaged, or already-free entries before changing allocator state.
3. Build one complete stack release plan covering all target stacks, mappings, physical pages, TSS RSP0/current RSP, and slot ownership. Do not clear page/slot/thread ownership metadata until the complete release commit is proven complete.
4. Use the same release contract for allocation rollback, unpublished-thread abort, process-thread reap, and kernel-thread reap. Keep a slot reserved or quarantine it if cleanup cannot be proven complete.
5. Introduce a bounded, read-only registry snapshot/validator (maximum 64 stack-owning slots), reject repeated nodes/cycles/over-limit chains, and compare stack slot, virtual range, and all four physical pages across the entire registry, including kernel threads and other processes.
6. Enforce a quiescent single-CPU lifecycle boundary across stack-slot reservation, mapping, thread-field initialization, frame construction, registry publication, validation, and release. Keep SMP explicitly unsupported.
7. Make `thread_manager_init()` reject non-empty queue/registry/current/slot state instead of silently clearing ownership.
8. Add rejection tests for cycles, cross-owner aliases, bitmap/registry disagreement, missing or mismatched PTEs, already-free pages, and failure on a later target with zero earlier mutation. Add success controls for multiple distinct stacks and exact allocator/registry restoration after allocation rollback.

### Verification and status

- Inspected PR branch `phase1/transactional-wait-reap-20261114` at `1fa1687ad870a406faa0c3de208699d9c5d9e41d`, including `kernel/thread.c`, `kernel/paging.c`, `kernel/memory.c`, `kernel/process.c`, relevant headers, and the current process-spawn contract.
- Existing exact-head Build/boot workflow run `43742860484` and Security architecture workflow run `43742860533` are reported **success** for that head. The combined-status endpoint returned no status contexts; the Actions run records report both completed successfully.
- No new build, QEMU, host test, source commit, or merge was performed for this audit entry.
- PR #642 remains open and unreviewed. Phase 1 remains incomplete; do not merge or begin Phase 2 until the thread-stack transaction dependencies, argv/envp content assertions, and all required exact-head checks are closed.


## 2026-11-16 — Checked non-contiguous stack-page release implementation

- Added `physical_page_free_batch()`: a bounded (1–64 page), non-contiguous release API. It validates alignment, managed/allocated state, and duplicate physical addresses across the entire batch before changing allocator bitmaps or free-page accounting.
- Added allocator self-test coverage for successful multi-page release, duplicate-address rejection, and misaligned-member rejection. Rejection cases assert that neither page nor free-page accounting changes.
- Added `paging_unmap_pages_expected()`: a bounded expected-PA unmap operation. It validates all VA/PA pairs, rejects duplicate VAs/physical pages, authenticates the kernel PML4 and every intermediate page-table page as allocator-owned before dereferencing, and validates all leaf PTEs before clearing any of them.
- Added `paging_query_physical_checked()` and moved kernel stack guard/data mapping validation to checked page-table walks rather than the unchecked legacy translation helper.
- Updated stack allocation rollback and stack release to use the checked batch operations. Interrupts are excluded across stack release and allocator batch-free on the current single-CPU model. If rollback cannot prove cleanup, its slot remains reserved rather than being recycled.
- Source changes are on PR #642 branch `phase1/transactional-wait-reap-20261114`. Exact latest commit at this log update: pending final verification.
- These are source changes and self-test additions only. No compiler, host-test, QEMU, BIOS boot, or Security PASS is claimed until the exact-head required workflows complete.
- Remaining Phase 1 work: bounded whole-registry snapshot/cycle and cross-owner alias validation; fail-closed thread-manager initialization; publication boundary for complete CPL3 frames; argv/envp byte-content assertions; full transactional spawn/wait/reap regression matrix.


## 2026-11-16 — Checked stack release and bounded registry hardening (PR #642)

- Added bounded `physical_page_free_batch()` for up to 64 distinct non-contiguous pages. It validates alignment, managed/allocated state, and duplicate addresses before any allocator bitmap/free-count mutation. Physical-memory self-tests cover successful batch release, duplicate rejection, and misaligned-member rejection with unchanged state on rejection.
- Added `paging_unmap_pages_expected()` for up to 64 unique kernel VA->expected-PA pairs. It validates every pair before clearing any PTE, rejects duplicate VAs/physical pages, and checks that the kernel PML4 and each intermediate table page are allocator-owned before dereferencing them.
- Added `paging_query_physical_checked()`; thread stack guard/data mapping validation no longer relies on the legacy unchecked `paging_translate()` walk.
- Thread stack allocation rollback and release now use checked batch unmap/free. Interrupts are excluded over stack release on the supported single-CPU model. If cleanup cannot be proven, the slot is retained and marked quarantined rather than recycled.
- Added a bounded global registry validator: at most 64 stack-owning nodes; repeated nodes/cycles, duplicate slots, cross-thread physical-page aliases, invalid stack ownership, and slot-bitmap/registry disagreement are rejected. Lifecycle traversals and statistics validate before walking the registry.
- `thread_manager_init()` now fails closed if run queue, current thread, registry, live slot, or quarantined slot state is present; it no longer clears ownership.
- Added thread self-tests for a registry cycle, a physical-page alias, and manager reinitialization preserving live ownership.
- Updated `scripts/test_kernel_stack_guard.sh` to assert the checked translation and batch release contract rather than obsolete unchecked/one-page operations.
- First Build run exposed the stale source-invariant script; after updating it, the next Build run exposed a missing closing brace in the allocation rollback block. Both were corrected.
- Exact tested source head: `1f4d2c9ed249855878234a436741dfee926af171`. Build/boot workflow run `43865301602`: **PASS**, including warnings-as-errors kernel build, physical-memory/paging/thread self-tests, ISO QEMU boot and native BIOS QEMU boot. Security architecture workflow run `43865301636`: **PASS**. Both runs used the GitHub-hosted fallback; local runner was not selected.
- Still incomplete: enforce the interrupt-exclusion/publication contract inside the public thread constructors so the CPL3 initial frame is complete before registry visibility; finish argv/envp byte-content assertions and full wait/reap failure-injection coverage. SMP remains unsupported. Do not mark Phase 1 complete or merge PR #642 until these remaining gates are closed and exact-head checks pass.


## 2026-11-17 — Phase 1 gate reconciliation and remaining wait/reap matrix

**Scope:** read-only cross-check of the current PR head `93df13538bd0c60c181a506cb25b763b5af6a156` against the Phase 1 exit gate. This entry reconciles older pending notes with the implementation now present in the source; it does not claim that Phase 1 is complete.

### Gates confirmed present in source

- Public `thread_init()`, `thread_init_kernel()`, and `thread_init_user()` each save the incoming interrupt flags, execute the common constructor with interrupts excluded, and restore the original flags on return.
- `thread_init_user()` validates the user entry mapping and writable stack endpoint, zero-initializes the complete saved interrupt frame, sets the CPL3 selectors and IRET tail, and passes that frame into `thread_initialize()`.
- `thread_initialize()` allocates and initializes the kernel stack and all thread fields, copies the optional user frame into thread-owned storage, and writes the registry link only as its final publication operation. The constructor therefore does not expose an incomplete CPL3 frame through the registry.
- `process_spawn_internal()` independently holds the process lifecycle interrupt exclusion across ELF loading, user-thread construction, process registration, and final enqueue/publication. Rollback remains staged across loader, thread, process-registry, and run-queue failures.
- `pros_elf_selftest()` now performs byte-content checks for both argv strings and the environment string, in addition to argc, argv terminator, envp terminator, pointer bounds, and stack alignment. The check reads the actual mapped user stack rather than trusting only the loader's return values.
- The process wait path has read-only preflight for process-registry integrity, handle-table teardown, address-space destruction, and thread reapability before releasing resources. Existing tests cover invalid parent/child relationships, process-list cycle rejection and retry, child exit status, external/self object references, active CR3/TSS stack, malformed thread stack, corrupt handle reference counts, duplicate ownership metadata, and selected teardown retry cases.

### Remaining Phase 1 work — do not close this gate yet

The full wait/reap failure-injection matrix is not yet demonstrated. Extend it to inject a failure at each independently fallible preflight/commit boundary and assert, for every rejected attempt:

1. the zombie remains registered and retryable;
2. the caller's exit-code output remains unchanged;
3. handle slots, object reference counts, and rights metadata remain unchanged;
4. address-space root, page-table ownership, leaf mappings, and owned-page records remain unchanged;
5. thread registry, run-queue links, TSS RSP0, current RSP, stack-slot bitmap, quarantine bitmap, and all stack pages remain unchanged;
6. a subsequent retry after removing the injected fault reaps exactly once and restores allocator/object/registry accounting.

The tests must include a late-target failure after earlier targets have passed read-only validation, proving zero earlier mutation. Do not introduce production-only unsafe mutation hooks; use narrowly scoped test-only fault injection or corruption fixtures with guaranteed restoration. Also continue auditing whether the commit phase can fail after the aggregate read-only preflight; if so, make the teardown staged/retryable or strengthen the commit contract rather than assuming the preflight guarantees success.

### Verification snapshot

- Current PR: `#642`, branch `phase1/transactional-wait-reap-20261114`, base `main`.
- Source head reviewed: `93df13538bd0c60c181a506cb25b763b5af6a156`.
- Build and boot-test workflow run `43890281647`: **success** for this exact head.
- Security architecture workflow run `43890281631`: **success** for this exact head.
- These workflow results are the current exact-head evidence; no additional runtime run was performed as part of this source/documentation reconciliation.
- PR #642 remains open and unreviewed. Phase 1 remains incomplete; do not merge it or start Phase 2 until the complete wait/reap failure-injection matrix and all exact-head checks pass.


## 2026-11-17 — local-runner CI recovery and wait/reap matrix checkpoint

**Branch head:** `fe684621a527994fe2c7b4966afa7d8462a672f9` (`phase1/transactional-wait-reap-20261114`).  
**PR:** #642, open and unmerged.

### CI runner correction and verification

- The previous runner selector silently chose GitHub-hosted fallback when `RUNNER_STATUS_TOKEN` was absent. The Build and Security workflows now select the self-hosted `[self-hosted, linux, x64]` runner directly; a missing local runner will queue rather than silently fall back.
- Build and boot-test run [`44062761710`](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/44062761710): **success**. Actual runner name: `office`, machine: `debian`.
- Security architecture run [`44062761646`](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/44062761646): **success**. Actual runner name: `Office1`, machine: `debian`.
- Both PR workflows checked out the generated PR merge ref `39772fad5cfcb4d275bed79a5ba0ad1161b3c31b`, which includes branch head `fe684621a527994fe2c7b4966afa7d8462a672f9` against base `aad3c14c0814a542a27c2a86096c768e7b9bda99`. The run metadata is associated with the branch-head commit; the build log records the merge-ref checkout.
- Build log contains successful markers for process wait/reap full-preflight retry, thread-stack reap and TSS guard, scheduler rollback, native user-process launch/exit/wait/reap, ProsenjitFS format/mount/allocation/journal/recovery, storage, USB Mass Storage, and xHCI tests.
- The native BIOS QEMU invocation ended with timeout exit status 124 after the serial log emitted the required regression markers. The workflow itself concluded **success**. Preserve this distinction: required workflow checks passed, but the QEMU timeout is visible in the raw log and must not be described as a clean zero exit.
- The workflow's previous missing-`mkfs.exfat` dependency gate was removed from the generic prerequisite loop; formatter-specific coverage still needs to be reviewed against the actual later exFAT test/setup steps.

### Source cross-check at this branch head

- `process_wait()` aggregates process-list membership/cycle validation, self-handle reference accounting, handle-table close preflight, address-space destroy preflight, current-process exclusion, and thread reapability before its first destructive operation.
- The added wait/reap regression independently corrupts the reserved handle slot and address-space root, verifies rejection with unchanged exit-code sentinel, zombie state, object refcount and process-list membership, restores each fixture, then verifies successful retry and final resource/object release.
- The global thread registry validator is bounded by the 64-slot allocator and rejects repeated nodes/cycles, duplicate stack slots, cross-thread physical-page aliases, and slot/registry bitmap disagreement. Existing thread self-tests exercise a registry cycle and physical-page alias.
- Aggregate spawn rollback preflight and bounded run-queue validation remain present in source.

### Remaining gates — Phase 1 is not complete

- Expand wait/reap corruption coverage beyond the two new independent wait fixtures: prove zero mutation for run-queue endpoint/link corruption, thread registry corruption, TSS/current-RSP/stack ownership failures, handle metadata/refcount/rights faults, address-space page-table/owned-page faults, and late-target failure after earlier targets pass preflight.
- Snapshot and compare all relevant handles, rights, reference counts, process/thread registry links, run queue, PTEs, physical allocation ownership, stack-slot/quarantine bitmaps, and exit-code output for every rejected attempt; then verify exactly-once retry.
- Reconcile the raw QEMU timeout with the boot harness contract and ensure the expected completion marker is emitted before timeout. Do not silently relabel exit 124 as a clean emulator exit.
- Complete the remaining PMM/VMM transactional heap-growth and panic/fault-detection audits.
- PR #642 remains open and unmerged. These two successful workflows do not close the remaining Phase 1 gates and do not authorize merge or Phase 2.

## 2026-11-17 — transactional wait/reap exact-head verification

**Tested source head:** `e33b1f80f02796ee4c7c2d88b8fc5d602e8d2e50` on PR #642.

### Exact-head CI evidence

- Build and boot-test workflow run [`44067056952`](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/44067056952): **success**.
- Security architecture workflow run [`44067057110`](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/44067057110): **success**.
- The Build job ran on the self-hosted runner `office` (Linux/Debian) and verified that the PR merge checkout's second parent was exactly the tested branch head `e33b1f80...`.
- The warnings-as-errors kernel build, ProsenjitFS block-init diagnostic, exFAT formatter check, ISO QEMU boot and native BIOS QEMU boot steps all completed successfully.
- Both QEMU invocations ended with timeout status 124 after the required serial milestones were emitted. The harness explicitly accepts 124 as its bounded observation timeout; this is **not** described as a clean QEMU exit status 0.
- The logs contain the corrected kernel heap deep-invariant marker, cooperative-scheduler TSS RSP0 marker, ProsenjitFS format/mount/allocation/journal/recovery marker, native spawn exit/wait/reap transaction marker, and CPL3 user-mode/IPC marker for both ISO and native BIOS boot paths.
- The native BIOS debugcon phase trace file **was created** in run `44067831089`; the job log prints the complete port-`0xE9` trace, beginning `123ABCD` and ending `FG`. This verifies stage-1 entry, stage-1 disk-read/jump markers, stage-2 E820/load milestones, repeated protected/real-mode kernel-copy transitions, and the final protected-mode handoff marker. Native BIOS serial milestones also passed; QEMU ended with accepted timeout status 124.

### Source cross-check

- Re-read `process_wait()`, `process_unregister()`, `process_release_resources()`, thread-registry/run-queue validation, `thread_process_reapable()`, `thread_reap_process()`, `thread_kernel_stack_release()`, `physical_page_free_batch()`, `paging_unmap_pages_expected()`, and address-space destruction preflight/commit.
- The aggregate wait preflight is present before the first destructive operation. It checks process-registry integrity and exact membership, self-handle reference accounting, current-process exclusion, complete handle-table close preflight, address-space destruction preflight, and thread reapability.
- Thread reapability validates the bounded global registry and run queue, excludes current/live threads, and validates dead-thread state, queue membership, guarded stack mappings, physical-page allocation, TSS RSP0 and current RSP before release. Stack unmapping validates the full expected-PA batch before clearing PTEs; physical batch-free validates all pages before changing allocator bitmaps.
- No new confirmed source defect was established by this read-only cross-check. The commit path still needs the broader late-target fault-injection proof described below; do not infer arbitrary-corruption or SMP safety from the current single-CPU preflight design.

### Remaining Phase 1 gates

- Complete the wait/reap failure-injection matrix at every independently fallible preflight/commit boundary, including a late-target failure after earlier targets have passed validation.
- For each rejected attempt, snapshot and prove unchanged caller exit-code output, zombie/process registry membership, handles/rights/object references, address-space root/page-table ownership/leaf mappings, thread registry/run queue, TSS RSP0/current RSP, stack-slot/quarantine bitmaps and all owned stack pages; then prove exactly-once successful retry after restoring the fixture.
- Finish the remaining PMM/VMM transactional heap-growth and panic/fault-detection audits.
- PR #642 remains open and unmerged. Phase 1 remains incomplete; do not merge or begin Phase 2 until these gates and exact-head required checks are closed.


## 2026-11-17 — correction and PMM/VMM heap-growth audit

- Corrected the preceding exact-head verification entry after re-reading the complete Build job log: run `44067831089` **did create** `build/native-qemu-debugcon.log` and printed the complete port-`0xE9` trace (`123ABCD...FG`). The earlier missing-file interpretation was incorrect. Debugcon phase coverage is verified for this run; serial milestone validation passed on both ISO and native BIOS paths, and native BIOS QEMU ended with the accepted timeout status 124.
- Re-read `kernel/heap.c` growth/rollback, `kernel/paging.c` map/unmap, and `kernel/memory.c` allocator batch-free contracts at source head `0320a313f95f788e9ff0c99ead76d9aacea23cc2`.
- Heap growth keeps `heap_end` uncommitted until a new block is linked. On a partial mapping failure it resolves and validates each mapped VA->PA pair, unmaps the complete bounded batch, then frees the corresponding physical pages. If either rollback proof/commit fails, it sets `heap_growth_poisoned` and fails closed rather than allowing later allocations to reuse uncertain state.
- The exact-head Build run `44067831089` passed the kernel warnings-as-errors build and the heap deep-invariant self-test in both ISO and native BIOS QEMU serial logs, including the forced partial-growth rollback and fresh-extension retry.
- No new confirmed PMM/VMM heap-growth defect was established by this source cross-check. Remaining coverage should include page-allocation failure before the first map, mapping failure with a partially created page-table hierarchy, rollback rejection caused by a corrupted late VA->PA pair, and proof that poisoned heap state rejects all subsequent allocations without modifying allocator or heap metadata.
- This is an audit checkpoint, not a claim that every possible allocator/page-table corruption or hardware failure is covered. PR #642 remains open and unmerged; Phase 1 remains incomplete.


## 2026-11-17 — wait/reap regression and exception-path source audit

- Re-read `kernel/process.c`, `kernel/thread.c`, `kernel/kernel.c`, `kernel/idt.asm`, and `kernel/crash_record.c` at source head `2a560125a8b2667de7a4d28b0ca30b85da4e70c1`.
- Correction to the earlier remaining-gates wording: a late-target multi-thread rejection test already exists in `kernel/kernel.c`. It corrupts the second process-owned thread's first stack-page record, calls both `process_wait()` and `thread_reap_process()`, checks that the first and second stack descriptors/registry links, process state/registry membership, address-space root, handle, object reference count, TSS RSP0 and caller exit-code sentinel remain intact, restores the second page record, and later exercises successful process reap. This is useful existing coverage; do not duplicate it blindly.
- `thread_kernel_selftest()` already injects a registry self-cycle, an intra-thread duplicate physical stack page, invalid run-queue endpoints/links, and a queue cycle, restoring each fixture. `process_selftest()` already injects a process-list cycle and retries after restoring it. The run-queue wait helper also routes malformed endpoints through the real `process_wait()` preflight.
- The existing tests do not yet constitute the complete requested snapshot matrix: they do not explicitly snapshot every stack-slot and quarantine bitmap word, every owned physical page's allocator bit, all page-table ownership metadata, or all unrelated queue links for every late-target failure. Cross-thread physical-page alias and duplicate-slot validator conditions are present in source, but a dedicated end-to-end wait/reap rejection-and-retry fixture for each is not established by this audit. Treat those as targeted remaining coverage, not as confirmed defects.
- Exception path audit: IDT setup assigns NMI to IST1, double fault to IST2, and page fault to IST3. `exception_dispatch()` has a nested-entry guard that disables interrupts and halts; ordinary exceptions update counters/last-vector/RIP, report vector/error/RIP/CR2 over serial, stage a crash record (capture-only), and record health telemetry. A usercopy page fault follows the fixup path, clears its active marker, adjusts saved RIP, clears the dispatch guard and returns. Other unhandled exceptions display a VGA fatal message and halt.
- Crash-record persistence is intentionally not performed from exception context: `exception_dispatch()` calls `pros_crash_record_capture()`, while `pros_crash_record_flush_pending()` performs block-device write/flush. A repository code search for `pros_crash_record_flush_pending` returned only its declaration and implementation (the other references are inside the crash-record self-test); no production call site was found on the default branch. This is a concrete persistence-integration gap to resolve: add/identify a safe non-fault-context caller, then verify pending-record durability and retry after storage errors. Do not move block I/O into the exception handler.
- No source code was changed in this audit. This is a source-coverage checkpoint only; no build or runtime test was run for this docs update. PR #642 remains open/unmerged and Phase 1 remains incomplete.


## 2026-11-18 — cross-thread stack-alias regression and wait/reap coverage review

**PR:** #642 — `phase1/transactional-wait-reap-20261114`  
**Reviewed source head:** `34ea7ba851cfb217f961d56f65a7406506a49697`  
**Scope:** read-only source review of the newest alias regression, process wait preflight, and current exact-head CI evidence.

### Exact-head CI

- Build and boot-test run [#44260473712](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/44260473712): **success**.
- Security architecture run [#44260473711](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/44260473711): **success**.
- Both runs are associated with the reviewed commit SHA above. This verifies the required hosted workflows for this head; it does not replace the additional targeted wait/reap regression below.

### Source review result

- The latest `thread_kernel_selftest()` addition creates a second otherwise-valid kernel thread stack, replaces one mapped page with the first thread's physical stack page, verifies that each per-thread stack validator still accepts its own metadata/mapping, and verifies that the global registry validator rejects the cross-record physical-page alias. It then restores an independently allocated page and reaps the fixture on the normal path.
- This closes the previously identified **registry-level pairwise alias branch test**. It is stronger than merely duplicating a stack-slot index because the two stack records and mappings are independently valid before the global alias check.
- `process_wait()` still correctly runs the aggregate read-only gates before releasing resources: self-handle reference accounting, process registry validation, handle-table close preflight, address-space destroy preflight, and thread-process reapability.
- The existing process wait/reap corruption fixtures cover external process-object references, malformed run-queue endpoints, active TSS RSP0/current stack, malformed later-thread stack metadata, active target CR3, bad handle-object refcount, and corrupted owned-page metadata. They preserve representative process state, registry links, resource handles/refcounts, and the caller's exit-code sentinel, then restore fixtures and reach a successful final reap.

### Remaining targeted coverage (not confirmed source defects)

- The new cross-thread alias test is currently a **thread-registry self-test**, not an end-to-end `process_wait()` rejection/retry fixture. Add a focused process-owned two-thread fixture that aliases a mapped physical stack page while preserving each per-thread mapping check, invokes both `process_wait()` and `thread_reap_process()`, proves rejection with no teardown, restores the original mapping/metadata, and then proves exactly-once successful reap.
- Extend zero-mutation snapshots to explicitly include the stack allocator's allocation/quarantine bitmap words and physical allocator ownership for every stack page in the late-target rejection case. Existing checks cover important descriptors and links but do not explicitly compare every one of these global metadata words.
- Continue the separate PMM/VMM failure-injection and crash-record persistence integration gates documented above. Do not move storage I/O into exception context; identify a safe deferred flush caller.

### Disposition

The latest exact-head Build and Security workflows pass, and the cross-thread alias validator now has a dedicated registry-level regression. **Phase 1 remains IN PROGRESS / NOT EXIT-GATE COMPLETE.** PR #642 remains open and unmerged. Do not merge or begin Phase 2 until the end-to-end alias wait/reap rejection-and-retry fixture, remaining global ownership snapshots, and all other Phase 1 gates are reviewed and pass on one exact head.


## 2026-11-18 — Native spawn rollback and initial-stack work package verified

**Reviewed PR head:** `646388874996d60dc6bfc88457d5d5f26354273a` (documentation reconciliation commit follows this source head).

### Source and regression evidence

- `kernel/process.c`: `process_spawn_internal()` keeps construction non-runnable until ELF loading, argv/envp stack construction, initial user-thread setup, process registration, and final queue publication. Deterministic failpoints 1–5 cover address-space initialization, post-loader construction, thread/kernel-stack creation, process registration, and queue publication. Failpoints 6–7 deliberately reject aggregate rollback preflight before mutation for unpublished and registered/queued children; the self-test restores the injected invariant and retries cleanup.
- `kernel/elf_loader.c`: `pros_elf_selftest()` checks the mapped initial stack's argc, argv/envp NULL terminators, pointer bounds, 16-byte alignment, and exact NUL-terminated bytes for both arguments and the environment string.
- `kernel/kernel.c`: the scheduler-created child fixture independently checks the actual child stack's argc, argv/envp terminators, pointer bounds, alignment, and exact `/init`, `elf-runtime`, and `PROS_ELF_RUNTIME=1` bytes before the CPL3 handoff. The boot path also verifies scheduler handoff rollback, SYS_EXIT(42), and parent exit/wait/reap.
- This closes the prior argv/envp-content assertion gap and the spawn-construction/rollback stage coverage gap. It does not prove arbitrary hardware-failure recovery or every possible cleanup fault after a commit preflight.

### Exact-head workflow verification

- Build and boot-test: [run 44292223796](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/44292223796) — **success**.
- Security architecture: [run 44292223802](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/44292223802) — **success**.
- The Build workflow's merge-checkout verification step passed for PR head `646388874996d60dc6bfc88457d5d5f26354273a`; the generated merge checkout was `ede5bb6`.
- ISO QEMU and Native BIOS QEMU both emitted the required spawn rollback, argv/envp verification, scheduler-created CPL3 launch, SYS_EXIT, and exit/wait/reap markers. Native BIOS QEMU ended at the configured observation timeout (exit 124) after the required markers; this is not represented as a natural kernel shutdown.
- The runner selector used GitHub-hosted fallback; no local-runner execution is claimed.

### Remaining Phase 1 gates

This work package is **CLOSED** for the current source head. Phase 1 remains **OPEN**: complete wait/reap zero-mutation/fault-injection coverage, panic/fault handoff, and all other master Phase 1 exit criteria still require implementation/review and exact-head evidence. PR #642 remains open and unmerged; no Phase 2 work is started here.


## 2026-11-18 — Cross-thread alias rejection now snapshots allocator state

- Added `thread_stack_allocator_snapshot()`, a read-only, interrupt-state-preserving snapshot of the 64-slot allocation and quarantine bitmaps. It intentionally avoids registry validation so corruption tests can snapshot allocator ownership while the registry is deliberately malformed.
- Extended the real process wait/reap cross-thread stack-page alias fixture to snapshot both slot bitmaps, physical allocator free-page count, all eight stack-page records and allocation states, and every corresponding stack VA-to-PA mapping before rejection.
- After both `process_wait()` and direct `thread_reap_process()` reject the alias, the fixture requires every snapshot to remain unchanged and the deliberately displaced page to remain free. It then restores a unique allocated page and rechecks reapability.
- Source head: `0168b8bed009efdb824762f5b8b21b8ea97a8397`. Build/boot run [44303434840](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/44303434840) and Security run [44303434802](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/44303434802) both passed. Exact merge checkout: `11fe330d`; warnings-as-errors build, ISO QEMU, and Native BIOS QEMU passed. Native BIOS QEMU used the bounded timeout 124 after required markers. GitHub-hosted fallback was selected because repository runners could not be inspected.
- This closes one specifically identified end-to-end zero-mutation snapshot gap. The broader wait/reap matrix, panic handoff, and Phase 1 exit gate remain open. PR #642 remains open/unmerged.


## 2026-11-25 — Heap rollback-poison fail-closed regression

**Inspected source head before this change:** `0b34dce8e97b7b698257ec7133a0a4241e8450b0`.  
**Implementation commits:** `df9024e712377290fe9adef4e5e9fe1df56e7fa5`, `4728ee97aecc778b2196b7f49c258bc8c5eb058e`, and `92b61ed9e39791aa8423bd0c10efbd64d09ea27e`.

### Source-first coverage reconciliation

- Re-read the current PMM batch-free, mixed-free, paging reverse-map, large-leaf, page-table hierarchy alias, and heap-growth tests before adding coverage.
- The existing source already exercises duplicate/misaligned/stale PMM batch members, mixed user/table allocator-type and cross-array alias rejection, stale bitmap members, reverse-map ownership, 2-MiB/1-GiB aliases, duplicate page-table child pointers, page-table allocation failure at the 2-MiB boundary, first/mid-growth PMM failures, and fresh-extension retry. Those cases were not duplicated.
- One focused gap remained: the heap's fail-closed `heap_growth_poisoned` branch had no direct regression proving that a rollback rejection blocks subsequent allocations while preserving the committed frontier and heap usage.

### Implementation

- Added a one-shot rollback-rejection test seam to `kernel/heap.c`.
- Extended `heap_selftest()` to force one page of temporary heap growth, reject the rollback commit, and assert that the heap is poisoned, the frontier and `heap_used` remain unchanged, and the retained page remains allocated.
- The test then verifies a second large allocation is rejected while poisoned. It explicitly unmaps/frees the deliberately retained page and only then clears the poison state so the rest of boot-time self-tests can continue.
- This seam validates the heap's fail-closed response to a rollback rejection. It does **not** replace the separate paging self-tests that inject actual alias/corruption conditions into the paging preflight.

### Verification and disposition

- The change is committed to PR #642. The exact new head must pass the warnings-as-errors Build/boot workflow and Security workflow before this regression is considered runtime-verified.
- No Build/QEMU/Security PASS is claimed for this head until the workflow runs identify this exact commit.
- PR #642 remains open and unmerged. Phase 1 remains incomplete; do not begin Phase 2.
