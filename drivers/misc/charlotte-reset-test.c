// SPDX-License-Identifier: GPL-2.0
/*
 * charlotte-reset-test -- deterministic boot-progress probe.
 *
 * At late_initcall time issues PSCI SYSTEM_RESET directly via an HVC
 * ( conduit per the vendor DTB psci node ).  The resulting boot loop
 * period is a signature:
 *
 *   tight loop (~ logo 2s + warning 5s + boot 3s )  -> kernel alive,
 *     earlycon/initcalls reached, PSCI hvc conduit functional
 *   single slow reset into fastboot w/ LPM3_S_LPMCURST -> kernel not
 *     entered (or died before initcalls) OR hvc conduit broken
 */

#include <linux/arm-smccc.h>

#ifdef CONFIG_CHARLOTTE_BBOXLOG
void charlotte_bbox_checkpoint(const char *s);
#endif
#include <linux/init.h>
#include <linux/printk.h>

static int __init charlotte_reset_test(void)
{
	struct arm_smccc_res res;

	pr_info("charlotte-reset-test: issuing PSCI SYSTEM_RESET via hvc\n");
#ifdef CONFIG_CHARLOTTE_BBOXLOG
	{
		extern void charlotte_bbox_checkpoint(const char *s);
		charlotte_bbox_checkpoint("E:PRE-HVC-RESET\n");
	}
#endif
	arm_smccc_hvc(0x84000009, 0, 0, 0, 0, 0, 0, 0, &res);
	pr_info("charlotte-reset-test: hvc returned 0x%lx?!\n", res.a0);

	/* If we get here the conduit is dead. */
	return 0;
}
late_initcall(charlotte_reset_test);
