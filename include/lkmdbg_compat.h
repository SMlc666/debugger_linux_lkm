#ifndef _LKMDBG_COMPAT_H
#define _LKMDBG_COMPAT_H

#include <linux/version.h>
#include <linux/highmem.h>

#ifndef TASK_SIZE_MAX
#define TASK_SIZE_MAX TASK_SIZE
#endif

#ifdef CONFIG_ARM64
#include <asm/pgtable.h>

/*
 * arm64 only gained the generic pte_pgprot() helper in 5.19. Older trees
 * keep the same logic locally in hugetlbpage.c, so mirror that here.
 */
static inline pgprot_t lkmdbg_pte_pgprot(pte_t pte)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 19, 0)
	return pte_pgprot(pte);
#else
	unsigned long pfn = pte_pfn(pte);

	return __pgprot(pte_val(pfn_pte(pfn, __pgprot(0))) ^ pte_val(pte));
#endif
}
#endif

#if defined(FOLL_NOFAULT)
#define LKMDBG_GUP_NOFAULT_FLAG FOLL_NOFAULT
#elif defined(FOLL_NOWAIT)
#define LKMDBG_GUP_NOFAULT_FLAG FOLL_NOWAIT
#else
#define LKMDBG_GUP_NOFAULT_FLAG 0U
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
static inline void *lkmdbg_kmap_local_page(struct page *page)
{
	return kmap_local_page(page);
}

static inline void lkmdbg_kunmap_local(struct page *page, const void *addr)
{
	(void)page;
	kunmap_local(addr);
}
#else
static inline void *lkmdbg_kmap_local_page(struct page *page)
{
	return kmap(page);
}

static inline void lkmdbg_kunmap_local(struct page *page, const void *addr)
{
	(void)addr;
	kunmap(page);
}
#endif

#endif
