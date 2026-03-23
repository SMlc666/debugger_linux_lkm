#include <linux/dcache.h>
#include <linux/fs.h>
#include <linux/kdev_t.h>
#include <linux/mm.h>
#include <linux/mm_types.h>

#include "lkmdbg_internal.h"

static u32 lkmdbg_target_pt_decode_flags(u64 entry_raw)
{
	u32 flags = 0;

#ifdef PTE_VALID
	if (entry_raw & PTE_VALID)
		flags |= LKMDBG_PAGE_PT_FLAG_VALID;
#endif
#ifdef PTE_USER
	if (entry_raw & PTE_USER)
		flags |= LKMDBG_PAGE_PT_FLAG_USER;
#endif
#if defined(PTE_WRITE)
	if (entry_raw & PTE_WRITE)
		flags |= LKMDBG_PAGE_PT_FLAG_WRITE;
#elif defined(PTE_RDONLY)
	if (!(entry_raw & PTE_RDONLY))
		flags |= LKMDBG_PAGE_PT_FLAG_WRITE;
#endif
#ifdef PTE_DIRTY
	if (entry_raw & PTE_DIRTY)
		flags |= LKMDBG_PAGE_PT_FLAG_DIRTY;
#endif
#ifdef PTE_AF
	if (entry_raw & PTE_AF)
		flags |= LKMDBG_PAGE_PT_FLAG_YOUNG;
#endif
#ifdef PTE_UXN
	if (!(entry_raw & PTE_UXN))
		flags |= LKMDBG_PAGE_PT_FLAG_EXEC;
#endif
#if defined(PTE_PROT_NONE)
	if (entry_raw & PTE_PROT_NONE)
		flags |= LKMDBG_PAGE_PT_FLAG_PROTNONE;
#elif defined(PTE_PRESENT_INVALID)
	if (entry_raw & PTE_PRESENT_INVALID)
		flags |= LKMDBG_PAGE_PT_FLAG_PROTNONE;
#endif

	return flags;
}

u32 lkmdbg_target_vm_prot_bits(u64 vm_flags)
{
	u32 prot = 0;

	if (vm_flags & VM_READ)
		prot |= LKMDBG_VMA_PROT_READ;
	if (vm_flags & VM_WRITE)
		prot |= LKMDBG_VMA_PROT_WRITE;
	if (vm_flags & VM_EXEC)
		prot |= LKMDBG_VMA_PROT_EXEC;
	if (vm_flags & VM_MAYREAD)
		prot |= LKMDBG_VMA_PROT_MAYREAD;
	if (vm_flags & VM_MAYWRITE)
		prot |= LKMDBG_VMA_PROT_MAYWRITE;
	if (vm_flags & VM_MAYEXEC)
		prot |= LKMDBG_VMA_PROT_MAYEXEC;

	return prot;
}

const char *lkmdbg_target_vma_special_name(struct mm_struct *mm,
					   struct vm_area_struct *vma,
					   u32 *flags)
{
	if (mm->start_stack >= vma->vm_start && mm->start_stack < vma->vm_end) {
		if (flags)
			*flags |= LKMDBG_VMA_FLAG_STACK;
		return "[stack]";
	}

	if (mm->start_brk >= vma->vm_start && mm->start_brk < vma->vm_end) {
		if (flags)
			*flags |= LKMDBG_VMA_FLAG_HEAP;
		return "[heap]";
	}

	return NULL;
}

static u32 lkmdbg_target_vma_flag_bits(struct mm_struct *mm,
				       struct vm_area_struct *vma)
{
	u64 vm_flags = (u64)vma->vm_flags;
	u32 flags = 0;

	if (vm_flags & VM_SHARED)
		flags |= LKMDBG_VMA_FLAG_SHARED;
	if (vm_flags & VM_PFNMAP)
		flags |= LKMDBG_VMA_FLAG_PFNMAP;
	if (vm_flags & VM_IO)
		flags |= LKMDBG_VMA_FLAG_IO;

	if (!vma->vm_file) {
		flags |= LKMDBG_VMA_FLAG_ANON;
		lkmdbg_target_vma_special_name(mm, vma, &flags);
	} else {
		flags |= LKMDBG_VMA_FLAG_FILE;
	}

	return flags;
}

void lkmdbg_target_vma_fill_info(struct mm_struct *mm,
				 struct vm_area_struct *vma,
				 struct lkmdbg_target_vma_info *info)
{
	struct inode *inode = NULL;

	memset(info, 0, sizeof(*info));
	info->start_addr = vma->vm_start;
	info->end_addr = vma->vm_end;
	info->pgoff = vma->vm_pgoff;
	info->vm_flags_raw = (u64)vma->vm_flags;
	info->prot = lkmdbg_target_vm_prot_bits(info->vm_flags_raw);
	info->flags = lkmdbg_target_vma_flag_bits(mm, vma);

	if (!vma->vm_file)
		return;

	inode = file_inode(vma->vm_file);
	if (!inode)
		return;

	info->inode = inode->i_ino;
	info->dev_major = MAJOR(inode->i_sb->s_dev);
	info->dev_minor = MINOR(inode->i_sb->s_dev);
}

int lkmdbg_target_vma_lookup_locked(struct mm_struct *mm, u64 addr, u64 length,
				    struct vm_area_struct **vma_out)
{
	struct vm_area_struct *vma;
	u64 end_addr;

	if (!mm || !vma_out || !length)
		return -EINVAL;

	end_addr = addr + length;
	if (end_addr < addr)
		return -EINVAL;

	vma = find_vma(mm, addr);
	if (!vma || addr < vma->vm_start || end_addr > vma->vm_end)
		return -EINVAL;

	*vma_out = vma;
	return 0;
}

static void lkmdbg_target_pt_fill_leaf(struct lkmdbg_target_pt_info *info,
				       u64 entry_raw, u64 phys_addr,
				       u32 level, u32 page_shift, bool huge)
{
	info->entry_raw = entry_raw;
	info->phys_addr = phys_addr;
	info->level = level;
	info->page_shift = page_shift;
	info->flags |= LKMDBG_TARGET_PT_FLAG_PRESENT;
	info->pt_flags = lkmdbg_target_pt_decode_flags(entry_raw);
	if (huge)
		info->flags |= LKMDBG_TARGET_PT_FLAG_HUGE;
}

int lkmdbg_target_pt_lookup_locked(struct mm_struct *mm, unsigned long addr,
				   struct lkmdbg_target_pt_info *info)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *ptep;
	pte_t pte;

	if (!mm || !info)
		return -EINVAL;

	memset(info, 0, sizeof(*info));

	pgd = pgd_offset(mm, addr);
	if (pgd_none(*pgd) || pgd_bad(*pgd))
		return 0;

	p4d = p4d_offset(pgd, addr);
	if (p4d_none(*p4d) || p4d_bad(*p4d))
		return 0;

	pud = pud_offset(p4d, addr);
	if (pud_none(*pud) || pud_bad(*pud))
		return 0;

	if (pud_leaf(*pud)) {
		if (pud_present(*pud))
			lkmdbg_target_pt_fill_leaf(
				info, (u64)pud_val(*pud),
				((u64)pud_pfn(*pud) << PAGE_SHIFT) +
					(addr & ~PUD_MASK),
				LKMDBG_PAGE_LEVEL_PUD, PUD_SHIFT, true);
		else
			info->level = LKMDBG_PAGE_LEVEL_PUD;
		return 0;
	}

	pmd = pmd_offset(pud, addr);
	if (pmd_none(*pmd) || pmd_bad(*pmd))
		return 0;

	if (pmd_leaf(*pmd)) {
		if (pmd_present(*pmd))
			lkmdbg_target_pt_fill_leaf(
				info, (u64)pmd_val(*pmd),
				((u64)pmd_pfn(*pmd) << PAGE_SHIFT) +
					(addr & ~PMD_MASK),
				LKMDBG_PAGE_LEVEL_PMD, PMD_SHIFT, true);
		else
			info->level = LKMDBG_PAGE_LEVEL_PMD;
		return 0;
	}

	ptep = pte_offset_kernel(pmd, addr);
	if (!ptep)
		return 0;

	pte = *ptep;
	if (pte_none(pte))
		return 0;

	info->entry_raw = (u64)pte_val(pte);
	info->level = LKMDBG_PAGE_LEVEL_PTE;
	info->page_shift = PAGE_SHIFT;
	info->pt_flags = lkmdbg_target_pt_decode_flags(info->entry_raw);
	if (pte_present(pte))
		lkmdbg_target_pt_fill_leaf(info, (u64)pte_val(pte),
					  (u64)pte_pfn(pte) << PAGE_SHIFT,
					  LKMDBG_PAGE_LEVEL_PTE, PAGE_SHIFT,
					  false);

	return 0;
}
