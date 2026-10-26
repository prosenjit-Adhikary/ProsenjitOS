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

## 9. Source audit and safe implementation gate (2026-10-26)

The following is a source-level interface audit, not a claim of runtime validation:

- `kernel/include/health.h` declares a bounded health-event interface (`PROS_HEALTH_EVENT_MAX`, `pros_health_snapshot`, `pros_health_get_event`, and `pros_health_export`). These are kernel C interfaces; they are **not** by themselves a user-mode IPC/API.
- `kernel/include/security_monitor.h` exposes process-security telemetry/state functions, and `kernel/security_monitor.c` records security decisions into the health-event system. The monitor uses a bounded process-state array. Do not expose this state directly to an untrusted UI.
- `kernel/include/security_quarantine.h` and `kernel/security_quarantine.c` provide a bounded in-memory metadata table and add/get/release functions. The table is not proof of a complete filesystem quarantine/restore workflow: the UI must not call `release` as if it restores a file.
- `kernel/include/device_broker.h` declares device grant/revoke/check operations. These are kernel-private headers under `kernel/include/`, not a public user-space ABI. A privileged UI must not call them directly.
- The project structure assigns public/shared user-kernel interfaces to `include/` and keeps private kernel interfaces under `kernel/include/`. The existing native user-mode and service/IPC path is still a prerequisite for any UI integration.

### Non-negotiable change-safety gates

1. Keep the current feature branch and draft PR isolated from `main`; do not merge automatically.
2. First implementation slice is **read-only** and must not edit kernel policy, syscall dispatch, memory management, scheduler, VFS/ProsenjitFS, boot code, or existing desktop files.
3. Do not add a new kernel syscall just to feed a settings page. First establish a versioned, caller-authenticated service/IPC contract using the project's public ABI conventions.
4. Until that contract exists, do not display invented live values. Mark those panels **Unavailable — no user-mode service contract yet**.
5. Any future read-only adapter must use bounded snapshots/paging, validate all lengths and caller identity, redact sensitive values, and never block or allocate unbounded memory in a kernel path.
6. Every implementation change must be a separate, reviewable commit with focused host-side regression tests plus both existing build/boot and security CI gates. Verify the exact head SHA; a green result on another commit does not count.
7. Stop and investigate on any unexpected diff in kernel-critical paths, any failed test, or any change to existing desktop/session contracts. Never “fix” a failure by weakening a test or bypassing a gate.
8. No permission grant/revoke, quarantine restore, package install/update, or network-policy toggle is enabled until its real enforcement path, authorization, audit event, and failure-injection tests exist.

### Immediate next step

Before writing UI code, map the repository's existing public ABI, syscall dispatch, user-copy validation, process startup, and IPC/service state. If there is no authenticated read-only route from user mode to the health-event snapshot, the safe deliverable is a tested contract/prototype outside the running kernel—not a direct UI-to-kernel call. Keep the existing desktop and ABI untouched.

## 10. Integration blocker confirmed by ABI audit

The current `include/abi/syscall.h` lists syscall IDs for ABI version, user-test completion, handle close/duplicate/transfer, and IPC send/receive. It does not define a security-status/event-query syscall, process spawn/exec, or display/input delivery syscall. The existing `docs/DESKTOP-IMPLEMENTATION-LOG.md` also records that the DML is a registry/preflight component rather than a session launcher, and that the userspace init/service manager and end-to-end display/input path remain prerequisites.

**Decision:** do not add a Security Control Center executable or kernel syscall in this change. That would create a UI with no supported session/service route, or tempt a shortcut across the security boundary. The next safe engineering milestone is to finish and test the existing user-mode/service/IPC prerequisites, then add a small versioned read-only security query contract with caller authorization and bounded output. Only after that contract has integration tests should a UI be connected through the existing desktop path.

## 11. AI/human continuation protocol

Use `docs/SECURITY-CONTROL-CENTER-WORKLOG.md` as the dated handoff record. At the end of every work session, update it with:
- current branch, PR, and exact head commit SHA;
- files changed and why;
- source paths inspected and confirmed facts versus assumptions;
- tests/CI actually run, with exact commit SHA and queued/passing/failing status;
- known blockers, safety boundaries, and the single next action.

For a new AI or contributor, read these in order:
1. `docs/SECURITY-CONTROL-CENTER-WORKLOG.md` (latest handoff and next action);
2. this document (architecture, source audit, and safety gates);
3. `docs/ROADMAP.md` (project-wide dependency order);
4. `docs/DESKTOP-IMPLEMENTATION-LOG.md` (desktop/session blockers and existing work).

Do not infer implementation from a plan or checklist. Re-check repository head, diff, and exact-head CI before acting. Continue on the feature branch unless the owner explicitly approves a different path. Never merge the draft PR, rewrite `main`, or make destructive changes as part of continuation.


## 10. Read-only IPC contract gate (design only)

The first implementation milestone is limited to read-only status and event queries. It must not add permission, quarantine, device, update, or network mutations.

### Transport and endpoint trust
- Use an existing IPC/object/handle path only after the trusted service launch path and endpoint provisioning are demonstrated in source and tests.
- IPC handle possession alone is not peer authentication. Define the trusted service identity, which process creates and owns the endpoint, how the client receives it, and whether handle duplication/transfer/inheritance is permitted.
- Grant only the minimum channel rights required. Reject wrong object types, stale handles, missing rights, quarantined callers, and denied policy decisions.
- Do not call kernel-private health/security APIs directly from the UI. Do not add a syscall solely to expose a Settings page.

### Versioned bounded messages
- Use fixed-size headers and explicitly versioned operation/status enums. Include protocol version, request ID, declared request/response size, and bounded payload length.
- Reject unknown versions/operations, inconsistent sizes, invalid enums, overflow, truncated messages, and requests larger than the IPC contract permits.
- Never transmit raw pointers, kernel addresses, or kernel-private structure layouts. Translate trusted service results into ABI-owned response structures.
- First-version operations are read-only. Unknown or mutation operations return an explicit unsupported/denied status.
- Responses must distinguish OK, INVALID_REQUEST, DENIED, UNAVAILABLE, NOT_IMPLEMENTED, BUFFER_TOO_SMALL, and INTERNAL_ERROR. A service failure must never be rendered as “protected” or as a verified empty event list.
- Event retrieval must be bounded and paginated by stable sequence identifiers. Report volatile-only storage and dropped events where known.

### Copy and failure semantics
- Validate and copy bounded request data before processing; validate response destination ranges before copy-out. Do not hold kernel locks while accessing user memory.
- Existing IPC receive currently dequeues before copy-out finishes. Treat copy-fault, retry, and message-consumption behavior as an explicit test gate; do not claim reliable retry semantics until proven.
- If the service is absent, restarting, unauthorized, or unable to return trusted data, show Unavailable/Not verified and preserve enforcement.

### Acceptance tests before a UI adapter
1. Malformed/truncated/oversized request, unsupported version/operation, invalid enum, inconsistent length and integer-overflow cases.
2. Wrong object type, stale handle, missing rights, quarantined/unauthorized caller, denied capability, unauthorized handle transfer/duplication.
3. Empty/full channel, service absent/restart, bounded timeout/failure, invalid user buffers and copy-in/copy-out faults.
4. Maximum event batch, pagination/sequence boundaries, dropped events and non-persistent logs.
5. Prove read-only operations cannot mutate permissions, quarantine, device grants, updates, or network policy.
6. Run host-side contract tests first; run QEMU integration tests only after a real service-launch and endpoint-provisioning path exists.

This section is a design contract, not an implementation claim. Keep PR #267 draft/unmerged until endpoint ownership, user-mode startup, protocol tests, review, and exact-head CI are verified.


## Implementation sequence and release gates (2026-10-26)

This sequence deliberately separates a safe first read-only view from enforcement controls. Do not implement later gates by bypassing an earlier one.

### Gate 0 — preserve and baseline
- Keep all work on `feature/security-control-center-design`; never commit directly to `main`.
- Record exact HEAD and workflow run IDs before source changes. Wait for a terminal build/boot result before another commit when the workflow's `cancel-in-progress` is enabled.
- Keep this PR documentation/design-only until the process launch and trusted IPC service path is designed and reviewable.

### Gate 1 — telemetry ABI and access policy
- Define a versioned fixed-width request/response contract in a shared ABI header. It must fit the existing IPC message limit or explicitly introduce a separately reviewed bounded transport; do not silently enlarge the kernel IPC ABI.
- Expose only read-only snapshot and paginated event retrieval initially. Include protocol version, operation, request ID, byte lengths, stable event sequence cursor, dropped-event count and an explicit unavailable/not-implemented status.
- Validate caller identity and READ capability at the service boundary. A possession of an IPC handle alone is not proof that the peer is the trusted security broker.
- Keep kernel-private structs out of the user ABI. Translate fields explicitly and zero/initialize output buffers.

### Gate 2 — trusted service lifecycle
- Reuse and complete the native spawn contract in `docs/NATIVE-SPAWN-CONTRACT.md`; do not build an ad-hoc process launcher inside the UI or bypass execution admission.
- Add a real service identity and endpoint provisioning mechanism only after process publication, initial CPL3 thread entry, cleanup/rollback and capability inheritance have tests.
- Provision the broker endpoint with least-privilege rights; do not grant ADMIN, unrestricted handle transfer, or arbitrary kernel telemetry access.
- Define restart/unavailable behavior and ensure a failed service cannot be reported as an empty-but-healthy security state.

### Gate 3 — kernel-backed integration tests
- Host-side tests: malformed version/opcode/length, integer overflow, undersized output, unknown cursor, empty/full IPC channel, stale handle, wrong object type, rights attenuation, unauthorized peer and service restart.
- Kernel/QEMU tests: trusted broker can read bounded snapshots/events; untrusted process cannot; copy-out failure behavior is documented; event pagination is monotonic; dropped-event counters are visible; service failure never changes enforcement.
- Do not claim delivery/retry guarantees while IPC receive dequeues before copy-out succeeds.

### Gate 4 — read-only UI adapter
- Build the Security Control Center UI only on top of the stable user-space ABI/client, not direct kernel calls.
- UI states must distinguish Verified, Unavailable, Not implemented and Stale; a missing service or failed query must never render as “No threats.”
- Add no permission, quarantine, device-grant or network-policy mutation controls in the first UI release.

### Gate 5 — reviewed enforcement controls (separate future scope)
- Any action control requires separate threat model, authenticated caller, authorization policy, confirmation UX, audit event, rollback/recovery and abuse tests.
- UI visibility is not authorization. Kernel/service policy remains authoritative.

### Completion definition
The feature is not complete until all of the following are true on one exact commit: the ABI/client and trusted service are implemented; real event retrieval is demonstrated end-to-end; UI failure states are tested; negative authorization tests pass; full build, ISO QEMU boot and native BIOS QEMU boot pass; the PR has review and is merged intentionally. Documentation or architecture checks alone do not satisfy this definition.


## Read-only telemetry wire proposal

A separate bounded IPC proposal lives in docs/SECURITY-CONTROL-CENTER-IPC-PROTOCOL.md. It specifies a 16-byte request header, 24-byte response header, read-only status/event operations, a 20-byte event record, explicit error states, and parser/authorization tests sized to the current 64-byte IPC limit. It is not implemented and does not authenticate peers; trusted broker identity and endpoint provisioning remain prerequisites.
