# External filesystem provider boundary

## Current architecture

Kernel-core contains only the native ProsenjitFS implementation.

Compatibility filesystem parsers for FAT32, exFAT, and NTFS are intentionally outside the kernel image. The VFS exposes filesystem-neutral vnode/file contracts; filesystem-specific parsing and metadata validation belong to the provider implementation.

The repository currently has no executable user-space provider runtime. The existing user-mode tree explicitly states that user-mode programs/services are introduced after process isolation and system calls, while the current syscall ABI exposes only handle and small IPC operations. Therefore this commit defines the stable provider wire ABI but does **not** claim runtime provider execution.

## Provider ABI

`include/abi/fs_provider.h` is the format-neutral boundary shared by the kernel adapter and future external providers.

It defines:

- provider identity and ABI version
- capability advertisement
- deterministic operation numbers
- common provider status values
- fixed-width request/response messages
- no kernel pointers
- no vnode layouts
- no filesystem-specific metadata structures

The operation set covers the VFS contract: `probe`, `mount`, `unmount`, `lookup`, `readdir`, `create`, `mkdir`, `unlink`, `rmdir`, `rename`, `read`, `write`, `stat`, `sync`, and `release`.

A provider must advertise only operations it actually implements.

## Addressing contract

The provider protocol does not redefine block addressing.

A provider must consume a bounded block-device capability/view and preserve this chain:

filesystem-relative unit
-> volume-relative sector/LBA
-> partition-relative LBA
-> parent-device LBA
-> physical device request

Cluster/VCN/LCN values are filesystem metadata, not physical LBAs. All multiplication, addition, range conversion, and signed-delta operations must be checked before a device request is issued.

## Runtime blocker

Runtime external-provider execution is not yet possible from the current HEAD without adding a real user-space service/process execution path.

The current repository has:

- process/address-space primitives
- capability/handle management
- a small IPC mechanism
- a syscall dispatcher

but no completed user-space executable loader/service supervisor and no capability-backed block-device/shared-buffer interface suitable for filesystem providers.

Consequently, FAT32/exFAT/NTFS must **not** be linked into kernel-core as a workaround, and no provider implementation is marked runtime-complete until that missing infrastructure exists.

## Required next runtime layer

The correct next dependency is:

user-space process loading
-> provider service lifecycle
-> capability-scoped block-device/partition access
-> bounded shared-memory or buffer transport
-> provider request/response transport
-> kernel VFS provider adapter
-> external FAT32/exFAT/NTFS implementations

The provider service must receive only the storage capability/view granted to it. It must not receive unrestricted physical-device access.

## Verification rule

Compilation of the ABI header is not filesystem-provider verification.

A provider can be marked implemented only after real image tests demonstrate probing, mounting, VFS operations, persistence/remount behavior, malformed-media rejection, LBA boundary safety, and the provider-specific crash-consistency guarantees documented for that filesystem.

Until the runtime layer exists, the honest status is:

- Native ProsenjitFS: kernel-native path
- FAT32: external-provider target; runtime unavailable
- exFAT: external-provider target; runtime unavailable
- NTFS: external-provider target; runtime unavailable
- Generic provider wire ABI: implemented
- Runtime VFS/provider adapter: blocked by process/service/device-capability infrastructure


## Kernel-side integration now present

The kernel contains a bounded provider registry and a transport adapter in `kernel/fs_provider.c`. The VFS calls deterministic provider discovery through this registry, and storage discovery can retain the selected provider identity. These components contain no FAT32, exFAT, or NTFS parser logic.

The transport context is responsible for supplying a capability-scoped device reference. A raw `struct pros_block_device *` is never serialized into the external ABI. This prevents a future user-space provider from receiving an arbitrary kernel pointer.

The registry self-test verifies registration, duplicate/provider lookup behavior, request/response correlation, mount identity validation, unsupported-provider handling, and unregister behavior. It is an adapter test only; it is not evidence that a real FAT32/exFAT/NTFS provider is executable.
