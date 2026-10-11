// SPDX-License-Identifier: GPL-2.0-only
/* Read-only Huawei NVE layout from vendor nve_ap_kernel.{c,h}.
 * Copyright (c) Huawei Technologies Co., Ltd. 2012-2020.
 * No factory partition write, repair, or ioctl interface is provided.
 */
#include <linux/crc32c.h>
#include <linux/fs.h>
#include <linux/mutex.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include "charlotte-nve.h"

#define NVE_SLOT_SIZE SZ_128K
#define NVE_ITEM_COUNT 1023

struct charlotte_nve_item {
	__le32 number;
	char name[8];
	__le32 property;
	__le32 size;
	__le32 crc;
	u8 data[NVE_NV_DATA_SIZE];
};

struct charlotte_nve_slot {
	struct charlotte_nve_item items[NVE_ITEM_COUNT];
	struct {
		char name[32];
		__le32 version, block_id, block_count, count, checksum, crc_support;
		u8 reserved[68];
		__le32 age;
	} header;
};

static_assert(sizeof(struct charlotte_nve_item) == 128);
static_assert(sizeof(struct charlotte_nve_slot) == NVE_SLOT_SIZE);
static DEFINE_MUTEX(charlotte_nve_lock);
static struct charlotte_nve_slot *charlotte_nve_cache;

static bool charlotte_nve_slot_valid(const struct charlotte_nve_slot *slot)
{
	u32 count = le32_to_cpu(slot->header.count);
	u32 crc_support = le32_to_cpu(slot->header.crc_support);
	u32 i;

	if (memcmp(slot->header.name, "Hisi-NV-Partition", sizeof("Hisi-NV-Partition")) &&
	    memcmp(slot->header.name, "AP-NV-Partition", sizeof("AP-NV-Partition")))
		return false;
	if (!count || count > NVE_ITEM_COUNT || !le32_to_cpu(slot->header.age) ||
	    crc_support > 2)
		return false;

	for (i = 0; i < count; i++) {
		const struct charlotte_nve_item *item = &slot->items[i];
		u32 crc;

		if (le32_to_cpu(item->number) != i ||
		    le32_to_cpu(item->size) > NVE_NV_DATA_SIZE)
			return false;
		if (crc_support != 2)
			continue;
		/* Vendor CRC32C skips the stored CRC word and includes all 104 bytes. */
		crc = crc32c(~0U, item, offsetof(struct charlotte_nve_item, crc));
		crc = crc32c(crc, item->data, sizeof(item->data));
		if (~crc != le32_to_cpu(item->crc))
			return false;
	}
	return true;
}

static int charlotte_nve_load(void)
{
	struct charlotte_nve_slot *slot, *selected;
	struct file *file;
	u32 age = 0, selected_id = 0;
	int ret = -EBADMSG;
	u32 id;

	/* Called after userland/udev is up, through the vendor init sysfs ABI. */
	file = filp_open("/dev/disk/by-partlabel/nvme", O_RDONLY, 0);
	if (IS_ERR(file))
		return PTR_ERR(file);
	slot = kvzalloc(sizeof(*slot), GFP_KERNEL);
	selected = kvzalloc(sizeof(*selected), GFP_KERNEL);
	if (!slot || !selected) {
		ret = -ENOMEM;
		goto out;
	}
	/* Slot 0 is the update/template area; runtime slots are 1 through 7. */
	for (id = 1; id < 8; id++) {
		loff_t position = (loff_t)id * NVE_SLOT_SIZE;
		ssize_t bytes = kernel_read(file, slot, sizeof(*slot), &position);

		if (bytes < 0) {
			ret = bytes;
			goto out;
		}
		if (bytes != sizeof(*slot) || !charlotte_nve_slot_valid(slot))
			continue;
		if (le32_to_cpu(slot->header.age) > age) {
			memcpy(selected, slot, sizeof(*selected));
			age = le32_to_cpu(slot->header.age);
			selected_id = id;
		}
	}
	if (!age)
		goto out;
	charlotte_nve_cache = selected;
	selected = NULL;
	pr_info("charlotte-wifi: read-only NVE slot %u age %u, %u records, CRC mode %u\n",
		selected_id, age, le32_to_cpu(charlotte_nve_cache->header.count),
		le32_to_cpu(charlotte_nve_cache->header.crc_support));
	ret = 0;
out:
	kvfree(slot);
	kvfree(selected);
	filp_close(file, NULL);
	return ret;
}

int charlotte_nve_read(struct charlotte_nve_info *info)
{
	const struct charlotte_nve_item *item;
	u32 size;
	int ret = 0;

	if (!info)
		return -EINVAL;
	if (info->nv_operation != NV_READ)
		return -EROFS;
	if (info->nv_number >= NVE_ITEM_COUNT || info->valid_size > NVE_NV_DATA_SIZE)
		return -EINVAL;
	mutex_lock(&charlotte_nve_lock);
	if (!charlotte_nve_cache) {
		ret = charlotte_nve_load();
		if (ret)
			goto out;
	}
	if (info->nv_number >= le32_to_cpu(charlotte_nve_cache->header.count)) {
		ret = -ENOENT;
		goto out;
	}
	item = &charlotte_nve_cache->items[info->nv_number];
	size = le32_to_cpu(item->size);
	if (!info->valid_size || info->valid_size > size)
		info->valid_size = size;
	memset(info->nv_data, 0, sizeof(info->nv_data));
	memcpy(info->nv_data, item->data, info->valid_size);
out:
	mutex_unlock(&charlotte_nve_lock);
	return ret;
}
