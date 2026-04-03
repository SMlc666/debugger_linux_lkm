#include <linux/kernel.h>
#include <linux/kprobes.h>

#include "lkmdbg_internal.h"

typedef unsigned long (*lkmdbg_kallsyms_lookup_name_runtime_fn)(
	const char *name);
typedef void (*lkmdbg_register_user_step_hook_runtime_fn)(void *hook);
typedef void (*lkmdbg_unregister_user_step_hook_runtime_fn)(void *hook);
typedef void (*lkmdbg_user_single_step_runtime_fn)(struct task_struct *task);
typedef int (*lkmdbg_task_work_add_runtime_fn)(struct task_struct *task,
					       struct callback_head *work,
					       unsigned int notify);
typedef struct callback_head *(*lkmdbg_task_work_cancel_match_runtime_fn)(
	struct task_struct *task,
	bool (*match)(struct callback_head *cb, void *data), void *data);
typedef struct callback_head *(*lkmdbg_task_work_cancel_func_runtime_fn)(
	struct task_struct *task, task_work_func_t func);
#ifdef TWA_RESUME
typedef bool (*lkmdbg_task_work_cancel_runtime_fn)(struct task_struct *task,
						   struct callback_head *work);
#else
typedef struct callback_head *(*lkmdbg_task_work_cancel_runtime_fn)(
	struct task_struct *task, task_work_func_t func);
#endif
typedef void (*lkmdbg_for_each_kernel_tracepoint_runtime_fn)(
	void (*fct)(struct tracepoint *tp, void *priv), void *priv);
typedef int (*lkmdbg_tracepoint_probe_register_runtime_fn)(
	struct tracepoint *tp, void *probe, void *data);
typedef int (*lkmdbg_tracepoint_probe_unregister_runtime_fn)(
	struct tracepoint *tp, void *probe, void *data);
typedef int (*lkmdbg_kern_path_runtime_fn)(const char *name, unsigned int flags,
					   struct path *path);
typedef void (*lkmdbg_perf_event_disable_local_fn)(struct perf_event *event);
typedef void (*lkmdbg_path_put_runtime_fn)(const struct path *path);
typedef void (*lkmdbg_d_drop_runtime_fn)(struct dentry *dentry);
typedef void (*lkmdbg_unpin_user_pages_dirty_lock_runtime_fn)(
	struct page **pages, unsigned long npages, bool make_dirty);
typedef struct perf_event *(*lkmdbg_register_user_hw_breakpoint_runtime_fn)(
	struct perf_event_attr *attr, void *triggered, void *context,
	struct task_struct *task);
typedef int (*lkmdbg_modify_user_hw_breakpoint_runtime_fn)(
	struct perf_event *bp, struct perf_event_attr *attr);
typedef void (*lkmdbg_unregister_hw_breakpoint_runtime_fn)(
	struct perf_event *bp);

unsigned long __nocfi lkmdbg_kallsyms_lookup_name_runtime(const char *name)
{
	lkmdbg_kallsyms_lookup_name_runtime_fn fn;

	if (!name || !lkmdbg_symbols.kallsyms_lookup_name)
		return 0;

	fn = (lkmdbg_kallsyms_lookup_name_runtime_fn)
		lkmdbg_symbols.kallsyms_lookup_name;
	return fn(name);
}

int __nocfi lkmdbg_aarch64_insn_write_runtime(void *addr, u32 insn)
{
	if (!lkmdbg_symbols.aarch64_insn_write)
		return -EOPNOTSUPP;

	return lkmdbg_symbols.aarch64_insn_write(addr, insn);
}

int __nocfi lkmdbg_aarch64_insn_patch_text_nosync_runtime(void *addr, u32 insn)
{
	if (!lkmdbg_symbols.aarch64_insn_patch_text_nosync)
		return -EOPNOTSUPP;

	return lkmdbg_symbols.aarch64_insn_patch_text_nosync(addr, insn);
}

int __nocfi lkmdbg_register_user_step_hook_runtime(void *hook)
{
	lkmdbg_register_user_step_hook_runtime_fn fn;

	if (!hook || !lkmdbg_symbols.register_user_step_hook_sym)
		return -EOPNOTSUPP;

	fn = (lkmdbg_register_user_step_hook_runtime_fn)
		lkmdbg_symbols.register_user_step_hook_sym;
	fn(hook);
	return 0;
}

void __nocfi lkmdbg_unregister_user_step_hook_runtime(void *hook)
{
	lkmdbg_unregister_user_step_hook_runtime_fn fn;

	if (!hook || !lkmdbg_symbols.unregister_user_step_hook_sym)
		return;

	fn = (lkmdbg_unregister_user_step_hook_runtime_fn)
		lkmdbg_symbols.unregister_user_step_hook_sym;
	fn(hook);
}

void __nocfi lkmdbg_user_enable_single_step_runtime(struct task_struct *task)
{
	lkmdbg_user_single_step_runtime_fn fn;

	if (!task || !lkmdbg_symbols.user_enable_single_step_sym)
		return;

	fn = (lkmdbg_user_single_step_runtime_fn)
		lkmdbg_symbols.user_enable_single_step_sym;
	fn(task);
}

void __nocfi lkmdbg_user_disable_single_step_runtime(struct task_struct *task)
{
	lkmdbg_user_single_step_runtime_fn fn;

	if (!task || !lkmdbg_symbols.user_disable_single_step_sym)
		return;

	fn = (lkmdbg_user_single_step_runtime_fn)
		lkmdbg_symbols.user_disable_single_step_sym;
	fn(task);
}

void __nocfi lkmdbg_perf_event_disable_local_runtime(struct perf_event *event)
{
	lkmdbg_perf_event_disable_local_fn fn;

	if (!event || !lkmdbg_symbols.perf_event_disable_local_sym)
		return;

	fn = (lkmdbg_perf_event_disable_local_fn)
		lkmdbg_symbols.perf_event_disable_local_sym;
	fn(event);
}

void __nocfi lkmdbg_for_each_kernel_tracepoint_runtime(
	void (*fct)(struct tracepoint *tp, void *priv), void *priv)
{
	lkmdbg_for_each_kernel_tracepoint_runtime_fn fn;

	if (!fct || !lkmdbg_symbols.for_each_kernel_tracepoint_sym)
		return;

	fn = (lkmdbg_for_each_kernel_tracepoint_runtime_fn)
		lkmdbg_symbols.for_each_kernel_tracepoint_sym;
	fn(fct, priv);
}

int __nocfi lkmdbg_tracepoint_probe_register_runtime(struct tracepoint *tp,
						       void *probe,
						       void *data)
{
	lkmdbg_tracepoint_probe_register_runtime_fn fn;

	if (!tp || !probe || !lkmdbg_symbols.tracepoint_probe_register_sym)
		return -EOPNOTSUPP;

	fn = (lkmdbg_tracepoint_probe_register_runtime_fn)
		lkmdbg_symbols.tracepoint_probe_register_sym;
	return fn(tp, probe, data);
}

int __nocfi lkmdbg_tracepoint_probe_unregister_runtime(struct tracepoint *tp,
							 void *probe,
							 void *data)
{
	lkmdbg_tracepoint_probe_unregister_runtime_fn fn;

	if (!tp || !probe || !lkmdbg_symbols.tracepoint_probe_unregister_sym)
		return -EOPNOTSUPP;

	fn = (lkmdbg_tracepoint_probe_unregister_runtime_fn)
		lkmdbg_symbols.tracepoint_probe_unregister_sym;
	return fn(tp, probe, data);
}

static unsigned long lkmdbg_lookup_runtime_symbol(const char *name)
{
	struct kprobe kp = {
		.symbol_name = name,
	};
	unsigned long addr = 0;

	if (lkmdbg_symbols.kallsyms_lookup_name)
		addr = lkmdbg_kallsyms_lookup_name_runtime(name);
	if (addr)
		return addr;

	if (register_kprobe(&kp))
		return 0;

	addr = (unsigned long)kp.addr;
	unregister_kprobe(&kp);
	return addr;
}

unsigned long lkmdbg_lookup_runtime_symbol_any(const char *name)
{
	return lkmdbg_lookup_runtime_symbol(name);
}

unsigned long lkmdbg_lookup_runtime_symbol_prefix(const char *prefix)
{
	(void)prefix;
	return 0;
}

static int lkmdbg_resolve_runtime_symbols(void)
{
	static struct kprobe kp = {
		.symbol_name = "kallsyms_lookup_name",
	};
	unsigned long addr;

	if (lkmdbg_symbols.kallsyms_lookup_name)
		return 0;

	if (register_kprobe(&kp))
		return -ENOENT;

	lkmdbg_symbols.kallsyms_lookup_name =
		(unsigned long (*)(const char *name))kp.addr;
	unregister_kprobe(&kp);

	if (!lkmdbg_symbols.kallsyms_lookup_name)
		return -ENOENT;

	addr = lkmdbg_kallsyms_lookup_name_runtime("filp_open");
	if (!addr)
		return -ENOENT;
	lkmdbg_symbols.filp_open =
		(struct file *(*)(const char *filename, int flags, umode_t mode))addr;

	addr = lkmdbg_kallsyms_lookup_name_runtime("filp_close");
	if (!addr)
		return -ENOENT;
	lkmdbg_symbols.filp_close =
		(int (*)(struct file *file, fl_owner_t id))addr;

	addr = lkmdbg_kallsyms_lookup_name_runtime("aarch64_insn_write");
	if (!addr)
		return -ENOENT;
	lkmdbg_symbols.aarch64_insn_write = (int (*)(void *, u32))addr;

	addr = lkmdbg_kallsyms_lookup_name_runtime(
		"aarch64_insn_patch_text_nosync");
	if (addr)
		lkmdbg_symbols.aarch64_insn_patch_text_nosync =
			(int (*)(void *, u32))addr;

	addr = lkmdbg_kallsyms_lookup_name_runtime("caches_clean_inval_pou");
	if (!addr)
		addr = lkmdbg_kallsyms_lookup_name_runtime("__flush_icache_range");
	if (!addr)
		return -ENOENT;
	lkmdbg_symbols.flush_icache_range =
		(void (*)(unsigned long, unsigned long))addr;

	addr = lkmdbg_kallsyms_lookup_name_runtime("set_memory_x");
	if (addr)
		lkmdbg_symbols.set_memory_x =
			(int (*)(unsigned long, int))addr;

	addr = lkmdbg_kallsyms_lookup_name_runtime("module_alloc");
	if (addr)
		lkmdbg_symbols.module_alloc =
			(void *(*)(unsigned long size))addr;

	addr = lkmdbg_kallsyms_lookup_name_runtime("module_memfree");
	if (addr)
		lkmdbg_symbols.module_memfree = (void (*)(void *region))addr;

	addr = lkmdbg_kallsyms_lookup_name_runtime("init_mm");
	if (addr)
		lkmdbg_symbols.init_mm = (struct mm_struct *)addr;

	addr = lkmdbg_kallsyms_lookup_name_runtime("task_work_add");
	if (addr)
		lkmdbg_symbols.task_work_add_sym = addr;

	addr = lkmdbg_kallsyms_lookup_name_runtime("task_work_cancel_match");
	if (addr)
		lkmdbg_symbols.task_work_cancel_match_sym = addr;

	addr = lkmdbg_kallsyms_lookup_name_runtime("task_work_cancel_func");
	if (addr)
		lkmdbg_symbols.task_work_cancel_func_sym = addr;

	addr = lkmdbg_kallsyms_lookup_name_runtime("task_work_cancel");
	if (addr)
		lkmdbg_symbols.task_work_cancel_sym = addr;

	addr = lkmdbg_kallsyms_lookup_name_runtime("register_user_step_hook");
	if (addr)
		lkmdbg_symbols.register_user_step_hook_sym = addr;

	addr = lkmdbg_kallsyms_lookup_name_runtime("unregister_user_step_hook");
	if (addr)
		lkmdbg_symbols.unregister_user_step_hook_sym = addr;

	addr = lkmdbg_kallsyms_lookup_name_runtime("user_enable_single_step");
	if (addr)
		lkmdbg_symbols.user_enable_single_step_sym = addr;

	addr = lkmdbg_kallsyms_lookup_name_runtime("user_disable_single_step");
	if (addr)
		lkmdbg_symbols.user_disable_single_step_sym = addr;

	addr = lkmdbg_lookup_runtime_symbol("for_each_kernel_tracepoint");
	if (addr)
		lkmdbg_symbols.for_each_kernel_tracepoint_sym = addr;

	addr = lkmdbg_lookup_runtime_symbol("tracepoint_probe_register");
	if (addr)
		lkmdbg_symbols.tracepoint_probe_register_sym = addr;

	addr = lkmdbg_lookup_runtime_symbol("tracepoint_probe_unregister");
	if (addr)
		lkmdbg_symbols.tracepoint_probe_unregister_sym = addr;

	addr = lkmdbg_lookup_runtime_symbol("perf_event_disable_local");
	if (addr)
		lkmdbg_symbols.perf_event_disable_local_sym = addr;

	addr = lkmdbg_lookup_runtime_symbol("__do_page_fault");
	if (addr)
		lkmdbg_symbols.do_page_fault_inner_sym = addr;

	addr = lkmdbg_lookup_runtime_symbol("do_page_fault");
	if (addr)
		lkmdbg_symbols.do_page_fault_sym = addr;

	addr = lkmdbg_lookup_runtime_symbol("do_el0_softstep");
	if (addr)
		lkmdbg_symbols.do_el0_softstep_sym = addr;

	addr = lkmdbg_lookup_runtime_symbol("invoke_syscall");
	if (addr)
		lkmdbg_symbols.invoke_syscall_sym = addr;

	addr = lkmdbg_lookup_runtime_symbol("__invoke_syscall");
	if (addr)
		lkmdbg_symbols.invoke_syscall_inner_sym = addr;

	addr = lkmdbg_lookup_runtime_symbol("do_el0_svc");
	if (addr)
		lkmdbg_symbols.do_el0_svc_sym = addr;

	addr = lkmdbg_lookup_runtime_symbol("process_vm_rw");
	if (addr)
		lkmdbg_symbols.process_vm_rw_sym = addr;

	addr = lkmdbg_lookup_runtime_symbol("__do_sys_process_vm_writev");
	if (addr)
		lkmdbg_symbols.do_sys_process_vm_writev_sym = addr;

	addr = lkmdbg_lookup_runtime_symbol("access_remote_vm");
	if (addr)
		lkmdbg_symbols.access_remote_vm_sym = addr;

	addr = lkmdbg_lookup_runtime_symbol("__access_remote_vm");
	if (addr)
		lkmdbg_symbols.access_remote_vm_inner_sym = addr;

	addr = lkmdbg_lookup_runtime_symbol("kern_path");
	if (addr)
		lkmdbg_symbols.kern_path_sym = addr;

	addr = lkmdbg_lookup_runtime_symbol("path_put");
	if (addr)
		lkmdbg_symbols.path_put_sym = addr;

	addr = lkmdbg_lookup_runtime_symbol("d_drop");
	if (addr)
		lkmdbg_symbols.d_drop_sym = addr;

	addr = lkmdbg_lookup_runtime_symbol("unpin_user_pages_dirty_lock");
	if (addr)
		lkmdbg_symbols.unpin_user_pages_dirty_lock_sym = addr;

	addr = lkmdbg_lookup_runtime_symbol("register_user_hw_breakpoint");
	if (addr)
		lkmdbg_symbols.register_user_hw_breakpoint_sym = addr;

	addr = lkmdbg_lookup_runtime_symbol("modify_user_hw_breakpoint");
	if (addr)
		lkmdbg_symbols.modify_user_hw_breakpoint_sym = addr;

	addr = lkmdbg_lookup_runtime_symbol("unregister_hw_breakpoint");
	if (addr)
		lkmdbg_symbols.unregister_hw_breakpoint_sym = addr;

	return 0;
}

int lkmdbg_symbols_init(void)
{
	return lkmdbg_resolve_runtime_symbols();
}

void __nocfi lkmdbg_flush_icache_runtime(unsigned long start, unsigned long end)
{
	if (!lkmdbg_symbols.flush_icache_range)
		return;

	lkmdbg_symbols.flush_icache_range(start, end);
}

int __nocfi lkmdbg_task_work_add_runtime(struct task_struct *task,
					 struct callback_head *work,
					 unsigned int notify)
{
	lkmdbg_task_work_add_runtime_fn fn;

	if (!lkmdbg_symbols.task_work_add_sym)
		return -EOPNOTSUPP;

	fn = (lkmdbg_task_work_add_runtime_fn)lkmdbg_symbols.task_work_add_sym;
	return fn(task, work, notify);
}

struct callback_head *__nocfi lkmdbg_task_work_cancel_match_runtime(
	struct task_struct *task,
	bool (*match)(struct callback_head *cb, void *data), void *data)
{
	lkmdbg_task_work_cancel_match_runtime_fn fn;

	if (!task || !match || !lkmdbg_symbols.task_work_cancel_match_sym)
		return NULL;

	fn = (lkmdbg_task_work_cancel_match_runtime_fn)
		lkmdbg_symbols.task_work_cancel_match_sym;
	return fn(task, match, data);
}

struct callback_head *__nocfi lkmdbg_task_work_cancel_func_runtime(
	struct task_struct *task, task_work_func_t func)
{
	lkmdbg_task_work_cancel_func_runtime_fn fn;

	if (!task || !func || !lkmdbg_symbols.task_work_cancel_func_sym)
		return NULL;

	fn = (lkmdbg_task_work_cancel_func_runtime_fn)
		lkmdbg_symbols.task_work_cancel_func_sym;
	return fn(task, func);
}

#ifdef TWA_RESUME
bool __nocfi lkmdbg_task_work_cancel_runtime(struct task_struct *task,
					     struct callback_head *work)
{
	lkmdbg_task_work_cancel_runtime_fn fn;

	if (!task || !work || !lkmdbg_symbols.task_work_cancel_sym)
		return false;

	fn = (lkmdbg_task_work_cancel_runtime_fn)
		lkmdbg_symbols.task_work_cancel_sym;
	return fn(task, work);
}
#else
struct callback_head *__nocfi lkmdbg_task_work_cancel_runtime(
	struct task_struct *task, task_work_func_t func)
{
	lkmdbg_task_work_cancel_runtime_fn fn;

	if (!task || !func || !lkmdbg_symbols.task_work_cancel_sym)
		return NULL;

	fn = (lkmdbg_task_work_cancel_runtime_fn)
		lkmdbg_symbols.task_work_cancel_sym;
	return fn(task, func);
}
#endif

int __nocfi lkmdbg_kern_path_runtime(const char *name, unsigned int flags,
				     struct path *path)
{
	lkmdbg_kern_path_runtime_fn fn;

	if (!lkmdbg_symbols.kern_path_sym)
		return -EOPNOTSUPP;

	fn = (lkmdbg_kern_path_runtime_fn)lkmdbg_symbols.kern_path_sym;
	return fn(name, flags, path);
}

void __nocfi lkmdbg_path_put_runtime(const struct path *path)
{
	lkmdbg_path_put_runtime_fn fn;

	if (!lkmdbg_symbols.path_put_sym)
		return;

	fn = (lkmdbg_path_put_runtime_fn)lkmdbg_symbols.path_put_sym;
	fn(path);
}

void __nocfi lkmdbg_d_drop_runtime(struct dentry *dentry)
{
	lkmdbg_d_drop_runtime_fn fn;

	if (!dentry || !lkmdbg_symbols.d_drop_sym)
		return;

	fn = (lkmdbg_d_drop_runtime_fn)lkmdbg_symbols.d_drop_sym;
	fn(dentry);
}

void __nocfi lkmdbg_unpin_user_pages_dirty_lock_runtime(
	struct page **pages, unsigned long npages, bool make_dirty)
{
	unsigned long i;

	if (!pages || !npages)
		return;

#ifdef FOLL_PIN
	if (lkmdbg_symbols.unpin_user_pages_dirty_lock_sym) {
		lkmdbg_unpin_user_pages_dirty_lock_runtime_fn fn;

		fn = (lkmdbg_unpin_user_pages_dirty_lock_runtime_fn)
			lkmdbg_symbols.unpin_user_pages_dirty_lock_sym;
		fn(pages, npages, make_dirty);
		return;
	}

	for (i = 0; i < npages; i++) {
		if (!pages[i])
			continue;
		if (make_dirty)
			set_page_dirty_lock(pages[i]);
		unpin_user_page(pages[i]);
	}
#else
	(void)make_dirty;
	for (i = 0; i < npages; i++) {
		if (!pages[i])
			continue;
		put_page(pages[i]);
	}
#endif
}

bool lkmdbg_hw_breakpoint_runtime_available(void)
{
	return lkmdbg_symbols.register_user_hw_breakpoint_sym &&
	       lkmdbg_symbols.modify_user_hw_breakpoint_sym &&
	       lkmdbg_symbols.unregister_hw_breakpoint_sym;
}

struct perf_event *__nocfi lkmdbg_register_user_hw_breakpoint_runtime(
	struct perf_event_attr *attr, void *triggered, void *context,
	struct task_struct *task)
{
	lkmdbg_register_user_hw_breakpoint_runtime_fn fn;

	if (!lkmdbg_symbols.register_user_hw_breakpoint_sym)
		return ERR_PTR(-EOPNOTSUPP);

	fn = (lkmdbg_register_user_hw_breakpoint_runtime_fn)
		lkmdbg_symbols.register_user_hw_breakpoint_sym;
	return fn(attr, triggered, context, task);
}

int __nocfi lkmdbg_modify_user_hw_breakpoint_runtime(
	struct perf_event *bp, struct perf_event_attr *attr)
{
	lkmdbg_modify_user_hw_breakpoint_runtime_fn fn;

	if (!lkmdbg_symbols.modify_user_hw_breakpoint_sym)
		return -EOPNOTSUPP;

	fn = (lkmdbg_modify_user_hw_breakpoint_runtime_fn)
		lkmdbg_symbols.modify_user_hw_breakpoint_sym;
	return fn(bp, attr);
}

void __nocfi lkmdbg_unregister_hw_breakpoint_runtime(struct perf_event *bp)
{
	lkmdbg_unregister_hw_breakpoint_runtime_fn fn;

	if (!lkmdbg_symbols.unregister_hw_breakpoint_sym)
		return;

	fn = (lkmdbg_unregister_hw_breakpoint_runtime_fn)
		lkmdbg_symbols.unregister_hw_breakpoint_sym;
	fn(bp);
}

void lkmdbg_symbols_exit(void)
{
	lkmdbg_symbols.kallsyms_lookup_name = NULL;
	lkmdbg_symbols.filp_open = NULL;
	lkmdbg_symbols.filp_close = NULL;
	lkmdbg_symbols.aarch64_insn_patch_text_nosync = NULL;
	lkmdbg_symbols.aarch64_insn_write = NULL;
	lkmdbg_symbols.flush_icache_range = NULL;
	lkmdbg_symbols.set_memory_x = NULL;
	lkmdbg_symbols.module_alloc = NULL;
	lkmdbg_symbols.module_memfree = NULL;
	lkmdbg_symbols.init_mm = NULL;
	lkmdbg_symbols.task_work_add_sym = 0;
	lkmdbg_symbols.task_work_cancel_match_sym = 0;
	lkmdbg_symbols.task_work_cancel_func_sym = 0;
	lkmdbg_symbols.task_work_cancel_sym = 0;
	lkmdbg_symbols.register_user_step_hook_sym = 0;
	lkmdbg_symbols.unregister_user_step_hook_sym = 0;
	lkmdbg_symbols.user_enable_single_step_sym = 0;
	lkmdbg_symbols.user_disable_single_step_sym = 0;
	lkmdbg_symbols.for_each_kernel_tracepoint_sym = 0;
	lkmdbg_symbols.tracepoint_probe_register_sym = 0;
	lkmdbg_symbols.tracepoint_probe_unregister_sym = 0;
	lkmdbg_symbols.perf_event_disable_local_sym = 0;
	lkmdbg_symbols.do_page_fault_inner_sym = 0;
	lkmdbg_symbols.do_page_fault_sym = 0;
	lkmdbg_symbols.do_el0_softstep_sym = 0;
	lkmdbg_symbols.invoke_syscall_sym = 0;
	lkmdbg_symbols.invoke_syscall_inner_sym = 0;
	lkmdbg_symbols.do_el0_svc_sym = 0;
	lkmdbg_symbols.process_vm_rw_sym = 0;
	lkmdbg_symbols.do_sys_process_vm_writev_sym = 0;
	lkmdbg_symbols.access_remote_vm_sym = 0;
	lkmdbg_symbols.access_remote_vm_inner_sym = 0;
	lkmdbg_symbols.kern_path_sym = 0;
	lkmdbg_symbols.path_put_sym = 0;
	lkmdbg_symbols.d_drop_sym = 0;
	lkmdbg_symbols.unpin_user_pages_dirty_lock_sym = 0;
	lkmdbg_symbols.register_user_hw_breakpoint_sym = 0;
	lkmdbg_symbols.modify_user_hw_breakpoint_sym = 0;
	lkmdbg_symbols.unregister_hw_breakpoint_sym = 0;
}
