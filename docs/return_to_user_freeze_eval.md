# Return-To-User Freeze Evaluation

## Scope

This note evaluates candidate arm64 hook points for an in-kernel
"freeze-before-user-return" mechanism and compares them against the
existing upstream resume-to-user infrastructure.

Targets inspected:

- upstream Linux `v6.6`
- Android common `android14-6.1`

## Candidate Points

### 1. `arch/arm64/kernel/entry-common.c`

Upstream `v6.6`:

- `exit_to_user_mode_prepare(struct pt_regs *regs)`
- `asm_exit_to_user_mode(struct pt_regs *regs)`

Android common `android14-6.1`:

- `prepare_exit_to_user_mode(struct pt_regs *regs)`
- `asm_exit_to_user_mode(struct pt_regs *regs)`

Assessment:

- These are arch-local return-to-user transition helpers.
- They are `static` / `static __always_inline` and sit on a sensitive path.
- They are not good symbol-level inline-hook targets.
- They are also the wrong place to sleep arbitrarily because they are part of
  the low-level entry/exit choreography.

Verdict:

- Do not use these as first-choice hook targets.

### 2. `kernel/entry/common.c`

Both upstream `v6.6` and Android common `android14-6.1` contain:

- `static unsigned long exit_to_user_mode_loop(struct pt_regs *regs, ...)`
- `static void exit_to_user_mode_prepare(struct pt_regs *regs)`
- `void syscall_exit_to_user_mode_work(struct pt_regs *regs)`
- `noinstr void irqentry_exit_to_user_mode(struct pt_regs *regs)`

Assessment:

- `exit_to_user_mode_loop()` has attractive semantics because it already enables
  interrupts, may call `schedule()`, and processes return-to-user work.
- However it is `static`, so it is not a clean symbol target.
- `irqentry_exit_to_user_mode()` is `noinstr`, which makes it an especially bad
  place for a generic inline-hook based design.
- `syscall_exit_to_user_mode_work()` is global, but it only covers syscall exit
  and is therefore incomplete for a general freezer.

Verdict:

- `exit_to_user_mode_loop()` is the most attractive conceptual location but not
  a practical first implementation target.
- `irqentry_exit_to_user_mode()` should be avoided.
- `syscall_exit_to_user_mode_work()` is not sufficient on its own.

### 3. `arch/arm64/kernel/signal.c::do_notify_resume()`

Both upstream `v6.6` and Android common `android14-6.1` export:

- `void do_notify_resume(struct pt_regs *regs, unsigned long thread_flags)`

Observed behavior:

- Caller masks DAIF before entry.
- `do_notify_resume()` restores a runnable interrupt state internally.
- It already handles `_TIF_NEED_RESCHED`, signals, `resume_user_mode_work()`,
  and loops until `_TIF_WORK_MASK` is clear.

Assessment:

- This is a real symbol and is materially more stable than the static entry
  helpers above.
- If a hook-based design is forced, this is the least bad entry point.
- It still has downsides:
  - it only runs when thread work bits are set;
  - it is mixed with signal / fpstate / resume work, so ownership is not clean;
  - Android adds vendor hook logic here (`trace_android_vh_read_lazy_flag()`),
    so patching it means stepping onto a policy-heavy path.

Verdict:

- Best fallback hook target.
- Not the preferred primary design if we can avoid hooking.

### 4. `include/linux/resume_user_mode.h::resume_user_mode_work()`

Both inspected trees provide:

- `set_notify_resume(struct task_struct *task)`
- `resume_user_mode_work(struct pt_regs *regs)`

Observed behavior:

- `set_notify_resume()` sets `TIF_NOTIFY_RESUME` and `kick_process(task)`.
- `resume_user_mode_work()` clears `TIF_NOTIFY_RESUME`, runs `task_work_run()`,
  then performs memcg / blkcg / rseq resume work.
- The header explicitly documents this as a before-return-to-user mechanism.

Assessment:

- `resume_user_mode_work()` itself is `static inline`, so it is not a hook
  target.
- But this is the official kernel mechanism for "run something before this task
  returns to user mode".

Verdict:

- This is the best semantic fit.
- Do not hook it. Use the infrastructure it already exposes.

## Preferred Design: No Hook, Use Task Work

Both inspected trees also provide:

- `task_work_add(task, work, TWA_RESUME)`
- `task_work_run()`

Important behavior from `kernel/task_work.c`:

- `TWA_RESUME` runs work when the task exits the kernel and returns to user mode
  or before entering guest mode.
- `task_work_run()` is explicitly documented as running before the task returns
  to user mode or stops.

This gives a cleaner design than any inline hook:

1. queue a freezer callback on each target thread with `task_work_add(...,
   TWA_RESUME)`;
2. the helper sets `TIF_NOTIFY_RESUME` through `set_notify_resume(task)`;
3. `kick_process(task)` accelerates delivery if the task is currently in user
   mode;
4. when the thread reaches the kernel-to-user boundary, the callback runs in
   task context;
5. the callback can park the thread on a debugger-owned waitqueue until thaw.

Why this is better:

- no inline hook on a fragile return path;
- uses official upstream control flow instead of reverse-engineering entry code;
- works on both upstream `v6.6` and Android common `android14-6.1`;
- avoids `SIGSTOP` / `ptrace` visibility;
- much lower compatibility risk than patching `noinstr` or static entry helpers.

## Ranking

1. `task_work_add(..., TWA_RESUME)` + `set_notify_resume()`
   Recommended implementation path.
2. `do_notify_resume()`
   Acceptable fallback if a hook is still required.
3. `syscall_exit_to_user_mode_work()`
   Incomplete, syscall-only fallback.
4. `exit_to_user_mode_loop()` / `exit_to_user_mode_prepare()` / arch-local
   entry helpers
   Conceptually relevant, practically poor hook targets.
5. `irqentry_exit_to_user_mode()`
   Avoid.

## Recommendation

Implement the freezer around task-work-on-resume first. Only fall back to an
inline hook on `do_notify_resume()` if task-work based parking proves
insufficient for a concrete workload.
