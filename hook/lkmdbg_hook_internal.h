#ifndef _LKMDBG_HOOK_INTERNAL_H
#define _LKMDBG_HOOK_INTERNAL_H

struct lkmdbg_inline_hook;

int lkmdbg_hooks_init(void);
void lkmdbg_hooks_exit(void);
int lkmdbg_hooks_prepare_exec_pool(void);

int lkmdbg_hook_create(void *target, void *replacement,
		       struct lkmdbg_inline_hook **hook_out,
		       void **orig_out);
int lkmdbg_hook_alloc_exec(struct lkmdbg_inline_hook *hook);
int lkmdbg_hook_prepare_exec(struct lkmdbg_inline_hook *hook, void **orig_out);
int lkmdbg_hook_patch_target(struct lkmdbg_inline_hook *hook, void **orig_out);
int lkmdbg_hook_activate(struct lkmdbg_inline_hook *hook, void **orig_out);
int lkmdbg_hook_deactivate(struct lkmdbg_inline_hook *hook);
int lkmdbg_hook_install(void *target, void *replacement,
			struct lkmdbg_inline_hook **hook_out,
			void **orig_out);
void lkmdbg_hook_destroy(struct lkmdbg_inline_hook *hook);
void lkmdbg_hook_remove(struct lkmdbg_inline_hook *hook);

#endif
