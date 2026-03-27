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

## Build And Test

- Build the module with `make`.
- Build against a specific tree with `make KDIR=/path/to/kernel/build`.
- Build user-space helpers with:
  - `cc -O2 -Wall -Wextra -o tools/lkmdbg_open_session tools/lkmdbg_open_session.c tools/driver/bridge_c.c`
  - `cc -O2 -Wall -Wextra -o tools/lkmdbg_stealth_ctl tools/lkmdbg_stealth_ctl.c`
  - `cc -O2 -Wall -Wextra -pthread -o tools/lkmdbg_mem_test tools/lkmdbg_mem_test.c tools/driver/bridge_c.c tools/driver/bridge_events.c tools/driver/bridge_memory.c tools/driver/bridge_control.c`

## Editing Guidelines

- Keep the hidden transport bootstrap narrow. `/proc/version` should remain a bootstrap entry, not a general command surface.
- Prefer adding new debugger features behind the session fd interface instead of growing bootstrap ioctls.
- Treat `core/lkmdbg_protect.c` as best-effort Android GKI compatibility code. Do not make module load depend on a bypass succeeding unless explicitly intended.
- When introducing hooks, isolate arch-specific patching logic from transport/session policy.
- Keep the first hook layer minimal: install/uninstall, relocation, trampoline, rollback. Add chaining or policy later.
- Keep user/kernel protocol changes synchronized in [include/lkmdbg_ioctl.h](/root/debugger_linux_lkm/include/lkmdbg_ioctl.h).

## Verification Expectations

- After structural or build changes, run `make`.
- After protocol changes, also rebuild the user-space helper that exercises the affected ioctl path.
- If Android-specific behavior cannot be validated locally, state that clearly in the final summary.
