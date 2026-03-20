# debugger_linux_lkm

Minimal out-of-tree kernel module scaffold for early arm64 Android GKI debugger experiments.

This repository intentionally starts with a small smoke-test module:

- loads and unloads cleanly
- exports a `debugfs` status file at `/sys/kernel/debug/lkmdbg/status`
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

Load and inspect:

```bash
sudo insmod lkmdbg.ko
sudo cat /sys/kernel/debug/lkmdbg/status
sudo rmmod lkmdbg
```

## Android GKI note

The GitHub Actions workflow in this repository is only a host-side smoke test. It builds the module against Ubuntu kernel headers to catch obvious compile regressions. It does not validate Android 14 GKI 6.1 compatibility.

For actual Android bring-up you should point `KDIR` at the exact `android14-6.1` kernel build tree or exported module build headers used by your target device.
