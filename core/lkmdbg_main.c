#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>

#include "lkmdbg_internal.h"

char *tag = "arm64-gki-smoke";
module_param(tag, charp, 0444);
MODULE_PARM_DESC(tag, "Human-readable tag shown in debugfs output");

bool hook_proc_version;
module_param(hook_proc_version, bool, 0444);
MODULE_PARM_DESC(hook_proc_version,
		 "Install an inline hook on /proc/version open to graft the hidden session bootstrap onto matching file instances");

unsigned int hook_selftest_mode;
module_param(hook_selftest_mode, uint, 0644);
MODULE_PARM_DESC(hook_selftest_mode,
		 "0=off, 1=prepare only, 2=prepare exec pool only, 3=allocate exec trampoline only, 4=populate exec trampoline, 5=install only, 6=install and invoke a module-local arm64 inline hook");

bool hook_seq_read;
module_param(hook_seq_read, bool, 0444);
MODULE_PARM_DESC(hook_seq_read,
		 "Install a pass-through inline hook on seq_read and expose hit counters in debugfs");

bool enable_debugfs;
module_param(enable_debugfs, bool, 0644);
MODULE_PARM_DESC(enable_debugfs,
		 "Create the debugfs diagnostics surface under /sys/kernel/debug/lkmdbg");

bool bypass_kprobe_blacklist;
module_param(bypass_kprobe_blacklist, bool, 0644);
MODULE_PARM_DESC(bypass_kprobe_blacklist,
		 "Best-effort clear the kernel kprobe blacklist during module init");

bool bypass_cfi;
module_param(bypass_cfi, bool, 0644);
MODULE_PARM_DESC(bypass_cfi,
		 "Best-effort patch common CFI slowpath symbols during module init");

struct lkmdbg_state lkmdbg_state = {
	.lock = __MUTEX_INITIALIZER(lkmdbg_state.lock),
};

struct lkmdbg_symbols lkmdbg_symbols;
static struct lkmdbg_inline_hook *lkmdbg_selftest_hook;
static u64 (*lkmdbg_selftest_orig)(u64 value);

static noinline __aligned(PAGE_SIZE) u64 lkmdbg_selftest_target(u64 value)
{
	if (value & 1)
		value += 3;
	else
		value += 7;

	value ^= 0x1122334455667788ULL;
	value += 0x0102030405060708ULL;
	return value;
}

static u64 lkmdbg_selftest_model(u64 value)
{
	if (value & 1)
		value += 3;
	else
		value += 7;

	value ^= 0x1122334455667788ULL;
	value += 0x0102030405060708ULL;
	return value;
}

static noinline __aligned(PAGE_SIZE) u64 lkmdbg_selftest_replacement(u64 value)
{
	u64 original;

	if (!lkmdbg_selftest_orig)
		return 0xBAD0BAD0BAD0BAD0ULL;

	original = lkmdbg_selftest_orig(value + 1);
	return original ^ 0x00FF00FF00FF00FFULL;
}

static void lkmdbg_trace_stage(const char *stage)
{
	pr_emerg("lkmdbg: stage=%s hook_selftest_mode=%u hook_proc_version=%u bypass_kprobe_blacklist=%u bypass_cfi=%u\n",
		 stage, hook_selftest_mode, hook_proc_version,
		 bypass_kprobe_blacklist, bypass_cfi);
}

static int lkmdbg_run_hook_selftest(void)
{
	const u64 input = 0x41;
	const u64 mask = 0x00FF00FF00FF00FFULL;
	u64 baseline;
	u64 direct;
	u64 expected;
	u64 actual;
	void *orig_fn = NULL;
	int ret;

	baseline = lkmdbg_selftest_model(input + 1);
	direct = lkmdbg_selftest_target(input + 1);
	expected = baseline ^ mask;

	if (direct != baseline) {
		pr_err("lkmdbg: hook selftest baseline mismatch direct=0x%llx model=0x%llx\n",
		       (unsigned long long)direct,
		       (unsigned long long)baseline);
		return -EIO;
	}

	lkmdbg_trace_stage("hook_selftest_prepare_begin");
	ret = lkmdbg_hook_create(lkmdbg_selftest_target,
				 lkmdbg_selftest_replacement,
				 &lkmdbg_selftest_hook, &orig_fn);

	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.hook_selftest_enabled = true;
	lkmdbg_state.hook_selftest_exec_pool_ready = false;
	lkmdbg_state.hook_selftest_exec_allocated = false;
	lkmdbg_state.hook_selftest_exec_ready = false;
	lkmdbg_state.hook_selftest_installed = false;
	lkmdbg_state.hook_selftest_ret = ret;
	mutex_unlock(&lkmdbg_state.lock);

	if (ret)
		return ret;

	lkmdbg_selftest_orig = orig_fn;

	if (hook_selftest_mode < 2) {
		lkmdbg_trace_stage("hook_selftest_prepare_done");
		return 0;
	}

	lkmdbg_trace_stage("hook_selftest_exec_pool_begin");
	ret = lkmdbg_hooks_prepare_exec_pool();
	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.hook_selftest_ret = ret;
	mutex_unlock(&lkmdbg_state.lock);
	if (ret)
		return ret;

	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.hook_selftest_exec_pool_ready = true;
	mutex_unlock(&lkmdbg_state.lock);

	if (hook_selftest_mode < 3) {
		lkmdbg_trace_stage("hook_selftest_exec_pool_done");
		return 0;
	}

	lkmdbg_trace_stage("hook_selftest_exec_alloc_begin");
	ret = lkmdbg_hook_alloc_exec(lkmdbg_selftest_hook);
	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.hook_selftest_ret = ret;
	mutex_unlock(&lkmdbg_state.lock);
	if (ret)
		return ret;

	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.hook_selftest_exec_allocated = true;
	mutex_unlock(&lkmdbg_state.lock);

	if (hook_selftest_mode < 4) {
		lkmdbg_trace_stage("hook_selftest_exec_alloc_done");
		return 0;
	}

	lkmdbg_trace_stage("hook_selftest_exec_prepare_begin");
	ret = lkmdbg_hook_prepare_exec(lkmdbg_selftest_hook, &orig_fn);
	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.hook_selftest_ret = ret;
	mutex_unlock(&lkmdbg_state.lock);
	if (ret)
		return ret;

	lkmdbg_selftest_orig = orig_fn;

	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.hook_selftest_exec_ready = true;
	mutex_unlock(&lkmdbg_state.lock);

	if (hook_selftest_mode < 5) {
		lkmdbg_trace_stage("hook_selftest_exec_prepare_done");
		return 0;
	}

	lkmdbg_trace_stage("hook_selftest_install_begin");
	ret = lkmdbg_hook_patch_target(lkmdbg_selftest_hook, &orig_fn);
	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.hook_selftest_ret = ret;
	mutex_unlock(&lkmdbg_state.lock);
	if (ret)
		return ret;

	lkmdbg_selftest_orig = orig_fn;

	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.hook_selftest_installed = true;
	mutex_unlock(&lkmdbg_state.lock);

	if (hook_selftest_mode < 6) {
		lkmdbg_trace_stage("hook_selftest_install_done");
		return 0;
	}

	lkmdbg_trace_stage("hook_selftest_invoke_begin");
	actual = lkmdbg_selftest_target(input);
	expected = lkmdbg_selftest_model(input + 1) ^ mask;

	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.hook_selftest_expected = expected;
	lkmdbg_state.hook_selftest_actual = actual;
	mutex_unlock(&lkmdbg_state.lock);

	pr_info("lkmdbg: hook selftest expected=0x%llx actual=0x%llx orig=%px target=%px replacement=%px\n",
		(unsigned long long)expected,
		(unsigned long long)actual,
		lkmdbg_selftest_orig,
		lkmdbg_selftest_target,
		lkmdbg_selftest_replacement);

	if (actual != expected)
		return -EIO;

	return 0;
}

static int __init lkmdbg_init(void)
{
	int cfi_patched;
	int blacklist_patched;
	int ret;

	lkmdbg_trace_stage("init_begin");
	lkmdbg_state.load_jiffies = jiffies;

	ret = lkmdbg_debugfs_set_visible(enable_debugfs);
	if (ret)
		return ret;
	lkmdbg_trace_stage("debugfs_ready");

	ret = lkmdbg_symbols_init();
	if (ret) {
		lkmdbg_debugfs_exit();
		return ret;
	}
	lkmdbg_trace_stage("symbols_ready");

	ret = lkmdbg_stealth_init();
	if (ret) {
		lkmdbg_symbols_exit();
		lkmdbg_debugfs_exit();
		return ret;
	}

	ret = lkmdbg_hooks_init();
	if (ret) {
		lkmdbg_stealth_exit();
		lkmdbg_symbols_exit();
		lkmdbg_debugfs_exit();
		return ret;
	}
	lkmdbg_trace_stage("hooks_ready");

	blacklist_patched = 0;
	cfi_patched = 0;
	if (bypass_kprobe_blacklist) {
		lkmdbg_trace_stage("bypass_kprobe_begin");
		blacklist_patched = lkmdbg_disable_kprobe_blacklist();
		lkmdbg_trace_stage("bypass_kprobe_done");
	}
	if (bypass_cfi) {
		lkmdbg_trace_stage("bypass_cfi_begin");
		cfi_patched = lkmdbg_cfi_bypass();
		lkmdbg_trace_stage("bypass_cfi_done");
	}

	if (hook_selftest_mode) {
		ret = lkmdbg_run_hook_selftest();
		if (ret) {
			pr_err("lkmdbg: hook selftest failed ret=%d\n", ret);
			lkmdbg_hooks_exit();
			lkmdbg_stealth_exit();
			lkmdbg_symbols_exit();
			lkmdbg_debugfs_exit();
			return ret;
		}
	}

	ret = lkmdbg_transport_init();
	if (ret) {
		lkmdbg_hooks_exit();
		lkmdbg_stealth_exit();
		lkmdbg_symbols_exit();
		lkmdbg_debugfs_exit();
		return ret;
	}
	lkmdbg_trace_stage("transport_ready");

	ret = lkmdbg_runtime_hooks_init();
	if (ret) {
		lkmdbg_transport_exit();
		lkmdbg_hooks_exit();
		lkmdbg_stealth_exit();
		lkmdbg_symbols_exit();
		lkmdbg_debugfs_exit();
		return ret;
	}
	lkmdbg_trace_stage("runtime_hooks_ready");

	ret = lkmdbg_thread_ctrl_init();
	if (ret) {
		lkmdbg_runtime_hooks_exit();
		lkmdbg_transport_exit();
		lkmdbg_hooks_exit();
		lkmdbg_stealth_exit();
		lkmdbg_symbols_exit();
		lkmdbg_debugfs_exit();
		return ret;
	}
	lkmdbg_trace_stage("thread_ctrl_ready");

	pr_info("lkmdbg: loaded tag=%s hook_proc_version=%u hook_selftest_mode=%u hook_seq_read=%u kprobe_patched=%d cfi_patched=%d\n",
		tag, hook_proc_version, hook_selftest_mode, hook_seq_read,
		blacklist_patched, cfi_patched);
	return 0;
}

static void __exit lkmdbg_exit(void)
{
	lkmdbg_stealth_exit();
	lkmdbg_thread_ctrl_exit();
	lkmdbg_runtime_hooks_exit();
	lkmdbg_transport_exit();
	lkmdbg_selftest_hook = NULL;
	lkmdbg_selftest_orig = NULL;
	lkmdbg_hooks_exit();
	lkmdbg_symbols_exit();
	lkmdbg_debugfs_exit();
	pr_info("lkmdbg: unloaded\n");
}

MODULE_AUTHOR("OpenAI Codex");
MODULE_DESCRIPTION("Minimal Linux kernel module scaffold for arm64 Android GKI debugger work");
MODULE_LICENSE("GPL");

module_init(lkmdbg_init);
module_exit(lkmdbg_exit);
