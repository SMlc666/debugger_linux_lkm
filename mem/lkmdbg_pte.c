#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/mmap_lock.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#ifdef CONFIG_ARM64
#include <asm/tlbflush.h>
#endif

#include "lkmdbg_internal.h"

#define LKMDBG_PTE_PATCH_MAX_ENTRIES 128U

struct lkmdbg_pte_patch {
	struct list_head node;
	u64 id;
	unsigned long page_addr;
	u64 raw_pte;
	u64 baseline_pte;
	u64 expected_pte;
	u64 current_pte;
	u64 baseline_vm_flags;
	u64 current_vm_flags;
	u64 target_gen;
	pid_t target_tgid;
	u32 mode;
	u32 flags;
	u32 state;
	u32 page_shift;
	bool active;
};

#ifdef CONFIG_ARM64
bool lkmdbg_pte_allows_access(pte_t pte, unsigned long vm_flags, u32 type)
{
	switch (type) {
	case LKMDBG_HWPOINT_TYPE_READ:
		return pte_access_permitted(pte, false);
	case LKMDBG_HWPOINT_TYPE_WRITE:
		return pte_access_permitted(pte, true);
	case LKMDBG_HWPOINT_TYPE_EXEC:
		return (vm_flags & VM_EXEC) && pte_user_exec(pte);
	default:
		return false;
	}
}

pte_t lkmdbg_pte_set_exec(pte_t pte, bool executable)
{
	if (executable)
		pte_val(pte) &= ~PTE_UXN;
	else
		pte_val(pte) |= PTE_UXN;
	return pte;
}

pte_t lkmdbg_pte_set_user_read(pte_t pte, bool readable)
{
	if (readable)
		pte = set_pte_bit(pte, __pgprot(PTE_USER));
	else
		pte = clear_pte_bit(pte, __pgprot(PTE_USER));
	return pte;
}

pte_t lkmdbg_pte_make_exec_only(pte_t pte)
{
	pte = pte_wrprotect(pte);
	pte = lkmdbg_pte_set_user_read(pte, false);
	pte = clear_pte_bit(pte, __pgprot(PTE_PROT_NONE));
	pte = set_pte_bit(pte, __pgprot(PTE_VALID));
	pte = lkmdbg_pte_set_exec(pte, true);
	return pte;
}

pte_t lkmdbg_pte_make_protnone(pte_t pte)
{
	pte = pte_wrprotect(pte);
	pte = set_pte_bit(pte, __pgprot(PTE_PROT_NONE));
	pte = clear_pte_bit(pte, __pgprot(PTE_VALID));
	return pte;
}

pte_t lkmdbg_pte_make_writable(pte_t pte)
{
	pte = set_pte_bit(pte, __pgprot(PTE_WRITE));
	pte = clear_pte_bit(pte, __pgprot(PTE_RDONLY));
	return pte;
}

pte_t lkmdbg_pte_build_alias_pte(struct page *page, pte_t template, u32 prot)
{
	pte_t pte;

	pte = pfn_pte(page_to_pfn(page), pte_pgprot(template));
	pte = clear_pte_bit(pte, __pgprot(PTE_PROT_NONE));
	pte = set_pte_bit(pte, __pgprot(PTE_VALID));
	pte = lkmdbg_pte_set_user_read(
		pte, !!(prot & LKMDBG_REMOTE_ALLOC_PROT_READ));
	if (prot & LKMDBG_REMOTE_ALLOC_PROT_WRITE)
		pte = lkmdbg_pte_make_writable(pte);
	else
		pte = pte_wrprotect(pte);
	pte = lkmdbg_pte_set_exec(pte,
				   !!(prot & LKMDBG_REMOTE_ALLOC_PROT_EXEC));
	return pte;
}

static u64 lkmdbg_pte_compare_mask(void)
{
	u64 mask = ~0ULL;

#ifdef PTE_AF
	mask &= ~((u64)PTE_AF);
#endif
#ifdef PTE_DIRTY
	mask &= ~((u64)PTE_DIRTY);
#endif
	return mask;
}

bool lkmdbg_pte_equivalent(pte_t current_pte, pte_t expected_pte)
{
	u64 mask = lkmdbg_pte_compare_mask();

	return (pte_val(current_pte) & mask) == (pte_val(expected_pte) & mask);
}

static void lkmdbg_pte_flush_page(struct mm_struct *mm, unsigned long addr)
{
	unsigned long tlbi_addr;

	dsb(ishst);
	tlbi_addr = __TLBI_VADDR(addr, ASID(mm));
	__tlbi(vale1is, tlbi_addr);
	__tlbi_user(vale1is, tlbi_addr);
	dsb(ish);
}

int lkmdbg_pte_lookup_locked(struct mm_struct *mm, unsigned long addr,
			     pte_t **ptep_out, spinlock_t **ptl_out)
{
	pgd_t *pgdp;
	p4d_t *p4dp;
	pud_t *pudp;
	pmd_t *pmdp;
	pte_t *ptep;
	spinlock_t *ptl;

	pgdp = pgd_offset(mm, addr);
	if (pgd_none(*pgdp) || pgd_bad(*pgdp))
		return -ENOENT;

	p4dp = p4d_offset(pgdp, addr);
	if (p4d_none(*p4dp) || p4d_bad(*p4dp))
		return -ENOENT;
#ifdef p4d_leaf
	if (p4d_leaf(*p4dp))
		return -EOPNOTSUPP;
#endif

	pudp = pud_offset(p4dp, addr);
	if (pud_none(*pudp) || pud_bad(*pudp))
		return -ENOENT;
	if (pud_leaf(*pudp))
		return -EOPNOTSUPP;

	pmdp = pmd_offset(pudp, addr);
	if (pmd_none(*pmdp) || pmd_bad(*pmdp))
		return -ENOENT;
	if (pmd_leaf(*pmdp) || pmd_trans_huge(*pmdp))
		return -EOPNOTSUPP;

	ptl = pte_lockptr(mm, pmdp);
	spin_lock(ptl);

	ptep = pte_offset_kernel(pmdp, addr);
	if (!ptep) {
		spin_unlock(ptl);
		return -ENOENT;
	}

	*ptep_out = ptep;
	*ptl_out = ptl;
	return 0;
}

int lkmdbg_pte_rewrite_locked(struct mm_struct *mm, unsigned long addr,
			      pte_t new_pte, pte_t *old_pte_out,
			      unsigned long *vm_flags_out)
{
	struct vm_area_struct *vma;
	pte_t *ptep;
	spinlock_t *ptl;
	pte_t pte;
	int ret;

	vma = find_vma(mm, addr);
	if (!vma || addr < vma->vm_start || addr >= vma->vm_end)
		return -ENOENT;

	ret = lkmdbg_pte_lookup_locked(mm, addr, &ptep, &ptl);
	if (ret)
		return ret;

	pte = READ_ONCE(*ptep);
	if (!pte_present(pte)) {
		spin_unlock(ptl);
		return -ENOENT;
	}

	if (old_pte_out)
		*old_pte_out = pte;
	if (vm_flags_out)
		*vm_flags_out = vma->vm_flags;

	if (pte_val(new_pte) != pte_val(pte))
		set_pte(ptep, new_pte);

	spin_unlock(ptl);
	if (pte_val(new_pte) != pte_val(pte))
		lkmdbg_pte_flush_page(mm, addr);
	return 0;
}

int lkmdbg_pte_read_locked(struct mm_struct *mm, unsigned long addr,
			   pte_t *pte_out, unsigned long *vm_flags_out)
{
	struct vm_area_struct *vma;
	pte_t *ptep;
	spinlock_t *ptl;
	pte_t pte;
	int ret;

	vma = find_vma(mm, addr);
	if (!vma || addr < vma->vm_start || addr >= vma->vm_end)
		return -ENOENT;

	ret = lkmdbg_pte_lookup_locked(mm, addr, &ptep, &ptl);
	if (ret)
		return ret;

	pte = READ_ONCE(*ptep);
	spin_unlock(ptl);
	if (!pte_present(pte))
		return -ENOENT;

	if (pte_out)
		*pte_out = pte;
	if (vm_flags_out)
		*vm_flags_out = vma->vm_flags;
	return 0;
}

int lkmdbg_pte_capture(struct mm_struct *mm, unsigned long addr, pte_t *pte_out,
		       unsigned long *vm_flags_out)
{
	int ret;

	if (!mm)
		return -ESRCH;

	mmap_read_lock(mm);
	ret = lkmdbg_pte_read_locked(mm, addr, pte_out, vm_flags_out);
	mmap_read_unlock(mm);
	return ret;
}

static int lkmdbg_pte_build_mode(pte_t current_pte, u32 mode, pte_t *new_out)
{
	pte_t new_pte = current_pte;

	switch (mode) {
	case LKMDBG_PTE_MODE_RO:
		new_pte = lkmdbg_pte_set_user_read(new_pte, true);
		new_pte = clear_pte_bit(new_pte, __pgprot(PTE_PROT_NONE));
		new_pte = set_pte_bit(new_pte, __pgprot(PTE_VALID));
		new_pte = pte_wrprotect(new_pte);
		new_pte = lkmdbg_pte_set_exec(new_pte, false);
		break;
	case LKMDBG_PTE_MODE_RW:
		new_pte = lkmdbg_pte_set_user_read(new_pte, true);
		new_pte = clear_pte_bit(new_pte, __pgprot(PTE_PROT_NONE));
		new_pte = set_pte_bit(new_pte, __pgprot(PTE_VALID));
		new_pte = lkmdbg_pte_make_writable(new_pte);
		new_pte = lkmdbg_pte_set_exec(new_pte, false);
		break;
	case LKMDBG_PTE_MODE_RX:
		new_pte = lkmdbg_pte_set_user_read(new_pte, true);
		new_pte = clear_pte_bit(new_pte, __pgprot(PTE_PROT_NONE));
		new_pte = set_pte_bit(new_pte, __pgprot(PTE_VALID));
		new_pte = pte_wrprotect(new_pte);
		new_pte = lkmdbg_pte_set_exec(new_pte, true);
		break;
	case LKMDBG_PTE_MODE_RWX:
		new_pte = lkmdbg_pte_set_user_read(new_pte, true);
		new_pte = clear_pte_bit(new_pte, __pgprot(PTE_PROT_NONE));
		new_pte = set_pte_bit(new_pte, __pgprot(PTE_VALID));
		new_pte = lkmdbg_pte_make_writable(new_pte);
		new_pte = lkmdbg_pte_set_exec(new_pte, true);
		break;
	case LKMDBG_PTE_MODE_PROTNONE:
		new_pte = lkmdbg_pte_make_protnone(new_pte);
		break;
	case LKMDBG_PTE_MODE_EXECONLY:
		new_pte = lkmdbg_pte_make_exec_only(new_pte);
		break;
	default:
		return -EINVAL;
	}

	*new_out = new_pte;
	return 0;
}
#else
bool lkmdbg_pte_allows_access(pte_t pte, unsigned long vm_flags, u32 type)
{
	(void)pte;
	(void)vm_flags;
	(void)type;
	return false;
}

pte_t lkmdbg_pte_set_exec(pte_t pte, bool executable)
{
	(void)executable;
	return pte;
}

pte_t lkmdbg_pte_set_user_read(pte_t pte, bool readable)
{
	(void)readable;
	return pte;
}

pte_t lkmdbg_pte_make_exec_only(pte_t pte)
{
	return pte;
}

pte_t lkmdbg_pte_make_protnone(pte_t pte)
{
	return pte;
}

pte_t lkmdbg_pte_make_writable(pte_t pte)
{
	return pte;
}

pte_t lkmdbg_pte_build_alias_pte(struct page *page, pte_t template, u32 prot)
{
	(void)page;
	(void)prot;
	return template;
}

bool lkmdbg_pte_equivalent(pte_t current_pte, pte_t expected_pte)
{
	return pte_val(current_pte) == pte_val(expected_pte);
}

int lkmdbg_pte_lookup_locked(struct mm_struct *mm, unsigned long addr,
			     pte_t **ptep_out, spinlock_t **ptl_out)
{
	(void)mm;
	(void)addr;
	(void)ptep_out;
	(void)ptl_out;
	return -EOPNOTSUPP;
}

int lkmdbg_pte_rewrite_locked(struct mm_struct *mm, unsigned long addr,
			      pte_t new_pte, pte_t *old_pte_out,
			      unsigned long *vm_flags_out)
{
	(void)mm;
	(void)addr;
	(void)new_pte;
	(void)old_pte_out;
	(void)vm_flags_out;
	return -EOPNOTSUPP;
}

int lkmdbg_pte_read_locked(struct mm_struct *mm, unsigned long addr,
			   pte_t *pte_out, unsigned long *vm_flags_out)
{
	(void)mm;
	(void)addr;
	(void)pte_out;
	(void)vm_flags_out;
	return -EOPNOTSUPP;
}

int lkmdbg_pte_capture(struct mm_struct *mm, unsigned long addr, pte_t *pte_out,
		       unsigned long *vm_flags_out)
{
	(void)mm;
	(void)addr;
	(void)pte_out;
	(void)vm_flags_out;
	return -EOPNOTSUPP;
}

static int lkmdbg_pte_build_mode(pte_t current_pte, u32 mode, pte_t *new_out)
{
	(void)current_pte;
	(void)mode;
	(void)new_out;
	return -EOPNOTSUPP;
}
#endif

static int lkmdbg_validate_pte_patch_request(
	const struct lkmdbg_pte_patch_request *req, bool remove)
{
	if (req->version != LKMDBG_PROTO_VERSION || req->size != sizeof(*req))
		return -EINVAL;

	if (remove)
		return req->id ? 0 : -EINVAL;

	if (!req->addr || req->addr >= (u64)TASK_SIZE_MAX)
		return -EINVAL;

	if (req->flags & ~LKMDBG_PTE_PATCH_FLAG_RAW)
		return -EINVAL;

	if (req->flags & LKMDBG_PTE_PATCH_FLAG_RAW) {
		if (!req->raw_pte)
			return -EINVAL;
#ifdef CONFIG_ARM64
		if (!pte_present(__pte(req->raw_pte)))
			return -EINVAL;
#endif
		return 0;
	}

	switch (req->mode) {
	case LKMDBG_PTE_MODE_RO:
	case LKMDBG_PTE_MODE_RW:
	case LKMDBG_PTE_MODE_RX:
	case LKMDBG_PTE_MODE_RWX:
	case LKMDBG_PTE_MODE_PROTNONE:
	case LKMDBG_PTE_MODE_EXECONLY:
		return 0;
	default:
		return -EINVAL;
	}
}

static int lkmdbg_validate_pte_patch_query(
	const struct lkmdbg_pte_patch_query_request *req)
{
	if (req->version != LKMDBG_PROTO_VERSION || req->size != sizeof(*req))
		return -EINVAL;

	if (!req->entries_addr || !req->max_entries ||
	    req->max_entries > LKMDBG_PTE_PATCH_MAX_ENTRIES)
		return -EINVAL;

	if (req->flags)
		return -EINVAL;

	return 0;
}

static struct lkmdbg_pte_patch *
lkmdbg_find_pte_patch_id_locked(struct lkmdbg_session *session, u64 id)
{
	struct lkmdbg_pte_patch *patch;

	list_for_each_entry(patch, &session->pte_patches, node) {
		if (patch->id == id)
			return patch;
	}

	return NULL;
}

static struct lkmdbg_pte_patch *
lkmdbg_find_pte_patch_page_locked(struct lkmdbg_session *session,
				      unsigned long page_addr)
{
	struct lkmdbg_pte_patch *patch;

	list_for_each_entry(patch, &session->pte_patches, node) {
		if (patch->page_addr == page_addr)
			return patch;
	}

	return NULL;
}

bool lkmdbg_pte_patch_has_overlap_locked(struct lkmdbg_session *session,
					 unsigned long start,
					 unsigned long length)
{
	struct lkmdbg_pte_patch *patch;
	unsigned long end;

	if (!session || !length)
		return false;

	end = start + length;
	if (end < start)
		return true;

	list_for_each_entry(patch, &session->pte_patches, node) {
		unsigned long patch_start = patch->page_addr;
		unsigned long patch_end = patch_start + PAGE_SIZE;

		if (start < patch_end && patch_start < end)
			return true;
	}

	return false;
}

static void lkmdbg_pte_patch_store_current(struct lkmdbg_pte_patch *patch,
					   pte_t current_pte,
					   unsigned long current_vm_flags)
{
	patch->current_pte = pte_val(current_pte);
	patch->current_vm_flags = current_vm_flags;
}

static int lkmdbg_pte_patch_get_target_mm(pid_t target_tgid,
					  struct mm_struct **mm_out)
{
	struct task_struct *task;
	struct mm_struct *mm;

	if (!target_tgid)
		return -ESTALE;

	task = get_pid_task(find_vpid(target_tgid), PIDTYPE_TGID);
	if (!task)
		return -ESRCH;

	mm = get_task_mm(task);
	put_task_struct(task);
	if (!mm)
		return -ESRCH;

	*mm_out = mm;
	return 0;
}

static int lkmdbg_pte_patch_refresh_state(struct lkmdbg_session *session,
					  struct lkmdbg_pte_patch *patch)
{
	struct mm_struct *mm = NULL;
	pte_t current_pte;
	unsigned long current_vm_flags = 0;
	int ret;

	if (!patch)
		return -EINVAL;

	patch->state = 0;
	patch->current_pte = 0;
	patch->current_vm_flags = 0;

	if (READ_ONCE(session->target_gen) != patch->target_gen ||
	    READ_ONCE(session->target_tgid) != patch->target_tgid ||
	    READ_ONCE(session->target_tgid) <= 0) {
		patch->active = false;
		patch->state = LKMDBG_PTE_PATCH_STATE_LOST |
			       LKMDBG_PTE_PATCH_STATE_MUTATED;
		return -ESTALE;
	}

	ret = lkmdbg_pte_patch_get_target_mm(patch->target_tgid, &mm);
	if (ret) {
		patch->active = false;
		patch->state = ret == -ESTALE ?
				       (LKMDBG_PTE_PATCH_STATE_LOST |
					LKMDBG_PTE_PATCH_STATE_MUTATED) :
				       LKMDBG_PTE_PATCH_STATE_LOST;
		return ret;
	}

	ret = lkmdbg_pte_capture(mm, patch->page_addr, &current_pte,
				 &current_vm_flags);
	mmput(mm);
	if (ret) {
		patch->active = false;
		patch->state = LKMDBG_PTE_PATCH_STATE_LOST;
		return ret;
	}

	lkmdbg_pte_patch_store_current(patch, current_pte, current_vm_flags);
	if (current_vm_flags != patch->baseline_vm_flags) {
		patch->active = false;
		patch->state = LKMDBG_PTE_PATCH_STATE_MUTATED;
		return -ESTALE;
	}

	if (patch->active) {
		if (lkmdbg_pte_equivalent(current_pte,
					  __pte(patch->expected_pte))) {
			patch->state = LKMDBG_PTE_PATCH_STATE_ACTIVE;
			return 0;
		}

		patch->active = false;
		if (!lkmdbg_pte_equivalent(current_pte,
					   __pte(patch->baseline_pte)))
			patch->state = LKMDBG_PTE_PATCH_STATE_MUTATED;
		return patch->state ? -ESTALE : 0;
	}

	if (!lkmdbg_pte_equivalent(current_pte, __pte(patch->baseline_pte))) {
		patch->state = LKMDBG_PTE_PATCH_STATE_MUTATED;
		return -ESTALE;
	}

	return 0;
}

static int lkmdbg_pte_patch_restore_live(struct lkmdbg_session *session,
					 struct lkmdbg_pte_patch *patch)
{
	struct mm_struct *mm = NULL;
	pte_t current_pte;
	unsigned long current_vm_flags = 0;
	int ret = 0;

	if (!patch || !patch->active)
		return 0;

	if (READ_ONCE(session->target_gen) != patch->target_gen ||
	    READ_ONCE(session->target_tgid) <= 0) {
		patch->active = false;
		patch->state = LKMDBG_PTE_PATCH_STATE_LOST |
			       LKMDBG_PTE_PATCH_STATE_MUTATED;
		return -ESTALE;
	}

	ret = lkmdbg_pte_patch_get_target_mm(patch->target_tgid, &mm);
	if (ret) {
		patch->active = false;
		patch->state = ret == -ESTALE ?
				       (LKMDBG_PTE_PATCH_STATE_LOST |
					LKMDBG_PTE_PATCH_STATE_MUTATED) :
				       LKMDBG_PTE_PATCH_STATE_LOST;
		return ret;
	}

	mmap_write_lock(mm);
	ret = lkmdbg_pte_read_locked(mm, patch->page_addr, &current_pte,
				     &current_vm_flags);
	if (!ret) {
		lkmdbg_pte_patch_store_current(patch, current_pte,
					       current_vm_flags);
		if (current_vm_flags != patch->baseline_vm_flags) {
			patch->active = false;
			patch->state = LKMDBG_PTE_PATCH_STATE_MUTATED;
			ret = -ESTALE;
		} else if (lkmdbg_pte_equivalent(
				   current_pte, __pte(patch->expected_pte))) {
			ret = lkmdbg_pte_rewrite_locked(mm, patch->page_addr,
							__pte(patch->baseline_pte),
							NULL, NULL);
			if (!ret) {
				patch->active = false;
				patch->state = 0;
				patch->current_pte = patch->baseline_pte;
				patch->current_vm_flags =
					patch->baseline_vm_flags;
			}
		} else if (lkmdbg_pte_equivalent(
				   current_pte, __pte(patch->baseline_pte))) {
			patch->active = false;
			patch->state = 0;
			ret = 0;
		} else {
			patch->active = false;
			patch->state = LKMDBG_PTE_PATCH_STATE_MUTATED;
			ret = -ESTALE;
		}
	} else {
		patch->active = false;
		patch->state = LKMDBG_PTE_PATCH_STATE_LOST;
	}
	mmap_write_unlock(mm);
	mmput(mm);
	return ret;
}

static void lkmdbg_pte_patch_fill_request(
	struct lkmdbg_pte_patch_request *req, const struct lkmdbg_pte_patch *patch)
{
	req->id = patch->id;
	req->addr = patch->page_addr;
	req->raw_pte = patch->raw_pte;
	req->baseline_pte = patch->baseline_pte;
	req->expected_pte = patch->expected_pte;
	req->current_pte = patch->current_pte;
	req->baseline_vm_flags = patch->baseline_vm_flags;
	req->current_vm_flags = patch->current_vm_flags;
	req->mode = patch->mode;
	req->flags = patch->flags;
	req->state = patch->state;
	req->page_shift = patch->page_shift;
}

static void lkmdbg_pte_patch_fill_entry(
	struct lkmdbg_pte_patch_entry *entry, const struct lkmdbg_pte_patch *patch)
{
	memset(entry, 0, sizeof(*entry));
	entry->id = patch->id;
	entry->page_addr = patch->page_addr;
	entry->raw_pte = patch->raw_pte;
	entry->baseline_pte = patch->baseline_pte;
	entry->expected_pte = patch->expected_pte;
	entry->current_pte = patch->current_pte;
	entry->baseline_vm_flags = patch->baseline_vm_flags;
	entry->current_vm_flags = patch->current_vm_flags;
	entry->mode = patch->mode;
	entry->flags = patch->flags;
	entry->state = patch->state;
	entry->page_shift = patch->page_shift;
}

static int lkmdbg_pte_patch_copy_reply(void __user *argp,
				       struct lkmdbg_pte_patch_request *req)
{
	if (copy_to_user(argp, req, sizeof(*req)))
		return -EFAULT;
	return 0;
}

static int lkmdbg_pte_patch_query_copy_reply(
	void __user *argp, struct lkmdbg_pte_patch_query_request *req,
	struct lkmdbg_pte_patch_entry *entries, size_t entries_bytes)
{
	if (copy_to_user(u64_to_user_ptr(req->entries_addr), entries,
			 entries_bytes))
		return -EFAULT;
	if (copy_to_user(argp, req, sizeof(*req)))
		return -EFAULT;
	return 0;
}

long lkmdbg_apply_pte_patch(struct lkmdbg_session *session, void __user *argp)
{
	struct lkmdbg_pte_patch_request req;
	struct lkmdbg_pte_patch *patch;
	struct mm_struct *mm = NULL;
	unsigned long page_addr;
	unsigned long baseline_vm_flags = 0;
	pte_t baseline_pte;
	pte_t expected_pte;
	u64 target_gen;
	pid_t target_tgid;
	int ret;

#ifndef CONFIG_ARM64
	return -EOPNOTSUPP;
#endif

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	ret = lkmdbg_validate_pte_patch_request(&req, false);
	if (ret)
		return ret;

	page_addr = (unsigned long)req.addr & PAGE_MASK;
	patch = kzalloc(sizeof(*patch), GFP_KERNEL);
	if (!patch)
		return -ENOMEM;

	mutex_lock(&session->lock);
	if (lkmdbg_find_pte_patch_page_locked(session, page_addr) ||
	    lkmdbg_remote_alloc_has_overlap_locked(session, page_addr,
						    PAGE_SIZE)) {
		mutex_unlock(&session->lock);
		kfree(patch);
		return -EEXIST;
	}
	session->next_pte_patch_id++;
	patch->id = session->next_pte_patch_id;
	target_gen = session->target_gen;
	target_tgid = session->target_tgid;
	mutex_unlock(&session->lock);

	ret = lkmdbg_get_target_mm(session, &mm);
	if (ret) {
		kfree(patch);
		return ret;
	}

	mmap_write_lock(mm);
	ret = lkmdbg_pte_read_locked(mm, page_addr, &baseline_pte,
				     &baseline_vm_flags);
	if (!ret) {
		if (req.flags & LKMDBG_PTE_PATCH_FLAG_RAW) {
			expected_pte = __pte(req.raw_pte);
		} else {
			ret = lkmdbg_pte_build_mode(baseline_pte, req.mode,
						     &expected_pte);
		}
	}
	if (!ret)
		ret = lkmdbg_pte_rewrite_locked(mm, page_addr, expected_pte, NULL,
						NULL);
	mmap_write_unlock(mm);
	if (ret) {
		mmput(mm);
		kfree(patch);
		return ret;
	}

	INIT_LIST_HEAD(&patch->node);
	patch->page_addr = page_addr;
	patch->raw_pte = req.raw_pte;
	patch->baseline_pte = pte_val(baseline_pte);
	patch->expected_pte = pte_val(expected_pte);
	patch->current_pte = pte_val(expected_pte);
	patch->baseline_vm_flags = baseline_vm_flags;
	patch->current_vm_flags = baseline_vm_flags;
	patch->target_gen = target_gen;
	patch->target_tgid = target_tgid;
	patch->mode = (req.flags & LKMDBG_PTE_PATCH_FLAG_RAW) ?
			      LKMDBG_PTE_MODE_RAW :
			      req.mode;
	patch->flags = req.flags;
	patch->page_shift = PAGE_SHIFT;
	patch->active = true;
	patch->state = LKMDBG_PTE_PATCH_STATE_ACTIVE;

	mutex_lock(&session->lock);
	if (session->target_gen != target_gen ||
	    lkmdbg_find_pte_patch_page_locked(session, page_addr)) {
		mutex_unlock(&session->lock);
		mmap_write_lock(mm);
		lkmdbg_pte_rewrite_locked(mm, page_addr, baseline_pte, NULL, NULL);
		mmap_write_unlock(mm);
		mmput(mm);
		kfree(patch);
		return -ESTALE;
	}
	list_add_tail(&patch->node, &session->pte_patches);
	mutex_unlock(&session->lock);
	mmput(mm);

	lkmdbg_pte_patch_fill_request(&req, patch);
	return lkmdbg_pte_patch_copy_reply(argp, &req);
}

long lkmdbg_remove_pte_patch(struct lkmdbg_session *session, void __user *argp)
{
	struct lkmdbg_pte_patch_request req;
	struct lkmdbg_pte_patch *patch;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	if (lkmdbg_validate_pte_patch_request(&req, true))
		return -EINVAL;

	mutex_lock(&session->lock);
	patch = lkmdbg_find_pte_patch_id_locked(session, req.id);
	if (!patch) {
		mutex_unlock(&session->lock);
		return -ENOENT;
	}
	list_del_init(&patch->node);
	mutex_unlock(&session->lock);

	lkmdbg_pte_patch_restore_live(session, patch);
	lkmdbg_pte_patch_fill_request(&req, patch);
	kfree(patch);
	return lkmdbg_pte_patch_copy_reply(argp, &req);
}

long lkmdbg_query_pte_patches(struct lkmdbg_session *session, void __user *argp)
{
	struct lkmdbg_pte_patch_query_request req;
	struct lkmdbg_pte_patch_entry *entries;
	struct lkmdbg_pte_patch *patch;
	u32 filled = 0;
	bool started;
	int ret;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	ret = lkmdbg_validate_pte_patch_query(&req);
	if (ret)
		return ret;

	entries = kcalloc(req.max_entries, sizeof(*entries), GFP_KERNEL);
	if (!entries)
		return -ENOMEM;

	started = req.start_id == 0;
	req.done = 1;
	req.next_id = req.start_id;
	mutex_lock(&session->lock);
	list_for_each_entry(patch, &session->pte_patches, node) {
		if (!started) {
			if (patch->id <= req.start_id)
				continue;
			started = true;
		}
		if (filled == req.max_entries) {
			req.done = 0;
			break;
		}

		lkmdbg_pte_patch_refresh_state(session, patch);
		lkmdbg_pte_patch_fill_entry(&entries[filled], patch);
		req.next_id = patch->id;
		filled++;
	}
	req.entries_filled = filled;
	mutex_unlock(&session->lock);

	ret = lkmdbg_pte_patch_query_copy_reply(
		argp, &req, entries, filled * sizeof(*entries));
	kfree(entries);
	return ret;
}

static void lkmdbg_pte_patch_release_list(struct lkmdbg_session *session,
					  struct list_head *release_list)
{
	struct lkmdbg_pte_patch *patch;
	struct lkmdbg_pte_patch *tmp;

	list_for_each_entry_safe(patch, tmp, release_list, node) {
		list_del_init(&patch->node);
		lkmdbg_pte_patch_restore_live(session, patch);
		kfree(patch);
	}
}

void lkmdbg_pte_patch_release(struct lkmdbg_session *session)
{
	LIST_HEAD(release_list);
	struct lkmdbg_pte_patch *patch;
	struct lkmdbg_pte_patch *tmp;

	if (!session)
		return;

	mutex_lock(&session->lock);
	list_for_each_entry_safe(patch, tmp, &session->pte_patches, node) {
		list_move_tail(&patch->node, &release_list);
	}
	mutex_unlock(&session->lock);

	lkmdbg_pte_patch_release_list(session, &release_list);
}

int lkmdbg_pte_patch_on_target_change(struct lkmdbg_session *session)
{
	lkmdbg_pte_patch_release(session);
	return 0;
}
