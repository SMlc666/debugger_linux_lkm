#!/bin/busybox sh

set -eu

export PATH=/bin:/sbin

fail() {
	echo "LKMDBG_QEMU_SMOKE_FAIL:$1"
	poweroff -f
}

mount -t devtmpfs devtmpfs /dev || fail devtmpfs
mount -t proc proc /proc || fail proc
mount -t sysfs sysfs /sys || fail sysfs
mount -t debugfs debugfs /sys/kernel/debug || fail debugfs

echo "LKMDBG_QEMU_SMOKE_BEGIN"
uname -a || true

insmod /lkmdbg.ko hook_selftest_mode=1 || fail insmod_mode1
grep -q '^hook_selftest_enabled=1$' /sys/kernel/debug/lkmdbg/status ||
	fail status_selftest_enabled
grep -q '^hook_selftest_installed=0$' /sys/kernel/debug/lkmdbg/status ||
	fail status_selftest_not_installed
rmmod lkmdbg || fail rmmod_mode1

insmod /lkmdbg.ko hook_proc_version=1 || fail insmod_proc
cat /proc/version >/dev/null || fail proc_version_read
grep -q '^proc_version_hook_active=1$' /sys/kernel/debug/lkmdbg/status ||
	fail status_proc_hook
rmmod lkmdbg || fail rmmod_proc

echo "LKMDBG_QEMU_SMOKE_OK"
poweroff -f
