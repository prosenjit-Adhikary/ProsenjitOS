# Native Userspace Spawn/Exec Contract

Status: design gate; implementation is not yet complete.

This contract is a prerequisite for desktop service startup. It does not claim that a general spawn/exec syscall, a user-mode runtime, or a graphical desktop session exists.

## Architecture boundary

- Native ELF loading and native packages stay on the Native subsystem path.
- Windows PE/COFF, MSI, Windows APIs, DLLs, and Windows services stay behind a separate Windows subsystem.
- Both paths may use shared user-space desktop services only after a supported process-launch and IPC path exists.
- The kernel owns address-space, process/thread lifecycle, capability checks, and syscall enforcement. Package managers, service orchestration, display servers, input routing, and desktop session policy remain user space.

## Required spawn transaction

1. Validate the caller is active and authorized by its syscall policy and capabilities.
2. Resolve the executable through an explicitly selected native VFS/provider path; do not guess a host path or fall back to a foreign backend.
3. Validate the ELF header, architecture, program-header bounds, load-segment arithmetic, entry point, and user address ranges before publishing a process.
4. Allocate process, address space, image mappings, initial user stack, and thread state through explicit ownership-aware APIs.
5. Keep the new process unpublished or non-runnable while construction is incomplete.
6. Apply the target's syscall/capability policy before it can execute.
7. Register the process and thread only after all prerequisites succeed; publish READY/enqueue as the final commit step.
8. Return a stable process identifier/handle only after successful publication.

## Failure and rollback invariants

- Every acquired resource has one documented owner and one cleanup path.
- Any failure before publication must leave no runnable thread, process registry entry, stale thread registry node, leaked mapping, open executable handle, or partially published identifier.
- Rollback proceeds in reverse acquisition order and preserves the original error for diagnostics.
- Do not free caller-owned process/thread storage unless an explicit allocator contract grants ownership.
- Never overwrite a caller's output identifier or exit-code output until the operation has succeeded.
- Never execute an image with writable+executable user mappings; preserve existing W^X/NX enforcement.
- Unknown, malformed, unsupported, or out-of-range inputs fail closed.

## Exit, wait, and reap invariants

- Exit transitions must not publish a zombie result until the thread termination and address-space transition succeed.
- Wait verifies parent-child ownership and zombie state before releasing resources.
- Final reap preflights all threads and IPC wait-channel references before unlinking any registry entries.
- Registry detachment and storage freeing are separate operations: detach caller-owned thread objects first; free only when an allocator/ownership API explicitly permits it.
- An unsuccessful wait/reap must not publish an exit code or leave a half-unregistered process.

## Required tests before enabling a public syscall

1. Invalid caller, denied syscall policy, and missing capability.
2. Missing executable, wrong format/architecture, truncated ELF headers, overflowed offsets/sizes, invalid program-header table, overlapping/invalid segments, invalid entry point, and invalid stack bounds.
3. Fault injection at every allocation, mapping, handle, registry, and enqueue boundary; assert complete rollback and no registry-count change.
4. Verify user mappings are not writable and executable simultaneously.
5. Verify failed spawn never becomes visible to lookup or scheduler.
6. Verify successful spawn reaches a valid CPL3 entry with a stable per-thread interrupt frame and correct user RSP/SS.
7. Verify exit/wait/reap refuses live threads and residual IPC wait-channel references, then clears registry links after a successful reap.
8. Run focused unit tests, warnings-as-errors build, ISO QEMU milestones, native BIOS QEMU milestones, and security architecture checks on the exact resulting commit.

## Enablement gate

Do not add a public syscall number, wire a mock backend into boot, or claim spawn/exec support until the syscall ABI, VFS executable lookup, ELF mapping/relocation policy, user-stack setup, thread frame/TSS integration, ownership-aware rollback, and tests are reviewed together. Do not enable user-mode timer preemption until the real interrupt-frame and context-switch path is verified end-to-end.

## Desktop integration gate

Only after the native spawn/exec contract is implemented and verified should user-space service startup, display/window routing, keyboard/pointer delivery, and boot-to-desktop integration proceed. Windows PE/API compatibility remains a separate downstream implementation.
