# Foundation Audit & Application Runtime Architecture

Audit date: 2026-10-29  
Audited ref: `main`  
Starting commit: `fc8604aca130561182e0e1b50077654569ba5e80`  
Repository: [prosenjit-Adhikary/ProsenjitOS](https://github.com/prosenjit-Adhikary/ProsenjitOS)  
Evidence boundary: GitHub source and Actions APIs. No local checkout or local build was available. "Verified" below means supported by checked-in implementation and/or a reported test marker; it does not imply physical-hardware or production readiness.

## Executive finding

The repository has substantial kernel, native ELF, process, memory, VFS/filesystem, IPC, security, and device foundations. Native process execution has a boot-time CPL3/init path with extensive QEMU markers, but there is no general public spawn/exec syscall or complete native application workflow. The desktop manager is an in-memory registry/preflight module, not an installer or launcher.

Windows application execution is **not implemented**. The installation specification says so. The checked-in `kernel/pe_inspect.c` and `kernel/nt_compat.c` are kernel-side inspection/NT-driver ABI scaffolding, not a user-space PE image loader or Win32 runtime. This is a direct mismatch with the intended user-space WinBridge boundary and must be resolved before implementation expands. Open PRs #123 and #124 are proposals/work in progress, not main-branch implementation evidence.

## Status labels and evidence

| Subsystem | Status | Evidence on main and gap |
|---|---|---|
| Boot and x86-64 entry | IMPLEMENTED_NOT_FULLY_VERIFIED | `boot/`, `kernel/kernel.c`, `.github/workflows/build.yml`; build workflow defines ISO and native BIOS QEMU marker checks. No local runner was available; UEFI/physical hardware not verified. |
| Physical memory, paging, address spaces, heap | IMPLEMENTED_NOT_FULLY_VERIFIED | `kernel/memory.c`, `kernel/paging.c`, `kernel/address_space.c`, `kernel/heap.c`; boot workflow requires allocator, paging isolation, and heap self-test markers. Exact current-main workflow success was not established in this audit. SMP/TLB shootdown and hardware behavior remain outside those markers. |
| Process/thread lifecycle and scheduler | IMPLEMENTED_NOT_FULLY_VERIFIED | `kernel/process.c`, `kernel/thread.c`, headers and `docs/PROCESS-LIFECYCLE.md`; workflow asserts process/thread lifecycle, teardown, scheduler-frame markers. `docs/NATIVE-SPAWN-CONTRACT.md` explicitly says process creation does not load ELF/create a runnable thread and no spawn/exec syscall exists. Caller-owned thread-stack lifecycle remains a risk documented in open PR #183. |
| Native ELF validation/loading | IMPLEMENTED_NOT_FULLY_VERIFIED | `kernel/elf_loader.c`, `kernel/include/elf_loader.h`, `kernel/init.c`; bounded PT_LOAD validation/mapping and startup load of `/init`; workflow checks ELF validation and CPL3 markers. This is not a general application launch contract. Dynamic/interpreter/TLS support is rejected. |
| Syscalls and user-copy | IMPLEMENTED_NOT_FULLY_VERIFIED | `kernel/syscall.c`, `include/abi/syscall.h`, `kernel/usercopy.c/.asm`; capability-mediated IPC and syscall policy markers exist. ABI lacks spawn/exec; user-copy fixup is documented as single-CPU. |
| VFS, native ProsenjitFS, permissions | IMPLEMENTED_NOT_FULLY_VERIFIED | `kernel/vfs.c`, `kernel/prosenjitfs.c`, `kernel/vfs_provider.c`, `docs/FILESYSTEM.md`; workflow requires mount/journal/recovery and permission markers. Canonical filesystem doc explicitly tracks incomplete release gates; alternate provider registry is scaffolding and VFS currently recognizes ProsenjitFS only. Do not generalize historical test claims to the audited head. |
| IPC | IMPLEMENTED_NOT_FULLY_VERIFIED | `kernel/ipc.c`, `docs/IPC.md`; bounded capability-based channels and blocking/wakeup paths with QEMU marker. Docs call it single-CPU, note locking/scheduler ordering concerns and no SMP proof. |
| Security/capabilities/admission | IMPLEMENTED_NOT_FULLY_VERIFIED | `kernel/object.c`, `kernel/intent.c`, `kernel/security_*.c`, `docs/SECURITY_MODEL.md`; policy and security runtime markers are required by workflow. Full compatibility runtime isolation and package admission flow are not implemented. |
| Native app registry/runtime | PARTIALLY_IMPLEMENTED | `user/desktop/desktop_manager.[ch]`, `tests/desktop_manager_test.c`; fixed-size manifest registry, dependency preflight, selection and host unit test target. Header explicitly says metadata entry point is not opened/executed and this is not a process launcher. |
| Graphics/desktop | PARTIALLY_IMPLEMENTED | Kernel display/device interfaces and desktop/security service stubs exist; `docs/V2-UNIFIED-APPLICATION-RUNTIME.md` places desktop/runtime pieces in userspace. No verified full native desktop or installer workflow in the checked-in tree. |
| Network | IMPLEMENTED_NOT_FULLY_VERIFIED | `kernel/network.c`; workflow requires network protocol/socket marker. Hardware coverage and broad application networking API not demonstrated by that marker. |
| PE inspection | PARTIALLY_IMPLEMENTED | `kernel/pe_inspect.c`, `kernel/include/pe_inspect.h`; bounded PE metadata inspection is present and exposed to kernel-side compatibility code. It is not a mapped/runnable PE loader. |
| WinBridge / Win32 compatibility | NOT_IMPLEMENTED | No user-space WinBridge runtime or Windows API implementation exists in the main tree. `docs/WINDOWS-APP-INSTALLATION.md` labels itself a design specification and says implementation is not verified. PR #124 is open and does not change main. |
| NT driver compatibility | DOCUMENTATION_ONLY | The current `kernel/nt_compat.[ch]` offers a policy/dispatch/resource contract but explicitly does not load Windows PE drivers as native modules. Windows kernel drivers remain out of scope for app compatibility. |
| Installer, MSI, dependencies, rollback | DOCUMENTATION_ONLY | Windows install design describes detector, dependency broker, transaction manager, isolation and rollback. No installer/prefix/package transaction implementation is present in the tree. |
| Application sandbox / isolation policy | PARTIALLY_IMPLEMENTED | Kernel process policies, handles, security admission and filesystem authorization exist; per-app filesystem view, quotas, complete syscall/API policy and WinBridge containment are not established. |
| Build/test infrastructure | IMPLEMENTED_NOT_FULLY_VERIFIED | `Makefile`, scripts and Actions workflows define host tests, ISO build and QEMU boots. Current local commands were not run because private-repository checkout credentials are unavailable. Recent Actions snapshot included run #8990 in progress, #8989/#8988 cancelled (workflow concurrency); the latest main-commit result was not independently established. |

Statuses deliberately distinguish source presence, test hooks, and end-to-end readiness.

## Architecture and integration contracts

### Native execution (current path and target)

`native ELF file -> VFS/admission -> pros_elf_load() -> address_space -> pros_process/thread -> CPL3 -> syscalls/IPC -> kernel services`

The checked-in path loads `/init`; it is not a general spawn/exec contract. Use `docs/NATIVE-SPAWN-CONTRACT.md` as the starting proposed contract. Its required sequencing—bounded user input, VFS executable reference, admission, isolated address space, ELF load/rollback, process/thread publication, stable interrupt-frame/CPL3 entry, and cleanup—is consistent with current ownership APIs. A spawn syscall, thread-stack ownership contract and tested user entry handoff remain prerequisites. Do not duplicate or silently alter the existing syscall ABI.

### Windows execution (target; not implemented)

`installer/file manager -> bounded format detection -> install transaction/dependency broker -> user-space WinBridge PE loader -> user-space Windows API/runtime registration and resolution -> existing native process/address-space, VFS, IPC, graphics and network services`

WinBridge should remain in userspace. Kernel work should be limited to verified missing primitives behind stable syscalls, capabilities and VFS APIs. The existing kernel PE inspection and `nt_compat` driver ABI are separate inspection/driver scaffolds, not the application runtime. Do not treat the driver compatibility contract as a way to run Windows applications.

### Interfaces and missing contracts

- **Executable loading:** native interface is `pros_elf_load()` from `kernel/include/elf_loader.h`, used by init. A user-space WinBridge PE loader contract is absent on main. PR #123 proposes one; review and reconcile it with the actual native ABI before adoption.
- **Process create/terminate:** `process_create/exit/wait` in `kernel/include/process.h`; create publishes a process but does not construct a loaded runnable process. Public spawn/exec missing.
- **Address space/protection:** `pros_address_space` APIs map user pages under user permission/W^X checks. PE loader needs reserve/map/unmap/protect, per-section permissions, relocation support, image lifetime and rollback. Only expose required safe operations through syscall/capability gates.
- **Runtime lifecycle/API resolution:** no WinBridge init/shutdown or DLL/API registry exists in main. Proposed userspace runtime must define module lifetime, import resolution, versioned API sets, unsupported-call errors, logging, and process exit cleanup.
- **Filesystem/permissions:** route installation and file access through VFS handles and existing identity/authorization. Per-app roots and transaction journal are missing. Never bypass VFS using raw host/kernel paths.
- **Dependencies/isolation:** no trusted Windows runtime dependency resolver or complete per-app sandbox. Require trusted provenance/hash/version/architecture, explicit capability manifest, constrained search paths and quotas before launching.
- **Errors/logging:** reuse kernel error conventions only at syscall boundary; user-space runtime should return structured stage/API/module diagnostics and persist logs through authorized VFS. No compatibility log/telemetry ABI exists.
- **Build boundary:** kernel remains freestanding C/assembly. WinBridge is a separately built userspace component and must not link compatibility implementation into kernel core. Current user tree has no general runtime loader/build target, so that boundary must be added deliberately.

## Dependency-based roadmap

1. **Foundation audit & architecture** — this document plus this turn's log; reconcile with open #123/#124/#183 and current-main contracts.
2. **PE parsing and loader** — userspace bounded PE32/PE32+ parser; architecture/machine checks; sections, imports, relocations and protections; fuzz/property and malformed-image tests. May prototype parser independently, but cannot launch until process/memory prerequisites hold.
3. **Process execution and memory foundations** — complete native spawn/exec, thread stack allocation/ownership, safe CPL3 first entry, VM reserve/protect and rollback. Required before executable PE mapping or user runtime.
4. **WinBridge runtime and API compatibility** — runtime lifecycle, module/API registry, explicit supported API subset, syscall/native-service adapters and executable tests. Requires steps 2–3.
5. **Filesystem, installer transactions and dependency handling** — VFS-backed per-app profile, trusted dependency catalogue, journaling, interrupted recovery/rollback. Can design in parallel; execution/install depends on step 4 contracts.
6. **Application isolation/security policy** — per-app identity/capability sandbox, quotas, child process policy, security logging, adversarial tests. Design in parallel; must gate installation/execution before broader compatibility claims.
7. **Native GUI integration and installation workflow** — file-manager/Software Center flow after registry, process API, install transaction and permissions are stable.
8. **MSI support** — only after EXE loader/runtime, transactions, custom-action sandbox and recovery are tested.
9. **Microsoft Office feasibility investigation** — begin with official deployment requirements and API/COM/Click-to-Run dependency gap analysis after API registry exists; no compatibility assumption or installer execution before prerequisites.
10. **End-to-end integration, recovery, security and release verification** — exact-head CI, malformed-input/fuzz, permission and rollback tests, reproducible install/launch/uninstall and supported-app matrix.

## Checks and limitations

The private repository was assembled into a local source snapshot from the authenticated GitHub API at audited base `fc8604aca130561182e0e1b50077654569ba5e80`. The PR changes documentation only; executable source and test inputs are identical to that base.

- `make` **PASS** (exit 0): built `build/prosenjitOS.iso` and `build/prosenjit-native-bios.img`; formatted the disposable native ProsenjitFS image.
- `make desktop-unit-test security-service-test` **PASS** (exit 0): both desktop manager and security control-center dispatcher tests passed.
- Workflow script syntax checks: `bash -n scripts/desktop_complete.sh`, `bash -n scripts/test_desktop_manager.sh`, and `bash -n scripts/test_security_control_center_service.sh` **PASS**.
- ISO QEMU boot: `timeout 20s qemu-system-x86_64 -cdrom build/prosenjitOS.iso -display none -serial file:build/qemu-serial.log -no-reboot -no-shutdown -d int -D build/qemu-debug.log`; expected timeout exit 124. All **37/37** `require_log` markers in `.github/workflows/build.yml` were present.
- Native BIOS QEMU boot: `timeout 20s qemu-system-x86_64 -drive format=raw,file=build/prosenjit-native-bios.img,if=ide -display none -serial file:build/native-qemu-serial.log -no-reboot -no-shutdown -d int -D build/native-qemu-debug.log`; expected timeout exit 124. All **41/41** `require_native_log` markers were present.
- Security architecture workflow contract check **PASS**: all required markers in `docs/SECURITY-MALWARE.md` and `docs/ROADMAP.md` validated.
- Static analysis was not run; no static-analysis target is defined in the inspected Makefile/workflows.
- GitHub Actions exact-head hosted runs remain queued on self-hosted Linux x64 runners. Local build, both QEMU marker suites, host tests and security contract check were executed on the same executable source as the PR; hosted runner execution remains unavailable.
- Earlier Actions observations: PR #305 run #8990 was in progress, and #8989/#8988 were cancelled as newer commits superseded those runs under workflow concurrency. The cancellation is not a source/test failure diagnosis.

Open PRs relevant to scope: #305 (scheduler/VFS audit), #183 (kernel/boot/hardware audit), #123 (WinBridge user-space loader contract), #124 (user-space PE metadata parser), and #164 (native app registry). Their proposals remain distinct from implementation on `main`.

## Next task

Start Part 2 with a source-first **native process execution and VM integration gate**: reconcile `docs/NATIVE-SPAWN-CONTRACT.md` with `process_create()`, `pros_init_load()`, `thread_init()`, syscall policy, user address-space APIs, and thread stack ownership; specify and test a general safe CPL3 spawn path. In parallel, review open PR #124's user-space PE parser as parser-only work. Do not implement API compatibility or claim Windows execution until the native spawn and user-memory/protection contracts are verified.
