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
		 "Clone and swap /proc/version inode file_operations to add hidden ioctl support");

struct lkmdbg_state lkmdbg_state = {
	.lock = __MUTEX_INITIALIZER(lkmdbg_state.lock),
};

static int __init lkmdbg_init(void)
{
	int ret;

	lkmdbg_state.load_jiffies = jiffies;

	ret = lkmdbg_debugfs_init();
	if (ret)
		return ret;

	ret = lkmdbg_transport_init();
	if (ret) {
		lkmdbg_debugfs_exit();
		return ret;
	}

	pr_info("lkmdbg: loaded tag=%s hook_proc_version=%u\n", tag,
		hook_proc_version);
	return 0;
}

static void __exit lkmdbg_exit(void)
{
	lkmdbg_transport_exit();
	lkmdbg_debugfs_exit();
	pr_info("lkmdbg: unloaded\n");
}

MODULE_AUTHOR("OpenAI Codex");
MODULE_DESCRIPTION("Minimal Linux kernel module scaffold for arm64 Android GKI debugger work");
MODULE_LICENSE("GPL");

module_init(lkmdbg_init);
module_exit(lkmdbg_exit);
