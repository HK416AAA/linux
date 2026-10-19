// SPDX-License-Identifier: GPL-2.0
/* Temporary cluster4 flight recorder. No printk, cache operations or new
 * barriers in the observed path. A remote CPU can read the best-effort ring.
 */
#include <linux/charlotte-cpu-diag.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kobject.h>
#include <linux/moduleparam.h>
#include <linux/sched.h>
#include <linux/sysfs.h>

struct entry { unsigned long stage, pid, a, b, tick; };
struct cpu_log { unsigned long seq; struct entry ring[32]; };
static struct cpu_log logs[2] ____cacheline_aligned;
static bool enabled = true;
module_param(enabled, bool, 0644);

void noinstr charlotte_cpu_diag(unsigned long stage, unsigned long a, unsigned long b)
{
	unsigned int cpu = raw_smp_processor_id();
	struct cpu_log *log;
	struct entry *e;
	unsigned long seq;

	if (!READ_ONCE(enabled) || cpu < 10 || cpu > 11)
		return;
	log = &logs[cpu - 10];
	seq = READ_ONCE(log->seq);
	e = &log->ring[seq % 32];
	WRITE_ONCE(e->stage, stage);
	WRITE_ONCE(e->pid, current->pid);
	WRITE_ONCE(e->a, a);
	WRITE_ONCE(e->b, b);
	WRITE_ONCE(e->tick, jiffies);
	WRITE_ONCE(log->seq, seq + 1);
}

static ssize_t format_log(char *buf, size_t size)
{
	ssize_t len = 0;
	unsigned int cpu;
	unsigned long i, seq;

	for (cpu = 0; cpu < 2; cpu++) {
		seq = READ_ONCE(logs[cpu].seq);
		len += scnprintf(buf + len, size - len, "CPU%u seq=%lu\n", cpu + 10, seq);
		for (i = seq > 24 ? seq - 24 : 0; i < seq; i++) {
			struct entry e = logs[cpu].ring[i % 32];

			len += scnprintf(buf + len, size - len,
				"%lu s=%lu pid=%lu a=%lx b=%lx t=%lu\n",
				i, e.stage, e.pid, e.a, e.b, e.tick);
		}
	}
	return len;
}

static ssize_t cpu_diag_show(struct kobject *k, struct kobj_attribute *a, char *buf)
{
	return format_log(buf, PAGE_SIZE);
}
static struct kobj_attribute cpu_diag_attr = __ATTR_RO(cpu_diag);

void charlotte_cpu_diag_dump(void)
{
	static char dump[4096];
	extern void charlotte_bbox_checkpoint(const char *s);

	format_log(dump, sizeof(dump));
	charlotte_bbox_checkpoint(dump);
	pr_err("charlotte-cpu-diag:\n%s", dump);
}

static int __init diag_init(void)
{
	return sysfs_create_file(kernel_kobj, &cpu_diag_attr.attr);
}
late_initcall(diag_init);
