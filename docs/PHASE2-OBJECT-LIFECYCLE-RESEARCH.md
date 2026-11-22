# Phase 2 Research: Native Object Lifecycle and Handle Compatibility

**Status:** Research and architecture only; implementation is intentionally gated  
**Research branch:** `phase2/research-object-lifecycle-20261122`  
**Baseline:** `main` at `aad3c14c0814a542a27c2a86096c768e7b9bda99`  
**Priority dependency:** PR #642 — transactional process wait/reap

## 1. Scope and safety boundary

Phase 2 is intended to extend the existing native object manager and define compatibility adapters for Windows-style handles and Linux-style file descriptors.

This branch is deliberately limited to research and architecture documentation. PR #642 remains isolated and has priority. No Phase 2 implementation may modify its branch, overlap its changed files, or be merged as a dependency before the Phase 1 gates are satisfied. The same restriction applies to other active PRs: before any future source edit, compare the complete proposed file list against the then-current changed-file lists of every open PR and stop on any overlap or dependency.

No kernel source, build file, test, workflow, or ABI file is changed by this research note.

## 2. Existing implementation observed on main

The following facts were checked against the baseline source:

- `kernel/object.c` implements a common `pros_object` header with a 16-bit type and a plain 32-bit reference count.
- The current object types include process, thread, memory/address-space, file, device, socket, and IPC.
- `object_init` starts the count at one. `object_retain` rejects zero and overflow; `object_release` decrements and changes the type to `PROS_OBJECT_NONE` at zero.
- The reference count operations are ordinary non-atomic increments/decrements. They are not sufficient for concurrent SMP access without an explicit synchronization design.
- The handle table has 256 entries. The handle encoding uses an 8-bit slot and a 16-bit generation. Slot zero is reserved; generation wrap is avoided by retiring a slot at the maximum generation.
- Entries carry an object pointer and a rights mask. Existing operations include allocation, validation, close, duplicate, cross-process copy/move transfer, and process-exit close-all.
- `include/abi/capability.h` defines READ, WRITE, EXECUTE, DUPLICATE, TRANSFER, and ADMIN rights.
- `kernel/process.c` initializes the process object and closes its handle table during process resource release.

These are baseline observations, not a claim that all concurrent lifetime or destruction cases are already solved. In particular, the generic object header currently has no destructor callback or explicit object-finalization state.

## 3. Target object lifecycle contract

The implementation should converge on the following lifecycle contract:

1. **Construct:** initialize type, immutable identity/metadata, synchronization primitives, reference state, and type-specific destructor before publication.
2. **Publish:** make the object reachable only after construction succeeds. Publication must establish the initial owning reference.
3. **Acquire:** a lookup must acquire a live reference while protected against concurrent close/final release. A plain pointer lookup followed by an unprotected retain is unsafe.
4. **Use:** validate object type and requested rights at the operation boundary. A handle is not authority by itself; its rights must be checked for every privileged operation.
5. **Duplicate/transfer:** acquire the destination reference before publishing the new entry. Rights may be preserved or attenuated, never amplified. A move operation must have a defined all-or-nothing failure contract.
6. **Close:** atomically detach the handle-table entry, invalidate its generation, then drop its object reference after leaving the table critical section.
7. **Finalize:** exactly one thread transitions the object to finalizing when the final reference is released. Invoke the type-specific destructor exactly once, with a documented rule for whether it may sleep or acquire subsystem locks.
8. **Reclaim:** free storage only after no lookup, waiter, callback, or deferred operation can still access the object.

The object manager should distinguish the lifetime of the kernel object from the lifetime of a handle-table entry. Multiple handles may refer to one object; closing one entry must not destroy an object that still has references.

## 4. Concurrency and memory-safety requirements

Before SMP or preemptible concurrent use, the implementation needs a reviewed synchronization strategy. Candidate mechanisms include atomic reference operations plus a lock protecting handle-table entries; the final choice must match the kernel's actual scheduler and interrupt model.

Required invariants:

- A successful get operation cannot resurrect an object whose reference count reached zero.
- The final-release transition is unique.
- A handle lookup cannot race with close and return an unreferenced pointer.
- Close removes the entry before dropping its reference.
- Duplicate and transfer hold a stable source reference while constructing the destination.
- No destructor runs while holding a global handle-table lock unless the destructor contract explicitly proves that safe.
- Generation values never wrap into a previously valid handle value.
- Integer overflow, underflow, invalid type, invalid rights, stale generation, and exhausted-table conditions fail closed.
- Lock ordering across process, object, file, and wait subsystems is documented and tested.
- Any wait queue or asynchronous callback that stores an object pointer owns a reference until it unregisters or completes.

A refcount alone does not protect a raw pointer obtained before the reference is acquired. The lookup protocol must close that gap.

## 5. Windows HANDLE compatibility adapter

Windows compatibility should be an adapter over native objects, not a second independent object manager.

Proposed contract:

- A guest HANDLE is an opaque guest-visible token mapped through a per-process guest handle table.
- The adapter translates guest access masks into native rights using an explicit allowlist per supported object type.
- `CloseHandle` detaches the guest entry and releases exactly one guest reference.
- `DuplicateHandle` validates source-process access, target-process access, object type, and requested rights; it must never amplify rights.
- Inheritable handles are copied only when the process-creation contract requests inheritance. Inheritance policy must be explicit and tested.
- Pseudo-handles, reserved values, and unsupported Windows object types are handled explicitly; they must not be treated as ordinary native handles.
- Wait operations are exposed only for object types with a defined waitable contract. Unsupported wait targets return a documented compatibility error.
- Guest HANDLE values must not expose kernel pointers or native slot/generation layout.

The first compatibility milestone should support a small documented subset (for example, process/thread references and file-like objects) only after their native lifetime and wait semantics are stable. Do not claim full Win32 HANDLE compatibility.

## 6. Linux file-descriptor compatibility adapter

A Linux-like FD is not equivalent to a generic kernel object handle. It names a per-process descriptor entry that refers to an open-file description.

Proposed layering:

- **FD table entry:** per-process integer descriptor, descriptor flags (including close-on-exec), and a reference to an open-file description.
- **Open-file description:** shared file state such as current offset and file status flags, with its own lifetime/reference count.
- **Underlying object:** native file, device, pipe, or socket object implementing the appropriate operations.

Required semantics for a future supported subset:

- `open` allocates a descriptor entry and an open-file description.
- `close` removes only that descriptor entry and releases its reference.
- `dup` creates another descriptor referring to the same open-file description; shared offset semantics must be preserved.
- `fork` copies descriptor entries with references to the same open-file descriptions.
- `exec` closes entries marked close-on-exec.
- Descriptor flags are distinct from open-file status flags.
- Descriptor allocation, lookup, close, and duplication are race-safe.
- Standard descriptors and invalid descriptor behavior are specified by the ABI adapter, not hard-coded into the generic object manager.

Do not map each FD directly to a unique native object if that would break shared open-file-description semantics.

## 7. Rights and access-control model

The current capability bits provide a useful starting point, but the adapter needs explicit mapping tables. For every guest operation, define:

- accepted object types;
- required native rights;
- guest access-mask translation;
- whether rights can be attenuated on duplication;
- whether cross-process transfer is allowed;
- whether the operation is waitable or inheritable.

Unknown bits and unsupported rights must be rejected. Compatibility translation must not silently convert a guest access request into broader native authority.

## 8. Upstream research and license handling

No upstream source has been copied or imported.

| Candidate | Research value | License finding | Current disposition |
|---|---|---|---|
| FreeBSD `sys/kern/kern_descrip.c` | Descriptor table, open-file and descriptor lifetime patterns | The inspected file identifies itself as SPDX BSD-3-Clause | Study concepts; any future reuse requires exact revision, file-header, notice, and compatibility review |
| Fuchsia Zircon kernel object/handle code | Object and handle lifecycle design | Fuchsia's official policy says the Zircon kernel is under its MIT-style `zircon/kernel/LICENSE`; individual file headers and the exact revision still need verification | Promising research candidate; no import approved |
| Linux `fs/file.c` | FD table and close-on-exec semantics | Linux kernel source is GPL-2.0-only | Do not copy into a differently licensed component without explicit legal approval |
| ReactOS object manager | Windows object/handle behavior | ReactOS kernel source is GPL-2.0-or-later | Do not copy into a differently licensed component without explicit legal approval |
| seL4 | Capability and object-lifetime concepts | GPL-2.0-only | Concepts may be studied; no source import |

Primary references:
- FreeBSD descriptor source: https://cgit.freebsd.org/src/tree/sys/kern/kern_descrip.c
- FreeBSD license guide: https://docs.freebsd.org/en/articles/license-guide/
- Fuchsia licensing policy: https://fuchsia.dev/fuchsia-src/contribute/governance/policy/open-source-licensing-policies
- Fuchsia Zircon handle implementation: https://fuchsia.googlesource.com/fuchsia/+/HEAD/zircon/kernel/object/handle.cc
- Linux descriptor source: https://github.com/torvalds/linux/blob/master/fs/file.c
- ReactOS object reference source: https://github.com/reactos/reactos/blob/master/ntoskrnl/ob/obref.c
- seL4 source: https://github.com/seL4/seL4

Before any import, record the exact upstream commit, file-level SPDX/header, applicable license text, copyright notices, and required attribution. A project-wide license statement is not a substitute for checking the actual source file and revision.

## 9. Required verification matrix

| Area | Required cases |
|---|---|
| Construction | invalid type, failed allocation rollback, initial reference, publication ordering |
| Reference lifetime | retain/release, overflow, underflow, get-unless-zero, final release exactly once |
| Handle table | slot exhaustion, slot zero, stale generation, generation retirement, invalid token |
| Rights | missing right, unknown bit, attenuation, no amplification, wrong object type |
| Duplicate/transfer | same-process duplicate, cross-process copy, move success, move rollback, source close race |
| Process exit | close-all, outstanding external references, waiters/callbacks, repeated cleanup |
| Concurrency | lookup vs close, duplicate vs close, simultaneous final release, table reuse, lock-order stress |
| Windows adapter | close, duplicate, access mapping, inheritance, pseudo-handle rejection, waitable subset |
| Linux adapter | open/close, dup shared offset, fork reference sharing, close-on-exec, descriptor reuse |
| Failure behavior | allocation failure, invalid guest pointers, unsupported operation, partial transfer rollback |

Host-side tests should be deterministic and include fault injection. Kernel/QEMU tests should be added only after the source-file conflict gate and Phase 1 dependency gate permit implementation.

## 10. Implementation gates and ordering

Implementation must not begin merely because this research note exists.

1. **Gate A — PR #642:** independent review, required tests, exact-head CI, approval, and merge. Recheck the exact head SHA immediately before relying on any CI result.
2. **Gate B — conflict scan:** fetch the current open-PR set and every changed-file list. Compare the complete proposed source-file set. Any overlap or dependency means stop.
3. **Gate C — native object lifetime:** define and test synchronization, safe lookup/reference acquisition, unique finalization, destructor ownership, and process-exit cleanup.
4. **Gate D — generic handle semantics:** harden duplicate, transfer, rights attenuation, stale-token defense, and concurrency.
5. **Gate E — Linux FD layer:** implement open-file-description semantics and descriptor flags as a separate adapter.
6. **Gate F — Windows HANDLE layer:** implement a deliberately limited, documented subset over the native object manager.
7. **Gate G — verification:** run targeted host tests, full build, security checks, and relevant QEMU tests at the exact proposed commit; obtain review before merge.

Phase 2 is not complete until the implementation and verification gates are satisfied. This document completes the research/architecture deliverable only; it does not claim code-level completion.


## 11. Phase 1 dependency checkpoint — 2026-11-22

A fresh check was performed before any further Phase 2 work:

- PR #642 remains open and unmerged.
- Current head branch: `phase1/transactional-wait-reap-20261114`.
- Current exact head: `10e42f872614138bebff10fadb693fd4ec627b06`.
- Current base: `main` at `aad3c14c0814a542a27c2a86096c768e7b9bda99`.
- The Build and boot-test workflow completed successfully at this exact head (run 45046859231).
- The Security architecture workflow completed successfully at this exact head (run 45046859230).
- No submitted review and no inline review thread are recorded at this checkpoint.
- PR #642 still changes `kernel/object.c`, `kernel/include/object.h`, and `kernel/process.c`, among other files. Those paths remain protected from Phase 2 edits.

The PR description continues to record source-audit and verification work that requires reviewer scrutiny, including stronger argv/envp content assertions and aggregate preflight/rollback invariants. Green CI alone does not satisfy the independent-review, approval, and merge gates.

The Phase 2 research branch therefore remains documentation-only. No source implementation, dependent merge, or modification to PR #642 has been made. Recheck this checkpoint against the live PR before starting any implementation; the SHA and workflow results above are historical facts for this checkpoint, not permanent guarantees.
