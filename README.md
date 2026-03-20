# debugger_linux_lkm

Minimal out-of-tree kernel module scaffold for early arm64 Android GKI debugger experiments.

This repository intentionally starts with a small smoke-test module and a safe-first hidden transport scaffold:

- loads and unloads cleanly
- exports a `debugfs` status file at `/sys/kernel/debug/lkmdbg/status`
- can optionally clone-and-swap `/proc/version` inode `file_operations`
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
cc -O2 -Wall -Wextra -o tools/lkmdbg_mem_test tools/lkmdbg_mem_test.c
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
sudo insmod lkmdbg.ko hook_selftest=1
sudo cat /sys/kernel/debug/lkmdbg/status
```

This stage is intentionally conservative:

- normal `/proc/version` reads stay untouched
- the module clones the target inode `file_operations` and swaps the inode pointer
- it does not patch the shared operations table in place
- unload restores the original inode pointer

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
cc -O2 -Wall -Wextra -o tools/lkmdbg_mem_test tools/lkmdbg_mem_test.c
sudo ./tools/lkmdbg_mem_test selftest
sudo ./tools/lkmdbg_mem_test read <pid> <remote_addr_hex> <length>
sudo ./tools/lkmdbg_mem_test write <pid> <remote_addr_hex> <ascii_data>
```

## Android GKI note

The GitHub Actions workflow in this repository is only a host-side smoke test. It builds the module against Ubuntu kernel headers to catch obvious compile regressions. It does not validate Android 14 GKI 6.1 compatibility.

For actual Android bring-up you should point `KDIR` at the exact `android14-6.1` kernel build tree or exported module build headers used by your target device.
