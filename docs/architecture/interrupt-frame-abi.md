# Interrupt-frame ABI and scheduler hand-off contract

Status: partially implemented / end-to-end runtime verification pending. The C layout assertions and CPL3 per-thread frame-copy foundation are present on the stacked implementation branch. Timer-driven scheduling is explicitly disabled because the IRQ0 raw frame does not match the scheduler restore ABI. Remaining checklist items are prerequisites before enabling preemption.

## Current source facts

- `kernel/idt.asm:isr128` saves 15 general-purpose registers in this order on the stack: RAX, RBX, RCX, RDX, RSI, RDI, RBP, R8, R9, R10, R11, R12, R13, R14, R15 (the push sequence is reversed because the stack grows down).
- The `struct pros_interrupt_frame` register section contains those 15 registers, followed by RIP, CS, and RFLAGS. It is 18 64-bit words (144 bytes) as currently declared; comments that call this “16 GPRs” are inaccurate.
- For a CPL3-origin interrupt, the CPU's return frame also contains user RSP and SS after RFLAGS. Those words are not currently members of `struct pros_interrupt_frame`.
- `isr128` uses IST1. Its current frame pointer therefore refers to the IST stack, not automatically to storage owned by the interrupted thread.
- `thread_schedule_interrupt()` currently stores the supplied pointer directly in `thread->interrupt_frame`. A later entry can reuse the IST stack, so this pointer does not establish stable per-thread frame ownership.
- `boot/entry.asm:enter_user_mode` clears IF before `iretq`. Do not enable CPL3 timer preemption until the return-frame and stack-switch path is validated.

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

- [ ] Add C layout assertions and matching assembly constants/comments; verify offsets from the actual NASM push order.
- [x] Introduce stable per-thread saved-frame storage for CPL3 RSP/SS; CPL0 frames remain on their owning kernel stack until same-CPL RSP restore is defined.
- [x] Update CPL3 ISR-to-scheduler hand-off to copy incoming frames into per-thread storage before IST1 can be reused.
- [ ] Implement matching restore paths for CPL0 and CPL3 frames, including correct stack cleanup and IRET behavior.
- [ ] Normalize the IRQ0 vector/error frame and define same-CPL kernel RSP restore before re-enabling timer-driven scheduling.
- [ ] Establish per-thread kernel-stack ownership and TSS.RSP0 updates before enabling CPL3 preemption.
- [ ] Add regression coverage for repeated syscalls from CPL3, blocked IPC wake/retry, successive IST entries, frame preservation across thread switches, and return to both CPL0 and CPL3.
- [ ] Run compiler/assembler checks, boot tests, and targeted QEMU runtime tests on the self-hosted Linux runner; use other runners for independent checks where the workflow supports them.
- [ ] Update the roadmap only after each runtime claim has matching evidence.

## Explicit non-claims

- A successful ISO/BIOS QEMU boot does not by itself prove arbitrary scheduler interleavings or a stable IST-frame lifetime.
- Existing synthetic C frame tests do not prove the real `isr128` CPL3 return path.
- This design note does not implement the changes above and does not enable user-mode timer preemption.
