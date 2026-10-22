# Native Graphics & Hardware Stack — Part 2

Status: **contract/lifecycle foundation implemented; concrete hardware drivers remain partial or unverified.**

## Boundaries

- PCI configuration-space discovery remains read-only. The existing scanner inventories PCI BDF, vendor/device IDs, class/subclass and programming interface. The Part 2 classifier is exercised on each discovered PCI record and boot logs count known-class versus unsupported/unknown-class devices; unknown devices remain OTHER/unsupported.
- The driver registry remains authoritative for native driver identity and lifecycle. The bounded endpoint registry is an adapter contract for display, input and audio backends; it does not grant a process hardware access.
- `kernel/display.c` remains a backend contract plus synthetic lifecycle test. Its synthetic framebuffer address is test data, not a mapped or writable hardware framebuffer.
- Endpoint init failure invokes shutdown to unwind partial acquisition. Failed shutdown preserves the endpoint record so cleanup can be retried. Transfers are bounded and validated before backend dispatch. Typed helpers validate XRGB8888 frame dimensions/stride/size, keyboard-vs-pointer event flags, and PCM rate/channel/sample alignment before routing to the relevant endpoint.
- MMIO/IO-port/IRQ resource descriptors are validated metadata only. The physical allocator and page mapper currently constrain managed physical pages to the first 1 GiB, so a PCI framebuffer/MMIO region above that range cannot be safely mapped through the current general page-map API. The interrupt setup currently verifies the timer IRQ/scheduler path and syscall entry; there is no keyboard/mouse IRQ route or audio interrupt handler. DMA descriptors are explicitly rejected as unsupported; there is no DMA allocator, IOMMU isolation, or bus-master enablement.
- Keyboard/mouse input and audio have typed endpoint interfaces but no concrete PS/2, USB HID, HDA, AC97 or other physical backend yet. They must report unavailable rather than synthesize hardware support.
- Windows `.sys` and Linux `.ko` images are not native ProsenjitOS drivers. Only drivers built for the native ProsenjitOS driver ABI may be registered as native; unsupported foreign drivers must be reported as unsupported.
- Desktop/compositor/window-management policy stays in user space. Kernel APIs expose low-level device contracts only.

## Acceptance evidence

`make hw-stack-test` runs host-side tests for PCI class classification, malformed/overflowing resource descriptors, explicitly unsupported DMA, invalid device IDs, malformed display/input/audio requests, missing endpoints, duplicate registration, initialization-failure cleanup, failed shutdown/retry, status reporting and normal cleanup. Kernel boot also runs the same deterministic contract self-test and emits a milestone before existing display, PCI, storage and user-mode boot checks.

Host tests do not prove a physical graphics/input/audio device works. QEMU PCI inventory and existing boot milestones prove discovery/boot only; no real display modeset, keyboard/mouse event delivery, audio playback, DMA, or MMIO mapping is claimed.
