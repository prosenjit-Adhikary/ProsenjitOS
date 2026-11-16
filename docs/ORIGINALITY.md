# ProsenjitOS Originality and Compatibility Policy

## Purpose

ProsenjitOS is an independent operating-system project. It may learn from public standards, public documentation, academic work, open-source software, and observable behavior, but it must not reproduce proprietary implementation code or protected assets from another operating system.

## Architectural inspiration is not source-code copying

General concepts such as virtual memory, processes, threads, system calls, access control, capability handles, GUI shells, filesystems, networking, package managers, and compatibility layers are broad engineering concepts and are not, by themselves, a copy of an operating system.

The implementation, APIs, naming, code, data structures, assets, and product identity of ProsenjitOS should remain independently developed.

## Windows compatibility

The Windows compatibility goal concerns application APIs and observable behavior needed to run selected software.

It does not mean:

- copying Windows source code
- copying Windows system binaries
- copying Microsoft trademarks or artwork
- redistributing proprietary Windows components
- promising arbitrary Windows kernel-driver compatibility

Where appropriate, an existing open-source compatibility project may be used under its own license, with clear attribution and component boundaries.

## Linux/POSIX compatibility

Linux/POSIX compatibility will target public interfaces and application behavior. It does not require copying the Linux kernel implementation.

Linux software distributed under its own open-source license remains subject to that license. ProsenjitOS will not remove or bypass those obligations.

## Public standards

The project may implement public technical standards and specifications where legally appropriate, including UEFI interfaces, ELF, POSIX-style APIs, standard network protocols, and standard storage protocols.

Implementations will be written independently.

## Branding

ProsenjitOS will use its own name, logos, icons, UI artwork, system sounds, package branding, and default themes.

The UI may be familiar or conventional where usability requires it, but it should not intentionally reproduce another vendor's protected visual identity.

## Code provenance

For substantial imported or adapted open-source components, the repository should record:

- upstream project
- exact version/commit
- license
- local modifications
- directory/component boundary

For original code, contributors should avoid copying implementation code from proprietary operating-system sources.

## Practical rule

When there is doubt, choose the independent implementation or a properly licensed open-source component instead of copying proprietary material.


## License inventory

The root MIT license applies only to ProsenjitOS-original material as described
in the repository README. Third-party components retain their own license terms
and notices. Maintain the repository-wide inventory in
[`THIRD-PARTY-NOTICES.md`](../THIRD-PARTY-NOTICES.md); do not treat architecture
references such as Wine or ReactOS as permission to copy their source.
