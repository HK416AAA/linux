// SPDX-License-Identifier: GPL-2.0-only
/*
 * Based on arch/arm/mm/fault.c
 *
 * Copyright (C) 1995  Linus Torvalds
 * Copyright (C) 1995-2004 Russell King
 * Copyright (C) 2012 ARM Ltd.
 */

#include <linux/acpi.h>
#include <linux/bitfield.h>
#include <linux/bpf_defs.h>
#include <linux/extable.h>
#include <linux/kfence.h>
#include <linux/signal.h>
#include <linux/mm.h>
#include <linux/hardirq.h>
#include <linux/init.h>
#include <linux/irqflags.h>
#include <linux/kasan.h>
#include <linux/kprobes.h>
#include <linux/uaccess.h>
#include <linux/page-flags.h>
#include <linux/sched/signal.h>
#include <linux/sched/debug.h>
#include <linux/highmem.h>
#include <linux/perf_event.h>
#include <linux/pkeys.h>
#include <linux/preempt.h>
#include <linux/hugetlb.h>
#include <linux/page-isolation.h>
#include <linux/memblock.h>

#include <asm/acpi.h>
#include <asm/sections.h>
#include <asm/bug.h>
#include <asm/cmpxchg.h>
#include <asm/cpufeature.h>
#include <asm/efi.h>
#include <asm/exception.h>
#include <asm/daifflags.h>
#include <asm/debug-monitors.h>
#include <asm/esr.h>
#include <asm/kprobes.h>
#include <asm/mte.h>
#include <asm/processor.h>
#include <asm/sysreg.h>
#include <asm/system_misc.h>
#include <asm/tlbflush.h>
#include <asm/traps.h>
#include <asm/virt.h>

struct fault_info {
	int	(*fn)(unsigned long far, unsigned long esr,
		      struct pt_regs *regs);
	int	sig;
	int	code;
	const char *name;
};

static const struct fault_info fault_info[];

void charlotte_ua_trace(unsigned long far, unsigned long esr,
			struct pt_regs *regs, const char *kind);
void charlotte_ckpt_hex(const char *tag, unsigned long long v);
void charlotte_ckpt(const char *tag);

static inline const struct fault_info *esr_to_fault_info(unsigned long esr)
{
	return fault_info + (esr & ESR_ELx_FSC);
}

static void data_abort_decode(unsigned long esr)
{
	unsigned long iss2 = ESR_ELx_ISS2(esr);

	pr_alert("Data abort info:\n");

	if (esr & ESR_ELx_ISV) {
		pr_alert("  Access size = %u byte(s)\n",
			 1U << ((esr & ESR_ELx_SAS) >> ESR_ELx_SAS_SHIFT));
		pr_alert("  SSE = %lu, SRT = %lu\n",
			 (esr & ESR_ELx_SSE) >> ESR_ELx_SSE_SHIFT,
			 (esr & ESR_ELx_SRT_MASK) >> ESR_ELx_SRT_SHIFT);
		pr_alert("  SF = %lu, AR = %lu\n",
			 (esr & ESR_ELx_SF) >> ESR_ELx_SF_SHIFT,
			 (esr & ESR_ELx_AR) >> ESR_ELx_AR_SHIFT);
		pr_alert("  Xs = %llu\n",
			 (iss2 & ESR_ELx_Xs_MASK) >> ESR_ELx_Xs_SHIFT);
	} else {
		pr_alert("  ISV = 0, ISS = 0x%08lx, ISS2 = 0x%08lx\n",
			 esr & ESR_ELx_ISS_MASK, iss2);
	}

	pr_alert("  CM = %lu, WnR = %lu, TnD = %lu, TagAccess = %lu\n",
		 (esr & ESR_ELx_CM) >> ESR_ELx_CM_SHIFT,
		 (esr & ESR_ELx_WNR) >> ESR_ELx_WNR_SHIFT,
		 (iss2 & ESR_ELx_TnD) >> ESR_ELx_TnD_SHIFT,
		 (iss2 & ESR_ELx_TagAccess) >> ESR_ELx_TagAccess_SHIFT);

	pr_alert("  GCS = %ld, Overlay = %lu, DirtyBit = %lu\n",
		 (iss2 & ESR_ELx_GCS) >> ESR_ELx_GCS_SHIFT,
		 (iss2 & ESR_ELx_Overlay) >> ESR_ELx_Overlay_SHIFT,
		 (iss2 & ESR_ELx_DirtyBit) >> ESR_ELx_DirtyBit_SHIFT);
}

static void mem_abort_decode(unsigned long esr)
{
	pr_alert("Mem abort info:\n");

	pr_alert("  ESR = 0x%016lx\n", esr);
	pr_alert("  EC = 0x%02lx: %s, IL = %u bits\n",
		 ESR_ELx_EC(esr), esr_get_class_string(esr),
		 (esr & ESR_ELx_IL) ? 32 : 16);
	pr_alert("  SET = %lu, FnV = %lu\n",
		 (esr & ESR_ELx_SET_MASK) >> ESR_ELx_SET_SHIFT,
		 (esr & ESR_ELx_FnV) >> ESR_ELx_FnV_SHIFT);
	pr_alert("  EA = %lu, S1PTW = %lu\n",
		 (esr & ESR_ELx_EA) >> ESR_ELx_EA_SHIFT,
		 (esr & ESR_ELx_S1PTW) >> ESR_ELx_S1PTW_SHIFT);
	pr_alert("  FSC = 0x%02lx: %s\n", (esr & ESR_ELx_FSC),
		 esr_to_fault_info(esr)->name);

	if (esr_is_data_abort(esr))
		data_abort_decode(esr);
}

static inline unsigned long mm_to_pgd_phys(struct mm_struct *mm)
{
	/* Either init_pg_dir or swapper_pg_dir */
	if (mm == &init_mm)
		return __pa_symbol(mm->pgd);

	return (unsigned long)virt_to_phys(mm->pgd);
}

/*
 * Dump out the page tables associated with 'addr' in the currently active mm.
 */
static void show_pte(unsigned long addr)
{
	struct mm_struct *mm;
	pgd_t *pgdp;
	pgd_t pgd;

	if (is_ttbr0_addr(addr)) {
		/* TTBR0 */
		mm = current->active_mm;
		if (mm == &init_mm) {
			pr_alert("[%016lx] user address but active_mm is swapper\n",
				 addr);
			return;
		}
	} else if (is_ttbr1_addr(addr)) {
		/* TTBR1 */
		mm = &init_mm;
	} else {
		pr_alert("[%016lx] address between user and kernel address ranges\n",
			 addr);
		return;
	}

	pr_alert("%s pgtable: %luk pages, %llu-bit VAs, pgdp=%016lx\n",
		 mm == &init_mm ? "swapper" : "user", PAGE_SIZE / SZ_1K,
		 vabits_actual, mm_to_pgd_phys(mm));

	guard(irqsave)();

	pgdp = pgd_offset(mm, addr);
	pgd = READ_ONCE(*pgdp);
	pr_alert("[%016lx] pgd=%016llx", addr, pgd_val(pgd));

	do {
		p4d_t *p4dp, p4d;
		pud_t *pudp, pud;
		pmd_t *pmdp, pmd;
		pte_t *ptep, pte;

		if (pgd_none(pgd) || pgd_bad(pgd))
			break;

		p4dp = p4d_offset_lockless(pgdp, pgd, addr);
		p4d = READ_ONCE(*p4dp);
		pr_cont(", p4d=%016llx", p4d_val(p4d));
		if (p4d_none(p4d) || p4d_bad(p4d))
			break;

		pudp = pud_offset_lockless(p4dp, p4d, addr);
		pud = READ_ONCE(*pudp);
		pr_cont(", pud=%016llx", pud_val(pud));
		if (pud_none(pud) || pud_bad(pud))
			break;

		pmdp = pmd_offset_lockless(pudp, pud, addr);
		pmd = READ_ONCE(*pmdp);
		pr_cont(", pmd=%016llx", pmd_val(pmd));
		if (pmd_none(pmd) || pmd_bad(pmd))
			break;

		ptep = pte_offset_map(&pmd, addr);
		if (!ptep)
			break;

		pte = __ptep_get(ptep);
		pr_cont(", pte=%016llx", pte_val(pte));
		pte_unmap(ptep);
	} while(0);

	pr_cont("\n");
}

/*
 * This function sets the access flags (dirty, accessed), as well as write
 * permission, and only to a more permissive setting.
 *
 * It needs to cope with hardware update of the accessed/dirty state by other
 * agents in the system and can safely skip the __sync_icache_dcache() call as,
 * like __set_ptes(), the PTE is never changed from no-exec to exec here.
 *
 * Returns whether or not the PTE actually changed.
 */
int __ptep_set_access_flags_anysz(struct vm_area_struct *vma,
				  unsigned long address, pte_t *ptep,
				  pte_t entry, int dirty, unsigned long pgsize)
{
	pteval_t old_pteval, pteval;
	pte_t pte = __ptep_get(ptep);
	int level;

	if (pte_same(pte, entry))
		return 0;

	/* only preserve the access flags and write permission */
	pte_val(entry) &= PTE_RDONLY | PTE_AF | PTE_WRITE | PTE_DIRTY;

	/*
	 * Setting the flags must be done atomically to avoid racing with the
	 * hardware update of the access/dirty state. The PTE_RDONLY bit must
	 * be set to the most permissive (lowest value) of *ptep and entry
	 * (calculated as: a & b == ~(~a | ~b)).
	 */
	pte_val(entry) ^= PTE_RDONLY;
	pteval = pte_val(pte);
	do {
		old_pteval = pteval;
		pteval ^= PTE_RDONLY;
		pteval |= pte_val(entry);
		pteval ^= PTE_RDONLY;
		pteval = cmpxchg_relaxed(&pte_val(*ptep), old_pteval, pteval);
	} while (pteval != old_pteval);

	/*
	 * Invalidate the local stale read-only entry.  Remote stale entries
	 * may still cause page faults and be invalidated via
	 * flush_tlb_fix_spurious_fault().
	 */
	if (dirty) {
		switch (pgsize) {
		case PAGE_SIZE:
			level = 3;
			break;
		case PMD_SIZE:
			level = 2;
			break;
#ifndef __PAGETABLE_PMD_FOLDED
		case PUD_SIZE:
			level = 1;
			break;
#endif
		default:
			level = TLBI_TTL_UNKNOWN;
			WARN_ON(1);
		}

		__flush_tlb_range(vma, address, address + pgsize, pgsize, level,
				  TLBF_NOWALKCACHE | TLBF_NOBROADCAST);
	}
	return 1;
}

static bool is_el1_instruction_abort(unsigned long esr)
{
	return ESR_ELx_EC(esr) == ESR_ELx_EC_IABT_CUR;
}

static bool is_el1_data_abort(unsigned long esr)
{
	return ESR_ELx_EC(esr) == ESR_ELx_EC_DABT_CUR;
}

static inline bool is_el1_permission_fault(unsigned long addr, unsigned long esr,
					   struct pt_regs *regs)
{
	if (!is_el1_data_abort(esr) && !is_el1_instruction_abort(esr))
		return false;

	if (esr_fsc_is_permission_fault(esr))
		return true;

	if (is_ttbr0_addr(addr) && system_uses_ttbr0_pan())
		return esr_fsc_is_translation_fault(esr) &&
			(regs->pstate & PSR_PAN_BIT);

	return false;
}

static bool is_pkvm_stage2_abort(unsigned int esr)
{
	/*
	 * S1PTW should only ever be set in ESR_EL1 if the pkvm hypervisor
	 * injected a stage-2 abort -- see host_inject_mem_abort().
	 */
	return is_pkvm_initialized() && (esr & ESR_ELx_S1PTW);
}

static bool __kprobes is_spurious_el1_translation_fault(unsigned long addr,
							unsigned long esr,
							struct pt_regs *regs)
{
	unsigned long flags;
	u64 par, dfsc;

	if (!is_el1_data_abort(esr) || !esr_fsc_is_translation_fault(esr))
		return false;

	local_irq_save(flags);
	asm volatile("at s1e1r, %0" :: "r" (addr));
	isb();
	par = read_sysreg_par();
	local_irq_restore(flags);

	/*
	 * If we now have a valid translation, treat the translation fault as
	 * spurious.
	 */
	if (!(par & SYS_PAR_EL1_F)) {
		if (is_pkvm_stage2_abort(esr)) {
			par &= SYS_PAR_EL1_PA;
			return pkvm_force_reclaim_guest_page(par);
		}

		return true;
	}

	/*
	 * If we got a different type of fault from the AT instruction,
	 * treat the translation fault as spurious.
	 */
	dfsc = FIELD_GET(SYS_PAR_EL1_FST, par);
	return !esr_fsc_is_translation_fault(dfsc);
}

static void die_kernel_fault(const char *msg, unsigned long addr,
			     unsigned long esr, struct pt_regs *regs)
{
	bust_spinlocks(1);

	pr_alert("Unable to handle kernel %s at virtual address %016lx\n", msg,
		 addr);

	kasan_non_canonical_hook(addr);

	mem_abort_decode(esr);

	show_pte(addr);
	die("Oops", regs, esr);
	bust_spinlocks(0);
	make_task_dead(SIGKILL);
}

#ifdef CONFIG_KASAN_HW_TAGS
static void report_tag_fault(unsigned long addr, unsigned long esr,
			     struct pt_regs *regs)
{
	/*
	 * SAS bits aren't set for all faults reported in EL1, so we can't
	 * find out access size.
	 */
	bool is_write = !!(esr & ESR_ELx_WNR);
	kasan_report((void *)addr, 0, is_write, regs->pc);
}
#else
/* Tag faults aren't enabled without CONFIG_KASAN_HW_TAGS. */
static inline void report_tag_fault(unsigned long addr, unsigned long esr,
				    struct pt_regs *regs) { }
#endif

static void do_tag_recovery(unsigned long addr, unsigned long esr,
			   struct pt_regs *regs)
{

	report_tag_fault(addr, esr, regs);

	/*
	 * Disable MTE Tag Checking on the local CPU for the current EL.
	 * It will be done lazily on the other CPUs when they will hit a
	 * tag fault.
	 */
	sysreg_clear_set(sctlr_el1, SCTLR_EL1_TCF_MASK,
			 SYS_FIELD_PREP_ENUM(SCTLR_EL1, TCF, NONE));
	isb();
}

static bool is_el1_mte_sync_tag_check_fault(unsigned long esr)
{
	unsigned long fsc = esr & ESR_ELx_FSC;

	if (!is_el1_data_abort(esr))
		return false;

	if (fsc == ESR_ELx_FSC_MTE)
		return true;

	return false;
}

static void __do_kernel_fault(unsigned long addr, unsigned long esr,
			      struct pt_regs *regs)
{
	const char *msg;

	/*
	 * Are we prepared to handle this kernel fault?
	 * We are almost certainly not prepared to handle instruction faults.
	 */
	if (!is_el1_instruction_abort(esr) && fixup_exception(regs, esr))
		return;

	if (is_spurious_el1_translation_fault(addr, esr, regs)) {
		WARN_RATELIMIT(!is_pkvm_stage2_abort(esr),
			"Ignoring spurious kernel translation fault at virtual address %016lx\n", addr);
		return;
	}

	if (is_el1_mte_sync_tag_check_fault(esr)) {
		do_tag_recovery(addr, esr, regs);

		return;
	}

	if (is_el1_permission_fault(addr, esr, regs)) {
		if (esr & ESR_ELx_WNR)
			msg = "write to read-only memory";
		else if (is_el1_instruction_abort(esr))
			msg = "execute from non-executable memory";
		else
			msg = "read from unreadable memory";
	} else if (addr < PAGE_SIZE) {
		msg = "NULL pointer dereference";
	} else if (is_pkvm_stage2_abort(esr)) {
		msg = "access to hypervisor-protected memory";
	} else {
		if (esr_fsc_is_translation_fault(esr)) {
			if (kfence_handle_page_fault(addr, esr & ESR_ELx_WNR, regs))
				return;
			if (bpf_arena_handle_page_fault(addr, esr & ESR_ELx_WNR, regs->pc))
				return;
		}

		msg = "paging request";
	}

	if (efi_runtime_fixup_exception(regs, msg))
		return;

	die_kernel_fault(msg, addr, esr, regs);
}

static void set_thread_esr(unsigned long address, unsigned long esr)
{
	current->thread.fault_address = address;

	/*
	 * If the faulting address is in the kernel, we must sanitize the ESR.
	 * From userspace's point of view, kernel-only mappings don't exist
	 * at all, so we report them as level 0 translation faults.
	 * (This is not quite the way that "no mapping there at all" behaves:
	 * an alignment fault not caused by the memory type would take
	 * precedence over translation fault for a real access to empty
	 * space. Unfortunately we can't easily distinguish "alignment fault
	 * not caused by memory type" from "alignment fault caused by memory
	 * type", so we ignore this wrinkle and just return the translation
	 * fault.)
	 */
	if (!is_ttbr0_addr(current->thread.fault_address)) {
		switch (ESR_ELx_EC(esr)) {
		case ESR_ELx_EC_DABT_LOW:
			/*
			 * These bits provide only information about the
			 * faulting instruction, which userspace knows already.
			 * We explicitly clear bits which are architecturally
			 * RES0 in case they are given meanings in future.
			 * We always report the ESR as if the fault was taken
			 * to EL1 and so ISV and the bits in ISS[23:14] are
			 * clear. (In fact it always will be a fault to EL1.)
			 */
			esr &= ESR_ELx_EC_MASK | ESR_ELx_IL |
				ESR_ELx_CM | ESR_ELx_WNR;
			esr |= ESR_ELx_FSC_FAULT;
			break;
		case ESR_ELx_EC_IABT_LOW:
			/*
			 * Claim a level 0 translation fault.
			 * All other bits are architecturally RES0 for faults
			 * reported with that DFSC value, so we clear them.
			 */
			esr &= ESR_ELx_EC_MASK | ESR_ELx_IL;
			esr |= ESR_ELx_FSC_FAULT;
			break;
		default:
			/*
			 * This should never happen (entry.S only brings us
			 * into this code for insn and data aborts from a lower
			 * exception level). Fail safe by not providing an ESR
			 * context record at all.
			 */
			WARN(1, "ESR 0x%lx is not DABT or IABT from EL0\n", esr);
			esr = 0;
			break;
		}
	}

	current->thread.fault_code = esr;
}

static void do_bad_area(unsigned long far, unsigned long esr,
			struct pt_regs *regs)
{
	unsigned long addr = untagged_addr(far);

	/*
	 * If we are in kernel mode at this point, we have no context to
	 * handle this fault with.
	 */
	if (user_mode(regs)) {
		const struct fault_info *inf = esr_to_fault_info(esr);

		set_thread_esr(addr, esr);
		arm64_force_sig_fault(inf->sig, inf->code, far, inf->name);
	} else {
		__do_kernel_fault(addr, esr, regs);
	}
}

static bool fault_from_pkey(struct vm_area_struct *vma, unsigned int mm_flags)
{
	if (!system_supports_poe())
		return false;

	/*
	 * We do not check whether an Overlay fault has occurred because we
	 * cannot make a decision based solely on its value:
	 *
	 * - If Overlay is set, a fault did occur due to POE, but it may be
	 *   spurious in those cases where we update POR_EL0 without ISB (e.g.
	 *   on context-switch). We would then need to manually check POR_EL0
	 *   against vma_pkey(vma), which is exactly what
	 *   arch_vma_access_permitted() does.
	 *
	 * - If Overlay is not set, we may still need to report a pkey fault.
	 *   This is the case if an access was made within a mapping but with no
	 *   page mapped, and POR_EL0 forbids the access (according to
	 *   vma_pkey()). Such access will result in a SIGSEGV regardless
	 *   because core code checks arch_vma_access_permitted(), but in order
	 *   to report the correct error code - SEGV_PKUERR - we must handle
	 *   that case here.
	 */
	return !arch_vma_access_permitted(vma,
			mm_flags & FAULT_FLAG_WRITE,
			mm_flags & FAULT_FLAG_INSTRUCTION,
			false);
}

static bool is_gcs_fault(unsigned long esr)
{
	if (!esr_is_data_abort(esr))
		return false;

	return ESR_ELx_ISS2(esr) & ESR_ELx_GCS;
}

static bool is_el0_instruction_abort(unsigned long esr)
{
	return ESR_ELx_EC(esr) == ESR_ELx_EC_IABT_LOW;
}

/*
 * Note: not valid for EL1 DC IVAC, but we never use that such that it
 * should fault. EL0 cannot issue DC IVAC (undef).
 */
static bool is_write_abort(unsigned long esr)
{
	return (esr & ESR_ELx_WNR) && !(esr & ESR_ELx_CM);
}

static bool is_invalid_gcs_access(struct vm_area_struct *vma, u64 esr)
{
	if (!system_supports_gcs())
		return false;

	if (unlikely(is_gcs_fault(esr))) {
		/* GCS accesses must be performed on a GCS page */
		if (!(vma->vm_flags & VM_SHADOW_STACK))
			return true;
	} else if (unlikely(vma->vm_flags & VM_SHADOW_STACK)) {
		/* Only GCS operations can write to a GCS page */
		return esr_is_data_abort(esr) && is_write_abort(esr);
	}

	return false;
}

/* charlotte v188: hhee's stage 2 denies EL0 instruction fetch on
 * some 2MB DRAM blocks (IABT_LOW with FSC 0x30-0x33 = S2 permission
 * encoding; S1 PERM_L is only 0x0C-0x0F).  The kernel cannot fix S2,
 * so: hardware-walk S1 for the PA, isolate the whole 2MB block from
 * the buddy allocator, copy the faulting page elsewhere, swap the S1
 * PTE, flush and let the fetch retry at the new PA.  PID 1 un-sticks
 * without userspace cooperation. */
#define CHARLOTTE_BAD_MAX	16
static unsigned long charlotte_bad_base[CHARLOTTE_BAD_MAX];
static unsigned long charlotte_bad_end[CHARLOTTE_BAD_MAX];
static int charlotte_bad_count;
static DEFINE_MUTEX(charlotte_bad_lock);

static bool charlotte_pa_in_bad_range(unsigned long pa)
{
	int i;

	for (i = 0; i < charlotte_bad_count; i++)
		if (pa >= charlotte_bad_base[i] && pa < charlotte_bad_end[i])
			return true;
	return false;
}

static bool charlotte_page_in_bad_range(struct page *p)
{
	return charlotte_pa_in_bad_range(page_to_phys(p));
}

/* v199: start_isolate_page_range removed from the fault path -- the
 * 16MB-margin isolation storm deadlocked the CPU at t=422s (v198).
 * The pre-verified pool plus bulk migration cover everything the
 * isolation was buying. */

#define CHARLOTTE_POOL_PAGES	2048
static struct page *charlotte_pool[CHARLOTTE_POOL_PAGES];
static int charlotte_pool_n;
static bool charlotte_pool_filled;
static unsigned long charlotte_pool_pa;
static int charlotte_pool_idx;
static int charlotte_pool_fail;
static bool charlotte_bulk_done;

/* v194: carve the migration pool at memblock time, next to the boot
 * image.  v193 proved the S2 denial is global (even the pinned boot
 * CPU), so the buddy-swept pool pages were denied too.  v171 proved
 * SOME region allows EL0 execution -- the prime candidate is the
 * kernel-image neighborhood hhee itself loaded. */
void __init charlotte_s2_pool_init(void)
{
	phys_addr_t size = 16UL << 20;
	phys_addr_t kern_end = __pa_symbol(_end);
	phys_addr_t pa = 0;

	/* v195: candidate windows, logged at fill time (the early
	 * ramlog lines wrap away within the first boot). */
	pa = memblock_phys_alloc_range(size, SZ_2M,
				       kern_end + SZ_2M,
				       kern_end + 64 * SZ_1M);
	if (!pa) {
		phys_addr_t end = memblock_end_of_DRAM();

		pa = memblock_phys_alloc_range(size, SZ_2M,
					       end - 6 * size, end - size);
		charlotte_pool_fail = 1;
	}
	if (!pa) {
		charlotte_pool_fail = 2;
		return;
	}
	charlotte_pool_pa = pa;
}

/* v191: draw migration targets from a one-shot pool of pages proven
 * outside the stage-2-denied ranges.  Buddy hands out the lowest free
 * pages in order, so a handful of retries never escapes an 18MB bad
 * window (v190 BAIL4 x17).  Pages falling inside known-bad ranges are
 * leaked, never freed -- freeing would put them back on the head of
 * the free list for the very next allocation. */
static void charlotte_fill_pool(void)
{
	int tries;

	if (charlotte_pool_filled)
		return;
	charlotte_pool_filled = true;

	charlotte_ckpt_hex("E:W POOLPA", charlotte_pool_pa);
	charlotte_ckpt_hex("E:W PFAIL ", charlotte_pool_fail);

	if (charlotte_pool_pa) {
		while (charlotte_pool_n < CHARLOTTE_POOL_PAGES &&
		       charlotte_pool_idx < (signed)(16UL << 20 >> PAGE_SHIFT)) {
			charlotte_pool[charlotte_pool_n++] = pfn_to_page(
				(charlotte_pool_pa >> PAGE_SHIFT) +
				charlotte_pool_idx++);
		}
		charlotte_ckpt_hex("E:W POOL ", charlotte_pool_n);
		return;
	}

	for (tries = 0; tries < 65536 &&
	     charlotte_pool_n < CHARLOTTE_POOL_PAGES; tries++) {
		struct page *p = alloc_pages(GFP_HIGHUSER_MOVABLE, 0);

		if (!p)
			break;
		if (charlotte_page_in_bad_range(p))
			continue;	/* leak: keep it out of buddy */
		charlotte_pool[charlotte_pool_n++] = p;
	}
	charlotte_ckpt_hex("E:W POOL ", charlotte_pool_n);
}

static struct page *charlotte_alloc_good_page(gfp_t gfp)
{
	charlotte_fill_pool();
	if (charlotte_pool_n > 0)
		return charlotte_pool[--charlotte_pool_n];
	return NULL;
}

/* v198/v199: one-shot bulk migration of every present page in the
 * vma.  The per-fault single-page crawl (v191..v197) advances ~1
 * page/s and never converges.  v199 runs in two phases per PMD table:
 * snapshot bad PTEs under the ptl, then swap them outside it (the
 * pool pop and page copy never sleep, but the v198 in-lock isolation
 * storm deadlocked the CPU at t=422s).  Caller holds mmap read lock;
 * PID 1 is single-threaded here. */
static void charlotte_bulk_migrate_vma(struct vm_area_struct *vma)
{
	struct mm_struct *mm = vma->vm_mm;
	unsigned long addr = vma->vm_start;
	int migrated = 0;

	while (addr < vma->vm_end) {
		pgd_t *pgdp = pgd_offset(mm, addr);
		p4d_t *p4dp = p4d_offset(pgdp, addr);
		pud_t *pudp = pud_offset(p4dp, addr);
		pmd_t *pmdp = pmd_offset(pudp, addr);
		pte_t *ptep, pte;
		spinlock_t *ptl = NULL;
		unsigned long e = min(addr + PMD_SIZE, vma->vm_end);
		unsigned long snap_addr[512];
		pte_t snap_pte[512];
		int snap_n = 0, i;

		ptep = pte_offset_map_lock(mm, pmdp, addr, &ptl);
		if (!ptep) {
			addr = e;
			continue;
		}
		for (; addr < e && snap_n < 512; addr += PAGE_SIZE, ptep++) {
			unsigned long pa;

			pte = READ_ONCE(*ptep);
			if (!pte_present(pte))
				continue;
			pa = __pte_to_phys(pte);
			if (!charlotte_pa_in_bad_range(pa))
				continue;
			snap_addr[snap_n] = addr;
			snap_pte[snap_n] = pte;
			snap_n++;
		}
		pte_unmap_unlock(ptep, ptl);

		for (i = 0; i < snap_n; i++) {
			unsigned long sa = snap_addr[i];
			unsigned long pa = __pte_to_phys(snap_pte[i]);
			struct page *old, *new;
			pte_t new_pte;

			new = charlotte_alloc_good_page(GFP_HIGHUSER_MOVABLE);
			if (!new)
				break;
			old = pfn_to_page(PHYS_PFN(pa));
			copy_highpage(new, old);
			ptep = pte_offset_map_lock(mm, pmdp, sa, &ptl);
			if (!ptep)
				break;
			pte = READ_ONCE(*ptep);
			if (pte_present(pte) && pte_val(pte) == pte_val(snap_pte[i])) {
				new_pte = __pte((pte_val(pte) & ~PTE_ADDR_LOW) |
						__phys_to_pte_val(page_to_phys(new)));
				set_pte_at(mm, sa, ptep, new_pte);
				migrated++;
			}
			pte_unmap_unlock(ptep, ptl);
		}
		if (migrated)
			flush_tlb_range(vma, vma->vm_start, vma->vm_end);
		addr = e;
	}
	charlotte_ckpt_hex("E:W BULK ", migrated);
}

static bool charlotte_s2_fixup(unsigned long far)
{
	struct mm_struct *mm = current->mm;
	unsigned long par, pa, pfn;
	struct vm_area_struct *vma;
	struct page *old, *new;
	pte_t *ptep, pte, new_pte;
	spinlock_t *ptl;
	pgd_t *pgdp;
	p4d_t *p4dp;
	pud_t *pudp;
	pmd_t *pmdp;
	bool ret = false;

	if (!mm)
		return false;

	/* S1-only hardware walk: the PA our PTE points at. */
	asm volatile("at s1e0r, %0" :: "r"(far));
	isb();
	par = read_sysreg(par_el1);
	if (par & 1)
		return false;		/* S1 itself faulted: not stage 2 */
	pa = par & PTE_ADDR_LOW;
	pfn = pa >> PAGE_SHIFT;
	charlotte_ckpt_hex("E:W PA  ", pa);

	if (mmap_read_lock_killable(mm))
		return false;
	vma = find_vma(mm, far);
	if (!vma || far < vma->vm_start || far >= vma->vm_end ||
	    !(vma->vm_flags & VM_EXEC)) {
		charlotte_ckpt_hex("E:W BAIL1", far);
		goto out_unlock;
	}

	pgdp = pgd_offset(mm, far);
	p4dp = p4d_offset(pgdp, far);
	pudp = pud_offset(p4dp, far);
	pmdp = pmd_offset(pudp, far);
	ptep = pte_offset_map_lock(mm, pmdp, far, &ptl);
	if (!ptep) {
		charlotte_ckpt_hex("E:W BAIL2", far);
		goto out_unlock;
	}
	pte = READ_ONCE(*ptep);
	if (!pte_present(pte) || pte_pfn(pte) != pfn) {
		charlotte_ckpt_hex("E:W BAIL3", pte_val(pte));
		goto out_pte;
	}

	new = charlotte_alloc_good_page(GFP_HIGHUSER_MOVABLE);
	if (!new) {
		charlotte_ckpt_hex("E:W BAIL4", 0);
		goto out_pte;
	}
	old = pfn_to_page(pfn);
	copy_highpage(new, old);

	/* Keep the faulting pte's permission bits, swap the PA. */
	new_pte = __pte((pte_val(pte) & ~PTE_ADDR_LOW) |
			__phys_to_pte_val(page_to_phys(new)));
	set_pte_at(mm, far, ptep, new_pte);
	flush_tlb_page(vma, far);
	charlotte_ckpt_hex("E:W MIGN ", page_to_phys(new));
	/* v191: leak the denied page on purpose -- put_page() would
	 * hand it back to buddy for the next innocent allocation. */
	ret = true;
out_pte:
	pte_unmap_unlock(ptep, ptl);
out_unlock:
	if (ret) {
		/* v201: bookkeep the denied 2MB block so the bulk sweep
		 * sees it (v199 dropped the isolator that used to feed
		 * this list -- pure bookkeeping, no page isolation). */
		unsigned long b = pa & ~((1UL << PMD_SHIFT) - 1);
		int i, known = 0;

		mutex_lock(&charlotte_bad_lock);
		for (i = 0; i < charlotte_bad_count; i++)
			if (b >= charlotte_bad_base[i] &&
			    b < charlotte_bad_end[i])
				known = 1;
		if (!known && charlotte_bad_count < CHARLOTTE_BAD_MAX) {
			charlotte_bad_base[charlotte_bad_count] = b;
			charlotte_bad_end[charlotte_bad_count] =
				b + (1UL << PMD_SHIFT);
			charlotte_bad_count++;
		}
		mutex_unlock(&charlotte_bad_lock);

		/* v200: run the bulk sweep on EVERY fixup -- fault_around
		 * keeps mapping fresh batches of denied file pages, so a
		 * one-shot bulk (v198) degenerates back into the crawl.
		 * Already-migrated pages fail pa_in_bad_range() in us. */
		charlotte_bulk_migrate_vma(vma);
	}
	mmap_read_unlock(mm);
	return ret;
}

static int __kprobes do_page_fault(unsigned long far, unsigned long esr,
				   struct pt_regs *regs)
{
	const struct fault_info *inf;
	struct mm_struct *mm = current->mm;
	vm_fault_t fault;
	vm_flags_t vm_flags;
	unsigned int mm_flags = FAULT_FLAG_DEFAULT;
	unsigned long addr = untagged_addr(far);
	struct vm_area_struct *vma;
	int si_code;
	int pkey = -1;

	if (kprobe_page_fault(regs, esr))
		return 0;

	/*
	 * If we're in an interrupt or have no user context, we must not take
	 * the fault.
	 */
	if (faulthandler_disabled() || !mm)
		goto no_context;

	if (user_mode(regs))
		mm_flags |= FAULT_FLAG_USER;

	/*
	 * vm_flags tells us what bits we must have in vma->vm_flags
	 * for the fault to be benign, __do_page_fault() would check
	 * vma->vm_flags & vm_flags and returns an error if the
	 * intersection is empty
	 */
	if (is_el0_instruction_abort(esr)) {
		/* It was exec fault */
		vm_flags = VM_EXEC;
		mm_flags |= FAULT_FLAG_INSTRUCTION;
	} else if (is_gcs_fault(esr)) {
		/*
		 * The GCS permission on a page implies both read and
		 * write so always handle any GCS fault as a write fault,
		 * we need to trigger CoW even for GCS reads.
		 */
		vm_flags = VM_WRITE;
		mm_flags |= FAULT_FLAG_WRITE;
	} else if (is_write_abort(esr)) {
		/* It was write fault */
		vm_flags = VM_WRITE;
		mm_flags |= FAULT_FLAG_WRITE;
	} else {
		/* It was read fault */
		vm_flags = VM_READ;
		/* Write implies read */
		vm_flags |= VM_WRITE;
		/* If EPAN is absent then exec implies read */
		if (!alternative_has_cap_unlikely(ARM64_HAS_EPAN))
			vm_flags |= VM_EXEC;
	}

	if (is_ttbr0_addr(addr) && is_el1_permission_fault(addr, esr, regs)) {
		if (is_el1_instruction_abort(esr))
			die_kernel_fault("execution of user memory",
					 addr, esr, regs);

		if (!insn_may_access_user(regs->pc, esr))
			die_kernel_fault("access to user memory outside uaccess routines",
					 addr, esr, regs);
	}

	if (is_pkvm_stage2_abort(esr)) {
		if (!user_mode(regs))
			goto no_context;
		arm64_force_sig_fault(SIGSEGV, SEGV_ACCERR, far, "stage-2 fault");
		return 0;
	}

	/* charlotte v188: hhee S2 permission aborts (FSC 0x30-0x33)
	 * arrive as plain IABT_LOW and are unfixable by handle_mm_fault.
	 * Migrate the page to a stage-2-allowed PA and retry instead of
	 * force-signalling PID 1 into its fatal loop. */
	if (user_mode(regs) && ESR_ELx_EC(esr) == ESR_ELx_EC_IABT_LOW) {
		unsigned long fsc = esr & ESR_ELx_FSC;

		if (fsc >= 0x30 && fsc <= 0x33 &&
		    charlotte_s2_fixup(far))
			return 0;
	}

	perf_sw_event(PERF_COUNT_SW_PAGE_FAULTS, 1, regs, addr);

	if (!(mm_flags & FAULT_FLAG_USER))
		goto lock_mmap;

	vma = lock_vma_under_rcu(mm, addr);
	if (!vma)
		goto lock_mmap;

	if (is_invalid_gcs_access(vma, esr)) {
		vma_end_read(vma);
		fault = 0;
		si_code = SEGV_ACCERR;
		goto bad_area;
	}

	if (!(vma->vm_flags & vm_flags)) {
		vma_end_read(vma);
		fault = 0;
		si_code = SEGV_ACCERR;
		count_vm_vma_lock_event(VMA_LOCK_SUCCESS);
		goto bad_area;
	}

	if (fault_from_pkey(vma, mm_flags)) {
		pkey = vma_pkey(vma);
		vma_end_read(vma);
		fault = 0;
		si_code = SEGV_PKUERR;
		count_vm_vma_lock_event(VMA_LOCK_SUCCESS);
		goto bad_area;
	}

	fault = handle_mm_fault(vma, addr, mm_flags | FAULT_FLAG_VMA_LOCK, regs);
	if (!(fault & (VM_FAULT_RETRY | VM_FAULT_COMPLETED)))
		vma_end_read(vma);

	if (!(fault & VM_FAULT_RETRY)) {
		count_vm_vma_lock_event(VMA_LOCK_SUCCESS);
		goto done;
	}
	count_vm_vma_lock_event(VMA_LOCK_RETRY);
	if (fault & VM_FAULT_MAJOR)
		mm_flags |= FAULT_FLAG_TRIED;

	/* Quick path to respond to signals */
	if (fault_signal_pending(fault, regs)) {
		if (!user_mode(regs))
			goto no_context;
		return 0;
	}
lock_mmap:

retry:
	vma = lock_mm_and_find_vma(mm, addr, regs);
	if (unlikely(!vma)) {
		fault = 0;
		si_code = SEGV_MAPERR;
		goto bad_area;
	}

	if (!(vma->vm_flags & vm_flags)) {
		mmap_read_unlock(mm);
		fault = 0;
		si_code = SEGV_ACCERR;
		goto bad_area;
	}

	if (fault_from_pkey(vma, mm_flags)) {
		pkey = vma_pkey(vma);
		mmap_read_unlock(mm);
		fault = 0;
		si_code = SEGV_PKUERR;
		goto bad_area;
	}

	fault = handle_mm_fault(vma, addr, mm_flags, regs);

	/* Quick path to respond to signals */
	if (fault_signal_pending(fault, regs)) {
		if (!user_mode(regs))
			goto no_context;
		return 0;
	}

	/* The fault is fully completed (including releasing mmap lock) */
	if (fault & VM_FAULT_COMPLETED)
		return 0;

	if (fault & VM_FAULT_RETRY) {
		mm_flags |= FAULT_FLAG_TRIED;
		goto retry;
	}
	mmap_read_unlock(mm);

done:
	/* Handle the "normal" (no error) case first. */
	if (likely(!(fault & VM_FAULT_ERROR)))
		return 0;

	si_code = SEGV_MAPERR;
bad_area:
	/*
	 * If we are in kernel mode at this point, we have no context to
	 * handle this fault with.
	 */
	if (!user_mode(regs))
		goto no_context;

	if (fault & VM_FAULT_OOM) {
		/*
		 * We ran out of memory, call the OOM killer, and return to
		 * userspace (which will retry the fault, or kill us if we got
		 * oom-killed).
		 */
		pagefault_out_of_memory();
		return 0;
	}

	inf = esr_to_fault_info(esr);
	set_thread_esr(addr, esr);
	if (fault & VM_FAULT_SIGBUS) {
		/*
		 * We had some memory, but were unable to successfully fix up
		 * this page fault.
		 */
		arm64_force_sig_fault(SIGBUS, BUS_ADRERR, far, inf->name);
	} else if (fault & (VM_FAULT_HWPOISON_LARGE | VM_FAULT_HWPOISON)) {
		unsigned int lsb;

		lsb = PAGE_SHIFT;
		if (fault & VM_FAULT_HWPOISON_LARGE)
			lsb = hstate_index_to_shift(VM_FAULT_GET_HINDEX(fault));

		arm64_force_sig_mceerr(BUS_MCEERR_AR, far, lsb, inf->name);
	} else {
		/*
		 * The pkey value that we return to userspace can be different
		 * from the pkey that caused the fault.
		 *
		 * 1. T1   : mprotect_key(foo, PAGE_SIZE, pkey=4);
		 * 2. T1   : set POR_EL0 to deny access to pkey=4, touches, page
		 * 3. T1   : faults...
		 * 4.    T2: mprotect_key(foo, PAGE_SIZE, pkey=5);
		 * 5. T1   : enters fault handler, takes mmap_lock, etc...
		 * 6. T1   : reaches here, sees vma_pkey(vma)=5, when we really
		 *	     faulted on a pte with its pkey=4.
		 */
		/* Something tried to access memory that out of memory map */
		if (si_code == SEGV_PKUERR)
			arm64_force_sig_fault_pkey(far, inf->name, pkey);
		else
			arm64_force_sig_fault(SIGSEGV, si_code, far, inf->name);
	}

	return 0;

no_context:
	__do_kernel_fault(addr, esr, regs);
	return 0;
}

static int __kprobes do_translation_fault(unsigned long far,
					  unsigned long esr,
					  struct pt_regs *regs)
{
	unsigned long addr = untagged_addr(far);

	if (is_ttbr0_addr(addr))
		return do_page_fault(far, esr, regs);

	do_bad_area(far, esr, regs);
	return 0;
}

static int do_alignment_fault(unsigned long far, unsigned long esr,
			      struct pt_regs *regs)
{
	if (IS_ENABLED(CONFIG_COMPAT_ALIGNMENT_FIXUPS) &&
	    compat_user_mode(regs))
		return do_compat_alignment_fixup(far, regs);
	do_bad_area(far, esr, regs);
	return 0;
}

static int do_bad(unsigned long far, unsigned long esr, struct pt_regs *regs)
{
	return 1; /* "fault" */
}

static int do_sea(unsigned long far, unsigned long esr, struct pt_regs *regs)
{
	const struct fault_info *inf;
	unsigned long siaddr;

	inf = esr_to_fault_info(esr);

	if (user_mode(regs) && apei_claim_sea(regs) == 0) {
		/*
		 * APEI claimed this as a firmware-first notification.
		 * Some processing deferred to task_work before ret_to_user().
		 */
		return 0;
	}

	if (esr & ESR_ELx_FnV) {
		siaddr = 0;
	} else {
		/*
		 * The architecture specifies that the tag bits of FAR_EL1 are
		 * UNKNOWN for synchronous external aborts. Mask them out now
		 * so that userspace doesn't see them.
		 */
		siaddr  = untagged_addr(far);
	}
	add_taint(TAINT_MACHINE_CHECK, LOCKDEP_STILL_OK);
	arm64_notify_die(inf->name, regs, inf->sig, inf->code, siaddr, esr);

	return 0;
}

static int do_tag_check_fault(unsigned long far, unsigned long esr,
			      struct pt_regs *regs)
{
	/*
	 * The architecture specifies that bits 63:60 of FAR_EL1 are UNKNOWN
	 * for tag check faults. Set them to corresponding bits in the untagged
	 * address if ARM64_MTE_FAR isn't supported.
	 * Otherwise, bits 63:60 of FAR_EL1 are not UNKNOWN.
	 */
	if (!cpus_have_cap(ARM64_MTE_FAR))
		far = (__untagged_addr(far) & ~MTE_TAG_MASK) | (far & MTE_TAG_MASK);

	do_bad_area(far, esr, regs);
	return 0;
}

static const struct fault_info fault_info[] = {
	{ do_bad,		SIGKILL, SI_KERNEL,	"ttbr address size fault"	},
	{ do_bad,		SIGKILL, SI_KERNEL,	"level 1 address size fault"	},
	{ do_bad,		SIGKILL, SI_KERNEL,	"level 2 address size fault"	},
	{ do_bad,		SIGKILL, SI_KERNEL,	"level 3 address size fault"	},
	{ do_translation_fault,	SIGSEGV, SEGV_MAPERR,	"level 0 translation fault"	},
	{ do_translation_fault,	SIGSEGV, SEGV_MAPERR,	"level 1 translation fault"	},
	{ do_translation_fault,	SIGSEGV, SEGV_MAPERR,	"level 2 translation fault"	},
	{ do_translation_fault,	SIGSEGV, SEGV_MAPERR,	"level 3 translation fault"	},
	{ do_page_fault,	SIGSEGV, SEGV_ACCERR,	"level 0 access flag fault"	},
	{ do_page_fault,	SIGSEGV, SEGV_ACCERR,	"level 1 access flag fault"	},
	{ do_page_fault,	SIGSEGV, SEGV_ACCERR,	"level 2 access flag fault"	},
	{ do_page_fault,	SIGSEGV, SEGV_ACCERR,	"level 3 access flag fault"	},
	{ do_page_fault,	SIGSEGV, SEGV_ACCERR,	"level 0 permission fault"	},
	{ do_page_fault,	SIGSEGV, SEGV_ACCERR,	"level 1 permission fault"	},
	{ do_page_fault,	SIGSEGV, SEGV_ACCERR,	"level 2 permission fault"	},
	{ do_page_fault,	SIGSEGV, SEGV_ACCERR,	"level 3 permission fault"	},
	{ do_sea,		SIGBUS,  BUS_OBJERR,	"synchronous external abort"	},
	{ do_tag_check_fault,	SIGSEGV, SEGV_MTESERR,	"synchronous tag check fault"	},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 18"			},
	{ do_sea,		SIGKILL, SI_KERNEL,	"level -1 (translation table walk)"	},
	{ do_sea,		SIGKILL, SI_KERNEL,	"level 0 (translation table walk)"	},
	{ do_sea,		SIGKILL, SI_KERNEL,	"level 1 (translation table walk)"	},
	{ do_sea,		SIGKILL, SI_KERNEL,	"level 2 (translation table walk)"	},
	{ do_sea,		SIGKILL, SI_KERNEL,	"level 3 (translation table walk)"	},
	{ do_sea,		SIGBUS,  BUS_OBJERR,	"synchronous parity or ECC error" },	// Reserved when RAS is implemented
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 25"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 26"			},
	{ do_sea,		SIGKILL, SI_KERNEL,	"level -1 synchronous parity error (translation table walk)"	},	// Reserved when RAS is implemented
	{ do_sea,		SIGKILL, SI_KERNEL,	"level 0 synchronous parity error (translation table walk)"	},	// Reserved when RAS is implemented
	{ do_sea,		SIGKILL, SI_KERNEL,	"level 1 synchronous parity error (translation table walk)"	},	// Reserved when RAS is implemented
	{ do_sea,		SIGKILL, SI_KERNEL,	"level 2 synchronous parity error (translation table walk)"	},	// Reserved when RAS is implemented
	{ do_sea,		SIGKILL, SI_KERNEL,	"level 3 synchronous parity error (translation table walk)"	},	// Reserved when RAS is implemented
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 32"			},
	{ do_alignment_fault,	SIGBUS,  BUS_ADRALN,	"alignment fault"		},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 34"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 35"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 36"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 37"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 38"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 39"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 40"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"level -1 address size fault"	},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 42"			},
	{ do_translation_fault,	SIGSEGV, SEGV_MAPERR,	"level -1 translation fault"	},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 44"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 45"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 46"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 47"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"TLB conflict abort"		},
	{ do_bad,		SIGKILL, SI_KERNEL,	"Unsupported atomic hardware update fault"	},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 50"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 51"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"implementation fault (lockdown abort)" },
	{ do_bad,		SIGBUS,  BUS_OBJERR,	"implementation fault (unsupported exclusive)" },
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 54"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 55"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 56"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 57"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 58" 			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 59"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 60"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"section domain fault"		},
	{ do_bad,		SIGKILL, SI_KERNEL,	"page domain fault"		},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 63"			},
};

void do_mem_abort(unsigned long far, unsigned long esr, struct pt_regs *regs)
{
	const struct fault_info *inf = esr_to_fault_info(esr);
	unsigned long addr = untagged_addr(far);

	{
		/* charlotte v186: rate-limited userspace IA/DA trace into
		 * the pstore ramlog (PID 1 IA storm diagnostics). */
		static atomic_t ua_limit = ATOMIC_INIT(64);

		if (user_mode(regs) &&
		    atomic_dec_if_positive(&ua_limit) >= 0)
			charlotte_ua_trace(far, esr, regs,
				ESR_ELx_EC(esr) == ESR_ELx_EC_IABT_LOW ?
					"IAB" : "DAB");

		/* charlotte v189: S2 permission aborts (FSC 0x30-0x33,
		 * the S2-only encoding range; S1 PERM_L is 0x0C-0x0F)
		 * land here unhandled because el0_ia calls do_mem_abort
		 * directly.  Migrate the page to a stage-2-allowed PA
		 * and retry instead of force-signalling PID 1.  NB: at
		 * s12e0r is UNDEF under hhee -- never use it here. */
		if (user_mode(regs) && ESR_ELx_EC(esr) == ESR_ELx_EC_IABT_LOW) {
			unsigned long fsc = esr & ESR_ELx_FSC;

			if (fsc >= 0x30 && fsc <= 0x33 &&
			    charlotte_s2_fixup(far))
				return;
		}
	}

	if (!inf->fn(far, esr, regs))
		return;

	if (!user_mode(regs))
		die_kernel_fault(inf->name, addr, esr, regs);

	/*
	 * At this point we have an unrecognized fault type whose tag bits may
	 * have been defined as UNKNOWN. Therefore we only expose the untagged
	 * address to the signal handler.
	 */
	arm64_notify_die(inf->name, regs, inf->sig, inf->code, addr, esr);
}
NOKPROBE_SYMBOL(do_mem_abort);

void do_sp_pc_abort(unsigned long addr, unsigned long esr, struct pt_regs *regs)
{
	arm64_notify_die("SP/PC alignment exception", regs, SIGBUS, BUS_ADRALN,
			 addr, esr);
}
NOKPROBE_SYMBOL(do_sp_pc_abort);

/*
 * Used during anonymous page fault handling.
 */
struct folio *vma_alloc_zeroed_movable_folio(struct vm_area_struct *vma,
						unsigned long vaddr)
{
	gfp_t flags = GFP_HIGHUSER_MOVABLE | __GFP_ZERO;

	/*
	 * If the page is mapped with PROT_MTE, initialise the tags at the
	 * point of allocation and page zeroing as this is usually faster than
	 * separate DC ZVA and STGM.
	 */
	if (vma->vm_flags & VM_MTE)
		flags |= __GFP_ZEROTAGS;

	return vma_alloc_folio(flags, 0, vma, vaddr);
}

bool tag_clear_highpages(struct page *page, int numpages, bool clear_pages)
{
	/*
	 * Check if MTE is supported and fall back to clear_highpage().
	 * get_huge_zero_folio() unconditionally passes __GFP_ZEROTAGS and
	 * post_alloc_hook() will invoke tag_clear_highpages().
	 */
	if (!system_supports_mte())
		return clear_pages;

	/* Newly allocated pages, shouldn't have been tagged yet */
	for (int i = 0; i < numpages; i++, page++) {
		WARN_ON_ONCE(!try_page_mte_tagging(page));
		if (clear_pages)
			mte_zero_clear_page_tags(page_address(page));
		else
			mte_clear_page_tags(page_address(page));
		set_page_mte_tagged(page);
	}
	return false;
}
