// SPDX-License-Identifier: GPL-2.0
/*
 * charlotte-keepalive -- keep the LPM3 housekeeper from executing the AP
 * while mainline Linux runs on Kirin 9000s (charlotte).
 *
 * The stock kernel does two things we must replicate early (vendor
 * wdt_v500.c + ipc_mailbox):
 *
 *  1. kick the local V500 watchdog (0xff4e1000, default-timeout 24 s per
 *     the vendor DTB) so it never expires;
 *  2. notify LPM3 over the HiIPCV230 mailbox IPC_ACPU_LPM3_MBX_5
 *     (channel 29, index 0x1d) with the message
 *         [0x00080201, timeout_seconds]
 *     where 0x00080201 = IPC_CMD(OBJ_AP, OBJ_LPM3, CMD_INQUIRY, 1)
 *     (vendor include/platform_include/basicplatform/linux/ipc_msg.h);
 *     this resets LPM3's own watchdog timeout.
 *
 * Everything is bare MMIO: no mailbox/pinctrl/clock framework needed.
 * The stock UEFI runs the same IPC hardware from EL1, so hhee does not
 * filter these writes.
 *
 * A dead-AP failure turns into LPM3_S_LPMCURST (0x46) in the DFX reset
 * log after ~24 s; repeated failures trip unexpected-max-reboot-times
 * and the phone lands in fastboot.  If this driver runs, that death
 * mode disappears.
 */

#include <linux/delay.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/panic_notifier.h>
#include <linux/printk.h>
#include <linux/reboot.h>
#include <linux/timer.h>

#ifdef CONFIG_CHARLOTTE_BBOXLOG
void charlotte_bbox_checkpoint(const char *s);
#endif

/* ---- local V500 watchdog (vendor DTB watchdog0 @ 0xff4e1000) ---- */
#define WDT_BASE	0xff4e1000UL
#define WDT_LOCKEN	0x000	/* write 0x1AAEE533 to unlock */
#define WDT_LOCKDIS	0x004	/* write 0x1 to re-lock */
#define WDT_KICK	0x018	/* write 0x1 to kick */
#define WDT_INTR_TIME	0x00c
#define WDT_RST_TIME	0x010
#define WDT_ENABLE	0x014
#define WDT_INTR_MASK	0x020
#define WDT_RST_MASK	0x024
#define WDT_CLOCK_HZ	32764
#define WDT_UNLOCK_KEY	0x1AAEE533

/* ---- HiIPCV230 IPC (vendor DTB ipc@FF870000, ipc_version=1) ---- */
#define IPC_BASE		0xff870000UL
#define IPC_LOCK_OFF		0xA000	/* ipc_lock(BOY) */
#define IPC_UNLOCK_KEY		0x1acce551

/* ACPU -> LPM3 MBX_5: channel 29, reg_spc = 7 (IPC_REG_SPACE_EX) */
#define MBX5_CHN		29
#define MBX5_OFF		(MBX5_CHN << 7)		/* 0xE80 */
#define MBX5_SRC		(MBX5_OFF + 0x00)	/* src bitmask, ACPU = bit 0 */
#define MBX5_DSET		(MBX5_OFF + 0x04)	/* des bitmask, LPMCU = bit 2 */
#define MBX5_DSTATUS	(MBX5_OFF + 0x0C)	/* state machine */
#define MBX5_MODE		(MBX5_OFF + 0x10)	/* AUTOMATIC_ACK_CONFIG = 1 */
#define MBX5_IMASK		(MBX5_OFF + 0x14)
#define MBX5_SEND		(MBX5_OFF + 0x1C)
#define MBX5_DATA(i)		(MBX5_OFF + 0x20 + ((i) << 2))

#define SRC_ACPU_BIT		BIT(0)
#define DES_LPMCU_BIT		BIT(2)
#define IDLE_STATUS		0x1	/* state machine nibble, low bits */

/* vendor default-timeout = 24 s; kick period = timeout/2/3 = 4 s */
#define KICK_PERIOD_MS		4000

static void __iomem *wdt_base;
static void __iomem *ipc_base;
static struct timer_list keepalive_timer;
static unsigned long mbx5_sent, mbx5_busy, wdt_kicks;
static bool panic_seen;
static bool poweroff_seen;

/* Keep feeding on healthy boots; allow several minutes for panic capture. */
static unsigned int timeout = 300;
module_param(timeout, uint, 0444);
MODULE_PARM_DESC(timeout, "Local watchdog and LPM3 recovery timeout in seconds");

/* Optional recovery window for bring-up; normal boots keep feeding forever. */
static unsigned int stop_after;
module_param(stop_after, uint, 0644);
MODULE_PARM_DESC(stop_after, "Stop watchdog feeding after this many seconds (0: never)");

static void wdt_configure(void __iomem *base, unsigned int seconds)
{
	u32 load = (WDT_CLOCK_HZ / 2) * seconds - 1;

	/* Vendor V500 uses two equal stages and the 32764 Hz clkin_ref. */
	writel_relaxed(WDT_UNLOCK_KEY, base + WDT_LOCKEN);
	writel_relaxed(load, base + WDT_INTR_TIME);
	writel_relaxed(load, base + WDT_RST_TIME);
	/* Recovery uses hardware reset; there is no root watchdog FIQ handler. */
	writel_relaxed(1, base + WDT_INTR_MASK);
	writel_relaxed(0, base + WDT_RST_MASK);
	writel_relaxed(1, base + WDT_ENABLE);
	writel_relaxed(1, base + WDT_KICK);
	writel_relaxed(1, base + WDT_LOCKDIS);
	readl(base + WDT_LOCKDIS);
}

static int keepalive_panic(struct notifier_block *nb, unsigned long event, void *data)
{
	WRITE_ONCE(panic_seen, true);
	pr_emerg("charlotte-keepalive: panic; stop feeding, hardware recovery within %us\n",
		 timeout);
	return NOTIFY_DONE;
}

static struct notifier_block keepalive_panic_nb = {
	.notifier_call = keepalive_panic,
	.priority = 200,
};

static int keepalive_poweroff(struct notifier_block *nb, unsigned long action,
			     void *data)
{
	if (action != SYS_POWER_OFF)
		return NOTIFY_DONE;
	WRITE_ONCE(poweroff_seen, true);
	pr_emerg("charlotte-keepalive: poweroff; stop feeding, failed shutdown recovers within %us\n",
		 timeout);
	return NOTIFY_DONE;
}

static struct notifier_block keepalive_poweroff_nb = {
	.notifier_call = keepalive_poweroff,
	.priority = 200,
};

static void wdt_kick(void)
{
	if (!wdt_base)
		return;
	writel_relaxed(WDT_UNLOCK_KEY, wdt_base + WDT_LOCKEN);
	writel_relaxed(1, wdt_base + WDT_KICK);
	writel_relaxed(1, wdt_base + WDT_LOCKDIS);
	/* flush posted writes, as the vendor driver does */
	readl(wdt_base + WDT_LOCKEN);
	readl(wdt_base + WDT_LOCKDIS);
	wdt_kicks++;
}

/* Mirrors ipc_mdev_unlock/occupy/hw_send (AUTO_ACK async send). */
static void mbx5_notify_timeout(u32 timeout_sec)
{
	u32 status;
	int retry = 10;

	if (!ipc_base)
		return;

	/* unlock (retry, mirroring ipc_mdev_unlock) */
	writel_relaxed(IPC_UNLOCK_KEY, ipc_base + IPC_LOCK_OFF);
	if (readl(ipc_base + IPC_LOCK_OFF) != 0) {
		mbx5_busy++;
		return;
	}

	/* occupy: wait for IDLE then claim the channel with our src bit */
	do {
		status = readl_relaxed(ipc_base + MBX5_DSTATUS);
		if (!(status & IDLE_STATUS)) {
			udelay(5);
			continue;
		}
		writel_relaxed(SRC_ACPU_BIT, ipc_base + MBX5_SRC);
		if (readl_relaxed(ipc_base + MBX5_SRC) & SRC_ACPU_BIT)
			break;
	} while (--retry);
	if (!retry) {
		mbx5_busy++;
		return;
	}

	/* hw_send with AUTO_ACK */
	writel_relaxed(~0U, ipc_base + MBX5_IMASK);
	writel_relaxed(readl_relaxed(ipc_base + MBX5_IMASK) & ~DES_LPMCU_BIT,
			ipc_base + MBX5_IMASK);
	writel_relaxed(DES_LPMCU_BIT, ipc_base + MBX5_DSET);
	writel_relaxed(1, ipc_base + MBX5_MODE);	/* AUTOMATIC_ACK_CONFIG */
	writel_relaxed(0x00080201, ipc_base + MBX5_DATA(0));
	writel_relaxed(timeout_sec, ipc_base + MBX5_DATA(1));
	writel_relaxed(SRC_ACPU_BIT, ipc_base + MBX5_SEND);
	mbx5_sent++;
}

static void keepalive_tick(struct timer_list *t)
{
	static bool first_tick;
	static unsigned int tick_count;
	unsigned int limit = READ_ONCE(stop_after);

	if (READ_ONCE(panic_seen) || READ_ONCE(poweroff_seen))
		return;

	if (limit && (u64)tick_count * KICK_PERIOD_MS >= (u64)limit * 1000) {
#ifdef CONFIG_CHARLOTTE_BBOXLOG
		charlotte_bbox_checkpoint("E:KA_STOP\n");
#endif
		return;
	}

	wdt_kick();
	if (!first_tick) {
		first_tick = true;
#ifdef CONFIG_CHARLOTTE_BBOXLOG
		charlotte_bbox_checkpoint("E:KA_TICK1\n");
#endif
	}
	mbx5_notify_timeout(timeout);
	tick_count++;
#ifdef CONFIG_CHARLOTTE_BBOXLOG
	/* liveness heartbeat: every other 4 s tick lands in the
	 * ramlog, so a frozen-but-alive kernel keeps appending while
	 * a dead one stops. */
	if (tick_count % 2 == 0) {
		char note[40];

		snprintf(note, sizeof(note), "E:KA_ALIVE %u\n", tick_count);
		charlotte_bbox_checkpoint(note);
	}
#endif
	mod_timer(t, jiffies + msecs_to_jiffies(KICK_PERIOD_MS));
}

/* Bare-PA poke for the pre-initcall window (start_kernel: setup_arch
 * returns long before do_initcalls arms the timer; without a kick the
 * 24 s LPM3 window expires mid-start_kernel with caches off). Uses
 * early_ioremap, which is live from setup_arch onward. */
void charlotte_keepalive_early_poke(void)
{
	void __iomem *w;
	void __iomem *p;

	w = early_ioremap(WDT_BASE, 0x100);
	if (w) {
		wdt_configure(w, timeout);
		early_iounmap(w, 0x100);
	}

	p = early_ioremap(IPC_BASE, 0x10000);
	if (!p)
		return;
	if (readl(p + IPC_LOCK_OFF) != 0) {
		writel_relaxed(IPC_UNLOCK_KEY, p + IPC_LOCK_OFF);
		if (readl(p + IPC_LOCK_OFF) != 0)
			goto out;
	}
	writel_relaxed(~0U, p + MBX5_IMASK);
	writel_relaxed(readl_relaxed(p + MBX5_IMASK) & ~DES_LPMCU_BIT,
			p + MBX5_IMASK);
	writel_relaxed(DES_LPMCU_BIT, p + MBX5_DSET);
	writel_relaxed(1, p + MBX5_MODE);
	writel_relaxed(0x00080201, p + MBX5_DATA(0));
	writel_relaxed(timeout, p + MBX5_DATA(1));
	writel_relaxed(SRC_ACPU_BIT, p + MBX5_SEND);
out:
	early_iounmap(p, 0x10000);
}

static int __init charlotte_keepalive_init(void)
{
	wdt_base = ioremap(WDT_BASE, 0x1000);
	ipc_base = ioremap(IPC_BASE, 0x10000);
	if (!wdt_base || !ipc_base) {
		pr_err("charlotte-keepalive: ioremap failed (wdt=%p ipc=%p)\n",
		       wdt_base, ipc_base);
		return -ENOMEM;
	}

	/* first kick + notify immediately, then periodically */
	timeout = clamp(timeout, 8U, 86400U);
	wdt_configure(wdt_base, timeout);
	wdt_kick();
	mbx5_notify_timeout(timeout);
	atomic_notifier_chain_register(&panic_notifier_list, &keepalive_panic_nb);
	register_reboot_notifier(&keepalive_poweroff_nb);

	timer_setup(&keepalive_timer, keepalive_tick, 0);
	keepalive_timer.expires = jiffies + msecs_to_jiffies(KICK_PERIOD_MS);
	add_timer(&keepalive_timer);

	pr_info("charlotte-keepalive: armed (wdt %us, kick %ums, mbx5 chn%u)\n",
		timeout, KICK_PERIOD_MS, MBX5_CHN);
#ifdef CONFIG_CHARLOTTE_BBOXLOG
	charlotte_bbox_checkpoint("E:KEEPALIVE_ARMED\n");
#endif
	return 0;
}
/*
 * early level: runs well inside the 24 s window, needs nothing but mm
 * (ioremap + timers), both up by do_initcalls().
 */
early_initcall(charlotte_keepalive_init);

MODULE_LICENSE("GPL");
