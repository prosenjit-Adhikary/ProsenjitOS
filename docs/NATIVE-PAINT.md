# Native Paint — implementation foundation

ProsenjitOS Paint is an original, native-first raster drawing application project. It does not reuse Microsoft Paint code, assets, or branding. This milestone supplies a dependency-free C drawing core that can be host-tested while the OS user-mode launch, display-server, and input-service path is still incomplete.

## Implemented core
- Bounded 1–4096 pixel canvas dimensions (up to 16,777,216 pixels).
- ARGB pixel buffer; pencil/brush strokes via thick Bresenham lines.
- Outline/filled rectangles and ellipses; flood fill.
- Bounded undo/redo history (8 full-canvas snapshots).
- Uncompressed 24-bit BMP export and 24/32-bit BMP import with format and dimension checks.
- Host regression tests compiled with strict C11 warnings-as-errors.

## Build and test
Run make paint-unit-test. This target is separate from make iso and does not add GUI libraries to the kernel build.

## Required integration stages
1. Add a user-mode app entry point after the real userspace spawn/exec ABI is implemented.
2. Connect a Paint frontend to the versioned display and input service contracts in include/abi/display.h and include/abi/input.h.
3. Add file-picker/save-dialog integration through the permission-brokered VFS API.
4. Add selection/crop, clipboard, text/font rendering, color picker/palette, zoom, and PNG/JPEG codecs through a reviewed userspace library strategy.
5. Run host unit tests, warnings-as-errors OS build, QEMU GUI/input tests, file round-trip tests, and native BIOS boot tests.

## Boundary and status
This milestone is a drawing-core foundation, not a claim that a clickable GUI is already bootable inside ProsenjitOS. The repository documents that a complete userspace process-launch/display/input path is not yet available. Paint remains outside kernel/ and introduces no direct hardware access. Never load untrusted BMP data into privileged kernel code.
