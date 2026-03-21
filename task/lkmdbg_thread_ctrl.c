#include <linux/errno.h>
#include <linux/hw_breakpoint.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/mmap_lock.h>
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

#ifdef CONFIG_ARM64
#include <asm/pgtable.h>
#include <asm/tlbflush.h>
#include <asm/debug-monitors.h>
#endif

#include "lkmdbg_internal.h"

#define LKMDBG_HWPOINT_MAX_ENTRIES 64U
#define LKMDBG_MMU_BREAKPOINT_PAGE_SIZE PAGE_SIZE

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
	pid_t rearm_step_tgid;
	pid_t rearm_step_tid;
	atomic64_t hits;
	refcount_t refs;
	bool armed;
	bool rearm_step_armed;
	atomic_t stop_latched;
};

#ifdef CONFIG_ARM64
typedef void (*lkmdbg_register_user_step_hook_fn)(struct step_hook *hook);
typedef void (*lkmdbg_unregister_user_step_hook_fn)(struct step_hook *hook);
typedef void (*lkmdbg_user_single_step_fn)(struct task_struct *task);

static void lkmdbg_regs_arm64_export_stop(struct lkmdbg_regs_arm64 *dst,
					  const struct pt_regs *src)
{
	unsigned int i;

	memset(dst, 0, sizeof(*dst));
	for (i = 0; i < ARRAY_SIZE(dst->regs); i++)
		dst->regs[i] = src->regs[i];
	dst->sp = src->sp;
	dst->pc = src->pc;
	dst->pstate = src->pstate;
}
#endif
typedef void (*lkmdbg_for_each_kernel_tracepoint_fn)(
	void (*fct)(struct tracepoint *tp, void *priv), void *priv);
typedef int (*lkmdbg_tracepoint_probe_register_fn)(struct tracepoint *tp,
						   void *probe, void *data);
typedef int (*lkmdbg_tracepoint_probe_unregister_fn)(struct tracepoint *tp,
						     void *probe, void *data);
typedef void (*lkmdbg_perf_event_disable_local_fn)(struct perf_event *event);

static bool lkmdbg_trace_fork_registered;
static bool lkmdbg_trace_exec_registered;
static bool lkmdbg_trace_exit_registered;
static bool lkmdbg_trace_signal_registered;
static struct tracepoint *lkmdbg_trace_fork_tp;
static struct tracepoint *lkmdbg_trace_exec_tp;
static struct tracepoint *lkmdbg_trace_exit_tp;
static struct tracepoint *lkmdbg_trace_signal_tp;
static bool lkmdbg_perf_disable_missing_logged;
#ifdef CONFIG_ARM64
static LIST_HEAD(lkmdbg_mmu_hwpoint_list);
static DEFINE_SPINLOCK(lkmdbg_mmu_hwpoint_lock);
static struct lkmdbg_inline_hook *lkmdbg_do_page_fault_hook;
static struct lkmdbg_hook_registry_entry *lkmdbg_do_page_fault_registry;
static int (*lkmdbg_do_page_fault_orig)(unsigned long far, unsigned long esr,
					struct pt_regs *regs);
static atomic_t lkmdbg_do_page_fault_inflight = ATOMIC_INIT(0);
static DECLARE_WAIT_QUEUE_HEAD(lkmdbg_do_page_fault_waitq);
#endif

#ifdef CONFIG_ARM64
static int lkmdbg_user_step_handler(struct pt_regs *regs, unsigned long esr);
static int lkmdbg_do_page_fault_replacement(unsigned long far,
					    unsigned long esr,
					    struct pt_regs *regs);

static struct step_hook lkmdbg_user_step_hook = {
	.fn = lkmdbg_user_step_handler,
};
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

static bool lkmdbg_hwpoint_type_valid(u32 type)
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

static bool lkmdbg_hwpoint_len_valid(u32 len)
{
	return len >= 1 && len <= 8;
}

static bool lkmdbg_hwpoint_is_mmu_exec(const struct lkmdbg_hwpoint *entry)
{
	return !!(entry->flags & LKMDBG_HWPOINT_FLAG_MMU_EXEC);
}

static int lkmdbg_validate_hwpoint_request(struct lkmdbg_hwpoint_request *req,
					   bool remove)
{
	if (req->version != LKMDBG_PROTO_VERSION || req->size != sizeof(*req))
		return -EINVAL;

	if (req->flags &
	    ~(LKMDBG_HWPOINT_FLAG_COUNTER_MODE | LKMDBG_HWPOINT_FLAG_MMU_EXEC))
		return -EINVAL;

	if (remove)
		return req->id ? 0 : -EINVAL;

	if (!req->addr || req->addr >= (u64)TASK_SIZE_MAX)
		return -EINVAL;

	if (!lkmdbg_hwpoint_type_valid(req->type) ||
	    !lkmdbg_hwpoint_len_valid(req->len))
		return -EINVAL;

	if ((req->flags & LKMDBG_HWPOINT_FLAG_MMU_EXEC) &&
	    (req->type != LKMDBG_HWPOINT_TYPE_EXEC ||
	     (req->flags & LKMDBG_HWPOINT_FLAG_COUNTER_MODE)))
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
static bool lkmdbg_is_el0_instruction_permission_fault(unsigned long esr)
{
	if (ESR_ELx_EC(esr) != ESR_ELx_EC_IABT_LOW)
		return false;

	return (esr & ESR_ELx_FSC_TYPE) == ESR_ELx_FSC_PERM;
}

static pte_t lkmdbg_mmu_pte_set_exec(pte_t pte, bool executable)
{
	if (executable)
		pte_val(pte) &= ~PTE_UXN;
	else
		pte_val(pte) |= PTE_UXN;
	return pte;
}

static int lkmdbg_mmu_lookup_pte(struct mm_struct *mm, unsigned long addr,
				 pte_t **ptep_out, spinlock_t **ptl_out)
{
	pgd_t *pgdp;
	p4d_t *p4dp;
	pud_t *pudp;
	pmd_t *pmdp;
	pte_t *ptep;
	spinlock_t *ptl;

	pgdp = pgd_offset(mm, addr);
	if (pgd_none(*pgdp) || pgd_bad(*pgdp))
		return -ENOENT;

	p4dp = p4d_offset(pgdp, addr);
	if (p4d_none(*p4dp) || p4d_bad(*p4dp))
		return -ENOENT;
#ifdef p4d_leaf
	if (p4d_leaf(*p4dp))
		return -EOPNOTSUPP;
#endif

	pudp = pud_offset(p4dp, addr);
	if (pud_none(*pudp) || pud_bad(*pudp))
		return -ENOENT;
	if (pud_leaf(*pudp))
		return -EOPNOTSUPP;

	pmdp = pmd_offset(pudp, addr);
	if (pmd_none(*pmdp) || pmd_bad(*pmdp))
		return -ENOENT;
	if (pmd_leaf(*pmdp) || pmd_trans_huge(*pmdp))
		return -EOPNOTSUPP;

	ptep = pte_offset_map_lock(mm, pmdp, addr, &ptl);
	if (!ptep)
		return -ENOENT;

	*ptep_out = ptep;
	*ptl_out = ptl;
	return 0;
}

static int lkmdbg_mmu_update_exec_locked(struct mm_struct *mm,
					 unsigned long addr, bool executable)
{
	struct vm_area_struct *vma;
	pte_t *ptep;
	spinlock_t *ptl;
	pte_t pte;
	pte_t new_pte;
	int ret;

	vma = find_vma(mm, addr);
	if (!vma || addr < vma->vm_start || addr >= vma->vm_end)
		return -ENOENT;

	ret = lkmdbg_mmu_lookup_pte(mm, addr, &ptep, &ptl);
	if (ret)
		return ret;

	pte = READ_ONCE(*ptep);
	if (!pte_present(pte) || !pte_valid(pte)) {
		pte_unmap_unlock(ptep, ptl);
		return -ENOENT;
	}

	if (executable) {
		new_pte = lkmdbg_mmu_pte_set_exec(pte, true);
	} else {
		if (!(vma->vm_flags & VM_EXEC) || !pte_user_exec(pte)) {
			pte_unmap_unlock(ptep, ptl);
			return -EACCES;
		}
		new_pte = lkmdbg_mmu_pte_set_exec(pte, false);
	}

	if (pte_val(new_pte) != pte_val(pte)) {
		set_pte_at(mm, addr, ptep, new_pte);
		flush_tlb_page(vma, addr);
	}

	pte_unmap_unlock(ptep, ptl);
	return 0;
}

static int lkmdbg_mmu_update_exec(struct mm_struct *mm, unsigned long addr,
				  bool executable)
{
	int ret;

	if (!mm)
		return -ESRCH;

	mmap_write_lock(mm);
	ret = lkmdbg_mmu_update_exec_locked(mm, addr, executable);
	mmap_write_unlock(mm);
	return ret;
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

static int lkmdbg_mmu_install_page_fault_hook(void)
{
	void *target;
	void *orig_fn = NULL;
	int ret;

	if (lkmdbg_do_page_fault_hook)
		return 0;
	if (!lkmdbg_symbols.do_page_fault_sym)
		return -ENOENT;

	target = (void *)lkmdbg_symbols.do_page_fault_sym;

	lkmdbg_do_page_fault_registry =
		lkmdbg_hook_registry_register("do_page_fault", target,
					      lkmdbg_do_page_fault_replacement);
	if (!lkmdbg_do_page_fault_registry)
		return -ENOMEM;

	ret = lkmdbg_hook_install(target, lkmdbg_do_page_fault_replacement,
				  &lkmdbg_do_page_fault_hook, &orig_fn);
	if (ret) {
		lkmdbg_hook_registry_unregister(lkmdbg_do_page_fault_registry, ret);
		lkmdbg_do_page_fault_registry = NULL;
		return ret;
	}

	lkmdbg_do_page_fault_orig = orig_fn;
	lkmdbg_hook_registry_mark_installed(lkmdbg_do_page_fault_registry, target,
						orig_fn, 0);
	pr_info("lkmdbg: runtime hook active target=do_page_fault origin=%px trampoline=%px\n",
		target, orig_fn);
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

static struct lkmdbg_hwpoint *
lkmdbg_find_breakpoint_by_addr_locked(struct lkmdbg_session *session, u64 addr,
				      u32 flags)
{
	(void)session;
	(void)addr;
	(void)flags;
	return NULL;
}

static bool lkmdbg_session_has_mmu_step_locked(struct lkmdbg_session *session)
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
	dst->tgid = src->tgid;
	dst->tid = src->tid;
	dst->type = src->type;
	dst->len = src->len;
	dst->flags = src->flags;
	dst->state = src->armed ? LKMDBG_HWPOINT_STATE_ACTIVE : 0;
	if (atomic_read(&src->stop_latched))
		dst->state |= LKMDBG_HWPOINT_STATE_LATCHED;
}

static void lkmdbg_hwpoint_disable_stop_mode(struct lkmdbg_hwpoint *entry)
{
	lkmdbg_perf_event_disable_local_fn disable_local_fn;

	disable_local_fn = (lkmdbg_perf_event_disable_local_fn)
		lkmdbg_symbols.perf_event_disable_local_sym;
	if (!disable_local_fn) {
		if (!READ_ONCE(lkmdbg_perf_disable_missing_logged)) {
			WRITE_ONCE(lkmdbg_perf_disable_missing_logged, true);
			pr_warn("lkmdbg: perf_event_disable_local unavailable, stop-mode hwpoints remain armed\n");
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
	atomic64_set(&entry->hits, 0);
	refcount_set(&entry->refs, 1);
	atomic_set(&entry->stop_latched, 0);
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

	ret = lkmdbg_mmu_update_exec(mm, entry->page_addr, false);
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

	lkmdbg_unregister_mmu_hwpoint_global(entry);
	lkmdbg_disable_user_single_step_tid(rearm_tid);

	task = get_pid_task(find_vpid(entry->tgid), PIDTYPE_TGID);
	if (task) {
		mm = get_task_mm(task);
		put_task_struct(task);
	}
	if (!mm)
		return 0;

	lkmdbg_mmu_update_exec(mm, entry->page_addr, true);
	mmput(mm);
	return 0;
}

static int lkmdbg_rearm_mmu_hwpoint_locked(struct lkmdbg_session *session,
					   struct lkmdbg_hwpoint *entry)
{
	struct mm_struct *mm = NULL;
	int ret;

	if (!lkmdbg_hwpoint_is_mmu_exec(entry))
		return -EINVAL;
	if (entry->rearm_step_armed)
		return -EBUSY;
	if (entry->armed && !atomic_read(&entry->stop_latched))
		return 0;

	ret = lkmdbg_get_target_mm(session, &mm);
	if (ret)
		return ret;

	ret = lkmdbg_mmu_update_exec(mm, entry->page_addr, false);
	mmput(mm);
	if (ret)
		return ret;

	atomic_set(&entry->stop_latched, 0);
	entry->armed = true;
	return 0;
}

static struct lkmdbg_hwpoint *
lkmdbg_find_breakpoint_by_addr_locked(struct lkmdbg_session *session, u64 addr,
				      u32 flags)
{
	struct lkmdbg_hwpoint *entry;

	list_for_each_entry(entry, &session->hwpoints, node) {
		if (entry->addr != addr)
			continue;
		if ((entry->type | entry->flags) != flags)
			continue;
		return entry;
	}

	return NULL;
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
	WRITE_ONCE(lkmdbg_state.hwpoint_last_tgid, entry->tgid);
	WRITE_ONCE(lkmdbg_state.hwpoint_last_tid, current->pid);
	WRITE_ONCE(lkmdbg_state.hwpoint_last_reason, reason);
	WRITE_ONCE(lkmdbg_state.hwpoint_last_type, entry->type);
	WRITE_ONCE(lkmdbg_state.hwpoint_last_addr, addr);
	WRITE_ONCE(lkmdbg_state.hwpoint_last_ip, ip);
	if (entry->flags & LKMDBG_HWPOINT_FLAG_COUNTER_MODE)
		return;
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
#ifdef CONFIG_ARM64
	if (req->flags & LKMDBG_HWPOINT_FLAG_MMU_EXEC)
		return lkmdbg_register_mmu_hwpoint(session, req);
#else
	if (req->flags & LKMDBG_HWPOINT_FLAG_MMU_EXEC)
		return -EOPNOTSUPP;
#endif

	struct perf_event_attr attr;
	struct lkmdbg_hwpoint *entry;
	struct task_struct *task = NULL;
	u64 id;
	int ret;

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
	atomic64_set(&entry->hits, 0);
	refcount_set(&entry->refs, 1);
	entry->armed = false;
	atomic_set(&entry->stop_latched, 0);
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
	mmu_exec = lkmdbg_hwpoint_is_mmu_exec(entry);
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
	if (lkmdbg_hwpoint_is_mmu_exec(entry))
		return lkmdbg_rearm_mmu_hwpoint_locked(entry->session, entry);
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

	atomic_set(&entry->stop_latched, 0);
	WRITE_ONCE(entry->armed, true);
	return 0;
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

	lkmdbg_session_broadcast_target_event(
		task->tgid, LKMDBG_EVENT_TARGET_SIGNAL, sig, task->pid,
		group ? LKMDBG_SIGNAL_EVENT_GROUP : 0, siginfo_code, result);
}

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
static int lkmdbg_do_page_fault_replacement(unsigned long far,
					    unsigned long esr,
					    struct pt_regs *regs)
{
	struct lkmdbg_hwpoint *entry = NULL;
	struct lkmdbg_regs_arm64 stop_regs;
	u64 ip;
	unsigned long irqflags;
	int ret;

	atomic_inc(&lkmdbg_do_page_fault_inflight);

	if (!user_mode(regs) || !current->mm ||
	    !lkmdbg_is_el0_instruction_permission_fault(esr))
		goto passthrough;

	ip = instruction_pointer(regs);
	spin_lock_irqsave(&lkmdbg_mmu_hwpoint_lock, irqflags);
	entry = lkmdbg_mmu_find_breakpoint_locked(current->tgid,
						  (unsigned long)far &
							  PAGE_MASK);
	if (entry)
		lkmdbg_hwpoint_get(entry);
	spin_unlock_irqrestore(&lkmdbg_mmu_hwpoint_lock, irqflags);
	if (!entry)
		goto passthrough;

	ret = lkmdbg_mmu_update_exec(current->mm, entry->page_addr, true);
	if (ret) {
		lkmdbg_hwpoint_put(entry);
		goto passthrough;
	}

	atomic64_inc(&lkmdbg_state.hwpoint_callback_total);
	atomic64_inc(&lkmdbg_state.breakpoint_callback_total);
	atomic64_inc(&entry->hits);
	WRITE_ONCE(lkmdbg_state.hwpoint_last_tgid, entry->tgid);
	WRITE_ONCE(lkmdbg_state.hwpoint_last_tid, current->pid);
	WRITE_ONCE(lkmdbg_state.hwpoint_last_reason, LKMDBG_STOP_REASON_BREAKPOINT);
	WRITE_ONCE(lkmdbg_state.hwpoint_last_type, entry->type);
	WRITE_ONCE(lkmdbg_state.hwpoint_last_addr, entry->addr);
	WRITE_ONCE(lkmdbg_state.hwpoint_last_ip, ip);

	if (!atomic_xchg(&entry->stop_latched, 1)) {
		entry->armed = false;
		lkmdbg_regs_arm64_export_stop(&stop_regs, regs);
		lkmdbg_session_request_async_stop(
			entry->session, LKMDBG_STOP_REASON_BREAKPOINT,
			entry->tgid, current->pid,
			entry->type | entry->flags,
			LKMDBG_STOP_FLAG_REARM_REQUIRED, entry->addr, ip,
			&stop_regs);
	}

	lkmdbg_hwpoint_put(entry);
	if (atomic_dec_and_test(&lkmdbg_do_page_fault_inflight))
		wake_up_all(&lkmdbg_do_page_fault_waitq);
	return 0;

passthrough:
	ret = lkmdbg_do_page_fault_orig ?
		      lkmdbg_do_page_fault_orig(far, esr, regs) :
		      0;
	if (atomic_dec_and_test(&lkmdbg_do_page_fault_inflight))
		wake_up_all(&lkmdbg_do_page_fault_waitq);
	return ret;
}

static int lkmdbg_user_step_handler(struct pt_regs *regs, unsigned long esr)
{
	struct lkmdbg_session *matched;
	struct lkmdbg_hwpoint *entry = NULL;
	pid_t tid = current->pid;
	pid_t tgid = current->tgid;
	lkmdbg_user_single_step_fn disable_fn;
	unsigned long irqflags;

	(void)esr;

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
		if (!lkmdbg_mmu_update_exec(current->mm, entry->page_addr, false)) {
			atomic_set(&entry->stop_latched, 0);
			entry->armed = true;
		}
		lkmdbg_hwpoint_put(entry);
		return DBG_HOOK_HANDLED;
	}

	matched = lkmdbg_session_consume_single_step(tgid, tid);
	if (!matched)
		return DBG_HOOK_ERROR;

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
	return DBG_HOOK_HANDLED;
}
#endif

static int lkmdbg_register_trace_hooks(void)
{
	lkmdbg_tracepoint_probe_register_fn register_fn;
	int ret;

	register_fn = (lkmdbg_tracepoint_probe_register_fn)
		lkmdbg_symbols.tracepoint_probe_register_sym;
	if (!register_fn) {
		pr_info("lkmdbg: tracepoint register helper unavailable\n");
		return 0;
	}

	lkmdbg_trace_fork_tp = lkmdbg_find_tracepoint("sched_process_fork");
	if (lkmdbg_trace_fork_tp) {
		ret = register_fn(lkmdbg_trace_fork_tp,
				  (void *)lkmdbg_trace_sched_process_fork, NULL);
		if (!ret)
			lkmdbg_trace_fork_registered = true;
		else
			pr_warn("lkmdbg: sched_process_fork trace hook failed ret=%d\n",
				ret);
	} else {
		pr_info("lkmdbg: sched_process_fork tracepoint unavailable\n");
	}

	lkmdbg_trace_exec_tp = lkmdbg_find_tracepoint("sched_process_exec");
	if (lkmdbg_trace_exec_tp) {
		ret = register_fn(lkmdbg_trace_exec_tp,
				  (void *)lkmdbg_trace_sched_process_exec, NULL);
		if (!ret)
			lkmdbg_trace_exec_registered = true;
		else
			pr_warn("lkmdbg: sched_process_exec trace hook failed ret=%d\n",
				ret);
	} else {
		pr_info("lkmdbg: sched_process_exec tracepoint unavailable\n");
	}

	lkmdbg_trace_exit_tp = lkmdbg_find_tracepoint("sched_process_exit");
	if (lkmdbg_trace_exit_tp) {
		ret = register_fn(lkmdbg_trace_exit_tp,
				  (void *)lkmdbg_trace_sched_process_exit, NULL);
		if (!ret)
			lkmdbg_trace_exit_registered = true;
		else
			pr_warn("lkmdbg: sched_process_exit trace hook failed ret=%d\n",
				ret);
	} else {
		pr_info("lkmdbg: sched_process_exit tracepoint unavailable\n");
	}

	lkmdbg_trace_signal_tp = lkmdbg_find_tracepoint("signal_generate");
	if (lkmdbg_trace_signal_tp) {
		ret = register_fn(lkmdbg_trace_signal_tp,
				  (void *)lkmdbg_trace_signal_generate, NULL);
		if (!ret)
			lkmdbg_trace_signal_registered = true;
		else
			pr_warn("lkmdbg: signal_generate trace hook failed ret=%d\n",
				ret);
	} else {
		pr_info("lkmdbg: signal_generate tracepoint unavailable\n");
	}

	return 0;
}

int lkmdbg_thread_ctrl_init(void)
{
#ifdef CONFIG_ARM64
	if (lkmdbg_symbols.register_user_step_hook_sym &&
	    lkmdbg_symbols.unregister_user_step_hook_sym &&
	    lkmdbg_symbols.user_enable_single_step_sym &&
	    lkmdbg_symbols.user_disable_single_step_sym) {
		((lkmdbg_register_user_step_hook_fn)
			 lkmdbg_symbols.register_user_step_hook_sym)(
			&lkmdbg_user_step_hook);
		lkmdbg_user_step_hook_registered = true;
	}
#endif

	return lkmdbg_register_trace_hooks();
}

void lkmdbg_thread_ctrl_exit(void)
{
	lkmdbg_tracepoint_probe_unregister_fn unregister_fn;

	unregister_fn = (lkmdbg_tracepoint_probe_unregister_fn)
		lkmdbg_symbols.tracepoint_probe_unregister_sym;

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

#ifdef CONFIG_ARM64
	if (lkmdbg_user_step_hook_registered) {
		((lkmdbg_unregister_user_step_hook_fn)
			 lkmdbg_symbols.unregister_user_step_hook_sym)(
			&lkmdbg_user_step_hook);
		lkmdbg_user_step_hook_registered = false;
	}

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
	lkmdbg_do_page_fault_orig = NULL;
#endif
}

void lkmdbg_thread_ctrl_release(struct lkmdbg_session *session)
{
	struct lkmdbg_hwpoint *entry;
	struct lkmdbg_hwpoint *tmp;
#ifdef CONFIG_ARM64
	lkmdbg_user_single_step_fn disable_fn;
	pid_t step_tid;
#endif

	if (!session)
		return;

	mutex_lock(&session->lock);
#ifdef CONFIG_ARM64
	step_tid = session->step_tid;
	WRITE_ONCE(session->step_armed, false);
	WRITE_ONCE(session->step_tgid, 0);
	WRITE_ONCE(session->step_tid, 0);
#endif
	list_for_each_entry_safe(entry, tmp, &session->hwpoints, node) {
		list_del_init(&entry->node);
		mutex_unlock(&session->lock);
		if (lkmdbg_hwpoint_is_mmu_exec(entry))
			lkmdbg_unregister_mmu_hwpoint(entry);
		else
			unregister_hw_breakpoint(entry->event);
		lkmdbg_hwpoint_put(entry);
		mutex_lock(&session->lock);
	}
	mutex_unlock(&session->lock);

#ifdef CONFIG_ARM64
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

	if (!(flags & LKMDBG_CONTINUE_FLAG_REARM_HWPOINTS))
		return 0;

	if (!stop || stop->reason != LKMDBG_STOP_REASON_BREAKPOINT ||
	    !(stop->flags & LKMDBG_STOP_FLAG_REARM_REQUIRED) ||
	    !(stop->event_flags & LKMDBG_HWPOINT_FLAG_MMU_EXEC))
		return lkmdbg_rearm_all_hwpoints(session);

	mutex_lock(&session->lock);
	entry = lkmdbg_find_breakpoint_by_addr_locked(session, stop->value0,
						      stop->event_flags);
	mutex_unlock(&session->lock);
	if (!entry)
		return 0;

	return 0;
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
	if (!lkmdbg_user_step_hook_registered ||
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
