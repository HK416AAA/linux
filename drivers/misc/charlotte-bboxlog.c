// SPDX-License-Identifier: GPL-2.0
/*
 * charlotte-ramlog -- boot checkpoints written straight into the vendor
 * pstore console zone, in persistent_ram_buffer format, so that the
 * stock kernel exports them as /sys/fs/pstore/console-ramoops-0 on the
 * next (stock/Magisk) boot.
 *
 * persistent_ram_buffer (fs/pstore/ram_core.c, identical in vendor
 * 5.10 and mainline):
 *   u32 sig; atomic_t start; atomic_t size; u8 data[];
 * console/dmesg zones are created with sig argument 0, so the stored
 * signature is PERSISTENT_RAM_SIG (0x43474244 "DBGC") unchanged.  A
 * buffer is accepted as valid old data when sig matches, size <= zone
 * size and start <= size.
 *
 * Write ordering: data first, then start/size, sig last, so a crash
 * mid-write never leaves a valid header pointing at garbage.
 *
 * Fixmap lifecycle: boot-CPU-only writes map and unmap per call.
 * An early initcall installs a permanent mapping before SMP; all
 * subsequent appends are serialized by ramlog_lock.
 *
 * Vendor DTB (charlotte_0.dts): /reserved-memory/pstore-mem
 * @0x2fc40000 1MB; /ramoops: record-size 0x20000, console-size
 * 0x80000, pmsg-size 0x20000, ecc 0.
 * Zone math (identical vendor 5.10 / mainline fs/pstore/ram.c):
 *   dump_mem_sz = 1MB - 0x80000 - 0 - 0x20000 = 0x60000
 *   -> 3 dmesg slots [0, 0x60000), console prz at +0x60000 (size
 *   0x80000), pmsg at +0xE0000.
 */

#include <linux/init.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/kernel.h>
#include <linux/memory.h>
#include <linux/panic_notifier.h>
#include <linux/reboot.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <linux/spinlock.h>
#include <linux/sched.h>
#include <linux/jiffies.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/sysfs.h>
#include <asm/ptrace.h>

#define RAMLOG_PA	(0x2fc40000UL + 0x60000UL)
#define RAMLOG_SIG	0x43474244U
#define RAMLOG_CAP	0x12000U	/* C log area; record0 mirror maps separately */
#define RAMLOG_HDR	12U		/* sig + start + size */
#define RAMLOG_BANNER	"CHARLOTTE-MAINLINE-V172\n"
enum ramlog_state {
	ST_EARLY,	/* early_memremap per write */
	ST_TRANS,	/* fixmaps dead; drop until armed */
	ST_ARMED,	/* cached memremap active */
};

static enum ramlog_state state = ST_EARLY;
static char hhee_logbuf[32768] __aligned(4096);
static u32 woff;
static u32 dropped;
static bool banner_done;
static void __iomem *ramlog_map;
static DEFINE_RAW_SPINLOCK(ramlog_lock);
/* Snapshot before this boot writes its first byte over the preceding boot. */
static u8 previous_ramlog[0x10000];
static bool previous_saved;

static void ramlog_emit(const char *s, size_t n)
{
	const size_t room = 0x10000U - RAMLOG_HDR;
	bool armed = state == ST_ARMED;
	unsigned long flags = 0, p;
	size_t start;
	void __iomem *m;

	if (state == ST_TRANS)
		return;
	m = armed ? ramlog_map : early_memremap(RAMLOG_PA, RAMLOG_CAP);
	if (!m)
		return;
	if (!previous_saved) {
		memcpy_fromio(previous_ramlog, m, sizeof(previous_ramlog));
		previous_saved = true;
	}
	/* Before SMP, only the boot CPU uses temporary fixmaps. */
	if (armed)
		raw_spin_lock_irqsave(&ramlog_lock, flags);
	if (n > room || woff + n > room)
		n = 0;
	start = woff;
	if (n)
		memcpy(m + RAMLOG_HDR + start, s, n);
	woff += n;
	iowrite32(0, m + 4);
	iowrite32(woff > 0x11000 ? woff : 0x11000, m + 8);
	iowrite32(RAMLOG_SIG, m);

	/* Persist only the header and newly appended cache lines. */
	asm volatile("dc cvac, %0" :: "r"(m) : "memory");
	for (p = (unsigned long)(m + RAMLOG_HDR + start) & ~63UL;
	     n && p < (unsigned long)(m + RAMLOG_HDR + woff); p += 64)
		asm volatile("dc cvac, %0" :: "r"(p) : "memory");
	asm volatile("dsb sy" ::: "memory");
	if (armed)
		raw_spin_unlock_irqrestore(&ramlog_lock, flags);
	else
		early_memunmap(m, RAMLOG_CAP);
}

static void ramlog_line(const char *tag)
{
	if (!banner_done) {
		void __iomem *m;

		ramlog_emit(RAMLOG_BANNER, sizeof(RAMLOG_BANNER) - 1);
		banner_done = true;
		/* v100: zero the per-cluster A-G slot window in THIS
		 * boot cycle (a head.S zero-loop erased the previous
		 * cycle's slots on every aborted panic-loop reboot). */
		m = early_memremap(RAMLOG_PA, RAMLOG_CAP);
		if (m) {
			memset(m + 0x10000 + 0x1c0, 0, 16 * 128);
			early_memunmap(m, RAMLOG_CAP);
		}
	}
	ramlog_emit(tag, strlen(tag));
}

/* Called from arch/arm64 setup.c / mmu.c and init/main.c hooks. */
void charlotte_ckpt(const char *tag)
{
	ramlog_line(tag);
}

void charlotte_ckpt_hex(const char *tag, unsigned long long v)
{
	char note[64];

	if (!banner_done) {
		ramlog_emit(RAMLOG_BANNER, sizeof(RAMLOG_BANNER) - 1);
		banner_done = true;
	}
	snprintf(note, sizeof(note), "%s%016llx\n", tag, v);
	ramlog_emit(note, strlen(note));
}

/* v186: userspace IA/DA storm tracer. Since v183 PID 1 (busybox
 * script and native ELF alike) pins in el0_ia -> force_sig with zero
 * userspace output; record FAR/ESR/PC so the fault address source
 * (ELF entry vs sigreturn trampoline vs unmapped) is visible. */
void charlotte_ua_trace(unsigned long far, unsigned long esr,
			struct pt_regs *regs, const char *kind)
{
	char note[112];

	if (!banner_done) {
		ramlog_emit(RAMLOG_BANNER, sizeof(RAMLOG_BANNER) - 1);
		banner_done = true;
	}
	snprintf(note, sizeof(note),
		 "E:%s +%ums FAR=%016lx ESR=%016lx PC=%016lx CPU=%d %s/%d\n",
		 kind, jiffies_to_msecs(jiffies - INITIAL_JIFFIES), far, esr,
		 instruction_pointer(regs), smp_processor_id(),
		 current->comm, current->pid);
	ramlog_emit(note, strlen(note));
}
EXPORT_SYMBOL_GPL(charlotte_ua_trace);


/* v101: dump the per-cluster A-G slot window as C-log text; called
 * from the smp cpu-up timeout path so the data lands long before
 * any later panic (and survives aborted-cycle banner memsets). */
void charlotte_slot_mirror(void)
{
	/* v112: hhee writes its judge/mismatch prints into our
	 * registered logbuf; mirror the fresh tail into the ramlog. */
	{
		u32 n = sizeof(hhee_logbuf);
		u32 last = 0, i;

		for (i = 0; i < n; i++)
			if (hhee_logbuf[i])
				last = i;
		if (last) {
			u32 start = last > 0x400 ? last - 0x400 : 0;

			ramlog_line("E:HL BEGIN\n");
			for (i = start; i <= last; i++)
				if (!hhee_logbuf[i])
					hhee_logbuf[i] = '\n';
			ramlog_emit(hhee_logbuf + start, last - start + 1);
			ramlog_line("E:HL END\n");
		}
	}
	if (ramlog_map) {
		char note[256];
		void __iomem *s = ramlog_map + 0x10000 + 0x1c0;
		/* v151: slot index = Aff1*2 + Aff0, stride 128B.
		 * DT cpu c: 0..3 -> mpids 0,100,200,300 -> idx 0,2,4,6
		 *           4..11 -> 400,401,500,501,600,601,700,701
		 *                 -> idx 8,9,10,11,12,13,14,15 */
		static const int idx[12] = { 0, 2, 4, 6, 8, 9, 10, 11, 12, 13, 14, 15 };
		int c;

		for (c = 0; c < 12; c++) {
			int o = idx[c] * 128;
			u64 isar1 = ioread64_lo_hi(s + o + 8);
			u64 ttbr1 = ioread64_lo_hi(s + o + 16);
			u64 tcr = ioread64_lo_hi(s + o + 24);
			u64 mair = ioread64_lo_hi(s + o + 48);
			u32 stage = ioread32(s + o + 56);
			u64 actual = ioread64_lo_hi(s + o + 72);
			u64 planned = ioread64_lo_hi(s + o + 80);
			u32 f3 = ioread32(s + o + 96);
			u64 pristine = ioread64_lo_hi(s + o + 104);
			u64 par = ioread64_lo_hi(s + o + 120);

			snprintf(note, sizeof(note),
				 "E:CL%d TTBR1=%llx TCR=%llx MAIR=%llx STAGE=%x SCTLR=%llx WANT=%llx F3=%x ENTRY=%llx PAR=%llx ISAR1=%llx\n",
				 c, ttbr1, tcr, mair, stage, actual, planned,
				 f3, pristine, par, isar1);
			ramlog_line(note);
		}
}
}


EXPORT_SYMBOL_GPL(charlotte_slot_mirror);



/* v111: HHEE_LOGBUF_INFO (hvc 0xC9000006).  The SCTLR judge prints
 * every mismatch (with the SAVED slot values) into the hhee log via
 * its printf; the vendor kernel reads that log from normal world.
 * Capture the returned buf/len and dump the tail. */
static void __iomem *hhee_log_map;
static u64 hhee_log_phys;
static u32 hhee_log_len;

static int __init ramlog_hhee_loginfo(void)
{
	u64 r0, r1, r2, r3;
	char note[96];

	/* register our buffer with hhee first (vendor hkip flow) */
	{
		register u64 x0 asm("x0") = 0xC9000005UL;
		register u64 x1 asm("x1") = __pa_symbol(hhee_logbuf);
		register u64 x2 asm("x2") = sizeof(hhee_logbuf);

		asm volatile("hvc #0"
			     : "+r"(x0)
			     : "r"(x1), "r"(x2)
			     : "x3", "x4", "x5", "x6", "x7", "x8", "x9",
			       "x10", "x11", "x12", "x13", "x14",
			       "x15", "x16", "x17", "memory");
		snprintf(note, sizeof(note), "E:HINIT ret=%llx pa=%llx\n",
			 x0, (u64)__pa_symbol(hhee_logbuf));
		ramlog_line(note);
	}
	{
		register u64 x0 asm("x0") = 0xC9000006UL;

		asm volatile("hvc #0"
			     : "=r"(x0)
			     : "0"(x0)
			     : "x1", "x2", "x3", "x4", "x5", "x6", "x7",
			       "x8", "x9", "x10", "x11", "x12", "x13", "x14",
			       "x15", "x16", "x17", "memory");
		r0 = x0;
	}
	{
		register u64 x0 asm("x0") = 0xC9000006UL;
		register u64 x1 asm("x1") = 0;
		register u64 x2 asm("x2") = 0;
		register u64 x3 asm("x3") = 0;

		asm volatile("hvc #0"
			     : "=r"(x0), "=r"(x1), "=r"(x2), "=r"(x3)
			     : "0"(x0), "1"(x1), "2"(x2), "3"(x3)
			     : "x4", "x5", "x6", "x7", "x8", "x9",
			       "x10", "x11", "x12", "x13", "x14",
			       "x15", "x16", "x17", "memory");
		r0 = x0; r1 = x1; r2 = x2; r3 = x3;
	}
	snprintf(note, sizeof(note), "E:HLOG r0=%llx r1=%llx r2=%llx r3=%llx\n",
		 r0, r1, r2, r3);
	ramlog_line(note);
	/* convention guess: x0 = buffer PA, x1 = length (or x0/x1 pair) */
	if (r0 > 0x10000000UL && r0 < 0x48000000UL && !(r0 & 0xfff)) {
		hhee_log_phys = r0;
		hhee_log_len = min_t(u64, r1 ? r1 : 0x8000, 0x20000);
		hhee_log_map = memremap(hhee_log_phys, hhee_log_len, MEMREMAP_WB);
		if (hhee_log_map) {
			snprintf(note, sizeof(note),
				 "E:HLOG mapped %llx +%x\n", hhee_log_phys, hhee_log_len);
			ramlog_line(note);
			/* dump the last 1.5KB as text */
			{
				u32 off = hhee_log_len > 0x600 ? hhee_log_len - 0x600 : 0;
				char *tail = (char *)hhee_log_map + off;
				u32 i, start = 0, n = hhee_log_len - off;

				for (i = 0; i < n; i++)
					if (tail[i])
						start = i;
				if (start > 0x500)
					start -= 0x500;
				else
					start = 0;
				ramlog_emit(tail + start, min_t(u32, start ? 0x500 : n - start, 0x500));
				ramlog_line("E:HLOG END\n");
			}
		}
	}
	return 0;
}
early_initcall(ramlog_hhee_loginfo);

static int ramlog_panic(struct notifier_block *nb, unsigned long code, void *d);

static struct notifier_block ramlog_panic_nb = {
	.notifier_call = ramlog_panic,
};
static bool panic_nb_registered;

/* Called from start_kernel (init/main.c) before setup_arch. */
void charlotte_panic_notify_arm(void)
{
	if (!panic_nb_registered) {
		atomic_notifier_chain_register(&panic_notifier_list,
					       &ramlog_panic_nb);
		panic_nb_registered = true;
	}
}

/* Called from charlotte-keepalive.c / charlotte-reset-test.c. */
void charlotte_bbox_checkpoint(const char *s)
{
	ramlog_line(s);
}

/* Called from uart_v500 earlycon setup. */
void charlotte_bboxlog_early(const char *tag)
{
	ramlog_line("E:EARLYCON_SETUP\n");
}

/* Mirror of earlycon output (uart_v500_earlycon_write).  Once the real
 * uart console is up (uart_v500_console_setup) the earlycon mirror is
 * muted and the console mirror takes over, so each printk line lands
 * exactly once. */
static bool mirror_console_mode;

void charlotte_mirror_handover(void)
{
	mirror_console_mode = true;
	ramlog_line("E:MIRROR_HANDOVER\n");
}

void charlotte_ramlog_mirror(const char *s, unsigned int n)
{
	if (!mirror_console_mode)
		ramlog_emit(s, n);
}

void charlotte_ramlog_mirror_console(const char *s, unsigned int n)
{
	if (mirror_console_mode)
		ramlog_emit(s, n);
}

static int __init ramlog_arm(void)
{
	void __iomem *m = early_memremap(RAMLOG_PA, RAMLOG_CAP);

	if (m) {
		char note[128];
		u32 mg[13];
		int i;

		/* magic1 was written explicitly at +0; macro magics sit at
		 * +N*4: 2->+8, 3->+0xc, ... 8->+0x20 */
		static const int off[13] = { 0, 0, 8, 0xc, 0x10, 0x14, 0x18, 0x1c,
					     0x20, 0x24, 0x28, 0x2c, 0x30 };

		for (i = 1; i <= 12; i++)
			mg[i] = ioread32(m + 0x10000 + off[i]);
		early_memunmap(m, RAMLOG_CAP);
		snprintf(note, sizeof(note),
			 "E:ASM %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
			 mg[1], mg[2], mg[3], mg[4], mg[5], mg[6], mg[7], mg[8],
			 mg[9], mg[10], mg[11], mg[12]);
		ramlog_line(note);
	}
	ramlog_map = memremap(RAMLOG_PA, RAMLOG_CAP, MEMREMAP_WB);
	if (!ramlog_map) {
		ramlog_line("E:RAMLOG_MAP_FAILED\n");
		state = ST_TRANS;
		return -ENOMEM;
	}
	state = ST_ARMED;
	if (dropped) {
		char note[48];

		snprintf(note, sizeof(note), "E:RAMLOG +%u bytes dropped\n",
			 dropped);
		ramlog_line(note);
	}
	ramlog_line("E:RAMLOG_ARMED\n");
	pr_info("charlotte-ramlog: armed at 0x%lx, previous=%s, dropped=%u\n",
		RAMLOG_PA, previous_saved ? "saved" : "empty", dropped);
	return 0;
}
early_initcall(ramlog_arm);
static int ramlog_panic(struct notifier_block *nb, unsigned long code, void *d)
{
	ramlog_line("E:PANIC\n");
	return NOTIFY_DONE;
}


static int __init ramlog_notify(void)
{
	if (!panic_nb_registered)
		atomic_notifier_chain_register(&panic_notifier_list,
					       &ramlog_panic_nb);
	ramlog_line("E:NOTIFIER_ARMED\n");
	return 0;
}
late_initcall(ramlog_notify);

static ssize_t charlotte_stage_store(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     const char *buf, size_t count)
{
	char note[128];
	size_t n = count;

	if (n > sizeof(note) - 2)
		n = sizeof(note) - 2;
	memcpy(note, buf, n);
	while (n && (note[n - 1] == '\n' || note[n - 1] == '\r'))
		n--;
	note[n++] = '\n';
	note[n] = '\0';
	ramlog_line(note);
	return count;
}

static struct kobj_attribute charlotte_stage_attr =
	__ATTR(charlotte_stage, 0200, NULL, charlotte_stage_store);

static int __init ramlog_stage_export(void)
{
	return sysfs_create_file(kernel_kobj, &charlotte_stage_attr.attr);
}
late_initcall(ramlog_stage_export);

static BIN_ATTR_SIMPLE_ADMIN_RO(charlotte_previous_ramlog);

static int __init ramlog_previous_export(void)
{
	if (!previous_saved)
		return -ENODATA;
	bin_attr_charlotte_previous_ramlog.private = previous_ramlog;
	bin_attr_charlotte_previous_ramlog.size = sizeof(previous_ramlog);
	return sysfs_create_bin_file(kernel_kobj,
				    &bin_attr_charlotte_previous_ramlog);
}
late_initcall(ramlog_previous_export);
