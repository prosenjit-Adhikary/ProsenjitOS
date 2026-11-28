# Phase 1 Handover

## Current GitHub state
- Repository prosenjit-Adhikary/ProsenjitOS; main baseline 495291ecf1f2eda34d507e1c8ed09af641e9bb86.
- PR #642 open/unmerged; branch head a1654c51a20a9d610ecde24f8b9361878762d303.
- PR #652 open draft; latest confirmed head ca6501a98935d87507c273bc0f49ca954cf3c076 (refresh before writing).
- No CI/CL, build, QEMU, or Security workflow triggered. Runtime verification PENDING.
- No code or tests changed by this audit branch.

## Architecture-to-source conclusions
- Master spec treats subsystem documents as design authority but says design and test markers are not implementation or runtime proof.
- Process lifecycle requires no partial teardown; Native Spawn Contract distinguishes init ELF loading from general spawn/exec.
- Main has no general spawn/exec syscall/launch transaction; pros_init_load is the boot init path.
- Main process_wait releases handles/address space before final thread stack reap.
- Main thread registry walks lack cycle/count bounds.
- Main PMM and heap mutations lack local interrupt exclusion; PR #642 adds single-CPU IRQ-state protection and checked rollback. Verify call-path preemption before classifying exploitability.

## PR #642 classification (all 66 changed paths)

### Lifecycle-critical code and direct dependencies
- Process/thread/scheduler: kernel/process.c, kernel/include/process.h, kernel/thread.c, kernel/include/thread.h, kernel/context_switch.asm, kernel/idt.asm, kernel/kernel.c.
- Address-space/paging/allocator: kernel/address_space.c, kernel/include/address_space.h, kernel/paging.c, kernel/include/paging.h, kernel/memory.c, kernel/include/memory.h, kernel/heap.c, kernel/include/heap.h.
- Object/handle/IPC/user boundary/ABI: kernel/object.c, kernel/include/object.h, kernel/ipc.c, kernel/include/ipc.h, kernel/syscall.c, kernel/usercopy.c, kernel/usercopy.asm, kernel/include/usercopy.h, include/abi/syscall.h.
- Image/admission/VFS: kernel/elf_loader.c, kernel/include/elf_loader.h, kernel/security_exec.c, kernel/security_hardening.c, kernel/vfs.c, kernel/intent.c.
- Lifecycle docs: docs/PROCESS-LIFECYCLE.md, docs/NATIVE-SPAWN-CONTRACT.md, docs/NATIVE-SPAWN-IMPLEMENTATION-LOG.md, docs/IPC.md, docs/FOUNDATION-AUDIT.md, docs/DEBUG_LOG.md, docs/ROADMAP.md, docs/PHASE1-PANIC-HANDOFF-FRAME-AND-STACK-DESIGN.md.
- These dependencies are grouped by source relationships; full hunk-level review remains outstanding.

### Boot protocol (independent platform work; limited kernel/PMM integration dependency)
- boot/entry.asm, boot/native_stage2.asm, include/abi/boot.h, kernel/boot_protocol.c, kernel/include/prosenjit.h, kernel/kernel.c, kernel/memory.c.
- docs/BOOT-ARCHITECTURE.md, docs/BOOT.md.

### Storage and crash records (independent subsystem)
- kernel/ata_pio.c, kernel/include/ata_pio.h, kernel/block.c, kernel/include/block.h, kernel/partition.c, kernel/prosenjitfs.c, kernel/crash_record.c, kernel/include/crash_record.h, kernel/include/panic_handoff.h, kernel/panic_handoff.asm, kernel/panic_handoff.c.
- scripts/test_crash_record_two_boot.sh, docs/CRASH-RECORD-PERSISTENCE-INTEGRATION-AUDIT.md, docs/CRASH-RECORD-QEMU-TWO-BOOT-TEST-PLAN.md.
- ProsenjitFS/crash recovery is Phase 1 audit scope but no process-lifecycle dependency is established.

### CI/build behavior (separate review)
- .github/workflows/build.yml, .github/workflows/security-design.yml, scripts/build.sh, scripts/test_kernel_stack_guard.sh, scripts/test_timer_irq_assembly.sh.
- Workflow diffs alter runner selection/concurrency and branch triggers. Review independently; no workflow is triggered by this agent.
- PR #642 is broad and is not ready to mark ready until all hunks/dependencies are reviewed.

## Corruption and rollback tests (partial review)
- Registry validation caps nodes at 64, checks trusted identity slots, stack/physical ownership uniqueness and current-thread identity before ordinary scans.
- Run-queue validation is bounded and checks registered membership, predecessor links, READY state and head/tail consistency.
- Spawn rollback preflight checks thread membership/state, stack batch free preflight, queue count/membership, and RSP/RSP0 exclusion.
- Wait/reap preflights registry/queue/process identity, dead-thread stack state, CPU stack pointers and expected batch unmap/free before other process resource release.
- Five fault stages check PID lookup, address-space root, refcount, process linkage, kernel stack, and thread process/registry/queue fields. Additional cases inject wait-channel, peer thread, handle-entry, owned-page, and queued-process corruption and retry cleanup.
- Snapshot gaps remain: scalar counts and the current process/thread field checks do not establish complete preservation of every page-table node, owned-page link, registry slot and queue-neighbor link for every rejection. The two spawn rollback rejection fixtures now compare every handle entry field exactly (commit `462ef15fe32bf75e163cc2b84a50597395e62e0e`); review the remaining fixtures before closing this gate.
- These test bodies are source only; none were executed.

## Next work
1. Finish hunk-level review of all 66 changes, especially workflow/script changes and lifecycle dependencies.
2. Review remaining zero-mutation/retry fixtures and identify concrete test gaps; do not mark PR #642 ready.
3. Audit PMM/VMM/heap and ProsenjitFS crash/recovery; patch only reproducible gaps and avoid duplicating PR work.
4. Choose focused branch structure after dependency validation; no rewrite or merge.
5. Update all tracking files with actual changes and exact heads; keep runtime verification PENDING.

## Latest verified checkpoint — 2026-11-28
- Official `main`: `495291ecf1f2eda34d507e1c8ed09af641e9bb86`.
- PR #642 head: `462ef15fe32bf75e163cc2b84a50597395e62e0e`, open and draft. Exact per-entry handle snapshots replaced FNV fingerprints in both rollback preflight rejection fixtures.
- PR #652 tracking branch head before this handover update: `d4dd0adef60e0b91699cb4505d9496a938907df2`.
- Test execution: none. CI/CL, Build/QEMU, Security, and runtime verification remain PENDING.
- Exact next action: continue hunk-level PR #642 review, close remaining zero-mutation assertion gaps, then audit dependency-ready memory and filesystem paths without running workflows.


## Workflow scope checkpoint — 2026-11-28
- Reviewed `.github/workflows/build.yml` and `.github/workflows/security-design.yml` separately. Their prior PR hunks changed runner selection, concurrency cancellation, push triggers, timeout and diagnostic steps without a lifecycle dependency.
- Restored both exact files from main; verified equality via GitHub API. The PR changed-file inventory now has 64 paths and no workflow paths.
- PR #642 exact head: `58bcf8f808baa549cf3180af1a21e558c1afd562`; open, unmerged, draft.
- No CI/workflow execution. Runtime verification PENDING.
