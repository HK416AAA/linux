// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2021 ARM Ltd.
 */

#include <linux/printk.h>
#include <linux/of.h>

#include "common.h"

static void __arm_ffa_fn_smc(ffa_value_t args, ffa_value_t *res)
{
	arm_smccc_1_2_smc(&args, res);
}

static void __arm_ffa_fn_hvc(ffa_value_t args, ffa_value_t *res)
{
	arm_smccc_1_2_hvc(&args, res);
}

int ffa_transport_init(ffa_fn **invoke_ffa_fn)
{
	enum arm_smccc_conduit conduit;

	/*
	 * Charlotte: v176 proved the FF-A responder sits at EL3 -- HVC
	 * reaches hhee, which answers FFA_VERSION with NOT_SUPPORTED,
	 * while SMC-routed VERSION/RXTX_MAP succeed.  Keep SMC.
	 */
	if (of_machine_is_compatible("hisilicon,charlotte")) {
		*invoke_ffa_fn = __arm_ffa_fn_smc;
		return 0;
	}

	if (arm_smccc_get_version() < ARM_SMCCC_VERSION_1_2)
		return -EOPNOTSUPP;

	conduit = arm_smccc_1_1_get_conduit();
	if (conduit == SMCCC_CONDUIT_NONE) {
		pr_err("%s: invalid SMCCC conduit\n", __func__);
		return -EOPNOTSUPP;
	}

	if (conduit == SMCCC_CONDUIT_SMC)
		*invoke_ffa_fn = __arm_ffa_fn_smc;
	else
		*invoke_ffa_fn = __arm_ffa_fn_hvc;

	return 0;
}
