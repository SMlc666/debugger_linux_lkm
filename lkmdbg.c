#include <linux/debugfs.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/seq_file.h>

#define LKMDBG_DIR_NAME "lkmdbg"

static char *tag = "arm64-gki-smoke";
module_param(tag, charp, 0444);
MODULE_PARM_DESC(tag, "Human-readable tag shown in debugfs output");

static struct dentry *lkmdbg_dir;
static DEFINE_MUTEX(lkmdbg_lock);
static unsigned long load_jiffies;
static unsigned long status_reads;

static int lkmdbg_status_show(struct seq_file *m, void *unused)
{
	unsigned long reads;

	mutex_lock(&lkmdbg_lock);
	status_reads++;
	reads = status_reads;
	mutex_unlock(&lkmdbg_lock);

	seq_printf(m, "tag=%s\n", tag);
	seq_printf(m, "load_jiffies=%lu\n", load_jiffies);
	seq_printf(m, "status_reads=%lu\n", reads);

	return 0;
}

static int lkmdbg_status_open(struct inode *inode, struct file *file)
{
	return single_open(file, lkmdbg_status_show, inode->i_private);
}

static const struct file_operations lkmdbg_status_fops = {
	.owner = THIS_MODULE,
	.open = lkmdbg_status_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int __init lkmdbg_init(void)
{
	load_jiffies = jiffies;

	lkmdbg_dir = debugfs_create_dir(LKMDBG_DIR_NAME, NULL);
	if (IS_ERR_OR_NULL(lkmdbg_dir))
		return -ENOMEM;

	if (!debugfs_create_file("status", 0444, lkmdbg_dir, NULL,
				 &lkmdbg_status_fops)) {
		debugfs_remove_recursive(lkmdbg_dir);
		return -ENOMEM;
	}

	pr_info("lkmdbg: loaded tag=%s\n", tag);
	return 0;
}

static void __exit lkmdbg_exit(void)
{
	debugfs_remove_recursive(lkmdbg_dir);
	pr_info("lkmdbg: unloaded\n");
}

MODULE_AUTHOR("OpenAI Codex");
MODULE_DESCRIPTION("Minimal Linux kernel module scaffold for arm64 Android GKI debugger work");
MODULE_LICENSE("GPL");

module_init(lkmdbg_init);
module_exit(lkmdbg_exit);
