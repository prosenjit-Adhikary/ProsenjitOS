# Version 2 Architecture Handoff — 2026-10-09

## Request
Record the Version 2 direction for ProsenjitOS: native ProsenjitOS remains the primary OS and desktop; supported Windows applications should install and launch from that same desktop without requiring a full Windows OS/desktop, a Windows VM as the normal application path, or Wine as a runtime dependency.

## Decisions recorded
- Add a modular user-space Windows App Runtime, including a bounded PE loader, DLL/import handling, selected Windows API contracts, and later GUI integration.
- Keep native applications on the native ProsenjitOS ABI/runtime.
- Present supported applications through one Application Manager and the existing shared desktop/window-system interfaces.
- Preserve ProsenjitOS security, least privilege, isolation, and resource limits for Windows applications.
- Prioritize native OS, process, syscall, VFS/ProsenjitFS, security, and userspace stability before broad application compatibility.
- Treat Windows kernel-mode driver support as a separate later research track. Never load Windows `.sys` binaries directly as native ProsenjitOS kernel drivers.
- Universal Windows application/driver compatibility is not promised; support must be documented and tested per application/API.

## Files changed on branch `docs/v2-unified-runtime-architecture`
- Added `docs/V2-UNIFIED-APPLICATION-RUNTIME.md`: goals, non-goals, component boundaries, security constraints, phased implementation plan, and verification gates.
- Updated `docs/ROADMAP.md`: added Version 2 milestones and replaced the generic Windows compatibility checkbox with an independent Windows App Runtime target.

## Checks and results
- [x] Confirmed repository: `prosenjit-Adhikary/ProsenjitOS`, default branch `main`.
- [x] Inspected the current roadmap and README compatibility strategy before editing.
- [x] Created an isolated documentation branch from `main`.
- [x] Added the V2 architecture document.
- [x] Updated the roadmap on the same branch.
- [ ] Build/QEMU tests: not run; this change is documentation-only and changes no source or build files.
- [ ] Merge: pending review of the pull request.

## Technical qualification
Not using Wine or booting the complete Windows OS does not remove the need to implement or lawfully reuse Windows executable/ABI/API behavior. The document explicitly records this limitation so the roadmap does not promise a shortcut to universal compatibility.

## Next steps
1. Review and merge the documentation PR.
2. Continue current kernel/filesystem/security/user-mode stabilization.
3. When prerequisites are stable, implement and test a bounded PE parser/validator, then a minimal controlled console-program proof of concept.
