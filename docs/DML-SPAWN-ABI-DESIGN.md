# DML production process-launch ABI — design draft

## Status

**Design draft only; not a published syscall ABI and not implemented.** No syscall number is reserved by this document. Do not write kernel or userspace code against this draft until the kernel/security owners review the contract and the ABI number, request layout, handle type, and error semantics are explicitly approved.

## Existing implementation boundary

- pros_dml_manager validates manifests and resolves dependency-first launch order.
- pros_dml_runtime orchestrates dependency start/stop through synchronous callbacks and rolls back dependencies it successfully started when a later start fails.
- The host tests use a mock backend. They do not start OS processes.
- pros_init_launch() is an internal kernel launch transaction that needs a mounted VFS, file-security metadata, caller-owned process/thread objects, IDs, and kernel-stack bounds. It is not a userspace spawn API.
- The current public syscall ABI provides wait/reap and exit, but no general process-spawn syscall. The DML runtime must not be wired to a mock or a privileged boot-time shortcut and described as production launch.

## Design goals

1. Launch a child only from a valid caller process and under explicit policy authorization.
2. Reference an executable through a kernel-validated object/handle with read/execute rights, not an unchecked userspace path string.
3. Copy and validate a bounded request from user memory before allocating child resources.
4. Run the same filesystem executable-admission/quarantine/block checks used by other filesystem-backed execution.
5. Create the child as non-runnable, load ELF into its own address space, build a verified CPL3 initial frame, assign least-privilege syscall/capability policy, then publish READY/enqueue as the final commit step.
6. On any pre-commit failure, release every acquired resource and ensure no runnable child or stale PID/handle remains.
7. Return a stable process identity/handle only after successful commit. Define how the caller closes the handle and how exit status is collected.
8. Preserve direct-child wait/reap, exactly-once teardown, and the rule that a process cannot be reaped while a thread remains live/current.

## Candidate request shape (not ABI-stable)

A future request should be fixed-width, versioned, pointer-width independent where possible, and include:
- struct_size and abi_version;
- an executable capability/handle plus requested access mode;
- a bounded argument-vector descriptor (count plus user address) and an explicit total byte length;
- optional bounded environment data only if the first implementation has a concrete need;
- explicit requested policy profile selected from kernel-approved profiles, never arbitrary syscall/capability masks from the caller;
- output PID/process-handle fields written only after successful launch.

The exact field widths, maxima, alignment, reserved-field requirements, pointer rules, and whether arguments are inline or indirect must be decided with the kernel's usercopy implementation and handle table before this becomes a shared ABI. Reject unknown versions, undersized/oversized structures, nonzero reserved fields, integer overflow, unterminated strings, invalid UTF-8 policy violations if UTF-8 is required, unmapped/unwritable output ranges, and aliased input/output ranges where copying could create ambiguity.

## Required authorization and validation

- Check syscall policy before request processing.
- Validate that the executable handle belongs to the calling process and grants the exact rights required; do not trust a raw PID, pointer, path, or object address as authority.
- Create an explicit security intent and require an allow decision before executable bytes are loaded.
- Check executable admission/quarantine/block state before ELF loading.
- Copy the complete request and argument bytes into kernel-owned storage before changing process state.
- Bound every count and byte length and use checked arithmetic for every offset/size calculation.
- Use a documented least-privilege child profile. The caller cannot escalate the child by choosing PROS_SECURITY_SUPER or setting arbitrary policy bits.
- Do not inherit parent handles implicitly; any inheritance/transfer must be explicit and rights-attenuated.

## Transaction and ownership requirements

1. Validate caller, executable capability, request, arguments, policy, and security admission.
2. Reserve PID/TID and allocate process/thread/kernel-stack resources with explicit ownership tracking.
3. Create/register the child as NEW (not scheduler-eligible).
4. Load/validate ELF and map user pages with W^X and user/kernel isolation enforced.
5. Build and validate the initial CPL3 interrupt/IRET frame.
6. Install child syscall/capability policy and only explicitly authorized handles.
7. Publish READY and enqueue the initial thread as the transaction's commit point.
8. Publish output PID/handle only after commit. If writing the output can fail, either validate and pin/copy the output destination before commit or specify a rollback-safe publication protocol.
9. On every earlier failure, unwind in reverse order. A rollback failure must remain observable and must not silently orphan a registered process, runnable thread, address space, or kernel stack.

The current caller-owned object model must be reconciled with process-table registration: document who retains the process/thread structs and stack after a successful launch, and which component releases them after wait/reap. Never use stack-local objects for child lifetime.

## DML adapter contract

The eventual DML start adapter must:
- resolve a registered manifest to a trusted package/executable object using a future package service; manifest entry_point text alone is not authority;
- call the reviewed spawn interface with validated arguments and a fixed kernel-approved policy profile;
- return a nonzero opaque handle only after successful launch and stable ownership registration;
- ensure failure means no child remains live, or return a distinct recoverable/rollback error while retaining enough state to retry cleanup;
- not report the desktop session as RUNNING until the child has actually reached the launch commit point.

The stop adapter must request termination through a documented authorized operation, wait for termination, reap the child, and release its process handle only after the child is no longer live. A timeout or denied termination must retain the handle and surface a failure; it must not pretend the process stopped.

## Test gates before implementation can be called production-ready

- ABI version/size/reserved-field rejection and unknown syscall behavior.
- Invalid, stale, wrong-type, wrong-owner, and insufficient-rights executable handles.
- Invalid/unmapped/overflowing argument and output ranges; argument-count and total-byte limits.
- Denied security intent, quarantined/blocked executable, malformed ELF, unsupported ELF features.
- PID/TID exhaustion and each allocation failure point, including thread/stack/frame construction.
- Parent policy denial and least-privilege child policy enforcement.
- No implicit handle inheritance; explicit transfer attenuation.
- Child exits before parent wait, wait before exit, double wait, non-child wait, child with descendants, and output-copy failure semantics.
- Fault injection at every transaction stage proves no orphan process, runnable thread, leaked stack/address space, stale PID, or double free.
- Real end-to-end DML launch and stop on a booted system, in addition to OfficeWindows host tests.

## Decisions required before coding

- Kernel-approved executable object/handle type and its lifecycle.
- Exact spawn ABI number and request layout, after ABI review.
- Argument/environment limits and string encoding policy.
- Process handle versus PID return semantics and close/wait ownership.
- How caller-owned process/thread structs and kernel stacks are allocated/freed.
- Whether termination is synchronous, waitable, or cancellable and how timeout/error recovery works.
- Package lookup/signature/trust model; a manifest's relative entry path must not bypass package trust or filesystem execution admission.

## Non-goals

This is a userspace launch contract. It does not move the DML or desktop environment into kernel-core, implement a window system, or claim Windows application compatibility.
