#include <linux/tracepoint.h>
#include <linux/string.h>

#include "lkmdbg_internal.h"

static bool lkmdbg_trace_fork_registered;
static bool lkmdbg_trace_exec_registered;
static bool lkmdbg_trace_exit_registered;
static bool lkmdbg_trace_signal_registered;
static bool lkmdbg_trace_sys_enter_registered;
static bool lkmdbg_trace_sys_exit_registered;
static struct tracepoint *lkmdbg_trace_fork_tp;
static struct tracepoint *lkmdbg_trace_exec_tp;
static struct tracepoint *lkmdbg_trace_exit_tp;
static struct tracepoint *lkmdbg_trace_signal_tp;
static struct tracepoint *lkmdbg_trace_sys_enter_tp;
static struct tracepoint *lkmdbg_trace_sys_exit_tp;

struct lkmdbg_tracepoint_lookup {
	const char *name;
	struct tracepoint *match;
};

static void lkmdbg_tracepoint_find_cb(struct tracepoint *tp, void *priv)
{
	struct lkmdbg_tracepoint_lookup *lookup = priv;

	if (lookup->match)
		return;
	if (strcmp(tp->name, lookup->name) == 0)
		lookup->match = tp;
}

static struct tracepoint *lkmdbg_find_tracepoint(const char *name)
{
	struct lkmdbg_tracepoint_lookup lookup = {
		.name = name,
	};

	if (!lkmdbg_symbols.for_each_kernel_tracepoint_sym)
		return NULL;

	lkmdbg_for_each_kernel_tracepoint_runtime(lkmdbg_tracepoint_find_cb,
						     &lookup);
	return lookup.match;
}

u32 lkmdbg_thread_tracepoint_phases(void)
{
	u32 phases = 0;

	if (READ_ONCE(lkmdbg_trace_sys_enter_registered))
		phases |= LKMDBG_SYSCALL_TRACE_PHASE_ENTER;
	if (READ_ONCE(lkmdbg_trace_sys_exit_registered))
		phases |= LKMDBG_SYSCALL_TRACE_PHASE_EXIT;

	return phases;
}

int lkmdbg_thread_trace_hooks_init(void)
{
	int ret;

	if (!lkmdbg_symbols.tracepoint_probe_register_sym) {
		lkmdbg_pr_info("lkmdbg: tracepoint register helper unavailable\n");
		return 0;
	}

	lkmdbg_trace_fork_tp = lkmdbg_find_tracepoint("sched_process_fork");
	if (lkmdbg_trace_fork_tp) {
		ret = lkmdbg_tracepoint_probe_register_runtime(
			lkmdbg_trace_fork_tp,
			(void *)lkmdbg_trace_sched_process_fork, NULL);
		if (!ret)
			lkmdbg_trace_fork_registered = true;
		else
			lkmdbg_pr_warn(
				"lkmdbg: sched_process_fork trace hook failed ret=%d\n",
				ret);
	} else {
		lkmdbg_pr_info(
			"lkmdbg: sched_process_fork tracepoint unavailable\n");
	}

	lkmdbg_trace_exec_tp = lkmdbg_find_tracepoint("sched_process_exec");
	if (lkmdbg_trace_exec_tp) {
		ret = lkmdbg_tracepoint_probe_register_runtime(
			lkmdbg_trace_exec_tp,
			(void *)lkmdbg_trace_sched_process_exec, NULL);
		if (!ret)
			lkmdbg_trace_exec_registered = true;
		else
			lkmdbg_pr_warn(
				"lkmdbg: sched_process_exec trace hook failed ret=%d\n",
				ret);
	} else {
		lkmdbg_pr_info(
			"lkmdbg: sched_process_exec tracepoint unavailable\n");
	}

	lkmdbg_trace_exit_tp = lkmdbg_find_tracepoint("sched_process_exit");
	if (lkmdbg_trace_exit_tp) {
		ret = lkmdbg_tracepoint_probe_register_runtime(
			lkmdbg_trace_exit_tp,
			(void *)lkmdbg_trace_sched_process_exit, NULL);
		if (!ret)
			lkmdbg_trace_exit_registered = true;
		else
			lkmdbg_pr_warn(
				"lkmdbg: sched_process_exit trace hook failed ret=%d\n",
				ret);
	} else {
		lkmdbg_pr_info(
			"lkmdbg: sched_process_exit tracepoint unavailable\n");
	}

	lkmdbg_trace_signal_tp = lkmdbg_find_tracepoint("signal_generate");
	if (lkmdbg_trace_signal_tp) {
		ret = lkmdbg_tracepoint_probe_register_runtime(
			lkmdbg_trace_signal_tp,
			(void *)lkmdbg_trace_signal_generate, NULL);
		if (!ret)
			lkmdbg_trace_signal_registered = true;
		else
			lkmdbg_pr_warn(
				"lkmdbg: signal_generate trace hook failed ret=%d\n",
				ret);
	} else {
		lkmdbg_pr_info("lkmdbg: signal_generate tracepoint unavailable\n");
	}

	lkmdbg_trace_sys_enter_tp = lkmdbg_find_tracepoint("sys_enter");
	if (lkmdbg_trace_sys_enter_tp) {
		ret = lkmdbg_tracepoint_probe_register_runtime(
			lkmdbg_trace_sys_enter_tp,
			(void *)lkmdbg_trace_raw_sys_enter, NULL);
		if (!ret)
			lkmdbg_trace_sys_enter_registered = true;
		else
			lkmdbg_pr_warn(
				"lkmdbg: raw sys_enter trace hook failed ret=%d\n",
				ret);
	} else {
		lkmdbg_pr_info("lkmdbg: raw sys_enter tracepoint unavailable\n");
	}

	lkmdbg_trace_sys_exit_tp = lkmdbg_find_tracepoint("sys_exit");
	if (lkmdbg_trace_sys_exit_tp) {
		ret = lkmdbg_tracepoint_probe_register_runtime(
			lkmdbg_trace_sys_exit_tp,
			(void *)lkmdbg_trace_raw_sys_exit, NULL);
		if (!ret)
			lkmdbg_trace_sys_exit_registered = true;
		else
			lkmdbg_pr_warn(
				"lkmdbg: raw sys_exit trace hook failed ret=%d\n",
				ret);
	} else {
		lkmdbg_pr_info("lkmdbg: raw sys_exit tracepoint unavailable\n");
	}

	return 0;
}

void lkmdbg_thread_trace_hooks_exit(void)
{
	if (lkmdbg_symbols.tracepoint_probe_unregister_sym &&
	    lkmdbg_trace_sys_exit_registered && lkmdbg_trace_sys_exit_tp) {
		lkmdbg_tracepoint_probe_unregister_runtime(
			lkmdbg_trace_sys_exit_tp,
			(void *)lkmdbg_trace_raw_sys_exit, NULL);
		lkmdbg_trace_sys_exit_registered = false;
	}
	lkmdbg_trace_sys_exit_tp = NULL;

	if (lkmdbg_symbols.tracepoint_probe_unregister_sym &&
	    lkmdbg_trace_sys_enter_registered && lkmdbg_trace_sys_enter_tp) {
		lkmdbg_tracepoint_probe_unregister_runtime(
			lkmdbg_trace_sys_enter_tp,
			(void *)lkmdbg_trace_raw_sys_enter, NULL);
		lkmdbg_trace_sys_enter_registered = false;
	}
	lkmdbg_trace_sys_enter_tp = NULL;

	if (lkmdbg_symbols.tracepoint_probe_unregister_sym &&
	    lkmdbg_trace_signal_registered && lkmdbg_trace_signal_tp) {
		lkmdbg_tracepoint_probe_unregister_runtime(
			lkmdbg_trace_signal_tp,
			(void *)lkmdbg_trace_signal_generate, NULL);
		lkmdbg_trace_signal_registered = false;
	}
	lkmdbg_trace_signal_tp = NULL;

	if (lkmdbg_symbols.tracepoint_probe_unregister_sym &&
	    lkmdbg_trace_exit_registered && lkmdbg_trace_exit_tp) {
		lkmdbg_tracepoint_probe_unregister_runtime(
			lkmdbg_trace_exit_tp,
			(void *)lkmdbg_trace_sched_process_exit, NULL);
		lkmdbg_trace_exit_registered = false;
	}
	lkmdbg_trace_exit_tp = NULL;

	if (lkmdbg_symbols.tracepoint_probe_unregister_sym &&
	    lkmdbg_trace_exec_registered && lkmdbg_trace_exec_tp) {
		lkmdbg_tracepoint_probe_unregister_runtime(
			lkmdbg_trace_exec_tp,
			(void *)lkmdbg_trace_sched_process_exec, NULL);
		lkmdbg_trace_exec_registered = false;
	}
	lkmdbg_trace_exec_tp = NULL;

	if (lkmdbg_symbols.tracepoint_probe_unregister_sym &&
	    lkmdbg_trace_fork_registered && lkmdbg_trace_fork_tp) {
		lkmdbg_tracepoint_probe_unregister_runtime(
			lkmdbg_trace_fork_tp,
			(void *)lkmdbg_trace_sched_process_fork, NULL);
		lkmdbg_trace_fork_registered = false;
	}
	lkmdbg_trace_fork_tp = NULL;
}
