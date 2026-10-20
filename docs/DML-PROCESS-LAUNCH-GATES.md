## 2026-10-20 — Production spawn contract drafted (not implemented)

- Added [DML-SPAWN-ABI-DESIGN.md](DML-SPAWN-ABI-DESIGN.md) as a review draft. It deliberately assigns no syscall number and is not a published ABI.
- The draft records the existing boundary: DML manager/runtime tests exercise a mock callback backend; current public syscalls include WAIT_REAP and EXIT but no general spawn/exec call; pros_init_launch() remains an internal kernel transaction requiring VFS/security metadata and caller-owned lifetime resources.
- Specified required authorization, executable-handle semantics, bounded request validation, NEW-to-READY transaction commit, rollback/ownership rules, DML start/stop callback guarantees, and required fault-injection/end-to-end test gates.
- Remaining blockers before production code: agree executable object/handle type, caller-owned process/thread/stack allocation and cleanup, syscall number/request layout, argument limits, child policy profile, package trust, and stop/wait ownership. These decisions must be checked against current kernel/security interfaces before implementation; do not guess the ABI.
- Verification remains pending for the new documentation commit until its latest-head workflows finish. Desktop tests must use OfficeWindows; kernel/QEMU tests remain on the designated Debian office runner.

# DML process-launch implementation gates

## 2026-10-20 verification update — CPL3 exit/wait-reap integration

- Exact head `f02b53701bfb0d66755d691f729a7c8d7ec69e15` passed [Build and boot-test run 39384197248](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39384197248).
- The ISO boot, first native BIOS boot, and second native BIOS boot each logged the real timer-preemption observation and the unique `user process exit/scheduler/wait-reap integration test passed` marker. The test validates exit status 23, parent process/thread restoration, successful ownership-checked wait/reap, PID removal, and address-space cleanup.
- The DML tests passed on the verified `OfficeWindows` runner; security architecture checks passed in [run 39384197268](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39384197268).
- This closes the **focused CPL3 child exit/scheduler/wait-reap integration** gate. It does not close general production spawn/exec: a caller-controlled, permission-checked spawn syscall and production DML adapter remain outstanding.
- Next implementation gate: design a bounded versioned spawn request with validated executable reference/arguments, explicit syscall policy and capability checks, transactional process/thread/stack ownership, and complete failure rollback. Do not claim production DML launch or graphical-session support until the adapter and end-to-end path pass.


## Status

This document is a source-audit record and implementation gate, not a claim
that process launching is implemented. The DML runtime currently coordinates
a backend through synchronous `start` and `stop` callbacks. The repository
does not yet provide a production adapter for those callbacks.

## Source-audit findings

- `include/abi/syscall.h` exposes ABI version, test completion, handle close,
  handle duplication, IPC send/receive, and handle transfer. It does not expose
  process spawn/exec or wait.
- `kernel/process.c` implements process-object creation/registration and
  lifecycle operations. `process_create()` initializes and registers a
  process in READY state; it does not itself load an executable or start a
  user-mode instruction stream.
- `kernel/elf_loader.c` validates and loads an ELF image into a supplied
  process address space and returns entry/image/stack metadata. Loading alone
  does not prove that the CPU has entered the process in user mode.
- `kernel/thread.c` has thread creation and scheduling primitives. Its
  `thread_init()` builds a kernel context-switch stack around a kernel entry
  address. That interface must not be assumed to be a safe CPL3 ELF entry
  trampoline.
- `kernel/init.c` has a filesystem-backed `/init` load helper with executable
  admission checks. This is a loader helper, not a general userspace spawn,
  wait, or service-manager API.
- The public syscall policy is an explicit bitmask. Any new syscall requires
  deliberate ABI numbering, policy-mask integration, argument validation,
  user-memory validation, authorization, and regression coverage.

## Required implementation sequence

1. **Trace the existing ring-3 entry path.** Identify the exact interrupt/IRET
   frame construction, user code and stack mapping permissions, segment
   selectors, entry trampoline, scheduler handoff, and cleanup path. Reuse a
   verified path instead of treating a kernel thread entry as a user entry.
2. **Define process ownership and lifetime.** Decide how process/thread objects
   and kernel stacks are allocated, who owns their memory, how PIDs/TIDs are
   assigned, and how failed ELF loading rolls back all resources.
3. **Define a narrow versioned spawn contract.** Use a validated executable
   reference/handle rather than trusting arbitrary user pointers or a path
   supplied without VFS permission checks. Bound argument/environment counts
   and total bytes. Specify output PID/handle semantics and all error cases.
4. **Preserve executable security admission.** Filesystem-backed execution must
   pass the existing admission/quarantine/block decision before ELF loading.
   Enforce user/kernel mapping separation and least-privilege syscall and
   capability policies.
5. **Define wait/reap and stop semantics.** Parent/child rules, zombie status,
   direct-child wait, handle closure, address-space destruction, thread
   termination, and exactly-once resource release must remain consistent with
   `docs/PROCESS-LIFECYCLE.md`.
6. **Add focused tests before integration.** Cover invalid/unmapped user
   pointers, malformed ELF, unsupported ELF features, denied execution,
   duplicate/invalid handles, PID/TID exhaustion, partial-allocation rollback,
   failed thread creation, child exit/wait/reap, double wait, and policy denial.
7. **Implement the DML backend only after the kernel contract is verified.**
   Its start callback must return a nonzero stable process handle only after a
   child has been successfully created and made runnable. Its stop callback
   must terminate/wait/reap the child and report success only after it is no
   longer running. A failed callback must not silently orphan a process.
8. **Verify on the designated OfficeWindows runner only.** Check the assigned
   runner name before accepting results. Run focused host tests, the full
   source build, and the existing boot checks. Keep QEMU/kernel boot failures
   separate from DML unit-test results; do not call GUI support complete until
   real rendering and input delivery work.

## Non-goals for this milestone

- Do not put the DML, a desktop shell, XFCE, Xorg, Wayland, or desktop
  applications inside kernel-core.
- Do not add a guessed syscall ABI merely to make the DML mock tests pass.
- Do not wire the mock backend into boot or claim it launches a real session.
- Do not mark a desktop environment complete based solely on manifest parsing,
  dependency ordering, ELF loading, or a successful ISO build.

## Exit criteria

The production-launch milestone is complete only when the source contains a
reviewed process creation + user-mode entry + wait/reap path, the DML adapter
uses that real path, focused regression tests pass, and the required build and
boot verification passes on the designated runner. Real desktop completion
additionally requires a supported display/window-system stack and verified
keyboard/pointer event delivery.


## 2026-10-17 verification update

- DML manager and runtime unit tests passed on the explicitly verified `OfficeWindows` runner in workflow run [39033799224](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39033799224). This includes the latest malformed-registry regressions present in the tested workflow checkout.
- Security architecture checks passed in run [39033799262](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39033799262).
- The Debian `office` runner passed toolchain verification and ISO build, but ISO QEMU boot failed: #PF vector 14, instruction-fetch error `0x11`, `RIP=0`, `CR2=0`, diagnostic stage `0xF041`. The required IPC milestone was absent, so the native BIOS boot test was skipped. This is a separate kernel/ProsenjitFS boot blocker; no root cause is claimed here.
- Source audit reconfirmed that process creation, ELF loading, and the current explicit boot-time CPL3 test do not yet form a general production spawn/exec/wait/reap service. The DML callbacks remain a mockable backend contract, not a real launcher.


## 2026-10-17 fresh boot trace (run 39036048406)

The ISO builds on `office`, but QEMU still fails before the required IPC-channel
and CPL3 milestones. The serial trace reaches storage-volume discovery and then
reports #PF vector `0xE`, error `0x11`, `RIP=0`, `CR2=0`, diagnostic stage
`0xF041`. The workflow correctly fails the required-marker checks even though
QEMU itself is stopped by the expected 20-second timeout.

The caller trace records target `0x112420`, RSP/frame/return
`0xD5990/0x1568F0/0x102B95`, and nonzero block-device/callback arguments.
However, the callee snapshots are all zero even though the linked disassembly
places their stores before the callee writes stage `0xF041`. This contradiction
means the current trace is not yet sufficient to identify the faulting
instruction or prove a bad return address. Next step: isolate and reconcile
checkpoint/snapshot behavior with a minimal call/return probe or debugger trace
before changing production code. No root cause is claimed.


## 2026-10-17 latest verification update — run 39036283325

- [x] DML manager and runtime unit tests passed on the explicitly checked `OfficeWindows` runner. The job log contains the three success lines for manager tests, runtime tests, and the overall DML test script.
- [x] Security architecture contract checks passed in run `39036283329`.
- [x] ISO build passed on the designated `office` Debian runner.
- [ ] QEMU ISO boot still fails before IPC-channel/capability and CPL3 milestones. Current diagnostic remains #PF vector `0xE`, error `0x11`, `RIP=0`, `CR2=0`, stage `0xF041`. Native BIOS boot is skipped.
- [ ] Do not treat the 20-second QEMU timeout as success by itself: the required serial markers must all be present. Here they are not.
- [ ] Do not implement a production DML launcher until the spawn/exec ABI, process/thread ownership, ELF mapping and CPL3 entry, syscall policy, and wait/reap semantics are designed against the existing kernel interfaces and covered by tests.
- **Status:** DML host tests and security checks pass. OS build passes. Kernel boot and end-to-end desktop remain blocked.


## 2026-10-17 — ProsenjitFS self-test stack regression

The `office` build for run [39036768898](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39036768898) reported `prosenjitfs_selftest` static stack usage of 528,240 bytes. Source inspection identified a nested local `struct pfs_test_disk small_disk` in the tiny-image exhaustion test; its durable and volatile 512-sector arrays alone occupy 524,288 bytes. Commit `5c70267a4066a6b4832902e9d4e6d3f113333e34` moves this test-only disk object to static storage and retains the existing explicit zeroing before use. This is a targeted diagnosis/fix, not yet a verified boot fix.

**Required validation:** the next `office` build must report reduced `.su` stack usage; ISO QEMU must reach all required IPC/CPL3 milestones; native BIOS must run if the workflow permits it; `OfficeWindows` DML tests and security checks must pass. Until then, do not mark the desktop complete or promote PR #137 from Draft.


## 2026-10-17 — Stack reduction verified; ProsenjitFS boundary test now blocks boot

The retried `office` run [39037088786, attempt 2](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39037088786) built the ISO and measured `prosenjitfs_selftest` stack usage at **7,408 bytes**, down from 528,240 bytes. The prior null-RIP/oversized-frame symptom did not recur. Boot now stops at the ProsenjitFS self-test with `rc=-15`, stage/source line 7272 (`0x1C68`), before the IPC-channel/CPL3 milestones; those missing markers are downstream of the filesystem abort.

Commit `fe59c25e8a1ce0883b0b37f9d400fc5b95d309d5` separates the negative/boundary assertions and uses a correctly typed `uint32_t` output for `pfs_inode_block_location`, so the next QEMU log should pinpoint the failing contract. Await the exact result before modifying production filesystem behavior. DML tests on `OfficeWindows` and security architecture checks passed on the prior head; both must be revalidated for final head.

**Status:** stack regression fix verified by compiler output; ProsenjitFS boundary assertion unresolved; no desktop completion claim until filesystem, IPC/CPL3, boot and real desktop integration gates pass.

## 2026-10-17 deeper launch-path audit

Inspected the actual assembly and kernel sources on this branch before considering a production DML adapter:

- `boot/entry.asm:104-123` implements `enter_user_mode(entry, stack_top, initial_handle)` by constructing an IRETQ frame with user SS `0x23`, user CS `0x1B`, the supplied user stack and entry, and then executing `iretq`. The same assembly file contains the boot-time IPC/syscall test payload. This is a boot/test entry path, not yet a general process/thread launch API.
- `kernel/idt.asm:18-96` saves syscall registers, invokes `syscall_dispatch`, and handles the special exit trampoline path. `kernel/kernel.c` calls `enter_user_mode` from the boot self-test. These references establish an existing CPL3 route to study/reuse, but do not prove that a scheduled child process can safely enter through it.
- `kernel/context_switch.asm` exports `thread_start_trampoline`; `kernel/thread.c::thread_init` constructs a kernel-context thread stack around that trampoline. It must not be passed an ELF user entry as though it were a ring-3 IRET frame.
- `kernel/elf_loader.c` populates `process->user_entry` and `process->user_stack_top`, creates user mappings, and rejects load segments that are simultaneously writable and executable. It does not itself enqueue a thread or transfer CPU control to that entry.
- `kernel/include/process.h` exposes internal `process_create`, `process_exit`, and `process_wait` operations. `include/abi/syscall.h` still publishes no spawn/exec or wait syscall; these internal C interfaces are not a user ABI.

**Implementation decision:** do not add a syscall number or a DML backend in this patch. The missing work is a reusable and audited child-process launch transaction that binds admission -> ELF load -> initial user IRET frame -> runnable thread -> exit/wait/reap, with rollback and ownership guarantees. First implement that transaction behind kernel-internal tests, then separately review/version any user-facing syscall contract. Keep the existing boot-time CPL3 test intact as a regression gate.


## 2026-10-18 verification refresh — current head `bdcd233c85fdb46a0d5cec4359b236258857584e`

This section supersedes the older boot-failure status above for the tested head.

- [x] DML manager/runtime unit tests passed on `OfficeWindows` / machine `IT` in [run 39055656118](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39055656118). The log confirms `desktop manager tests passed`, `desktop runtime tests passed`, and `DML manager and runtime unit tests passed`.
- [x] ISO QEMU boot milestones passed on `office` / machine `debian` in [run 39055656118](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39055656118).
- [x] Native BIOS QEMU boot milestones passed, including the native filesystem LBA boundary diagnostic and the second native boot persistence check, in the same build run.
- [x] Security architecture checks passed in [run 39055656164](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39055656164).
- [ ] Production user-mode spawn/exec + wait/reap path remains unimplemented. Existing DML start/stop callbacks are still only an orchestration contract; no production kernel-backed adapter is wired in.
- [ ] Real display/window-system integration, keyboard/pointer delivery, a running desktop session, and boot-to-desktop verification remain incomplete.

The QEMU boot blockers recorded in earlier dated sections are historical and did not recur on this tested head. Do not infer desktop-launch completion from the passing kernel boot tests or DML unit tests. Keep this PR Draft until the production launch path and real desktop integration gates pass.


## 2026-10-18 follow-up source audit — user-thread entry is not a launch transaction

Fresh source review of `kernel/thread.c`, `kernel/process.c`,
`kernel/elf_loader.c`, `kernel/syscall.c`, and `boot/entry.asm`
identified these precise boundaries:

- `thread_init_user()` exists and validates that the ELF entry maps to a
  user-accessible executable page and the initial stack maps to a user,
  writable, NX page. It constructs a ring-3 IRET frame with selectors
  `CS=0x1B` and `SS=0x23`. This is a useful primitive, not proof of a
  complete process launch.
- The ordinary `thread_schedule()` path explicitly skips candidates with
  `user_mode != 0`, re-enqueues them, and continues searching. Therefore a
  launch implementation must not assume this scheduler function can start a
  user thread.
- `thread_schedule_interrupt()` has a separate interrupt-frame return path,
  updates the TSS kernel stack, and can return a thread's saved interrupt
  frame. Its interaction with the actual interrupt entry/return assembly,
  CR3 switching, first-run state, preemption, and user-thread lifetime must be
  proven together before choosing it as the production launch route.
- `process_create()` registers a READY process, while `process_wait()`
  accepts a direct child only after it becomes ZOMBIE and then releases
  resources. These internal operations do not yet provide a complete atomic
  create/load/thread-init/enqueue transaction.
- The syscall dispatcher still exposes no public spawn/exec or wait operation.
  Adding syscall IDs alone would not fix the scheduler or lifecycle gap.

### Required next implementation slice

1. Trace the actual interrupt stubs and frame layout used by the current
   scheduler, then write a focused kernel-side test for a newly created user
   thread's first entry and return/exit path.
2. Design a single-owner launch transaction with explicit ownership for the
   process object, ELF image buffer, address space, user stack, thread object,
   and kernel stack. Every allocation or registration failure must unwind in
   reverse order without leaving a registered process or runnable orphan.
3. Make first-run user scheduling explicit and safe. Do not route CPL3 frames
   through the kernel context-switch RET trampoline, and do not enable timer
   preemption until interrupt-frame save/restore and per-thread kernel-stack
   switching are verified.
4. Reuse the existing executable-admission checks and ELF loader. Then test
   success, malformed/denied ELF, unmapped entry/stack, allocation failure at
   each stage, child exit, direct-parent wait/reap, and double wait.
5. Only after those tests pass, design the versioned syscall surface and
   implement a production DML backend. Keep the desktop stack in userspace.

**Current decision:** this source audit found a concrete scheduler integration
hazard, but did not modify kernel scheduling code because the ISO/native boot
and interrupt-frame regression gates must remain green. The production
launcher and graphical desktop remain incomplete; no completion claim is made.


## 2026-10-18 — first-run CPL3 interrupt-flag consistency fix

A concrete mismatch was found between the established boot-time CPL3 entry and the first-run scheduler-frame constructor: `enter_user_mode()` deliberately clears IF because user-mode timer preemption is not yet considered scheduler-safe, while `thread_init_user()` was constructing a first-run frame with RFLAGS `0x202` (IF=1).

- [x] Updated `thread_init_user()` to set RFLAGS to `0x2` (reserved bit 1 set, IF clear), matching the existing boot-time safety policy.
- [x] Updated the kernel self-test to assert the first-run CPL3 frame has IF clear.
- [ ] Re-run warnings-as-errors build, DML tests, ISO/native BIOS QEMU, and security checks on the new head before treating the fix as verified.

This is a narrow safety-consistency fix, **not** a production process-launch implementation. With IF clear, a first-run user thread cannot be timer-preempted; do not enable IF until the scheduler/interrupt-frame path and a supported userspace interrupt policy are verified end-to-end.

## 2026-10-18 — Transactional launch and wait/reap hardening

- [x] `pros_init_launch()` keeps a newly registered child in `NEW` during executable admission, ELF mapping, and CPL3-frame setup; it publishes `READY` only after the frame is validated and immediately before run-queue insertion.
- [x] `process_wait()` now preflights quiescence, direct-child registry membership, descendant absence, and the sole base object reference before destructive resource cleanup. A failed preflight leaves the zombie's address space intact and does not publish an exit status.
- [x] Reap detaches dead thread records from the global thread registry before caller-owned thread/stack storage may be released. A kernel process self-test exercises retained-reference rejection and confirms the dead thread record is detached on successful reap.
- [x] Updated `docs/PROCESS-LIFECYCLE.md` to record launch publication and reap preflight invariants.
- [ ] Verify these exact changes on the designated self-hosted runners: build/ISO/native BIOS QEMU, OfficeWindows DML tests, and security architecture checks. The current workflow runs are queued/pending; no pass is claimed for this patch yet.
- [ ] Still missing: a real allocation/ownership policy for process/thread/kernel-stack storage, an end-to-end safe user-mode interrupt-frame scheduler path, a public spawn/exec/wait ABI with user-pointer and security-policy enforcement, and a production DML backend. Do not merge or promote PR #137 from Draft based on this source-only checkpoint.


## Wait/reap syscall checkpoint

- [x] Published `PROSENJIT_SYS_WAIT_REAP = 8` and included it in the supported syscall-policy mask.
- [x] Added a user-mode-only dispatcher path that validates the status output range, limits wait to the current parent process, and returns `EAGAIN` for a child that is not yet a zombie.
- [x] Added a dispatcher self-test that confirms a ring-0 call is rejected.
- [ ] CI/build/boot-test verification for these commits is still pending. The syscall is non-blocking and must not be described as full POSIX wait or a complete spawn/exec/wait ABI.


## 2026-10-20 latest-head verification — PR #137

- [x] DML manager, runtime, and compositor/window-primitive unit tests passed in [workflow run 39398979928](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39398979928). The job log confirms runner name `OfficeWindows` (machine `IT`), then reports all three test groups passed.
- [x] The designated Debian `office` build job in the same run passed toolchain verification, ISO build, QEMU ISO boot milestones, and native BIOS QEMU boot milestones.
- [x] Security architecture contract validation passed in [workflow run 39398980049](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39398980049).
- [x] Runner separation was enforced: DML tests ran on `OfficeWindows`; kernel/build/QEMU checks ran on Debian `office`.
- [ ] These green checks do **not** establish production DML spawn/exec. The current runtime still depends on callbacks; a production permission-checked spawn adapter and end-to-end graphical desktop session remain outstanding.
- [ ] This documentation update changes the PR head, so the new head must complete its own CI before the latest commit is considered verified. Keep PR #137 Draft until the production launch and desktop integration exit criteria above pass.
