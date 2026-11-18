# Phase 1 Panic Handoff — Frame and Stack Design Contract

Status: DESIGN ONLY — no panic-stack implementation or runtime verification is claimed.
Design source ref: `3da3728e10ab93a83b4c44057f1e4328b663740c`.

## Scope and non-goals

This document defines the first reviewable panic-handoff slice (Phase A). It does not enable disk writes, call ATA, change normal exception return behavior, or claim durable crash persistence. The existing capture-only crash-record API remains the only operation permitted from the ordinary exception context until the later panic-writer contract is separately reviewed.

The current design is single-CPU. No SMP safety is implied.

## Verified current entry path

- `kernel/idt.asm` defines error-code and no-error-code stubs. The stubs push a synthetic zero for no-error vectors, then push the vector, and branch to `isr_common`.
- `isr_common` executes `cli`, passes the current RSP to `exception_dispatch`, and later adds 16 bytes before `iretq`.
- IDT vectors 2, 8 and 14 use IST1, IST2 and IST3 respectively. Other exception gates use IST=0.
- `kernel/kernel.c` configures NMI, double-fault and page-fault ISTs. There is no panic IST or panic continuation stack.
- The exception C handler currently has both recoverable user-copy #PF behavior (it edits the saved RIP and returns) and fatal halt behavior. A panic transition must not accidentally capture or discard a recoverable user-copy fault.

## Critical frame-layout constraint

The common C handler currently receives a pointer to the raw stack frame. The frame is not one uniform seven-word structure for all exceptions:

1. The stub always contributes `vector` and `error_code` (the latter is synthetic zero for no-error vectors).
2. The CPU contributes RIP, CS and RFLAGS.
3. The CPU contributes old RSP and SS when a privilege-level transition or an IST stack switch causes those fields to be saved. For a same-CPL, IST=0 exception, those two words are not present.

Thus the raw frame length and the meaning of words after RFLAGS depend on the gate's IST and interrupted privilege level. In particular, #PF uses IST3 and has an IST stack-switch frame, while an ordinary same-CPL #GP currently uses IST=0. Any new assembly handoff must not assume that `frame[5]` and `frame[6]` are universally valid.

Before implementing a switch, define a normalized, tagged panic-frame representation containing at minimum:
- vector and error code;
- saved RIP, CS and RFLAGS;
- an explicit flag describing whether old RSP/SS are present;
- old RSP/SS only when the flag says they are valid;
- CR2 captured for #PF before any helper can fault;
- a version/size field so assembly and C layout drift is detected.

The assembly must identify the raw frame shape from the known vector/gate configuration and saved CS privilege bits. Do not infer presence of RSP/SS by reading beyond a same-CPL non-IST frame.

## Panic stack ownership contract

A dedicated panic stack must be statically reserved by the linker in kernel BSS or a dedicated alloc-free section, with explicit start/end symbols and alignment. It must not be allocated from the ordinary heap, PMM page allocator, thread stack-slot allocator, or ProsenjitFS. The linker symbols must be checked against the loaded kernel bounds and the boot memory map in both boot paths.

The first implementation should use one emergency panic stack only under the current single-CPU assumption. A separate atomic/early flag must make the first panic owner explicit. If a second exception occurs after ownership is claimed, the nested path must execute only a tiny assembly fail-stop sequence (disable interrupts and halt); it must not re-enter C, overwrite the normalized first frame, touch serial, health, filesystem, or storage.

The panic stack top must be 16-byte aligned before calling C. The assembly handoff must preserve the normalized first-fault frame in dedicated static storage before changing RSP. The panic continuation must never return through the original exception stub or execute `iretq`.

## Which faults may enter the panic continuation

- Fatal kernel exceptions: eligible after the first minimal capture and normalized-frame construction.
- Recoverable user-copy #PF: must retain the existing fixup-and-return path; it must not claim the panic stack or enter the fatal continuation.
- User-originated faults: policy must be explicit. A user fault that can be converted to process termination must not automatically become a machine-wide panic.
- NMI, double fault and page fault: preserve their existing IST semantics. A fault while already on a panic stack must take the nested fail-stop path. The double-fault handler must not depend on the potentially corrupted ordinary or panic C stack.
- Storage-originated fault: may record the in-memory frame, but later storage persistence must be marked prohibited for that panic instance.

The exact fatal-versus-recoverable classification must be reviewed against all exception vectors before enabling the continuation. Unknown vectors default to fail-stop without storage I/O.

## Required Phase A implementation/test sequence

1. Add linker-reserved panic-stack bounds and normalized-frame storage, without changing IDT behavior.
2. Add compile-time/static assertions for frame field offsets, sizes, stack alignment and stack capacity.
3. Add an assembly-only handoff entry that receives a synthetic test frame, validates its tagged shape, copies it to static panic-frame storage, switches to the dedicated stack, and enters a test continuation that records a pass marker then halts. It must not call ATA, block, health, serial, VFS or ProsenjitFS from the handoff path.
4. Add tests for error-code and no-error-code vectors; IST and non-IST frame shapes; same-CPL and privilege-transition shapes; absent versus present old RSP/SS; alignment; first-owner and nested-entry rejection.
5. Add a regression proving recoverable user-copy #PF still edits the original saved RIP and returns through the original frame unchanged apart from the documented fixup.
6. Run required Build and boot tests on the exact commit head, with ISO and Native BIOS QEMU markers. If the permitted runner is unavailable, keep the gate open and do not claim a pass.

## Explicit stop conditions before Phase B/C

Do not move to early production capture or device persistence until Phase A establishes:
- raw-frame shape is correctly normalized for every configured gate class;
- the original return frame is not corrupted;
- panic stack is mapped and reserved in both boot paths;
- nested-fault handling cannot recurse into C or overwrite the first record;
- exact-head Build and supported QEMU tests pass.

Phase C (panic-safe storage) remains a separate design and implementation gate. It must not reuse ordinary ATA/block wrappers or assume the faulting CPU released their locks.

## Current gate

**OPEN — DESIGN RECORDED, NOT IMPLEMENTED, NOT TESTED.** This document is an implementation contract only. It does not close the Phase 1 panic/crash gate.
