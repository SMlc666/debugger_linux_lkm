# debugger_linux_lkm

Minimal out-of-tree kernel module scaffold for early arm64 Android GKI debugger experiments.

This repository intentionally starts with a small smoke-test module and a safe-first hidden transport scaffold:

- loads and unloads cleanly
- can optionally export a `debugfs` status file at `/sys/kernel/debug/lkmdbg/status`
- can optionally inline-hook `/proc/version` open as a narrow session bootstrap
- provides a stable place to add probe, breakpoint, and event plumbing next
- on load, best-effort disables the kprobe blacklist and patches common CFI
  slowpath symbols to simplify inline hooks on Android GKI

Current source layout:

- `core/`: module init, runtime symbol lookup, protection bypass helpers
- `hook/`: arm64 inline hook support with trampoline generation and rollback
- `transport/`: hidden `/proc/version` transport and session fd handling
- `mem/`: target memory read/write helpers
- `ui/`: debugfs status export
- `include/`: shared kernel/user protocol and internal declarations

Current hook support is intentionally minimal:

- single-target arm64 inline hook install/uninstall
- 4-instruction entry patch with a relocated trampoline
- relocation handling for common branch, ADR/ADRP, literal load, and test-branch instructions
- stop-machine patching for install and rollback

The current low-level arm64 hook core is vendored from `KernelPatch` and
adapted into this repository as a kernel-internal backend:

- source basis: `kernel/include/hook.h` and `kernel/base/hook.c`
- local adapter keeps a private `lkmdbg_hook_*` kernel API
- KernelPatch-specific KPM/runtime integration is not included yet
- chain hooks (`hook_wrap`) and function-pointer hooks are not wired into `lkmdbg` yet

## Local build

Build against the currently selected kernel headers:

```bash
make
```

Build against a specific kernel tree or headers directory:

```bash
make KDIR=/path/to/kernel/build
```

Build the user-space bootstrap test tool:

```bash
cc -O2 -Wall -Wextra -o tools/lkmdbg_open_session tools/lkmdbg_open_session.c tools/driver/bridge_c.c
cc -O2 -Wall -Wextra -o tools/lkmdbg_stealth_ctl tools/lkmdbg_stealth_ctl.c
cc -O2 -Wall -Wextra -pthread -o tools/lkmdbg_mem_test tools/lkmdbg_mem_test.c tools/driver/bridge_c.c tools/driver/bridge_events.c tools/driver/bridge_memory.c tools/driver/bridge_control.c
```

User-space driver code is split under `tools/driver/`:

- `session.*` for session open/target/status
- `events.*` for event config ioctls
- `memory.*` for memory transfer ioctls
- `driver.*` as a thin C++ facade
- `bridge_c.*` for C session/status calls
- `bridge_events.*` for C event queue helpers
- `bridge_memory.*` for C memory xfer helpers
- `bridge_control.*` for C control-path ioctls
- `common.hpp` for shared logging/error formatting

Load and inspect:

```bash
sudo insmod lkmdbg.ko enable_debugfs=1
sudo cat /sys/kernel/debug/lkmdbg/status
sudo rmmod lkmdbg
```

Enable the hidden `/proc/version` ioctl transport:

```bash
sudo insmod lkmdbg.ko hook_proc_version=1 enable_debugfs=1
sudo cat /sys/kernel/debug/lkmdbg/status
```

Run the module-local inline hook smoke test on load:

```bash
sudo insmod lkmdbg.ko hook_selftest_mode=1 enable_debugfs=1
sudo cat /sys/kernel/debug/lkmdbg/status
```

Selftest modes:

- `hook_selftest_mode=1`: prepare the inline hook only
- `hook_selftest_mode=2`: prepare the exec pool only
- `hook_selftest_mode=3`: allocate an exec trampoline slot only
- `hook_selftest_mode=4`: populate the exec trampoline
- `hook_selftest_mode=5`: install the hook, but do not invoke the hooked function
- `hook_selftest_mode=6`: install the hook and invoke one module-local test call

The hardening bypasses are now opt-in:

```bash
sudo insmod lkmdbg.ko bypass_kprobe_blacklist=1 bypass_cfi=1
```

This stage is intentionally conservative:

- normal `/proc/version` reads stay untouched
- the module hooks the original `/proc/version` `open` callback and only swaps `file->f_op` on matching opened instances
- it does not patch the shared operations table in place
- unload removes the inline hook and restores the original path

The hidden ioctl protocol now uses a bootstrap-plus-session model:

- `/proc/version` only handles `LKMDBG_IOC_OPEN_SESSION`
- that ioctl returns an anonymous session fd created with `anon_inode_getfd()`
- follow-up control ioctls run on the session fd instead of `/proc/version`
- the session fd now supports `read()` and `poll()` for queued events
- stealth control also lives on the session fd; `/proc/version` remains bootstrap-only

Current stealth controls are intentionally narrow:

- `debugfs` visibility can be toggled on or off at runtime
- module-list hide only removes `lkmdbg` from `/proc/modules` and `lsmod`
- optional `sysfshide` removes the module kobject from `/sys/module/lkmdbg`
- optional `ownerprochide` hides the owner session process from `/proc` views
- module-list hide is only accepted when the `/proc/version` hook is active, so there is still a restore path
- `sysfshide` is also only accepted when the `/proc/version` hook is active, so there is still a restore path

`ownerprochide` currently uses a lookup-only procfs backend:

- hook points: `proc_pid_lookup`, `proc_pid_readdir`, and `proc_tgid_base_lookup`
- cache invalidation: enabling owner hide drops cached `/proc/<tgid>` dentries with `d_drop()`
- no `proc_pid_permission` hook is used in the current design
- direct file paths such as `/proc/<tgid>/status`, `/proc/<tgid>/comm`, and `/proc/<tgid>/cmdline` are blocked via `proc_tgid_base_lookup`
- owner process self-bypass is intentionally disabled while owner hide is active

Example stealth flow:

```bash
sudo insmod lkmdbg.ko hook_proc_version=1
cc -O2 -Wall -Wextra -o tools/lkmdbg_stealth_ctl tools/lkmdbg_stealth_ctl.c
sudo ./tools/lkmdbg_stealth_ctl report
sudo ./tools/lkmdbg_stealth_ctl show
sudo ./tools/lkmdbg_stealth_ctl hide
sudo ./tools/lkmdbg_stealth_ctl restore
```

`report` also checks the current user-visible exposure surface, including:

- `/proc/modules`
- `/sys/module/lkmdbg*`
- `/sys/kernel/debug/lkmdbg*`
- `/proc/kallsyms`
- `/proc/bus/input/devices`

The current session ioctls include:

- `LKMDBG_IOC_GET_STATUS`
- `LKMDBG_IOC_RESET_SESSION`
- `LKMDBG_IOC_SET_TARGET`
- `LKMDBG_IOC_READ_MEM`
- `LKMDBG_IOC_WRITE_MEM`
- `LKMDBG_IOC_QUERY_VMAS`
- `LKMDBG_IOC_QUERY_PAGES`
- `LKMDBG_IOC_QUERY_THREADS`
- `LKMDBG_IOC_GET_REGS`
- `LKMDBG_IOC_SET_REGS`
- `LKMDBG_IOC_SET_SYSCALL_TRACE`
- `LKMDBG_IOC_GET_SYSCALL_TRACE`
- `LKMDBG_IOC_SET_EVENT_CONFIG`
- `LKMDBG_IOC_GET_EVENT_CONFIG`
- `LKMDBG_IOC_RESOLVE_SYSCALL`
- `LKMDBG_IOC_FREEZE_THREADS`
- `LKMDBG_IOC_THAW_THREADS`
- `LKMDBG_IOC_GET_STOP_STATE`
- `LKMDBG_IOC_CONTINUE_TARGET`
- `LKMDBG_IOC_ADD_HWPOINT`
- `LKMDBG_IOC_REMOVE_HWPOINT`
- `LKMDBG_IOC_QUERY_HWPOINTS`
- `LKMDBG_IOC_REARM_HWPOINT`
- `LKMDBG_IOC_SINGLE_STEP`
- `LKMDBG_IOC_REMOTE_CALL`
- `LKMDBG_IOC_REMOTE_THREAD_CREATE`

Syscall tracing now has three session-fd modes:

- `EVENT`: queue `LKMDBG_EVENT_TARGET_SYSCALL` records
- `STOP`: freeze the target on enter/exit and surface a normal stop state
- `CONTROL`: enter-only fallback-hook mode where user space resolves the
  syscall before it runs

The control flow is:

- arm `LKMDBG_IOC_SET_SYSCALL_TRACE` with `LKMDBG_SYSCALL_TRACE_MODE_CONTROL`
  and `LKMDBG_SYSCALL_TRACE_PHASE_ENTER`
- wait for a `LKMDBG_STOP_REASON_SYSCALL` stop with
  `LKMDBG_STOP_FLAG_SYSCALL_CONTROL`
- issue `LKMDBG_IOC_RESOLVE_SYSCALL` with one of:
  `ALLOW`, `SKIP`, or `REWRITE`
- `LKMDBG_IOC_CONTINUE_TARGET` then releases the frozen target and lets the
  intercepted syscall complete with the chosen behavior

The helper tool now exposes the same flow directly:

```bash
sudo ./tools/lkmdbg_mem_test sysset <pid> control enter [tid] [syscall_nr]
sudo ./tools/lkmdbg_mem_test stop <pid>
sudo ./tools/lkmdbg_mem_test sysresolve <pid> allow
sudo ./tools/lkmdbg_mem_test cont <pid>
```

Remote call now supports optional arm64 register overrides through request
flags:

- `LKMDBG_REMOTE_CALL_FLAG_SET_SP`
- `LKMDBG_REMOTE_CALL_FLAG_SET_RETURN_PC`
- `LKMDBG_REMOTE_CALL_FLAG_SET_X8`

`LKMDBG_IOC_REMOTE_THREAD_CREATE` is a synchronous convenience wrapper built on
top of remote call:

- user space provides a target-side launcher helper address
- the module runs that helper on a parked frozen thread
- it auto-continues until the launcher returns and then re-freezes at the
  normal remote-call stop
- the ioctl returns the launcher result, created tid, remote-call id, and stop
  cookie

The helper tool also provides end-to-end wrappers that freeze, pick a parked
thread, run the operation, print the result, and resume the target:

```bash
sudo ./tools/lkmdbg_mem_test rcall <pid> <target_pc_hex> [arg0 ... arg7]
sudo ./tools/lkmdbg_mem_test rcallx8 <pid> <target_pc_hex> <x8_hex> [arg0 ... arg7]
sudo ./tools/lkmdbg_mem_test rthread <pid> <launcher_pc_hex> <start_pc_hex> <arg_hex> <stack_top_hex> [tls_hex]
```

Memory transfer requests now use a single batched shape:

- one `READ_MEM` or `WRITE_MEM` ioctl carries an array of transfer ops
- a single-op request is just `op_count=1`
- the kernel processes ops against one target `mm` without faulting missing remote pages in
- large contiguous transfers are handled as one op; sparse transfers can be grouped into one batched request
- `lkmdbg_mem_op.flags` currently supports `LKMDBG_MEM_OP_FLAG_FORCE_ACCESS` for privileged access to already-present pages even when user permissions are removed
- forced writes are now kernel-permitted on already-present pages as well; user space is responsible for avoiding COW-sensitive or file-backed targets if that distinction matters

Current session events include:

- `LKMDBG_EVENT_SESSION_OPENED`
- `LKMDBG_EVENT_SESSION_RESET`
- `LKMDBG_EVENT_INTERNAL_NOTICE`
- `LKMDBG_EVENT_TARGET_CLONE`
- `LKMDBG_EVENT_TARGET_EXEC`
- `LKMDBG_EVENT_TARGET_EXIT`
- `LKMDBG_EVENT_TARGET_SIGNAL`
- `LKMDBG_EVENT_TARGET_SYSCALL`
- `LKMDBG_EVENT_TARGET_MMAP`
- `LKMDBG_EVENT_TARGET_MUNMAP`
- `LKMDBG_EVENT_TARGET_MPROTECT`
- `LKMDBG_EVENT_TARGET_STOP`

Event delivery is session-configurable:

- `LKMDBG_IOC_GET_EVENT_CONFIG` returns the current mask and supported mask
- `LKMDBG_IOC_SET_EVENT_CONFIG` applies a per-session event mask

Execution and watchpoints now have two backends behind the existing session fd
API:

- hardware breakpoints/watchpoints via `LKMDBG_HWPOINT_TYPE_*`
- arm64 mmu breakpoints/watchpoints via `LKMDBG_HWPOINT_FLAG_MMU`

The MMU backend currently supports `read`, `write`, `exec`, and mixed
combinations such as `rx`, `rw`, `wx`, and `rwx`, with one guarded page per
target page.

The current MMU backend is intentionally conservative:

- it rewrites the target user PTEs in kernel space to remove the requested
  access class from the guarded page
- it reports the requested guard address in `value0`; execute faults also
  report the actual faulting PC in `value1`
- after a hit the backend behaves as a one-shot guard until user space
  explicitly rearms or removes it
- `LKMDBG_CONTINUE_FLAG_REARM_HWPOINTS` does not automatically rearm MMU
  hwpoints; user space must call `LKMDBG_IOC_REARM_HWPOINT`
- only one MMU hwpoint is allowed per target page today

`LKMDBG_IOC_QUERY_HWPOINTS` exposes live hwpoint state bits:

- `ACTIVE`: the backend is currently armed
- `LATCHED`: a stop was delivered and explicit rearm or remove is required
- `LOST`: the original mapping disappeared and the hwpoint can no longer be
  rearmed
- `MUTATED`: the guarded mapping or effective PTEs no longer match the
  originally armed baseline

For MMU hwpoints, `MUTATED` is not necessarily a crash condition. `mprotect`
and similar mapping changes will intentionally surface as `MUTATED`, and some
external kernel access paths can disturb a `READ`-style MMU trap enough to
require remove-and-recreate instead of rearm.

Session fds are readable and pollable:

- `read()` now drains one or more whole `struct lkmdbg_event_record`
  instances per call when the caller provides a larger buffer
- `poll()`/`POLLIN` still signal that at least one queued event is available
- `event.seq` and `event.reserved0` let user space detect queue drops and
  recover after bursts

Shared definitions live in [lkmdbg_ioctl.h](/root/debugger_linux_lkm/include/lkmdbg_ioctl.h).

Example bootstrap flow:

```bash
sudo insmod lkmdbg.ko hook_proc_version=1
cc -O2 -Wall -Wextra -o tools/lkmdbg_open_session tools/lkmdbg_open_session.c tools/driver/bridge_c.c
sudo ./tools/lkmdbg_open_session
```

Example direct memory access flow:

```bash
cc -O2 -Wall -Wextra -pthread -o tools/lkmdbg_mem_test tools/lkmdbg_mem_test.c tools/driver/bridge_c.c tools/driver/bridge_events.c tools/driver/bridge_memory.c tools/driver/bridge_control.c
sudo ./tools/lkmdbg_mem_test selftest
sudo ./tools/lkmdbg_mem_test read <pid> <remote_addr_hex> <length>
sudo ./tools/lkmdbg_mem_test write <pid> <remote_addr_hex> <ascii_data>
sudo ./tools/lkmdbg_mem_test hwadd <pid> <tid> rwx <addr_hex> <len> mmu
sudo ./tools/lkmdbg_mem_test hwlist <pid>
```

The current `selftest` path exercises:

- batched target memory read/write and partial-progress accounting
- signal, syscall event/stop/control flows
- hardware and MMU break/watchpoints
- single-step stop delivery
- remote call, `x8` override, and remote thread create
- stealth controls and event queue behavior

## Android GKI note

The GitHub Actions workflow in this repository currently covers two generic CI checks:

- a host-side build against Ubuntu kernel headers
- a generic arm64 QEMU smoke test that boots a temporary upstream kernel and runs a basic `insmod`/`rmmod` cycle

That QEMU job is only meant to catch obvious runtime regressions in a plain arm64 Linux environment. It still does not validate Android 14 GKI 6.1 compatibility.

For actual Android bring-up you should point `KDIR` at the exact `android14-6.1` kernel build tree or exported module build headers used by your target device.

## License

This project is licensed under `GPL-2.0-or-later`. See [LICENSE](/root/debugger_linux_lkm/LICENSE).
