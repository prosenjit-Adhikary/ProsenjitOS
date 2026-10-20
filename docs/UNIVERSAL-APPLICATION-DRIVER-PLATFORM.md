# Universal Application & Driver Platform

**Status:** Proposed architecture and phased implementation plan  
**Scope:** Design only. This document does not claim that cross-OS applications or drivers currently install or run.  
**Primary OS:** ProsenjitOS (native ABI, kernel, security policy, desktop, and hardware ownership)

## 1. Goal

Provide one trusted management experience for discovering, installing, launching, updating, repairing, and removing supported applications and driver packages from three initial ecosystems:

1. Native ProsenjitOS packages, applications, and drivers.
2. Linux applications and packages through an independently implemented Linux user-space compatibility backend and package-source adapters.
3. Windows applications and driver packages through a separate Windows user-space compatibility backend and Windows-oriented package-source adapters.

The user should see one application/driver center and the existing ProsenjitOS desktop. A common interface does **not** mean a common executable format, common ABI, or universal driver binary. Every backend must declare exactly what it supports and prove it with tests.

The architecture should be extensible to future FreeBSD, macOS, and other backends without changing the central manager's core contract. Those backends are future possibilities, not current compatibility promises.

## 2. Non-goals and hard constraints

- ProsenjitOS remains the host OS and owns the primary desktop, kernel, hardware policy, and security boundary.
- Do not require a full Windows installation or a Windows VM as the normal application execution path.
- Do not make Wine a required runtime dependency.
- Do not create a separate Windows desktop/shell for users.
- Do not run Windows `.sys` drivers directly as native ProsenjitOS kernel drivers.
- Do not treat a Windows, Linux, FreeBSD, or macOS driver as compatible merely because it targets the same device or has a familiar file extension.
- Do not promise universal application or driver compatibility. Publish a tested compatibility matrix by app/version/architecture/API/device.
- Do not execute untrusted installers with elevated rights by default.
- Do not let package scripts, applications, compatibility runtimes, or AI-generated plans grant themselves privileges.
- Do not copy proprietary OS implementation code. Review third-party component licenses before reuse.
- Keep desktop/window-system components in user space, not in the kernel.
- Preserve existing native boot, process, ELF, VFS/ProsenjitFS, security, and driver paths; compatibility work must not weaken or silently change them.

Running Windows binaries without a Windows kernel still requires a substantial, independently implemented or properly licensed PE/ABI/API/runtime implementation. Avoiding Wine or a full Windows VM does not remove that work.

## 3. Architecture overview

```text
 ProsenjitOS Application & Driver Center
  search | install | launch | update | repair | remove | status
                           |
                 Universal Manager Service
  identity | policy | metadata | source routing | transactions | audit
                  /             |             \
                 /              |              \
       Native backend     Linux backend     Windows backend
       native ABI         ELF/Linux ABI      PE/Win32 ABI
       native repo        distro/package     Windows-oriented
       native drivers     adapter(s)         app/runtime adapter
                 \              |              /
                  \             |             /
                 Shared ProsenjitOS user-space services
         VFS | IPC | graphics | audio | network | notifications
                           |
            ProsenjitOS security/capability broker
                           |
        Kernel process, memory, I/O, device and driver interfaces
                           |
                    Physical hardware
```

The central manager coordinates policy and lifecycle. It must not implement every foreign OS API itself or turn into a monolithic kernel feature. Each backend is a versioned, replaceable user-space component with a declared capability set.

## 4. Core components and contracts

### 4.1 Universal Manager Service

Responsibilities:

- Identify package type using bounded content inspection and trusted manifest metadata; never trust filename extension alone.
- Resolve an explicit backend and source based on package type, architecture, ABI, user policy, trust state, and tested compatibility.
- Present a truthful preflight: supported, partially supported, unsupported, untrusted, requires reboot, or requires privileged approval.
- Coordinate dependency resolution, staged installation, update, uninstall, repair, and rollback.
- Keep per-application identity, capabilities, sandbox profile, resource limits, and compatibility record.
- Record security-relevant actions and failures without logging secrets.
- Expose a stable versioned interface so backends can be added or replaced independently.

Suggested logical modules: `app_registry`, `package_inspector`, `backend_registry`, `compatibility_db`, `source_router`, `dependency_resolver`, `install_transaction`, `permission_broker`, `process_launcher`, `driver_manager`, `audit_log`, and `recovery_manager`. These are design boundaries, not a requirement to create one source file per name.

### 4.2 Backend interface

Each backend should declare and implement only the capabilities it can actually provide:

- identify/inspect package and executable metadata
- report supported architectures, ABIs, package formats, API families, and known limits
- query configured sources and verify provenance/integrity
- resolve dependencies without silently mixing incompatible OS packages
- stage, install, update, repair, and uninstall with transaction/recovery metadata
- preflight launch requirements and launch only under a declared sandbox/policy
- report process state, logs, errors, and clean shutdown
- expose explicit driver-package support separately from application support

All external input is untrusted. Interfaces need size limits, version negotiation, deterministic error codes, timeouts where applicable, and tests for malformed metadata. A backend must fail closed when its required trust or compatibility checks cannot be completed.

### 4.3 Package-source adapters

Source adapters fetch and verify metadata/packages; they do not make incompatible binaries executable.

- **Native ProsenjitOS:** the official native repository and package format are authoritative for native applications/drivers.
- **Linux:** use selected distribution package sources and tools through a Linux-specific adapter. Respect distribution metadata, dependency rules, signatures, and package ownership. Do not install arbitrary Linux kernel modules into the ProsenjitOS kernel.
- **Windows applications:** use legitimate vendor/publisher sources and Windows-oriented package metadata where available. Windows Package Manager/WinGet is not a universal driver manager and is not assumed to support every installer or driver package.
- **Windows driver packages:** support source discovery/import only after source policy, provenance, signature, redistribution, and licensing rules are specified. A downloaded Windows driver package remains a Windows driver package, not a native ProsenjitOS driver.
- **Future FreeBSD/macOS:** add adapters only after their package formats, source policies, ABI requirements, licensing, and security model are documented and tested.

Avoid a universal dependency namespace. Preserve each backend's package identity, version constraints, architecture, ABI, and provenance. Dependencies from different OS ecosystems must not be merged just because package names match.

### 4.4 Application runtime backends

**Native backend**
- Use the ProsenjitOS executable format, native ABI, native SDK, and native process services.
- Keep native execution as the simplest, best-integrated, best-supported path.
- Test launch/exit, permission enforcement, file associations, update/uninstall, and regression against existing native ELF and kernel paths.

**Linux backend**
- Start with a tightly scoped Linux ELF/POSIX user-space compatibility target and a small, self-authored test corpus.
- Explicitly implement and version required syscall, library, filesystem, process, signal, threading, and error semantics; do not assume ELF format alone means Linux compatibility.
- Use an isolated user-space environment and controlled filesystem mapping. Any optional container-like packaging must still use the ProsenjitOS kernel/security boundary.
- Add GUI integration only after the required process, API, security, and shared desktop interfaces are stable.

**Windows backend**
- Use a modular user-space runtime, including bounded PE32/PE32+ inspection/loading, import/DLL handling, selected Win32/NT user-mode API contracts, dependency handling, and explicit compatibility reporting.
- Treat the existing kernel PE inspector as metadata inspection only; it is not an executable loader.
- Begin with a minimal self-authored console test program, then add a documented API subset, and only later integrate selected GUI applications with the existing desktop/window-system interfaces.
- Treat MSI, MSIX, Click-to-Run, Microsoft 365 Apps, DRM, anti-cheat, Store/UWP, and driver-dependent software as separate feasibility/test gates. Do not claim that an installer running means the installed application works.
- Do not use Wine as a required runtime dependency and do not boot a full Windows VM as the normal execution path.

### 4.5 Shared desktop integration

The manager should register supported applications with one launcher, taskbar/session, file-association service, notification service, and existing window-system interfaces. Foreign API semantics stay in the relevant runtime; the shared desktop bridge translates only explicitly supported window, input, clipboard, audio, and presentation behavior.

Do not place Xorg, Wayland, XFCE, application launchers, or compatibility runtimes inside the kernel. Use existing/mature user-space display components rather than inventing a new window server as a prerequisite.

## 5. Driver management is a separate lifecycle

Application compatibility and driver compatibility are separate capabilities. A backend may support application installation while reporting zero foreign-driver execution support.

### 5.1 Native device and driver path

1. Enumerate devices through implemented bus/platform interfaces (initially PCI; extend to USB/ACPI and other buses when available).
2. Capture stable hardware identifiers, class/subclass, vendor/device IDs, revision, and required resources.
3. Match against a signed/approved driver manifest and explicit compatibility database.
4. Verify driver architecture, target kernel-driver API version, dependencies, signature/trust policy, and device ownership.
5. Stage the package transactionally and validate dependencies before activation.
6. Load the driver only through the controlled native driver loader; initialize and bind it to the appropriate device/subsystem.
7. Test required device operations and health checks; publish status to the manager.
8. If activation or health checks fail, detach safely where possible, restore the previous known-good driver/configuration, and record diagnostics.
9. Support update, unload, removal, and reboot-required states explicitly.

The native kernel needs a documented driver API, device model, bus enumeration contract, resource/interrupt/DMA rules, lifecycle, and error semantics before a broad native driver catalog can be reliable.

### 5.2 Driver classes and isolation

Prefer user-mode drivers/services for device classes that can safely use them. Kernel-mode drivers are highly privileged and part of the trusted computing base. For kernel drivers, require strong package provenance, signature verification, least privilege, bounded interfaces, dependency/version metadata, audit records, and a recovery path. Use IOMMU/DMA isolation where platform support exists; do not claim it on platforms without verified support.

Never load a driver just because its package signature is valid: authenticity does not prove compatibility or safety. A signed but wrong or vulnerable driver must still be rejected by compatibility and policy checks.

### 5.3 Windows driver support

Windows user-mode apps and Windows kernel drivers are different engineering problems. The first platform milestone should support **native ProsenjitOS drivers**, not Windows kernel drivers.

Any future Windows driver compatibility effort must be a separate research track with a threat model, a specific supported driver framework/class, ABI/framework implementation plan, device ownership, interrupt/DMA model, signing/trust rules, crash containment, rollback/recovery, and real hardware tests. Windows `.sys` files must never be loaded directly as native ProsenjitOS kernel drivers. A Windows driver package may be inspected or catalogued without being executable.

### 5.4 Linux and future foreign drivers

A Linux `.ko` module targets a Linux kernel ABI and must not be loaded directly into the ProsenjitOS kernel. A FreeBSD or macOS driver likewise targets a different kernel/device framework. Prefer a native port or an explicitly isolated compatibility implementation for a narrow, justified driver class; never infer compatibility from the hardware model alone.

## 6. Security, trust, and recovery requirements

- Verify package hashes and signatures using supported algorithms and trusted key policy; handle revocation and key rotation.
- Authenticate source metadata and prevent downgrade/replay where supported.
- Keep installation staged and recoverable after power loss, cancellation, disk-full, or dependency failure.
- Use least-privilege installer helpers and explicit authorization for privileged changes.
- Never run arbitrary package scripts as root/superuser by default.
- Isolate package inspection, PE/ELF parsing, dependency resolution, and foreign API emulation from privileged kernel code where feasible.
- Enforce per-app filesystem, device, IPC, process, and network capabilities through the OS security broker.
- For drivers, require a compatibility match in addition to signature/provenance checks; deny by default if the target API/version is unknown.
- Keep known-good driver/package state and provide recovery/rollback, including a safe boot/recovery path for driver failures.
- Make update/uninstall idempotent; preserve user data unless the user explicitly approves deletion.
- Log source, version, signer, hashes, permissions, decisions, result, and rollback state while excluding credentials and secrets.

## 7. Compatibility database

Each record should include at least:

- ecosystem/backend and package source
- application/driver name, publisher, version, architecture, and package hash
- executable/package format and required ABI/API/runtime versions
- for drivers: hardware IDs, device class, target driver API/framework, kernel interface version, required firmware, and supported device revisions
- signature/provenance status and policy decision
- supported capabilities, known limitations, dependencies, and security notes
- exact test profile, test date, commit/build ID, test logs/results, and regression status
- install/update/uninstall/rollback status and whether reboot is required

Use clear states such as `supported`, `partial`, `unsupported`, `blocked`, and `not-tested`. Never convert `not-tested` into `supported` by inference.

## 8. Implementation sequence and gates

Do not start broad compatibility implementation until the native kernel/process/syscall/VFS/ProsenjitFS/security and user-space foundations are stable enough to enforce the boundaries.

### Phase A — Inventory and contracts
- Audit existing native PCI/device enumeration, driver abstractions, package-related code, PE inspection, process/ELF startup, security broker, and desktop interfaces.
- Reconcile existing documents and branches before creating duplicate managers or APIs.
- Define versioned backend, package manifest, driver manifest, compatibility record, transaction, and error contracts.
- Produce threat models and ownership diagrams. No broad implementation in this phase.

**Gate:** reviewed contracts, scope, non-goals, and a list of existing code reused versus missing.

### Phase B — Native manager and native drivers
- Build the minimum native package metadata/source/verification path and a common manager service.
- Define driver manifest, native driver API/versioning, controlled loader lifecycle, hardware binding, and recovery behavior.
- Add device discovery/matching and one or more narrow, testable native driver classes as prerequisites allow.
- Ensure compatibility adapters cannot bypass native permission/security policy.

**Gate:** clean build, focused unit tests, native device/driver lifecycle tests, failure rollback, and existing boot/VFS/ProsenjitFS/security regressions pass.

### Phase C — Native application management
- Add registry, install transactions, launch integration, update/uninstall, dependency metadata, file associations, and permission prompts.
- Test interrupted install/update, disk-full, corrupted package, signature failure, dependency conflict, rollback, and repeated operations.

**Gate:** no partial install leaves an inconsistent registry or privileged state; launch/permission and rollback behavior are deterministic.

### Phase D — Linux user-space compatibility
- Specify the initial Linux ABI/API subset and a self-authored test suite.
- Add the Linux package-source adapter separately from runtime execution.
- Implement bounded ELF inspection/loading only when the native process/memory interfaces support it; add only needed Linux syscall/library semantics.
- Add sandbox and filesystem mapping tests before broad packages.

**Gate:** selected Linux test programs pass the declared API matrix, unsupported APIs fail clearly, and native apps/filesystem/security remain regression-clean.

### Phase E — Windows user-space compatibility
- Reuse the metadata-only PE inspector where appropriate; do not confuse it with the loader.
- Implement and fuzz bounded PE parsing/mapping and imports in user space, with strict architecture, range, overflow, section-permission, and cleanup checks.
- Start with a self-authored console test executable and a tiny documented API subset.
- Expand DLL/API support only for selected test applications; then add installer/dependency management and desktop integration.

**Gate:** malformed PE inputs are rejected; selected tests pass launch/exit, isolation, API behavior, dependency, cleanup, and native regression gates. Office or GUI support remains unclaimed until its own gates pass.

### Phase F — Driver catalog and optional foreign-driver research
- Improve source cataloguing/import for foreign driver packages without activating them.
- Add driver matching, signature/provenance, compatibility status, and clear unsupported states.
- Research Windows/Linux driver compatibility only for a specific justified device class after native driver contracts and recovery are proven.
- Require a separate design review before any foreign driver can be activated.

**Gate:** no foreign kernel driver is loadable by default; policy-denial, wrong-device, wrong-ABI, rollback, and recovery tests pass.

### Phase G — Future backends
Consider FreeBSD, macOS, or other OS backends only after the backend contract is stable and the first supported backends have real tests. Each future backend needs its own ABI, package-source, license, security, and compatibility plan. Do not promise macOS app support merely because a Mach-O parser exists.

## 9. Verification and release policy

Every milestone must include:

- exact source commit and reproducible commands
- clean build and focused unit/host tests where applicable
- malformed-input and negative tests, fuzz/property tests for parsers, and resource-exhaustion cases
- install/update/uninstall/interruption/rollback tests
- permission-denial, sandbox escape-attempt, dependency-confusion, downgrade, and signature-failure tests
- driver wrong-device/wrong-ABI/signature-failure/initialization-failure/recovery tests
- regression checks for boot, process/thread/syscall, native ELF, VFS/ProsenjitFS, security, and existing drivers
- QEMU and hardware results where the feature requires them, with logs and limitations
- an updated compatibility matrix with no unsupported claims

A documentation change or successful compilation is not runtime verification. A package downloading successfully is not proof it installed safely; a driver loading is not proof the hardware is functional; one application starting is not proof of ecosystem compatibility.

## 10. Current status and relationship to existing documents

This is a proposed design, not an implementation-complete report. The existing [Version 2 Unified Application Runtime](V2-UNIFIED-APPLICATION-RUNTIME.md) remains the detailed Windows application-runtime design. [Windows App Installation](WINDOWS-APP-INSTALLATION.md) and the WinBridge loader contract define more specific Windows app-install/loader gates. [Security Model](SECURITY_MODEL.md) defines the host security boundaries. This document adds the cross-ecosystem manager, package-source routing, native/Linux/Windows backend contract, and separate driver lifecycle.

If an older high-level paragraph conflicts with these explicit constraints (for example, suggesting Wine as a runtime dependency or implying that foreign kernel drivers can load natively), prefer the current approved Version 2 and this platform specification; update the older paragraph during the next relevant documentation cleanup.

No source code, package manager, runtime backend, or driver loader is implemented by this document alone.

## 11. Contributor and AI-agent handoff contract

This section is normative for human contributors and coding agents (including future AI agents) so work can continue from repository evidence rather than assumptions.

### Before changing code or documentation

1. Read this document and the linked `docs/V2-UNIFIED-APPLICATION-RUNTIME.md`, `docs/WINDOWS-APP-INSTALLATION.md`, `docs/ARCHITECTURE.md`, and `docs/SECURITY_MODEL.md`. Read the relevant roadmap section and the current implementation before proposing changes.
2. Inspect the live default branch, current open PRs, their base/head branches, changed files, review comments, and CI/runner status. Do not assume a PR's description is current truth; compare it with its actual diff and current source.
3. Search the repository for existing managers, manifests, device APIs, driver loaders, package formats, security brokers, tests, and documentation before adding a parallel implementation. Extend the existing contract when safe; explain any intentional replacement.
4. State the exact task boundary, prerequisites, files expected to change, verification commands, and risks. Keep unrelated subsystems out of the patch.
5. If another PR or unfinished branch already owns the same area, coordinate with it or build on its reviewed base. Avoid duplicate documents, duplicate APIs, accidental stacked-PR base mistakes, and editing `main` directly when a review branch is appropriate.

### Implementation rules

- Treat this specification as architecture guidance, not proof that a feature already exists. Verify every claimed capability in source and tests.
- Work in small, reviewable changes. Preserve user data and existing working native paths; do not perform broad unrelated refactors.
- Never weaken a security boundary to make a compatibility test pass. Unknown formats, unsupported ABIs, untrusted packages, and incompatible drivers must fail closed with actionable diagnostics.
- Never load foreign OS kernel-driver binaries directly into the ProsenjitOS kernel. Driver-source discovery/import is not driver activation.
- Do not report a test as passed unless it was actually executed and its result inspected. Distinguish source review, host tests, clean build, CI, QEMU, and physical-hardware verification.
- Where repository policy or the user's current instructions specify a self-hosted runner, use the designated runner and avoid spending GitHub-hosted Actions minutes unnecessarily. Do not claim a runner was used unless its actual job result/log confirms it.
- Keep the patch within the requested scope. For this platform, kernel/ProsenjitFS work remains separate from desktop/compatibility work unless an explicit shared contract requires a change.

### Required completion report

For each task, report:

1. **Status:** complete, partial, blocked, or not started.
2. **Repository evidence:** branch, base branch, PR/commit URL and exact changed paths.
3. **What changed:** implementation/documentation boundaries and any deliberately unsupported features.
4. **Verification:** exact commands or CI jobs actually run, their outcomes, and links/log references where available.
5. **Unverified work:** what could not be run (for example QEMU, offline Debian, or physical hardware) and why.
6. **Remaining blockers:** ordered by dependency, with the next concrete action.
7. **Regression assessment:** confirm relevant native process, VFS/ProsenjitFS, security, boot, and driver paths were considered; cite tests rather than making blanket claims.

Do not say “complete” for the universal platform until the declared backend contracts, security gates, install/update/rollback behavior, compatibility matrix, and required regression tests are implemented and verified. Documentation-only work must be labelled documentation-only.
