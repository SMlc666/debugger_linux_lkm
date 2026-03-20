# debugger_linux_lkm

Minimal out-of-tree kernel module scaffold for early arm64 Android GKI debugger experiments.

This repository intentionally starts with a small smoke-test module and a safe-first hidden transport scaffold:

- loads and unloads cleanly
- exports a `debugfs` status file at `/sys/kernel/debug/lkmdbg/status`
- can optionally clone-and-swap `/proc/version` inode `file_operations`
- provides a stable place to add probe, breakpoint, and event plumbing next

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

This stage is intentionally conservative:

- normal `/proc/version` reads stay untouched
- the module clones the target inode `file_operations` and swaps the inode pointer
- it does not patch the shared operations table in place
- unload restores the original inode pointer

The hidden ioctl protocol now uses a bootstrap-plus-session model:

- `/proc/version` only handles `LKMDBG_IOC_OPEN_SESSION`
- that ioctl returns an anonymous session fd created with `anon_inode_getfd()`
- follow-up control ioctls run on the session fd instead of `/proc/version`

The current session ioctls include:

- `LKMDBG_IOC_GET_STATUS`
- `LKMDBG_IOC_RESET_SESSION`

Shared definitions live in [lkmdbg_ioctl.h](/root/debugger_linux_lkm/lkmdbg_ioctl.h).

Example bootstrap flow:

```bash
sudo insmod lkmdbg.ko hook_proc_version=1
cc -O2 -Wall -Wextra -o tools/lkmdbg_open_session tools/lkmdbg_open_session.c
sudo ./tools/lkmdbg_open_session
```

## Android GKI note

The GitHub Actions workflow in this repository is only a host-side smoke test. It builds the module against Ubuntu kernel headers to catch obvious compile regressions. It does not validate Android 14 GKI 6.1 compatibility.

For actual Android bring-up you should point `KDIR` at the exact `android14-6.1` kernel build tree or exported module build headers used by your target device.
