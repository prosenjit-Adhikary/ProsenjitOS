# ProsenjitFS Physical-Durability Validation Plan

## Purpose and limits

This plan defines a separate, opt-in lab validation for storage durability on real hardware. It is **not** a claim that ProsenjitFS has passed physical power-loss testing. Mock-device fault injection and QEMU boot tests remain the routine CI gates; they cannot prove that a drive, controller, bridge, or firmware honors flush barriers under sudden power loss.

Do not run this plan on a production system, a personal data disk, or any device whose contents are not disposable.

## Preconditions

- Use a dedicated, expendable test drive and a separate test host or controlled power-cut fixture.
- Record the drive model, serial number, firmware, capacity, connection type, controller, and any USB/SATA bridge.
- Confirm the exact target device through two independent identifiers before any destructive setup.
- Disconnect or make inaccessible all unrelated writable drives where practical.
- Use a disposable image or dedicated test partition. Never rely on a drive name such as `/dev/sdX` remaining stable across boots.
- Keep automatic formatting, repair, orphan reclamation, and any unattended destructive operation disabled.
- Preserve the exact kernel commit, formatter version, test image hash, firmware settings, and test logs for every run.

## Test sequence

1. **Baseline:** create the test image/partition using the reviewed formatter path; mount it; write deterministic payloads with sequence numbers and checksums; flush; unmount cleanly; read back and verify.
2. **Flush capability:** record whether the block path exposes a flush callback and whether the device reports write-cache settings. If the path cannot provide a meaningful durable flush barrier, classify power-loss durability as unsupported for that setup.
3. **Controlled interruption:** use a purpose-built fixture to cut power at randomized points while repeatedly performing small file replacements and metadata mutations. Do not pull cables or interrupt power on a live production machine.
4. **Recovery:** restore power, boot into the test kernel or a controlled inspection environment, and mount without running any repair tool. Record whether recovery selects the complete pre-commit state or complete committed state.
5. **Integrity:** run the read-only integrity scan, verify payload hashes and namespace invariants, and confirm there are no mixed/partial logical results.
6. **Repeatability:** repeat each workload across many interruption points and cold boots; retain every failure and never discard an unexplained result.
7. **Read-only fallback:** for ambiguous/corrupt recovery states, verify that the filesystem fails closed or stays read-only and does not silently rewrite or reclaim metadata.
8. **Control run:** repeat the same workload without power interruption to separate test-fixture or media failures from recovery failures.

## Required workload coverage

- File create, overwrite, append, truncate, unlink, mkdir, rmdir, and rename.
- Journal header/images flush, commit-record publication, home-block application, and journal retirement.
- Repeated recovery after an interrupted recovery attempt.
- Full and nearly-full allocation state; directory and extent metadata updates.
- Devices with and without a supported flush path, recorded as separate configurations.

## Acceptance criteria

- Every recovered operation yields a complete allowed state: old state if commit was not durable, or new state if a valid commit was durable.
- No mixed payload, invalid namespace, leaked/duplicated allocation, or silent metadata repair is observed.
- Ambiguous commit state fails closed or exposes a read-only mount until safely resolved.
- The read-only integrity scan does not mutate durable media.
- Results are reproducible, tied to exact source and hardware identifiers, and reviewed before any durability claim is published.

## Safety and reporting

This test is destructive by design and may permanently damage the test filesystem or drive. A human operator must select and verify the expendable target separately for every test session. The test harness must not discover and choose a physical disk automatically.

Report results only for the exact tested device/controller/firmware configuration. Passing one setup does not certify all storage devices. QEMU and mock-device results must continue to be labeled separately from physical-device results.

## Current status

**Plan defined; physical execution not performed.** ProsenjitFS must not be described as physically power-loss certified until this plan has been executed on explicitly selected expendable hardware and the evidence has been reviewed.
