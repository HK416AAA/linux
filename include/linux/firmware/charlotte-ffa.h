/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_CHARLOTTE_FFA_H
#define _LINUX_CHARLOTTE_FFA_H
#include <linux/types.h>
struct device;
int charlotte_ffa_usb_access(struct device *dev);
int charlotte_ffa_pcie_access(u32 rc_id);
int charlotte_ffa_pcie_phy_trim(u32 *value);
#endif
