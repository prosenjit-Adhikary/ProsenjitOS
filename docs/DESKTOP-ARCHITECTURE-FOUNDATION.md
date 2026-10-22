# ProsenjitOS Desktop Architecture Foundation — Part 1

**Status:** source-audited architectural baseline; implementation and runtime acceptance are **not yet passed**.  
**Scope:** establish a stable component map and contracts for one ProsenjitOS desktop hosting native and supported Windows applications. This is not a claim that a desktop session or Windows compatibility runtime currently works.

## 1. Architecture invariants

1. There is one ProsenjitOS desktop shell, launcher, taskbar/window-management experience. Applications retain distinct execution backends.
2. Native applications execute through the Native Subsystem. Supported Windows applications execute through a separate Windows Subsystem. A shared desktop protocol integrates their windows and events; it does not merge their ABIs or trust domains.
3. The Desktop Management Layer (DML), window manager/compositor, desktop shell, application runtimes, compatibility libraries, and user applications remain user-space components. Kernel code is limited to generic primitives: process/address-space isolation, syscalls/IPC, capability enforcement, VFS, driver/device interfaces, and minimal display/input hardware access.
4. User-space services access kernel resources only through documented syscalls, capability handles, and validated IPC/device contracts. They must not link private kernel headers or pass raw user pointers/mount/device objects into kernel internals.
5. Format detection is not signature verification, compatibility validation, or authorization to execute. PE detection does not imply a Windows runtime; OLE detection does not prove MSI.
6. A component is not “implemented” because a design document, ABI header, mock backend, or unit test exists. Runtime claims require reproducible runtime evidence.

## 2. Target architecture

```text
                         ProsenjitOS Desktop
             Shell / launcher / taskbar / window manager
                                  |
                Shared Desktop Integration Layer (user space)
         Window protocol | display/session | keyboard/mouse | clipboard | audio
                    /                              \
                   /                                \
       Native Application                    Windows Application
       ELF / native packages                 PE .exe / .msi packages
                 |                                     |
          Native Subsystem                      Windows Subsystem
       ELF loader + native libs             PE loader + DLLs + Win APIs
       native ABI / SDK                      supported Win32/NT user APIs
                   \                                /
                    \                              /
                  Common brokered user-space services
                       VFS / IPC / package service
                                  |
                       Stable kernel-user ABI
                                  |
          ProsenjitOS Native Graphics & Hardware Stack
        device broker / drivers / display backend / input / audio
                                  |
                         ProsenjitOS kernel
        process & address spaces / syscalls / capabilities / VFS
                                  |
                              Hardware
```

The graphics/hardware stack is the platform beneath the desktop, not a reason to put the compositor in kernel space. The drawing is a target dependency graph, not an assertion that every box is available today.

## 3. Source-backed component inventory

Status meanings: **Implemented** = concrete code exists for the stated scope; **Partial** = useful subset or contract exists but the component's stated role is incomplete; **Missing** = no implementation was found in the inspected paths; **Unverified** = source exists but the required execution evidence has not been established. Statuses apply to the full component as named, not to individual low-level primitives.

| Required component | Status | Source evidence and precise limit |
|---|---|---|
| ProsenjitOS Desktop | **Partial / unverified** | `user/desktop/desktop_manager.{h,c}` provides a bounded DML manifest registry, feature/dependency preflight, and selection. It does not start a session or provide a shell, launcher, taskbar, window manager, compositor, or desktop service supervisor. A booted desktop session is not verified. |
| Windows Application (.exe, .msi, Windows APIs) | **Partial** | `user/apps/app_format.{h,c}` classifies bounded PE32/PE32+ headers and OLE container signatures. OLE is not MSI validation. There is no end-to-end installation, launch, or WinAPI compatibility demonstrated. |
| Windows Subsystem (runtime, DLL loading, supported Windows services) | **Missing** | No integrated PE process loader, import/DLL loader, Win32/NT user-mode API implementation, or Windows service runtime was found in the inspected app/runtime paths. The subsystem must be scoped to tested APIs and programs; universal compatibility is not promised. |
| Native Application (ELF, native packages) | **Partial** | Kernel ELF validation/loading primitives and `pros_init_load()` support a constrained `/init` path; `user/apps/` has manifest/format metadata. This is not a general application package installer or public spawn/exec workflow. |
| Native Subsystem (native loader and libraries) | **Partial** | Kernel ELF loader/process/address-space pieces exist. `docs/NATIVE-SPAWN-CONTRACT.md` documents gaps: no general public spawn/exec syscall and no verified complete userspace init/service-launch path. A stable native SDK/library set and normal user process launch remain incomplete. |
| Shared Desktop Integration Layer | **Partial** | `include/abi/desktop.h` defines a versioned, size-tagged message contract. The header now provides explicit received-length validators for window, key/pointer input, clipboard, and audio requests, alongside rectangle and size bounds. ABI v1 rejects non-zero `window_flags` because no window flags are defined yet; keyboard events require exactly one press/release edge, repeat is only valid with key press, and pointer repeat is rejected. `tests/desktop_abi_test.c` includes focused negative/positive cases for these rules. `include/abi/display.h`, `include/abi/input.h`, and `user/desktop/` provide additional contracts/registry. These tests have not yet been rerun against the current PR head because runner execution is unavailable. There is no verified running compositor/window server, clipboard/audio service, or end-to-end input/display event route; ABI headers are not syscalls or service implementations. |
| ProsenjitOS Native Graphics & Hardware Stack | **Partial / unverified** | `kernel/display.c` contains a driver-backend binding/present path and a synthetic self-test; `kernel/include/driver_class.h` defines driver-class contracts. This does not prove a real GPU/display driver, user-space graphics API, compositor, concrete keyboard/mouse event delivery, or audio playback. Real hardware operation is unverified. |

### Repository and workflow findings

- `README.md`, `docs/ARCHITECTURE.md`, `docs/PROJECT-STRUCTURE.md`, `docs/DESKTOP-IMPLEMENTATION-PLAN.md`, `docs/V2-UNIFIED-APPLICATION-RUNTIME.md`, `docs/NATIVE-SPAWN-CONTRACT.md`, and `user/README.md` describe the target and record important implementation gaps. This document consolidates their desktop-specific status.
- The inspected public syscall enum in `include/abi/syscall.h` contains version/test-completion, handle close/duplicate, IPC send/receive, and handle transfer operations. It does not expose general spawn/exec, display, input, or filesystem operations. Do not invent a syscall or bypass this boundary to wire a service.
- `kernel/include/vfs.h` is a private kernel interface; user-space code must not link it directly.
- PR #159 (`feature/shared-desktop-abi-foundation`) contains the shared desktop ABI and host-side contract tests, but its test/runtime acceptance was not established during this audit. PR #156 documents desktop/process prerequisites; PR #164's app registry is metadata/control-plane only. These open PRs are not equivalent to merged or runtime-verified features.
- Build and QEMU workflows are configured for the Linux x64 self-hosted runner. OfficeWindows is permanently retired and must never be selected.

## 4. Ownership and stable interface rules

### 4.1 Ownership

- **Kernel core:** process/address spaces, scheduling, user-copy validation, capability/handle tables, syscall policy, IPC primitives, VFS, driver broker, minimal display/device operations. No shell, compositor, WinAPI, DLL loader, or desktop policy.
- **Native Subsystem (user space):** native ELF launch policy, native ABI/libraries, native package metadata, and native process startup once a safe general spawn contract exists.
- **Windows Subsystem (user space):** bounded PE parsing/loading, DLL/import resolution, per-process Windows API compatibility, Windows error semantics, and only the Windows services explicitly implemented for supported applications. It receives no raw hardware privileges.
- **Shared Desktop Integration Layer (user space):** window ownership, composition, focus/input routing, clipboard policy, audio client/session routing, and application-independent desktop presentation. It is not a privileged execution backend.
- **DML / package manager (user space):** package/desktop metadata, dependency preflight, lifecycle orchestration, diagnostics, and requests to trusted installation/launch services. A manifest or package cannot self-attest trust.
- **Graphics/hardware services:** kernel/device broker owns hardware access; user-space graphics/compositor consumes authorized buffers and device capabilities. No arbitrary application maps or writes the physical framebuffer directly.

### 4.2 ABI compatibility

- Shared ABI headers must use fixed-width integer types, explicit version and size fields, bounded counts/lengths, documented ownership, and reserved fields that must be zero for version 1.
- Preserve published syscall numbers and ABI field meanings. Incompatible changes require a new version or explicit size/version negotiation; unknown flags/types must fail closed.
- Treat IPC payloads as untrusted at both endpoints. Validate version, exact/minimum structure size, payload bounds, enum values, arithmetic overflow, handles, and caller authorization before state mutation.
- For `include/abi/desktop.h` messages, every receiver must first prove the transport contains the complete fixed header, then validate the declared `struct_size + payload_size` against the actual received byte count (for example, with `pros_desktop_message_fits()`); only then may it read message-specific fields or payload bytes. The current header helper is a contract, not proof that a production receiver already enforces it.
- The shared desktop ABI is an IPC message contract, not a direct kernel syscall. Add kernel interfaces only when a missing primitive is proven necessary and has a narrowly scoped security review.

### 4.3 Lifecycle contract

Target lifecycle (not currently end-to-end implemented):

1. **Discover:** inspect package metadata with bounded parsing; never execute during detection.
2. **Verify/admit:** trusted verifier checks provenance/signature/hash and policy; only a privileged trusted component may change trust state.
3. **Preflight:** validate architecture/ABI/features/dependencies and backend selection before changing persistent state.
4. **Stage:** install into isolated temporary storage with bounded quotas and no visible partial package.
5. **Publish/commit:** use a documented atomic publication and durability contract, persist registry/journal state, and reconcile after interruption. A per-file rename is not whole-package atomicity.
6. **Launch:** the selected backend validates/loads its own executable format, creates an isolated process, and receives only approved capabilities. Native ELF and Windows PE must not be loaded by the wrong backend.
7. **Integrate:** register a window through the shared protocol; route input only to authorized focused windows; clipboard/audio operations use explicit policy and limits.
8. **Exit/failure:** revoke session handles, stop/reap process resources, release window/audio/input state, persist diagnostics, and restart only through the service manager's bounded policy. A failed user-space component must not panic the kernel.

### 4.4 Error handling

- Use stable, documented error classes for invalid input/version, unsupported feature, denied capability, missing object, resource exhaustion, timeout, I/O failure, and corrupt state. Preserve detailed internal diagnostics without exposing kernel pointers or sensitive data to untrusted apps.
- Reject unknown ABI versions, oversized payloads, integer overflow, malformed paths, invalid handles, untrusted packages, missing dependencies, and backend/format mismatches before mutating state.
- Make operations either transactional or explicitly document partial effects. Rollback failure is a separate fatal-to-operation result and must be logged; never report success if cleanup/rollback failed.
- On service disconnect, invalidate its session/handles and fail pending requests closed. Do not silently redirect a Windows app into the native ABI or vice versa.

## 5. Security boundaries

1. Applications are mutually untrusted, including applications from the same backend.
2. Windows user-mode compatibility never permits loading Windows kernel drivers (`.sys`) into ProsenjitOS kernel-core.
3. File/package format and signature status are separate from launch authorization. A valid signature does not bypass sandbox or capability policy.
4. Display buffers, clipboard data, audio payloads, event counts, and IPC message sizes must be bounded. Avoid pointer-bearing persistent manifests and untrusted pointers crossing the kernel boundary.
5. The compositor is not a security authority for file/device/process access. The kernel policy/capability layer remains authoritative.
6. The Windows and native backends share presentation, not loader state, API semantics, or privileges.
7. All desktop services remain restartable user-space processes once process lifecycle support is ready; do not add desktop service code to kernel-core to work around missing process/IPC features.

## 6. Part 1 implementation scope and acceptance gate

Part 1 establishes the architecture contract, maps source to status, and verifies the contract/test foundation. It does **not** start implementation of the Windows loader, DLL runtime, compositor, or full desktop session.

Required evidence before Part 1 can be accepted:
- [ ] Architecture status table and ownership/security/lifecycle/error contracts reviewed against current source.
- [ ] Host-side DML registry and shared desktop ABI tests compile with `-std=c11 -Wall -Wextra -Werror -pedantic` and pass.
- [ ] Shell scripts pass `bash -n`.
- [ ] Linux x64 CI builds the kernel/ISO and existing boot/QEMU gates pass on the exact PR head.
- [ ] CI logs and head SHA are checked; cancelled/queued jobs do not count as passes.
- [ ] No desktop/compositor/compatibility code has moved into kernel-core.
- [ ] All seven architecture components retain honest status labels; no runtime/hardware success is claimed without evidence.

**Part 1 is not accepted yet. Do not start Part 2 until every applicable acceptance gate above passes.** A green host unit test alone does not prove desktop runtime, Windows compatibility, or real hardware operation.
