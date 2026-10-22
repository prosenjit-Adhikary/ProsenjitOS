# Native Graphics & Hardware Stack — Part 2

Status: **contract/lifecycle foundation implemented; concrete hardware drivers remain partial or unverified.**

## Boundaries

- PCI configuration-space discovery remains read-only. The existing scanner inventories PCI BDF, vendor/device IDs, class/subclass and programming interface. The Part 2 classifier maps known PCI classes to existing device classes; unknown devices remain OTHER/unsupported.
- The driver registry remains authoritative for native driver identity and lifecycle. The bounded endpoint registry is an adapter contract for display, input and audio backends; it does not grant a process hardware access.
- `kernel/display.c` remains a backend contract plus synthetic lifecycle test. Its synthetic framebuffer address is test data, not a mapped or writable hardware framebuffer.
- Endpoint init failure invokes shutdown to unwind partial acquisition. Failed shutdown preserves the endpoint record so cleanup can be retried. Transfers are bounded and validated before backend dispatch.
- MMIO/IO-port/IRQ resource descriptors are validated metadata only. The current paging layer does not provide a safe general MMIO mapping API. DMA descriptors are explicitly rejected as unsupported; there is no DMA allocator, IOMMU isolation, or bus-master enablement.
- Keyboard/mouse input and audio have typed endpoint interfaces but no concrete PS/2, USB HID, HDA, AC97 or other physical backend yet. They must report unavailable rather than synthesize hardware support.
- Windows `.sys` and Linux `.ko` images are not native ProsenjitOS drivers. Only drivers built for the native ProsenjitOS driver ABI may be registered as native; unsupported foreign drivers must be reported as unsupported.
- Desktop/compositor/window-management policy stays in user space. Kernel APIs expose low-level device contracts only.

## Acceptance evidence

`make hw-stack-test` runs host-side tests for PCI class classification, malformed/overflowing resource descriptors, explicitly unsupported DMA, invalid device IDs, missing endpoints, duplicate registration, initialization-failure cleanup, failed shutdown/retry, status reporting and normal cleanup. Kernel boot also runs the same deterministic contract self-test and emits a milestone before existing display, PCI, storage and user-mode boot checks.

Host tests do not prove a physical graphics/input/audio device works. QEMU PCI inventory and existing boot milestones prove discovery/boot only; no real display modeset, keyboard/mouse event delivery, audio playback, DMA, or MMIO mapping is claimed.
