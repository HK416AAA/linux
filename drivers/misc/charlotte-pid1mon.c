// SPDX-License-Identifier: GPL-2.0
/*
 * charlotte-pid1mon -- bring-up probe: report the state of PID 1 every
 * 5 seconds so a silently-stuck userspace shows its wait channel in the
 * pstore console.  Remove once the initramfs boot is proven.
 */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/sched/debug.h>
#include <linux/sched/signal.h>
#include <linux/timer.h>

static struct timer_list pid1_timer;

static void pid1_tick(struct timer_list *t)
{
	struct task_struct *p;

	rcu_read_lock();
	p = find_task_by_vpid(1);
	if (p) {
		pr_info("charlotte-pid1mon: pid1 comm=%s state=0x%x\n",
			p->comm, READ_ONCE(p->__state));
		sched_show_task(p);
	} else {
		pr_info("charlotte-pid1mon: pid1 gone\n");
	}
	rcu_read_unlock();

	mod_timer(t, jiffies + 5 * HZ);
}

static int __init pid1mon_init(void)
{
	timer_setup(&pid1_timer, pid1_tick, 0);
	pid1_timer.expires = jiffies + 15 * HZ;
	add_timer(&pid1_timer);
	pr_info("charlotte-pid1mon: armed\n");
	return 0;
}
early_initcall(pid1mon_init);
MODULE_LICENSE("GPL");
