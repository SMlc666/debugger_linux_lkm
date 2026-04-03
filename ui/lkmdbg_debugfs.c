#include <linux/debugfs.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/seq_file.h>

#include "lkmdbg_internal.h"

static int lkmdbg_status_show(struct seq_file *m, void *unused)
{
	u64 reads;
	u64 calls;
	u64 opens;
	u64 sessions;
	u64 sessions_total;
	u64 hook_expected;
	u64 hook_actual;
	u64 inline_hook_create_total;
	u64 inline_hook_install_total;
	u64 inline_hook_remove_total;
	u64 inline_hook_active;
	u64 inline_hook_last_target;
	u64 inline_hook_last_origin;
	u64 inline_hook_last_replacement;
	u64 inline_hook_last_trampoline;
	u64 seq_read_hook_hits;
	u64 owner_proc_hide_hook_hits;
	u64 hwpoint_callback_total;
	u64 breakpoint_callback_total;
	u64 watchpoint_callback_total;
	u64 target_stop_event_read_total;
	u64 breakpoint_stop_event_read_total;
	u64 watchpoint_stop_event_read_total;
	u64 event_drop_total;
	u64 hwpoint_last_addr;
	u64 hwpoint_last_ip;
	u32 stealth_flags;
	u32 stealth_supported_flags;
	bool hook_active;
	bool debugfs_active;
	bool selftest_enabled;
	bool selftest_exec_pool_ready;
	bool selftest_exec_allocated;
	bool selftest_exec_ready;
	bool selftest_installed;
	bool seq_read_hook_active;
	bool owner_proc_hidden;
	bool owner_proc_hide_hook_active;
	pid_t owner_proc_hidden_tgid;
	pid_t hwpoint_last_tgid;
	pid_t hwpoint_last_tid;
	u32 hwpoint_last_reason;
	u32 hwpoint_last_type;
	int selftest_ret;
	int inline_hook_last_ret;
	int seq_read_hook_last_ret;
	int owner_proc_hide_hook_last_ret;

	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.status_reads++;
	reads = lkmdbg_state.status_reads;
	calls = lkmdbg_state.bootstrap_ioctl_calls;
	opens = lkmdbg_state.proc_open_successes;
	sessions = lkmdbg_state.active_sessions;
	sessions_total = lkmdbg_state.session_opened_total;
	hook_active = lkmdbg_state.proc_version_hook_active;
	selftest_enabled = lkmdbg_state.hook_selftest_enabled;
	selftest_exec_pool_ready = lkmdbg_state.hook_selftest_exec_pool_ready;
	selftest_exec_allocated = lkmdbg_state.hook_selftest_exec_allocated;
	selftest_exec_ready = lkmdbg_state.hook_selftest_exec_ready;
	selftest_installed = lkmdbg_state.hook_selftest_installed;
	selftest_ret = lkmdbg_state.hook_selftest_ret;
	hook_expected = lkmdbg_state.hook_selftest_expected;
	hook_actual = lkmdbg_state.hook_selftest_actual;
	inline_hook_create_total = lkmdbg_state.inline_hook_create_total;
	inline_hook_install_total = lkmdbg_state.inline_hook_install_total;
	inline_hook_remove_total = lkmdbg_state.inline_hook_remove_total;
	inline_hook_active = lkmdbg_state.inline_hook_active;
	inline_hook_last_ret = lkmdbg_state.inline_hook_last_ret;
	inline_hook_last_target = lkmdbg_state.inline_hook_last_target;
	inline_hook_last_origin = lkmdbg_state.inline_hook_last_origin;
	inline_hook_last_replacement = lkmdbg_state.inline_hook_last_replacement;
	inline_hook_last_trampoline = lkmdbg_state.inline_hook_last_trampoline;
	seq_read_hook_active = lkmdbg_state.seq_read_hook_active;
	seq_read_hook_hits = lkmdbg_state.seq_read_hook_hits;
	seq_read_hook_last_ret = lkmdbg_state.seq_read_hook_last_ret;
	debugfs_active = lkmdbg_state.debugfs_active;
	stealth_supported_flags = lkmdbg_state.stealth_supported_flags;
	stealth_flags = 0;
	if (debugfs_active)
		stealth_flags |= LKMDBG_STEALTH_FLAG_DEBUGFS_VISIBLE;
	if (lkmdbg_state.module_list_hidden)
		stealth_flags |= LKMDBG_STEALTH_FLAG_MODULE_LIST_HIDDEN;
	if (lkmdbg_state.sysfs_module_hidden)
		stealth_flags |= LKMDBG_STEALTH_FLAG_SYSFS_MODULE_HIDDEN;
	if (lkmdbg_state.owner_proc_hidden)
		stealth_flags |= LKMDBG_STEALTH_FLAG_OWNER_PROC_HIDDEN;
	owner_proc_hidden = lkmdbg_state.owner_proc_hidden;
	owner_proc_hidden_tgid = lkmdbg_state.owner_proc_hidden_tgid;
	owner_proc_hide_hook_active = lkmdbg_state.owner_proc_hide_hook_active;
	owner_proc_hide_hook_hits = lkmdbg_state.owner_proc_hide_hook_hits;
	owner_proc_hide_hook_last_ret = lkmdbg_state.owner_proc_hide_hook_last_ret;
	mutex_unlock(&lkmdbg_state.lock);

	hwpoint_callback_total = atomic64_read(&lkmdbg_state.hwpoint_callback_total);
	breakpoint_callback_total =
		atomic64_read(&lkmdbg_state.breakpoint_callback_total);
	watchpoint_callback_total =
		atomic64_read(&lkmdbg_state.watchpoint_callback_total);
	target_stop_event_read_total =
		atomic64_read(&lkmdbg_state.target_stop_event_read_total);
	breakpoint_stop_event_read_total =
		atomic64_read(&lkmdbg_state.breakpoint_stop_event_read_total);
	watchpoint_stop_event_read_total =
		atomic64_read(&lkmdbg_state.watchpoint_stop_event_read_total);
	event_drop_total = atomic64_read(&lkmdbg_state.event_drop_total);
	hwpoint_last_tgid = READ_ONCE(lkmdbg_state.hwpoint_last_tgid);
	hwpoint_last_tid = READ_ONCE(lkmdbg_state.hwpoint_last_tid);
	hwpoint_last_reason = READ_ONCE(lkmdbg_state.hwpoint_last_reason);
	hwpoint_last_type = READ_ONCE(lkmdbg_state.hwpoint_last_type);
	hwpoint_last_addr = READ_ONCE(lkmdbg_state.hwpoint_last_addr);
	hwpoint_last_ip = READ_ONCE(lkmdbg_state.hwpoint_last_ip);

	seq_printf(m, "tag=%s\n", tag);
	seq_printf(m, "load_jiffies=%lu\n", lkmdbg_state.load_jiffies);
	seq_printf(m, "status_reads=%llu\n", (unsigned long long)reads);
	seq_printf(m, "hook_requested=%u\n", hook_proc_version);
	seq_printf(m, "hook_active=%u\n", hook_active);
	seq_printf(m, "proc_version_hook_requested=%u\n", hook_proc_version);
	seq_printf(m, "proc_version_hook_active=%u\n", hook_active);
	seq_printf(m, "bootstrap_ioctl_calls=%llu\n",
		   (unsigned long long)calls);
	seq_printf(m, "proc_open_successes=%llu\n",
		   (unsigned long long)opens);
	seq_printf(m, "active_sessions=%llu\n",
		   (unsigned long long)sessions);
	seq_printf(m, "session_opened_total=%llu\n",
		   (unsigned long long)sessions_total);
	seq_printf(m, "target_path=%s\n", LKMDBG_TARGET_PATH);
	seq_printf(m, "debugfs_requested=%u\n", enable_debugfs);
	seq_printf(m, "debugfs_active=%u\n", debugfs_active);
	seq_printf(m, "stealth_flags=0x%x\n", stealth_flags);
	seq_printf(m, "stealth_supported_flags=0x%x\n",
		   stealth_supported_flags);
	seq_printf(m, "hook_selftest_mode=%u\n", hook_selftest_mode);
	seq_printf(m, "hook_selftest_enabled=%u\n", selftest_enabled);
	seq_printf(m, "hook_selftest_exec_pool_ready=%u\n",
		   selftest_exec_pool_ready);
	seq_printf(m, "hook_selftest_exec_allocated=%u\n", selftest_exec_allocated);
	seq_printf(m, "hook_selftest_exec_ready=%u\n", selftest_exec_ready);
	seq_printf(m, "hook_selftest_installed=%u\n", selftest_installed);
	seq_printf(m, "hook_selftest_ret=%d\n", selftest_ret);
	seq_printf(m, "hook_selftest_expected=0x%llx\n",
		   (unsigned long long)hook_expected);
	seq_printf(m, "hook_selftest_actual=0x%llx\n",
		   (unsigned long long)hook_actual);
	seq_printf(m, "inline_hook_create_total=%llu\n",
		   (unsigned long long)inline_hook_create_total);
	seq_printf(m, "inline_hook_install_total=%llu\n",
		   (unsigned long long)inline_hook_install_total);
	seq_printf(m, "inline_hook_remove_total=%llu\n",
		   (unsigned long long)inline_hook_remove_total);
	seq_printf(m, "inline_hook_active=%llu\n",
		   (unsigned long long)inline_hook_active);
	seq_printf(m, "inline_hook_last_ret=%d\n", inline_hook_last_ret);
	seq_printf(m, "inline_hook_last_target=0x%llx\n",
		   (unsigned long long)inline_hook_last_target);
	seq_printf(m, "inline_hook_last_origin=0x%llx\n",
		   (unsigned long long)inline_hook_last_origin);
	seq_printf(m, "inline_hook_last_replacement=0x%llx\n",
		   (unsigned long long)inline_hook_last_replacement);
	seq_printf(m, "inline_hook_last_trampoline=0x%llx\n",
		   (unsigned long long)inline_hook_last_trampoline);
	seq_printf(m, "seq_read_hook_requested=%u\n", hook_seq_read);
	seq_printf(m, "seq_read_hook_active=%u\n", seq_read_hook_active);
	seq_printf(m, "seq_read_hook_hits=%llu\n",
		   (unsigned long long)seq_read_hook_hits);
	seq_printf(m, "seq_read_hook_last_ret=%d\n", seq_read_hook_last_ret);
	seq_printf(m, "owner_proc_hidden=%u\n", owner_proc_hidden);
	seq_printf(m, "owner_proc_hidden_tgid=%d\n", owner_proc_hidden_tgid);
	seq_printf(m, "owner_proc_hide_hook_active=%u\n",
		   owner_proc_hide_hook_active);
	seq_printf(m, "owner_proc_hide_hook_hits=%llu\n",
		   (unsigned long long)owner_proc_hide_hook_hits);
	seq_printf(m, "owner_proc_hide_hook_last_ret=%d\n",
		   owner_proc_hide_hook_last_ret);
	seq_printf(m, "hwpoint_callback_total=%llu\n",
		   (unsigned long long)hwpoint_callback_total);
	seq_printf(m, "breakpoint_callback_total=%llu\n",
		   (unsigned long long)breakpoint_callback_total);
	seq_printf(m, "watchpoint_callback_total=%llu\n",
		   (unsigned long long)watchpoint_callback_total);
	seq_printf(m, "target_stop_event_read_total=%llu\n",
		   (unsigned long long)target_stop_event_read_total);
	seq_printf(m, "breakpoint_stop_event_read_total=%llu\n",
		   (unsigned long long)breakpoint_stop_event_read_total);
	seq_printf(m, "watchpoint_stop_event_read_total=%llu\n",
		   (unsigned long long)watchpoint_stop_event_read_total);
	seq_printf(m, "event_drop_total=%llu\n",
		   (unsigned long long)event_drop_total);
	seq_printf(m, "hwpoint_last_tgid=%d\n", hwpoint_last_tgid);
	seq_printf(m, "hwpoint_last_tid=%d\n", hwpoint_last_tid);
	seq_printf(m, "hwpoint_last_reason=%u\n", hwpoint_last_reason);
	seq_printf(m, "hwpoint_last_type=0x%x\n", hwpoint_last_type);
	seq_printf(m, "hwpoint_last_addr=0x%llx\n",
		   (unsigned long long)hwpoint_last_addr);
	seq_printf(m, "hwpoint_last_ip=0x%llx\n",
		   (unsigned long long)hwpoint_last_ip);
	seq_printf(m, "bypass_kprobe_blacklist=%u\n", bypass_kprobe_blacklist);
	seq_printf(m, "bypass_cfi=%u\n", bypass_cfi);

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(lkmdbg_status);

static int lkmdbg_hooks_show(struct seq_file *m, void *unused)
{
	return lkmdbg_hook_registry_debugfs_show(m);
}

DEFINE_SHOW_ATTRIBUTE(lkmdbg_hooks);

static int lkmdbg_debugfs_dentry_err(struct dentry *dentry)
{
	if (IS_ERR(dentry))
		return PTR_ERR(dentry);
	if (!dentry)
		return -ENOMEM;
	return 0;
}

int lkmdbg_debugfs_init(void)
{
	struct dentry *status_dentry;
	struct dentry *hooks_dentry;
	int ret;

	if (lkmdbg_state.debugfs_dir)
		return 0;

	lkmdbg_state.debugfs_dir = debugfs_create_dir(LKMDBG_DIR_NAME, NULL);
	if (IS_ERR_OR_NULL(lkmdbg_state.debugfs_dir))
		return lkmdbg_debugfs_dentry_err(lkmdbg_state.debugfs_dir);

	status_dentry = debugfs_create_file("status", 0444,
					    lkmdbg_state.debugfs_dir, NULL,
					    &lkmdbg_status_fops);
	ret = lkmdbg_debugfs_dentry_err(status_dentry);
	if (ret) {
		pr_err("lkmdbg: debugfs create status failed ret=%d\n", ret);
		debugfs_remove_recursive(lkmdbg_state.debugfs_dir);
		lkmdbg_state.debugfs_dir = NULL;
		return ret;
	}

	hooks_dentry = debugfs_create_file("hooks", 0444,
					   lkmdbg_state.debugfs_dir, NULL,
					   &lkmdbg_hooks_fops);
	ret = lkmdbg_debugfs_dentry_err(hooks_dentry);
	if (ret) {
		pr_err("lkmdbg: debugfs create hooks failed ret=%d\n", ret);
		debugfs_remove_recursive(lkmdbg_state.debugfs_dir);
		lkmdbg_state.debugfs_dir = NULL;
		return ret;
	}

	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.debugfs_active = true;
	mutex_unlock(&lkmdbg_state.lock);

	return 0;
}

void lkmdbg_debugfs_exit(void)
{
	debugfs_remove_recursive(lkmdbg_state.debugfs_dir);
	lkmdbg_state.debugfs_dir = NULL;
	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.debugfs_active = false;
	mutex_unlock(&lkmdbg_state.lock);
}

int lkmdbg_debugfs_set_visible(bool visible)
{
	if (visible)
		return lkmdbg_debugfs_init();

	lkmdbg_debugfs_exit();
	return 0;
}

bool lkmdbg_debugfs_is_active(void)
{
	return READ_ONCE(lkmdbg_state.debugfs_active);
}
