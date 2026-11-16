/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_CHARLOTTE_FFA_H
#define _LINUX_CHARLOTTE_FFA_H
#include <linux/types.h>
struct device;
struct ffa_send_direct_data;
int charlotte_ffa_usb_access(struct device *dev);
int charlotte_ffa_pcie_access(u32 rc_id);
int charlotte_ffa_pcie_phy_trim(u32 *value);
int charlotte_ffa_gpu_power(struct device *dev, bool enable);
int charlotte_ffa_dss_sec_config(struct device *dev, u32 func_id, u32 channel,
				 u32 mode, u32 dpu_index,
				 struct ffa_send_direct_data *reply);
#endif
