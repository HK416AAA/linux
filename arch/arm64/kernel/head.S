/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Low-level CPU initialisation
 * Based on arch/arm/kernel/head.S
 *
 * Copyright (C) 1994-2002 Russell King
 * Copyright (C) 2003-2012 ARM Ltd.
 * Authors:	Catalin Marinas <catalin.marinas@arm.com>
 *		Will Deacon <will.deacon@arm.com>
 */

#include <linux/linkage.h>
#include <linux/init.h>
#include <linux/pgtable.h>

#include <asm/asm_pointer_auth.h>
#include <asm/assembler.h>
#include <asm/boot.h>
#include <asm/bug.h>
#include <asm/ptrace.h>
#include <asm/asm-offsets.h>
#include <asm/cache.h>
#include <asm/cputype.h>
#include <asm/el2_setup.h>
#include <asm/elf.h>
#include <asm/image.h>
#include <asm/kernel-pgtable.h>
#include <asm/kvm_arm.h>
#include <asm/memory.h>
#include <asm/pgtable-hwdef.h>
#include <asm/page.h>
#include <asm/scs.h>
#include <asm/smp.h>
#include <asm/sysreg.h>
#include <asm/stacktrace/frame.h>
#include <asm/thread_info.h>
#include <asm/virt.h>

#include "efi-header.S"

#if (PAGE_OFFSET & 0x1fffff) != 0
#error PAGE_OFFSET must be at least 2MB aligned
#endif

/*
 * Kernel startup entry point.
 * ---------------------------
 *
 * The requirements are:
 *   MMU = off, D-cache = off, I-cache = on or off,
 *   x0 = physical address to the FDT blob.
 *
 * Note that the callee-saved registers are used for storing variables
 * that are useful before the MMU is enabled. The allocations are described
 * in the entry routines.
 */
	__HEAD
	/*
	 * DO NOT MODIFY. Image header expected by Linux boot-loaders.
	 */
	efi_signature_nop			// special NOP to identity as PE/COFF executable
	b	primary_entry			// branch to kernel start, magic
	.quad	0				// Image load offset from start of RAM, little-endian
	le64sym	_kernel_size_le			// Effective size of kernel image, little-endian
	le64sym	_kernel_flags_le		// Informative flags, little-endian
	.quad	0				// reserved
	.quad	0				// reserved
	.quad	0				// reserved
	.ascii	ARM64_IMAGE_MAGIC		// Magic number
	.long	.Lpe_header_offset		// Offset to the PE header.

	__EFI_PE_HEADER

	.section ".idmap.text","a"

	/*
	 * The following callee saved general purpose registers are used on the
	 * primary lowlevel boot path:
	 *
	 *  Register   Scope                      Purpose
	 *  x19        primary_entry() .. start_kernel()        whether we entered with the MMU on
	 *  x20        primary_entry() .. __primary_switch()    CPU boot mode
	 *  x21        primary_entry() .. start_kernel()        FDT pointer passed at boot in x0
	 */
	.macro CHARLOTTE_MAGIC, num
	movz	x6, #0x0000
	movk	x6, #0x2fcb, lsl #16	// PA 0x2fcb0000 = phase slots
	movz	w7, #(\num & 0xffff)
	movk	w7, #0xcafe, lsl #16	// 0xcafe000N
	str	w7, [x6, #(\num * 4)]
	.endm

	.macro CHARLOTTE_MAGIC_PM, num
	movz	x6, #0x0000
	movk	x6, #0x2fcb, lsl #16	// PA 0x2fcb0000 = phase slots
	movz	w7, #(\num & 0xffff)
	movk	w7, #0xcafe, lsl #16
	str	w7, [x6, #(\num * 4)]
	dc	cvac, x6			// push to DRAM: warm reset drops dirty lines
	dsb	sy
	.endm
SYM_CODE_START(primary_entry)
#ifdef CONFIG_CHARLOTTE_BBOXLOG
	/* v108: mask SError (PSTATE.A).  Every mainline boot eventually
	 * takes an asynchronous SError 0xbe000011 from still-secure
	 * hardware; when it lands early it panics the kernel before
	 * any data is collected (v107: 1.5s).  Masking buys the boot. */
	msr	daifset, #0x4
#endif
#ifdef CONFIG_CHARLOTTE_BBOXLOG
	/* Diagnostics run before a stack exists. Keep the bootloader's
	 * x0-x3 intact until preserve_boot_args records them. */
	mov	x24, x0
	mov	x25, x1
	mov	x26, x2
	mov	x27, x3
#endif
#ifdef CONFIG_CHARLOTTE_BBOXLOG
	/* charlotte probe: raw physical writes while the MMU is off
	 * (VA=PA).  Mark the console prz header valid with a size that
	 * covers the phase-magic slots at +0x10000, so stock exports
	 * everything even if we die before any C code runs.
	 * Registers: x0/x1/x7 only -- x2-x5 still hold boot args. */
	movz	x0, #0x0000
	movk	x0, #0x2fca, lsl #16	// PA 0x2fca0000 = console prz
	movz	w1, #0x4244
	movk	w1, #0x4347, lsl #16	// 0x43474244 "DBGC"
	str	w1, [x0]		// sig
	str	wzr, [x0, #4]		// start = 0
	movz	w1, #0x1000
	movk	w1, #0x0001, lsl #16	// size = 0x11000
	str	w1, [x0, #8]
	/* v100: the per-cluster A-G slot window at 0x2fcb01c0 is
	 * zeroed from C (first ramlog emit, pre-SMP) instead: a primary
	 * zero-loop here erases the slots written by the previous
	 * panic-loop cycle's secondaries (v99 all-zero mystery). */
	/* phase magic 1: kernel entry reached */
	movz	x0, #0x0000
	movk	x0, #0x2fcb, lsl #16	// PA 0x2fcb0000 = slot area
	movz	w1, #0x0001
	movk	w1, #0xcafe, lsl #16	// 0xcafe0001
	str	w1, [x0]
	str	wzr, [x0, #0xc0]	// +0xc0: secondary-entry counter (v59)
	/* zero every probe slot 0xc0..0x3fc in one loop: stale stock
	 * console residue must not read as "counter never executed"
	 * (v61 lesson).  The A-G window (0x1c0..0x6c0) is zeroed from
	 * C at banner time in THIS boot cycle instead (a head.S loop
	 * would erase the previous panic-loop cycle's slots). */
	movz	x1, #0x00c4
1:	str	wzr, [x0, x1]
	add	x1, x1, #4
	cmp	x1, #0x1c0
	b.ne	1b
	/* Install an emergency exception dumper BEFORE anything else can
	 * fault: any abort/SError in the instrumented window writes
	 * ESR/ELR/FAR into the prz dump slots and parks in WFI (until the
	 * LPM3 watchdog resets us), turning every silent death into an
	 * autopsy.  Runs pre-MMU (VA=PA) and stays valid post-MMU via the
	 * kernel image's idmap entry. */
	adrp	x0, charlotte_vectors
	add	x0, x0, :lo12:charlotte_vectors
	msr	vbar_el1, x0
	isb
#endif
#ifdef CONFIG_CHARLOTTE_BBOXLOG
	mov	x0, x24
	mov	x1, x25
	mov	x2, x26
	mov	x3, x27
#endif
	bl	record_mmu_state
	bl	preserve_boot_args
	CHARLOTTE_MAGIC 2

	adrp	x1, early_init_stack
	mov	sp, x1
	mov	x29, xzr
	adrp	x0, __pi_init_idmap_pg_dir
	mov	x1, xzr
	bl	__pi_create_init_idmap
#ifdef CONFIG_CHARLOTTE_BBOXLOG
	/* charlotte: make the pstore prz area reachable via VA=PA on
	 * TTBR0 after the MMU comes on (needed by the post-MMU phase
	 * magics and the exception dumper's cache clean).
	 *
	 * The kernel may be loaded anywhere (measured ~0x1e000000, i.e.
	 * inside pud[0]'s 1GB window), so pud[0] may already hold the
	 * image's own pmd table: in that case APPEND the entry instead
	 * of replacing pud[0] (v19 destroyed the idmap that way).
	 * pmd[0x17e] maps VA/PA 0x2fc00000..0x2fe00000 (2MB block).
	 */
	adrp	x1, __pi_init_idmap_pg_dir
	ldr	x2, [x1]			// pgd[0] -> pud table
	ubfx	x2, x2, #12, #36		// keep only PA bits [47:12]: table
					// descriptors carry UXNTable/PXNTable in
					// bits 59/60 -- leaving them in produced
					// 0x1000_0000_0210_1000-style addresses
					// and an address-size abort (v18/19/22)
	lsl	x2, x2, #12
	ldr	x3, [x2]			// pud[0] descriptor
	cbz	x3, 2f				// empty -> create table in prz tail
	ubfx	x2, x3, #12, #36		// same mask: strip table attr bits
	lsl	x2, x2, #12			// x2 = existing pmd table
	b	3f
2:	movz	x4, #0xf000
	movk	x4, #0x2fd1, lsl #16		// 0x2fd1f000: table in prz tail
	orr	x3, x4, #0x3			// table descriptor
	str	x3, [x2]			// pud[0] = new table (x2 = pud table)
	mov	x2, x4				// x2 = new pmd table
3:	movz	x3, #0x0701
	movk	x3, #0x2fc0, lsl #16		// 0x2fc00701: 2MB block, WB, AF
	movk	x3, #0x0040, lsl #48		// UXN (bit 54)
	str	x3, [x2, #0xbf0]			// pmd[0x17e]
#endif
.pushsection .init.text, "ax"
/*
 * charlotte emergency exception vectors: 16 slots, each branches to the
 * dumper.  Pre-MMU the CPU fetches handlers at PA (VBAR set to the
 * runtime address); post-MMU the idmap keeps VA=PA for the image.
 */
	.align 11
SYM_CODE_START(charlotte_vectors)
	.rept 16
	b	charlotte_exc_dump
	.space 0x80 - 4
	.endr
SYM_CODE_END(charlotte_vectors)

SYM_CODE_START(charlotte_exc_dump)
	mrs	x0, sctlr_el1		// MMU/C/A bits: decode the world we died in
	mrs	x1, esr_EL1
	mrs	x2, elr_EL1
	mrs	x3, far_EL1
	mrs	x4, spsr_el1
	movz	x9, #0x0000
	movk	x9, #0x2fcb, lsl #16	// PA 0x2fcb0000 + 0x100: dump slots
	add	x9, x9, #0x100
	stp	x0, x1, [x9, #0x40]	// +0x140: sctlr, +0x148: esr(again)
	stp	x3, x4, [x9, #16]	// +0x110: far, spsr
	str	x2, [x9, #8]		// +0x108: elr
	movz	w10, #0xcafe
	movk	w10, #0xdead, lsl #16	// 0xdeadcafe: exception taken
	str	w10, [x9, #32]
	mrs	x0, cntvct_EL0
	str	x0, [x9, #40]
	adrp	x11, charlotte_exc_dump
	add	x11, x11, :lo12:charlotte_exc_dump
	str	x11, [x9, #0x50]	// +0x150: runtime PA anchor -> voffset
	/* Pre-MMU stores are uncached and already reach DRAM; post-MMU
	 * the mapping is cacheable, so clean explicitly. */
	dc	cvac, x9
	add	x0, x9, #0x40
	dc	cvac, x0
	dsb	sy
1:	wfi
	b	1b
SYM_CODE_END(charlotte_exc_dump)
.popsection
	/*
	 * If the page tables have been populated with non-cacheable
	 * accesses (MMU disabled), invalidate those tables again to
	 * remove any speculatively loaded cache lines.
	 */
	cbnz	x19, 0f
	dmb     sy
	mov	x1, x0				// end of used region
	adrp    x0, __pi_init_idmap_pg_dir
	adr_l	x2, dcache_inval_poc
	blr	x2
	b	1f

	/*
	 * If we entered with the MMU and caches on, clean the ID mapped part
	 * of the primary boot code to the PoC so we can safely execute it with
	 * the MMU off.
	 */
0:	adrp	x0, __idmap_text_start
	adr_l	x1, __idmap_text_end
	adr_l	x2, dcache_clean_poc
	blr	x2

1:	CHARLOTTE_MAGIC 3
	mov	x0, x19
	bl	init_kernel_el			// w0=cpu_boot_mode
	CHARLOTTE_MAGIC 4
	mov	x20, x0

	/*
	 * The following calls CPU setup code, see arch/arm64/mm/proc.S for
	 * details.
	 * On return, the CPU will be ready for the MMU to be turned on and
	 * the TCR will have been set.
	 */
	bl	__cpu_setup			// initialise processor
	CHARLOTTE_MAGIC 5			// last pre-MMU checkpoint
	b	__primary_switch
SYM_CODE_END(primary_entry)

	__INIT
SYM_CODE_START_LOCAL(record_mmu_state)
	mrs	x19, CurrentEL
	cmp	x19, #CurrentEL_EL2
	mrs	x19, sctlr_el1
	b.ne	0f
	mrs	x19, sctlr_el2
0:
CPU_LE( tbnz	x19, #SCTLR_ELx_EE_SHIFT, 1f	)
CPU_BE( tbz	x19, #SCTLR_ELx_EE_SHIFT, 1f	)
	tst	x19, #SCTLR_ELx_C		// Z := (C == 0)
	and	x19, x19, #SCTLR_ELx_M		// isolate M bit
	csel	x19, xzr, x19, eq		// clear x19 if Z
	ret

	/*
	 * Set the correct endianness early so all memory accesses issued
	 * before init_kernel_el() occur in the correct byte order. Note that
	 * this means the MMU must be disabled, or the active ID map will end
	 * up getting interpreted with the wrong byte order.
	 */
1:	eor	x19, x19, #SCTLR_ELx_EE
	bic	x19, x19, #SCTLR_ELx_M
	b.ne	2f
	pre_disable_mmu_workaround
	msr	sctlr_el2, x19
	b	3f
2:	pre_disable_mmu_workaround
	msr	sctlr_el1, x19
3:	isb
	mov	x19, xzr
	ret
SYM_CODE_END(record_mmu_state)

/*
 * Preserve the arguments passed by the bootloader in x0 .. x3
 */
SYM_CODE_START_LOCAL(preserve_boot_args)
	mov	x21, x0				// x21=FDT

	adr_l	x0, boot_args			// record the contents of
	stp	x21, x1, [x0]			// x0 .. x3 at kernel entry
	stp	x2, x3, [x0, #16]

	cbnz	x19, 0f				// skip cache invalidation if MMU is on
	dmb	sy				// needed before dc ivac with
						// MMU off

	add	x1, x0, #0x20			// 4 x 8 bytes
	b	dcache_inval_poc		// tail call
0:	str_l   x19, mmu_enabled_at_boot, x0
	ret
SYM_CODE_END(preserve_boot_args)

	/*
	 * Initialize CPU registers with task-specific and cpu-specific context.
	 *
	 * Create a final frame record at task_pt_regs(current)->stackframe, so
	 * that the unwinder can identify the final frame record of any task by
	 * its location in the task stack. We reserve the entire pt_regs space
	 * for consistency with user tasks and kthreads.
	 */
	.macro	init_cpu_task tsk, tmp1, tmp2
	msr	sp_el0, \tsk

	ldr	\tmp1, [\tsk, #TSK_STACK]
	add	sp, \tmp1, #THREAD_SIZE
	sub	sp, sp, #PT_REGS_SIZE

	stp	xzr, xzr, [sp, #S_STACKFRAME]
	mov	\tmp1, #FRAME_META_TYPE_FINAL
	str	\tmp1, [sp, #S_STACKFRAME_TYPE]
	add	x29, sp, #S_STACKFRAME

	scs_load_current

	adr_l	\tmp1, __per_cpu_offset
	ldr	w\tmp2, [\tsk, #TSK_TI_CPU]
	ldr	\tmp1, [\tmp1, \tmp2, lsl #3]
	set_this_cpu_offset \tmp1
	.endm

/*
 * The following fragment of code is executed with the MMU enabled.
 *
 *   x0 = __pa(KERNEL_START)
 */
SYM_FUNC_START_LOCAL(__primary_switched)
	CHARLOTTE_MAGIC_PM 6
	adr_l	x4, init_task
	init_cpu_task x4, x5, x6

	adr_l	x8, vectors			// load VBAR_EL1 with virtual
	msr	vbar_el1, x8			// vector table address
	isb

	stp	x29, x30, [sp, #-16]!
	mov	x29, sp

	str_l	x21, __fdt_pointer, x5		// Save FDT pointer

	adrp	x4, _text			// Save the offset between
	sub	x4, x4, x0			// the kernel virtual and
	str_l	x4, kimage_voffset, x5		// physical mappings

	mov	x0, x20
	bl	set_cpu_boot_mode_flag

#if defined(CONFIG_KASAN_GENERIC) || defined(CONFIG_KASAN_SW_TAGS)
	bl	kasan_early_init
#endif
	mov	x0, x20
	bl	finalise_el2			// Prefer VHE if possible
	CHARLOTTE_MAGIC_PM 7

	/* v54 lesson (do not retry blindly): an M-free SCTLR_EL1 write
	 * setting only C|I, issued HERE (post-MMU, post-finalise_el2),
	 * kills the CPU instantly and silently -- magic7 fires, magic8
	 * never does, and no exception reaches the prz dumper.  The v28
	 * "M-free writes pass" model holds pre-MMU only.  Caches stay
	 * off; boot just runs slow until an hhee-aware path is found. */
	ldp	x29, x30, [sp], #16
	bl	start_kernel
SYM_FUNC_END(__primary_switched)

/*
 * end early head section, begin head code that is also used for
 * hotplug and needs to have the same protections as the text region
 */
	.section ".idmap.text","a"

/*
 * Starting from EL2 or EL1, configure the CPU to execute at the highest
 * reachable EL supported by the kernel in a chosen default state. If dropping
 * from EL2 to EL1, configure EL2 before configuring EL1.
 *
 * Since we cannot always rely on ERET synchronizing writes to sysregs (e.g. if
 * SCTLR_ELx.EOS is clear), we place an ISB prior to ERET.
 *
 * Returns either BOOT_CPU_MODE_EL1 or BOOT_CPU_MODE_EL2 in x0 if
 * booted in EL1 or EL2 respectively, with the top 32 bits containing
 * potential context flags. These flags are *not* stored in __boot_cpu_mode.
 *
 * x0: whether we are being called from the primary boot path with the MMU on
 */
SYM_FUNC_START(init_kernel_el)
	mrs	x1, CurrentEL
	cmp	x1, #CurrentEL_EL2
	b.eq	init_el2

SYM_INNER_LABEL(init_el1, SYM_L_LOCAL)
	mov_q	x0, INIT_SCTLR_EL1_MMU_OFF
	pre_disable_mmu_workaround
	msr	sctlr_el1, x0
	isb
	mov_q	x0, INIT_PSTATE_EL1
	msr	spsr_el1, x0
	msr	elr_el1, lr
	mov	w0, #BOOT_CPU_MODE_EL1
	eret

SYM_INNER_LABEL(init_el2, SYM_L_LOCAL)
	msr	elr_el2, lr

	// clean all HYP code to the PoC if we booted at EL2 with the MMU on
	cbz	x0, 0f
	adrp	x0, __hyp_idmap_text_start
	adr_l	x1, __hyp_text_end
	adr_l	x2, dcache_clean_poc
	blr	x2

	mov_q	x0, INIT_SCTLR_EL2_MMU_OFF
	pre_disable_mmu_workaround
	msr	sctlr_el2, x0
	isb
0:

	init_el2_hcr	HCR_HOST_NVHE_FLAGS | HCR_ATA
	init_el2_state

	/* Hypervisor stub */
	adr_l	x0, __hyp_stub_vectors
	msr	vbar_el2, x0
	isb

	mov_q	x1, INIT_SCTLR_EL1_MMU_OFF

	mrs	x0, hcr_el2
	and	x0, x0, #HCR_E2H
	cbz	x0, 2f

	/* Set a sane SCTLR_EL1, the VHE way */
	msr_s	SYS_SCTLR_EL12, x1
	mov	x2, #BOOT_CPU_FLAG_E2H
	b	3f

2:
	msr	sctlr_el1, x1
	mov	x2, xzr
3:
	mov	x0, #INIT_PSTATE_EL1
	msr	spsr_el2, x0

	mov	w0, #BOOT_CPU_MODE_EL2
	orr	x0, x0, x2
	eret
SYM_FUNC_END(init_kernel_el)

	/*
	 * This provides a "holding pen" for platforms to hold all secondary
	 * cores are held until we're ready for them to initialise.
	 */
SYM_FUNC_START(secondary_holding_pen)
	mov	x0, xzr
	bl	init_kernel_el			// w0=cpu_boot_mode
	mrs	x2, mpidr_el1
	mov_q	x1, MPIDR_HWID_BITMASK
	and	x2, x2, x1
	adr_l	x3, secondary_holding_pen_release
pen:	ldr	x4, [x3]
	cmp	x4, x2
	b.eq	secondary_startup
	wfe
	b	pen
SYM_FUNC_START(secondary_entry)
#ifdef CONFIG_CHARLOTTE_BBOXLOG
	msr	daifset, #0x4			/* v108: mask SError */
	/* v137: the v122 entry vbar install was cut for idmap space;
	 * the tail re-arm (post-dsb) covers the critical window. */
	/* charlotte v59 probe: count PSCI-delivered secondary entries in
	 * the prz slot at PA 0x2fcb00c0.  MMU is off here (VA=PA), same
	 * contract as the primary-entry probes. */
	movz	x1, #0x0000
	movk	x1, #0x2fcb, lsl #16
	ldr	w2, [x1, #0xc0]
	add	w2, w2, #1
	str	w2, [x1, #0xc0]
	mov	x0, xzr
#endif
	bl	init_kernel_el			// w0=cpu_boot_mode
#ifdef CONFIG_CHARLOTTE_BBOXLOG
	/* v61: step counter -- MMU is still off here, VA=PA stores are
 * always safe.  v60 showed cores reach the entry (+0xc0=11) but
 * never the post-__enable_mmu block; bisect the gap. */
	movz	x1, #0x0000
	movk	x1, #0x2fcb, lsl #16
	ldr	w2, [x1, #0xc4]
	add	w2, w2, #1
	str	w2, [x1, #0xc4]			// past init_kernel_el
#endif
	b	secondary_startup
SYM_FUNC_END(secondary_entry)

#ifdef CONFIG_CHARLOTTE_BBOXLOG
/*
 * v109: hotplug-cycle trampoline.  hhee's SCTLR judge drops M=1
 * writes on cores whose per-cpu block is still "managed" with
 * stock-era snapshots; the vendor lifecycle clears that state on
 * PSCI CPU_OFF (cpu1-3 escape via idle hotplug, cpu4-11 never got
 * off'd).  Bring each stuck core here first: it immediately calls
 * PSCI CPU_OFF, then the kernel cpu_ons it again into the normal
 * entry.  Runs with the MMU off at PA==VA inside the idmap.
 */
SYM_CODE_START(charlotte_cycle_pen)
	/* v113 suspend pen: mark entry, arm the local physical timer as
	 * the wake source (100ms), enable its PPI at this core's own
	 * redistributor, unmask IRQs, then PSCI CPU_SUSPEND power-down.
	 * If hhee's idle path clears the per-core "managed" police the
	 * way erecovery idle does for cpu1-3, the core resumes here,
	 * marks 0xbeef, CPU_OFFs itself and becomes re-on-able clean. */
	/* v152: pen shrunk to mark + CPU_OFF (the v113 suspend/timer
	 * dance never woke once; 140B of idmap space reclaimed for
	 * the judge scanner). */
	movz	x1, #0x01f0
	movk	x1, #0x2fcb, lsl #16
	mrs	x9, mpidr_el1
	ubfx	x9, x9, #8, #4		// Aff1 (slot index)
	add	x1, x1, x9, lsl #2
	mov	w10, #0xfeed
	orr	w10, w10, w9
	str	w10, [x1]			// pen entered
	movz	x0, #0x0002
	movk	x0, #0x8400, lsl #16		// PSCI CPU_OFF
	hvc	#0
4:	wfe
	b	4b
SYM_CODE_END(charlotte_cycle_pen)
#endif

SYM_FUNC_START_LOCAL(secondary_startup)
	/*
	 * Common entry point for secondary CPUs.
	 */
	mov	x20, x0				// preserve boot mode

#ifdef CONFIG_CHARLOTTE_BBOXLOG
	/* Per-CPU diagnostics: idx = Aff1*2+Aff0, base 0x2fcb01c0,
	 * stride 128B.  All fields stay inside their own slot:
	 * +8 ISAR1, +16 TTBR1, +24 TCR, +48 MAIR, +56 trial,
	 * +72 actual SCTLR, +80 planned SCTLR, +88/+92/+96 F1/F2/F3,
	 * +104 entry SCTLR, +120 high-VA PAR. */
	movz	x1, #0x01c0
	movk	x1, #0x2fcb, lsl #16		// PA 0x2fcb01c0 = slot window
	mrs	x9, mpidr_el1
	ubfx	x10, x9, #8, #4			// Aff1
	and	x9, x9, #1			// Aff0
	add	x9, x9, x10, lsl #1		// idx = Aff1*2 + Aff0
	mov	x10, #128
	mul	x9, x9, x10			// byte offset
	add	x10, x1, x9			// x10 = this core's slot
	mrs	x5, sctlr_el1
	str	x5, [x10, #104]		// pristine SCTLR, inside this 128B slot
	/* v170: all cores use __cpu_setup's valid 4K translation state.
	 * Hot Taishan cores retain their entry control bits at M-enable;
	 * in particular, do not clear an already-set SCTLR.SPAN. */
	mov	w11, #0x171
	str	w11, [x10, #56]
	mrs	x11, id_aa64isar1_el1
	str	x11, [x10, #8]
#endif

#ifdef CONFIG_ARM64_VA_BITS_52
alternative_if ARM64_HAS_VA52
	bl	__cpu_secondary_check52bitva
alternative_else_nop_endif
#endif

	bl	__cpu_setup			// initialise processor
#ifdef CONFIG_CHARLOTTE_BBOXLOG
	movz	x1, #0x0000
	movk	x1, #0x2fcb, lsl #16
	ldr	w2, [x1, #0xcc]
	add	w2, w2, #1
	str	w2, [x1, #0xcc]			// past __cpu_setup
#endif
	/* v64 recipe: write everything except M, then M-only. */
	bic	x0, x0, #SCTLR_ELx_M
	adrp	x1, swapper_pg_dir
	adrp	x2, idmap_pg_dir
	bl	__enable_mmu
	/* Reconstruct the slot pointer after __enable_mmu clobbers x9-x11. */
	mrs	x2, mpidr_el1
	ubfx	x3, x2, #8, #4
	and	x2, x2, #1
	add	x2, x2, x3, lsl #1		// idx = Aff1*2 + Aff0 (v151)
	movz	x7, #0x01c0
	movk	x7, #0x2fcb, lsl #16
	add	x7, x7, x2, lsl #7		// per-core slot base
	ldr	x11, =0xfeedfacefeedface
	mrs	x11, tcr_el1
	str	x11, [x7, #24]
	ldr	x11, =0xfeedfacefeedface
	mrs	x11, ttbr1_el1
	str	x11, [x7, #16]
	ldr	x11, =0xfeedfacefeedface
	mrs	x11, mair_el1
	str	x11, [x7, #48]
	cmp	x2, #8
	b.lo	24f
	ldr	x0, [x7, #104]
	orr	x0, x0, #SCTLR_ELx_M
	b	25f
24:	ldr	x0, =0x30500801			// preserve the working A510 path
25:
	str	x0, [x7, #80]			// L: planned value (pre-msr)
	msr	sctlr_el1, x0
	movz	w9, #0xF111
	str	w9, [x7, #88]			// F1: trap returned
	isb
	movz	w9, #0xF222
	str	w9, [x7, #92]			// F2: post-isb
	dsb	sy
	movz	w9, #0xF333
	str	w9, [x7, #96]			// F3: post-dsb
	ldr	x11, =0xfeedfacefeedface
	mrs	x11, sctlr_el1
	str	x11, [x7, #72]
	ldr	x11, =__secondary_switched
	at	s1e1r, x11
	isb
	ldr	x11, =0xfeedfacefeedface
	mrs	x11, par_el1
	str	x11, [x7, #120]
	/* re-arm the emergency dumper right before the jump */
	adrp	x0, charlotte_vectors
	add	x0, x0, :lo12:charlotte_vectors
	msr	vbar_el1, x0
	isb
	ldr	x8, =__secondary_switched
	br	x8
SYM_FUNC_END(secondary_startup)

	.text
SYM_FUNC_START_LOCAL(__secondary_switched)
#ifdef CONFIG_CHARLOTTE_BBOXLOG
	movz	x6, #0x0000
	movk	x6, #0x2fcb, lsl #16
	ldr	w2, [x6, #0x160]
	add	w2, w2, #1
	str	w2, [x6, #0x160]		// reached __secondary_switched (v66)
	str	w2, [x6, #0x164]		// v140 NUKE: alive 1 instr past counter
#endif
	/* v152: the v141/v142 S-marker probes are dropped (banked;
	 * idmap space needed for the judge scanner). */
	mov	x0, x20
	bl	set_cpu_boot_mode_flag

	mov	x0, x20
	bl	finalise_el2

	str_l	xzr, __early_cpu_boot_status, x3
	adr_l	x5, vectors
	msr	vbar_el1, x5
	isb

	adr_l	x0, secondary_data
	ldr	x2, [x0, #CPU_BOOT_TASK]
	cbz	x2, __secondary_too_slow

	init_cpu_task x2, x1, x3

#ifdef CONFIG_ARM64_PTR_AUTH
	ptrauth_keys_init_cpu x2, x3, x4, x5
#endif

	bl	secondary_start_kernel
	ASM_BUG()
SYM_FUNC_END(__secondary_switched)

SYM_FUNC_START_LOCAL(__secondary_too_slow)
	wfe
	wfi
	b	__secondary_too_slow
SYM_FUNC_END(__secondary_too_slow)

/*
 * Sets the __boot_cpu_mode flag depending on the CPU boot mode passed
 * in w0. See arch/arm64/include/asm/virt.h for more info.
 */
SYM_FUNC_START_LOCAL(set_cpu_boot_mode_flag)
	adr_l	x1, __boot_cpu_mode
	cmp	w0, #BOOT_CPU_MODE_EL2
	b.ne	1f
	add	x1, x1, #4
1:	str	w0, [x1]			// Save CPU boot mode
	ret
SYM_FUNC_END(set_cpu_boot_mode_flag)

/*
 * The booting CPU updates the failed status @__early_cpu_boot_status,
 * with MMU turned off.
 *
 * update_early_cpu_boot_status tmp, status
 *  - Corrupts tmp1, tmp2
 *  - Writes 'status' to __early_cpu_boot_status and makes sure
 *    it is committed to memory.
 */

	.macro	update_early_cpu_boot_status status, tmp1, tmp2
	mov	\tmp2, #\status
	adr_l	\tmp1, __early_cpu_boot_status
	str	\tmp2, [\tmp1]
	dmb	sy
	dc	ivac, \tmp1			// Invalidate potentially stale cache line
	.endm

/*
 * Enable the MMU.
 *
 *  x0  = SCTLR_EL1 value for turning on the MMU.
 *  x1  = TTBR1_EL1 value
 *  x2  = ID map root table address
 *
 * Returns to the caller via x30/lr. This requires the caller to be covered
 * by the .idmap.text section.
 *
 * Checks if the selected granule size is supported by the CPU.
 * If it isn't, park the CPU
 */
	.section ".idmap.text","a"
SYM_FUNC_START(__enable_mmu)
	mrs	x3, ID_AA64MMFR0_EL1
	ubfx	x3, x3, #ID_AA64MMFR0_EL1_TGRAN_SHIFT, 4
	cmp     x3, #ID_AA64MMFR0_EL1_TGRAN_SUPPORTED_MIN
	b.lt    __no_granule_support
	cmp     x3, #ID_AA64MMFR0_EL1_TGRAN_SUPPORTED_MAX
	b.gt    __no_granule_support
	phys_to_ttbr x2, x2
#ifdef CONFIG_CHARLOTTE_BBOXLOG
	/* v63: v62 showed all 11 cores pass __cpu_setup but only
	 * cluster0's 3 survive __enable_mmu.  Count cores past each
	 * sysreg write (all pre-MMU stores; set_sctlr is dropped on
	 * survivors).  +0xf0 records the last MPIDR to reach set_sctlr.
	 * The primary passes here too, so expect +1 on every counter. */
	movz	x9, #0x0000
	movk	x9, #0x2fcb, lsl #16
	ldr	w10, [x9, #0xe4]
	add	w10, w10, #1
	str	w10, [x9, #0xe4]			// past TGRAN check
#endif
	msr	ttbr0_el1, x2			// load TTBR0
#ifdef CONFIG_CHARLOTTE_BBOXLOG
	ldr	w10, [x9, #0xe8]
	add	w10, w10, #1
	str	w10, [x9, #0xe8]			// past ttbr0 write
#endif
	load_ttbr1 x1, x1, x3
#ifdef CONFIG_CHARLOTTE_BBOXLOG
	ldr	w10, [x9, #0xec]
	add	w10, w10, #1
	str	w10, [x9, #0xec]			// past ttbr1 write
	mrs	x11, mpidr_el1
	str	w11, [x9, #0xf0]			// last MPIDR to get here
#endif

	set_sctlr_el1	x0

#ifdef CONFIG_CHARLOTTE_BBOXLOG
	ldr	w10, [x9, #0xf4]
	add	w10, w10, #1
	str	w10, [x9, #0xf4]			// survived set_sctlr
#endif
	ret
SYM_FUNC_END(__enable_mmu)

#ifdef CONFIG_ARM64_VA_BITS_52
SYM_FUNC_START(__cpu_secondary_check52bitva)
#ifndef CONFIG_ARM64_LPA2
	mrs_s	x0, SYS_ID_AA64MMFR2_EL1
	and	x0, x0, ID_AA64MMFR2_EL1_VARange_MASK
	cbnz	x0, 2f
#else
	mrs	x0, id_aa64mmfr0_el1
	sbfx	x0, x0, #ID_AA64MMFR0_EL1_TGRAN_SHIFT, 4
	cmp	x0, #ID_AA64MMFR0_EL1_TGRAN_LPA2
	b.ge	2f
#endif

	update_early_cpu_boot_status \
		CPU_STUCK_IN_KERNEL | CPU_STUCK_REASON_52_BIT_VA, x0, x1
1:	wfe
	wfi
	b	1b

2:	ret
SYM_FUNC_END(__cpu_secondary_check52bitva)
#endif

SYM_FUNC_START_LOCAL(__no_granule_support)
	/* Indicate that this CPU can't boot and is stuck in the kernel */
	update_early_cpu_boot_status \
		CPU_STUCK_IN_KERNEL | CPU_STUCK_REASON_NO_GRAN, x1, x2
1:
	wfe
	wfi
	b	1b
SYM_FUNC_END(__no_granule_support)

SYM_FUNC_START_LOCAL(__primary_switch)
	adrp	x1, reserved_pg_dir
	adrp	x2, __pi_init_idmap_pg_dir
	bl	__enable_mmu
	mrs	x1, sctlr_el1
	movz	x6, #0x0000
	movk	x6, #0x2fcb, lsl #16		// prz slot area
	str	x1, [x6, #0x58]			// raw SCTLR_EL1 readback


	/* ---- charlotte hhee handshake experiment (v28) ----
	 * Vendor head.S calls hkip_tvm_enable (source withheld) between
	 * __cpu_setup and __primary_switch: hhee holds HCR_EL2.TVM and
	 * swallows EL1 MMU-register writes until the kernel performs this
	 * handshake.  HVC clobbers x0-x17: x6 is re-materialised after
	 * every call.  x19+ survive per SMCCC. */
	movz	x6, #0x0000
	movk	x6, #0x2fcb, lsl #16
	movz	x0, #0x108a
	movk	x0, #0xc600, lsl #16		// HHEE_HVC_ENABLE_TVM
	mov	x1, #0
	hvc	#0
	movz	x6, #0x0000
	movk	x6, #0x2fcb, lsl #16
	str	x0, [x6, #0x78]			// TVM handshake result

	/* ---- v29: bisect which SCTLR values survive hhee's filter ----
	 * Model: hhee merges the written value but forces M=0 (or drops
	 * writes containing bits it does not know).  Try M with growing
	 * bit sets; the first that sticks turns the MMU on right here
	 * (ttbr0 was already loaded by the first __enable_mmu). */
	/* A: M only */
	movz	x0, #0x0801
	movk	x0, #0x3050, lsl #16
	msr	sctlr_el1, x0
	isb
	mrs	x1, sctlr_el1
	str	x1, [x6, #0x90]
	tbnz	x1, #0, 8f
	CHARLOTTE_MAGIC 19			// A dropped
	/* B: M|C */
	movz	x0, #0x0805
	movk	x0, #0x3050, lsl #16
	msr	sctlr_el1, x0
	isb
	mrs	x1, sctlr_el1
	str	x1, [x6, #0x98]
	tbnz	x1, #0, 8f
	CHARLOTTE_MAGIC 20			// B dropped
	/* C: M|C|I */
	movz	x0, #0x1805
	movk	x0, #0x3050, lsl #16
	msr	sctlr_el1, x0
	isb
	mrs	x1, sctlr_el1
	str	x1, [x6, #0xa0]
	tbnz	x1, #0, 8f
	CHARLOTTE_MAGIC 21			// C dropped
	/* D: M|C|I + SPAN/IESB-era bits */
	movz	x0, #0x0807
	movk	x0, #0x30d0, lsl #16
	msr	sctlr_el1, x0
	isb
	mrs	x1, sctlr_el1
	str	x1, [x6, #0xa8]
	tbnz	x1, #0, 8f
	CHARLOTTE_MAGIC 22			// every M variant dropped
8:	ic	iallu				/* M stuck: safe instruction path */
	dsb	sy
	isb
	/* flow continues into the original __primary_switch body */
	adrp	x1, early_init_stack
	mov	sp, x1
	mov	x29, xzr
	mov	x0, x20				// pass the full boot status
	mov	x1, x21				// pass the FDT
	bl	__pi_early_map_kernel		// Map and relocate the kernel

	ldr	x8, =__primary_switched
	adrp	x0, KERNEL_START		// __pa(KERNEL_START)
	br	x8
SYM_FUNC_END(__primary_switch)
