# DML runtime orchestration contract

## What this module implements

`user/desktop/dml_runtime.c` consumes the registry's validated dependency
order and coordinates a single active desktop session through a narrow
platform-backend interface:

- launch each dependency before the component that requires it;
- track opaque process handles and lifecycle state;
- reject overlapping roots and components already in an unexpected state;
- roll back newly launched dependencies in reverse order if a later launch
  fails;
- stop a session in reverse dependency order;
- preserve handles/state when stop fails so cleanup can be retried;
- allow a cleanly rolled-back failed launch to be retried; and
- report a desktop as active only while its root state is RUNNING; and
- reject runtime initialization if the manager already contains non-stopped sessions, because a fresh runtime has no trustworthy handles for existing processes.

The runtime validates the manager's public registry layout before acting: slots below `count` must be registered and slots at/above `count` must be unregistered. It also validates the public active-index list: every active index must refer to a registered entry, have a nonzero process handle, and not duplicate another active index. Reporting a root as active also requires that the root appears in the tracked active order; a stale root or an empty active set cannot be reported as a running desktop. The manager and runtime are still single-owner structures; these checks are defensive validation, not a substitute for synchronization or memory protection.

All calls must be serialized by one control-plane owner. The callbacks are
synchronous and must not re-enter the runtime API.

## Platform backend requirements

The `start` callback must validate package-root/entry-point resolution, create
a real isolated process using a supported OS process API, apply the intended
identity/permissions/capabilities, and return a non-zero opaque process handle (zero is reserved as invalid).
It must return zero only when the process was created successfully and the
handle was published; on failure it must not leave a live process behind. The
orchestrator rejects a success result with a zero handle as a backend contract
violation and does not mark the component active.

The `stop` callback must request orderly shutdown, wait/reap using the real
process API, and return zero only when the process is no longer running. If
shutdown fails or times out, it must return an error without pretending that
the process stopped. This module retains the handle and FAILED state so a
later stop attempt can retry.

The current kernel public syscall ABI has no process spawn/exec syscall, and
this repository has not yet supplied a production backend implementing these
callbacks. Therefore the runtime module is an implemented orchestration layer
with a testable adapter contract, **not yet a working OS desktop launcher**.
Do not wire a mock backend into the boot path or describe mock tests as a real
desktop session. See [DML process-launch implementation gates](DML-PROCESS-LAUNCH-GATES.md)
for the source-audit findings and the required kernel/userspace integration sequence.

## Host tests

`make desktop-unit-test` compiles and runs both the registry tests and
`tests/desktop_runtime_test.c`. The runtime test uses a deterministic mock
backend to cover dependency-first startup, failed-start rollback, retry after
rollback, reverse-order shutdown, retry after a stop failure, invalid process
handles, and stale/corrupted active-root bookkeeping. These tests
must be executed on the current source revision before their result can be
claimed. They do not prove kernel process creation, display rendering, input
delivery, window-system compatibility, or XFCE operation.
