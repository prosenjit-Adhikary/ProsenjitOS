# Native Graphics & Hardware Stack — Part 2

Status: **contract/lifecycle foundation implemented; concrete hardware drivers remain partial or unverified.**

## Verification record — 2026-10-23

Latest verified Part 2 PR head at this record: `35236f5d5060304b5e53510c860d4cf1fb4e3c11`.
- Build and boot-test: [run 39930667435](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39930667435) — **passed**, including all hosted desktop/hardware contract suites, ISO build, ProsenjitFS block-init diagnostics, ISO QEMU boot, and native BIOS image QEMU boot.
- Security architecture checks: [run 39930667462](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39930667462) — **passed**.
- This commit extends CI syntax checks and runs for USB descriptor, USB HID report decoder, USB host-controller classification, PS/2 decoder, input-service queue, keymap, and PCI BAR contract tests. These remain host/emulator tests; no live USB controller transport or physical hardware was tested.
- USB descriptor validation rejects duplicate interface-number/alternate-setting pairs while accepting the same interface number with distinct alternate settings. Hosted regression tests cover both cases.
- USB host-controller classification tests now also exercise all controller-name mappings, including generic and unsupported values.

These results verify the automated checks attached to this exact commit only. They do **not** close the Part 2 acceptance gates below and do not establish physical-hardware support.

## Remaining acceptance gates (must pass before Part 2 is complete)

- [ ] Establish an authorized, persistent userspace input-service lifecycle; provision its READ capability through the trusted service-launch path and connect the keymap to live events.
- [ ] Complete PS/2 mouse integration: the current three-byte movement decoder is compiled and unit-tested, but controller enablement, live transport/polling, button-state delivery, and device reset/disconnect handling are not integrated. USB HID boot-protocol keyboard/mouse report decoders now exist and are hosted-tested, but they are not connected to USB enumeration, host-controller transfers, live input service delivery, or disconnect/error recovery. USB host-controller support remains open.
- [ ] Define and test keyboard/mouse IRQ routing and interrupt acknowledgement; do not claim IRQ-driven input while the current backend is polling.
- [ ] Design safe DMA allocation, cache/coherency rules, device ownership and IOMMU isolation before enabling bus mastering or implementing a DMA-backed audio driver.
- [ ] Implement a real supported audio backend (for example, a narrowly scoped HDA or AC'97 target) with failure-safe initialization, bounded buffers, shutdown and tests.
- [ ] Run all host suites and both boot modes on the authorized Linux x64 runner for the exact final commit; record physical-hardware results separately for each tested device.
- [ ] Re-audit Part 1 allocator/page-table ownership and concurrency blockers before treating the hardware stack as safe for SMP or broader device use.

Until every applicable gate is checked with evidence, keep this PR draft and do not begin Part 3.


## Boundaries

- PCI configuration-space discovery remains read-only. The scanner inventories PCI BDF, vendor/device IDs, class/subclass and programming interface. Boot now reads each discovered device's BARs and reports assigned/unsupported BAR counts. `pci_read_bar()` decodes assigned I/O, 32-bit MMIO and 64-bit MMIO BAR base addresses without writing configuration registers; reserved BAR types, unsupported I/O addresses and addresses outside the x86-64 physical-address mask fail closed. BAR aperture sizing and PCI command-register enablement are deliberately not performed. The Part 2 classifier is exercised on discovered PCI records; unknown devices remain OTHER/unsupported.
- The driver registry remains authoritative for native driver identity and lifecycle. The bounded endpoint registry is an adapter contract for display, input and audio backends; it does not grant a process hardware access.
- A Multiboot2 RGB framebuffer backend now requests 1024x768x32 from the bootloader, validates the returned format/geometry, reserves its physical pages during memory initialization, maps it supervisor-only/NX with PCD/PWT, and registers a display endpoint. It copies only complete matching XRGB8888 frames and unmaps on endpoint shutdown. Native BIOS boot or unsupported framebuffer formats fall back safely to the legacy text console. This is a bootloader-provided linear framebuffer path, not a GPU modesetting driver; the separate `kernel/display.c` synthetic lifecycle test remains synthetic.
- Endpoint registry state transitions are protected by an IRQ-safe atomic lock; backend callbacks run outside the lock, in-flight transfers prevent shutdown, and failed shutdown moves the endpoint into an inactive cleanup-pending state that can be retried. Endpoint init failure invokes shutdown to unwind partial acquisition. Transfers are bounded and validated before backend dispatch. Display/audio endpoint callbacks receive typed frame/buffer descriptors so format metadata is not discarded. Typed helpers validate XRGB8888 frame dimensions/stride/size, keyboard-vs-pointer event flags, and PCM rate/channel/sample alignment before routing to the relevant endpoint.
- Normal managed RAM allocation and the general RAM page-map API remain limited to the first 1 GiB. A dedicated `paging_map_mmio_range()` now supports page-aligned MMIO ranges up to 256 MiB within the x86-64 physical-address mask; it maps supervisor-only, NX pages with PCD/PWT cache-disable attributes, rejects non-canonical/overflowing ranges, refuses to overwrite existing mappings, and rolls back pages mapped earlier in a failed range. `paging_unmap_mmio_range()` releases the virtual mappings. These APIs do not reserve PCI BARs, probe devices, configure PAT/MTRRs, or establish ownership automatically; callers must reserve an exclusive virtual range and use the API only for genuine MMIO resources. The interrupt setup currently verifies the timer IRQ/scheduler path and syscall entry; there is no keyboard/mouse IRQ route or audio interrupt handler. DMA descriptors are explicitly rejected as unsupported; there is no DMA allocator, IOMMU isolation, or bus-master enablement.
- A concrete legacy PS/2 set-1 polling backend reads controller status/data ports, filters parity/timeout/AUX bytes, decodes make/break and E0-prefixed keys, and filters Pause/fake Print Screen shift transitions. Polling is bounded to 32 controller bytes per call. The kernel input service registers a fixed-size 128-event queue endpoint and exposes `pros_input_service_shutdown()` for managed teardown. Shutdown returns `EBUSY` while any READ capability still references the static service object, preventing a handle from becoming valid again after stop/restart; the check and teardown are serialized against local interrupts on the current single-CPU kernel. The endpoint registry also rejects teardown while transfers are in flight. Once handles are closed, successful teardown makes the queue unavailable and repeat shutdown returns `ENODEV`. The boot self-test now asserts that shutdown is denied while a READ handle is live and that the service remains ready. Hosted tests cover normal shutdown, stopped-state errors, and clean shutdown/reinitialization.  The timer heartbeat polls the controller every four ticks; the event callback is bounded, uses no allocation, and counts queue overflows. A capability-checked `PROSENJIT_SYS_INPUT_READ` syscall now accepts a READ-only device handle and a validated userspace output buffer; the kernel-only `pros_input_service_grant_read()` provisioning hook grants only READ authority, and the syscall rejects handles that do not refer to this exact input endpoint. A boot-time self-test grants a READ-only handle, verifies that a forged handle is denied without consuming the queued event, then confirms the valid handle can dequeue it. The CPL3 syscall test now receives a separately granted READ-only input handle in RSI, reads the deterministic event into a validated userspace buffer, checks the ABI fields, and closes the handle. The actual C US-QWERTY keymap is linked into the copied user image and runs at CPL3; the test verifies that PS/2 set-1 scan code `0x20` translates to `d`, that an empty queue returns `EAGAIN`, that forged handles are denied, and that an invalid output pointer is rejected. Build, ISO QEMU boot, and native-BIOS QEMU boot passed on head `ed813b3a9d03e1e1d2553741507f3649d6353ae9` (run `39908564038`), and the security workflow passed (`39908564046`). This is a deterministic QEMU acceptance test, not proof of physical keyboard compatibility or a persistent input service/complete desktop input pipeline. USB HID support, integration of the keymap into a persistent input service, keyboard/mouse IRQ routing, and physical-hardware verification remain open. The PS/2 controller is not reconfigured and is assumed to provide set-1 translation. Audio still has no concrete HDA/AC'97 backend and must report unavailable rather than synthesize hardware support.
- Windows `.sys` and Linux `.ko` images are not native ProsenjitOS drivers. Only drivers built for the native ProsenjitOS driver ABI may be registered as native; unsupported foreign drivers must be reported as unsupported.
- Desktop/compositor/window-management policy stays in user space. Kernel APIs expose low-level device contracts only.

## Acceptance evidence

`make ps2-keyboard-test` exercises ordinary press/release codes, E0 extended keys, Pause-sequence filtering, fake Print Screen shifts, malformed-byte recovery, AUX interleaving, the 32-byte polling budget, and the hosted fake-controller FIFO path. `make ps2-mouse-test` exercises a pure three-byte PS/2 relative-movement decoder, packet sync recovery, sign handling, overflow rejection, and malformed sign metadata. The mouse decoder is compiled into the kernel and emits bounded relative-motion and left/right/middle button-transition events, but is not yet connected to a controller transport and is not evidence of physical mouse support. `make input-service-test` exercises endpoint registration, PS/2-to-queue delivery, dequeue behavior, and overflow accounting. These are hosted contract tests; no physical PS/2 device or userspace input path was tested.

`make hw-stack-test` runs host-side tests for PCI class classification, malformed/overflowing resource descriptors, explicitly unsupported DMA, invalid device IDs, malformed display/input/audio requests, missing endpoints, duplicate registration, initialization-failure cleanup, failed shutdown/retry, status reporting and normal cleanup. Kernel boot also runs the same deterministic contract self-test and emits a milestone before existing display, PCI, storage and user-mode boot checks.

The paging self-test now maps a reserved APIC/PCI-style MMIO address range without dereferencing it, verifies physical translation plus supervisor/NX/PCD/PWT flags, checks malformed and overflowing ranges, and unmaps the range; both ISO and native-BIOS QEMU boot tests exercise this self-test. This proves the page-table mapping contract in the emulator, not a physical device's BAR ownership, register access, or operation. No real display modeset, keyboard/mouse event delivery, audio playback, or DMA support is claimed.

## Framebuffer endpoint integration check

On a bootloader-provided supported RGB framebuffer, the kernel now checks that the registered display endpoint is active and submits a full-frame descriptor through the public presentation API. The mapped framebuffer is used as the source, so the check is visually neutral and avoids allocating a duplicate frame. The QEMU ISO acceptance gate requires this milestone. This confirms endpoint dispatch and descriptor validation in the emulator; it does not prove visible pixel correctness, GPU modesetting, or physical display compatibility. Native BIOS boot without a Multiboot framebuffer continues to use the legacy text console.

## Userspace keyboard layout foundation

`user/desktop/keymap.c` adds a stateful US-QWERTY translation helper for the shared input ABI: set-1 letter/digit/punctuation keys, left/right Shift, Caps Lock, Control and Alt state. It ignores key releases, unsupported extended keys, malformed ABI versions, and ambiguous press/release edges. `make keymap-test` exercises the hosted translator. This is a userspace library component only: it is not yet connected to a running input service, does not provide non-US layouts or Unicode/IME support, and does not close the missing capability-checked kernel-to-userspace event route.


## USB-first input direction

Modern input work should prioritize USB HID; PS/2 remains a legacy fallback only. The new `kernel/usb_hid.c` is deliberately a transport-independent parser for the USB HID **boot protocol** reports, not a USB driver:

- Keyboard reports are fixed at eight bytes. The parser emits HID usage-code press/release transitions, handles modifier bits E0–E7, rejects duplicate usages and rollover/error usages 1–3, and preserves the previous state when a report is invalid.
- Mouse reports are fixed at three bytes. The parser emits signed relative motion and left/right/middle button transitions through the shared native input event ABI.
- The parser has no controller register access, DMA, interrupt handling, USB enumeration, descriptor parsing, endpoint scheduling, or hot-plug lifecycle. A successful parser unit test must not be represented as a working USB keyboard/mouse.
- `make usb-hid-test` runs hosted tests for report transitions, unchanged reports, invalid keyboard rollover/duplicate usages, pointer motion, button press/release, and invalid arguments.

Implementation sequence before claiming usable USB input:
1. Keep PCI enumeration read-only and identify supported USB host-controller programming interfaces from class/subclass/prog-if.
2. Implement one controller family at a time, preferably xHCI for modern machines, with spec-correct reset, ownership, command/event rings, bounded waits and timeout/error paths.
3. Implement safe DMA memory allocation and cache/coherency/ownership rules before enabling bus mastering; add IOMMU isolation where available. Do not pass ordinary kernel virtual pointers as DMA addresses.
4. Enumerate USB devices and parse bounded configuration/interface/endpoint descriptors; reject malformed lengths and unsupported configurations.
5. Implement interrupt-IN transfer scheduling, report polling/interrupt completion, and disconnect/reset cleanup without stale handles or stuck key/button state.
6. Connect parsed events to the authorized input-service queue and userspace keymap. Add QEMU and physical-device tests for each declared controller/device combination.
7. Keep Bluetooth HID as a separate later transport; do not imply USB HID support covers Bluetooth.



## PCI USB host-controller discovery groundwork

Added a pure classifier for PCI serial-bus/USB host-controller functions (base class 0x0C, subclass 0x03). It recognizes UHCI (prog-if 0x00), OHCI (0x10), EHCI (0x20), xHCI (0x30), and generic/other (0x80); unknown programming interfaces are reported as unsupported. Hosted tests cover each classification and invalid inputs. This helper does not scan hardware itself, enable PCI bus mastering, map controller registers, reset a controller, allocate DMA, enumerate USB devices, or perform transfers. The next driver stage should target xHCI first, after implementing safe DMA allocation and BAR aperture validation.


## Bounded USB configuration descriptor parsing

Added `kernel/usb_descriptor.c` as a pure parser for a configuration-descriptor blob already obtained from a USB transport. It validates the configuration header and total length, walks descriptors only within that bound, rejects truncated or nonstandard-length standard interface/endpoint descriptors, and selects a boot-protocol HID interface with an interrupt-IN endpoint. It validates the fixed nine-byte configuration header (including nonzero configuration value and reserved `bmAttributes` bits), checks the declared interface count against distinct interface numbers, rejects duplicate interface-number/alternate-setting pairs while allowing distinct alternate settings for the same interface number, and validates per-interface endpoint counts, requires endpoints to follow an interface descriptor, rejects duplicate endpoint addresses within an interface, and includes regression tests proving malformed non-selected OUT endpoints are rejected; it validates every endpoint descriptor (not only the selected HID endpoint) for endpoint number, reserved address/attribute bits, and nonzero packet size. Selected boot-keyboard interrupt-IN endpoints must carry at least the eight-byte boot report; boot-mouse endpoints must carry at least three bytes. Interrupt endpoints must have a nonzero polling interval; unsupported high-bandwidth transaction encodings are rejected because the current endpoint contract does not represent their multiplier. The output structure is only committed after the entire descriptor blob has passed validation and a matching endpoint is found, so malformed input cannot leave partially updated output. `make usb-descriptor-test` covers keyboard/mouse selection, truncation, impossible total length, malformed lengths, direction/type, reserved bits, endpoint zero, zero interval, unsupported high-bandwidth encoding, undersized keyboard/mouse packets, and output atomicity.

This is **not** USB enumeration or a live driver: no device request, configuration selection, HID class request, endpoint transfer, controller scheduling, DMA, interrupt handling, or disconnect recovery is implemented here. The parser must only receive a descriptor buffer whose actual received length is supplied by a trusted transport.


## Follow-up verification — 2026-10-23

The current Part 2 PR head `99ece4de28db55bb44be5d691ef302129a4ea07a` has both required automated workflows green:
- [Build and boot-test, run 39931620524](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39931620524) — passed.
- [Security architecture checks, run 39931620532](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39931620532) — passed.
- PR: [#171](https://github.com/prosenjit-Adhikary/ProsenjitOS/pull/171), currently open and mergeable at the time of this record.

Since the earlier verification record, input validation rejects pointer events that combine relative-motion flags with press/release transitions, both at the hardware-stack API and the input-service queue boundary. USB boot-keyboard reports with a nonzero reserved byte are rejected before state mutation. The current green workflows cover the current PR head; they do not establish physical USB transport, real xHCI initialization, DMA, or hardware audio support.


## xHCI capability inspection foundation — 2026-10-23

Added `kernel/xhci.c` and `kernel/include/xhci.h` to parse a bounded, read-only byte snapshot of an xHCI BAR's capability registers. The parser validates the capability-header length/alignment, xHCI interface version, nonzero structural limits, doorbell/runtime offsets and minimum spans, and optional extended-capability pointer before publishing output. Hosted regression tests cover truncated/malformed values and ensure failed parsing does not partially update the output structure. The test is wired into `make xhci-capability-test` and the build workflow.

**Scope boundary:** this is not an xHCI driver. No PCI BAR is mapped by this helper, no controller registers are read from live MMIO, no reset/run state transition is attempted, and no DMA rings, command/event processing, port enumeration or USB transfers are implemented. The parser only accepts an already-copied register snapshot; physical-controller bring-up remains an open acceptance gate.

## xHCI capability inspection follow-up — 2026-10-23

The extended-capability snapshot validator now treats xECP as a BAR-relative offset; it is not incorrectly constrained to the range below CAPLENGTH. The parser validates that the xECP header fits in the supplied BAR snapshot, and the linked-list walker checks each capability header and positive DWORD-relative Next pointer against the BAR bounds, rejects reserved capability ID zero, and publishes the count only after the whole chain validates. The capability parser also validates the minimum runtime-register span for the reported interrupter count rather than checking only the first runtime-register block.

Hosted regression coverage includes a valid extended-capability chain beyond CAPLENGTH, a valid multi-entry chain, a reserved capability ID, invalid alignment, a pointer escaping the BAR, a truncated runtime region, and output atomicity on errors.

Latest code verification before this documentation follow-up: PR head `f4dc640c2c02636136c3e89106c2e0075c8af1f7`.
- [Build and boot-test, run 39935984771](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39935984771) — **passed**, including ISO and native-BIOS QEMU boot tests.
- [Security architecture checks, run 39935984813](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39935984813) — **passed**.

These results apply to the cited code commit, before this documentation-only follow-up. The parser still consumes a copied snapshot only: no live MMIO access, controller reset/start, DMA rings, USB enumeration, or USB transfers are implemented.


## PCI configuration serialization audit — 2026-10-23

An audit found that legacy PCI configuration mechanism #1 uses a shared address/data port pair, but individual reads previously had no transaction lock. Concurrent callers could interleave the write to 0xCF8 and read from 0xCFC, returning a value from the wrong BDF/register. pci_config_read32() now serializes that pair with an atomic lock and masks local interrupts while the lock is held, preventing same-CPU interrupt re-entry deadlock; hosted builds avoid privileged interrupt instructions. This protects each address/data transaction, not an entire multi-register scan, and does not add PCIe ECAM support or change the read-only BAR policy.

Code commit: fddb9742553f1f16a4e25f4ad3e3e2e801c30258. The exact latest branch head and CI results must be checked after this change before treating it as verified. No hardware behavior is claimed from this serialization fix alone.


## Physical memory-map reservation audit — 2026-10-23

The allocator previously marked available Multiboot2 memory ranges but did not explicitly apply non-available map entries. Firmware maps are expected to be non-overlapping, but if an overlapping/contradictory map described a reserved page as available, allocator safety depended on entry ordering. Initialization now performs a second pass over the already-validated map and reserves every non-available range after available ranges have been collected, so reserved entries win regardless of ordering. This does not replace validation of firmware-provided map quality and does not expand the managed physical range beyond the existing 1 GiB limit.

Code commit: 5e152af95b0423f3057fa13f85828a88e75bbd9b. Await the exact-head build/ISO/native-BIOS QEMU and security workflows before treating the change as verified.
