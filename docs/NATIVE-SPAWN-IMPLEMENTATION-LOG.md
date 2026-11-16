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
