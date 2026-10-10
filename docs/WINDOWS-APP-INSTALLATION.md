# ProsenjitOS Windows Application Installation Pipeline

Status: **design specification / implementation not yet verified**.

This document defines the installation and management workflow for Windows applications without Wine or a virtual machine. The intended implementation is an independently developed, user-space compatibility runtime. This is not a claim that Windows applications or Microsoft Office currently run on ProsenjitOS.

## Goals

- Let the user open a Windows `.exe` or `.msi` from the native Software Center or file manager.
- Support install, upgrade, repair, uninstall, per-app configuration, logs, and rollback where safe.
- Use an independent PE loader and Win32-compatible user-space API runtime (working name: **Prosenjit WinBridge**).
- Make Microsoft 365 Apps / the latest eligible Office package an explicit compatibility target, not an assumed success.
- Keep all compatibility code out of kernel core; use documented native syscall, VFS, process, capability, graphics, and network contracts.
- Never bypass Microsoft licensing, activation, publisher checks, or installer requirements.

## User flow

1. User selects an installer or chooses an app from Software Center.
2. Identify the actual format from file contents; do not trust the extension alone.
3. Stage the installer in a temporary, quota-limited directory.
4. Verify file bounds and structure, compute SHA-256, record provenance, and verify Authenticode/publisher trust when implementable. A valid signature is not a guarantee that software is harmless.
5. Display publisher, requested permissions, compatibility status, required dependencies, disk space, network needs, and whether the installer is known to be unsupported.
6. Resolve dependencies from trusted sources. Do not download arbitrary DLLs from untrusted websites or silently fetch redistributables.
7. Create an isolated per-application prefix/profile and least-privilege capability set. Ask for elevation only for a specific justified operation.
8. Execute through WinBridge only if the required PE architecture, API set, loader features, and dependencies pass preflight checks. Never execute the Windows binary as native machine code without a compatible execution environment.
9. Track files, registry-like configuration, services, shortcuts, file associations, and install actions in a transaction manifest. Treat arbitrary installer side effects as untrusted and constrain them.
10. Run post-install smoke tests, commit the manifest only on success, and offer rollback for files/configuration that can safely be restored.
11. Show an honest result: installed and smoke-tested, installed but not fully tested, blocked as unsupported, or failed with a diagnostic log.

## Components

- **Windows Installer Front End**: file-manager association, Software Center UI, progress, consent, logs.
- **Format Detector**: PE executable, MSI database/package, archive, script, or unknown.
- **WinBridge PE Loader**: PE32/PE32+ parsing, architecture checks, imports, relocations, section protections, TLS, exception/unwind requirements, and bounded memory operations. Reject unsupported features clearly.
- **WinAPI Runtime**: staged implementation of documented Win32 API behavior, DLL loading, process/thread lifecycle, synchronization, filesystem and registry-like semantics, COM and graphics only as implemented and tested.
- **Dependency Broker**: trusted, versioned, architecture-aware runtime dependencies; provenance and hashes; no untrusted DLL search-path fallback.
- **Application Isolation Manager**: per-app identity, filesystem mapping, IPC/device/network permissions, quotas, and explicit brokered access.
- **Install Transaction Manager**: manifest, before/after state, idempotent operations, recovery after interruption, uninstall/repair, and rollback where possible.
- **Compatibility Database**: tested app/version/build, installer type, required APIs, known limitations, test results, and regression status.
- **Office Deployment Adapter**: guided configuration for licensed Microsoft 365 Apps deployments; must not pretend the Windows Office Deployment Tool is a native Linux/ProsenjitOS executable.

## EXE and MSI behavior

### EXE

- Validate PE structure, architecture, import table, resource limits, and signature where available.
- Do not assume every `.exe` is a GUI application or installer; it may be a console app, self-extractor, updater, driver installer, or malicious payload.
- Run only inside WinBridge with a declared sandbox profile.
- Block or explicitly report unsupported services, kernel-driver installation, privileged system modification, and unsupported API calls.

### MSI

- Parse the MSI database using a dedicated parser; validate tables and custom-action metadata before execution.
- Implement Windows Installer semantics incrementally; MSI custom actions can run arbitrary code and must be treated as untrusted execution.
- Do not claim general MSI support until transaction, rollback, upgrade, repair, custom-action, and uninstall tests pass.
- Never silently invoke a Windows-only helper that requires Wine or a VM.

## Microsoft 365 Apps / latest Office requirement

The project must target the latest **Microsoft-supported Microsoft 365 Apps package that the user is eligible to install**, with a configurable update channel (default target: Current Channel when supported). Office 2024 perpetual and Microsoft 365 subscription products are different deployment/licensing targets and must not be conflated.

Microsoft's official Office Deployment Tool (ODT) is a Windows deployment executable designed to download/configure Click-to-Run Office. Its official workflow uses a configuration XML and ODT modes such as `/download` and `/configure`. ODT is **not itself a cross-platform Office runtime**. Without Wine or a VM, ProsenjitOS must implement enough of the required PE/Win32/Click-to-Run behavior independently before the Windows ODT or Office installer can run. Do not bundle or reimplement Microsoft's proprietary code.

Office installation workflow, once compatibility prerequisites exist:

1. Let the user choose Microsoft 365 Apps product/edition, language, architecture, and supported update channel.
2. Direct the user to official Microsoft sign-in/licensing and deployment configuration resources. Never store account passwords or licensing tokens in plain-text app manifests.
3. Retrieve deployment metadata and installation content only from official Microsoft endpoints, validating transport, signatures/hashes where available, and package identity.
4. Preflight required PE/Win32 APIs, Click-to-Run services, COM components, licensing/activation, WebView/browser dependencies where applicable, update service behavior, fonts, printing, file dialogs, and graphics APIs.
5. If any mandatory dependency is unsupported, stop before installation and show the missing capability list; do not label Office as compatible merely because its installer starts.
6. If preflight passes, install in a dedicated isolated Office profile, preserving the user's explicit consent and Microsoft's licence terms.
7. Verify launch and basic workflows for Word, Excel, and PowerPoint; verify sign-in/activation, file open/save, fonts, printing/export, updates, and crash-free restart. Test OneDrive separately because its client has its own requirements.
8. Keep Office updates managed through the supported deployment channel; rerun compatibility gates after major updates. Provide recovery/rollback where technically safe.
9. Maintain a compatibility matrix for the exact Office product, build, channel, architecture, ProsenjitOS build, and WinBridge revision.

Official Microsoft references:
- Office Deployment Tool overview: https://learn.microsoft.com/en-us/microsoft-365-apps/deploy/overview-office-deployment-tool
- Microsoft 365 Apps cloud deployment: https://learn.microsoft.com/en-us/microsoft-365-apps/deploy/deploy-microsoft-365-apps-cloud
- Office Customization Tool: https://config.office.com/deploymentsettings

**Hard limitation:** Latest Office is a major compatibility milestone, not a promise of immediate support. A complete no-Wine/no-VM solution requires a substantial, independently implemented and tested Win32/COM/Click-to-Run/licensing/graphics/API surface. Until those tests pass on native ProsenjitOS, Office desktop installation and execution must be marked **not verified / not yet supported**. The browser-based Microsoft 365 apps may be offered as a separate web-app shortcut, but that is not the desktop package.

## Security and reliability requirements

- Least privilege; no automatic root/admin execution of arbitrary installers.
- Verify content, provenance, and publisher where possible; unknown is not the same as malicious.
- Isolate app files and configuration; deny kernel-driver installation through the Windows app path.
- Constrain filesystem, network, device, IPC, process, and resource access.
- Log installer identity, digest, source, requested permissions, decisions, exit status, and rollback outcome without recording secrets.
- Make install/upgrade operations recoverable after interruption; preserve user data during uninstall by default.
- Protect the package database against partial writes and concurrent package operations.
- Never silently downgrade, replace, or remove another product/version.
- No dependency on GitHub-hosted Actions; use reproducible local build/test commands and deterministic test fixtures.

## Implementation gates

1. **Foundation audit**: inventory native process/address-space, syscall, VFS, user-copy, thread, IPC, GUI, and networking contracts. Record missing dependencies.
2. **PE parser**: malformed/truncated/overflow fixtures, PE32+ headers, import parsing, section bounds, relocations, and rejection tests. Parser alone does not execute apps.
3. **Runtime MVP**: a tiny, self-authored PE test program using a minimal documented API subset; no proprietary binaries in tests.
4. **Filesystem/process API subset**: deterministic tests for handles, paths, errors, threads, synchronization, and cleanup.
5. **Installer transaction manager**: staged install, cancellation, interrupted install recovery, uninstall, upgrade, repair, rollback and manifest-integrity tests.
6. **Sandbox/policy**: adversarial path, privilege, dependency hijack, malformed package, and denied-resource tests.
7. **GUI integration**: file association, compatibility preflight, permission prompt, progress, logs, and truthful unsupported-state UI.
8. **MSI support**: only after the required parser and Windows Installer semantics have explicit test coverage.
9. **Office feasibility gate**: inventory mandatory APIs and test the newest eligible Microsoft 365 Apps build. Stop and document blockers instead of claiming support.
10. **Release gate**: clean local build, unit tests, fuzz/property tests for parsers, install/upgrade/uninstall/recovery matrix, security review, then native boot/runtime tests on the developer's Debian/QEMU setup.

## Definition of done

Do not call the installation process complete until the code exists, clean builds pass, deterministic tests pass, sandbox boundaries are verified, installation/recovery works across reboots, and a published compatibility matrix names exactly which app versions are supported. Architecture documentation alone is not implementation.
