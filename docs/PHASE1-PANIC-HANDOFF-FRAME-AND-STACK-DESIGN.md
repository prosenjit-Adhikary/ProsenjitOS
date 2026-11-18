# Phase 1 Panic Handoff — Frame and Stack Design Contract

Status: PARTIAL PHASE A IMPLEMENTATION — isolated assembly entry, normalized-frame validation, first-owner checks, and reversible stack-switch tests exist; production exception routing remains disabled.
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


## Phase A implementation note — static reservation

The first storage-only implementation slice is now present on the Phase 1 branch:

- `kernel/linker.ld` reserves a page-aligned 16 KiB panic stack and a separate 128-byte, 16-byte-aligned normalized-frame storage slot inside the kernel BSS output section.
- Because both reservations are inside `__kernel_bss_start..__kernel_bss_end`, they are included in the kernel image's BSS extent and in the allocator's existing `__kernel_start..__kernel_end` reservation.
- `kernel/paging.c` validates the exact sizes, alignment, BSS containment and identity mappings, with supervisor RW + NX permissions. It checks both ends of each reserved range after mapping.
- No IDT gate, exception stub, `exception_dispatch()` behavior, recoverable user-copy #PF path, or storage path was changed. The new stack and frame slot are not yet consumed by exception handling.

This closes only the static-reservation/mapping subtask. It does **not** establish that the native BIOS loader clears BSS, and no such zero-initialization claim is needed for the stack reservation: the future handoff must write every frame field before reading it. The native boot path's complete memory-extent/E820 contract still needs a separate check before claiming both boot paths safe. Build, ISO boot, native BIOS boot, and synthetic handoff tests remain unverified for this new head.


### Native-entry BSS initialization

A follow-up boot-path fix now makes the kernel entry initialize the complete linker-defined BSS extent before using any BSS-resident bootstrap resource:

- `boot/entry.asm` preserves the Multiboot2 magic and information pointer in registers, clears `__kernel_bss_start..__kernel_bss_end` with a bounded dword `rep stosd`, then restores those values into their BSS slots.
- The clear runs before loading `stack_top`, initializing page tables, or using the BSS-resident TSS and C globals. This is required for the native BIOS flat-image loader, which copies the file-backed kernel bytes and does not itself materialize ELF NOBITS zero-fill semantics.
- A non-dword-aligned BSS extent fails closed before the bootstrap stack is touched. The linker currently page-aligns the BSS end, so the expected contract is satisfied by construction; exact-head build verification is still required.
- The panic stack/frame reservations are now covered by both the BSS clear and the existing BSS page mapping. This does not yet switch exception handling to the panic stack or validate the normalized frame.


### Link-time contract enforcement

The linker script now has hard `ASSERT` checks for the panic stack's 4 KiB alignment and exact 16 KiB size, the frame slot's 16-byte alignment and exact 128-byte size, containment of both reservations inside kernel BSS, and dword alignment of the complete BSS extent required by the native startup clear. These checks make layout drift a link failure rather than a runtime surprise. They still require an exact-head build to demonstrate that the current toolchain accepts the assertions.


### Normalized frame ABI declaration

`kernel/include/panic_handoff.h` now declares the version-1 normalized snapshot layout and explicit validity flags for old RSP/SS, CR2, user-origin and IST usage. Compile-time assertions pin the 128-byte size and key field offsets; the paging reservation checks consume the shared size constants rather than duplicating magic numbers. This is a declaration/compile-time contract only: no assembly producer or exception-handler consumer has been connected yet.



### Exact-head CI evidence — 2026-11-18

The PR validation workflows were inspected at job and log level after the static reservation, native BSS initialization, linker assertions, and versioned frame ABI commits.

- PR head under review: `b35f6e9c33f1d156e25f055d7b932acaaf869eb5`.
- GitHub generated merge checkout: `f796be0e470cf0feb0aa3be23fae1b7f34eb6d48`, explicitly showing the merge of the above PR head into base `aad3c14c0814a542a27c2a86096c768e7b9bda99`.
- Security architecture checks: workflow run `44289603413`; runner-selection and security-design jobs completed successfully.
- Build and boot-test: workflow run `44289603462`; runner-selection and build jobs completed successfully. The build job's merge-checkout verification, architecture/documentation contracts, kernel/artifact build, ISO QEMU boot test, and Native BIOS QEMU boot test steps all completed successfully.
- The runner selector could not inspect repository runners and explicitly selected the GitHub-hosted fallback. This is not evidence that the offline local runner recovered.
- Native QEMU returned timeout status 124 after its bounded observation window. The workflow explicitly accepts 124, then requires the native serial boot milestones and regression markers; those checks completed successfully. This is a bounded boot-test success, not a claim that QEMU exited naturally.
- The Native BIOS serial output includes the process wait/reap transaction regression and ProsenjitFS persistence/mount regression pass markers. The ISO and Native BIOS boot milestones were both asserted by the workflow.

**Scope limitation:** these runs prove that the current code builds and passes the existing architecture/security and boot-test suites on the PR merge checkout containing this head. They do not prove the future panic assembly producer, normalized-frame runtime copying, nested-entry fail-stop, recoverable user-copy #PF preservation under a panic handoff, or synthetic exception-frame matrix: those mechanisms are not implemented yet.

Accordingly, the static reservation and ABI declaration have exact-merge-checkout build/boot evidence, but **Phase A remains OPEN** until the assembly handoff and its focused regressions are implemented and tested.



### Recoverable user-copy page-fault discriminator audit — 2026-11-18

The current user-copy implementation was inspected before changing exception entry:

- `kernel/usercopy.c` exports the aligned BSS symbol `usercopy_fault_active_state` and the associated `usercopy_fault_rip_state`; the C accessors are only simple reads/clears of those globals.
- `kernel/usercopy.asm` disables interrupts before publishing the active flag and fixup RIP, performs the byte-copy loop, and clears both values on success or at its fixup label before restoring the saved flags.
- Therefore a #PF delivered during the protected copy window sees a stable active flag under the current single-CPU/interrupt-disabled contract. The page-fault gate is IST3, so the raw CPU frame includes the old RSP/SS pair even for a same-CPL kernel fault.
- A fatal-handoff selector can avoid calling a C helper before the stack switch: for vector 14 it can read the exported active-state symbol directly and route an active user-copy fault through the existing exception path without claiming panic ownership. Other exceptions and inactive #PFs remain eligible for the fatal path, subject to the final vector policy.
- Do not use `usercopy_fault_rip_state != 0` alone as proof of a recoverable fault, and do not clear either state variable in assembly entry. The existing C recovery path owns the fixup and clears the state after it has updated the original saved RIP.
- The selector must preserve the original IST3 frame and must not normalize/copy it in a way that changes the saved RIP or the return path. The legacy handler currently performs diagnostics before its recovery branch; this is not ideal for fault-path latency, but changing that ordering is a separate, explicitly tested hardening change and must not be mixed into the first handoff slice.

**Implementation boundary:** this audit establishes a viable no-C-call discriminator for the existing recoverable #PF path; it does not yet implement or validate the assembly branch. Synthetic tests must prove both active user-copy #PF bypass and inactive #PF handoff, including unchanged original frame contents on the recovery branch.



### Exception-entry integration audit — 2026-11-18

A second source pass inspected the current IDT, common exception stub, user-copy assembly and C dispatcher. This identifies why the next change must be an isolated assembly test entry first, rather than switching the production exception path immediately.

#### Confirmed current gate layout

- All vectors 0–31 initially use IST=0.
- Vector 2 (NMI) uses IST1; vector 8 (double fault) uses IST2; vector 14 (page fault) uses IST3.
- Vector 13 (#GP) explicitly remains IST=0.
- The current `ISR_NO_ERROR` stubs push synthetic error=0 and then the vector. `ISR_WITH_ERROR` stubs push only the vector because the CPU already pushed the error code.
- `isr_common` passes the raw stack pointer directly to `exception_dispatch`, then unconditionally returns through the original frame with `iretq`. There is currently no panic-stack switch, no normalized-frame producer and no first-owner state in assembly.

#### Confirmed recoverable #PF constraints

The page-fault gate's IST3 means the old RSP/SS words are present in the raw frame even for the current same-CPL user-copy fault. The user-copy assembly disables interrupts before setting `usercopy_fault_active_state` and publishing its fixup RIP, and keeps interrupts disabled until it clears both values. This gives a stable discriminator on the current single-CPU implementation.

However, the current C dispatcher performs nested-guard handling, counter updates, serial output and ProsenjitFS diagnostic reads before it checks `usercopy_fault_active()`. It then records the fixup RIP into raw `frame[2]`, clears the user-copy state and returns to the original stub. A fatal-only assembly branch must therefore detect active user-copy #PF before claiming panic ownership or switching stacks, and route it to the legacy C recovery path unchanged. Merely switching all vector-14 entries and asking the new C continuation to decide later would violate the existing recovery contract.

#### Blocking policy issue: user-origin exceptions

The IDT is configured for CPL3 execution and the exception stubs receive the saved CS. The current dispatcher halts for every non-recoverable exception; there is no established per-process user-exception termination/reap path in this dispatcher. The panic design requires user faults that can be contained to a process not to become machine-wide panics, but the current code does not yet provide a safe process-fault disposition contract for all vectors.

Do not add a blanket `CS.RPL == 3` panic exclusion either: that would silently return or mishandle user exceptions without a defined disposition. Before production routing, enumerate each configured exception and define whether it is recoverable, process-fatal, kernel-fatal, or unconditional fail-stop, including how the original frame is consumed.

#### Safe next implementation boundary

1. Keep `isr_common` and every IDT gate unchanged while building an isolated synthetic-frame assembly entry.
2. Make the isolated entry accept only a pointer to a fully formed version-1 normalized frame; validate version and size before claiming ownership.
3. Use a dedicated first-owner word and an atomic compare/exchange. A losing/nested entry must execute only `cli; hlt` and must not call C or touch the saved frame.
4. Copy all 128 bytes to the linker-reserved storage before switching RSP. The copy must not read from or write to the original exception frame.
5. Switch to the statically reserved panic stack, align it for the SysV ABI, and call only a test continuation. The continuation must record a RAM-only test marker and halt; it must not invoke serial, health, filesystem, block or ATA paths.
6. Test the isolated entry with synthetic frames covering both error-code classes, IST/non-IST, same-CPL and privilege-transition shapes, optional RSP/SS and CR2 flags, invalid ABI rejection, first-owner success and nested rejection.
7. Separately prove that the production user-copy #PF path still takes the original `exception_dispatch` route and edits only the documented saved RIP.

No production exception routing should be enabled until the per-vector user/kernel fault disposition is reviewed and the isolated handoff tests pass. The existing exact-head CI evidence for the prior head does not cover this new entry because it has not yet been implemented.


## Implementation progress — isolated assembly handoff path (2026-11-18)

The following implementation is now present on the Phase 1 branch:

- `kernel/panic_handoff.asm` validates the version-1 normalized frame without reading optional fields unless their validity flags are set. It checks the version/size, vector range, selector privilege bits, old RSP/SS consistency, CR2/vector relationship, IST/old-stack relationship, unknown flags, and reserved words.
- The first-owner state uses an atomic `lock cmpxchg`. A losing entry branches directly to `cli; hlt`; it does not call C or touch the saved snapshot.
- The isolated `pros_panic_handoff_enter_test()` entry copies all 128 bytes into `__panic_frame_storage` before changing RSP, then switches to `__panic_stack_top` and calls an assembly-only continuation.
- The continuation writes only a RAM marker and, with the default return mode, executes `cli; hlt`. A narrowly scoped reversible test mode allows the boot self-test to return to its original test stack after proving the complete first-owner/copy/stack-switch path; this mode is not connected to IDT or exception dispatch.
- `kernel/panic_handoff.c` exercises valid kernel same-CPL, user privilege-transition, and IST/#PF normalized frames; rejects malformed version/size, misaligned frame pointers, selector/flag mismatches, invalid optional fields, unknown flags and nonzero reserved fields; checks first-owner/duplicate claim behavior; and verifies the copied 128-byte frame, panic-stack bounds, SysV call alignment and restoration of the original caller stack.
- `kernel/kernel.c` runs only these isolated tests during normal boot. It does not invoke the non-returning continuation in production mode and does not change `kernel/idt.asm`, `isr_common`, IDT gate IST assignments, or `exception_dispatch()`.

### Exact-head verification

Tested PR head: `4671900d7f873e2c9ee62a10af0e7d993f62ba05`.

- Build and boot-test workflow `44350208630`: **SUCCESS**.
- Security architecture workflow `44350208539`: **SUCCESS**.
- The ISO and Native BIOS QEMU boot logs both contain `isolated panic frame validation/owner selftest passed`. That marker is emitted only after the reversible full-entry test, byte-for-byte frame-copy check, dedicated-stack range/alignment check, and original-stack restoration check all succeed.
- The runner selector used `RUNNER_MODE: github-hosted`; this does not claim local-runner recovery.

### Remaining Phase A gates

- The actual duplicate-owner `cli; hlt` branch is present and source-reviewed. The atomic losing-claim is exercised, but a second entry is not deliberately triggered in the standard QEMU boot because it intentionally halts the CPU. A dedicated bounded test harness is still required to verify this fail-stop branch independently.
- The active user-copy #PF discriminator and legacy recovery path have not been changed. A focused regression must still prove that recoverable #PF bypasses panic ownership and preserves the original exception return frame except for the documented RIP fixup.
- The normalized-frame ABI is tested with representative kernel, user, and IST shapes; the full configured-vector/error-code/IST matrix and production fatal-versus-process-fault policy remain open.
- No production exception routing, crash persistence, serial diagnostics, health reporting, filesystem, block, or ATA work is routed through the new continuation.

**Phase A remains OPEN.** The current evidence proves the isolated first-owner handoff path and reversible stack-switch mechanics under both boot paths, not a production panic system or SMP safety.
