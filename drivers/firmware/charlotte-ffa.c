// SPDX-License-Identifier: GPL-2.0-only
/* Charlotte platform ABI from vendor ffa_plat_drv and efuse common_call. */
#include <linux/arm_ffa.h>
#include <linux/device.h>
#include <linux/firmware/charlotte-ffa.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mutex.h>

#define USB_TZPC        0xc3002504
#define PCIE_TBU_BYPASS 0xc3002700
#define PCIE_MEM_UNSEC  0xc3002702
#define EFUSE_READ      0xc3001900
#define EFUSE_ITEM_INFO 0xc3001902
#define EFUSE_COMBO2_PHY 0x00030025

static DEFINE_MUTEX(charlotte_ffa_lock);
static struct ffa_device *charlotte_ffa_dev;

/* The platform partition is a single FF-A bus device. One driver owns it;
 * peripheral drivers share these calls instead of competing to bind its UUID.
 */
static int charlotte_ffa_call(struct device *consumer,
			      struct ffa_send_direct_data *args)
{
	struct device_link *link;
	int ret;

	mutex_lock(&charlotte_ffa_lock);
	if (!charlotte_ffa_dev) {
		ret = -EPROBE_DEFER;
		goto out;
	}
	if (consumer) {
		link = device_link_add(consumer, &charlotte_ffa_dev->dev,
				       DL_FLAG_AUTOREMOVE_CONSUMER);
		if (!link) {
			ret = -ENOMEM;
			goto out;
		}
	}
	ret = charlotte_ffa_dev->ops->msg_ops->sync_send_receive(charlotte_ffa_dev, args);
	if (!ret && args->data0)
		ret = -EIO;
out:
	mutex_unlock(&charlotte_ffa_lock);
	return ret;
}

int charlotte_ffa_usb_access(struct device *dev)
{
	struct ffa_send_direct_data args = { .data0 = USB_TZPC };
	int ret;

	ret = charlotte_ffa_call(dev, &args);
	if (!ret)
		dev_info(dev, "USB_TZPC FF-A response %#lx\n", args.data0);
	return ret;
}
EXPORT_SYMBOL_GPL(charlotte_ffa_usb_access);

int charlotte_ffa_pcie_access(u32 rc_id)
{
	struct ffa_send_direct_data args = { .data0 = PCIE_MEM_UNSEC, .data1 = rc_id };
	int ret;

	if (rc_id)
		return -EINVAL;
	ret = charlotte_ffa_call(NULL, &args);
	if (ret)
		return ret;
	args = (struct ffa_send_direct_data) { .data0 = PCIE_TBU_BYPASS, .data1 = rc_id };
	return charlotte_ffa_call(NULL, &args);
}
EXPORT_SYMBOL_GPL(charlotte_ffa_pcie_access);

int charlotte_ffa_pcie_phy_trim(u32 *value)
{
	struct ffa_send_direct_data args = { .data0 = EFUSE_ITEM_INFO, .data1 = EFUSE_COMBO2_PHY };
	u32 bits;
	int ret;

	ret = charlotte_ffa_call(NULL, &args);
	if (ret)
		return ret;
	bits = args.data1;
	if (!bits || bits > 32)
		return -ERANGE;
	/* The vendor combines start_bit=0 and bit_cnt in data2. */
	args = (struct ffa_send_direct_data) {
		.data0 = EFUSE_READ, .data1 = EFUSE_COMBO2_PHY, .data2 = bits,
	};
	ret = charlotte_ffa_call(NULL, &args);
	if (!ret)
		*value = args.data1;
	return ret;
}
EXPORT_SYMBOL_GPL(charlotte_ffa_pcie_phy_trim);

static int charlotte_ffa_probe(struct ffa_device *dev)
{
	if (!dev->ops || !dev->ops->msg_ops)
		return -ENODEV;
	dev->ops->msg_ops->mode_32bit_set(dev);
	mutex_lock(&charlotte_ffa_lock);
	charlotte_ffa_dev = dev;
	mutex_unlock(&charlotte_ffa_lock);
	dev_info(&dev->dev, "Charlotte platform service ready\n");
	return 0;
}

static void charlotte_ffa_remove(struct ffa_device *dev)
{
	mutex_lock(&charlotte_ffa_lock);
	charlotte_ffa_dev = NULL;
	mutex_unlock(&charlotte_ffa_lock);
}

static const struct ffa_device_id charlotte_ffa_ids[] = {
	/* Preserve the vendor FF-A wire UUID byte order. */
	{ .uuid = { .b = { 0x33, 0x5f, 0xe7, 0xf9, 0x82, 0x4f, 0xca, 0xa1,
			    0x02, 0xc8, 0xe6, 0x98, 0xb4, 0xec, 0x20, 0x0f } } },
	{ }
};
MODULE_DEVICE_TABLE(ffa, charlotte_ffa_ids);

static struct ffa_driver charlotte_ffa_driver = {
	.name = "charlotte-platform", .id_table = charlotte_ffa_ids,
	.probe = charlotte_ffa_probe, .remove = charlotte_ffa_remove,
};

static int __init charlotte_ffa_init(void)
{
	return ffa_register(&charlotte_ffa_driver);
}
subsys_initcall(charlotte_ffa_init);

static void __exit charlotte_ffa_exit(void)
{
	ffa_unregister(&charlotte_ffa_driver);
}
module_exit(charlotte_ffa_exit);

MODULE_DESCRIPTION("Shared Charlotte USB/PCIe platform FF-A service");
MODULE_LICENSE("GPL");
