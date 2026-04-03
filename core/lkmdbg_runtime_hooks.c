#include <linux/atomic.h>
#include <linux/dcache.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/namei.h>
#include <linux/sched/signal.h>
#include <linux/types.h>
#include <linux/version.h>

#include "lkmdbg_internal.h"

static struct lkmdbg_inline_hook *lkmdbg_seq_read_hook;
static struct lkmdbg_hook_registry_entry *lkmdbg_seq_read_registry;
static ssize_t (*lkmdbg_seq_read_orig)(struct file *file, char __user *buf,
				       size_t count, loff_t *ppos);
static atomic_t lkmdbg_seq_read_inflight = ATOMIC_INIT(0);
static DECLARE_WAIT_QUEUE_HEAD(lkmdbg_seq_read_waitq);

static struct lkmdbg_inline_hook *lkmdbg_proc_pid_lookup_hook;
static struct lkmdbg_hook_registry_entry *lkmdbg_proc_pid_lookup_registry;
static struct dentry *(*lkmdbg_proc_pid_lookup_orig)(struct dentry *dentry,
						      unsigned int flags);
static struct lkmdbg_inline_hook *lkmdbg_proc_pid_readdir_hook;
static struct lkmdbg_hook_registry_entry *lkmdbg_proc_pid_readdir_registry;
static int (*lkmdbg_proc_pid_readdir_orig)(struct file *file,
					   struct dir_context *ctx);
static struct lkmdbg_inline_hook *lkmdbg_proc_tgid_base_lookup_hook;
static struct lkmdbg_hook_registry_entry *lkmdbg_proc_tgid_base_lookup_registry;
static struct dentry *(*lkmdbg_proc_tgid_base_lookup_orig)(struct inode *dir,
							   struct dentry *dentry,
							   unsigned int flags);
static struct task_struct *(*lkmdbg_get_proc_task_orig)(const struct inode *inode);
static atomic_t lkmdbg_owner_proc_hide_inflight = ATOMIC_INIT(0);
static DECLARE_WAIT_QUEUE_HEAD(lkmdbg_owner_proc_hide_waitq);
static pid_t lkmdbg_owner_proc_hidden_tgid;
static bool lkmdbg_owner_proc_hidden_enabled;

struct lkmdbg_ownerhide_dir_context {
	struct dir_context ctx;
	struct dir_context *orig_ctx;
};

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 0, 0)
#define LKMDBG_FILLDIR_RET bool
#else
#define LKMDBG_FILLDIR_RET int
#endif

static void *lkmdbg_lookup_runtime_symbol_variants(const char *name)
{
	char prefixed[96];
	const char *candidates[2] = { name, NULL };
	unsigned long addr;
	int ret;
	size_t i;

	if (!name || !name[0])
		return NULL;

	ret = scnprintf(prefixed, sizeof(prefixed), "__pfx_%s", name);
	if (ret > 0 && ret < sizeof(prefixed))
		candidates[1] = prefixed;

	for (i = 0; i < ARRAY_SIZE(candidates); i++) {
		if (!candidates[i])
			continue;
		addr = lkmdbg_lookup_runtime_symbol_any(candidates[i]);
		if (addr)
			return (void *)addr;
	}
	return NULL;
}

static void *lkmdbg_lookup_proc_pid_lookup(void)
{
	return lkmdbg_lookup_runtime_symbol_variants("proc_pid_lookup");
}

static void *lkmdbg_lookup_proc_pid_readdir(void)
{
	return lkmdbg_lookup_runtime_symbol_variants("proc_pid_readdir");
}

static void *lkmdbg_lookup_proc_tgid_base_lookup(void)
{
	return lkmdbg_lookup_runtime_symbol_variants("proc_tgid_base_lookup");
}

static void *lkmdbg_lookup_get_proc_task(void)
{
	return lkmdbg_lookup_runtime_symbol_variants("get_proc_task");
}

static bool lkmdbg_parse_proc_pid_name(const char *name, int len, pid_t *tgid)
{
	unsigned int value = 0;
	int i;

	if (!name || !tgid || len <= 0 || len > 10)
		return false;

	for (i = 0; i < len; i++) {
		if (name[i] < '0' || name[i] > '9')
			return false;
		value = value * 10 + (unsigned int)(name[i] - '0');
	}

	if (!value)
		return false;

	*tgid = (pid_t)value;
	return true;
}

static bool lkmdbg_should_hide_owner_proc(pid_t tgid)
{
	pid_t hidden_tgid;
	bool hidden_enabled;

	if (tgid <= 0)
		return false;

	hidden_enabled = READ_ONCE(lkmdbg_owner_proc_hidden_enabled);
	hidden_tgid = READ_ONCE(lkmdbg_owner_proc_hidden_tgid);
	if (!hidden_enabled || hidden_tgid <= 0 || tgid != hidden_tgid)
		return false;

	return true;
}

static bool lkmdbg_parse_proc_parent_tgid(struct dentry *dentry, pid_t *tgid)
{
	struct dentry *parent;

	if (!dentry || !tgid)
		return false;

	parent = READ_ONCE(dentry->d_parent);
	if (!parent || parent == dentry)
		return false;

	return lkmdbg_parse_proc_pid_name(parent->d_name.name,
					  parent->d_name.len, tgid);
}

static void lkmdbg_owner_proc_d_drop(struct dentry *dentry)
{
	if (!dentry)
		return;
	lkmdbg_d_drop_runtime(dentry);
}

static void lkmdbg_owner_proc_drop_cached_path(const char *path_str)
{
	struct path path;

	if (!path_str || !path_str[0])
		return;

	if (!lkmdbg_kern_path_runtime(path_str, LOOKUP_FOLLOW, &path)) {
		lkmdbg_owner_proc_d_drop(path.dentry);
		lkmdbg_path_put_runtime(&path);
	}
}

static void lkmdbg_owner_proc_drop_cached_pid_dentry(pid_t tgid)
{
	char proc_path[48];
	int n;

	if (tgid <= 0)
		return;

	n = scnprintf(proc_path, sizeof(proc_path), "/proc/%d", tgid);
	if (n <= 0 || n >= sizeof(proc_path))
		return;

	lkmdbg_owner_proc_drop_cached_path(proc_path);

	n = scnprintf(proc_path, sizeof(proc_path), "/proc/%d/status", tgid);
	if (n > 0 && n < sizeof(proc_path))
		lkmdbg_owner_proc_drop_cached_path(proc_path);

	n = scnprintf(proc_path, sizeof(proc_path), "/proc/%d/comm", tgid);
	if (n > 0 && n < sizeof(proc_path))
		lkmdbg_owner_proc_drop_cached_path(proc_path);

	n = scnprintf(proc_path, sizeof(proc_path), "/proc/%d/cmdline", tgid);
	if (n > 0 && n < sizeof(proc_path))
		lkmdbg_owner_proc_drop_cached_path(proc_path);
}

static ssize_t __nocfi
lkmdbg_seq_read_replacement(struct file *file, char __user *buf, size_t count,
			    loff_t *ppos)
{
	struct lkmdbg_hook_registry_entry *registry;
	ssize_t (*orig)(struct file *file, char __user *buf, size_t count,
			loff_t *ppos);
	ssize_t ret;

	atomic_inc(&lkmdbg_seq_read_inflight);

	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.seq_read_hook_hits++;
	mutex_unlock(&lkmdbg_state.lock);

	registry = READ_ONCE(lkmdbg_seq_read_registry);
	if (registry)
		lkmdbg_hook_registry_note_hit(registry);

	orig = READ_ONCE(lkmdbg_seq_read_orig);
	if (!orig)
		ret = -ENOENT;
	else
		ret = orig(file, buf, count, ppos);

	if (atomic_dec_and_test(&lkmdbg_seq_read_inflight))
		wake_up_all(&lkmdbg_seq_read_waitq);

	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.seq_read_hook_last_ret = (int)ret;
	mutex_unlock(&lkmdbg_state.lock);

	return ret;
}

static int lkmdbg_install_seq_read_hook(void)
{
	void *target;
	void *orig_fn = NULL;
	int ret;

	if (!lkmdbg_symbols.kallsyms_lookup_name)
		return -ENOENT;

	target = (void *)lkmdbg_kallsyms_lookup_name_runtime("seq_read");
	if (!target)
		return -ENOENT;

	lkmdbg_seq_read_registry = lkmdbg_hook_registry_register("seq_read",
							target,
							lkmdbg_seq_read_replacement);
	if (!lkmdbg_seq_read_registry)
		return -ENOMEM;

	ret = lkmdbg_hook_install(target, lkmdbg_seq_read_replacement,
				  &lkmdbg_seq_read_hook, &orig_fn);
	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.seq_read_hook_last_ret = ret;
	mutex_unlock(&lkmdbg_state.lock);
	if (ret) {
		lkmdbg_hook_registry_unregister(lkmdbg_seq_read_registry, ret);
		lkmdbg_seq_read_registry = NULL;
		return ret;
	}

	lkmdbg_seq_read_orig = orig_fn;
	lkmdbg_hook_registry_mark_installed(lkmdbg_seq_read_registry, target,
						orig_fn, 0);

	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.seq_read_hook_active = true;
	mutex_unlock(&lkmdbg_state.lock);

	lkmdbg_pr_info("lkmdbg: runtime hook active target=seq_read origin=%px trampoline=%px\n",
		target, orig_fn);
	return 0;
}

static struct dentry *__nocfi
lkmdbg_proc_pid_lookup_replacement(struct dentry *dentry, unsigned int flags)
{
	struct lkmdbg_hook_registry_entry *registry;
	struct dentry *(*orig)(struct dentry *dentry, unsigned int flags);
	struct dentry *ret;
	pid_t tgid;

	atomic_inc(&lkmdbg_owner_proc_hide_inflight);

	registry = READ_ONCE(lkmdbg_proc_pid_lookup_registry);
	if (registry)
		lkmdbg_hook_registry_note_hit(registry);

	orig = READ_ONCE(lkmdbg_proc_pid_lookup_orig);
	if (!orig) {
		ret = ERR_PTR(-ENOENT);
		goto out;
	}

	if (dentry &&
	    lkmdbg_parse_proc_pid_name(dentry->d_name.name, dentry->d_name.len,
				       &tgid) &&
	    lkmdbg_should_hide_owner_proc(tgid)) {
		lkmdbg_owner_proc_d_drop(dentry);
		ret = ERR_PTR(-ENOENT);
		goto out;
	}

	ret = orig(dentry, flags);

out:
	if (atomic_dec_and_test(&lkmdbg_owner_proc_hide_inflight))
		wake_up_all(&lkmdbg_owner_proc_hide_waitq);

	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.owner_proc_hide_hook_hits++;
	lkmdbg_state.owner_proc_hide_hook_last_ret = IS_ERR(ret) ?
		PTR_ERR(ret) : 0;
	mutex_unlock(&lkmdbg_state.lock);

	return ret;
}

static LKMDBG_FILLDIR_RET
lkmdbg_proc_pid_readdir_actor(struct dir_context *ctx, const char *name,
			      int namelen, loff_t offset, u64 ino,
			      unsigned int d_type)
{
	struct lkmdbg_ownerhide_dir_context *wrapped =
		container_of(ctx, struct lkmdbg_ownerhide_dir_context, ctx);
	pid_t tgid;

	if (lkmdbg_parse_proc_pid_name(name, namelen, &tgid) &&
	    lkmdbg_should_hide_owner_proc(tgid))
		return 0;

	return wrapped->orig_ctx->actor(wrapped->orig_ctx, name, namelen, offset,
					ino, d_type);
}

static int __nocfi lkmdbg_proc_pid_readdir_replacement(
	struct file *file, struct dir_context *ctx)
{
	struct lkmdbg_hook_registry_entry *registry;
	int (*orig)(struct file *file, struct dir_context *ctx);
	struct lkmdbg_ownerhide_dir_context wrapped;
	int ret;

	atomic_inc(&lkmdbg_owner_proc_hide_inflight);

	registry = READ_ONCE(lkmdbg_proc_pid_readdir_registry);
	if (registry)
		lkmdbg_hook_registry_note_hit(registry);

	orig = READ_ONCE(lkmdbg_proc_pid_readdir_orig);
	if (!orig) {
		ret = -ENOENT;
		goto out;
	}

	if (!ctx || !ctx->actor) {
		ret = -EINVAL;
		goto out;
	}

	wrapped.ctx.actor = lkmdbg_proc_pid_readdir_actor;
	wrapped.ctx.pos = ctx->pos;
	wrapped.orig_ctx = ctx;
	ret = orig(file, &wrapped.ctx);
	ctx->pos = wrapped.ctx.pos;

out:
	if (atomic_dec_and_test(&lkmdbg_owner_proc_hide_inflight))
		wake_up_all(&lkmdbg_owner_proc_hide_waitq);

	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.owner_proc_hide_hook_hits++;
	lkmdbg_state.owner_proc_hide_hook_last_ret = ret;
	mutex_unlock(&lkmdbg_state.lock);

	return ret;
}

static struct dentry *__nocfi lkmdbg_proc_tgid_base_lookup_replacement(
	struct inode *dir, struct dentry *dentry, unsigned int flags)
{
	struct lkmdbg_hook_registry_entry *registry;
	struct dentry *(*orig)(struct inode *dir, struct dentry *dentry,
			       unsigned int flags);
	struct task_struct *task = NULL;
	struct dentry *ret;
	pid_t tgid;

	atomic_inc(&lkmdbg_owner_proc_hide_inflight);

	registry = READ_ONCE(lkmdbg_proc_tgid_base_lookup_registry);
	if (registry)
		lkmdbg_hook_registry_note_hit(registry);

	orig = READ_ONCE(lkmdbg_proc_tgid_base_lookup_orig);
	if (!orig) {
		ret = ERR_PTR(-ENOENT);
		goto out;
	}

	if (dir && lkmdbg_get_proc_task_orig)
		task = lkmdbg_get_proc_task_orig(dir);
	if (task && lkmdbg_should_hide_owner_proc(task_tgid_nr(task))) {
		lkmdbg_owner_proc_d_drop(dentry);
		ret = ERR_PTR(-ENOENT);
		goto out_put_task;
	}

	if (lkmdbg_parse_proc_parent_tgid(dentry, &tgid) &&
	    lkmdbg_should_hide_owner_proc(tgid)) {
		lkmdbg_owner_proc_d_drop(dentry);
		ret = ERR_PTR(-ENOENT);
		goto out_put_task;
	}

	ret = orig(dir, dentry, flags);
out_put_task:
	if (task)
		put_task_struct(task);
out:
	if (atomic_dec_and_test(&lkmdbg_owner_proc_hide_inflight))
		wake_up_all(&lkmdbg_owner_proc_hide_waitq);

	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.owner_proc_hide_hook_hits++;
	lkmdbg_state.owner_proc_hide_hook_last_ret = IS_ERR(ret) ?
		PTR_ERR(ret) : 0;
	mutex_unlock(&lkmdbg_state.lock);

	return ret;
}

static int lkmdbg_install_owner_proc_hide_hook(void)
{
	void *target;
	void *readdir_target;
	void *tgid_base_lookup_target;
	void *get_proc_task_target;
	void *orig_fn = NULL;
	void *readdir_orig_fn = NULL;
	void *tgid_base_lookup_orig_fn = NULL;
	int ret;

	if (lkmdbg_proc_pid_lookup_hook &&
	    lkmdbg_proc_pid_readdir_hook &&
	    lkmdbg_proc_tgid_base_lookup_hook)
		return 0;

	target = lkmdbg_lookup_proc_pid_lookup();
	readdir_target = lkmdbg_lookup_proc_pid_readdir();
	tgid_base_lookup_target = lkmdbg_lookup_proc_tgid_base_lookup();
	get_proc_task_target = lkmdbg_lookup_get_proc_task();
	if (!target || !readdir_target || !tgid_base_lookup_target)
		return -EOPNOTSUPP;

	lkmdbg_get_proc_task_orig =
		(struct task_struct *(*)(const struct inode *))get_proc_task_target;

	lkmdbg_proc_pid_lookup_registry = lkmdbg_hook_registry_register(
		"proc_pid_lookup", target, lkmdbg_proc_pid_lookup_replacement);
	if (!lkmdbg_proc_pid_lookup_registry)
		return -ENOMEM;

	ret = lkmdbg_hook_install(target, lkmdbg_proc_pid_lookup_replacement,
				  &lkmdbg_proc_pid_lookup_hook, &orig_fn);
	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.owner_proc_hide_hook_last_ret = ret;
	mutex_unlock(&lkmdbg_state.lock);
	if (ret) {
		lkmdbg_hook_registry_unregister(lkmdbg_proc_pid_lookup_registry,
						ret);
		lkmdbg_proc_pid_lookup_registry = NULL;
		return ret;
	}

	lkmdbg_proc_pid_lookup_orig = orig_fn;
	lkmdbg_hook_registry_mark_installed(lkmdbg_proc_pid_lookup_registry,
					    target, orig_fn, 0);

	lkmdbg_proc_pid_readdir_registry = lkmdbg_hook_registry_register(
		"proc_pid_readdir", readdir_target,
		lkmdbg_proc_pid_readdir_replacement);
	if (!lkmdbg_proc_pid_readdir_registry) {
		ret = -ENOMEM;
		goto rollback_lookup;
	}

	ret = lkmdbg_hook_install(readdir_target,
				  lkmdbg_proc_pid_readdir_replacement,
				  &lkmdbg_proc_pid_readdir_hook,
				  &readdir_orig_fn);
	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.owner_proc_hide_hook_last_ret = ret;
	mutex_unlock(&lkmdbg_state.lock);
	if (ret)
		goto rollback_readdir;

	lkmdbg_proc_pid_readdir_orig = readdir_orig_fn;
	lkmdbg_hook_registry_mark_installed(lkmdbg_proc_pid_readdir_registry,
					    readdir_target, readdir_orig_fn, 0);

	lkmdbg_proc_tgid_base_lookup_registry = lkmdbg_hook_registry_register(
		"proc_tgid_base_lookup", tgid_base_lookup_target,
		lkmdbg_proc_tgid_base_lookup_replacement);
	if (!lkmdbg_proc_tgid_base_lookup_registry) {
		ret = -ENOMEM;
		goto rollback_readdir;
	}

	ret = lkmdbg_hook_install(tgid_base_lookup_target,
				  lkmdbg_proc_tgid_base_lookup_replacement,
				  &lkmdbg_proc_tgid_base_lookup_hook,
				  &tgid_base_lookup_orig_fn);
	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.owner_proc_hide_hook_last_ret = ret;
	mutex_unlock(&lkmdbg_state.lock);
	if (ret)
		goto rollback_tgid_base_lookup_registry;

	lkmdbg_proc_tgid_base_lookup_orig = tgid_base_lookup_orig_fn;
	lkmdbg_hook_registry_mark_installed(
		lkmdbg_proc_tgid_base_lookup_registry, tgid_base_lookup_target,
		tgid_base_lookup_orig_fn, 0);

	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.owner_proc_hide_hook_active = true;
	mutex_unlock(&lkmdbg_state.lock);

	lkmdbg_pr_info("lkmdbg: runtime hook active target=proc_pid_lookup origin=%px trampoline=%px\n",
		       target, orig_fn);
	lkmdbg_pr_info("lkmdbg: runtime hook active target=proc_pid_readdir origin=%px trampoline=%px\n",
		       readdir_target, readdir_orig_fn);
	lkmdbg_pr_info("lkmdbg: runtime hook active target=proc_tgid_base_lookup origin=%px trampoline=%px\n",
		       tgid_base_lookup_target, tgid_base_lookup_orig_fn);
	return 0;

rollback_tgid_base_lookup_registry:
	lkmdbg_hook_registry_unregister(lkmdbg_proc_tgid_base_lookup_registry,
					ret);
	lkmdbg_proc_tgid_base_lookup_registry = NULL;

rollback_readdir:
	if (lkmdbg_proc_pid_readdir_hook) {
		lkmdbg_hook_destroy(lkmdbg_proc_pid_readdir_hook);
		lkmdbg_proc_pid_readdir_hook = NULL;
	}
	if (lkmdbg_proc_pid_readdir_registry) {
		lkmdbg_hook_registry_unregister(lkmdbg_proc_pid_readdir_registry,
						ret);
		lkmdbg_proc_pid_readdir_registry = NULL;
	}
	lkmdbg_proc_pid_readdir_orig = NULL;

rollback_lookup:
	if (lkmdbg_proc_pid_lookup_hook) {
		lkmdbg_hook_destroy(lkmdbg_proc_pid_lookup_hook);
		lkmdbg_proc_pid_lookup_hook = NULL;
	}
	if (lkmdbg_proc_pid_lookup_registry) {
		lkmdbg_hook_registry_unregister(lkmdbg_proc_pid_lookup_registry,
						ret);
		lkmdbg_proc_pid_lookup_registry = NULL;
	}
	lkmdbg_proc_pid_lookup_orig = NULL;
	lkmdbg_proc_tgid_base_lookup_orig = NULL;
	lkmdbg_get_proc_task_orig = NULL;
	return ret;
}

bool lkmdbg_runtime_hook_owner_proc_hide_supported(void)
{
	return lkmdbg_lookup_proc_pid_lookup() != NULL &&
	       lkmdbg_lookup_proc_pid_readdir() != NULL &&
	       lkmdbg_lookup_proc_tgid_base_lookup() != NULL;
}

int lkmdbg_runtime_hook_set_owner_proc_hidden(pid_t owner_tgid, bool hidden)
{
	int ret;

	if (!hidden) {
		WRITE_ONCE(lkmdbg_owner_proc_hidden_enabled, false);
		WRITE_ONCE(lkmdbg_owner_proc_hidden_tgid, 0);
		mutex_lock(&lkmdbg_state.lock);
		lkmdbg_state.owner_proc_hidden = false;
		lkmdbg_state.owner_proc_hidden_tgid = 0;
		mutex_unlock(&lkmdbg_state.lock);
		return 0;
	}

	if (owner_tgid <= 0)
		return -EINVAL;

	ret = lkmdbg_install_owner_proc_hide_hook();
	if (ret)
		return ret;

	lkmdbg_owner_proc_drop_cached_pid_dentry(owner_tgid);
	WRITE_ONCE(lkmdbg_owner_proc_hidden_tgid, owner_tgid);
	WRITE_ONCE(lkmdbg_owner_proc_hidden_enabled, true);

	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.owner_proc_hidden = true;
	lkmdbg_state.owner_proc_hidden_tgid = owner_tgid;
	lkmdbg_state.owner_proc_hide_hook_last_ret = 0;
	mutex_unlock(&lkmdbg_state.lock);
	return 0;
}

int lkmdbg_runtime_hooks_init(void)
{
	int ret = 0;

	atomic_set(&lkmdbg_seq_read_inflight, 0);
	atomic_set(&lkmdbg_owner_proc_hide_inflight, 0);
	WRITE_ONCE(lkmdbg_owner_proc_hidden_tgid, 0);
	WRITE_ONCE(lkmdbg_owner_proc_hidden_enabled, false);
	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.seq_read_hook_active = false;
	lkmdbg_state.seq_read_hook_hits = 0;
	lkmdbg_state.seq_read_hook_last_ret = 0;
	lkmdbg_state.owner_proc_hidden = false;
	lkmdbg_state.owner_proc_hidden_tgid = 0;
	lkmdbg_state.owner_proc_hide_hook_active = false;
	lkmdbg_state.owner_proc_hide_hook_hits = 0;
	lkmdbg_state.owner_proc_hide_hook_last_ret = 0;
	mutex_unlock(&lkmdbg_state.lock);

	if (!hook_seq_read)
		return 0;

	ret = lkmdbg_install_seq_read_hook();
	if (ret)
		lkmdbg_pr_err("lkmdbg: runtime seq_read hook install failed ret=%d\n", ret);

	return ret;
}

void lkmdbg_runtime_hooks_exit(void)
{
	long remaining = 1;

	WRITE_ONCE(lkmdbg_owner_proc_hidden_enabled, false);
	WRITE_ONCE(lkmdbg_owner_proc_hidden_tgid, 0);
	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.owner_proc_hidden = false;
	lkmdbg_state.owner_proc_hidden_tgid = 0;
	mutex_unlock(&lkmdbg_state.lock);

	if (lkmdbg_proc_pid_lookup_hook &&
	    !lkmdbg_hook_deactivate(lkmdbg_proc_pid_lookup_hook))
		remaining = 0;
	if (lkmdbg_proc_pid_readdir_hook &&
	    !lkmdbg_hook_deactivate(lkmdbg_proc_pid_readdir_hook))
		remaining = 0;
	if (lkmdbg_proc_tgid_base_lookup_hook &&
	    !lkmdbg_hook_deactivate(lkmdbg_proc_tgid_base_lookup_hook))
		remaining = 0;

	if (!remaining) {
		long drained;

		drained = wait_event_timeout(
			lkmdbg_owner_proc_hide_waitq,
			atomic_read(&lkmdbg_owner_proc_hide_inflight) == 0,
			msecs_to_jiffies(1000));
		if (!drained)
			lkmdbg_pr_warn("lkmdbg: owner proc hide hook drain timed out inflight=%d\n",
				atomic_read(&lkmdbg_owner_proc_hide_inflight));
	}

	if (lkmdbg_proc_pid_lookup_hook) {
		lkmdbg_hook_destroy(lkmdbg_proc_pid_lookup_hook);
		lkmdbg_proc_pid_lookup_hook = NULL;
	}
	if (lkmdbg_proc_pid_readdir_hook) {
		lkmdbg_hook_destroy(lkmdbg_proc_pid_readdir_hook);
		lkmdbg_proc_pid_readdir_hook = NULL;
	}
	if (lkmdbg_proc_tgid_base_lookup_hook) {
		lkmdbg_hook_destroy(lkmdbg_proc_tgid_base_lookup_hook);
		lkmdbg_proc_tgid_base_lookup_hook = NULL;
	}
	if (lkmdbg_proc_pid_lookup_registry) {
		lkmdbg_hook_registry_unregister(lkmdbg_proc_pid_lookup_registry,
						0);
		lkmdbg_proc_pid_lookup_registry = NULL;
	}
	if (lkmdbg_proc_pid_readdir_registry) {
		lkmdbg_hook_registry_unregister(lkmdbg_proc_pid_readdir_registry,
						0);
		lkmdbg_proc_pid_readdir_registry = NULL;
	}
	if (lkmdbg_proc_tgid_base_lookup_registry) {
		lkmdbg_hook_registry_unregister(
			lkmdbg_proc_tgid_base_lookup_registry, 0);
		lkmdbg_proc_tgid_base_lookup_registry = NULL;
	}
	lkmdbg_proc_pid_lookup_orig = NULL;
	lkmdbg_proc_pid_readdir_orig = NULL;
	lkmdbg_proc_tgid_base_lookup_orig = NULL;
	lkmdbg_get_proc_task_orig = NULL;

	if (lkmdbg_seq_read_hook) {
		if (!lkmdbg_hook_deactivate(lkmdbg_seq_read_hook)) {
			remaining = wait_event_timeout(
				lkmdbg_seq_read_waitq,
				atomic_read(&lkmdbg_seq_read_inflight) == 0,
				msecs_to_jiffies(1000));
			if (!remaining)
				lkmdbg_pr_warn("lkmdbg: seq_read hook drain timed out inflight=%d\n",
					atomic_read(&lkmdbg_seq_read_inflight));
		}

		lkmdbg_hook_destroy(lkmdbg_seq_read_hook);
		lkmdbg_seq_read_hook = NULL;
	}
	if (lkmdbg_seq_read_registry) {
		lkmdbg_hook_registry_unregister(lkmdbg_seq_read_registry, 0);
		lkmdbg_seq_read_registry = NULL;
	}

	lkmdbg_seq_read_orig = NULL;

	mutex_lock(&lkmdbg_state.lock);
	lkmdbg_state.seq_read_hook_active = false;
	lkmdbg_state.owner_proc_hide_hook_active = false;
	mutex_unlock(&lkmdbg_state.lock);
}
