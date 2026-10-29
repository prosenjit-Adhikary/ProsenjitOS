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
- Post-change host suites and full build: **PASS** (both host suites passed; ISO and native BIOS images produced). Post-change QEMU ISO boot: **PASS**, expected timeout exit 124 after milestones. Post-change native BIOS boot: **PASS**, expected timeout exit 124 after milestones. Workflow marker check: **PASS**, 37/37 ISO and 41/41 native BIOS markers, including the stack-reap regression and existing CPL3/IPC smoke test. GitHub Actions on the initial PR commit `74c6a59ed02bbf2a5b052d6e06959e6c93df2827`: queued at the latest check; no pass is claimed. The PR is updated with a final unregister ordering correction, which has been rebuilt and re-boot-tested locally; its new exact-head Actions results remain pending.

### Limitations and next step

- No general native spawn/exec ABI; no proof of general CPL3 scheduled entry; no PE/Windows `.exe` support.
- Scheduler/registry synchronization is not SMP-safe. Kernel heap stacks have no guard pages, and thread object storage remains caller-owned.
- Next: resolve dedicated per-thread user interrupt/return-frame and TSS RSP0 ownership, establish scheduler synchronization, then design and test a minimal spawn ABI using the existing ELF loader before PE integration.
