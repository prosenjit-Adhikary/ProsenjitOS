# Native VFS Concurrency Audit — PR #305 Follow-up

**Status:** The VFS findings below are source-level risk findings, not reproduced races. Scheduler runtime changes on this branch have passed the current Build/boot-test workflow, including ISO and native BIOS QEMU boot tests; Security architecture checks also passed on the same head before this documentation update.
**Audited PR head before this documentation update:** `1daec37bd3e0cf1b4991b74823bd6f3624a12f72` (2026-10-29).
**Scope:** `kernel/vfs.c`, `kernel/include/vfs.h`, `kernel/thread.c`, `kernel/kernel.c`, `kernel/ipc.c`, and `docs/SCHEDULER-AWARE-MUTEX-PREREQUISITES.md`.
**Purpose:** Keep VFS concurrency risks distinct from the scheduler regression that has been built and boot-tested; do not infer general VFS concurrency safety from a successful boot.

## Findings confirmed from source

### VFS file position is shared mutable state without a per-handle synchronization contract

- `struct pros_vfs_file` contains a mutable `uint64_t position` and no lock/operation-reference field.
- `vfs_file_seek()` validates the handle and then assigns `file->position = position` directly.
- `vfs_file_read()` and `vfs_file_write()` dispatch through provider callbacks using the handle position and advance it after successful I/O. The ProsenjitFS provider I/O lock serializes provider-side data operations, but that does not by itself serialize the VFS-level position read/update with seek or with another operation on the same handle.
- Therefore concurrent read/write/seek on the same handle is **not proven safe**. Treat this as a race-risk finding, not a reproduced failure.

### Mount lifecycle requires an operation-entry/drain protocol before general concurrency can be claimed

- File and directory entry points check mount/handle state, while mount deactivation has busy checks for existing references.
- A state check alone is not an atomic lifetime guarantee if operation entry and deactivation can race. Before adding a per-handle or sleeping lock, define how an operation pins the mount/vnode, how unmount blocks new operations, and how active operations drain before resources are released.
- Do not add a naive blocking mutex yet: `docs/SCHEDULER-AWARE-MUTEX-PREREQUISITES.md` records that scheduler block/wakeup does not currently provide the proven atomic waiter-registration and handoff contract required to prevent lost wakeups.

### Namespace operations are a separate concurrency domain

- Rename/unlink/mkdir/rmdir require review for lookup-to-mutation races, vnode lifetime, parent-directory serialization, and lock ordering.
- The filesystem I/O lock does not automatically make a multi-step VFS namespace transaction atomic.
- Do not claim this is fixed until the scheduler/locking design and deterministic tests cover competing namespace operations.

## Work to implement when safe synchronization primitives are available

1. Introduce an explicit VFS operation lifetime protocol: reject new operations after unmount begins, pin the mount/vnode while an operation is active, drain active operations, then flush/deactivate. Define rollback on flush failure.
2. Add a per-open-handle operation serialization contract for read/write/seek/truncate/close. Ensure the position and handle lifetime are protected as one coherent operation. Do not hold a spinlock across potentially blocking storage I/O.
3. Define and document lock order across scheduler guard, handle state, namespace/vnode state, ProsenjitFS I/O lock, block device, and allocator.
4. Audit namespace mutation paths and make lookup/validation/mutation/lifetime rules explicit; add concurrent rename-vs-unlink and cross-directory rename tests.
5. Add deterministic scheduler/interleaving tests for: seek racing with read/write; close racing with I/O; unmount racing with open/read/write; rename racing with lookup/unlink; waiter registration racing with unlock; and cancellation/teardown of blocked waiters.
6. Run exact-head Build/boot-test and Security architecture workflows, then inspect ISO QEMU, native BIOS QEMU, filesystem integrity, and persistent ATA→MBR→ProsenjitFS markers. Record the exact SHA and results.

## Verification boundary

- Source review only; this document does not change runtime behavior.
- No build, local runner, QEMU, stress test, or physical power-loss test was executed for this checkpoint.
- Existing passing CI on earlier commits does not verify this audit commit.
- The project must not claim general concurrent VFS safety or physical-drive power-loss durability until those acceptance gates pass.


## Additional concrete findings from the follow-up audit

### Close-vs-operation is a handle-lifetime race, not only a position race

- `vfs_file_close()` calls the provider's `close` callback before marking `file->open = 0` and clearing the handle fields.
- Read/write/truncate validate `open` and then use `ops`, `vnode`, `mount`, `private_data`, and `position` across a provider callback. There is no shared operation reference or per-handle gate in `struct pros_vfs_file`.
- If the same handle is shared between concurrent callers, close can race with an operation after its initial validation. The source does not prove that provider state remains alive until that operation finishes. This is not a reproduced runtime failure; it is an unclosed lifetime guarantee.

### Vnode reference counts are not concurrency primitives

- `vfs_vnode_get()` and `vfs_vnode_put()` perform ordinary increment/decrement operations on `refcount`; no atomic operation or lock is used in these functions.
- This is safe only if all reference ownership transitions are externally serialized. Current interfaces do not express that requirement in the type/API contract. Before claiming concurrent lookup/open/unlink safety, audit every call site and establish the lock that protects refcount, parent retention, release callbacks, and initialized-state transitions.
- The root-refcount check in `vfs_mount_deactivate()` is also a check-then-act sequence. It prevents unmount when references are already visible, but does not alone serialize a concurrent operation that is acquiring a new reference.

### Directory handles have the same close/lifetime concern

- `vfs_dir_read()` checks `dir->open` and uses the vnode and mutable `cookie`; `vfs_dir_close()` drops the vnode reference and clears those fields without a per-directory operation gate.
- Concurrent readdir/close on the same directory handle is therefore not established as safe. Include it in the same handle-lifetime and deterministic interleaving test family.

## Updated severity and scope

These findings do **not** prove a specific exploitable race on the current single-CPU execution model. They establish that the API does not provide a general concurrency guarantee: correctness depends on scheduling, caller ownership, and serialization assumptions that are not enforced by the file/directory handle structures themselves. Do not patch this by adding a spinlock around provider I/O; synchronous storage I/O may block, and the scheduler-aware mutex prerequisites must be satisfied first.


## Scheduler/IPC cross-check (follow-up)

The current source review found an important qualification that must be preserved:

- `kernel/thread.c`: `thread_block()` sets `wait_channel` and marks the current thread BLOCKED; `thread_wake()` changes BLOCKED to READY, enqueues the thread, and clears `wait_channel`. These operations do not themselves acquire a shared scheduler guard.
- `kernel/ipc.c`: unlike a plain spinlock, `ipc_lock()` saves RFLAGS and executes `cli` before taking the atomic channel lock; `ipc_unlock()` clears the lock and restores interrupts only if IF was previously set. `ipc_channel_wait()` registers the waiter while that lock/interrupt exclusion is held.
- Therefore the specific scenario “local timer preempts the waiter while it owns the IPC channel lock” is **not supported by the inspected current code path**, because local interrupts are disabled during that critical section. The broader claim in `docs/SCHEDULER-AWARE-MUTEX-PREREQUISITES.md` should be re-reviewed/worded more precisely rather than repeated as a confirmed defect.
- The inspected protocol is still not an SMP guarantee: disabling interrupts affects only the local CPU, while the atomic channel lock alone does not make scheduler queue/state changes globally atomic against another CPU. Also, the generic `thread_block/thread_wake` API does not encode the caller's exclusion/guard contract, so a new sleeping mutex must define and enforce that contract explicitly.
- No runtime interleaving test was run. This is a source-level correction to the audit narrative, not a claim that IPC or scheduler concurrency is fully proven.

### Safe next implementation gate

Before implementing a sleeping mutex, map all callers of `thread_block()`, `thread_wake()`, and scheduler queue/state mutators; state whether the supported target is strictly uniprocessor or SMP; then add a testable scheduler guard/atomic wait protocol consistent with that target. Do not replace the filesystem spinlock until that prerequisite and the “never sleep while holding a spinlock” rule are proven.


## Handle/mount lifetime: concrete implementation contract

This section is a design gate, not a claim that locks already exist.

### 1. Per-open-handle operation gate

The current public `struct pros_vfs_file` has a mutable `position`, `open`, `vnode`, `mount`, and provider-private state, but no operation reference count or close state. Read/write/seek/truncate and close therefore cannot safely race on the same handle.

Required semantics for a future patch:

1. Add an explicit handle lifecycle state (`OPEN`, `CLOSING`, `CLOSED`) and in-flight operation count, protected by a proven scheduler-safe gate.
2. Operation entry must atomically validate OPEN and pin the operation before reading the handle's vnode/provider fields. If not OPEN, return `PROS_VFS_EINVAL`.
3. `seek`, `read`, `write`, and `truncate` on the same handle must serialize access to `position`; do not hold a spinlock across provider I/O. The gate may be a sleeping mutex only after the scheduler wait/wakeup contract is implemented and audited.
4. Close must atomically transition OPEN -> CLOSING, reject new operations, wait/drain existing operations without holding a spinlock, then invoke provider close and release vnode/mount pins exactly once. Define what happens if provider close fails; do not mark CLOSED and discard provider state on a failed close unless provider contract explicitly guarantees that is safe.
5. A raw pointer to a caller-owned `struct pros_vfs_file` cannot be made safe against the caller freeing/reusing the structure concurrently unless the public API also defines object ownership/lifetime. Document this caller contract or move handles into a VFS-owned descriptor table with generation-tagged IDs.

### 2. Mount operation pin and unmount drain

The current `vfs_mount_deactivate()` checks root refcount and then changes `active`. That is a check-then-act sequence, not an operation drain.

Required semantics:

- A mount operation must pin the active mount before dereferencing provider/device/private state; pin fails once unmount enters QUIESCING.
- Unmount changes ACTIVE -> QUIESCING under the same lifecycle gate used by operation entry, rejects new operations, and waits for the active-operation count to reach zero without holding a spinlock.
- Only after draining operations may it check outstanding vnode/handle ownership, release root state, and transition to INACTIVE.
- If open handles/vnodes remain, return EBUSY and restore ACTIVE only if provider teardown has not begun. Do not partially deactivate and then reopen admission.
- Filesystem provider lifetime must remain valid until every pinned operation and object reference is released.

### 3. Lock order and non-blocking constraints

Proposed order, subject to implementation review:

1. Scheduler wait-queue guard: very short, IRQ-safe, never held over I/O, allocation, or a context switch.
2. Per-handle sleeping gate: serialize same-handle operations; do not acquire while holding a provider spinlock.
3. Mount lifecycle pin/count: short state update only; wait/drain outside the guard.
4. Provider filesystem serialization: protects provider metadata/data structures; no sleeping while holding a spinlock.
5. Block-device and allocator internals: must be checked against actual call graph before fixing the final total order.

This is intentionally not yet a complete total order: source call graphs for every provider/device/allocator path must be mapped before implementation. Avoid asserting a lock hierarchy is proven before that audit.

### 4. Deterministic regression test matrix

- Same handle: two reads cannot consume the same offset; final position equals the sum of successful byte counts.
- Same handle: concurrent seek and read follow serialized ordering, not a torn/stale position.
- Close race: operation entry either pins before CLOSING and completes before provider close, or is rejected; provider close must never overlap provider read/write/truncate.
- Failed close: handle/provider state remains recoverable and no vnode ref is dropped twice.
- Mount race: unmount blocks new operation pins after QUIESCING and cannot clear provider/device state until in-flight count is zero.
- Vnode lifetime: get/put stress cannot underflow, overflow, or free while a pinned user remains.
- IPC wakeup: message-arrival-vs-wait boundary has no lost wakeup on the supported CPU model; also verify interrupt flags are restored on every return path.
- Run normal unit/self-tests, Security architecture checks, ISO QEMU boot test, native BIOS QEMU boot test, and repeat under a deterministic interleaving/fault-injection harness where available.

### 5. Implementation decision

Do **not** add an ad-hoc per-handle spinlock to `vfs_file_read/write/close`: provider callbacks may perform storage I/O, so holding that spinlock across callbacks risks stalling the CPU or deadlocking. Do **not** add a sleeping mutex until its atomic waiter registration/block/wakeup contract is proven. The next code change should be the scheduler guard/wait protocol plus focused scheduler/IPC tests, or a formally documented strict-uniprocessor critical-section design if the kernel explicitly excludes SMP.


## Scheduler API call-graph audit (follow-up)

A repository search of the current default-branch snapshot found the only non-definition call to `thread_block()` in `kernel/ipc.c` (`ipc_channel_wait()`). This narrows the immediate migration surface, but it does not prove that scheduler state is globally serialized.

### Observed state transitions

- `thread_block()` in `kernel/thread.c` sets `current_thread->wait_channel` and `state = PROS_THREAD_BLOCKED`; it does not itself switch context or acquire a scheduler guard.
- `thread_wake()` checks BLOCKED/runnable state, sets READY, calls `thread_enqueue()`, then clears `wait_channel`. These scheduler globals and thread fields are ordinary accesses in this path.
- `thread_enqueue()` changes state and manipulates `runq_head`, `runq_tail`, `next`, and `prev`; `thread_dequeue()` mutates the same queue without an internal guard.
- `thread_schedule()` dequeues, may requeue the outgoing thread, switches process address space, and changes `current_thread`/state. Its internal operations are not individually guarded by a shared scheduler-lock API.
- `ipc_channel_wait()` disables local interrupts while holding the channel spinlock, calls `thread_block()`, registers the waiter, then restores the prior interrupt state. This is a coherent local-CPU critical section for the current single-CPU model, but it is not an SMP scheduler lock.

### Decision for the next code patch

Do not patch only `thread_block()` and assume the protocol is fixed: block/wakeup correctness depends on the channel waiter list, run queue, thread state, and preemption paths as a combined state transition. The minimum safe patch scope must include:

1. A documented scheduler critical-section primitive that saves and restores the caller's prior interrupt state and explicitly states whether SMP is unsupported.
2. Guarding scheduler queue/state mutations that can race with interrupt-driven scheduling on the supported execution model.
3. A single wait-queue API that couples condition recheck, waiter registration, BLOCKED transition, and wakeup under a defined guard; no caller should separately mutate scheduler state and then register itself.
4. IPC migration to that API, with regression tests for message-arrival vs wait, cancellation vs wake, and interrupt-state preservation.
5. Only after those tests pass, introduce a sleeping mutex for VFS handle and mount lifetime operations.

This patch scope is intentionally broader than a documentation-only VFS change. It avoids introducing a mutex that depends on an unproven scheduler protocol.


## Runtime-code follow-up: blocked-current/no-successor scheduler path

A deeper review of `thread_schedule_interrupt()` found a concrete state-check bug beyond the earlier general concurrency risks:

- When `thread_dequeue()` found no successor, the idle decision checked only `thread_process_runnable(prev)`. A thread can be `PROS_THREAD_BLOCKED` while its owning process remains READY/RUNNING, so this predicate can be true even though the current thread must not resume.
- In that state the old code returned `current_frame`, which could iret back into a thread that had explicitly blocked (including an IPC receiver) if no runnable successor remained.
- Both the no-successor path and the address-space-switch-failure rollback path now treat `prev->state == PROS_THREAD_BLOCKED` as sufficient reason to park, preserve the BLOCKED state, clear `current_thread`, and wait in the interruptible idle loop rather than return the blocked frame.
- This is a source-reviewed runtime-code fix on this PR branch. The exact pre-documentation head `1daec37bd3e0cf1b4991b74823bd6f3624a12f72` passed Build/boot-test, including ISO build, ISO QEMU boot-test, and native BIOS QEMU boot-test; its Security architecture workflow also passed.
- The shared fallback state transition is now factored into `thread_interrupt_fallback_prepare()`, used by both production fallback paths and directly exercised by the kernel self-test for a BLOCKED thread. This verifies the transition without entering the non-returning idle loop.
- Remaining coverage gap: the self-test still does **not** execute the actual no-successor branch or force an address-space-switch failure while the previous thread is BLOCKED. A fuller injectable scheduler selection/switch seam is still needed.


## Latest scheduler regression extension

The follow-up self-test now exercises the state-transition branch for a thread marked RUNNING while its owning process is quarantined. It temporarily toggles the init process quarantine flag inside the boot-time test, verifies `thread_interrupt_fallback_prepare()` converts the probe thread to BLOCKED, then restores the original process flag before normal scheduler tests continue. This strengthens helper coverage but does not replace the remaining integration tests for the real no-successor idle loop or forced address-space-switch failure.


A second helper-level regression uses a local copy of the init process with its process state set to `PROS_PROCESS_BLOCKED`. A probe thread that is stale-`RUNNING` must transition to `PROS_THREAD_BLOCKED`. The copied process avoids mutating the live init process state. This covers both quarantine and explicit process-blocked reasons, while the scheduler integration-path gaps remain open.


The latest regression also verifies the distinct dead-owner outcome: a stale `RUNNING` probe owned by a `PROS_PROCESS_DEAD` process must become `PROS_THREAD_DEAD`, whereas a blocked owner becomes `PROS_THREAD_BLOCKED`. This tests the state mapping only; it does not emulate the real non-returning idle loop.


The fallback regression suite now explicitly verifies the safe no-op outcome too: when the probe belongs to an eligible process and is `RUNNING`, `thread_interrupt_fallback_prepare()` must return zero and preserve `RUNNING`. This closes a policy-helper branch gap; actual interrupt-path integration testing remains required.


## Progress log — 2026-10-29

Verified branch: `audit/vfs-concurrency-offline-20261029`. At commit `51d86a871c1c110972b018e5565dcf3b30189310`, both GitHub workflows passed: Build and boot-test run `40927057441` (success), Security architecture run `40927057469` (success). The previous head `ab5db43b2316` also passed both workflows (runs `40926296402` and `40926296531`). PR #305 remains open, draft, and unmerged.

### Completed and verified
- Fallback policy helper is shared by both production scheduler fallback paths.
- Helper-level regression checks cover eligible RUNNING no-op, blocked thread, quarantined owner, blocked process owner, and dead process owner transitions.
- Existing boot-time scheduler tests cover queue/state handling and wake recovery.
- Build/boot-test and Security workflows pass on the recorded latest commit.

### Still open
1. Deterministic integration test for the real no-successor idle path without entering a non-returning loop.
2. Deterministic integration test for address-space-switch failure when the interrupted thread is no longer eligible.
3. Per-handle read/write/seek/close concurrency and a safe handle-lifetime contract.
4. Vnode reference-count synchronization and stress tests.
5. Mount quiescing/draining to prevent unmount racing active operations.
6. Directory read/close race, failed-close recovery, and mount-vs-operation tests.
7. Final-head CI review and PR #305 merge decision.

### Progress accounting
No percentage is assigned until a finite checklist and acceptance criteria are defined. Scheduler helper-level coverage and CI are verified; scheduler integration and VFS concurrency items above remain incomplete. This log is a status record, not a declaration of completion.


## Latest CI report checked — 2026-10-29

Checked the latest PR branch head before this log update: `07d342025bda578b1efb355e96cb4f78ebf117c9` (documentation-only progress-log commit).
- Build and boot-test ProsenjitOS: run `40927890114` — **completed / success**.
- Security architecture checks: run `40927890104` — **completed / success**.
- Previous code-change head `51d86a871c1c110972b018e5565dcf3b30189310`: Build run `40927057441` and Security run `40927057469` — both **completed / success**.
- PR #305: **OPEN, DRAFT, NOT MERGED**.

Interpretation: CI is green for the latest checked branch head. This confirms the configured workflow checks passed; it does not close the explicitly listed scheduler integration and VFS concurrency work items, and it does not by itself authorize marking the project complete.


## Follow-up source audit — handle, vnode, mount, and directory lifetime

Audited the current PR branch head `undefined` while the local runner was offline. This was a source-only review; no runtime behavior was changed in this checkpoint.

### Confirmed code-level hazards (not yet reproduced as runtime failures)

1. **File close can race with active I/O.** In `kernel/vfs.c`, `vfs_file_close()` calls the provider close callback at lines 589–596 before setting `open = 0` and clearing `ops`, `vnode`, `mount`, and `private_data` (597–610). Read/write/truncate validate these fields and then call through the provider. Without an operation pin/gate, another CPU or re-entrant caller could pass validation while close releases provider state.
2. **Per-handle position is unsynchronized.** Read/write consume and update `file->position`; seek assigns it directly. The struct has no per-handle operation lock or in-flight reference. Provider data-I/O serialization does not protect the VFS-level cursor.
3. **Vnode refcount is a plain 32-bit counter.** `vfs_vnode_get()` checks and increments `refcount`; `vfs_vnode_put()` decrements and may invoke the final release callback. Concurrent get/put can lose updates or race final release. An atomic increment alone is insufficient unless zero-to-live acquisition and final-release ownership are also made safe.
4. **Unmount uses a check-then-act sequence.** `vfs_mount_deactivate()` checks `root->refcount > 1`, then marks the mount inactive and drops the root. Operation entry checks `mount->active` separately. No shared admission/drain protocol currently makes those actions atomic with respect to in-flight operations.
5. **Directory read/close share mutable state.** `vfs_dir_read()` dereferences `dir->vnode` and updates `dir->cookie`; `vfs_dir_close()` drops the vnode reference and clears the same fields. A concurrent close can invalidate the read's vnode/cookie while the provider callback is running.
6. **Namespace mutation still needs provider-level transaction semantics.** VFS unlink/mkdir/rmdir resolve and authorize the parent, then invoke a provider callback. The VFS layer does not itself serialize lookup/authorization against a competing namespace mutation. The provider must define and test atomicity and lock ordering; a generic VFS lock must not be guessed here.

### Safe implementation decision

Do **not** patch these with ad-hoc spinlocks around provider callbacks: callbacks may perform storage I/O and must not hold a spinlock across a potentially blocking operation. Do not add a sleeping mutex until scheduler waiter registration, wakeup handoff, cancellation, and interrupt-state rules are proven. The next implementation prerequisite is a documented mount/handle operation-lifetime protocol with:
- operation admission and reference pinning before dereferencing provider state;
- a close/unmount transition that rejects new operations;
- draining in-flight operations without holding a spinlock while waiting;
- rollback/recovery when provider close or flush fails;
- a defined ownership contract for caller-held raw handle pointers;
- deterministic tests for close-vs-read/write, directory read-vs-close, vnode final-put races, and unmount-vs-operation.

### Scope boundary

These findings are source-confirmed hazards, not claims of observed crashes. No runtime code was changed and no CI/build/QEMU test was run for this documentation checkpoint. Existing earlier green workflows do not validate this audit entry.


## Mount admission and file-I/O path review — a49852f2d5b8983b219f187868146585da3aacd7

A second pass traced the exact mount and file-operation entry points in `kernel/vfs.c` and the public object layouts in `kernel/include/vfs.h`.

### Additional observations

- `struct pros_vfs_mount` exposes only a byte-sized `active` state; there is no QUIESCING state, active-operation count, or admission token in the public mount object.
- `vfs_mount_deactivate()` checks the root's ordinary `refcount` and then clears `active`, drops the root, and clears the device/private-data pointers. This is a valid single-threaded busy check, but it is not a concurrent drain protocol: operation entry and deactivation do not share an atomic gate.
- `vfs_file_read()` and `vfs_file_write()` check `open`, `mount->active`, operation callbacks, vnode and credentials before invoking the provider. The callback is outside any VFS handle-level operation gate. A successful callback then updates the shared `position`.
- The append path delegates offset selection to the native provider's serialized append operation and then refreshes the handle position from `vnode->size`. This is a useful provider-level append guarantee, but it does not serialize two operations sharing one VFS handle or make the vnode-size read atomic with another writer.
- The existing parent-reference design explains why the root refcount can detect retained descendants in a quiescent single-threaded state. It does not make the check safe against a concurrent vnode get/put or a new operation starting after the check.

### Decision for this checkpoint

No runtime patch was made. Adding a mount counter or a handle busy byte without a proven atomic primitive would only create another racy state variable; a spinlock held over provider I/O would risk deadlock. The next runtime patch should first introduce and test one narrowly scoped operation-admission primitive for the supported CPU model, then use it consistently in file, directory, namespace, and mount lifecycle entry points. The primitive must be chosen only after its IRQ/preemption and scheduler-wait behavior is proven.

### Verification

This is source review only. No runner, compiler, boot, QEMU, stress, or fault-injection test was executed for this note. The findings are race risks inferred from the code paths, not observed failures. CI results must be checked against the resulting commit SHA before claiming this note's commit is validated.


## Scheduler failure-path coverage distinction — 2026-10-30

A follow-up source pass compared the interrupt-driven scheduler path in `kernel/thread.c` with the boot regression in `kernel/kernel.c`.

- The boot regression around the scheduler address-space failure case deliberately invokes the **cooperative** `thread_schedule()` path after invalidating a test process address-space root. It verifies rollback of the candidate and restoration of the still-running worker. This is useful coverage, but it is not the same as forcing `thread_schedule_interrupt()` through its failed CR3-switch fallback.
- The interrupt-driven path has two production-only park branches: no eligible successor (around the no-successor fallback) and failed address-space switch when the previous interrupted thread is no longer resumable. Both enter a non-returning `sti; hlt` loop after detaching `current_thread`.
- Existing helper tests exercise the fallback state policy, and the cooperative test exercises a related address-space rollback. Neither proves the interrupt-specific branch's frame ownership, queue rollback, or idle-loop transition.
- A test seam must preserve the production branch's exact state/queue decision while allowing the test harness to assert the selected action before entering the non-returning idle loop. It must not silently return the previous frame in a state where production would park.

### Updated test matrix

| Case | Current evidence | Remaining acceptance test |
|---|---|---|
| Fallback policy for eligible/blocked/quarantined/dead owner | Helper-level boot assertions present | Run on exact head |
| Cooperative address-space switch failure | Boot self-test setup and rollback assertions present | Run on exact head |
| Interrupt no-successor park | Source branch only | Deterministic interrupt-path seam + QEMU serial assertion |
| Interrupt CR3-switch failure with blocked previous thread | Source branch and helper policy only | Deterministic interrupt-path seam + queue/frame invariant assertions |
| Actual idle-loop wake/re-entry | Production code only | QEMU integration test proving IRQ wakes a runnable successor safely |

No scheduler runtime code was changed in this review. Do not mark the two interrupt-specific failure paths complete based solely on the cooperative scheduler test.

### CI status at the time of this log update

The previous code/documentation head `4ef465831f2c` has Build/boot-test run `40928521371` and Security run `40928521228`, both completed successfully. The subsequent documentation commits triggered new workflows; the newest observed runs for commit `62e4f92f87e5` were Build run `40993220792` (pending) and Security run `40993220929` (queued). These are not passing results. Recheck the exact latest head before making any CI claim.
