# AGENTS.md

## Project Overview

This repository contains an out-of-tree Linux kernel module for early arm64 Android GKI debugger work.
The current design is intentionally conservative:

- `core/` owns module init, runtime symbol lookup, and hardening bypass helpers.
- `hook/` owns arch-specific inline hook and trampoline logic.
- `transport/` owns the hidden `/proc/version` bootstrap path and session fd lifecycle.
- `mem/` owns target memory operations.
- `ui/` owns debugfs status output.
- `include/` owns shared kernel/user protocol headers and internal declarations.
- `tools/` owns host-side CLI tools, examples, and reusable bridge helpers.
- `android/shared/` owns the pipe protocol shared by the Android app and root agent.
- `android/agent/` owns the root-side `lkmdbg-agent --stdio` bridge that talks to the kernel session fd API.
- `android/app/` owns the MD3 APK shell and Android-side session workflow UI.

## Build And Test

- Build the module with `make`.
- Build against a specific tree with `make KDIR=/path/to/kernel/build`.
- Build user-space helpers with:
  - `cc -O2 -Wall -Wextra -o tools/lkmdbg_open_session tools/lkmdbg_open_session.c tools/driver/bridge_c.c`
  - `cc -O2 -Wall -Wextra -o tools/lkmdbg_stealth_ctl tools/lkmdbg_stealth_ctl.c`
  - `cc -O2 -Wall -Wextra -pthread -o tools/lkmdbg_mem_test tools/lkmdbg_mem_test.c tools/driver/bridge_c.c tools/driver/bridge_events.c tools/driver/bridge_memory.c tools/driver/bridge_control.c`
  - `cc -O2 -Wall -Wextra -o tools/lkmdbg_sysrule_ctl tools/lkmdbg_sysrule_ctl.c tools/driver/bridge_c.c tools/driver/bridge_control.c`
  - `tools/examples/*.c` are one-feature-per-file runnable examples and should stay buildable with the same bridge C sources.
- Android CI lives in `.github/workflows/android-build.yml` and currently builds:
  - the host-check `android/agent/` binary with CMake
  - the debug APK from `android/app/`

## Editing Guidelines

- Keep the hidden transport bootstrap narrow. `/proc/version` should remain a bootstrap entry, not a general command surface.
- Prefer adding new debugger features behind the session fd interface instead of growing bootstrap ioctls.
- Treat `core/lkmdbg_protect.c` as best-effort Android GKI compatibility code. Do not make module load depend on a bypass succeeding unless explicitly intended.
- When introducing hooks, isolate arch-specific patching logic from transport/session policy.
- Keep the first hook layer minimal: install/uninstall, relocation, trampoline, rollback. Add chaining or policy later.
- Keep owner-proc hide on the current lookup-only procfs path (`proc_pid_lookup` + `proc_pid_readdir` + `proc_tgid_base_lookup` + `d_drop`) and avoid reintroducing `proc_pid_permission` hooking unless explicitly requested.
- Keep user/kernel protocol changes synchronized in [include/lkmdbg_ioctl.h](/root/debugger_linux_lkm/include/lkmdbg_ioctl.h).
- Keep the Android bridge narrow too: the APK should talk to the root agent over stdio pipe framing, and the agent should remain a thin adapter onto the existing kernel session-fd API.
- Do not move general debugger policy into `/proc/version` or into Android-only side channels when the session fd transport already models it.

## Verification Expectations

- After structural or build changes, run `make`.
- After protocol changes, also rebuild the user-space helper that exercises the affected ioctl path.
- After Android protocol changes, keep `android/shared/` and `android/agent/` frame sizes and field order aligned.
- After Android app or agent changes, check `.github/workflows/android-build.yml`.
- If Android-specific behavior cannot be validated locally, state that clearly in the final summary.
