#include <linux/errno.h>
#include <linux/hashtable.h>
#include <linux/hw_breakpoint.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/mmap_lock.h>
#include <linux/module.h>
#include <linux/perf_event.h>
#include <linux/pid.h>
#include <linux/ptrace.h>
#include <linux/refcount.h>
#include <linux/sched.h>
#include <linux/signal.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/tracepoint.h>
#include <linux/uaccess.h>
#include <linux/uio.h>
#include <linux/unistd.h>

#ifdef CONFIG_ARM64
#include <asm/syscall.h>
#include <asm/esr.h>
#include <asm/pgtable.h>
#include <asm/processor.h>
#include <asm/tlbflush.h>
#include <asm/debug-monitors.h>
#else
#include <asm/syscall.h>
#endif

#include "lkmdbg_internal.h"

#define LKMDBG_HWPOINT_MAX_ENTRIES 64U
#define LKMDBG_MMU_BREAKPOINT_PAGE_SIZE PAGE_SIZE
#define LKMDBG_MM_EVENT_HASH_BITS 8

struct linux_binprm;
struct kernel_siginfo;

struct lkmdbg_hwpoint {
	struct list_head node;
	struct list_head mmu_node;
	struct lkmdbg_session *session;
	struct perf_event *event;
	u64 id;
	u64 addr;
	unsigned long page_addr;
	pid_t tgid;
	pid_t tid;
	u32 type;
	u32 len;
	u32 flags;
	u32 action_flags;
	u32 mmu_effective_type;
	u32 mmu_state;
	u64 mmu_baseline_pte;
	u64 mmu_expected_pte;
	u64 mmu_baseline_vm_flags;
	u64 trigger_hit_count;
	u64 cycle_hits;
	pid_t rearm_step_tgid;
	pid_t rearm_step_tid;
	atomic64_t hits;
	refcount_t refs;
	bool armed;
	bool mmu_disturbed;
	bool rearm_step_armed;
	bool oneshot_complete;
	atomic_t stop_latched;
};

struct lkmdbg_mm_event_pending {
	struct hlist_node node;
	pid_t tgid;
	pid_t tid;
	s32 syscall_nr;
	u64 arg0;
	u64 arg1;
	u64 arg2;
	u64 arg3;
};

#ifdef CONFIG_ARM64
typedef void (*lkmdbg_user_single_step_fn)(struct task_struct *task);
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 18, 0)
#define LKMDBG_ARM64_USER_STEP_HOOKS 1
typedef void (*lkmdbg_register_user_step_hook_fn)(struct step_hook *hook);
typedef void (*lkmdbg_unregister_user_step_hook_fn)(struct step_hook *hook);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 0, 0)
typedef unsigned long lkmdbg_step_hook_esr_t;
#else
typedef unsigned int lkmdbg_step_hook_esr_t;
#endif
#else
#define LKMDBG_ARM64_USER_STEP_HOOKS 0
#endif

static inline void lkmdbg_u128_split(__uint128_t value, u64 *lo, u64 *hi)
{
	*lo = (u64)value;
	*hi = (u64)(value >> 64);
}

static void lkmdbg_regs_arm64_export_stop(struct lkmdbg_regs_arm64 *dst,
					  const struct pt_regs *src)
{
	const struct user_fpsimd_state *fpsimd;
	unsigned int i;

	memset(dst, 0, sizeof(*dst));
	for (i = 0; i < ARRAY_SIZE(dst->regs); i++)
		dst->regs[i] = src->regs[i];
	dst->sp = src->sp;
	dst->pc = src->pc;
	dst->pstate = src->pstate;
	fpsimd = &current->thread.uw.fpsimd_state;
	dst->features |= LKMDBG_REGS_ARM64_FEATURE_FP;
	dst->fpsr = fpsimd->fpsr;
	dst->fpcr = fpsimd->fpcr;
	for (i = 0; i < ARRAY_SIZE(dst->vregs); i++) {
		u64 lo = 0;
		u64 hi = 0;

		lkmdbg_u128_split(fpsimd->vregs[i], &lo, &hi);
		dst->vregs[i].lo = lo;
		dst->vregs[i].hi = hi;
	}
}
#endif
typedef void (*lkmdbg_for_each_kernel_tracepoint_fn)(
	void (*fct)(struct tracepoint *tp, void *priv), void *priv);
typedef int (*lkmdbg_tracepoint_probe_register_fn)(struct tracepoint *tp,
						   void *probe, void *data);
typedef int (*lkmdbg_tracepoint_probe_unregister_fn)(struct tracepoint *tp,
						     void *probe, void *data);
typedef void (*lkmdbg_perf_event_disable_local_fn)(struct perf_event *event);
#ifdef CONFIG_ARM64
typedef void (*lkmdbg_invoke_syscall_fn)(struct pt_regs *regs,
					 unsigned int scno,
					 unsigned int sc_nr,
					 const syscall_fn_t syscall_table[]);
typedef long (*lkmdbg_invoke_syscall_inner_fn)(struct pt_regs *regs,
					       syscall_fn_t syscall_fn);
typedef void (*lkmdbg_do_el0_svc_fn)(struct pt_regs *regs);
#endif

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
static bool lkmdbg_perf_disable_missing_logged;
static DEFINE_HASHTABLE(lkmdbg_mm_event_pending_ht, LKMDBG_MM_EVENT_HASH_BITS);
static DEFINE_SPINLOCK(lkmdbg_mm_event_pending_lock);
#ifdef CONFIG_ARM64
static struct lkmdbg_inline_hook *lkmdbg_syscall_enter_hook;
static struct lkmdbg_hook_registry_entry *lkmdbg_syscall_enter_registry;
enum lkmdbg_syscall_enter_hook_kind {
	LKMDBG_SYSCALL_ENTER_HOOK_NONE = 0,
	LKMDBG_SYSCALL_ENTER_HOOK_INVOKE,
	LKMDBG_SYSCALL_ENTER_HOOK_INVOKE_INNER,
	LKMDBG_SYSCALL_ENTER_HOOK_DO_EL0_SVC,
};
static enum lkmdbg_syscall_enter_hook_kind lkmdbg_syscall_enter_hook_kind;
static lkmdbg_invoke_syscall_fn lkmdbg_invoke_syscall_orig;
static lkmdbg_invoke_syscall_inner_fn lkmdbg_invoke_syscall_inner_orig;
static lkmdbg_do_el0_svc_fn lkmdbg_do_el0_svc_orig;
static atomic_t lkmdbg_syscall_enter_inflight = ATOMIC_INIT(0);
static atomic_t lkmdbg_syscall_enter_users = ATOMIC_INIT(0);
static DECLARE_WAIT_QUEUE_HEAD(lkmdbg_syscall_enter_waitq);
static DEFINE_MUTEX(lkmdbg_syscall_enter_backend_lock);
static bool lkmdbg_syscall_enter_module_ref;
static bool lkmdbg_syscall_enter_teardown_queued;
static void lkmdbg_syscall_enter_teardown_workfn(struct work_struct *work);
static DECLARE_WORK(lkmdbg_syscall_enter_teardown_work,
		    lkmdbg_syscall_enter_teardown_workfn);
static LIST_HEAD(lkmdbg_mmu_hwpoint_list);
static DEFINE_SPINLOCK(lkmdbg_mmu_hwpoint_lock);
static struct lkmdbg_inline_hook *lkmdbg_do_page_fault_hook;
static struct lkmdbg_hook_registry_entry *lkmdbg_do_page_fault_registry;
enum lkmdbg_page_fault_hook_kind {
	LKMDBG_PAGE_FAULT_HOOK_NONE = 0,
	LKMDBG_PAGE_FAULT_HOOK_DO_PAGE_FAULT,
	LKMDBG_PAGE_FAULT_HOOK_DO_PAGE_FAULT_INNER,
};
static enum lkmdbg_page_fault_hook_kind lkmdbg_do_page_fault_hook_kind;
static int (*lkmdbg_do_page_fault_orig)(unsigned long far, unsigned long esr,
					struct pt_regs *regs);
static vm_fault_t (*lkmdbg_do_page_fault_inner_orig)(
	struct mm_struct *mm, struct vm_area_struct *vma, unsigned long addr,
	unsigned int mm_flags, unsigned long vm_flags, struct pt_regs *regs);
static atomic_t lkmdbg_do_page_fault_inflight = ATOMIC_INIT(0);
static DECLARE_WAIT_QUEUE_HEAD(lkmdbg_do_page_fault_waitq);
static struct lkmdbg_inline_hook *lkmdbg_do_el0_softstep_hook;
static struct lkmdbg_hook_registry_entry *lkmdbg_do_el0_softstep_registry;
static void (*lkmdbg_do_el0_softstep_orig)(unsigned long esr,
					   struct pt_regs *regs);
static atomic_t lkmdbg_do_el0_softstep_inflight = ATOMIC_INIT(0);
static DECLARE_WAIT_QUEUE_HEAD(lkmdbg_do_el0_softstep_waitq);
static struct lkmdbg_inline_hook *lkmdbg_process_vm_write_hook;
static struct lkmdbg_hook_registry_entry *lkmdbg_process_vm_write_registry;
static ssize_t (*lkmdbg_process_vm_rw_orig)(pid_t pid,
					    const struct iovec __user *lvec,
					    unsigned long liovcnt,
					    const struct iovec __user *rvec,
					    unsigned long riovcnt,
					    unsigned long flags,
					    int vm_write);
static ssize_t (*lkmdbg_do_sys_process_vm_writev_orig)(
	pid_t pid, const struct iovec __user *lvec, unsigned long liovcnt,
	const struct iovec __user *rvec, unsigned long riovcnt,
	unsigned long flags);
static atomic_t lkmdbg_process_vm_write_inflight = ATOMIC_INIT(0);
static DECLARE_WAIT_QUEUE_HEAD(lkmdbg_process_vm_write_waitq);
enum lkmdbg_process_vm_write_hook_kind {
	LKMDBG_PROCESS_VM_WRITE_HOOK_NONE = 0,
	LKMDBG_PROCESS_VM_WRITE_HOOK_PROCESS_VM_RW,
	LKMDBG_PROCESS_VM_WRITE_HOOK_DO_SYS,
};
static enum lkmdbg_process_vm_write_hook_kind
	lkmdbg_process_vm_write_hook_kind;
static struct lkmdbg_inline_hook *lkmdbg_remote_vm_write_hook;
static struct lkmdbg_hook_registry_entry *lkmdbg_remote_vm_write_registry;
static int (*lkmdbg_access_remote_vm_orig)(struct mm_struct *mm,
					       unsigned long addr, void *buf,
					       int len, unsigned int gup_flags);
static int (*lkmdbg_access_remote_vm_inner_orig)(struct mm_struct *mm,
						     unsigned long addr,
						     void *buf, int len,
						     unsigned int gup_flags);
static atomic_t lkmdbg_remote_vm_write_inflight = ATOMIC_INIT(0);
static DECLARE_WAIT_QUEUE_HEAD(lkmdbg_remote_vm_write_waitq);
enum lkmdbg_remote_vm_write_hook_kind {
	LKMDBG_REMOTE_VM_WRITE_HOOK_NONE = 0,
	LKMDBG_REMOTE_VM_WRITE_HOOK_ACCESS_REMOTE_VM,
	LKMDBG_REMOTE_VM_WRITE_HOOK_INNER,
};
static enum lkmdbg_remote_vm_write_hook_kind
	lkmdbg_remote_vm_write_hook_kind;
#endif

#if defined(CONFIG_ARM64) && LKMDBG_ARM64_USER_STEP_HOOKS
static int lkmdbg_user_step_handler(struct pt_regs *regs,
				    lkmdbg_step_hook_esr_t esr);
#endif
#ifdef CONFIG_ARM64
static bool lkmdbg_handle_user_single_step(struct pt_regs *regs);
static int lkmdbg_install_syscall_enter_backend(void);
static void lkmdbg_uninstall_syscall_enter_backend_locked(void);
static bool lkmdbg_syscall_enter_fallback_available(void);
static u32 lkmdbg_syscall_trace_tracepoint_phases(void);
static u32 lkmdbg_syscall_trace_fallback_phases(void);
static bool lkmdbg_syscall_trace_needs_hook_fallback(u32 mode, u32 phases);
static int lkmdbg_ensure_syscall_enter_backend(void);
static void lkmdbg_queue_syscall_enter_teardown(void);
static void lkmdbg_syscall_enter_broadcast_regs(struct pt_regs *regs, s32 nr);
static void lkmdbg_syscall_exit_broadcast_regs(struct pt_regs *regs, s32 nr,
					       s64 retval);
static void lkmdbg_invoke_syscall_replacement(
	struct pt_regs *regs, unsigned int scno, unsigned int sc_nr,
	const syscall_fn_t syscall_table[]);
static long lkmdbg_invoke_syscall_inner_replacement(
	struct pt_regs *regs, syscall_fn_t syscall_fn);
static void lkmdbg_do_el0_svc_replacement(struct pt_regs *regs);
static void lkmdbg_do_el0_softstep_replacement(unsigned long esr,
					       struct pt_regs *regs);
static int lkmdbg_do_page_fault_replacement(unsigned long far,
					    unsigned long esr,
					    struct pt_regs *regs);
static vm_fault_t lkmdbg_do_page_fault_inner_replacement(
	struct mm_struct *mm, struct vm_area_struct *vma, unsigned long addr,
	unsigned int mm_flags, unsigned long vm_flags, struct pt_regs *regs);
#if LKMDBG_ARM64_USER_STEP_HOOKS
static struct step_hook lkmdbg_user_step_hook = {
	.fn = lkmdbg_user_step_handler,
};
#endif
static bool lkmdbg_user_step_hook_registered;
#endif

static void __maybe_unused lkmdbg_hwpoint_get(struct lkmdbg_hwpoint *entry)
{
	refcount_inc(&entry->refs);
}

static void lkmdbg_hwpoint_put(struct lkmdbg_hwpoint *entry)
{
	if (refcount_dec_and_test(&entry->refs))
		kfree(entry);
}

static bool lkmdbg_hwpoint_hw_type_valid(u32 type)
{
	switch (type) {
	case LKMDBG_HWPOINT_TYPE_READ:
	case LKMDBG_HWPOINT_TYPE_WRITE:
	case LKMDBG_HWPOINT_TYPE_READWRITE:
	case LKMDBG_HWPOINT_TYPE_EXEC:
		return true;
	default:
		return false;
	}
}

static bool lkmdbg_hwpoint_mmu_type_valid(u32 type)
{
	if (!type)
		return false;

	return !(type & ~(LKMDBG_HWPOINT_TYPE_READ | LKMDBG_HWPOINT_TYPE_WRITE |
			  LKMDBG_HWPOINT_TYPE_EXEC));
}

static bool lkmdbg_hwpoint_len_valid(u32 len)
{
	return len >= 1 && len <= 8;
}

static bool lkmdbg_hwpoint_is_mmu(const struct lkmdbg_hwpoint *entry)
{
	return !!(entry->flags & LKMDBG_HWPOINT_FLAG_MMU);
}

static u64 lkmdbg_hwpoint_trigger_hit_count(u64 requested)
{
	return requested ? requested : 1;
}

static bool lkmdbg_hwpoint_threshold_reached(const struct lkmdbg_hwpoint *entry,
					     u64 cycle_hits)
{
	return cycle_hits >= lkmdbg_hwpoint_trigger_hit_count(
				    entry->trigger_hit_count);
}

static bool lkmdbg_hwpoint_is_oneshot(const struct lkmdbg_hwpoint *entry)
{
	return !!(entry->action_flags & LKMDBG_HWPOINT_ACTION_ONESHOT);
}

static bool lkmdbg_hwpoint_auto_continue(const struct lkmdbg_hwpoint *entry,
					 u64 cycle_hits)
{
	if (entry->flags & LKMDBG_HWPOINT_FLAG_COUNTER_MODE)
		return true;

	if (!lkmdbg_hwpoint_threshold_reached(entry, cycle_hits))
		return true;

	return !!(entry->action_flags & LKMDBG_HWPOINT_ACTION_AUTO_CONTINUE);
}

static void lkmdbg_hwpoint_reset_cycle(struct lkmdbg_hwpoint *entry)
{
	entry->cycle_hits = 0;
	entry->oneshot_complete = false;
	atomic_set(&entry->stop_latched, 0);
}

static void lkmdbg_hwpoint_finish_rearm(struct lkmdbg_hwpoint *entry)
{
	lkmdbg_hwpoint_reset_cycle(entry);
	WRITE_ONCE(entry->armed, true);
}

static int lkmdbg_validate_hwpoint_request(struct lkmdbg_hwpoint_request *req,
					   bool remove)
{
	if (req->version != LKMDBG_PROTO_VERSION || req->size != sizeof(*req))
		return -EINVAL;

	if (req->flags &
	    ~(LKMDBG_HWPOINT_FLAG_COUNTER_MODE | LKMDBG_HWPOINT_FLAG_MMU))
		return -EINVAL;

	if (req->action_flags &
	    ~(LKMDBG_HWPOINT_ACTION_ONESHOT |
	      LKMDBG_HWPOINT_ACTION_AUTO_CONTINUE))
		return -EINVAL;

	if (remove)
		return req->id ? 0 : -EINVAL;

	req->trigger_hit_count =
		lkmdbg_hwpoint_trigger_hit_count(req->trigger_hit_count);

	if (!req->addr || req->addr >= (u64)TASK_SIZE_MAX)
		return -EINVAL;

	if (!lkmdbg_hwpoint_len_valid(req->len))
		return -EINVAL;

	if (req->flags & LKMDBG_HWPOINT_FLAG_MMU) {
		if (!lkmdbg_hwpoint_mmu_type_valid(req->type) ||
		    (req->flags & LKMDBG_HWPOINT_FLAG_COUNTER_MODE))
			return -EINVAL;
	} else {
		if (!lkmdbg_hwpoint_hw_type_valid(req->type))
			return -EINVAL;
		if ((req->action_flags & LKMDBG_HWPOINT_ACTION_AUTO_CONTINUE) ||
		    req->trigger_hit_count != 1)
			return -EOPNOTSUPP;
	}

	if ((req->flags & LKMDBG_HWPOINT_FLAG_COUNTER_MODE) &&
	    (req->action_flags || req->trigger_hit_count != 1))
		return -EINVAL;

	if (req->tid < 0)
		return -EINVAL;

	return 0;
}

static int lkmdbg_validate_hwpoint_query(
	struct lkmdbg_hwpoint_query_request *req)
{
	if (req->version != LKMDBG_PROTO_VERSION || req->size != sizeof(*req))
		return -EINVAL;

	if (!req->entries_addr || !req->max_entries ||
	    req->max_entries > LKMDBG_HWPOINT_MAX_ENTRIES)
		return -EINVAL;

	if (req->flags)
		return -EINVAL;

	return 0;
}

static int lkmdbg_validate_single_step_request(
	struct lkmdbg_single_step_request *req)
{
	if (req->version != LKMDBG_PROTO_VERSION || req->size != sizeof(*req))
		return -EINVAL;

	if (req->flags)
		return -EINVAL;

	if (req->tid < 0)
		return -EINVAL;

	return 0;
}

static int lkmdbg_validate_signal_config(
	struct lkmdbg_signal_config_request *req)
{
	if (req->version != LKMDBG_PROTO_VERSION || req->size != sizeof(*req))
		return -EINVAL;

	if (req->flags & ~LKMDBG_SIGNAL_CONFIG_STOP)
		return -EINVAL;

	return 0;
}

static int lkmdbg_validate_syscall_trace_request(
	struct lkmdbg_syscall_trace_request *req)
{
	u32 valid_modes = LKMDBG_SYSCALL_TRACE_MODE_EVENT |
			  LKMDBG_SYSCALL_TRACE_MODE_STOP |
			  LKMDBG_SYSCALL_TRACE_MODE_CONTROL;
	u32 valid_phases = LKMDBG_SYSCALL_TRACE_PHASE_ENTER |
			    LKMDBG_SYSCALL_TRACE_PHASE_EXIT;

	if (req->version != LKMDBG_PROTO_VERSION || req->size != sizeof(*req))
		return -EINVAL;

	if (req->tid < 0 || req->syscall_nr < -1)
		return -EINVAL;

	if (req->mode & ~valid_modes)
		return -EINVAL;

	if (!req->mode) {
		if (req->phases)
			return -EINVAL;
		return 0;
	}

	if (!req->phases || (req->phases & ~valid_phases))
		return -EINVAL;

	if ((req->mode & LKMDBG_SYSCALL_TRACE_MODE_CONTROL) &&
	    ((req->mode & LKMDBG_SYSCALL_TRACE_MODE_STOP) ||
	     req->phases != LKMDBG_SYSCALL_TRACE_PHASE_ENTER))
		return -EINVAL;

	return 0;
}

static u32 lkmdbg_syscall_trace_tracepoint_phases(void)
{
	u32 phases = 0;

	if (READ_ONCE(lkmdbg_trace_sys_enter_registered))
		phases |= LKMDBG_SYSCALL_TRACE_PHASE_ENTER;
	if (READ_ONCE(lkmdbg_trace_sys_exit_registered))
		phases |= LKMDBG_SYSCALL_TRACE_PHASE_EXIT;
	return phases;
}

static u32 lkmdbg_syscall_trace_fallback_phases(void)
{
#ifdef CONFIG_ARM64
	if (!lkmdbg_syscall_enter_fallback_available())
		return 0;

	return (LKMDBG_SYSCALL_TRACE_PHASE_ENTER |
		LKMDBG_SYSCALL_TRACE_PHASE_EXIT) &
	       ~lkmdbg_syscall_trace_tracepoint_phases();
#else
	return 0;
#endif
}

static u32 lkmdbg_syscall_trace_supported_phases(void)
{
	u32 phases = lkmdbg_syscall_trace_tracepoint_phases();

#ifdef CONFIG_ARM64
	phases |= lkmdbg_syscall_trace_fallback_phases();
#endif

	return phases;
}

static u32 lkmdbg_syscall_trace_capability_flags(void)
{
	u32 flags = 0;

	if (lkmdbg_syscall_trace_tracepoint_phases())
		flags |= LKMDBG_SYSCALL_TRACE_FLAG_BACKEND_TRACEPOINT;
#ifdef CONFIG_ARM64
	if (lkmdbg_syscall_enter_fallback_available()) {
		flags |= LKMDBG_SYSCALL_TRACE_FLAG_BACKEND_ENTRY_HOOK;
		flags |= LKMDBG_SYSCALL_TRACE_FLAG_BACKEND_CONTROL;
	}
#endif

	return flags;
}

static struct lkmdbg_hwpoint *
lkmdbg_find_hwpoint_locked(struct lkmdbg_session *session, u64 id)
{
	struct lkmdbg_hwpoint *entry;

	list_for_each_entry(entry, &session->hwpoints, node) {
		if (entry->id == id)
			return entry;
	}

	return NULL;
}

static long lkmdbg_hwpoint_copy_reply(void __user *argp,
				      struct lkmdbg_hwpoint_request *req,
				      long ret)
{
	if (copy_to_user(argp, req, sizeof(*req)))
		return -EFAULT;

	return ret;
}

static long lkmdbg_hwpoint_query_copy_reply(
	void __user *argp, struct lkmdbg_hwpoint_query_request *req,
	struct lkmdbg_hwpoint_entry *entries, size_t entries_bytes)
{
	if (copy_to_user(u64_to_user_ptr(req->entries_addr), entries,
			 entries_bytes))
		return -EFAULT;

	if (copy_to_user(argp, req, sizeof(*req)))
		return -EFAULT;

	return 0;
}

static long lkmdbg_single_step_copy_reply(void __user *argp,
					  struct lkmdbg_single_step_request *req,
					  long ret)
{
	if (copy_to_user(argp, req, sizeof(*req)))
		return -EFAULT;

	return ret;
}

#ifdef CONFIG_ARM64
static u32 lkmdbg_mmu_fault_access_type(unsigned long esr)
{
	switch (ESR_ELx_EC(esr)) {
	case ESR_ELx_EC_IABT_LOW:
		return LKMDBG_HWPOINT_TYPE_EXEC;
	case ESR_ELx_EC_DABT_LOW:
		return (esr & ESR_ELx_WNR) ? LKMDBG_HWPOINT_TYPE_WRITE :
					     LKMDBG_HWPOINT_TYPE_READ;
	default:
		return 0;
	}
}

static bool lkmdbg_mmu_fault_from_user(unsigned long esr)
{
	switch (ESR_ELx_EC(esr)) {
	case ESR_ELx_EC_IABT_LOW:
	case ESR_ELx_EC_DABT_LOW:
		return true;
	default:
		return false;
	}
}

static int lkmdbg_mmu_prepare_trap_pte(pte_t current_pte,
				       unsigned long vm_flags,
				       u32 requested_type, pte_t *trap_out,
				       u32 *effective_type_out)
{
	pte_t trap = current_pte;
	u32 effective = 0;

	if (!trap_out || !effective_type_out)
		return -EINVAL;

	if (requested_type & LKMDBG_HWPOINT_TYPE_READ) {
		if (!lkmdbg_pte_allows_access(current_pte, vm_flags,
					      LKMDBG_HWPOINT_TYPE_READ))
			return -EACCES;

		if (!(requested_type & LKMDBG_HWPOINT_TYPE_EXEC) &&
		    lkmdbg_pte_allows_access(current_pte, vm_flags,
					     LKMDBG_HWPOINT_TYPE_EXEC)) {
			trap = lkmdbg_pte_make_exec_only(current_pte);
			effective = LKMDBG_HWPOINT_TYPE_READ |
				    LKMDBG_HWPOINT_TYPE_WRITE;
		} else {
			trap = lkmdbg_pte_make_protnone(current_pte);
			effective = LKMDBG_HWPOINT_TYPE_READ |
				    LKMDBG_HWPOINT_TYPE_WRITE |
				    LKMDBG_HWPOINT_TYPE_EXEC;
		}

		*trap_out = trap;
		*effective_type_out = effective;
		return 0;
	}

	if (requested_type & LKMDBG_HWPOINT_TYPE_WRITE) {
		if (!lkmdbg_pte_allows_access(current_pte, vm_flags,
					      LKMDBG_HWPOINT_TYPE_WRITE))
			return -EACCES;
		trap = pte_wrprotect(trap);
		effective |= LKMDBG_HWPOINT_TYPE_WRITE;
	}

	if (requested_type & LKMDBG_HWPOINT_TYPE_EXEC) {
		if (!lkmdbg_pte_allows_access(current_pte, vm_flags,
					      LKMDBG_HWPOINT_TYPE_EXEC))
			return -EACCES;
		trap = lkmdbg_pte_set_exec(trap, false);
		effective |= LKMDBG_HWPOINT_TYPE_EXEC;
	}

	*trap_out = trap;
	*effective_type_out = effective;
	return 0;
}

static int lkmdbg_mmu_apply_trap(struct mm_struct *mm,
				 struct lkmdbg_hwpoint *entry)
{
	pte_t current_pte;
	pte_t trap;
	unsigned long vm_flags = 0;
	int ret;
	u32 effective_type = 0;

	if (!mm)
		return -ESRCH;

	mmap_write_lock(mm);
	ret = lkmdbg_pte_read_locked(mm, entry->page_addr, &current_pte,
				     &vm_flags);
	if (!ret)
		ret = lkmdbg_mmu_prepare_trap_pte(current_pte, vm_flags,
						  entry->type, &trap,
						  &effective_type);
	if (!ret)
		ret = lkmdbg_pte_rewrite_locked(mm, entry->page_addr, trap, NULL,
						NULL);
	mmap_write_unlock(mm);

	if (!ret) {
		entry->mmu_baseline_pte = pte_val(current_pte);
		entry->mmu_baseline_vm_flags = vm_flags;
		entry->mmu_expected_pte = pte_val(trap);
		entry->mmu_effective_type = effective_type;
		entry->mmu_state &=
			~(LKMDBG_HWPOINT_STATE_LOST | LKMDBG_HWPOINT_STATE_MUTATED);
	}

	return ret;
}

static int lkmdbg_mmu_restore_baseline(struct mm_struct *mm,
				       struct lkmdbg_hwpoint *entry)
{
	int ret;

	if (!mm)
		return -ESRCH;

	mmap_write_lock(mm);
	ret = lkmdbg_pte_rewrite_locked(mm, entry->page_addr,
					__pte(entry->mmu_baseline_pte), NULL,
					NULL);
	mmap_write_unlock(mm);
	return ret;
}

static bool lkmdbg_mmu_can_restore_baseline(struct mm_struct *mm,
					    struct lkmdbg_hwpoint *entry)
{
	pte_t current_pte;
	unsigned long vm_flags = 0;
	pte_t baseline_pte;
	int ret;

	if (!mm || !entry)
		return false;

	ret = lkmdbg_pte_capture(mm, entry->page_addr, &current_pte, &vm_flags);
	if (ret)
		return false;
	if (vm_flags != entry->mmu_baseline_vm_flags)
		return false;

	baseline_pte = __pte(entry->mmu_baseline_pte);
	if (!pte_present(current_pte) || !pte_present(baseline_pte))
		return false;

	/*
	 * Only restore when the mapping still targets the same backing page.
	 * That lets us clean up permission-bit churn from legitimate writes on
	 * the original page without clobbering a COW/remap transition.
	 */
	return pte_pfn(current_pte) == pte_pfn(baseline_pte);
}

static struct lkmdbg_hwpoint *
lkmdbg_mmu_find_breakpoint_locked(pid_t tgid, unsigned long page_addr)
{
	struct lkmdbg_hwpoint *entry;

	list_for_each_entry(entry, &lkmdbg_mmu_hwpoint_list, mmu_node) {
		if (entry->tgid != tgid)
			continue;
		if (entry->page_addr != page_addr)
			continue;
		return entry;
	}

	return NULL;
}

static struct lkmdbg_hwpoint *
lkmdbg_mmu_find_step_rearm_locked(pid_t tgid, pid_t tid)
{
	struct lkmdbg_hwpoint *entry;

	list_for_each_entry(entry, &lkmdbg_mmu_hwpoint_list, mmu_node) {
		if (!entry->rearm_step_armed)
			continue;
		if (entry->rearm_step_tgid != tgid || entry->rearm_step_tid != tid)
			continue;
		return entry;
	}

	return NULL;
}

static bool lkmdbg_session_has_mmu_step_locked(struct lkmdbg_session *session)
{
	struct lkmdbg_hwpoint *entry;

	list_for_each_entry(entry, &session->hwpoints, node) {
		if (entry->rearm_step_armed)
			return true;
	}

	return false;
}

static void lkmdbg_mmu_mark_state(struct lkmdbg_hwpoint *entry, u32 set_bits,
				  u32 clear_bits)
{
	u32 state = READ_ONCE(entry->mmu_state);

	state &= ~clear_bits;
	state |= set_bits;
	WRITE_ONCE(entry->mmu_state, state);
}

static void lkmdbg_mmu_mark_disturbed(struct lkmdbg_hwpoint *entry)
{
	if (!entry)
		return;

	WRITE_ONCE(entry->mmu_disturbed, true);
}

static struct mm_struct *lkmdbg_mmu_get_mm_by_tgid(pid_t tgid)
{
	struct task_struct *task;
	struct mm_struct *mm;

	task = get_pid_task(find_vpid(tgid), PIDTYPE_TGID);
	if (!task)
		return NULL;

	mm = get_task_mm(task);
	put_task_struct(task);
	return mm;
}

static int lkmdbg_mmu_refresh_entry_state(struct lkmdbg_hwpoint *entry)
{
	struct mm_struct *mm;
	pte_t current_pte;
	unsigned long vm_flags = 0;
	int ret;

	if (!entry || !lkmdbg_hwpoint_is_mmu(entry))
		return 0;

	mm = lkmdbg_mmu_get_mm_by_tgid(entry->tgid);
	if (!mm) {
		entry->armed = false;
		lkmdbg_mmu_mark_state(entry, LKMDBG_HWPOINT_STATE_LOST,
				      LKMDBG_HWPOINT_STATE_MUTATED);
		return -ESRCH;
	}

	ret = lkmdbg_pte_capture(mm, entry->page_addr, &current_pte,
				 &vm_flags);
	if (ret) {
		entry->armed = false;
		lkmdbg_mmu_mark_state(entry, LKMDBG_HWPOINT_STATE_LOST,
				      LKMDBG_HWPOINT_STATE_MUTATED);
		mmput(mm);
		return ret;
	}

	if (vm_flags != entry->mmu_baseline_vm_flags) {
		entry->armed = false;
		lkmdbg_mmu_mark_disturbed(entry);
		lkmdbg_mmu_mark_state(entry, LKMDBG_HWPOINT_STATE_MUTATED,
				      LKMDBG_HWPOINT_STATE_LOST);
		mmput(mm);
		return -ESTALE;
	}

	if (entry->armed) {
		if (!lkmdbg_pte_equivalent(current_pte,
					   __pte(entry->mmu_expected_pte))) {
			entry->armed = false;
			lkmdbg_mmu_mark_disturbed(entry);
			if (!atomic_read(&entry->stop_latched) &&
			    lkmdbg_pte_equivalent(
				    current_pte,
				    __pte(entry->mmu_baseline_pte)) &&
			    !lkmdbg_mmu_apply_trap(mm, entry)) {
				entry->armed = true;
				lkmdbg_mmu_mark_state(entry, 0,
						      LKMDBG_HWPOINT_STATE_LOST);
				mmput(mm);
				return 0;
			}
			lkmdbg_mmu_mark_state(entry, LKMDBG_HWPOINT_STATE_MUTATED,
					      LKMDBG_HWPOINT_STATE_LOST);
			mmput(mm);
			return -ESTALE;
		}
		lkmdbg_mmu_mark_state(entry, 0,
				      LKMDBG_HWPOINT_STATE_LOST |
					      LKMDBG_HWPOINT_STATE_MUTATED);
		mmput(mm);
		return 0;
	}

	if (!lkmdbg_pte_equivalent(current_pte,
				   __pte(entry->mmu_baseline_pte))) {
		lkmdbg_mmu_mark_disturbed(entry);
		lkmdbg_mmu_mark_state(entry, LKMDBG_HWPOINT_STATE_MUTATED,
				      LKMDBG_HWPOINT_STATE_LOST);
		mmput(mm);
		return -ESTALE;
	}

	if (READ_ONCE(entry->mmu_disturbed) &&
	    !atomic_read(&entry->stop_latched) &&
	    !lkmdbg_mmu_apply_trap(mm, entry)) {
		entry->armed = true;
		lkmdbg_mmu_mark_state(entry, 0, LKMDBG_HWPOINT_STATE_LOST);
		mmput(mm);
		return 0;
	}

	lkmdbg_mmu_mark_state(entry, 0,
			      LKMDBG_HWPOINT_STATE_LOST |
				      LKMDBG_HWPOINT_STATE_MUTATED);
	mmput(mm);
	return 0;
}

static int lkmdbg_mmu_install_page_fault_hook(void)
{
	void *target;
	void *replacement;
	void *orig_fn = NULL;
	const char *name;
	int ret;

	if (lkmdbg_do_page_fault_hook)
		return 0;
	if (lkmdbg_symbols.do_page_fault_inner_sym) {
		target = (void *)lkmdbg_symbols.do_page_fault_inner_sym;
		replacement = lkmdbg_do_page_fault_inner_replacement;
		name = "__do_page_fault";
		lkmdbg_do_page_fault_hook_kind =
			LKMDBG_PAGE_FAULT_HOOK_DO_PAGE_FAULT_INNER;
	} else if (lkmdbg_symbols.do_page_fault_sym) {
		target = (void *)lkmdbg_symbols.do_page_fault_sym;
		replacement = lkmdbg_do_page_fault_replacement;
		name = "do_page_fault";
		lkmdbg_do_page_fault_hook_kind =
			LKMDBG_PAGE_FAULT_HOOK_DO_PAGE_FAULT;
	} else {
		return -ENOENT;
	}

	lkmdbg_do_page_fault_registry =
		lkmdbg_hook_registry_register(name, target, replacement);
	if (!lkmdbg_do_page_fault_registry)
		return -ENOMEM;

	ret = lkmdbg_hook_install(target, replacement,
				  &lkmdbg_do_page_fault_hook, &orig_fn);
	if (ret) {
		lkmdbg_hook_registry_unregister(lkmdbg_do_page_fault_registry, ret);
		lkmdbg_do_page_fault_registry = NULL;
		lkmdbg_do_page_fault_hook_kind = LKMDBG_PAGE_FAULT_HOOK_NONE;
		return ret;
	}

	if (lkmdbg_do_page_fault_hook_kind ==
	    LKMDBG_PAGE_FAULT_HOOK_DO_PAGE_FAULT_INNER)
		lkmdbg_do_page_fault_inner_orig = orig_fn;
	else
		lkmdbg_do_page_fault_orig = orig_fn;
	lkmdbg_hook_registry_mark_installed(lkmdbg_do_page_fault_registry, target,
						orig_fn, 0);
	lkmdbg_pr_info("lkmdbg: runtime hook active target=%s origin=%px trampoline=%px\n",
		name, orig_fn, target);
	return 0;
}

static int lkmdbg_install_user_single_step_backend(void)
{
#ifdef CONFIG_ARM64
	void *target;
	void *replacement;
	void *orig_fn;
	int ret;

#if LKMDBG_ARM64_USER_STEP_HOOKS
	if (lkmdbg_symbols.register_user_step_hook_sym &&
	    lkmdbg_symbols.unregister_user_step_hook_sym &&
	    lkmdbg_symbols.user_enable_single_step_sym &&
	    lkmdbg_symbols.user_disable_single_step_sym) {
		((lkmdbg_register_user_step_hook_fn)
			 lkmdbg_symbols.register_user_step_hook_sym)(
			&lkmdbg_user_step_hook);
		lkmdbg_user_step_hook_registered = true;
		return 0;
	}
#endif
	if (lkmdbg_do_el0_softstep_hook)
		return 0;
	if (lkmdbg_symbols.do_el0_softstep_sym) {
		target = (void *)lkmdbg_symbols.do_el0_softstep_sym;
		replacement = lkmdbg_do_el0_softstep_replacement;
		orig_fn = NULL;

		lkmdbg_do_el0_softstep_registry =
			lkmdbg_hook_registry_register("do_el0_softstep", target,
							 replacement);
		if (!lkmdbg_do_el0_softstep_registry)
			return -ENOMEM;

		ret = lkmdbg_hook_install(target, replacement,
					  &lkmdbg_do_el0_softstep_hook,
					  &orig_fn);
		if (ret) {
			lkmdbg_hook_registry_unregister(
				lkmdbg_do_el0_softstep_registry, ret);
			lkmdbg_do_el0_softstep_registry = NULL;
			return ret;
		}

		lkmdbg_do_el0_softstep_orig = orig_fn;
		lkmdbg_hook_registry_mark_installed(
			lkmdbg_do_el0_softstep_registry, target, orig_fn, 0);
		lkmdbg_pr_info("lkmdbg: runtime hook active target=%s origin=%px trampoline=%px\n",
			"do_el0_softstep", orig_fn, target);
		return 0;
	}
#endif
	return -ENOENT;
}

static bool lkmdbg_mmu_iov_overlaps_page(const struct iovec __user *rvec,
					 unsigned long riovcnt,
					 ssize_t bytes_done,
					 unsigned long page_addr)
{
	ssize_t remaining = bytes_done;
	unsigned long i;
	u64 page_end = (u64)page_addr + PAGE_SIZE;

	for (i = 0; i < riovcnt && remaining > 0; i++) {
		struct iovec iov;
		u64 start;
		u64 len;
		u64 end;

		if (copy_from_user(&iov, &rvec[i], sizeof(iov)))
			return false;
		if (!iov.iov_len)
			continue;

		start = (u64)(unsigned long)iov.iov_base;
		len = min_t(u64, (u64)iov.iov_len, (u64)remaining);
		end = start + len;
		if (end < start)
			end = ~0ULL;
		if (start < page_end && end > (u64)page_addr)
			return true;

		remaining -= (ssize_t)len;
	}

	return false;
}

static size_t lkmdbg_mmu_collect_tgid_entries(
	pid_t tgid, struct lkmdbg_hwpoint **entries, size_t max_entries)
{
	struct lkmdbg_hwpoint *entry;
	unsigned long irqflags;
	size_t count = 0;

	spin_lock_irqsave(&lkmdbg_mmu_hwpoint_lock, irqflags);
	list_for_each_entry(entry, &lkmdbg_mmu_hwpoint_list, mmu_node) {
		if (entry->tgid != tgid)
			continue;
		if (count == max_entries)
			break;
		lkmdbg_hwpoint_get(entry);
		entries[count++] = entry;
	}
	spin_unlock_irqrestore(&lkmdbg_mmu_hwpoint_lock, irqflags);

	return count;
}

static size_t lkmdbg_mmu_collect_all_entries(struct lkmdbg_hwpoint **entries,
					     size_t max_entries)
{
	struct lkmdbg_hwpoint *entry;
	unsigned long irqflags;
	size_t count = 0;

	spin_lock_irqsave(&lkmdbg_mmu_hwpoint_lock, irqflags);
	list_for_each_entry(entry, &lkmdbg_mmu_hwpoint_list, mmu_node) {
		if (count == max_entries)
			break;
		lkmdbg_hwpoint_get(entry);
		entries[count++] = entry;
	}
	spin_unlock_irqrestore(&lkmdbg_mmu_hwpoint_lock, irqflags);

	return count;
}

static bool lkmdbg_mmu_range_overlaps_page(unsigned long addr, size_t len,
					   unsigned long page_addr)
{
	if (!len)
		return false;

	if (addr <= page_addr)
		return (page_addr - addr) < len;

	return (addr - page_addr) < LKMDBG_MMU_BREAKPOINT_PAGE_SIZE;
}

static bool lkmdbg_mmu_entry_targets_mm(struct lkmdbg_hwpoint *entry,
					struct mm_struct *mm)
{
	struct mm_struct *entry_mm;
	bool match = false;

	if (!entry || !mm)
		return false;

	entry_mm = lkmdbg_mmu_get_mm_by_tgid(entry->tgid);
	if (!entry_mm)
		return false;

	match = entry_mm == mm;
	mmput(entry_mm);
	return match;
}

static void lkmdbg_mmu_recover_after_process_vm_write(
	pid_t tgid, const struct iovec __user *rvec, unsigned long riovcnt,
	ssize_t bytes_done)
{
	struct lkmdbg_hwpoint *entries[LKMDBG_HWPOINT_MAX_ENTRIES];
	size_t count;
	size_t i;

	if (bytes_done <= 0 || !rvec || !riovcnt)
		return;

	count = lkmdbg_mmu_collect_tgid_entries(tgid, entries,
						ARRAY_SIZE(entries));
	for (i = 0; i < count; i++) {
		struct lkmdbg_hwpoint *entry = entries[i];

		if (!lkmdbg_mmu_iov_overlaps_page(rvec, riovcnt, bytes_done,
						  entry->page_addr)) {
			lkmdbg_hwpoint_put(entry);
			continue;
		}

		lkmdbg_mmu_mark_disturbed(entry);
		lkmdbg_mmu_refresh_entry_state(entry);
		lkmdbg_hwpoint_put(entry);
	}
}

static void lkmdbg_mmu_recover_after_remote_vm_write(struct mm_struct *mm,
						     unsigned long addr,
						     int bytes_done)
{
	struct lkmdbg_hwpoint *entries[LKMDBG_HWPOINT_MAX_ENTRIES];
	size_t count;
	size_t i;

	if (!mm || bytes_done <= 0)
		return;

	count = lkmdbg_mmu_collect_all_entries(entries, ARRAY_SIZE(entries));
	for (i = 0; i < count; i++) {
		struct lkmdbg_hwpoint *entry = entries[i];

		if (!lkmdbg_mmu_entry_targets_mm(entry, mm) ||
		    !lkmdbg_mmu_range_overlaps_page(addr, bytes_done,
						    entry->page_addr)) {
			lkmdbg_hwpoint_put(entry);
			continue;
		}

		lkmdbg_mmu_mark_disturbed(entry);
		lkmdbg_mmu_refresh_entry_state(entry);
		lkmdbg_hwpoint_put(entry);
	}
}

static ssize_t lkmdbg_process_vm_rw_replacement(pid_t pid,
						const struct iovec __user *lvec,
						unsigned long liovcnt,
						const struct iovec __user *rvec,
						unsigned long riovcnt,
						unsigned long flags, int vm_write)
{
	ssize_t ret;

	atomic_inc(&lkmdbg_process_vm_write_inflight);
	ret = lkmdbg_process_vm_rw_orig ?
		      lkmdbg_process_vm_rw_orig(pid, lvec, liovcnt, rvec,
						riovcnt, flags, vm_write) :
		      -ENOENT;
	if (vm_write && ret > 0)
		lkmdbg_mmu_recover_after_process_vm_write(pid, rvec, riovcnt,
							  ret);
	if (atomic_dec_and_test(&lkmdbg_process_vm_write_inflight))
		wake_up_all(&lkmdbg_process_vm_write_waitq);
	return ret;
}

static ssize_t lkmdbg_do_sys_process_vm_writev_replacement(
	pid_t pid, const struct iovec __user *lvec, unsigned long liovcnt,
	const struct iovec __user *rvec, unsigned long riovcnt,
	unsigned long flags)
{
	ssize_t ret;

	atomic_inc(&lkmdbg_process_vm_write_inflight);
	ret = lkmdbg_do_sys_process_vm_writev_orig ?
		      lkmdbg_do_sys_process_vm_writev_orig(pid, lvec, liovcnt,
							   rvec, riovcnt,
							   flags) :
		      -ENOENT;
	if (ret > 0)
		lkmdbg_mmu_recover_after_process_vm_write(pid, rvec, riovcnt,
							  ret);
	if (atomic_dec_and_test(&lkmdbg_process_vm_write_inflight))
		wake_up_all(&lkmdbg_process_vm_write_waitq);
	return ret;
}

static int lkmdbg_access_remote_vm_replacement(struct mm_struct *mm,
					       unsigned long addr, void *buf,
					       int len, unsigned int gup_flags)
{
	int ret;

	atomic_inc(&lkmdbg_remote_vm_write_inflight);
	ret = lkmdbg_access_remote_vm_orig ?
		      lkmdbg_access_remote_vm_orig(mm, addr, buf, len, gup_flags) :
		      -ENOENT;
	if (ret > 0 && (gup_flags & FOLL_WRITE))
		lkmdbg_mmu_recover_after_remote_vm_write(mm, addr, ret);
	if (atomic_dec_and_test(&lkmdbg_remote_vm_write_inflight))
		wake_up_all(&lkmdbg_remote_vm_write_waitq);
	return ret;
}

static int lkmdbg_access_remote_vm_inner_replacement(struct mm_struct *mm,
						     unsigned long addr,
						     void *buf, int len,
						     unsigned int gup_flags)
{
	int ret;

	atomic_inc(&lkmdbg_remote_vm_write_inflight);
	ret = lkmdbg_access_remote_vm_inner_orig ?
		      lkmdbg_access_remote_vm_inner_orig(mm, addr, buf, len,
							 gup_flags) :
		      -ENOENT;
	if (ret > 0 && (gup_flags & FOLL_WRITE))
		lkmdbg_mmu_recover_after_remote_vm_write(mm, addr, ret);
	if (atomic_dec_and_test(&lkmdbg_remote_vm_write_inflight))
		wake_up_all(&lkmdbg_remote_vm_write_waitq);
	return ret;
}

static int lkmdbg_mmu_install_process_vm_write_hook(void)
{
	void *target;
	void *orig_fn = NULL;
	void *replacement;
	const char *name;
	int ret;

	if (lkmdbg_process_vm_write_hook)
		return 0;

	if (lkmdbg_symbols.process_vm_rw_sym) {
		target = (void *)lkmdbg_symbols.process_vm_rw_sym;
		replacement = lkmdbg_process_vm_rw_replacement;
		name = "process_vm_rw";
		lkmdbg_process_vm_write_hook_kind =
			LKMDBG_PROCESS_VM_WRITE_HOOK_PROCESS_VM_RW;
	} else if (lkmdbg_symbols.do_sys_process_vm_writev_sym) {
		target = (void *)lkmdbg_symbols.do_sys_process_vm_writev_sym;
		replacement = lkmdbg_do_sys_process_vm_writev_replacement;
		name = "__do_sys_process_vm_writev";
		lkmdbg_process_vm_write_hook_kind =
			LKMDBG_PROCESS_VM_WRITE_HOOK_DO_SYS;
	} else {
		return -ENOENT;
	}

	lkmdbg_process_vm_write_registry =
		lkmdbg_hook_registry_register(name, target, replacement);
	if (!lkmdbg_process_vm_write_registry)
		return -ENOMEM;

	ret = lkmdbg_hook_install(target, replacement,
				  &lkmdbg_process_vm_write_hook, &orig_fn);
	if (ret) {
		lkmdbg_hook_registry_unregister(lkmdbg_process_vm_write_registry,
						ret);
		lkmdbg_process_vm_write_registry = NULL;
		lkmdbg_process_vm_write_hook_kind =
			LKMDBG_PROCESS_VM_WRITE_HOOK_NONE;
		return ret;
	}

	if (lkmdbg_process_vm_write_hook_kind ==
	    LKMDBG_PROCESS_VM_WRITE_HOOK_PROCESS_VM_RW)
		lkmdbg_process_vm_rw_orig = orig_fn;
	else
		lkmdbg_do_sys_process_vm_writev_orig = orig_fn;

	lkmdbg_hook_registry_mark_installed(lkmdbg_process_vm_write_registry,
					    target, orig_fn, 0);
	lkmdbg_pr_info("lkmdbg: runtime hook active target=%s origin=%px trampoline=%px\n",
		name, orig_fn, target);
	return 0;
}

static int lkmdbg_mmu_install_remote_vm_write_hook(void)
{
	void *target;
	void *orig_fn = NULL;
	void *replacement;
	const char *name;
	int ret;

	if (lkmdbg_remote_vm_write_hook)
		return 0;

	if (lkmdbg_symbols.access_remote_vm_inner_sym) {
		target = (void *)lkmdbg_symbols.access_remote_vm_inner_sym;
		replacement = lkmdbg_access_remote_vm_inner_replacement;
		name = "__access_remote_vm";
		lkmdbg_remote_vm_write_hook_kind =
			LKMDBG_REMOTE_VM_WRITE_HOOK_INNER;
	} else if (lkmdbg_symbols.access_remote_vm_sym) {
		target = (void *)lkmdbg_symbols.access_remote_vm_sym;
		replacement = lkmdbg_access_remote_vm_replacement;
		name = "access_remote_vm";
		lkmdbg_remote_vm_write_hook_kind =
			LKMDBG_REMOTE_VM_WRITE_HOOK_ACCESS_REMOTE_VM;
	} else {
		return -ENOENT;
	}

	lkmdbg_remote_vm_write_registry =
		lkmdbg_hook_registry_register(name, target, replacement);
	if (!lkmdbg_remote_vm_write_registry)
		return -ENOMEM;

	ret = lkmdbg_hook_install(target, replacement,
				  &lkmdbg_remote_vm_write_hook, &orig_fn);
	if (ret) {
		lkmdbg_hook_registry_unregister(lkmdbg_remote_vm_write_registry,
						ret);
		lkmdbg_remote_vm_write_registry = NULL;
		lkmdbg_remote_vm_write_hook_kind =
			LKMDBG_REMOTE_VM_WRITE_HOOK_NONE;
		return ret;
	}

	if (lkmdbg_remote_vm_write_hook_kind ==
	    LKMDBG_REMOTE_VM_WRITE_HOOK_INNER)
		lkmdbg_access_remote_vm_inner_orig = orig_fn;
	else
		lkmdbg_access_remote_vm_orig = orig_fn;

	lkmdbg_hook_registry_mark_installed(lkmdbg_remote_vm_write_registry,
					    target, orig_fn, 0);
	lkmdbg_pr_info("lkmdbg: runtime hook active target=%s origin=%px trampoline=%px\n",
		name, orig_fn, target);
	return 0;
}

static void lkmdbg_disable_user_single_step_tid(pid_t tid)
{
	lkmdbg_user_single_step_fn disable_fn;
	struct task_struct *task;

	if (tid <= 0 || !lkmdbg_symbols.user_disable_single_step_sym)
		return;

	disable_fn =
		(lkmdbg_user_single_step_fn)lkmdbg_symbols.user_disable_single_step_sym;
	task = get_pid_task(find_vpid(tid), PIDTYPE_PID);
	if (!task)
		return;

	disable_fn(task);
	put_task_struct(task);
}
#endif

#ifndef CONFIG_ARM64
static int lkmdbg_unregister_mmu_hwpoint(struct lkmdbg_hwpoint *entry)
{
	(void)entry;
	return -EOPNOTSUPP;
}

static bool __maybe_unused
lkmdbg_session_has_mmu_step_locked(struct lkmdbg_session *session)
{
	(void)session;
	return false;
}
#endif

static void lkmdbg_hwpoint_fill_entry(struct lkmdbg_hwpoint_entry *dst,
				      const struct lkmdbg_hwpoint *src)
{
	memset(dst, 0, sizeof(*dst));
	dst->id = src->id;
	dst->addr = src->addr;
	dst->hits = atomic64_read(&src->hits);
	dst->trigger_hit_count =
		lkmdbg_hwpoint_trigger_hit_count(src->trigger_hit_count);
	dst->tgid = src->tgid;
	dst->tid = src->tid;
	dst->type = src->type;
	dst->len = src->len;
	dst->flags = src->flags;
	dst->action_flags = src->action_flags;
	dst->state = src->armed ? LKMDBG_HWPOINT_STATE_ACTIVE : 0;
	if (atomic_read(&src->stop_latched))
		dst->state |= LKMDBG_HWPOINT_STATE_LATCHED;
	if (lkmdbg_hwpoint_is_mmu(src)) {
		dst->state |= READ_ONCE(src->mmu_state);
		if (READ_ONCE(src->mmu_disturbed))
			dst->state |= LKMDBG_HWPOINT_STATE_MUTATED;
	}
}

static void lkmdbg_hwpoint_disable_stop_mode(struct lkmdbg_hwpoint *entry)
{
	lkmdbg_perf_event_disable_local_fn disable_local_fn;

	disable_local_fn = (lkmdbg_perf_event_disable_local_fn)
		lkmdbg_symbols.perf_event_disable_local_sym;
	if (!disable_local_fn) {
		if (!READ_ONCE(lkmdbg_perf_disable_missing_logged)) {
			WRITE_ONCE(lkmdbg_perf_disable_missing_logged, true);
			lkmdbg_pr_warn("lkmdbg: perf_event_disable_local unavailable, stop-mode hwpoints remain armed\n");
		}
		return;
	}

	disable_local_fn(entry->event);
	WRITE_ONCE(entry->armed, false);
}

#ifdef CONFIG_ARM64
static int lkmdbg_register_mmu_hwpoint(struct lkmdbg_session *session,
				       struct lkmdbg_hwpoint_request *req)
{
	struct lkmdbg_hwpoint *entry;
	struct task_struct *task = NULL;
	struct mm_struct *mm = NULL;
	unsigned long irqflags;
	int ret;
	u64 id;

	ret = lkmdbg_mmu_install_page_fault_hook();
	if (ret)
		return ret;

	ret = lkmdbg_mmu_install_process_vm_write_hook();
	if (ret && ret != -ENOENT)
		return ret;

	ret = lkmdbg_mmu_install_remote_vm_write_hook();
	if (ret && ret != -ENOENT)
		return ret;

	ret = lkmdbg_get_target_thread(session, req->tid, &task);
	if (ret)
		return ret;

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry) {
		put_task_struct(task);
		return -ENOMEM;
	}

	mutex_lock(&session->lock);
	session->next_hwpoint_id++;
	id = session->next_hwpoint_id;
	mutex_unlock(&session->lock);

	entry->session = session;
	entry->id = id;
	entry->addr = req->addr;
	entry->page_addr = req->addr & PAGE_MASK;
	entry->tgid = task->tgid;
	entry->tid = task->pid;
	entry->type = req->type;
	entry->len = req->len;
	entry->flags = req->flags;
	entry->action_flags = req->action_flags;
	entry->mmu_effective_type = 0;
	entry->mmu_state = 0;
	entry->mmu_baseline_pte = 0;
	entry->mmu_expected_pte = 0;
	entry->mmu_baseline_vm_flags = 0;
	entry->trigger_hit_count = req->trigger_hit_count;
	entry->cycle_hits = 0;
	entry->mmu_disturbed = false;
	atomic64_set(&entry->hits, 0);
	refcount_set(&entry->refs, 1);
	lkmdbg_hwpoint_reset_cycle(entry);
	INIT_LIST_HEAD(&entry->node);
	INIT_LIST_HEAD(&entry->mmu_node);
	put_task_struct(task);

	spin_lock_irqsave(&lkmdbg_mmu_hwpoint_lock, irqflags);
	if (lkmdbg_mmu_find_breakpoint_locked(entry->tgid, entry->page_addr)) {
		spin_unlock_irqrestore(&lkmdbg_mmu_hwpoint_lock, irqflags);
		kfree(entry);
		return -EEXIST;
	}
	spin_unlock_irqrestore(&lkmdbg_mmu_hwpoint_lock, irqflags);

	ret = lkmdbg_get_target_mm(session, &mm);
	if (ret) {
		kfree(entry);
		return ret;
	}

	ret = lkmdbg_mmu_apply_trap(mm, entry);
	mmput(mm);
	if (ret) {
		kfree(entry);
		return ret;
	}

	entry->armed = true;

	mutex_lock(&session->lock);
	list_add_tail(&entry->node, &session->hwpoints);
	mutex_unlock(&session->lock);

	spin_lock_irqsave(&lkmdbg_mmu_hwpoint_lock, irqflags);
	list_add_tail(&entry->mmu_node, &lkmdbg_mmu_hwpoint_list);
	spin_unlock_irqrestore(&lkmdbg_mmu_hwpoint_lock, irqflags);

	req->id = entry->id;
	req->tid = entry->tid;
	req->flags = entry->flags;
	req->trigger_hit_count = entry->trigger_hit_count;
	req->action_flags = entry->action_flags;
	return 0;
}

static void lkmdbg_unregister_mmu_hwpoint_global(struct lkmdbg_hwpoint *entry)
{
	unsigned long irqflags;

	spin_lock_irqsave(&lkmdbg_mmu_hwpoint_lock, irqflags);
	if (!list_empty(&entry->mmu_node))
		list_del_init(&entry->mmu_node);
	entry->rearm_step_armed = false;
	entry->rearm_step_tgid = 0;
	entry->rearm_step_tid = 0;
	spin_unlock_irqrestore(&lkmdbg_mmu_hwpoint_lock, irqflags);
}

static int lkmdbg_unregister_mmu_hwpoint(struct lkmdbg_hwpoint *entry)
{
	struct task_struct *task;
	struct mm_struct *mm = NULL;
	pid_t rearm_tid = entry->rearm_step_tid;
	u32 state;

	lkmdbg_mmu_refresh_entry_state(entry);
	state = READ_ONCE(entry->mmu_state);
	lkmdbg_unregister_mmu_hwpoint_global(entry);
	lkmdbg_disable_user_single_step_tid(rearm_tid);

	task = get_pid_task(find_vpid(entry->tgid), PIDTYPE_TGID);
	if (task) {
		mm = get_task_mm(task);
		put_task_struct(task);
	}
	if (!mm)
		return 0;

	if (!(state & LKMDBG_HWPOINT_STATE_LOST) &&
	    (entry->armed || lkmdbg_mmu_can_restore_baseline(mm, entry)))
		lkmdbg_mmu_restore_baseline(mm, entry);
	mmput(mm);
	return 0;
}

static int lkmdbg_rearm_mmu_hwpoint_locked(struct lkmdbg_hwpoint *entry)
{
	struct task_struct *task;
	struct mm_struct *mm = NULL;
	int ret;

	if (!lkmdbg_hwpoint_is_mmu(entry))
		return -EINVAL;
	ret = lkmdbg_mmu_refresh_entry_state(entry);
	if (ret)
		return ret;
	if (entry->rearm_step_armed)
		return -EBUSY;
	if (entry->armed && !atomic_read(&entry->stop_latched))
		return 0;

	task = get_pid_task(find_vpid(entry->tgid), PIDTYPE_TGID);
	if (!task)
		return -ESRCH;

	mm = get_task_mm(task);
	put_task_struct(task);
	if (!mm)
		return -ESRCH;

	ret = lkmdbg_mmu_apply_trap(mm, entry);
	if (ret) {
		mmput(mm);
		return ret;
	}

	lkmdbg_hwpoint_finish_rearm(entry);
	mmput(mm);
	return 0;
}
#endif

static void lkmdbg_hwpoint_event(struct perf_event *bp,
				 struct perf_sample_data *data,
				 struct pt_regs *regs)
{
	struct lkmdbg_hwpoint *entry = bp->overflow_handler_context;
	struct lkmdbg_regs_arm64 *stop_regs_ptr = NULL;
#ifdef CONFIG_ARM64
	struct lkmdbg_regs_arm64 stop_regs;
#endif
	u64 cycle_hits;
	u32 reason;
	u64 addr;
	u64 ip = 0;

	(void)data;

	if (!entry || !entry->session)
		return;

	reason = entry->type == LKMDBG_HWPOINT_TYPE_EXEC ?
		 LKMDBG_STOP_REASON_BREAKPOINT :
		 LKMDBG_STOP_REASON_WATCHPOINT;
	addr = hw_breakpoint_addr(bp);
	if (regs) {
		ip = instruction_pointer(regs);
#ifdef CONFIG_ARM64
		lkmdbg_regs_arm64_export_stop(&stop_regs, regs);
		stop_regs_ptr = &stop_regs;
#endif
	}

	atomic64_inc(&lkmdbg_state.hwpoint_callback_total);
	if (reason == LKMDBG_STOP_REASON_BREAKPOINT)
		atomic64_inc(&lkmdbg_state.breakpoint_callback_total);
	else if (reason == LKMDBG_STOP_REASON_WATCHPOINT)
		atomic64_inc(&lkmdbg_state.watchpoint_callback_total);
	atomic64_inc(&entry->hits);
	cycle_hits = ++entry->cycle_hits;
	WRITE_ONCE(lkmdbg_state.hwpoint_last_tgid, entry->tgid);
	WRITE_ONCE(lkmdbg_state.hwpoint_last_tid, current->pid);
	WRITE_ONCE(lkmdbg_state.hwpoint_last_reason, reason);
	WRITE_ONCE(lkmdbg_state.hwpoint_last_type, entry->type);
	WRITE_ONCE(lkmdbg_state.hwpoint_last_addr, addr);
	WRITE_ONCE(lkmdbg_state.hwpoint_last_ip, ip);

	if (!lkmdbg_hwpoint_threshold_reached(entry, cycle_hits))
		return;

	if (lkmdbg_hwpoint_is_oneshot(entry))
		entry->oneshot_complete = true;

	if (lkmdbg_hwpoint_auto_continue(entry, cycle_hits)) {
		if (entry->oneshot_complete)
			lkmdbg_hwpoint_disable_stop_mode(entry);
		return;
	}
	if (atomic_xchg(&entry->stop_latched, 1))
		return;
	lkmdbg_hwpoint_disable_stop_mode(entry);

	lkmdbg_session_request_async_stop(
		entry->session, reason, entry->tgid, current->pid, entry->type,
		LKMDBG_STOP_FLAG_REARM_REQUIRED, addr, ip ? ip : entry->id,
		stop_regs_ptr);
}

static int lkmdbg_register_hwpoint(struct lkmdbg_session *session,
				   struct lkmdbg_hwpoint_request *req)
{
	struct perf_event_attr attr;
	struct lkmdbg_hwpoint *entry;
	struct task_struct *task = NULL;
	u64 id;
	int ret;

#ifdef CONFIG_ARM64
	if (req->flags & LKMDBG_HWPOINT_FLAG_MMU)
		return lkmdbg_register_mmu_hwpoint(session, req);
#else
	if (req->flags & LKMDBG_HWPOINT_FLAG_MMU)
		return -EOPNOTSUPP;
#endif

	ret = lkmdbg_get_target_thread(session, req->tid, &task);
	if (ret)
		return ret;

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry) {
		put_task_struct(task);
		return -ENOMEM;
	}

	ptrace_breakpoint_init(&attr);
	attr.bp_addr = req->addr;
	attr.bp_len = req->len;
	attr.bp_type = req->type;
	attr.disabled = 1;

	mutex_lock(&session->lock);
	session->next_hwpoint_id++;
	id = session->next_hwpoint_id;
	mutex_unlock(&session->lock);

	entry->event = register_user_hw_breakpoint(&attr, lkmdbg_hwpoint_event,
						     entry, task);
	entry->session = session;
	entry->id = id;
	entry->addr = req->addr;
	entry->tgid = task->tgid;
	entry->tid = task->pid;
	entry->type = req->type;
	entry->len = req->len;
	entry->flags = req->flags;
	entry->action_flags = req->action_flags;
	entry->trigger_hit_count = req->trigger_hit_count;
	entry->cycle_hits = 0;
	atomic64_set(&entry->hits, 0);
	refcount_set(&entry->refs, 1);
	entry->armed = false;
	lkmdbg_hwpoint_reset_cycle(entry);
	put_task_struct(task);

	if (!entry->event) {
		kfree(entry);
		return -EOPNOTSUPP;
	}

	if (IS_ERR(entry->event)) {
		ret = PTR_ERR(entry->event);
		kfree(entry);
		return ret;
	}

	attr.disabled = 0;
	ret = modify_user_hw_breakpoint(entry->event, &attr);
	if (ret) {
		unregister_hw_breakpoint(entry->event);
		kfree(entry);
		return ret;
	}
	entry->armed = true;

	mutex_lock(&session->lock);
	list_add_tail(&entry->node, &session->hwpoints);
	mutex_unlock(&session->lock);

	req->id = entry->id;
	req->tid = entry->tid;
	req->trigger_hit_count = entry->trigger_hit_count;
	req->action_flags = entry->action_flags;
	return 0;
}

static int lkmdbg_unregister_hwpoint(struct lkmdbg_session *session, u64 id)
{
	struct lkmdbg_hwpoint *entry;
	bool mmu_exec = false;
	int ret = 0;

	mutex_lock(&session->lock);
	entry = lkmdbg_find_hwpoint_locked(session, id);
	if (!entry) {
		mutex_unlock(&session->lock);
		return -ENOENT;
	}

	list_del_init(&entry->node);
	mmu_exec = lkmdbg_hwpoint_is_mmu(entry);
	mutex_unlock(&session->lock);

	if (mmu_exec)
		ret = lkmdbg_unregister_mmu_hwpoint(entry);
	else
		unregister_hw_breakpoint(entry->event);
	lkmdbg_hwpoint_put(entry);
	return ret;
}

static int lkmdbg_rearm_hwpoint_locked(struct lkmdbg_hwpoint *entry)
{
	struct perf_event_attr attr;
	int ret;

#ifdef CONFIG_ARM64
	if (lkmdbg_hwpoint_is_mmu(entry))
		return lkmdbg_rearm_mmu_hwpoint_locked(entry);
#endif

	if (!entry->event)
		return -EINVAL;
	if (entry->flags & LKMDBG_HWPOINT_FLAG_COUNTER_MODE)
		return -EINVAL;
	if (entry->armed && !atomic_read(&entry->stop_latched))
		return 0;

	ptrace_breakpoint_init(&attr);
	attr.bp_addr = entry->addr;
	attr.bp_len = entry->len;
	attr.bp_type = entry->type;
	attr.disabled = 0;

	ret = modify_user_hw_breakpoint(entry->event, &attr);
	if (ret)
		return ret;

	lkmdbg_hwpoint_finish_rearm(entry);
	return 0;
}

static u32 lkmdbg_mm_event_type_for_syscall(s32 syscall_nr)
{
	switch (syscall_nr) {
#ifdef __NR_mmap
	case __NR_mmap:
		return LKMDBG_EVENT_TARGET_MMAP;
#endif
#ifdef __NR_munmap
	case __NR_munmap:
		return LKMDBG_EVENT_TARGET_MUNMAP;
#endif
#ifdef __NR_mprotect
	case __NR_mprotect:
		return LKMDBG_EVENT_TARGET_MPROTECT;
#endif
	default:
		return 0;
	}
}

static struct lkmdbg_mm_event_pending *
lkmdbg_mm_event_pending_find_locked(pid_t tgid, pid_t tid)
{
	struct lkmdbg_mm_event_pending *pending;

	hash_for_each_possible(lkmdbg_mm_event_pending_ht, pending, node,
			       (u32)tid) {
		if (pending->tid == tid && pending->tgid == tgid)
			return pending;
	}

	return NULL;
}

static void lkmdbg_mm_event_pending_drop_task(pid_t tgid, pid_t tid)
{
	struct lkmdbg_mm_event_pending *pending;
	unsigned long irqflags;

	spin_lock_irqsave(&lkmdbg_mm_event_pending_lock, irqflags);
	pending = lkmdbg_mm_event_pending_find_locked(tgid, tid);
	if (pending)
		hash_del(&pending->node);
	spin_unlock_irqrestore(&lkmdbg_mm_event_pending_lock, irqflags);

	kfree(pending);
}

static void lkmdbg_mm_event_pending_clear_all(void)
{
	struct lkmdbg_mm_event_pending *pending;
	struct hlist_node *tmp;
	unsigned int bucket;
	unsigned long irqflags;

	spin_lock_irqsave(&lkmdbg_mm_event_pending_lock, irqflags);
	hash_for_each_safe(lkmdbg_mm_event_pending_ht, bucket, tmp, pending,
			   node) {
		hash_del(&pending->node);
		kfree(pending);
	}
	spin_unlock_irqrestore(&lkmdbg_mm_event_pending_lock, irqflags);
}

static void lkmdbg_mm_event_track_sys_enter(struct pt_regs *regs, s32 syscall_nr)
{
	struct lkmdbg_mm_event_pending *pending;
	u32 event_type;
	unsigned long irqflags;

	if (!regs || current->tgid <= 0 || current->pid <= 0)
		return;

	event_type = lkmdbg_mm_event_type_for_syscall(syscall_nr);
	if (!event_type)
		return;

	if (!lkmdbg_session_has_target_event_type(current->tgid, event_type))
		return;

	spin_lock_irqsave(&lkmdbg_mm_event_pending_lock, irqflags);
	pending =
		lkmdbg_mm_event_pending_find_locked(current->tgid, current->pid);
	if (!pending) {
		pending = kzalloc(sizeof(*pending), GFP_ATOMIC);
		if (!pending) {
			spin_unlock_irqrestore(&lkmdbg_mm_event_pending_lock,
					       irqflags);
			return;
		}
		pending->tgid = current->tgid;
		pending->tid = current->pid;
		hash_add(lkmdbg_mm_event_pending_ht, &pending->node,
			 (u32)pending->tid);
	}

	pending->syscall_nr = syscall_nr;
#ifdef CONFIG_ARM64
	pending->arg0 = regs->regs[0];
	pending->arg1 = regs->regs[1];
	pending->arg2 = regs->regs[2];
	pending->arg3 = regs->regs[3];
#else
	pending->arg0 = 0;
	pending->arg1 = 0;
	pending->arg2 = 0;
	pending->arg3 = 0;
#endif
	spin_unlock_irqrestore(&lkmdbg_mm_event_pending_lock, irqflags);
}

static void lkmdbg_mm_event_emit_sys_exit(struct pt_regs *regs, s32 syscall_nr,
					  s64 retval)
{
	struct lkmdbg_mm_event_pending snapshot;
	struct lkmdbg_mm_event_pending *pending;
	u64 value0;
	u64 value1;
	unsigned long irqflags;
	u32 event_type;
	u32 code;
	u32 flags;

	if (!regs || current->tgid <= 0 || current->pid <= 0)
		return;

	event_type = lkmdbg_mm_event_type_for_syscall(syscall_nr);
	if (!event_type)
		return;

	spin_lock_irqsave(&lkmdbg_mm_event_pending_lock, irqflags);
	pending =
		lkmdbg_mm_event_pending_find_locked(current->tgid, current->pid);
	if (!pending || pending->syscall_nr != syscall_nr) {
		spin_unlock_irqrestore(&lkmdbg_mm_event_pending_lock, irqflags);
		return;
	}

	snapshot = *pending;
	hash_del(&pending->node);
	spin_unlock_irqrestore(&lkmdbg_mm_event_pending_lock, irqflags);
	kfree(pending);

	if (event_type == LKMDBG_EVENT_TARGET_MMAP) {
		if (retval < 0)
			return;
		code = (u32)snapshot.arg2;
		flags = (u32)snapshot.arg3;
		value0 = (u64)retval;
		value1 = snapshot.arg1;
	} else if (event_type == LKMDBG_EVENT_TARGET_MUNMAP) {
		if (retval)
			return;
		code = 0;
		flags = 0;
		value0 = snapshot.arg0;
		value1 = snapshot.arg1;
	} else if (event_type == LKMDBG_EVENT_TARGET_MPROTECT) {
		if (retval)
			return;
		code = (u32)snapshot.arg2;
		flags = 0;
		value0 = snapshot.arg0;
		value1 = snapshot.arg1;
	} else {
		return;
	}

	lkmdbg_session_broadcast_target_event(current->tgid, event_type, code,
					      current->pid, flags, value0,
					      value1);
}

static void lkmdbg_trace_sched_process_fork(void *data,
					    struct task_struct *parent,
					    struct task_struct *child)
{
	(void)data;

	if (!parent || !child)
		return;

	if (parent->tgid == child->tgid) {
		lkmdbg_session_broadcast_target_event(
			child->tgid, LKMDBG_EVENT_TARGET_CLONE,
			LKMDBG_TARGET_CLONE_THREAD, child->pid, 0,
			child->pid, parent->pid);
		return;
	}

	lkmdbg_session_broadcast_target_event(parent->tgid,
					      LKMDBG_EVENT_TARGET_CLONE,
					      LKMDBG_TARGET_CLONE_PROCESS,
					      child->pid, 0, child->tgid,
					      parent->pid);
}

static void lkmdbg_trace_sched_process_exec(void *data, struct task_struct *p,
					    pid_t old_pid,
					    struct linux_binprm *bprm)
{
	(void)data;
	(void)bprm;

	if (!p)
		return;

	lkmdbg_session_broadcast_target_event(p->tgid, LKMDBG_EVENT_TARGET_EXEC,
					      0, p->pid, 0, old_pid, 0);
}

static void lkmdbg_trace_sched_process_exit(void *data, struct task_struct *p)
{
	(void)data;

	if (!p)
		return;

	lkmdbg_mm_event_pending_drop_task(p->tgid, p->pid);
	lkmdbg_session_broadcast_target_event(p->tgid, LKMDBG_EVENT_TARGET_EXIT,
					      0, p->pid, 0,
					      (u64)(unsigned int)p->exit_code,
					      0);
}

static void lkmdbg_trace_signal_generate(void *data, int sig,
					 struct kernel_siginfo *info,
					 struct task_struct *task, int group,
					 int result)
{
	u64 siginfo_code = 0;

	(void)data;

	if (!task)
		return;

	if (info && info != SEND_SIG_NOINFO && info != SEND_SIG_PRIV)
		siginfo_code = (u64)(u32)info->si_code;

	lkmdbg_session_broadcast_signal_event(
		task->tgid, (u32)sig, task->pid,
		group ? LKMDBG_SIGNAL_EVENT_GROUP : 0, siginfo_code, result);
}

static void lkmdbg_trace_raw_sys_enter(void *data, struct pt_regs *regs,
				       long id)
{
	struct lkmdbg_regs_arm64 *stop_regs_ptr = NULL;
#ifdef CONFIG_ARM64
	struct lkmdbg_regs_arm64 stop_regs;
#endif

	(void)data;

	if (!regs || current->tgid <= 0 || current->pid <= 0 || id < 0)
		return;

	lkmdbg_mm_event_track_sys_enter(regs, (s32)id);

#ifdef CONFIG_ARM64
	lkmdbg_regs_arm64_export_stop(&stop_regs, regs);
	stop_regs_ptr = &stop_regs;
#endif
	lkmdbg_session_broadcast_syscall_event(
		current->tgid, current->pid, LKMDBG_SYSCALL_TRACE_PHASE_ENTER,
		(s32)id, 0, stop_regs_ptr);
}

static void lkmdbg_trace_raw_sys_exit(void *data, struct pt_regs *regs,
				      long ret)
{
	struct lkmdbg_regs_arm64 *stop_regs_ptr = NULL;
	long nr;
#ifdef CONFIG_ARM64
	struct lkmdbg_regs_arm64 stop_regs;
#endif

	(void)data;

	if (!regs || current->tgid <= 0 || current->pid <= 0)
		return;

	nr = syscall_get_nr(current, regs);
	if (nr < 0)
		return;

	lkmdbg_mm_event_emit_sys_exit(regs, (s32)nr, (s64)ret);

#ifdef CONFIG_ARM64
	lkmdbg_regs_arm64_export_stop(&stop_regs, regs);
	stop_regs_ptr = &stop_regs;
#endif
	lkmdbg_session_broadcast_syscall_event(
		current->tgid, current->pid, LKMDBG_SYSCALL_TRACE_PHASE_EXIT,
		(s32)nr, (s64)ret, stop_regs_ptr);
}

#ifdef CONFIG_ARM64
static void lkmdbg_syscall_enter_broadcast_regs(struct pt_regs *regs, s32 nr)
{
	struct lkmdbg_regs_arm64 stop_regs;
	struct lkmdbg_regs_arm64 *stop_regs_ptr = NULL;

	if (!regs || current->tgid <= 0 || current->pid <= 0 || nr < 0 ||
	    READ_ONCE(lkmdbg_trace_sys_enter_registered))
		return;

	lkmdbg_mm_event_track_sys_enter(regs, nr);

	lkmdbg_regs_arm64_export_stop(&stop_regs, regs);
	stop_regs_ptr = &stop_regs;
	lkmdbg_session_broadcast_syscall_event(
		current->tgid, current->pid, LKMDBG_SYSCALL_TRACE_PHASE_ENTER,
		nr, 0, stop_regs_ptr);
}

static void lkmdbg_syscall_exit_broadcast_regs(struct pt_regs *regs, s32 nr,
					       s64 retval)
{
	struct lkmdbg_regs_arm64 stop_regs;
	struct lkmdbg_regs_arm64 *stop_regs_ptr = NULL;

	if (!regs || current->tgid <= 0 || current->pid <= 0 || nr < 0 ||
	    READ_ONCE(lkmdbg_trace_sys_exit_registered))
		return;

	lkmdbg_mm_event_emit_sys_exit(regs, nr, retval);

	lkmdbg_regs_arm64_export_stop(&stop_regs, regs);
	stop_regs_ptr = &stop_regs;
	lkmdbg_session_broadcast_syscall_event(
		current->tgid, current->pid, LKMDBG_SYSCALL_TRACE_PHASE_EXIT,
		nr, retval, stop_regs_ptr);
}

static void lkmdbg_invoke_syscall_replacement(
	struct pt_regs *regs, unsigned int scno, unsigned int sc_nr,
	const syscall_fn_t syscall_table[])
{
	bool skip = false;
	s64 retval = 0;
	s32 nr = (s32)scno;

	atomic_inc(&lkmdbg_syscall_enter_inflight);

	lkmdbg_control_syscall_entry(regs, &nr, true, &skip, &retval);
	lkmdbg_syscall_enter_broadcast_regs(regs, nr);
	if (!skip && lkmdbg_invoke_syscall_orig)
		lkmdbg_invoke_syscall_orig(regs, (unsigned int)nr, sc_nr,
					   syscall_table);
	if (regs)
		retval = (s64)regs->regs[0];
	lkmdbg_syscall_exit_broadcast_regs(regs, nr, retval);

	if (atomic_dec_and_test(&lkmdbg_syscall_enter_inflight))
		wake_up_all(&lkmdbg_syscall_enter_waitq);
}

static long lkmdbg_invoke_syscall_inner_replacement(
	struct pt_regs *regs, syscall_fn_t syscall_fn)
{
	bool skip = false;
	s64 control_ret = 0;
	long ret = 0;
	s32 nr = -1;

	atomic_inc(&lkmdbg_syscall_enter_inflight);

	if (regs)
		nr = (s32)syscall_get_nr(current, regs);
	lkmdbg_control_syscall_entry(regs, &nr, false, &skip, &control_ret);
	lkmdbg_syscall_enter_broadcast_regs(regs, nr);
	if (!skip && lkmdbg_invoke_syscall_inner_orig)
		ret = lkmdbg_invoke_syscall_inner_orig(regs, syscall_fn);
	else if (skip)
		ret = (long)control_ret;
	lkmdbg_syscall_exit_broadcast_regs(regs, nr, ret);

	if (atomic_dec_and_test(&lkmdbg_syscall_enter_inflight))
		wake_up_all(&lkmdbg_syscall_enter_waitq);

	return ret;
}

static void lkmdbg_do_el0_svc_replacement(struct pt_regs *regs)
{
	s32 nr = -1;
	bool skip = false;
	s64 retval = 0;

	atomic_inc(&lkmdbg_syscall_enter_inflight);

	if (regs)
		nr = (s32)regs->regs[8];
	lkmdbg_control_syscall_entry(regs, &nr, true, &skip, &retval);
	lkmdbg_syscall_enter_broadcast_regs(regs, nr);
	if (!skip && lkmdbg_do_el0_svc_orig)
		lkmdbg_do_el0_svc_orig(regs);
	if (regs)
		retval = (s64)regs->regs[0];
	lkmdbg_syscall_exit_broadcast_regs(regs, nr, retval);

	if (atomic_dec_and_test(&lkmdbg_syscall_enter_inflight))
		wake_up_all(&lkmdbg_syscall_enter_waitq);
}
#endif

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
	lkmdbg_for_each_kernel_tracepoint_fn for_each_fn;

	for_each_fn = (lkmdbg_for_each_kernel_tracepoint_fn)
		lkmdbg_symbols.for_each_kernel_tracepoint_sym;
	if (!for_each_fn)
		return NULL;

	for_each_fn(lkmdbg_tracepoint_find_cb, &lookup);
	return lookup.match;
}

#ifdef CONFIG_ARM64
static bool lkmdbg_user_single_step_backend_ready(void)
{
	if (LKMDBG_ARM64_USER_STEP_HOOKS && lkmdbg_user_step_hook_registered)
		return true;

	return lkmdbg_do_el0_softstep_hook != NULL;
}

static int lkmdbg_mmu_queue_step_rearm(struct lkmdbg_hwpoint *entry)
{
	lkmdbg_user_single_step_fn enable_fn;
	unsigned long irqflags;

	if (!entry)
		return -EINVAL;
	if (!lkmdbg_user_single_step_backend_ready())
		return -EOPNOTSUPP;

	enable_fn =
		(lkmdbg_user_single_step_fn)lkmdbg_symbols.user_enable_single_step_sym;
	if (!enable_fn)
		return -EOPNOTSUPP;

	spin_lock_irqsave(&lkmdbg_mmu_hwpoint_lock, irqflags);
	if (entry->rearm_step_armed) {
		spin_unlock_irqrestore(&lkmdbg_mmu_hwpoint_lock, irqflags);
		return -EBUSY;
	}

	entry->rearm_step_armed = true;
	entry->rearm_step_tgid = current->tgid;
	entry->rearm_step_tid = current->pid;
	spin_unlock_irqrestore(&lkmdbg_mmu_hwpoint_lock, irqflags);

	enable_fn(current);
	return 0;
}
#endif

#ifdef CONFIG_ARM64
static bool lkmdbg_mmu_try_handle_fault(struct mm_struct *mm,
					unsigned long addr, u32 actual_type,
					struct pt_regs *regs)
{
	struct lkmdbg_hwpoint *entry = NULL;
	struct lkmdbg_regs_arm64 stop_regs;
	pte_t current_pte;
	u64 cycle_hits;
	u64 ip;
	u32 reason;
	unsigned long vm_flags = 0;
	unsigned long irqflags;
	int ret;

	if (!mm || !regs || !actual_type)
		return false;

	ip = instruction_pointer(regs);
	spin_lock_irqsave(&lkmdbg_mmu_hwpoint_lock, irqflags);
	entry = lkmdbg_mmu_find_breakpoint_locked(current->tgid,
						  addr & PAGE_MASK);
	if (entry)
		lkmdbg_hwpoint_get(entry);
	spin_unlock_irqrestore(&lkmdbg_mmu_hwpoint_lock, irqflags);
	if (!entry)
		return false;

	ret = lkmdbg_pte_capture(mm, entry->page_addr, &current_pte, &vm_flags);
	if (ret) {
		entry->armed = false;
		lkmdbg_mmu_mark_state(entry, LKMDBG_HWPOINT_STATE_LOST,
				      LKMDBG_HWPOINT_STATE_MUTATED);
		lkmdbg_hwpoint_put(entry);
		return false;
	}

	if (!entry->armed ||
	    !lkmdbg_pte_equivalent(current_pte,
				   __pte(entry->mmu_expected_pte))) {
		/* Recover the narrow window where the trap is visible before
		 * the software-armed flag is published.
		 */
		if (!entry->armed &&
		    !atomic_read(&entry->stop_latched) &&
		    lkmdbg_pte_equivalent(current_pte,
					 __pte(entry->mmu_expected_pte))) {
			entry->armed = true;
			lkmdbg_mmu_mark_state(
				entry, 0,
				LKMDBG_HWPOINT_STATE_LOST |
					LKMDBG_HWPOINT_STATE_MUTATED);
		}
		if (entry->armed)
			goto armed_ok;
		entry->armed = false;
		lkmdbg_mmu_mark_state(entry, LKMDBG_HWPOINT_STATE_MUTATED,
				      LKMDBG_HWPOINT_STATE_LOST);
		lkmdbg_hwpoint_put(entry);
		return false;
	}

armed_ok:
	ret = lkmdbg_mmu_restore_baseline(mm, entry);
	if (ret) {
		entry->armed = false;
		lkmdbg_mmu_mark_state(entry, LKMDBG_HWPOINT_STATE_LOST,
				      LKMDBG_HWPOINT_STATE_MUTATED);
		lkmdbg_hwpoint_put(entry);
		return false;
	}

	entry->armed = false;
	if (!(entry->type & actual_type)) {
		if (!lkmdbg_pte_allows_access(__pte(entry->mmu_baseline_pte),
					      entry->mmu_baseline_vm_flags,
					      actual_type)) {
			lkmdbg_hwpoint_put(entry);
			return false;
		}

		if (!lkmdbg_mmu_queue_step_rearm(entry)) {
			lkmdbg_hwpoint_put(entry);
			return true;
		}

		lkmdbg_hwpoint_put(entry);
		return false;
	}

	reason = actual_type == LKMDBG_HWPOINT_TYPE_EXEC ?
			 LKMDBG_STOP_REASON_BREAKPOINT :
			 LKMDBG_STOP_REASON_WATCHPOINT;
	atomic64_inc(&lkmdbg_state.hwpoint_callback_total);
	if (reason == LKMDBG_STOP_REASON_BREAKPOINT)
		atomic64_inc(&lkmdbg_state.breakpoint_callback_total);
	else
		atomic64_inc(&lkmdbg_state.watchpoint_callback_total);
	atomic64_inc(&entry->hits);
	cycle_hits = ++entry->cycle_hits;
	WRITE_ONCE(lkmdbg_state.hwpoint_last_tgid, entry->tgid);
	WRITE_ONCE(lkmdbg_state.hwpoint_last_tid, current->pid);
	WRITE_ONCE(lkmdbg_state.hwpoint_last_reason, reason);
	WRITE_ONCE(lkmdbg_state.hwpoint_last_type, actual_type);
	WRITE_ONCE(lkmdbg_state.hwpoint_last_addr, entry->addr);
	WRITE_ONCE(lkmdbg_state.hwpoint_last_ip, ip);

	if (!lkmdbg_hwpoint_threshold_reached(entry, cycle_hits)) {
		if (lkmdbg_pte_allows_access(__pte(entry->mmu_baseline_pte),
					     entry->mmu_baseline_vm_flags,
					     actual_type))
			lkmdbg_mmu_queue_step_rearm(entry);

		lkmdbg_hwpoint_put(entry);
		return true;
	}

	if (lkmdbg_hwpoint_is_oneshot(entry))
		entry->oneshot_complete = true;

	if (entry->action_flags & LKMDBG_HWPOINT_ACTION_AUTO_CONTINUE) {
		if (!entry->oneshot_complete &&
		    lkmdbg_pte_allows_access(__pte(entry->mmu_baseline_pte),
					     entry->mmu_baseline_vm_flags,
					     actual_type))
			lkmdbg_mmu_queue_step_rearm(entry);

		lkmdbg_hwpoint_put(entry);
		return true;
	}

	if (!atomic_xchg(&entry->stop_latched, 1)) {
		lkmdbg_regs_arm64_export_stop(&stop_regs, regs);
		lkmdbg_session_request_async_stop(
			entry->session, reason, entry->tgid, current->pid,
			actual_type | entry->flags,
			LKMDBG_STOP_FLAG_REARM_REQUIRED, entry->addr, ip,
			&stop_regs);
	}

	lkmdbg_hwpoint_put(entry);
	return true;
}

static int lkmdbg_do_page_fault_replacement(unsigned long far,
					    unsigned long esr,
					    struct pt_regs *regs)
{
	u32 actual_type;
	int ret;

	atomic_inc(&lkmdbg_do_page_fault_inflight);

	if (!user_mode(regs) || !current->mm ||
	    !lkmdbg_mmu_fault_from_user(esr))
		goto passthrough;

	actual_type = lkmdbg_mmu_fault_access_type(esr);
	if (!actual_type)
		goto passthrough;

	if (lkmdbg_mmu_try_handle_fault(current->mm, (unsigned long)far,
					actual_type, regs)) {
		if (atomic_dec_and_test(&lkmdbg_do_page_fault_inflight))
			wake_up_all(&lkmdbg_do_page_fault_waitq);
		return 0;
	}

passthrough:
	ret = lkmdbg_do_page_fault_orig ?
		      lkmdbg_do_page_fault_orig(far, esr, regs) :
		      0;
	if (atomic_dec_and_test(&lkmdbg_do_page_fault_inflight))
		wake_up_all(&lkmdbg_do_page_fault_waitq);
	return ret;
}

static vm_fault_t lkmdbg_do_page_fault_inner_replacement(
	struct mm_struct *mm, struct vm_area_struct *vma, unsigned long addr,
	unsigned int mm_flags, unsigned long vm_flags, struct pt_regs *regs)
{
	vm_fault_t ret;
	u32 actual_type;

	(void)vma;
	(void)vm_flags;

	atomic_inc(&lkmdbg_do_page_fault_inflight);

	if (!(mm_flags & FAULT_FLAG_USER) || !user_mode(regs) || !mm)
		goto passthrough;

	if (mm_flags & FAULT_FLAG_INSTRUCTION)
		actual_type = LKMDBG_HWPOINT_TYPE_EXEC;
	else if (mm_flags & FAULT_FLAG_WRITE)
		actual_type = LKMDBG_HWPOINT_TYPE_WRITE;
	else
		actual_type = LKMDBG_HWPOINT_TYPE_READ;

	if (lkmdbg_mmu_try_handle_fault(mm, addr, actual_type, regs)) {
		if (atomic_dec_and_test(&lkmdbg_do_page_fault_inflight))
			wake_up_all(&lkmdbg_do_page_fault_waitq);
		return 0;
	}

passthrough:
	ret = lkmdbg_do_page_fault_inner_orig ?
		      lkmdbg_do_page_fault_inner_orig(mm, vma, addr, mm_flags,
						      vm_flags, regs) :
		      0;
	if (atomic_dec_and_test(&lkmdbg_do_page_fault_inflight))
		wake_up_all(&lkmdbg_do_page_fault_waitq);
	return ret;
}

#ifdef CONFIG_ARM64
static bool lkmdbg_handle_user_single_step(struct pt_regs *regs)
{
	struct lkmdbg_session *matched;
	struct lkmdbg_hwpoint *entry = NULL;
	pid_t tid = current->pid;
	pid_t tgid = current->tgid;
	lkmdbg_user_single_step_fn disable_fn;
	unsigned long irqflags;

	spin_lock_irqsave(&lkmdbg_mmu_hwpoint_lock, irqflags);
	entry = lkmdbg_mmu_find_step_rearm_locked(tgid, tid);
	if (entry) {
		entry->rearm_step_armed = false;
		entry->rearm_step_tgid = 0;
		entry->rearm_step_tid = 0;
		lkmdbg_hwpoint_get(entry);
	}
	spin_unlock_irqrestore(&lkmdbg_mmu_hwpoint_lock, irqflags);

	if (entry) {
		disable_fn = (lkmdbg_user_single_step_fn)
			lkmdbg_symbols.user_disable_single_step_sym;
		if (disable_fn)
			disable_fn(current);
		if (!lkmdbg_mmu_apply_trap(current->mm, entry)) {
			atomic_set(&entry->stop_latched, 0);
			entry->armed = true;
		}
		lkmdbg_hwpoint_put(entry);
		return true;
	}

	matched = lkmdbg_session_consume_single_step(tgid, tid);
	if (!matched)
		return false;

	disable_fn =
		(lkmdbg_user_single_step_fn)lkmdbg_symbols.user_disable_single_step_sym;
	if (disable_fn)
		disable_fn(current);

	{
		struct lkmdbg_regs_arm64 stop_regs;

		lkmdbg_regs_arm64_export_stop(&stop_regs, regs);
		lkmdbg_session_request_async_stop(
			matched, LKMDBG_STOP_REASON_SINGLE_STEP, tgid, tid, 0, 0,
			instruction_pointer(regs), 0, &stop_regs);
	}
	lkmdbg_session_async_put(matched);
	return true;
}
#endif

#if defined(CONFIG_ARM64) && LKMDBG_ARM64_USER_STEP_HOOKS
static int lkmdbg_user_step_handler(struct pt_regs *regs,
				    lkmdbg_step_hook_esr_t esr)
{
	(void)esr;

	if (lkmdbg_handle_user_single_step(regs))
		return DBG_HOOK_HANDLED;

	return DBG_HOOK_ERROR;
}
#endif

#ifdef CONFIG_ARM64
static void lkmdbg_do_el0_softstep_replacement(unsigned long esr,
					       struct pt_regs *regs)
{
	atomic_inc(&lkmdbg_do_el0_softstep_inflight);

	if (!regs || !user_mode(regs) || !current->mm)
		goto passthrough;

	if (lkmdbg_handle_user_single_step(regs))
		goto out;

passthrough:
	if (lkmdbg_do_el0_softstep_orig)
		lkmdbg_do_el0_softstep_orig(esr, regs);
out:
	if (atomic_dec_and_test(&lkmdbg_do_el0_softstep_inflight))
		wake_up_all(&lkmdbg_do_el0_softstep_waitq);
}
#endif
#endif

static int lkmdbg_register_trace_hooks(void)
{
	lkmdbg_tracepoint_probe_register_fn register_fn;
	int ret;

	register_fn = (lkmdbg_tracepoint_probe_register_fn)
		lkmdbg_symbols.tracepoint_probe_register_sym;
	if (!register_fn) {
		lkmdbg_pr_info("lkmdbg: tracepoint register helper unavailable\n");
		return 0;
	}

	lkmdbg_trace_fork_tp = lkmdbg_find_tracepoint("sched_process_fork");
	if (lkmdbg_trace_fork_tp) {
		ret = register_fn(lkmdbg_trace_fork_tp,
				  (void *)lkmdbg_trace_sched_process_fork, NULL);
		if (!ret)
			lkmdbg_trace_fork_registered = true;
		else
			lkmdbg_pr_warn("lkmdbg: sched_process_fork trace hook failed ret=%d\n",
				ret);
	} else {
		lkmdbg_pr_info("lkmdbg: sched_process_fork tracepoint unavailable\n");
	}

	lkmdbg_trace_exec_tp = lkmdbg_find_tracepoint("sched_process_exec");
	if (lkmdbg_trace_exec_tp) {
		ret = register_fn(lkmdbg_trace_exec_tp,
				  (void *)lkmdbg_trace_sched_process_exec, NULL);
		if (!ret)
			lkmdbg_trace_exec_registered = true;
		else
			lkmdbg_pr_warn("lkmdbg: sched_process_exec trace hook failed ret=%d\n",
				ret);
	} else {
		lkmdbg_pr_info("lkmdbg: sched_process_exec tracepoint unavailable\n");
	}

	lkmdbg_trace_exit_tp = lkmdbg_find_tracepoint("sched_process_exit");
	if (lkmdbg_trace_exit_tp) {
		ret = register_fn(lkmdbg_trace_exit_tp,
				  (void *)lkmdbg_trace_sched_process_exit, NULL);
		if (!ret)
			lkmdbg_trace_exit_registered = true;
		else
			lkmdbg_pr_warn("lkmdbg: sched_process_exit trace hook failed ret=%d\n",
				ret);
	} else {
		lkmdbg_pr_info("lkmdbg: sched_process_exit tracepoint unavailable\n");
	}

	lkmdbg_trace_signal_tp = lkmdbg_find_tracepoint("signal_generate");
	if (lkmdbg_trace_signal_tp) {
		ret = register_fn(lkmdbg_trace_signal_tp,
				  (void *)lkmdbg_trace_signal_generate, NULL);
		if (!ret)
			lkmdbg_trace_signal_registered = true;
		else
			lkmdbg_pr_warn("lkmdbg: signal_generate trace hook failed ret=%d\n",
				ret);
	} else {
		lkmdbg_pr_info("lkmdbg: signal_generate tracepoint unavailable\n");
	}

	lkmdbg_trace_sys_enter_tp = lkmdbg_find_tracepoint("sys_enter");
	if (lkmdbg_trace_sys_enter_tp) {
		ret = register_fn(lkmdbg_trace_sys_enter_tp,
				  (void *)lkmdbg_trace_raw_sys_enter, NULL);
		if (!ret)
			lkmdbg_trace_sys_enter_registered = true;
		else
			lkmdbg_pr_warn("lkmdbg: raw sys_enter trace hook failed ret=%d\n",
				ret);
	} else {
		lkmdbg_pr_info("lkmdbg: raw sys_enter tracepoint unavailable\n");
	}

	lkmdbg_trace_sys_exit_tp = lkmdbg_find_tracepoint("sys_exit");
	if (lkmdbg_trace_sys_exit_tp) {
		ret = register_fn(lkmdbg_trace_sys_exit_tp,
				  (void *)lkmdbg_trace_raw_sys_exit, NULL);
		if (!ret)
			lkmdbg_trace_sys_exit_registered = true;
		else
			lkmdbg_pr_warn("lkmdbg: raw sys_exit trace hook failed ret=%d\n",
				ret);
	} else {
		lkmdbg_pr_info("lkmdbg: raw sys_exit tracepoint unavailable\n");
	}

	return 0;
}

#ifdef CONFIG_ARM64
static int lkmdbg_install_syscall_enter_backend(void)
{
	void *target;
	void *orig_fn = NULL;
	void *replacement;
	const char *name;
	int ret;

	if (lkmdbg_syscall_enter_hook)
		return 0;

	if (
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
		lkmdbg_symbols.invoke_syscall_inner_sym
#else
		false
#endif
	) {
		target = (void *)lkmdbg_symbols.invoke_syscall_inner_sym;
		replacement = lkmdbg_invoke_syscall_inner_replacement;
		name = "__invoke_syscall";
		lkmdbg_syscall_enter_hook_kind =
			LKMDBG_SYSCALL_ENTER_HOOK_INVOKE_INNER;
	} else if (lkmdbg_symbols.invoke_syscall_sym) {
		target = (void *)lkmdbg_symbols.invoke_syscall_sym;
		replacement = lkmdbg_invoke_syscall_replacement;
		name = "invoke_syscall";
		lkmdbg_syscall_enter_hook_kind =
			LKMDBG_SYSCALL_ENTER_HOOK_INVOKE;
	} else if (lkmdbg_symbols.invoke_syscall_inner_sym) {
		target = (void *)lkmdbg_symbols.invoke_syscall_inner_sym;
		replacement = lkmdbg_invoke_syscall_inner_replacement;
		name = "__invoke_syscall";
		lkmdbg_syscall_enter_hook_kind =
			LKMDBG_SYSCALL_ENTER_HOOK_INVOKE_INNER;
	} else if (lkmdbg_symbols.do_el0_svc_sym) {
		target = (void *)lkmdbg_symbols.do_el0_svc_sym;
		replacement = lkmdbg_do_el0_svc_replacement;
		name = "do_el0_svc";
		lkmdbg_syscall_enter_hook_kind =
			LKMDBG_SYSCALL_ENTER_HOOK_DO_EL0_SVC;
	} else {
		return -ENOENT;
	}

	lkmdbg_syscall_enter_registry =
		lkmdbg_hook_registry_register(name, target, replacement);
	if (!lkmdbg_syscall_enter_registry)
		return -ENOMEM;

	ret = lkmdbg_hook_install(target, replacement, &lkmdbg_syscall_enter_hook,
				  &orig_fn);
	if (ret) {
		lkmdbg_hook_registry_unregister(lkmdbg_syscall_enter_registry,
						ret);
		lkmdbg_syscall_enter_registry = NULL;
		lkmdbg_syscall_enter_hook_kind = LKMDBG_SYSCALL_ENTER_HOOK_NONE;
		return ret;
	}

	switch (lkmdbg_syscall_enter_hook_kind) {
	case LKMDBG_SYSCALL_ENTER_HOOK_INVOKE:
		lkmdbg_invoke_syscall_orig = (lkmdbg_invoke_syscall_fn)orig_fn;
		break;
	case LKMDBG_SYSCALL_ENTER_HOOK_INVOKE_INNER:
		lkmdbg_invoke_syscall_inner_orig =
			(lkmdbg_invoke_syscall_inner_fn)orig_fn;
		break;
	case LKMDBG_SYSCALL_ENTER_HOOK_DO_EL0_SVC:
		lkmdbg_do_el0_svc_orig = (lkmdbg_do_el0_svc_fn)orig_fn;
		break;
	default:
		break;
	}

	lkmdbg_hook_registry_mark_installed(lkmdbg_syscall_enter_registry, target,
					    orig_fn, 0);
	lkmdbg_pr_info("lkmdbg: syscall fallback active target=%s origin=%px trampoline=%px\n",
		name, orig_fn, target);
	return 0;
}

static void lkmdbg_uninstall_syscall_enter_backend_locked(void)
{
	if (lkmdbg_syscall_enter_hook) {
		if (!lkmdbg_hook_deactivate(lkmdbg_syscall_enter_hook))
			wait_event_timeout(
				lkmdbg_syscall_enter_waitq,
				atomic_read(&lkmdbg_syscall_enter_inflight) == 0,
				msecs_to_jiffies(1000));
		lkmdbg_hook_destroy(lkmdbg_syscall_enter_hook);
		lkmdbg_syscall_enter_hook = NULL;
	}
	if (lkmdbg_syscall_enter_registry) {
		lkmdbg_hook_registry_unregister(lkmdbg_syscall_enter_registry,
						0);
		lkmdbg_syscall_enter_registry = NULL;
	}
	lkmdbg_syscall_enter_hook_kind = LKMDBG_SYSCALL_ENTER_HOOK_NONE;
	lkmdbg_invoke_syscall_orig = NULL;
	lkmdbg_invoke_syscall_inner_orig = NULL;
	lkmdbg_do_el0_svc_orig = NULL;
	if (lkmdbg_syscall_enter_module_ref) {
		module_put(THIS_MODULE);
		lkmdbg_syscall_enter_module_ref = false;
	}
}

static bool lkmdbg_syscall_enter_fallback_available(void)
{
	return !!(lkmdbg_symbols.invoke_syscall_sym ||
		  lkmdbg_symbols.invoke_syscall_inner_sym ||
		  lkmdbg_symbols.do_el0_svc_sym);
}

static bool lkmdbg_syscall_trace_needs_hook_fallback(u32 mode, u32 phases)
{
	u32 missing_phases;

	if (!(mode & (LKMDBG_SYSCALL_TRACE_MODE_EVENT |
		      LKMDBG_SYSCALL_TRACE_MODE_STOP |
		      LKMDBG_SYSCALL_TRACE_MODE_CONTROL)))
		return false;
	if (mode & LKMDBG_SYSCALL_TRACE_MODE_CONTROL)
		return true;
	missing_phases = phases & ~lkmdbg_syscall_trace_tracepoint_phases();
	if (!missing_phases)
		return false;

	return !!(missing_phases & lkmdbg_syscall_trace_fallback_phases());
}

static int lkmdbg_ensure_syscall_enter_backend(void)
{
	int ret = 0;

	mutex_lock(&lkmdbg_syscall_enter_backend_lock);
	if (lkmdbg_syscall_enter_hook)
		goto out;
	if (!lkmdbg_syscall_enter_fallback_available()) {
		ret = -EOPNOTSUPP;
		goto out;
	}
	if (!lkmdbg_syscall_enter_module_ref) {
		if (!try_module_get(THIS_MODULE)) {
			ret = -ENODEV;
			goto out;
		}
		lkmdbg_syscall_enter_module_ref = true;
	}

	ret = lkmdbg_install_syscall_enter_backend();
	if (ret && lkmdbg_syscall_enter_module_ref) {
		module_put(THIS_MODULE);
		lkmdbg_syscall_enter_module_ref = false;
	}
out:
	mutex_unlock(&lkmdbg_syscall_enter_backend_lock);
	return ret;
}

static void lkmdbg_syscall_enter_teardown_workfn(struct work_struct *work)
{
	(void)work;

	mutex_lock(&lkmdbg_syscall_enter_backend_lock);
	lkmdbg_syscall_enter_teardown_queued = false;
	if (atomic_read(&lkmdbg_syscall_enter_users) == 0)
		lkmdbg_uninstall_syscall_enter_backend_locked();
	mutex_unlock(&lkmdbg_syscall_enter_backend_lock);
}

static void lkmdbg_queue_syscall_enter_teardown(void)
{
	mutex_lock(&lkmdbg_syscall_enter_backend_lock);
	if (atomic_read(&lkmdbg_syscall_enter_users) == 0 &&
	    lkmdbg_syscall_enter_hook &&
	    !lkmdbg_syscall_enter_teardown_queued) {
		lkmdbg_syscall_enter_teardown_queued = true;
		schedule_work(&lkmdbg_syscall_enter_teardown_work);
	}
	mutex_unlock(&lkmdbg_syscall_enter_backend_lock);
}
#endif

int lkmdbg_thread_ctrl_init(void)
{
	int ret;

	hash_init(lkmdbg_mm_event_pending_ht);

#ifdef CONFIG_ARM64
	lkmdbg_install_user_single_step_backend();
#endif

	ret = lkmdbg_register_trace_hooks();
	return ret;
}

long lkmdbg_set_signal_config(struct lkmdbg_session *session, void __user *argp)
{
	struct lkmdbg_signal_config_request req;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	if (lkmdbg_validate_signal_config(&req))
		return -EINVAL;

	mutex_lock(&session->lock);
	session->signal_mask_words[0] = req.mask_words[0];
	session->signal_mask_words[1] = req.mask_words[1];
	session->signal_flags = req.flags;
	mutex_unlock(&session->lock);

	if (copy_to_user(argp, &req, sizeof(req)))
		return -EFAULT;

	return 0;
}

long lkmdbg_get_signal_config(struct lkmdbg_session *session, void __user *argp)
{
	struct lkmdbg_signal_config_request req;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	if (req.version != LKMDBG_PROTO_VERSION || req.size != sizeof(req))
		return -EINVAL;

	mutex_lock(&session->lock);
	req.mask_words[0] = session->signal_mask_words[0];
	req.mask_words[1] = session->signal_mask_words[1];
	req.flags = session->signal_flags;
	mutex_unlock(&session->lock);

	if (copy_to_user(argp, &req, sizeof(req)))
		return -EFAULT;

	return 0;
}

long lkmdbg_set_syscall_trace(struct lkmdbg_session *session, void __user *argp)
{
	struct lkmdbg_syscall_trace_request req;
	u32 supported_phases;
	bool new_need = false;
#ifdef CONFIG_ARM64
	bool old_need;
	int ret;
#endif

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	if (lkmdbg_validate_syscall_trace_request(&req))
		return -EINVAL;

	supported_phases = lkmdbg_syscall_trace_supported_phases();
	if (req.mode && (req.phases & ~supported_phases))
		return -EOPNOTSUPP;
#ifdef CONFIG_ARM64
	if ((req.mode & LKMDBG_SYSCALL_TRACE_MODE_CONTROL) &&
	    !lkmdbg_syscall_enter_fallback_available())
		return -EOPNOTSUPP;
#else
	if (req.mode & LKMDBG_SYSCALL_TRACE_MODE_CONTROL)
		return -EOPNOTSUPP;
#endif

#ifdef CONFIG_ARM64
	mutex_lock(&session->lock);
	old_need = session->syscall_trace_hook_fallback;
	mutex_unlock(&session->lock);

	new_need = lkmdbg_syscall_trace_needs_hook_fallback(req.mode,
							    req.phases);
	if (new_need) {
		if (!old_need)
			atomic_inc(&lkmdbg_syscall_enter_users);
		ret = lkmdbg_ensure_syscall_enter_backend();
		if (ret) {
			if (!old_need)
				atomic_dec(&lkmdbg_syscall_enter_users);
			return ret;
		}
	}
#endif

	mutex_lock(&session->lock);
	session->syscall_trace_tid = req.tid;
	session->syscall_trace_nr = req.syscall_nr;
	session->syscall_trace_mode = req.mode;
	session->syscall_trace_phases = req.phases;
	session->syscall_trace_hook_fallback = new_need;
	mutex_unlock(&session->lock);

#ifdef CONFIG_ARM64
	if (old_need && !new_need) {
		atomic_dec(&lkmdbg_syscall_enter_users);
		lkmdbg_queue_syscall_enter_teardown();
	}
#endif

	req.flags = lkmdbg_syscall_trace_capability_flags();
	req.supported_phases = supported_phases;

	if (copy_to_user(argp, &req, sizeof(req)))
		return -EFAULT;

	return 0;
}

long lkmdbg_get_syscall_trace(struct lkmdbg_session *session, void __user *argp)
{
	struct lkmdbg_syscall_trace_request req;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	if (req.version != LKMDBG_PROTO_VERSION || req.size != sizeof(req))
		return -EINVAL;

	mutex_lock(&session->lock);
	req.tid = session->syscall_trace_tid;
	req.syscall_nr = session->syscall_trace_nr;
	req.mode = session->syscall_trace_mode;
	req.phases = session->syscall_trace_phases;
	mutex_unlock(&session->lock);
	req.flags = lkmdbg_syscall_trace_capability_flags();
	req.supported_phases = lkmdbg_syscall_trace_supported_phases();

	if (copy_to_user(argp, &req, sizeof(req)))
		return -EFAULT;

	return 0;
}

void lkmdbg_thread_ctrl_exit(void)
{
	lkmdbg_tracepoint_probe_unregister_fn unregister_fn;

	unregister_fn = (lkmdbg_tracepoint_probe_unregister_fn)
		lkmdbg_symbols.tracepoint_probe_unregister_sym;

	if (unregister_fn && lkmdbg_trace_sys_exit_registered &&
	    lkmdbg_trace_sys_exit_tp) {
		unregister_fn(lkmdbg_trace_sys_exit_tp,
			      (void *)lkmdbg_trace_raw_sys_exit, NULL);
		lkmdbg_trace_sys_exit_registered = false;
	}
	lkmdbg_trace_sys_exit_tp = NULL;

	if (unregister_fn && lkmdbg_trace_sys_enter_registered &&
	    lkmdbg_trace_sys_enter_tp) {
		unregister_fn(lkmdbg_trace_sys_enter_tp,
			      (void *)lkmdbg_trace_raw_sys_enter, NULL);
		lkmdbg_trace_sys_enter_registered = false;
	}
	lkmdbg_trace_sys_enter_tp = NULL;

	if (unregister_fn && lkmdbg_trace_signal_registered &&
	    lkmdbg_trace_signal_tp) {
		unregister_fn(lkmdbg_trace_signal_tp,
			      (void *)lkmdbg_trace_signal_generate, NULL);
		lkmdbg_trace_signal_registered = false;
	}
	lkmdbg_trace_signal_tp = NULL;

	if (unregister_fn && lkmdbg_trace_exit_registered && lkmdbg_trace_exit_tp) {
		unregister_fn(lkmdbg_trace_exit_tp,
			      (void *)lkmdbg_trace_sched_process_exit, NULL);
		lkmdbg_trace_exit_registered = false;
	}
	lkmdbg_trace_exit_tp = NULL;

	if (unregister_fn && lkmdbg_trace_exec_registered && lkmdbg_trace_exec_tp) {
		unregister_fn(lkmdbg_trace_exec_tp,
			      (void *)lkmdbg_trace_sched_process_exec, NULL);
		lkmdbg_trace_exec_registered = false;
	}
	lkmdbg_trace_exec_tp = NULL;

	if (unregister_fn && lkmdbg_trace_fork_registered && lkmdbg_trace_fork_tp) {
		unregister_fn(lkmdbg_trace_fork_tp,
			      (void *)lkmdbg_trace_sched_process_fork, NULL);
		lkmdbg_trace_fork_registered = false;
	}
	lkmdbg_trace_fork_tp = NULL;

#if defined(CONFIG_ARM64) && LKMDBG_ARM64_USER_STEP_HOOKS
	if (lkmdbg_user_step_hook_registered) {
		((lkmdbg_unregister_user_step_hook_fn)
			 lkmdbg_symbols.unregister_user_step_hook_sym)(
			&lkmdbg_user_step_hook);
		lkmdbg_user_step_hook_registered = false;
	}
#endif

#ifdef CONFIG_ARM64
	flush_work(&lkmdbg_syscall_enter_teardown_work);
	mutex_lock(&lkmdbg_syscall_enter_backend_lock);
	lkmdbg_uninstall_syscall_enter_backend_locked();
	mutex_unlock(&lkmdbg_syscall_enter_backend_lock);

	if (lkmdbg_do_el0_softstep_hook) {
		if (!lkmdbg_hook_deactivate(lkmdbg_do_el0_softstep_hook))
			wait_event_timeout(
				lkmdbg_do_el0_softstep_waitq,
				atomic_read(&lkmdbg_do_el0_softstep_inflight) == 0,
				msecs_to_jiffies(1000));
		lkmdbg_hook_destroy(lkmdbg_do_el0_softstep_hook);
		lkmdbg_do_el0_softstep_hook = NULL;
	}
	if (lkmdbg_do_el0_softstep_registry) {
		lkmdbg_hook_registry_unregister(
			lkmdbg_do_el0_softstep_registry, 0);
		lkmdbg_do_el0_softstep_registry = NULL;
	}
	lkmdbg_do_el0_softstep_orig = NULL;

	if (lkmdbg_do_page_fault_hook) {
		if (!lkmdbg_hook_deactivate(lkmdbg_do_page_fault_hook))
			wait_event_timeout(
				lkmdbg_do_page_fault_waitq,
				atomic_read(&lkmdbg_do_page_fault_inflight) == 0,
				msecs_to_jiffies(1000));
		lkmdbg_hook_destroy(lkmdbg_do_page_fault_hook);
		lkmdbg_do_page_fault_hook = NULL;
	}
	if (lkmdbg_do_page_fault_registry) {
		lkmdbg_hook_registry_unregister(lkmdbg_do_page_fault_registry, 0);
		lkmdbg_do_page_fault_registry = NULL;
	}
	lkmdbg_do_page_fault_hook_kind = LKMDBG_PAGE_FAULT_HOOK_NONE;
	lkmdbg_do_page_fault_orig = NULL;
	lkmdbg_do_page_fault_inner_orig = NULL;

	if (lkmdbg_process_vm_write_hook) {
		if (!lkmdbg_hook_deactivate(lkmdbg_process_vm_write_hook))
			wait_event_timeout(
				lkmdbg_process_vm_write_waitq,
				atomic_read(&lkmdbg_process_vm_write_inflight) ==
					0,
				msecs_to_jiffies(1000));
		lkmdbg_hook_destroy(lkmdbg_process_vm_write_hook);
		lkmdbg_process_vm_write_hook = NULL;
	}
	if (lkmdbg_process_vm_write_registry) {
		lkmdbg_hook_registry_unregister(
			lkmdbg_process_vm_write_registry, 0);
		lkmdbg_process_vm_write_registry = NULL;
	}
	lkmdbg_process_vm_rw_orig = NULL;
	lkmdbg_do_sys_process_vm_writev_orig = NULL;
	lkmdbg_process_vm_write_hook_kind =
		LKMDBG_PROCESS_VM_WRITE_HOOK_NONE;

	if (lkmdbg_remote_vm_write_hook) {
		if (!lkmdbg_hook_deactivate(lkmdbg_remote_vm_write_hook))
			wait_event_timeout(
				lkmdbg_remote_vm_write_waitq,
				atomic_read(&lkmdbg_remote_vm_write_inflight) == 0,
				msecs_to_jiffies(1000));
		lkmdbg_hook_destroy(lkmdbg_remote_vm_write_hook);
		lkmdbg_remote_vm_write_hook = NULL;
	}
	if (lkmdbg_remote_vm_write_registry) {
		lkmdbg_hook_registry_unregister(lkmdbg_remote_vm_write_registry,
						0);
		lkmdbg_remote_vm_write_registry = NULL;
	}
	lkmdbg_access_remote_vm_orig = NULL;
	lkmdbg_access_remote_vm_inner_orig = NULL;
	lkmdbg_remote_vm_write_hook_kind =
		LKMDBG_REMOTE_VM_WRITE_HOOK_NONE;
#endif

	lkmdbg_mm_event_pending_clear_all();
}

void lkmdbg_thread_ctrl_release(struct lkmdbg_session *session)
{
	struct lkmdbg_hwpoint *entry;
	struct lkmdbg_hwpoint *tmp;
#ifdef CONFIG_ARM64
	lkmdbg_user_single_step_fn disable_fn;
	bool fallback_active;
	pid_t step_tid;
#endif

	if (!session)
		return;

	mutex_lock(&session->lock);
#ifdef CONFIG_ARM64
	step_tid = session->step_tid;
	fallback_active = session->syscall_trace_hook_fallback;
	WRITE_ONCE(session->step_armed, false);
	WRITE_ONCE(session->step_tgid, 0);
	WRITE_ONCE(session->step_tid, 0);
	session->syscall_trace_hook_fallback = false;
#endif
	list_for_each_entry_safe(entry, tmp, &session->hwpoints, node) {
		list_del_init(&entry->node);
		mutex_unlock(&session->lock);
		if (lkmdbg_hwpoint_is_mmu(entry))
			lkmdbg_unregister_mmu_hwpoint(entry);
		else
			unregister_hw_breakpoint(entry->event);
		lkmdbg_hwpoint_put(entry);
		mutex_lock(&session->lock);
	}
	mutex_unlock(&session->lock);

#ifdef CONFIG_ARM64
	if (fallback_active) {
		atomic_dec(&lkmdbg_syscall_enter_users);
		lkmdbg_queue_syscall_enter_teardown();
	}

	if (!step_tid || !lkmdbg_symbols.user_disable_single_step_sym)
		return;

	disable_fn =
		(lkmdbg_user_single_step_fn)lkmdbg_symbols.user_disable_single_step_sym;
	if (disable_fn) {
		struct task_struct *task;

		task = get_pid_task(find_vpid(step_tid), PIDTYPE_PID);
		if (task) {
			disable_fn(task);
			put_task_struct(task);
		}
	}
#endif
}

long lkmdbg_add_hwpoint(struct lkmdbg_session *session, void __user *argp)
{
	struct lkmdbg_hwpoint_request req;
	long ret;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	ret = lkmdbg_validate_hwpoint_request(&req, false);
	if (ret)
		return ret;

	ret = lkmdbg_register_hwpoint(session, &req);
	return lkmdbg_hwpoint_copy_reply(argp, &req, ret);
}

long lkmdbg_remove_hwpoint(struct lkmdbg_session *session, void __user *argp)
{
	struct lkmdbg_hwpoint_request req;
	long ret;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	ret = lkmdbg_validate_hwpoint_request(&req, true);
	if (ret)
		return ret;

	ret = lkmdbg_unregister_hwpoint(session, req.id);
	return lkmdbg_hwpoint_copy_reply(argp, &req, ret);
}

long lkmdbg_query_hwpoints(struct lkmdbg_session *session, void __user *argp)
{
	struct lkmdbg_hwpoint_query_request req;
	struct lkmdbg_hwpoint_entry *entries;
	struct lkmdbg_hwpoint *entry;
	u32 filled = 0;
	long ret = 0;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	ret = lkmdbg_validate_hwpoint_query(&req);
	if (ret)
		return ret;

	entries = kcalloc(req.max_entries, sizeof(*entries), GFP_KERNEL);
	if (!entries)
		return -ENOMEM;

	req.done = 1;
	req.next_id = 0;

	mutex_lock(&session->lock);
	list_for_each_entry(entry, &session->hwpoints, node) {
		if (entry->id <= req.start_id)
			continue;
		if (filled == req.max_entries) {
			req.done = 0;
			req.next_id = entries[filled - 1].id;
			break;
		}

		if (lkmdbg_hwpoint_is_mmu(entry)) {
#ifdef CONFIG_ARM64
			lkmdbg_mmu_refresh_entry_state(entry);
#endif
		}
		lkmdbg_hwpoint_fill_entry(&entries[filled], entry);
		filled++;
	}
	mutex_unlock(&session->lock);

	req.entries_filled = filled;
	ret = lkmdbg_hwpoint_query_copy_reply(
		argp, &req, entries, req.max_entries * sizeof(*entries));
	kfree(entries);
	return ret;
}

long lkmdbg_rearm_hwpoint(struct lkmdbg_session *session, void __user *argp)
{
	struct lkmdbg_hwpoint_request req;
	struct lkmdbg_hwpoint *entry;
	long ret;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	ret = lkmdbg_validate_hwpoint_request(&req, true);
	if (ret)
		return ret;

	mutex_lock(&session->lock);
	entry = lkmdbg_find_hwpoint_locked(session, req.id);
	if (!entry) {
		mutex_unlock(&session->lock);
		return lkmdbg_hwpoint_copy_reply(argp, &req, -ENOENT);
	}

	ret = lkmdbg_rearm_hwpoint_locked(entry);
	if (!ret) {
		req.tid = entry->tid;
		req.addr = entry->addr;
		req.type = entry->type;
		req.len = entry->len;
		req.flags = entry->flags;
		req.trigger_hit_count = entry->trigger_hit_count;
		req.action_flags = entry->action_flags;
	}
	mutex_unlock(&session->lock);

	return lkmdbg_hwpoint_copy_reply(argp, &req, ret);
}

int lkmdbg_rearm_all_hwpoints(struct lkmdbg_session *session)
{
	struct lkmdbg_hwpoint *entry;
	int ret = 0;

	mutex_lock(&session->lock);
	list_for_each_entry(entry, &session->hwpoints, node) {
		if (entry->flags & LKMDBG_HWPOINT_FLAG_COUNTER_MODE)
			continue;
		if (entry->oneshot_complete) {
			atomic_set(&entry->stop_latched, 0);
			continue;
		}
		if (!atomic_read(&entry->stop_latched) && entry->armed)
			continue;

		ret = lkmdbg_rearm_hwpoint_locked(entry);
		if (ret)
			break;
	}
	mutex_unlock(&session->lock);

	return ret;
}

int lkmdbg_prepare_continue_hwpoints(struct lkmdbg_session *session,
				     const struct lkmdbg_stop_state *stop,
				     u32 flags)
{
	struct lkmdbg_hwpoint *entry;

	mutex_lock(&session->lock);
	list_for_each_entry(entry, &session->hwpoints, node) {
		if (entry->oneshot_complete)
			atomic_set(&entry->stop_latched, 0);
	}
	mutex_unlock(&session->lock);

	if (!(flags & LKMDBG_CONTINUE_FLAG_REARM_HWPOINTS))
		return 0;

	if (stop && (stop->flags & LKMDBG_STOP_FLAG_REARM_REQUIRED) &&
	    (stop->event_flags & LKMDBG_HWPOINT_FLAG_MMU))
		return 0;

	return lkmdbg_rearm_all_hwpoints(session);
}

long lkmdbg_single_step(struct lkmdbg_session *session, void __user *argp)
{
	struct lkmdbg_single_step_request req;
	long ret;
#ifdef CONFIG_ARM64
	struct task_struct *task = NULL;
	pid_t tid;
	lkmdbg_user_single_step_fn enable_fn;
	u32 freeze_flags;
#endif

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	ret = lkmdbg_validate_single_step_request(&req);
	if (ret)
		return ret;

#ifndef CONFIG_ARM64
	return lkmdbg_single_step_copy_reply(argp, &req, -EOPNOTSUPP);
#else
	if (!lkmdbg_user_single_step_backend_ready() ||
	    !lkmdbg_symbols.user_enable_single_step_sym)
		return lkmdbg_single_step_copy_reply(argp, &req, -EOPNOTSUPP);

	ret = lkmdbg_get_target_thread(session, req.tid, &task);
	if (ret)
		return lkmdbg_single_step_copy_reply(argp, &req, ret);

	tid = task->pid;
	freeze_flags = lkmdbg_freeze_thread_flags(session, tid);
	if (!(freeze_flags & LKMDBG_THREAD_FLAG_FREEZE_PARKED)) {
		put_task_struct(task);
		return lkmdbg_single_step_copy_reply(argp, &req, -EBUSY);
	}

	mutex_lock(&session->lock);
	if (session->step_armed || lkmdbg_session_has_mmu_step_locked(session)) {
		mutex_unlock(&session->lock);
		put_task_struct(task);
		return lkmdbg_single_step_copy_reply(argp, &req, -EBUSY);
	}

	WRITE_ONCE(session->step_armed, true);
	WRITE_ONCE(session->step_tgid, task->tgid);
	WRITE_ONCE(session->step_tid, tid);
	mutex_unlock(&session->lock);

	enable_fn =
		(lkmdbg_user_single_step_fn)lkmdbg_symbols.user_enable_single_step_sym;
	enable_fn(task);
	put_task_struct(task);

	req.tid = tid;
	return lkmdbg_single_step_copy_reply(argp, &req, 0);
#endif
}
