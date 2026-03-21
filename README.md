# debugger_linux_lkm

Minimal out-of-tree kernel module scaffold for early arm64 Android GKI debugger experiments.

This repository intentionally starts with a small smoke-test module and a safe-first hidden transport scaffold:

- loads and unloads cleanly
- exports a `debugfs` status file at `/sys/kernel/debug/lkmdbg/status`
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
cc -O2 -Wall -Wextra -o tools/lkmdbg_open_session tools/lkmdbg_open_session.c
cc -O2 -Wall -Wextra -pthread -o tools/lkmdbg_mem_test tools/lkmdbg_mem_test.c
```

Load and inspect:

```bash
sudo insmod lkmdbg.ko
sudo cat /sys/kernel/debug/lkmdbg/status
sudo rmmod lkmdbg
```

Enable the hidden `/proc/version` ioctl transport:

```bash
sudo insmod lkmdbg.ko hook_proc_version=1
sudo cat /sys/kernel/debug/lkmdbg/status
```

Run the module-local inline hook smoke test on load:

```bash
sudo insmod lkmdbg.ko hook_selftest_mode=1
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

The current session ioctls include:

- `LKMDBG_IOC_GET_STATUS`
- `LKMDBG_IOC_RESET_SESSION`
- `LKMDBG_IOC_SET_TARGET`
- `LKMDBG_IOC_READ_MEM`
- `LKMDBG_IOC_WRITE_MEM`

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

Shared definitions live in [lkmdbg_ioctl.h](/root/debugger_linux_lkm/include/lkmdbg_ioctl.h).

Example bootstrap flow:

```bash
sudo insmod lkmdbg.ko hook_proc_version=1
cc -O2 -Wall -Wextra -o tools/lkmdbg_open_session tools/lkmdbg_open_session.c
sudo ./tools/lkmdbg_open_session
```

Example direct memory access flow:

```bash
cc -O2 -Wall -Wextra -pthread -o tools/lkmdbg_mem_test tools/lkmdbg_mem_test.c
sudo ./tools/lkmdbg_mem_test selftest
sudo ./tools/lkmdbg_mem_test read <pid> <remote_addr_hex> <length>
sudo ./tools/lkmdbg_mem_test write <pid> <remote_addr_hex> <ascii_data>
```

## Android GKI note

The GitHub Actions workflow in this repository currently covers two generic CI checks:

- a host-side build against Ubuntu kernel headers
- a generic arm64 QEMU smoke test that boots a temporary upstream kernel and runs a basic `insmod`/`rmmod` cycle

That QEMU job is only meant to catch obvious runtime regressions in a plain arm64 Linux environment. It still does not validate Android 14 GKI 6.1 compatibility.

For actual Android bring-up you should point `KDIR` at the exact `android14-6.1` kernel build tree or exported module build headers used by your target device.

## License

This project is licensed under `GPL-2.0-or-later`. See [LICENSE](/root/debugger_linux_lkm/LICENSE).
