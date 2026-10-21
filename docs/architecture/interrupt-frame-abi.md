# Interrupt-frame ABI and scheduler hand-off contract

Status: partially implemented / end-to-end runtime verification pending. The C layout assertions and CPL3 per-thread frame-copy foundation are present on the stacked implementation branch. Timer-driven scheduling is explicitly disabled because the IRQ0 raw frame does not match the scheduler restore ABI. Remaining checklist items are prerequisites before enabling preemption.

## Current source facts

- `kernel/idt.asm:isr128` saves 15 general-purpose registers in this order on the stack: RAX, RBX, RCX, RDX, RSI, RDI, RBP, R8, R9, R10, R11, R12, R13, R14, R15 (the push sequence is reversed because the stack grows down).
- The `struct pros_interrupt_frame` register section contains those 15 registers, followed by RIP, CS, and RFLAGS. It is 18 64-bit words (144 bytes) as currently declared; comments that call this “16 GPRs” are inaccurate.
- For a CPL3-origin `INT 0x80`, the CPU's return frame contains user RSP and SS after RFLAGS. `struct pros_interrupt_frame` models only the first 18 qwords; the per-thread storage capacity is 20 qwords and preserves the two trailing CPL3 words.
- `isr128` uses IST1 for both CPL3 and CPL0 entries. A CPL3 frame initially lives on reusable IST1 and must be copied before another entry reuses that stack. A CPL0-origin entry also lands on IST1, so it is not a valid same-CPL kernel-stack return frame.
- `thread_schedule_interrupt()` copies a frame into `thread->interrupt_frame_storage` only when the saved CS indicates CPL3. The IPC receive syscall now refuses to block on an empty queue unless the saved CS is CPL3; this prevents the CPL0 IST frame from being retained by the scheduler. CPL0 timer scheduling remains disabled.
- `isr32` places synthetic vector/error words between the saved GPRs and CPU IRET frame. Its raw frame is not the scheduler's 18-qword layout; `timer_should_preempt()` therefore returns zero until a normalized IRQ0 hand-off and same-CPL stack policy are implemented.
- `boot/entry.asm:enter_user_mode` clears IF before `iretq`. Do not enable CPL3 timer preemption until the return-frame and stack-switch path is validated.
- The current IPC wait path has an SMP-sensitive publication window: `ipc_channel_wait()` marks the thread blocked and publishes it in the waiter list, then `syscall_dispatch()` sets `blocked_syscall` after the channel lock has been released. A concurrent sender could observe a partially published blocked syscall; the existing single-CPU QEMU boot tests do not establish SMP safety. Do not claim SMP-safe IPC blocking until waiter state, saved frame, and syscall continuation are published as one synchronized transition.

## Required ABI invariants

1. Define named layouts for at least:
   - CPL0-origin interrupt return frame: 15 saved GPRs + RIP/CS/RFLAGS.
   - CPL3-origin interrupt return frame: 15 saved GPRs + RIP/CS/RFLAGS/RSP/SS.
   - Any normalized scheduler-owned frame, including an explicit origin/shape tag if a single representation is used.
2. Use compile-time assertions for offsets and sizes shared between C and assembly. Document the exact pointer passed to each C handler, including any alignment padding and synthetic vector/error words.
3. A suspended thread's saved frame must live in storage that remains valid until that thread resumes. Do not retain a pointer into a reusable IST stack as the thread's only saved state.
4. Restore must match the origin and frame shape. A CPL3 return must restore the complete five-word CPU IRET frame; a CPL0 return must not consume user RSP/SS words that are absent.
5. Define the kernel-stack policy for every thread and update TSS.RSP0 before any future privilege transition that depends on it. Keep IST stacks reserved for their configured exception/interrupt roles.
6. Specify who owns the incoming frame, when it is copied, and how blocked syscalls update saved RIP/RAX without modifying another thread's frame.
7. Keep IF clear in user entry until the full CPL3 save/restore, address-space transition, per-thread kernel stack, and TSS policy are tested end to end.

## Implementation and verification sequence

- [x] Add C compile-time size/offset assertions for the 18-qword C frame; matching assembly constants and a full offset audit remain pending.
- [x] Introduce stable per-thread saved-frame storage for CPL3 RSP/SS; CPL0-origin IST1 blocking is rejected, and CPL0 frame scheduling remains unsupported until same-CPL RSP restore is defined.
- [x] Update CPL3 ISR-to-scheduler hand-off to copy incoming frames into per-thread storage before IST1 can be reused.
- [ ] Implement matching restore paths for CPL0 and CPL3 frames, including correct stack cleanup and IRET behavior.
- [ ] Normalize the IRQ0 vector/error frame and define same-CPL kernel RSP restore before re-enabling timer-driven scheduling.
- [ ] Establish per-thread kernel-stack ownership and TSS.RSP0 updates before enabling CPL3 preemption.
- [ ] Add targeted runtime regressions for blocked IPC wake/retry, repeated IST1 entries while a thread is suspended, and real CPL0/CPL3 return paths. The current CPL3 syscall/IPC test exercises multiple ordinary syscalls but does not force the blocked-receive scheduler hand-off.
- [x] Run compiler/assembler checks, ISO build, ISO and native-BIOS QEMU boot tests, and the CPL3 user-mode syscall/IPC test on the self-hosted Linux runner; latest PR #153 head passed both build/boot and security workflows.
- [ ] Update the roadmap only after each runtime claim has matching evidence.

## Explicit non-claims

- A successful ISO/BIOS QEMU boot does not by itself prove arbitrary scheduler interleavings, per-thread TSS.RSP0 correctness, or a stable IST-frame lifetime under repeated hardware entries.
- The synthetic C frame tests exercise copy/selection logic but do not execute the real `isr128` CPL3 return path or prove that `iretq` returns through the copied storage correctly.
- This design note does not implement the changes above and does not enable user-mode timer preemption.
