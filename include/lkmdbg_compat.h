#ifndef _LKMDBG_COMPAT_H
#define _LKMDBG_COMPAT_H

#include <linux/version.h>

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

#endif
