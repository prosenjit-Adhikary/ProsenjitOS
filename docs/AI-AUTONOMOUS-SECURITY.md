# ProsenjitOS Autonomous AI Security Policy

## Purpose

ProsenjitOS is designed to protect users who may not have the technical knowledge required to identify or respond to security threats manually.

For this reason, security decisions must not depend on human intervention when the operating system has enough evidence to require containment.

This document defines the intended autonomous-security boundary for unknown, suspicious, quarantined, and critical processes.

## Core Policy

> **Unknown or quarantine-required activity is handled automatically by the security system. Human approval is not required for containment.**

The security architecture is:

```
Process / File / Memory / Network / Persistence Telemetry
                         |
                         v
              Deterministic Security Engine
                         |
                         v
                  AI Security Analysis
                         |
                         v
                 Security Policy Engine
                         |
          +--------------+---------------+
          |              |               |
        ALLOW         QUARANTINE      BLOCK/STOP
          |              |               |
          +--------------+---------------+
                         |
                         v
             Evidence / Recovery / Verify
```

## Autonomous Handling Classes

### 1. Trusted / Known-Safe Process

A process that satisfies the configured trust and security policy may continue normally.

The system must still monitor behavior because a previously trusted process can become compromised.

### 2. Unknown Process

An unknown process is not automatically considered safe.

The security system collects available telemetry such as:

- process identity and parent/child relationship
- executable provenance and identity
- capability and privilege state
- security-policy changes
- filesystem activity
- persistence-related changes
- network activity
- suspicious process interactions
- security-monitor tampering
- behavior over time

AI analysis can classify the activity and recommend an enforcement action.

**No human approval is required before protective containment.**

### 3. Quarantine Candidate

When deterministic policy or AI analysis identifies activity requiring isolation, the system automatically moves the process into the quarantine path.

The objective is to prevent further damage while preserving evidence for later analysis.

Quarantine handling may include:

- stopping or suspending dangerous execution
- restricting capabilities
- restricting protected-resource access
- isolating network activity where supported
- preserving security telemetry
- preventing persistence
- triggering filesystem recovery workflows where applicable

### 4. Critical Threat

Critical threats are interrupted immediately.

Examples include:

- process injection
- security-monitor tampering
- confirmed destructive malware behavior
- ransomware-like mass file modification
- credential-access behavior combined with other malicious signals
- privilege escalation combined with persistence or malicious activity
- confirmed malicious execution

**Critical containment must not wait for a user to understand the threat.**

## AI Authority Boundary

AI is allowed to initiate a security intervention, but it must not receive unrestricted kernel execution privileges.

The safe boundary is:

```
AI
 |
 | structured security decision
 v
Security Policy API
 |
 +-- allow
 +-- monitor
 +-- quarantine
 +-- block
 +-- isolate
 +-- revoke capability
 +-- protect resource
 |
 v
Kernel Enforcement
```

The kernel remains the enforcement authority.

Therefore:

- AI cannot execute arbitrary kernel code.
- AI cannot modify kernel memory directly.
- AI cannot bypass mandatory security policy.
- AI cannot grant itself capabilities.
- AI cannot disable the security monitor.
- AI decisions are constrained to explicitly supported security actions.

This provides autonomous protection without giving an AI model unrestricted kernel control.

## Deterministic Fallback

AI availability must never be a single point of failure.

If AI analysis is unavailable, deterministic security controls continue to enforce known critical rules.

For example:

```
Known critical malicious behavior
        -> deterministic detection
        -> immediate containment
```

AI therefore improves detection and analysis, but the OS remains capable of enforcing fundamental security policy independently.

## False-Positive Protection

Autonomous security must avoid unnecessary termination of legitimate software.

Before escalation, the security engine should consider multiple signals instead of relying on a single filename or event.

Relevant context can include:

- process ancestry
- executable provenance
- capabilities
- security policy
- resource sensitivity
- event frequency
- event sequence
- temporal correlation
- network destination/reputation where available
- known-safe behavior
- repeated suspicious behavior

High-confidence critical behavior can bypass normal escalation delays.

Lower-confidence unknown behavior should normally be observed, restricted, or quarantined rather than immediately destroyed.

## Security Incident Evidence

Every autonomous intervention should produce structured evidence sufficient for later investigation.

An incident record should contain, where available:

- timestamp
- process ID
- parent process ID
- process identity
- triggering events
- correlated behavior
- risk/severity
- AI assessment
- deterministic policy result
- enforcement action
- affected resources
- recovery result

Evidence collection must not weaken the containment boundary.

## Recovery and Self-Healing

Security containment and filesystem self-healing are separate subsystems.

When malicious activity damages protected data, the security layer may request recovery through the filesystem recovery subsystem.

The recovery path is:

```
Threat detected
     |
     v
Contain activity
     |
     v
Preserve evidence
     |
     v
Identify affected resources
     |
     v
Request safe recovery
     |
     v
Verify recovered state
```

Recovery must never blindly restore corrupted or untrusted data.

## Human Role

Human intervention remains useful for:

- reviewing incidents
- changing security policy
- investigating false positives
- approving exceptional administrative actions
- forensic analysis
- system recovery when automated recovery is impossible

However:

> **Human intervention is not a prerequisite for protecting the machine.**

The default security objective is that an ordinary user can remain protected even without understanding the underlying attack.

## Security Design Principle

ProsenjitOS follows this principle:

> **Detect automatically, reason automatically, contain automatically, recover safely, verify continuously.**

AI is therefore an autonomous security-analysis component, not an unrestricted kernel administrator.


## Vendor-Inspired Security Research Baseline

The autonomous-security design has been reviewed against publicly documented security approaches from major endpoint/network-security vendors. The goal is to adopt the **security principles and control patterns**, not to copy proprietary implementations.

### Microsoft Defender / Defender for Endpoint

Microsoft documents a layered model that combines:

- real-time process, file, and service behavior monitoring
- behavior-based detection for threats that do not match known signatures
- process-tree context and behavioral blocking
- cloud/ML-assisted classification
- automatic containment and remediation
- network protection
- attack-surface reduction
- tamper protection for security controls
- automatic attack disruption for active attacks

An important architectural lesson is that security decisions should be based on **behavior and process context**, not only on file hashes or signatures. Defender also throttles repetitive telemetry so high-rate activity does not overwhelm the detection pipeline.

### Palo Alto Networks Cortex XDR / WildFire

Cortex XDR documentation demonstrates a layered treatment of unknown files and processes:

- determine known/unknown reputation
- perform endpoint/local analysis for unknown artifacts
- obtain deeper cloud/sandbox analysis where available
- continue observing runtime behavior
- correlate process activity and malicious behavior
- block/quarantine when evidence crosses the configured prevention boundary

The architectural lesson for ProsenjitOS is that **unknown must mean “needs analysis,” not “trusted” and not automatically “malicious.”**

### Fortinet / FortiGate

Fortinet documents application-aware and heuristic controls that can identify applications and suspicious behavior beyond simple port-based or signature-only matching.

The architectural lesson is to combine:

- identity
- protocol/application context
- behavioral/heuristic evidence
- policy
- risk level

rather than making a decision from one indicator.

### Cisco Secure Endpoint

Cisco's endpoint architecture uses multiple detection engines and telemetry sources rather than relying on a single detection mechanism.

The architectural lesson is to preserve **multiple independent detection paths** and make the final enforcement decision through a common policy layer.


### CrowdStrike Falcon / Malware Analysis

Public CrowdStrike documentation reinforces several additional design patterns relevant to ProsenjitOS:

- suspicious files and behaviors can be automatically submitted for deeper malware analysis
- behavioral analysis should capture filesystem and network interactions, not only static file properties
- unknown threats benefit from controlled analysis and enriched behavioral evidence
- malware analysis can produce actionable indicators and support automated defensive workflows

The architectural lesson for ProsenjitOS is to treat **unknown activity as an evidence-generation problem**: observe it safely, correlate its behavior, increase confidence when independent signals agree, and only then apply the narrowest sufficient containment action.

### Additional Cross-Vendor Design Refinements

The latest public documentation also reinforces several details that should be explicit in the ProsenjitOS design:

1. **Signal correlation over single-indicator blocking** — high-confidence automated containment should be based on multiple related signals whenever practical.
2. **Continuous runtime observation** — a process that was trusted at launch must remain observable because its behavior can change after execution begins.
3. **Attack-story / causality context** — security records should preserve enough process and event relationships to reconstruct how the activity developed.
4. **Bounded telemetry** — repeated identical events must be throttled or aggregated so the security subsystem cannot be exhausted by high-volume activity.
5. **Tamper resistance** — security settings and enforcement paths must not be silently disabled by the process being monitored.
6. **Automatic containment** — sufficiently high-confidence active attacks should be containable without waiting for human approval.
7. **Controlled analysis of unknowns** — unknown does not equal malicious, but unknown activity should receive enhanced monitoring and progressively stronger controls when suspicious behavior accumulates.

These principles are consistent with public Microsoft and CrowdStrike documentation on behavior monitoring, automated containment, tamper protection, telemetry throttling, and behavioral malware analysis. citeturn0search0turn0search2turn0search4turn0search9turn0search1

### Trend Micro

Trend Micro's documented behavior-monitoring and ransomware-protection approaches emphasize sequences of suspicious operations over time, such as repeated destructive file activity.

The architectural lesson is that ransomware and similar threats are often better detected as a **behavioral sequence** than as a single event.

### Cross-Vendor Conclusions

Across these documented approaches, the common production-grade pattern is:

```
Identity / Reputation
        +
Static / Local Analysis
        +
Runtime Behavior
        +
Process Tree / Causality
        +
File / Persistence / Credential Activity
        +
Network Context
        +
Temporal Correlation
        +
Policy / Resource Sensitivity
        |
        v
Risk + Confidence
        |
        +--> Allow / Monitor
        +--> Restrict
        +--> Quarantine / Isolate
        +--> Block / Stop
        +--> Protect / Recover
```

This becomes the ProsenjitOS target architecture.

## Known vs Unknown Decision Policy

Unknown activity is handled with graduated containment rather than a single binary rule.

| Classification | Typical OS action |
|---|---|
| Known + normal behavior | Allow + monitor |
| Known + abnormal behavior | Monitor/restrict and correlate |
| Known + confirmed malicious behavior | Block/quarantine |
| Unknown + low-risk behavior | Allow with enhanced monitoring |
| Unknown + suspicious behavior | Restrict/quarantine while analysis continues |
| Unknown + malicious behavior | Block/quarantine immediately |
| Critical behavior | Immediate containment without waiting for human approval |

A trusted executable is **not permanently trusted**: its runtime behavior, ancestry, capabilities, resource access, and security-policy interactions remain observable.

## Process-Causality Model

The security engine must evolve from an isolated event score toward a process-causality model.

Example:

```
User application
   |
   +--> script/interpreter
          |
          +--> unknown child process
                  |
                  +--> credential access
                  |
                  +--> persistence change
                  |
                  +--> suspicious network connection
                  |
                  +--> mass file modification
```

Each individual event may have a legitimate explanation. The complete causal chain can cross a much higher-confidence malicious threshold.

The target implementation therefore correlates:

- PID / parent PID
- executable identity
- process ancestry
- event ordering
- time window
- capabilities/privileges
- resource sensitivity
- filesystem targets
- persistence targets
- network endpoints
- security-control tampering
- repeated behavior

## Autonomous Containment Policy

Containment must be executable without human approval when the evidence is sufficiently strong.

Supported containment primitives should remain narrow and kernel-enforced:

- allow
- monitor
- restrict
- quarantine
- terminate/block
- isolate network access
- revoke selected capabilities
- protect selected resources
- request safe recovery

AI may select or request one of these actions, but **AI never receives arbitrary kernel execution authority**.

The kernel must validate:

1. the requesting security principal
2. the target process/resource
3. the requested action
4. mandatory policy
5. protected-process/resource exceptions
6. action safety and idempotence

## Tamper-Resistant Security Boundary

The security subsystem itself is a protected resource.

Attempts to:

- disable the monitor
- modify security policy without authorization
- tamper with security state
- terminate or impair the security subsystem
- bypass mandatory enforcement

must generate high-severity security events and must not be allowed to silently disable protection.

The design follows the same general principle documented by commercial endpoint products: security controls must protect themselves against tampering.

## Telemetry and Event Correlation

The target telemetry model is:

### Process

- creation/exit
- parent/child relationship
- executable identity/provenance
- privilege/capability changes
- suspicious cross-process operations

### Filesystem

- open/create
- write
- rename
- unlink/delete
- permission/security changes
- protected-resource access
- high-rate modification patterns

### Persistence

- service/startup configuration changes
- boot/session persistence
- security-policy modifications

### Network

- connection attempts
- endpoint identity
- unusual destination/context where available
- suspicious process-to-network relationships

### Security Control

- monitor tampering
- policy bypass attempts
- unauthorized capability changes

The implementation should use bounded buffers, event throttling, temporal windows, and aggregation so high-volume benign activity cannot exhaust security resources.

## Detection Pipeline

The target pipeline is:

```
Event producer
     |
     v
Normalization
     |
     v
Rate limiting / deduplication
     |
     v
Process + resource correlation
     |
     v
Temporal / causal analysis
     |
     v
Deterministic rules
     |
     +------> immediate critical containment
     |
     v
Risk + confidence assessment
     |
     v
AI analysis (when available)
     |
     v
Security Policy Engine
     |
     +--> allow / monitor
     +--> restrict
     +--> quarantine / isolate
     +--> block / terminate
     +--> protect / recover
```

Deterministic critical rules remain active even when AI is unavailable.

## False-Positive and Safety Controls

Autonomous security must be conservative about irreversible actions while remaining decisive against high-confidence destructive behavior.

The target policy is:

- do not block solely because a file is unknown
- do not trust solely because a hash is known
- prefer multiple independent signals
- increase confidence when suspicious events form a causal chain
- increase severity for protected-resource impact
- use temporal correlation for burst attacks
- preserve evidence before destructive containment where technically safe
- make containment actions idempotent
- prevent the security subsystem from being recursively disabled
- maintain deterministic fallback rules

Critical signals such as security-monitor tampering or confirmed destructive behavior may bypass normal escalation delays.

## Recovery / Self-Healing Boundary

Security containment and filesystem recovery remain separate subsystems.

Security may identify the affected process and resources and request recovery. The filesystem layer must independently validate journal/snapshot/COW state, transaction integrity, and recovery safety before restoring data.

The security layer must **never assume that recovery succeeded**. Recovery must return a verifiable result, followed by post-recovery integrity checks.

## Implementation Roadmap Derived from Research

The research is now converted into an implementation plan:

1. **Telemetry foundation** — complete process, filesystem, persistence, network, and security-control event producers.
2. **Event normalization and throttling** — bounded queues, deduplication, rate limits, and loss accounting.
3. **Process-causality graph** — parent/child/process-chain tracking.
4. **Behavioral correlation** — ransomware-like sequences, injection, credential access, privilege escalation, persistence, C2/network combinations, and tampering.
5. **Reputation and unknown handling** — known/unknown identity with runtime re-evaluation.
6. **Policy engine** — allow/monitor/restrict/quarantine/block/isolate/capability-revoke/protect-resource decisions.
7. **Protected-process/resource enforcement** — prevent unsafe containment of critical kernel/system components.
8. **Incident evidence** — bounded incident records with causal sequence and enforcement reason.
9. **AI userspace interface** — structured telemetry in; constrained security decisions out.
10. **Recovery integration** — safe request/verify interface to filesystem recovery.
11. **Adversarial testing** — synthetic ransomware, injection, persistence, privilege escalation, security tampering, C2-like activity, false-positive workloads, and AI-unavailable fallback.
12. **Local verification** — Debian build/tests and QEMU security regression using the user's local/self-hosted environment; do not make GitHub-hosted Actions a prerequisite.

## Research References

The design review is based on public vendor documentation, including:

- Microsoft Learn — Behavior monitoring in Microsoft Defender Antivirus
- Microsoft Learn — Behavioral blocking and containment
- Microsoft Learn — Endpoint detection and response
- Microsoft Learn — Automatic attack disruption
- Microsoft Learn — Tamper protection
- Palo Alto Networks — Cortex XDR / WildFire documentation
- Fortinet Documentation — FortiGate application/heuristic security controls
- Cisco Secure Endpoint documentation
- Trend Micro documentation on behavior monitoring and ransomware protection

These references are used as architectural input only. ProsenjitOS remains an independent implementation and must not claim feature parity with any commercial product until the corresponding functionality is actually implemented and locally verified.

## Implementation Status

This document defines the target security policy for the autonomous AI security architecture and records the public research baseline used to guide implementation.

The existing deterministic security monitor provides the kernel-side enforcement foundation. Full production implementation still requires complete telemetry producers, robust behavioral correlation, AI userspace integration, quarantine/isolation enforcement, evidence management, recovery integration, and end-to-end adversarial testing.

No feature should be described as production-complete until it has passed local build, boot, security, regression, containment, recovery, and failure-mode verification.


## Long-Term Engineering Specification

This section is intentionally detailed so future kernel/security agents can use this document as an implementation contract instead of relying on historical chat context.

### Security Objectives

The long-term objective is a layered, autonomous endpoint-defense architecture for ProsenjitOS. It must:

- detect suspicious behavior before damage when technically possible
- detect attacks that are not represented by known hashes/signatures
- correlate events across processes, files, persistence, privileges, memory interactions, and networking
- contain sufficiently high-confidence attacks without requiring a human decision
- preserve evidence before destructive actions whenever safe
- prevent the security subsystem itself from being disabled or bypassed
- remain functional when AI analysis is unavailable
- keep containment actions narrow, auditable, idempotent, and kernel-enforced
- integrate with filesystem recovery without allowing the security layer to blindly restore data
- remain bounded in CPU, memory, queue depth, and storage consumption

This is a design target, not a claim that every capability is already implemented.

### Detection Layers

Future implementations should keep detection layers independent so that one failed or unavailable component does not disable protection.

#### Layer A — Identity and Provenance

Record, where available:

- executable identity
- immutable file/object identity
- cryptographic digest
- signer/provenance information when the platform supports it
- parent process
- loader/source information
- user/security principal
- capabilities
- launch context

Identity is evidence, not permanent trust.

#### Layer B — Pre-Execution Admission

Before execution of a high-risk or unknown object, the admission path should be able to evaluate:

- policy
- object identity/reputation
- source/provenance
- requested privileges
- target user/context
- protected-resource access requirements
- known malicious indicators

The admission layer must fail safely and must never become a bypass around runtime monitoring.

#### Layer C — Runtime Behavior

Runtime telemetry should cover, as supported by the kernel:

- process creation/exit
- parent-child relationships
- executable/image changes
- privilege/capability changes
- cross-process operations
- memory-sensitive operations
- file creation/write/rename/unlink
- permission/security changes
- persistence changes
- network connection attempts
- security-policy changes
- security-monitor tampering

Each producer must use bounded, non-blocking or carefully bounded paths appropriate for kernel context.

#### Layer D — Sequence and Causality

Do not treat every event as an independent score.

Maintain a bounded causal history so the engine can recognize sequences such as:

1. unknown process starts
2. interpreter/script child appears
3. privilege changes
4. persistence is created
5. suspicious network activity begins
6. protected files are modified
7. many files are renamed/encrypted/deleted

The combined incident should carry more confidence than the sum of isolated unrelated events.

#### Layer E — Resource Sensitivity

The same action should have different risk depending on the target.

Examples:

- ordinary user cache → lower sensitivity
- user document → medium sensitivity
- boot configuration/security policy → high sensitivity
- security monitor state → critical sensitivity
- filesystem metadata/journal structures → critical integrity sensitivity

Resource sensitivity must be explicit rather than hidden inside ad-hoc scoring.

### Risk and Confidence Model

Future policy should separate risk, confidence, and impact.

Conceptually: risk = likelihood × confidence × impact.

The exact production formula must be validated experimentally; it must not be copied blindly from this document.

Confidence should increase when:

- independent telemetry sources agree
- suspicious events occur in a coherent process chain
- events occur within a relevant time window
- protected resources are targeted
- behavior matches a known attack pattern
- multiple stages of an attack are observed

Confidence should decrease when:

- the behavior is consistent with established trusted workflows
- the event is isolated and low impact
- the process has legitimate provenance and expected ancestry
- the activity falls within a documented administrative operation

A single weak indicator should normally not cause irreversible containment.

### Severity Classes

Define stable semantic classes rather than scattering numeric constants through the kernel:

- INFO — informational/low risk
- LOW — suspicious but low impact
- MEDIUM — multiple suspicious signals
- HIGH — strong malicious evidence or protected-resource targeting
- CRITICAL — active destructive attack, security tampering, or equivalent immediate danger

Numeric weights may change during tuning; semantic meaning must remain stable.

### Action Classes

Every security action should be represented by a constrained action type:

- ALLOW
- MONITOR
- RESTRICT
- QUARANTINE
- BLOCK
- ISOLATE_NETWORK
- REVOKE_CAPABILITY
- PROTECT_RESOURCE
- REQUEST_RECOVERY

Actions must be authenticated/authorized by the security boundary, validated against target state, idempotent where possible, recorded in evidence, safe when repeated, and denied when mandatory policy forbids them.

### Protected Targets

The policy engine should maintain explicit protected-target classes.

At minimum:

- kernel/security-monitor code and state
- security policy state
- boot-critical metadata
- filesystem journal/recovery metadata
- identity/credential stores when present
- critical system services

A protected target must not be terminated, modified, or isolated using the same generic path as an ordinary user process without an explicit policy decision.

### Safe Containment Ordering

Where technically supported, prefer a reversible sequence:

observe → restrict → preserve evidence → isolate → quarantine → terminate

This is not a hard rule for critical attacks. If delay creates unacceptable damage, immediate blocking/termination is permitted.

Containment must not itself corrupt filesystem state, deadlock kernel subsystems, or create an attacker-controlled recovery path.

### Network Isolation

Network containment should eventually support policy at the narrowest useful scope:

- process
- connection
- endpoint
- interface
- whole system

Do not default to whole-system isolation when a narrower containment is sufficient.

Network isolation must preserve the minimum connectivity required for security telemetry/recovery only when explicitly allowed by policy.

### Ransomware / Mass-Modification Detection

Future ransomware protection should combine:

- write/rename/delete rate
- number of distinct files
- directory breadth
- extension/type changes where available
- entropy/content-change signals where technically feasible
- process ancestry
- user/session context
- protected-resource sensitivity
- time-window acceleration
- recovery/journal state

The detector should distinguish legitimate bulk operations such as backups, compilers, archive extraction, indexing, and filesystem maintenance from destructive encryption/deletion patterns.

Do not make a single file-write event equivalent to ransomware.

### Process Injection / Cross-Process Abuse

Future injection detection should correlate:

- source process identity
- target process identity
- privilege relationship
- unusual memory access
- executable-memory transitions where observable
- thread/process manipulation
- ancestry mismatch
- protected-target status

Security-sensitive cross-process operations should produce explicit telemetry even if the operation is ultimately allowed.

### Persistence Detection

Persistence monitoring should cover every supported persistence mechanism rather than assuming one platform-specific location.

Each persistence event should record:

- actor process
- target mechanism
- target object
- previous state
- new state
- privilege context
- whether the target is protected
- whether the change was expected/authorized

### Evidence and Incident Records

Incident records should be bounded and structured.

Recommended fields:

- incident ID
- monotonic sequence number
- timestamp
- PID/PPID
- process/object identity
- user/security principal
- severity
- confidence
- impact
- event type
- source subsystem
- target resource
- causal parent/related event
- policy decision
- action requested
- action actually enforced
- recovery request/result
- integrity/verification result

Evidence should survive enough of a containment operation to explain why the action occurred, but evidence storage must have strict resource limits.

### Queue, Rate-Limit, and Backpressure Rules

Security telemetry must never be allowed to consume unbounded kernel memory.

Future implementation should define:

- maximum queue depth
- maximum event size
- per-process rate limits
- global rate limits
- duplicate suppression
- aggregation windows
- overflow counters
- dropped-event accounting
- priority classes

Critical security events must have priority over routine telemetry.

If telemetry is dropped, the system should record that loss occurred. It must never silently report a complete security picture when data was actually lost.

### AI Interface Contract

AI should receive structured, minimized security context rather than arbitrary kernel memory.

Input should contain only the telemetry required for analysis.

AI output should be a typed security decision containing, conceptually:

- decision
- confidence
- reason code
- evidence references
- requested action
- optional expiry/duration
- policy compatibility result

The kernel/security policy layer must validate every field.

AI output must never be interpreted as executable kernel instructions.

If AI is unavailable, times out, returns malformed data, or conflicts with mandatory policy:

deterministic policy wins.

### AI Safety Rules

AI must never be able to:

- write arbitrary kernel memory
- execute arbitrary kernel code
- disable mandatory security
- remove its own audit trail
- grant itself capabilities
- modify immutable/protected security state
- bypass authentication/authorization
- directly manipulate filesystem recovery state

AI may only request explicitly supported security actions.

### False-Positive Control

False positives must be treated as an engineering metric.

Future tests should measure:

- benign workload interruption rate
- false quarantine rate
- false block rate
- detection latency
- containment latency
- recovery success
- telemetry loss
- CPU/memory overhead

Before enabling aggressive automatic actions broadly, the detector should be exercised against representative benign workloads.

Administrative operations, development builds, backups, indexing, package management, filesystem checks, and normal user bulk file operations should be represented in regression tests.

### Self-Protection / Tamper Response

The security subsystem should detect attempts to:

- stop/disable security monitoring
- alter mandatory policy
- overwrite security state
- bypass enforcement APIs
- spoof security telemetry
- flood event queues to cause denial of service
- manipulate timestamps or identity fields used for policy decisions

Tamper events should be high priority and should never be suppressible by the untrusted process that generated them.

### Recovery Integration Contract

Security recovery must be two-phase:

1. Request — security identifies affected resources and requests recovery.
2. Validate/Execute — the filesystem/recovery subsystem independently validates journal/COW/snapshot state, transaction integrity, ownership, and consistency before restoration.

After recovery:

restore → remount/reopen if required → verify metadata → verify data → verify journal state → record result

A successful recovery request is not equivalent to a successful recovery.

### Testing Matrix

Future security work must include at least:

| Test family | Required scenarios |
|---|---|
| Identity | known, unknown, changed executable identity |
| Process | normal tree, suspicious child, orphan/abnormal ancestry |
| Privilege | normal transition, unauthorized escalation |
| Injection | benign cross-process access, synthetic malicious injection |
| Filesystem | normal bulk write, rename burst, delete burst, protected-resource modification |
| Ransomware | simulated mass modification with temporal escalation |
| Persistence | authorized and unauthorized persistence |
| Network | normal connection, suspicious burst, process/network correlation |
| Tamper | disable attempt, policy modification, telemetry spoof/flood |
| AI | unavailable, timeout, malformed result, conflicting result |
| Containment | allow, restrict, quarantine, block, network isolation |
| Recovery | interrupted recovery, corrupted journal, successful recovery, failed verification |
| Resource exhaustion | queue saturation, event storm, memory pressure |
| Regression | normal OS boot, filesystem operations, ordinary userspace workloads |

Every new detector should add both positive and negative tests.

### Failure-Safety Requirements

Security code must not:

- panic the kernel because telemetry is malformed
- dereference untrusted event pointers without validation
- allocate unbounded memory
- deadlock while holding unrelated filesystem/process locks
- block indefinitely in a security callback
- terminate a protected critical process without an explicit safe policy path
- assume an AI response is valid
- assume filesystem recovery succeeded
- assume an event stream is complete after overflow

Security failures should fail closed for security decisions where safe, but should fail safely for kernel stability. The exact behavior must be specified per subsystem.

### Implementation Gates

A security feature should not be marked complete merely because it compiles.

Completion requires, as applicable:

1. source implementation
2. compile with warnings treated appropriately
3. unit/self-test coverage
4. integration test
5. negative/false-positive test
6. failure-path test
7. persistence/reboot test where state is persistent
8. QEMU/local runtime verification
9. evidence/log verification
10. documentation update

GitHub-hosted Actions must not be a required completion gate. The authoritative verification environment remains the local/self-hosted Debian/QEMU workflow.

### Versioning and Future-Agent Handoff

When this architecture changes, future agents should:

- inspect the current implementation before changing interfaces
- preserve existing ABI/API contracts unless migration is intentional
- update this document when a security invariant changes
- document every new enforcement primitive
- add regression tests for every security bug found
- never revive obsolete diagnostic assumptions from older kernel states
- distinguish implemented, partially implemented, planned, and experimentally validated features
- never claim commercial feature parity without local evidence

The source tree and tests are the implementation authority; this document is the architectural contract and handoff record.

### Current Implementation Boundary

The deterministic security monitor currently represents the foundation of the kernel-side enforcement path. It must be treated as Layer 1 protection, not as the completed commercial-style security system.

Future work should build upward in controlled stages:

telemetry → normalization → causality → behavioral detection → risk/confidence → policy → containment → evidence → AI analysis → recovery → verification

No stage should silently bypass the previous security boundary.
