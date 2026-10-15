# Syscall and Context-Switch Stack Contract

**Status:** design/hardening note; implementation and runtime verification are pending.

## Why this contract is needed

The current source has separate mechanisms for the user syscall entry path and the kernel thread scheduler:

- `kernel/idt.asm:isr128` uses IST index 1 and builds the syscall register frame on the TSS IST stack.
- `boot/entry.asm` defines the bootstrap TSS stacks; IST1 is a shared entry stack rather than storage owned by a particular thread.
- `kernel/thread.c:thread_schedule_interrupt` saves the supplied `pros_interrupt_frame *` in `thread->interrupt_frame` and returns a saved frame when selecting a thread.
- The syscall dispatcher can request an internal block/schedule path for a blocking IPC receive.
- `kernel/include/thread.h` currently describes `pros_interrupt_frame` as GPRs followed by a CPL0 IRET frame (`RIP, CS, RFLAGS`), while a CPL3-to-CPL0 transition also has user `RSP` and `SS` on the CPU-created IRET frame.

This combination creates a source-level frame-lifetime and frame-shape hazard: a saved pointer may refer to reusable shared IST storage, and a CPL3 return frame must not be treated as a CPL0-only frame. Runtime manifestation has not been established.

## Required invariants

1. **Stable saved context.** Any frame retained across a block or thread switch must live in storage owned by that thread and remain valid until the thread resumes or is destroyed. It must never point into a shared IST area that another entry can reuse.
2. **Explicit frame kind.** The entry/scheduler contract must distinguish at least kernel-originated interrupt frames from user-originated syscall frames. Code must not infer the frame kind from an incomplete C structure.
3. **Complete user return state.** A saved CPL3 return context preserves all GPRs and the complete CPU return frame, including user RIP, CS, RFLAGS, RSP and SS. Restore must match the actual assembly stack layout.
4. **Per-thread kernel entry stack.** Each schedulable thread that can enter from CPL3 has a valid kernel stack and bounds. The active TSS `RSP0` (or an explicitly documented equivalent entry-stack scheme) must refer to the selected thread before returning to user mode.
5. **IST isolation.** IST remains suitable for exceptional/emergency entry and is not used as persistent per-thread syscall context storage.
6. **Correct ABI alignment.** Every C call from assembly observes the x86-64 SysV stack-alignment contract, including when a timer IRQ interrupts CPL0 at arbitrary stack alignment.
7. **Fail-safe switch behavior.** If no runnable successor exists, a blocked or quarantined context must not be resumed merely because its saved frame is the current entry frame. An explicit idle/handoff policy is required.
8. **Address-space consistency.** The selected thread, current process, active CR3 and TSS kernel-stack pointer must be updated in a documented order; a failed transition must not return to a frame under the wrong address space.

## Implementation sequence

1. Define the interrupt-frame ABI in one header and document exact offsets shared with assembly. Add compile-time offset/size assertions where possible.
2. Add owned kernel-stack metadata and bounded allocation/release for threads. Preserve the existing kernel-thread context-switch ABI while introducing a separate, explicit user-return context.
3. Implement TSS `RSP0` update at the point where a thread becomes current. Keep IST stack setup independent.
4. Change syscall entry/exit and blocking-syscall handoff so a frame is copied or created on stable per-thread storage before scheduling away. Ensure the restore path consumes the matching frame kind.
5. Define an idle-thread/handoff path for an empty run queue; never fall back to resuming a blocked current frame.
6. Integrate quarantine with scheduling: remove quarantined threads from runnable selection, preserve unrelated IPC-blocked state, and safely requeue only after quarantine is cleared.
7. Add regressions before enabling user-mode preemption/blocking in the live boot path.

## Required regression matrix

- Two CPL3 threads issue repeated syscalls while checking caller-saved and callee-saved registers, return values, RIP, RFLAGS and user RSP.
- Thread A blocks on an empty IPC receive; thread B performs repeated syscalls; a message wakes A; A resumes at the correct syscall and receives the expected message.
- Repeated switches between user and kernel address spaces verify the correct CR3, TSS `RSP0`, stack bounds and return frame.
- Timer IRQs interrupt CPL0 at multiple stack alignments; all registers and the original stack pointer are restored.
- No-successor cases verify that blocked, dead, or quarantined contexts are not resumed.
- Malformed frame metadata, stack-bound violations and failed stack allocation fail closed without returning to user mode.
- Quarantine/clear-quarantine tests verify run-queue removal and restoration without corrupting normal IPC blocking.

## Verification gate

Do not mark this fixed until all of the following are recorded against the same commit:

- NASM assembly succeeds.
- Clean build with `-Wall -Wextra -Werror -O2` succeeds.
- Kernel self-tests include the regressions above and pass.
- Debian/QEMU boot and runtime tests exercise actual CPL3 blocking/wakeup and context restoration.
- The test output and commit SHA are recorded. Source inspection alone is not a runtime pass.

## Current scope

This document records the required contract; it does not claim that the implementation is fixed. PR #132's timer EOI/register and alignment changes are related but do not, by themselves, solve per-thread syscall-frame lifetime or user-return-frame shape.


## Follow-up source audit (2026-10-15)

The scheduler/quarantine audit tracked in [issue #135](https://github.com/prosenjit-Adhikary/ProsenjitOS/issues/135) confirmed additional interactions that must be fixed together, not as isolated predicate changes:

- `thread_process_runnable()` currently filters only ZOMBIE/DEAD process states. A quarantined or process-BLOCKED thread can therefore still pass this predicate.
- `security_quarantine()` changes process state but does not atomically detach READY threads from the run queue. `security_clear_quarantine()` restores process state but does not restore the appropriate runnable threads.
- `thread_schedule_interrupt()` currently returns the incoming frame when no successor is found. That fallback is not acceptable after the current thread has blocked or been quarantined.
- The scheduler's current dequeue loop marks a candidate DEAD when `thread_process_runnable()` is false. Therefore adding quarantine/BLOCKED checks to that predicate without changing dequeue semantics would convert temporarily unschedulable threads into DEAD threads.
- IPC-blocked threads must remain blocked after quarantine clears; only threads that were runnable before quarantine may be re-enqueued. This requires per-thread quarantine bookkeeping or an equivalent explicit state model.

**Do not apply a one-line runnable-predicate fix.** The queue removal, lifetime-vs-eligibility distinction, quarantine restoration, and no-successor handoff must be implemented and tested as one scheduler state-machine change. Until then, this remains an open source-level correctness issue; no runtime reproduction is claimed.
