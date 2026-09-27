# ProsenjitOS Security Model

## Security objective

ProsenjitOS is designed around **least privilege, isolation, explicit authority, and recovery**.

The target is not an unrealistic promise of being impossible to hack. The target is to reduce attack surface, prevent one application from freely reaching another application's data, contain compromised software, make privileged operations auditable, and recover safely.

## Trust boundaries

The primary boundaries are:

- firmware -> boot environment
- boot environment -> kernel
- kernel -> user mode
- process -> process
- application -> brokered resource
- user -> privileged administration
- compatibility runtime -> host resources
- driver -> kernel

Crossing a boundary requires a defined mechanism. An application must not obtain authority merely because it knows a pathname, process ID, device name, or IPC endpoint.

## Application identity

Each application/process will have a security identity containing:

- process/application identifier
- user identity
- package identity
- integrity/trust state
- granted capabilities
- sandbox profile
- resource limits
- audit context

## Capabilities and handles

The kernel will expose resources through controlled handles/capabilities rather than unrestricted kernel object access.

Examples: file handles, directory handles, process handles, shared-memory handles, IPC endpoints, device handles, and network sockets.

A process that does not possess an appropriate capability cannot simply manufacture one by guessing an identifier.

## Memory isolation

User processes will receive separate virtual address spaces.

Planned protections include:

- user/kernel address separation
- page-level read/write/execute permissions
- non-executable memory where supported
- guarded kernel mappings
- controlled shared-memory regions
- no direct access to another process's private pages

Shared memory will be explicit and capability-controlled.

## IPC isolation

IPC endpoints will have ownership and access policy.

The security model will support endpoint identity, sender/receiver authorization, capability transfer, message-size/resource limits, and audit events for sensitive IPC.

GUI services, shell services, compatibility runtimes and ordinary applications will use the same underlying IPC security model.

## Filesystem isolation

Applications will not automatically receive access to every user's files.

The runtime model should support private application data, shared application data with explicit grants, user-selected file access, protected system locations, read-only system components where practical, per-application writable areas, and permission-controlled removable storage access.

## Device isolation

Applications will not directly program hardware. Sensitive devices will be accessed through kernel drivers and brokered interfaces.

Examples include storage, camera, microphone, keyboard, display, USB devices, and network adapters.

## Process isolation

An application should not be able to:

- read another application's private memory
- inject code into another application without explicit privileged authorization
- terminate another application without appropriate authority
- inspect arbitrary process state without permission
- obtain privileged kernel handles by guessing identifiers

Debugging and administration will be separate privileged capabilities.

## Network security

The networking stack will eventually support policy based on application identity, user identity, destination, protocol, port, and connection direction.

A compatibility application will therefore not automatically receive unrestricted network access.

## Executable and package integrity

The package system is planned to support package manifests, hashes, publisher identity, digital signatures, dependency metadata, version metadata, revocation/blocking information, and safe update/rollback.

Unsigned software may exist in a development mode, but production policy should be able to restrict it.

## Driver security

Kernel-mode drivers are part of the trusted computing base.

The design therefore plans signed-driver policy, controlled loading, version/dependency metadata, crash containment where practical, separation between user-mode drivers/services and kernel drivers, and audit records for driver installation/loading.

Windows/Linux kernel drivers will not be treated as automatically compatible with ProsenjitOS.

## Compatibility runtimes

Windows and Linux application compatibility will be treated as **untrusted user-space workloads**.

The compatibility runtime will sit behind the ProsenjitOS security boundary:

    Windows/Linux application
              |
      Compatibility runtime
              |
      ProsenjitOS security broker
              |
        Kernel capabilities
              |
            Hardware

Compatibility must never become an excuse to bypass the native security model.

## Malware containment

A malicious application may still exploit an undiscovered vulnerability. The architecture therefore uses defense in depth:

- sandboxing
- least privilege
- memory isolation
- syscall policy
- capability restrictions
- package verification
- driver controls
- network policy
- logging/auditing
- updates
- recovery/rollback

The intended result is containment and recoverability, not a claim of perfect prevention.

## Secure boot and measured trust

Later milestones will investigate secure/verified boot.

The boot chain should eventually verify trusted components before transferring control to the next stage. Firmware boot services are a platform interface; after the OS loader has prepared its environment, it takes ownership of the platform.

## Security policy engine

A future security broker will make decisions using identity, capability, requested operation, resource, user policy, application manifest, and runtime sandbox policy.

Example:

    App A requests: read Documents/report.docx
                 |
                 v
          Security Broker
          - App identity
          - Capability?
          - User grant?
          - Sandbox policy?
                 |
            +----+----+
            |         |
          ALLOW      DENY
            |         |
          handle   audit event

## GUI and CUI security

GUI and CUI are presentation layers, not separate security domains.

A GUI application and a command-line application must be subject to the same kernel capabilities, permission broker, identity model and audit system.

## Recovery

Security includes recovery.

The platform should eventually provide transactional updates, rollback, protected system components, a rescue environment, configuration recovery, diagnostic boot, and security event history.

## Design rule

**No compatibility feature may silently weaken the native security boundary.**
