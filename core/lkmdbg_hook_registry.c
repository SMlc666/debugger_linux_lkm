#include <linux/kernel.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "lkmdbg_internal.h"

static LIST_HEAD(lkmdbg_hook_registry);
static DEFINE_MUTEX(lkmdbg_hook_registry_lock);
static u64 lkmdbg_hook_registry_next_id;

int lkmdbg_hook_registry_debugfs_show(struct seq_file *m)
{
	struct lkmdbg_hook_registry_entry *entry;

	mutex_lock(&lkmdbg_hook_registry_lock);
	list_for_each_entry(entry, &lkmdbg_hook_registry, node) {
		seq_printf(m,
			   "id=%llu name=%s active=%u hits=%llu last_ret=%d target=0x%llx origin=0x%llx replacement=0x%llx trampoline=0x%llx\n",
			   (unsigned long long)entry->hook_id, entry->name,
			   entry->active, (unsigned long long)entry->hits,
			   entry->last_ret,
			   (unsigned long long)entry->target,
			   (unsigned long long)entry->origin,
			   (unsigned long long)entry->replacement,
			   (unsigned long long)entry->trampoline);
	}
	mutex_unlock(&lkmdbg_hook_registry_lock);

	return 0;
}

struct lkmdbg_hook_registry_entry *
lkmdbg_hook_registry_register(const char *name, void *target, void *replacement)
{
	struct lkmdbg_hook_registry_entry *entry;

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return NULL;

	INIT_LIST_HEAD(&entry->node);
	strscpy(entry->name, name, sizeof(entry->name));
	entry->target = (u64)target;
	entry->replacement = (u64)replacement;
	entry->last_ret = 0;

	mutex_lock(&lkmdbg_hook_registry_lock);
	lkmdbg_hook_registry_next_id++;
	entry->hook_id = lkmdbg_hook_registry_next_id;
	list_add_tail(&entry->node, &lkmdbg_hook_registry);
	mutex_unlock(&lkmdbg_hook_registry_lock);

	return entry;
}

void lkmdbg_hook_registry_mark_installed(
	struct lkmdbg_hook_registry_entry *entry, void *origin,
	void *trampoline, int ret)
{
	if (!entry)
		return;

	mutex_lock(&lkmdbg_hook_registry_lock);
	entry->origin = (u64)origin;
	entry->trampoline = (u64)trampoline;
	entry->last_ret = ret;
	entry->active = ret == 0;
	mutex_unlock(&lkmdbg_hook_registry_lock);

	if (ret == 0)
		lkmdbg_session_broadcast_event(LKMDBG_EVENT_HOOK_INSTALLED,
					       entry->hook_id, 0);
}

void lkmdbg_hook_registry_note_hit(struct lkmdbg_hook_registry_entry *entry)
{
	u64 hits;

	if (!entry)
		return;

	mutex_lock(&lkmdbg_hook_registry_lock);
	entry->hits++;
	hits = entry->hits;
	mutex_unlock(&lkmdbg_hook_registry_lock);

	lkmdbg_session_broadcast_event(LKMDBG_EVENT_HOOK_HIT, entry->hook_id,
				       hits);
}

void lkmdbg_hook_registry_unregister(struct lkmdbg_hook_registry_entry *entry,
					 int ret)
{
	u64 hook_id;

	if (!entry)
		return;

	mutex_lock(&lkmdbg_hook_registry_lock);
	hook_id = entry->hook_id;
	entry->active = false;
	entry->last_ret = ret;
	list_del_init(&entry->node);
	mutex_unlock(&lkmdbg_hook_registry_lock);

	lkmdbg_session_broadcast_event(LKMDBG_EVENT_HOOK_REMOVED, hook_id, ret);
	kfree(entry);
}
