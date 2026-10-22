# Shared Desktop Integration Protocol (Part 3)

Status: **protocol and host-side state model only**. This is not a display server, compositor, kernel ABI implementation, audio driver, or proof of a graphical desktop.

## Contract

Native and Windows application subsystems use one service contract. A future IPC adapter must bind caller identity to kernel-provided IPC peer identity; clients must not self-assert a process ID or subsystem. The current host model accepts IDs directly for deterministic testing and is not a security boundary.

- The wire protocol is the existing Part 1 ABI in include/abi/desktop.h (PROSENJIT_DESKTOP_ABI_VERSION); this layer calls its size/type/operation validators before state changes. pros_sd_event is an internal routed-event record, not a replacement wire ABI.
- Handles are opaque nonzero IDs checked against live records. A production implementation must use generation-tagged kernel capability handles to prevent stale-handle reuse; the host model is not a substitute.
- Process ownership is authoritative. Only a window owner may destroy it. Focus/input routing accepts only live connected processes and windows.
- Window lifecycle: connect -> create -> focus -> route input -> destroy. Disconnect/exit revokes owned windows/audio handles and clears focus.
- Display service owns the device/presentation lease. Clients submit bounded surface/buffer descriptors; they never access privileged registers or framebuffer directly. Surface/presentation adapter is not implemented here.
- Input service normalizes keyboard/pointer events; broker routes only to focused live window. Physical device delivery, queue backpressure and key-state recovery remain unimplemented.
- Clipboard rights are explicit per process and data is bounded to 256 bytes in this host model. Production defaults should deny or prompt for cross-application reads and scope grants; no consent UI exists here.
- Audio handles are per-owner. This model does not implement an audio backend, mixing, arbitration or playback.
- Lifecycle event schema is versioned; production must send focus, clipboard, process-exit and subsystem-loss notifications over authenticated bounded IPC. Transport is not implemented.
- Crash/disconnect fails closed, revokes resources, clears focus and drops stale events. A userspace supervisor must use bounded restart budgets; no supervisor exists here.
- Unsupported features must return a stable unsupported-feature error through future feature negotiation, never silently claim success.

## Runtime adapter security requirements

1. Authenticate peer identity using IPC/kernel metadata; never trust a payload PID/subsystem label.
2. Validate ABI version, struct size, message type, flags, reserved fields and payload length before reading optional fields.
3. Check handle existence, generation, owner, state and rights on every operation.
4. Check overflow, buffer lengths, alignment and memory ownership before copying; impose quotas and bounded queues.
5. Keep compositor/window-manager policy and composition in userspace. Kernel responsibilities are device ownership, memory isolation, validated mapping/presentation primitives and event source boundaries.
6. Native and Windows runtimes remain isolated; Windows semantics are translated by its adapter, not a separate privileged display path.
7. Reject unsupported formats, flags and features explicitly.
8. On service loss, revoke resources, drop stale events, preserve kernel stability and return a retryable disconnected result where applicable.

## Tests and acceptance

The host state model in user/desktop/shared_desktop.[ch] consumes the existing Part 1 wire contract through pros_sd_validate_request() and covers focus/input routing, window destruction and ownership, clipboard permissions and size limits, invalid/stale handles, audio ownership, process exit and disconnect cleanup. It does not add a second public wire protocol. Run make desktop-shared-test. This is not connected to kernel IPC, include/abi/display.h, include/abi/input.h, real devices or a process manager.

| Criterion | Evidence/status |
|---|---|
| Same Native/Windows contract | Protocol/model only |
| Display/presentation | No surface/present adapter; blocked |
| Window/focus/ownership | Host tests pending |
| Keyboard/mouse routing | Host routing test pending; no hardware route |
| Clipboard access policy | Host rights tests pending; no consent UI |
| Audio integration | Handle lifecycle only; backend missing |
| Lifecycle notifications | Schema only; transport missing |
| Invalid handles/buffers/messages | Host validation tests pending; kernel copy validation not wired |
| Process termination/disconnection | Host cleanup tests pending; real supervisor absent |
| Runtime graphical desktop | No evidence; not met |

Part 3 acceptance is not passed until host tests and runtime adapter criteria have evidence. A host test does not prove a graphical desktop. Do not start Part 4 before all acceptance criteria pass.
