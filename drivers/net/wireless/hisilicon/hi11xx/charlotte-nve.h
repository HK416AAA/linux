/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CHARLOTTE_NVE_H
#define CHARLOTTE_NVE_H

#include <linux/types.h>

#define NV_NAME_LENGTH 8
#define NVE_NV_DATA_SIZE 104
#define NV_WRITE 0
#define NV_READ 1

/* Vendor caller ABI; the Charlotte implementation accepts reads only. */
struct charlotte_nve_info {
	u32 nv_operation;
	u32 nv_number;
	char nv_name[NV_NAME_LENGTH];
	u32 valid_size;
	u8 nv_data[NVE_NV_DATA_SIZE];
};

int charlotte_nve_read(struct charlotte_nve_info *info);

#endif
