# Foundation Audit Log

Date: 2026-10-29  
Repository: `prosenjit-Adhikary/ProsenjitOS`  
Branch: `audit/foundation-architecture-20261029`  
Starting branch / commit: `main` / `fc8604aca130561182e0e1b50077654569ba5e80`  
Starting working-tree status: not available; the workspace did not contain a repository checkout.  
Audit document commit: `ee13c8f81f410a03541b0de779ba2854dc980aeb`. Log created in the following commit.

## Work completed

- Inspected repository metadata, default branch, recursive source tree (193 entries), latest main commit, open PR list, recent Actions runs, and build workflow.
- Read the README, architecture, project structure, native spawn contract, process lifecycle, unified runtime design, Windows install specification, IPC contract, roadmap, filesystem canonical document and security model.
- Inspected implementation interfaces/source for ELF loader, process manager, desktop manager, PE inspection, and NT compatibility.
- Recorded evidence-backed subsystem labels, current native/Windows execution split, interfaces/gaps, risks and dependency roadmap in [FOUNDATION-AUDIT.md](FOUNDATION-AUDIT.md).
- Confirmed Windows application compatibility and Microsoft Office are targets only. The main branch contains no user-space WinBridge runtime or installer. Existing kernel PE inspection and NT driver ABI must not be mistaken for an application loader.
- Identified a boundary conflict: current PE inspection and NT compatibility code live under `kernel/`, while the architecture/install specification requires app compatibility in userspace. These files are inspection/driver scaffolds, not proof of runnable PE applications. Keep runtime in userspace unless a concrete kernel primitive is justified.
- Open PRs checked include #183 (kernel/boot/hardware audit), #123 (WinBridge loader contract), #124 (user-space PE metadata parser), #164 (native app registry), and #305 (scheduler/VFS concurrency audit). Open PR work is not main implementation evidence.

## Verification record

The private repository source tree was fetched from the authenticated GitHub API at `main` commit `fc8604aca130561182e0e1b50077654569ba5e80` into a local snapshot. The audit PR changes documentation only, so executable source and test inputs match this base.

| Check | Exact command/workflow | Actual result |
|---|---|---|
| Build / image creation | `make` | PASS, exit 0. Produced ISO and native BIOS image; formatter created the disposable native ProsenjitFS partition. |
| Desktop host unit tests | `make desktop-unit-test` | PASS: `desktop manager tests passed`. |
| Security service host unit tests | `make security-service-test` | PASS: `security control center service dispatcher tests passed`. |
| Workflow script syntax | `bash -n scripts/desktop_complete.sh`; `bash -n scripts/test_desktop_manager.sh`; `bash -n scripts/test_security_control_center_service.sh` | PASS, all commands exit 0. |
| ISO boot test | `timeout 20s qemu-system-x86_64 -cdrom build/prosenjitOS.iso -display none -serial file:build/qemu-serial.log -no-reboot -no-shutdown -d int -D build/qemu-debug.log` | Expected timeout exit 124. All 37/37 workflow `require_log` markers found. Hosted Build/boot run #9000 also passed the ISO QEMU step. |
| Native BIOS boot test | `timeout 20s qemu-system-x86_64 -drive format=raw,file=build/prosenjit-native-bios.img,if=ide -display none -serial file:build/native-qemu-serial.log -no-reboot -no-shutdown -d int -D build/native-qemu-debug.log` | Expected timeout exit 124. All 41/41 workflow `require_native_log` markers found. Hosted Build/boot run #9000 also passed this step. |
| Security architecture contract | Required-marker checks from `.github/workflows/security-design.yml` against `docs/SECURITY-MALWARE.md` and `docs/ROADMAP.md` | PASS locally; hosted Security workflow #8495 passed. |
| Exact-head hosted workflows | GitHub Actions at PR head `d0f9e933f298da7a2760dee605850e83fcab4bbe` | Build/boot #9000 **SUCCESS**; Security architecture #8495 **SUCCESS**. |
| Static analysis | None defined by inspected Makefile/workflows | NOT RUN; no configured static-analysis target was found. |

## Hosted CI follow-up (2026-10-29)

- Security run #8493 succeeded on earlier doc-only PR SHA `ef888b42b977911a219f243e0ad967d917889075`.
- Build run #8998 attempt 1 ran on runner `home`; checkout, toolchain validation, host tests, ISO build and filesystem diagnostic passed. The ISO QEMU step ended with GitHub's `The operation was canceled`; available logs do not establish the cause of that first cancellation.
- Attempt 2 of #8998 then passed the ISO QEMU step but was cancelled during native BIOS QEMU when a newer progress-log commit arrived. This is explained by the workflow's `cancel-in-progress: true` concurrency policy for a PR. That cancellation was not a source failure.
- The newer exact-head runs #8495 (Security) and #9000 (Build/boot, including both QEMU suites) both passed on SHA `d0f9e933f298da7a2760dee605850e83fcab4bbe`.
- Earlier PR #305 runs #8989/#8988 were cancelled as newer commits superseded them; #8990 was in progress when first inspected. No root cause for unrelated earlier PR cancellations was inferred.

## Architecture decisions and source evidence

- Native current boot path: `kernel/init.c` calls native ELF loading for `/init`; `kernel/elf_loader.c` handles ELF64 load segments; `kernel/process.c` and `kernel/include/process.h` expose process lifecycle. `docs/NATIVE-SPAWN-CONTRACT.md` explicitly records there is no public spawn/exec syscall and process creation alone does not load an image or create a runnable thread.
- `user/desktop/desktop_manager.[ch]` implements a fixed-size registry/preflight API. Its header says entry paths are metadata only and it is not a launcher. A host test exists at `tests/desktop_manager_test.c`.
- `kernel/pe_inspect.c` / `kernel/include/pe_inspect.h` provide PE inspection, not a mapped PE execution path. `kernel/nt_compat.c` / `kernel/include/nt_compat.h` define NT/driver compatibility scaffolding and explicitly state a Windows PE driver is never treated as a native kernel module.
- `docs/WINDOWS-APP-INSTALLATION.md` labels itself design-only and describes proposed user-space WinBridge, dependency resolution, transactions, isolation and rollback.
- `docs/V2-UNIFIED-APPLICATION-RUNTIME.md` keeps Windows runtime userspace and warns avoiding Wine does not remove the need to implement PE/ABI/API/runtime contracts.
- Existing memory, process, syscall, VFS, IPC and security components offer foundations but end-to-end app creation, per-app installation transactions and compatibility isolation remain unverified or missing.

## Remaining work / next recommended task

1. Obtain a credentialed local checkout and run the exact-head available build, host tests, static checks and documentation checks.
2. Reconcile this audit with existing open PRs #123/#124/#183 and update statuses after their merge state changes.
3. Start Part 2 with the native spawn/memory gate: specify and test safe general CPL3 process construction, thread stack allocation/ownership/reclamation, user address-space protection/rollback, and syscall policy. Review PR #124 as parser-only work in parallel.
4. Only after those gates, proceed to a separate userspace PE loader and later WinBridge API/runtime stages.

This log records completed documentation, local exact-source build/test verification, and successful exact-head hosted Build/boot and Security workflows. No Windows compatibility implementation is claimed.


## Hosted CI follow-up (2026-10-29)

- Security architecture workflow #8493 succeeded for audited PR source SHA `ef888b42b977911a219f243e0ad967d917889075`.
- Build/boot workflow #8998 attempt 1 ran on runner `home`; checkout, runner/toolchain validation, host tests, ISO build, and ProsenjitFS diagnostic passed. The ISO QEMU step ended with GitHub's `The operation was canceled` at 17:30:42 UTC; native BIOS test was skipped. Available logs do not identify who/what canceled the operation, so root cause is **UNKNOWN**. Do not label this a source failure.
- GitHub automatically shows attempt 2 of #8998 queued at last inspection, with no job started. Independent local verification above passed `make`, both complete QEMU marker suites, host tests, and the security contract check on the PR-identical executable source.
- Exact hosted CI references: [Security #8493](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40921633640), [Build/boot #8998](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40921633561).
