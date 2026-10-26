# ProsenjitOS Security Control Center

Status: proposed integration specification; UI and complete end-to-end enforcement are not claimed implemented.

## 1. Purpose and architectural fit

The Security Control Center is the user-facing administration surface for the existing ProsenjitOS security architecture. It is **not a second OS architecture**, does not introduce an Aegis OS identity, and must reuse the existing desktop/session work and shared desktop ABI. Do not delete or replace existing desktop components to add this feature.

The Control Center is an unprivileged user-space client of trusted ProsenjitOS security services. It must never implement authoritative permission decisions, access kernel-private state directly, or treat a visual toggle as enforcement.

Constraints:
- Keep the native x86-64 kernel, object/capability model, syscall policy, VFS/ProsenjitFS, Reason Ledger, security admission/quarantine, device permission broker, and existing desktop architecture.
- GUI and CUI use the same authenticated service/API contracts and receive the same policy decisions.
- AI, reputation providers, and UI are advisory/requesting components only. The OS security manager remains authoritative.
- No unsupported capability may be shown as active. Display Unavailable, Not implemented, or Not verified explicitly.
- Changes that grant privileges require authenticated authorization and an auditable decision.
- Logging and UI failure must not fail open or weaken enforcement.

## 2. Navigation and information architecture

The Control Center should be reachable from the existing desktop Settings and application launcher. Reuse the project's current desktop/windowing components; do not create a competing window manager or parallel desktop ABI.

### Overview
Show evidence-based component states, pending security actions, recent Reason Ledger/structured events, and last successful update/integrity verification only when a trusted service supplies the value. Never show a green “protected” state merely because the page loaded.

### Application permissions
For each application identity, display granted capabilities and their source:
- Files and directories.
- Network access.
- Devices, including camera/microphone only where supported.
- Process/IPC access.
- Sensitive system operations.

Allow inspect/revoke or approval of pending requests only where the policy supports it. The trusted permission broker and kernel capability/handle enforcement must validate changes; a GUI preference alone must not grant access.

### Network and firewall
Display actual network-policy availability, effective rules, decisions, and related events. Show Not implemented until the network stack and enforcement hook exist. Do not expose a fake on/off switch before a real policy API can confirm the applied state.

### Updates and software trust
Display package signature/integrity status, update availability, verification failures, and rollback/recovery state from the package/update service. Updates must be verified before installation. The UI must not store signing keys or provider credentials.

### Devices
Display discovered devices and effective application/device grants supplied by the device permission broker. Grant/revoke actions must use the broker and verify revocation. Never infer protection merely from a device appearing in a list.

### Threats and quarantine
Display the existing trust states: TRUSTED, VERIFIED, UNKNOWN, SUSPICIOUS, and MALICIOUS. Show evidence, provenance, original location, quarantine status, and recovery options where available. UNKNOWN is not synonymous with malicious. Quarantine/restore must preserve metadata and be authorized; destructive deletion is not the default.

### Security event log
Provide filterable, bounded event views using the central structured logging / Reason Ledger interfaces. Events should include, when available: sequence and timestamp; severity/category/source/reason code; process/user identity; resource/action and decision; policy result and evidence reference.

Redact secrets, credentials, and sensitive file contents. Restrict log access. Export must use bounded buffers and must not let untrusted users erase authoritative records. Distinguish in-memory events from persistent logs and show when persistence is unavailable.

### Users and privileged actions
Show account/session identity and privilege level. For protected operations, present the exact requested action, target, privilege required, and consequences before authorization. The UI and AI must not grant themselves privilege.

## 3. Logical component flow

    Existing Desktop / Settings
              |
              v
    Security Control Center (unprivileged UI)
              |
       authenticated IPC / service API
              |
              v
    Security service / permission broker
       |          |             |
       v          v             v
    Policy     Reason Ledger   Update / file-security
       |
       v
    Kernel object/capability + syscall/device/VFS enforcement

The transport must follow the current shared user/kernel ABI and IPC contracts. Do not invent a direct GUI-to-kernel shortcut. Each operation returns an explicit result and reason code. A failed or unavailable service must be shown as unavailable, not silently treated as success.

## 4. Service/API contract requirements

Before controls become actionable, define versioned contracts in the existing public/shared interface area (include/abi/ and/or the established service contract location):
- get_security_status: component state, capability/version, last verified time, limitations.
- list_permissions: caller-authorized application/device grants and sources.
- request_permission_change: target identity, capability, action, authorization context.
- list_security_events: bounded query with filters and continuation/sequence handling.
- get_quarantine_records and request_quarantine_action: metadata-only listing plus separately authorized quarantine/restore actions.
- get_update_trust_status and request_update_action: verification state and explicit result.
- get_network_policy_status and policy mutation requests only after a real network enforcement API exists.

These are logical operations, not a claim that matching callable APIs already exist. Reuse existing contracts where present; avoid duplicate APIs. Validate caller, handle, length, enum, and requested transition at the trusted service/kernel boundary. Mutations should be idempotent where practical and return an auditable decision identifier.

## 5. Failure and security behavior

- Fail closed for privilege grants, package/update verification, quarantine release, and protected settings.
- Audit persistence failure must not turn an authorization denial into an allow. Record the failure in the bounded in-memory health/event ring where available.
- Keep the UI responsive if a service is absent; label the feature unavailable and provide safe diagnostics without secrets.
- Prevent untrusted applications from spoofing privileged prompts. Bind authorization UI to a trusted session/service identity.
- Never upload file contents to an external reputation provider without applicable explicit policy/consent. Prefer local verification and hash-only lookups where supported.
- Use bounded output, paging, and rate limits to avoid memory exhaustion or log flooding.
- Treat displayed names, paths, provider text, and event payloads as untrusted data.

## 6. Implementation stages

### Stage A — Contract and design
Map each page to an existing trusted service and ABI. Mark missing enforcement and service dependencies as blockers. Define event fields, permission semantics, failure states, and tests before UI work.

### Stage B — Read-only status and audit viewer
Reuse the existing desktop/Settings entry point. Display only real status and structured events from available interfaces. Add bounded queries, redaction, unavailable-state handling, and deterministic tests.

### Stage C — Permission management
Integrate with the existing capability/permission broker. Add pending requests, grant/revoke workflows, authorization prompts, and revocation tests. Do not expose mutations until enforcement is verified.

### Stage D — File trust, quarantine, devices, and updates
Connect to existing file-admission, quarantine, device-permission, and update contracts as each becomes available. Add failure-injection and recovery tests for every mutation.

### Stage E — Network policy and hardening
Integrate only after a real network stack and policy enforcement path exist. Run IPC/authentication, privilege-boundary, spoofed-prompt, log-redaction, fuzzing, and regression tests. Conduct independent security review before release approval.

## 7. Acceptance criteria

The Control Center is not complete merely because the screen renders. Each criterion requires implementation evidence and automated tests:
1. It uses the existing ProsenjitOS desktop and shared ABI; no parallel desktop architecture is introduced.
2. Every displayed state comes from a trusted service and identifies unavailable/unverified capabilities.
3. GUI and CUI requests produce consistent authorization decisions.
4. A denied or unauthorized request cannot be made successful by changing UI state or forging IPC input.
5. Permission revocation is enforced at the resource boundary and existing handles follow the defined revocation contract.
6. Security events are bounded, structured, privacy-filtered, and traceable to policy decisions.
7. Quarantine restore and update actions verify authorization and integrity before release/installation.
8. Service failure, log sink failure, malformed requests, and resource exhaustion do not fail open.
9. QEMU/integration/security regression checks pass for the exact commit under review.
10. Features without end-to-end enforcement remain marked not implemented/not verified.

## 8. Current status and next dependencies

This document specifies the integration target only. It does not claim that the graphical Control Center, user-mode security service, network firewall, package update UI, or all permission-management flows are implemented.

The repository already documents the Reason Ledger/security event foundation, file-security admission and quarantine policy, device permission broker, shared ABI, and existing desktop direction. The next code task is to inspect those actual interfaces and implement the first read-only status/audit slice against them, rather than building mock controls or duplicating security policy.
