# DML dependency-order API

The Desktop Management Layer registry can now resolve a registered desktop's
transitive dependency closure into a deterministic, dependency-first order.
The API returns registry indices; callers can retrieve the corresponding
validated, copied manifest with `pros_dml_get()` or inspect the manager entry
under the DML's single-owner/serialized-call contract.

## Contract

- The requested desktop and every transitive dependency must be registered.
- Missing dependencies return `PROS_DML_ENODEP`; cycles return
  `PROS_DML_ELOOP`; an unknown root returns `PROS_DML_ENOENT`.
- Dependencies appear before the desktop that requires them. Shared
  dependencies are emitted once.
- Output is built in a bounded local array and copied only after successful
  resolution and capacity validation. An insufficient buffer returns
  `PROS_DML_ENOSPC`, reports the required count, and leaves the caller's
  output array untouched.
- This is a planning/preflight API, not a service or process launcher. It does
  not start services, execute manifests, install packages, or imply that a
  desktop runtime is available.
- DML manager calls remain serialized by the control-plane owner; locking is
  not implemented by this registry module.

## Verification

The host test source covers dependency-first order, shared dependency
ordering, cycle rejection, capacity failure without partial output, and
missing-root handling. Run `make desktop-unit-test` in
the repository's Debian environment. No test result should be claimed until
that command has actually completed.

## Session-state bookkeeping

The DML exposes a bounded per-desktop lifecycle state machine: STOPPED -> STARTING -> RUNNING -> STOPPING -> STOPPED, with failure edges to FAILED and an explicit reset from FAILED to STOPPED. Illegal transitions return `PROS_DML_ESTATE`. This is control-plane bookkeeping only: it does not spawn or supervise processes, prove that a session is running, or replace a real init/service manager. All calls must be serialized by the owner.
