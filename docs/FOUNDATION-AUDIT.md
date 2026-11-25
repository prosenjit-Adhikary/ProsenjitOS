# Foundation Audit & Application Runtime Architecture

Audit date: 2026-10-29  
Audited ref: `main`  
Starting commit: `fc8604aca130561182e0e1b50077654569ba5e80`  
Repository: [prosenjit-Adhikary/ProsenjitOS](https://github.com/prosenjit-Adhikary/ProsenjitOS)  
Evidence boundary: GitHub source and Actions APIs. No local checkout or local build was available. "Verified" below means supported by checked-in implementation and/or a reported test marker; it does not imply physical-hardware or production readiness.


## 2026-11-14 — current-main revalidation for the Master Architecture Completion

**Audited ref:** `main`  
**Audited commit:** `98ed2f76c5a03fecba38b742423619bfe8238d97`  
**Scope:** native kernel/microkernel prerequisites, user-mode execution, Linux/Windows compatibility, shared guest abstractions, driver isolation, security and verification.

**Evidence boundary:** source and documentation were re-fetched from GitHub. The source audit inspected `19a86627a57b75b15f1611650e6da897132b6e42`; main then advanced to `98ed2f76c5a03fecba38b742423619bfe8238d97` with a one-commit delta limited to `README.md` and `docs/FILESYSTEM.md`, so the audited kernel source is unchanged at the latest main ref. This session did not have a local repository checkout and did not run a build, QEMU, host test, fuzz test or physical-hardware test. The available Actions lookup returned no PR-triggered runs and no combined status contexts for this main commit; that lookup does not expose push-run history. Therefore no current-head CI pass or failure is asserted below.

### Source-backed implementation matrix

| Subsystem | Existing implementation | Missing functionality / security risk | Tests or evidence inspected | Status |
|---|---|---|---|---|
| x86-64 boot, CPU entry and interrupts | BIOS/Multiboot boot paths, IDT/ISR assembly, timer and exception entry | Physical/UEFI coverage and current-head CI evidence are not established here | `boot/`, `kernel/kernel.c`, `kernel/idt.asm`, build workflow markers | IMPLEMENTED_NOT_FULLY_VERIFIED |
| PMM | Multiboot memory-map parsing, reserved-range exclusion, page bitmap allocator, explicit user-page classification, contiguous allocation/free | No physical-page reference counts or shared-page ownership model; no SMP allocator synchronization; managed physical range is capped at 1 GiB | `kernel/memory.c`, `kernel/include/memory.h`, `physical_memory_selftest()` and boot self-test call | IMPLEMENTED_NOT_FULLY_VERIFIED |
| VMM and address spaces | Per-process page-table roots, supervisor/user separation, W^X-oriented user mapping checks, owned-page list, unmap and teardown | No SMP TLB shootdown; shared mappings/reference ownership are not a complete facility; current design is single-CPU | `kernel/paging.c`, `kernel/address_space.c`, `kernel/include/address_space.h` | IMPLEMENTED_NOT_FULLY_VERIFIED |
| Process lifecycle | NEW/READY/RUNNING/BLOCKED/ZOMBIE/DEAD states, parent-child relation, handle table, exit/wait/reap and resource teardown | No general transactional spawn/exec; `process_create()` publishes READY before an executable and initial user thread exist. Also harden wait/reap preflight: `process_wait()` destroys the address space before `process_unregister()` calls the stronger `thread_reap_process()` stack/TSS preflight; a late reap failure can leave a ZOMBIE with already-released resources and no clean retry path | `kernel/process.c` (`process_create()`, `process_wait()`, `process_unregister()`), `kernel/thread.c` (`thread_process_reapable()`, `thread_reap_process()`), `docs/PROCESS-LIFECYCLE.md` | IMPLEMENTED_NOT_FULLY_VERIFIED |
| Thread lifecycle and kernel stacks | Per-thread owned kernel stacks in a guarded stack arena, TSS RSP0 publication, stack release during reap | Caller still owns the thread object storage; no production spawn-owned process/thread allocation lifecycle | `kernel/thread.c`, `kernel/include/thread.h`, `thread_reap_process()` | IMPLEMENTED_NOT_FULLY_VERIFIED |
| Scheduler/context switching | Cooperative and interrupt-frame scheduling; CPL3 frame includes user RSP/SS, is copied into per-thread storage and validated; syscall entry uses per-thread RSP0 | CPL3 timer preemption remains deliberately disabled; no scheduler-created initial CPL3 frame for a newly spawned thread; no dedicated idle thread or SMP-wide scheduler lock | `kernel/thread.c`, `kernel/idt.asm`, `kernel/kernel.c`, `scripts/test_timer_irq_assembly.sh` | IMPLEMENTED_NOT_FULLY_VERIFIED |
| Native ELF loader | Bounded ELF64 x86-64 ET_EXEC validation, PT_LOAD mapping, argument/environment stack construction, BSS zeroing and rollback tests | PT_DYNAMIC, PT_INTERP and PT_TLS are rejected; no dynamic linker; loading is not connected to general process spawn/exec | `kernel/elf_loader.c`, `kernel/include/elf_loader.h`, `kernel/init.c`, `pros_elf_selftest()` | IMPLEMENTED_NOT_FULLY_VERIFIED |
| Native spawn/exec | Existing process, ELF, address-space, usercopy and scheduler primitives are reusable | No spawn syscall/ABI; no initial user-thread frame construction and final publish/enqueue transaction; rollback across PID, process, thread, mappings, handles and VFS refs is not integrated | `include/abi/syscall.h` currently defines syscall IDs 1–7; `docs/NATIVE-SPAWN-CONTRACT.md` | BLOCKED_BY_DEPENDENCY |
| Syscall ABI and user-copy | Versioned native ABI, syscall policy, capability checks, validated copy helpers and page-fault fixup | No spawn/wait/exit ABI; usercopy fault state is single-CPU, not per-CPU; no SMP proof | `include/abi/syscall.h`, `kernel/syscall.c`, `kernel/usercopy.c` | IMPLEMENTED_NOT_FULLY_VERIFIED |
| Objects and capabilities | Typed kernel objects, per-process handle tables, generation-tagged handles, rights attenuation, duplication and transfer | Object reference counts are not atomic; no SMP-safe lifetime/revocation protocol; guest HANDLE/FD semantics are not present | `kernel/object.c`, `kernel/include/object.h`, `kernel/intent.c` | IMPLEMENTED_NOT_FULLY_VERIFIED |
| IPC | Bounded 8-slot/64-byte channels, capability-mediated send/receive, blocking wait/wakeup and waiter cancellation | No user-space channel creation/lifecycle service; lock/scheduler ordering is single-CPU only; no shared-memory transport | `kernel/ipc.c`, `kernel/include/ipc.h`, `docs/IPC.md` | IMPLEMENTED_NOT_FULLY_VERIFIED |
| Native VFS and ProsenjitFS | Existing native VFS and ProsenjitFS path with permission checks and provider interface | No Linux mount namespace or Windows drive namespace; guest path semantics must remain adapters above the native VFS | `kernel/vfs.c`, `kernel/vfs_provider.c`, `docs/FILESYSTEM.md` | IMPLEMENTED_NOT_FULLY_VERIFIED |
| Native user-space services | Kernel-only service registry with dependency ordering, callback start/stop and bounded retry bookkeeping | Callbacks are kernel function pointers; no service process identity, user-space IPC endpoint ownership, process launch, watchdog isolation or crash restart boundary | `kernel/service_manager.c`, `kernel/include/service_manager.h`, `kernel/kernel.c` | PARTIALLY_IMPLEMENTED |
| Microkernel boundary | Kernel has separable process, memory, IPC, object, VFS and driver subsystems | Current implementation still keeps major services and driver registry in kernel space; no verified user-space service process, service identity broker or crash/restart isolation | `kernel/` source layout, `docs/PROJECT-STRUCTURE.md`, `docs/IPC.md` | PARTIALLY_IMPLEMENTED |
| Linux ELF/syscall runtime | No Linux execution path found on main | Linux ABI loader, initial stack/auxv, syscall translation, FD table, signal model, libc/runtime services and executable tests are absent | `docs/COMPATIBILITY-FIRST-IMPLEMENTATION-MODEL.md`; current native syscall ABI is not Linux ABI | NOT_IMPLEMENTED |
| Windows PE/Win32 runtime | Kernel-side PE metadata inspection and NT/driver policy scaffolding | No user-space PE image mapper, relocation/import resolver, DLL loader, Win32/NT user runtime or executable test. Separately audit optional-header field bounds: the current size guards should be checked against every field read (including subsystem and PE32/PE32+ import-directory offsets) with malformed-size tests | `kernel/pe_inspect.c`, `kernel/nt_compat.c`, `docs/V2-UNIFIED-APPLICATION-RUNTIME.md` | NOT_IMPLEMENTED |
| Separate Windows HANDLE and Linux FD models | Native ProsenjitOS handle/capability table | No guest-specific HANDLE table or Linux FD/file-description table; must not alias guest values to native pointers/handles | `kernel/object.c`, `kernel/process.c`, compatibility master guide | NOT_IMPLEMENTED |
| Guest VFS namespaces | Native VFS exists | No POSIX mount namespace or Windows drive/UTF-16 path namespace and semantic adapters | `kernel/vfs.c`, compatibility master guide | NOT_IMPLEMENTED |
| Exceptions, Linux signals and Windows SEH | Native exception/interrupt entry and kernel fault handling foundation | Guest signal delivery and SEH records/context/unwind are separate, absent compatibility subsystems | `kernel/idt.asm`, exception entry in `kernel/kernel.c`, compatibility master guide | PARTIALLY_IMPLEMENTED |
| Shared memory / zero-copy IPC | Address-space mapping primitives | No reference-counted shared-memory object, capability-mediated cross-process mapping lifecycle, revocation or benchmark | `kernel/address_space.c`, `kernel/object.c`, `kernel/ipc.c` | NOT_IMPLEMENTED |
| Native driver registry and device broker | Driver registry, PCI matching/bind lifecycle, bounded device grant/revoke/check table | Grants are policy records, not an isolated driver host; no general driver process, watchdog/restart or process-bound device capability ABI | `kernel/driver.c`, `kernel/device_broker.c`, `docs/DRIVER-MODEL.md` | PARTIALLY_IMPLEMENTED |
| Linux .ko / Windows .sys compatibility | Documentation and limited NT policy/dispatch scaffolding | No supported Linux kernel-module or WDM/WDF execution framework; arbitrary foreign kernel binaries remain unsupported | `kernel/nt_compat.c`, `kernel/include/nt_compat.h`, `docs/DRIVER-MODEL.md` | NOT_IMPLEMENTED |
| Physical DMA/IOMMU/interrupt and device validation | Some controller, PCI, DMA and interrupt source plus QEMU/synthetic tests | IOMMU mediation, real-device ownership/teardown and physical fault-injection evidence are not established | Driver and hardware source; physical lab evidence absent in this audit | REQUIRES_PHYSICAL_HARDWARE_TEST |
| Build/test infrastructure | Warnings-as-errors freestanding build, host-test scripts, ISO/native BIOS QEMU and security workflows | Exact latest main-head run results were not retrievable through the available PR-only workflow lookup in this audit | `Makefile`, `.github/workflows/build.yml`, `.github/workflows/security-design.yml` | IMPLEMENTED_NOT_FULLY_VERIFIED |

### Architecture finding

The repository has a substantial native-kernel foundation, but it is **not yet a completed microkernel**. The current mainline still hosts major services and the driver registry in kernel space. The safe path is not to move code into user space prematurely: first complete user-mode process creation, capability-backed service identity, IPC endpoint lifecycle, memory isolation, and service crash/teardown behavior. Only then can non-critical services be moved behind stable user-space boundaries without weakening the trusted computing base.

### Reusable implementation and smallest safe next changes

Do not recreate PMM, VMM, process lifecycle, ELF validation/loading, usercopy, object handles, IPC, native VFS or the driver registry. The first implementation PR should be limited to the native execution transaction:

1. Add an internal non-runnable process-construction path while preserving existing `process_create()` callers.
2. Construct and validate the initial CPL3 interrupt frame for a new thread using the existing 20-qword frame ABI; do not enable CPL3 timer preemption yet.
3. Stage executable admission, ELF load, user stack, kernel stack, thread record, initial capabilities and syscall policy before publication.
4. Make publication/enqueue the final commit point with explicit ownership flags and complete reverse-order rollback.
5. Add a read-only full thread-stack/TSS reap preflight before irreversible process resource release; test failed reap and retry invariants. Add wait/exit ABI only after user output-copy and reap ordering are defined and tested.
6. Add negative tests for every allocation/admission/mapping/queue/publication failure and a real QEMU user-mode launch test.

No Linux or Windows compatibility implementation should begin until the native process launch and service/IPC security gates are stable.

### Relevant PR and branch audit

- **PR #472:** merged into main by commit `05fdcd8561327ca1122626378297edae4be56565`; it is the documentation baseline, not runtime implementation.
- **PR #183:** older Part 1 kernel/boot/hardware audit branch is 93 commits ahead and 682 behind current main, with a broad 20+ file source diff. Its own acceptance is explicitly PARTIAL; it includes allocator/interrupt/paging hardening and tests that must be individually reconciled against later mainline changes. Do not merge the stale branch wholesale.
- **PR #157:** documentation-only native-spawn audit branch; comparison showed 1 commit ahead and 693 behind current main. Do not merge this stale branch as-is.
- **PR #152:** interrupt-frame documentation-only branch; comparison showed 1 commit ahead and 691 behind current main. Main already contains a later 20-qword CPL3 frame implementation; reconcile source before considering any part of the old branch.
- **PR #155:** scheduler wake-frame fix is stacked on #153 and explicitly not SMP-safe. Main has later scheduler frame-publication/rollback commits (#635 and subsequent IPC/process waiter regressions); compare exact diffs before considering this PR.
- **PR #125:** ELF rollback branch is 12 commits ahead and 865 behind main. Its latest branch description says exact-head build/QEMU verification was pending. Main must remain authoritative; do not merge or cherry-pick the stale branch without a fresh source diff and tests.
- **PR #122/#123/#124:** Windows inspection hardening, WinBridge loader contract and user-space PE metadata parser are stacked historical work. #123 is documentation-only; #124 is parser-only and its latest import-reader tests were explicitly unrun. The parser branch is 128 commits ahead and 865 behind main; no automatic merge/cherry-pick.
- **PR #137:** desktop dependency orchestration is preflight/adapter scaffolding, not native spawn/exec or a running user-space service manager.
- **PR #171/#175:** native hardware/desktop integration contracts and host models are stacked, acceptance-incomplete work. Reuse only after checking their ABI and device/service dependencies; they do not establish a running microkernel service environment.
- **PR #180/#143:** earlier compatibility and universal application/driver planning documents. PR #472 is the merged, newer compatibility master guide; avoid duplicating or merging superseded plans.
- **PR #164:** app registry/transaction coordinator is not a concrete VFS installer or executable launcher; its current self-hosted test status was reported pending.
- Other open PRs outside this architecture slice are left untouched.

### Verification status

This is a source/documentation audit only. No code was changed in this audit section, no build or runtime test was run, and no PR was merged. Exact current-main CI remains **UNVERIFIED** in this session. Physical hardware remains **NOT TESTED**.

## Executive finding

The repository has substantial kernel, native ELF, process, memory, VFS/filesystem, IPC, security, and device foundations. Native process execution has a boot-time CPL3/init path with extensive QEMU markers, but there is no general public spawn/exec syscall or complete native application workflow. The desktop manager is an in-memory registry/preflight module, not an installer or launcher.

Windows application execution is **not implemented**. The installation specification says so. The checked-in `kernel/pe_inspect.c` and `kernel/nt_compat.c` are kernel-side inspection/NT-driver ABI scaffolding, not a user-space PE image loader or Win32 runtime. This is a direct mismatch with the intended user-space WinBridge boundary and must be resolved before implementation expands. Open PRs #123 and #124 are proposals/work in progress, not main-branch implementation evidence.

## Status labels and evidence

| Subsystem | Status | Evidence on main and gap |
|---|---|---|
| Boot and x86-64 entry | IMPLEMENTED_NOT_FULLY_VERIFIED | `boot/`, `kernel/kernel.c`, `.github/workflows/build.yml`; build workflow defines ISO and native BIOS QEMU marker checks. ISO and native BIOS QEMU marker suites passed locally and on hosted run #9000; UEFI/physical hardware not verified. |
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
| Build/test infrastructure | IMPLEMENTED_AND_VERIFIED | `Makefile`, scripts and Actions define host tests, ISO build and QEMU boots. Local `make`, host tests, both QEMU marker suites, script syntax, and security contract checks passed; hosted Security #8495 and Build/boot #9000 also passed on the audited executable source. Physical hardware and UEFI are unverified. |

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
- On the current audited source PR head `d0f9e933f298da7a2760dee605850e83fcab4bbe`, GitHub hosted Security run #8495 and Build/boot run #9000 both **passed**. Their executable source is identical to audited main; the PR changes documentation only.
- Earlier Actions observations: PR #305 run #8990 was in progress, and #8989/#8988 were cancelled as newer commits superseded those runs under workflow concurrency. The cancellation is not a source/test failure diagnosis.

Open PRs relevant to scope: #305 (scheduler/VFS audit), #183 (kernel/boot/hardware audit), #123 (WinBridge user-space loader contract), #124 (user-space PE metadata parser), and #164 (native app registry). Their proposals remain distinct from implementation on `main`.

## Next task

Start Part 2 with a source-first **native process execution and VM integration gate**: reconcile `docs/NATIVE-SPAWN-CONTRACT.md` with `process_create()`, `pros_init_load()`, `thread_init()`, syscall policy, user address-space APIs, and thread stack ownership; specify and test a general safe CPL3 spawn path. In parallel, review open PR #124's user-space PE parser as parser-only work. Do not implement API compatibility or claim Windows execution until the native spawn and user-memory/protection contracts are verified.

## 2026-11-18 — PR #642 Phase 1 branch revalidation

**Audited ref:** `phase1/transactional-wait-reap-20261114`  
**Audited commit before this documentation update:** `55f06d314621a8537d9db88663fc71d92c39051b`  
**Scope:** reconcile the historical 2026-11-14 mainline assessment above with the later native-spawn and transactional wait/reap implementation on PR #642. This section describes the PR branch only; it does not imply these changes are merged into `main`.

### Corrected branch implementation status

The older matrix above is a historical snapshot and several of its process/thread/spawn statements are superseded on this branch:

- **Native process construction:** the branch contains an internal staged spawn transaction. The child is built while unpublished/non-runnable; executable admission, ELF load, user stack/argv/envp, kernel thread/frame, policy/capability state and final scheduler publication are handled as a transaction with deterministic rollback failpoints.
- **Initial CPL3 entry:** the branch constructs the user interrupt-return frame before publishing the thread. The scheduler-created child enters through the validated CPL3 return path; this is not a C call into user mode.
- **Argument/environment handoff:** the actual spawned child validates `argc == 2`, `argv[0] == "/init"`, `argv[1] == "elf-runtime"`, the argv terminator, `envp[0] == "PROS_ELF_RUNTIME=1"`, the envp terminator, pointer bounds and NUL-terminated bytes.
- **Exit/wait/reap:** the branch has staged/read-only preflight and retry-oriented cleanup checks. Tests exercise corrupted run-queue endpoints, active TSS stack, malformed stack metadata, active CR3, handle refcount corruption and owned-page metadata corruption; a rejected reap preserves the ZOMBIE/resources so a corrected retry can complete.
- **Thread registry/stack validation:** bounded registry validation checks cycles/repeated records, duplicate stack slots, physical-page aliases across records, and allocator/registry bitmap consistency. Stack teardown uses checked expected-physical-page unmapping and batched physical-page release; failed cleanup is quarantined rather than silently freeing unproven ownership.
- **IRQ serialization:** kernel-thread reap and unpublished-thread rollback preserve caller interrupt state while keeping their validation and destructive mutation in one interrupt-excluded transaction.

### Remaining Phase 1 verification gaps and explicit limits

- The registry validator's cross-record physical-page alias check still needs a dedicated regression that proves the pairwise branch is reached while both records are independently valid. A test that merely duplicates a stack slot would exercise the duplicate-slot rejection instead and is not equivalent.
- Add a focused rollback-preflight rejection test proving that aggregate preflight failure leaves every resource, registry link, queue state and publication bit unchanged. Existing allocation/stage failpoints do not by themselves prove this invariant.
- The public native syscall ABI still has no general spawn/wait/exit syscall contract. The current kernel-side boot/test spawn path must not be described as a general user-facing application launcher.
- Dynamic ELF features such as `PT_DYNAMIC`, `PT_INTERP` and `PT_TLS` remain unsupported/rejected; no dynamic linker is claimed.
- CPL3 timer preemption remains disabled. This work does not establish SMP safety, multi-CPU locking, TLB shootdown, production readiness or physical-hardware coverage.
- The kernel still hosts major services and driver registry components; the microkernel boundary is not complete merely because a CPL3 process can be launched.

### Exact-head hosted verification

At the pre-update source head `55f06d314621a8537d9db88663fc71d92c39051b`, GitHub Actions reported:

- Build and boot-test ProsenjitOS, run `44247429299`: **success**.
- Security architecture checks, run `44247429257`: **success**.

These are exact-head hosted workflow results for the source head named above. They do not substitute for the two remaining regression tests listed here, and they do not verify the documentation-only commit that appends this section until workflows run on that new head.

**Disposition:** Master Phase 1 remains **IN PROGRESS / NOT EXIT-GATE COMPLETE**. PR #642 remains open and unmerged. No merge is authorized by this audit.


## Phase 1 follow-up verification — process rollback and heap-growth rollback

This addendum supersedes the earlier open-test bullets above where the corresponding regression is now present and passes:

- **Thread-registry cross-record ownership:** `thread_registry_integrity_selftest()` validates a bounded, acyclic registry; it injects a duplicate slot and, separately, a duplicate physical stack page across two distinct registered thread records, then restores each fixture. It also tests a registry cycle, allocator-slot bitmap disagreement, address-space-owned-page aliasing against a registered stack page, and page-table-typed stack-page rejection. These checks run on both ISO and Native BIOS QEMU paths.
- **Aggregate spawn rollback preflight:** `process_spawn_rollback_selftest()` exercises rejection before mutation for both unpublished and registered/queued child paths. It restores the injected invariant and retries the same cleanup transaction, verifying process lookup/publication, process state, address-space root, object lifetime, queue links, thread registry membership and stack ownership.
- **ELF initial stack:** the loader self-test and the scheduler-created CPL3 child boot test now verify argv/envp pointer vectors, NULL terminators, exact NUL-terminated string contents, bounds within the stack mapping, and 16-byte initial-RSP alignment. This replaces the earlier argc-only verification note.
- **Transactional heap growth rollback:** `heap_grow()` now uses `paging_unmap_pages_expected_and_free()` for each bounded rollback batch instead of separate unmap and PMM-free calls. The combined helper validates the expected VA-to-PA mappings and the complete physical-page free batch before clearing PTEs, and restores PTEs if the final PMM batch commit unexpectedly rejects. Existing heap-growth fault tests cover failure before the first mapping, real PMM failure before any page allocation, PMM failure after one successful mapped page, and injected failure after one successful mapping; the paging self-test separately verifies a rejected free-batch leaves its mapping and physical allocations unchanged.

Exact source head for the heap rollback change: `26cd28312c9c6254d15529a8e7adaeb90be9614d`. Build/boot-test run [44436176104](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/44436176104) and Security architecture run [44436176125](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/44436176125) both completed successfully for that exact head. Both ISO and Native BIOS logs include the physical-memory, paging, heap deep-invariant, recoverable usercopy IST3-frame, process wait/reap retry, argv/envp-content, exit/wait/reap, and isolated panic-frame markers. The duplicate-owner panic fail-stop marker is also present in both debugcon captures.

**Limits remain:** this is still the documented single-CPU/interrupt-exclusion design. No SMP allocator/lifecycle locking, TLB shootdown, physical-page reference counting/shared-page ownership model, general user-facing spawn syscall, dynamic ELF linker, or physical-hardware coverage is claimed. Phase 1 remains open until the remaining architecture and panic/fault coverage exit criteria are reviewed and verified.


## 2026-11-20 — process registry exact-address identity and allocation rollback

The earlier registry validator checked supervisor mappings and structural fields, but those checks alone did not distinguish a registered caller-owned process object from a byte-for-byte copy placed at another mapped kernel address. The registry now maintains a private kernel-heap sidecar chain. Each successful `process_register()` publishes one sidecar record containing the exact process-storage address; the process and sidecar chains are validated in bounded lockstep, and `process_unregister()` removes and frees the matching record only after teardown and final object-reference release.

The process structure remains caller-owned and must stay mapped and stable until successful unregister. `process_register()` is a trusted kernel publication boundary, not a user-mode API. The sidecar is exact-address membership evidence, not cryptographic protection against arbitrary corruption of privileged kernel memory. The implementation remains single-CPU/interrupt-exclusion only.

Boot regressions substitute a copied process object at the same registry-list position, inject a cycle into the private identity chain, and fail the sidecar heap allocation during `process_create()`. The tests require bounded rejection, successful recovery after fixture restoration, and no published PID, address-space root, or retained initial object reference after allocation failure. The first allocation-failure fixture was moved from the already-large process self-test stack to static storage after QEMU exposed stack exhaustion in the test harness; the production process-object storage contract was unchanged.

This closes the exact-address registry-membership gap for published process records. It does not close the remaining Phase 1 PMM/VMM, panic/fault handoff, boot-device identity, physical-hardware, or broader architecture exit gates. PR #642 remains open pending exact-head required checks and independent source review.


## 2026-11-25 — Multiboot2 memory-map record boundary

The PMM Multiboot2 memory-map parser previously iterated while a complete entry remained, silently ignoring a trailing partial entry when `(tag->size - 16) % entry_size` was nonzero. The parser now rejects that malformed map before consuming any of its entries. This keeps the firmware-provided map structurally exact instead of accepting a truncated final record.

The parser also now requires a correctly sized 8-byte Multiboot2 end tag to be the final tag in the declared information block; exhausting the block without that terminator is rejected. This prevents a truncated tag stream from being accepted as a complete memory map. Source-level change only. No compiler, boot, QEMU, or exact-head CI result is claimed; PR #642 remains open pending required exact-head checks and independent review.
