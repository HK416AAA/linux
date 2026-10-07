// SPDX-License-Identifier: GPL-2.0-only
/* Charlotte USB2 peripheral initialization, following the vendor DT sequence. */

#include <linux/arm_ffa.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

#include "glue.h"
#include "io.h"

#define CHARLOTTE_USB_TZPC	0xc3002504U
#define HSDT_PEREN0		0x000
#define HSDT_PERDIS0		0x004
#define HSDT_PERRSTEN0		0x020
#define HSDT_PERRSTDIS0		0x024
#define USB_MISC_CLOCKS		(BIT(29) | BIT(13))
#define USB_PHY_CLOCKS		(BIT(18) | BIT(2))
/*
 * HSDT1 CRG PEREN0 gate bits (soc_hsdt1_crg_interface.h): the dwc3 sits
 * behind a HISI "hi" bridge whose clocks are separate from the snps
 * domain.  DEPCMD stalls unless both sides are running (v179: snp side
 * on, hi side off -> "failed to enable ep0out").
 */
#define USB_CORE_CLOCKS		(BIT(6) | BIT(8) | BIT(9) | BIT(10) | \
				 BIT(11) | BIT(12) | BIT(13) | BIT(14) | \
				 BIT(15) | BIT(22))
#define USB_CORE_RESETS		(BIT(13) | BIT(16) | BIT(18))
#define USB_PHY_RESETS		(BIT(7) | BIT(15))
#define USB_NOC_IDLE		BIT(6)

enum charlotte_usb_bank {
	HSDT, PERI, PMC, SYSCTRL, MISC, SYSQIC, UTMI, NUM_BANKS,
};

static const char * const charlotte_bank_names[NUM_BANKS] = {
	"hsdt-crg", "peri-crg", "pmctrl", "sysctrl", "misc", "sysqic", "utmi",
};

struct charlotte_usb {
	struct dwc3 dwc;
	void __iomem *bank[NUM_BANKS];
	bool powered;
};

static DEFINE_MUTEX(charlotte_fw_lock);
static struct ffa_device *charlotte_fw;

static int charlotte_ffa_probe(struct ffa_device *fdev)
{
	fdev->ops->msg_ops->mode_32bit_set(fdev);
	mutex_lock(&charlotte_fw_lock);
	charlotte_fw = fdev;
	mutex_unlock(&charlotte_fw_lock);
	dev_info(&fdev->dev, "Charlotte USB platform service available\n");
	return 0;
}

static void charlotte_ffa_remove(struct ffa_device *fdev)
{
	mutex_lock(&charlotte_fw_lock);
	charlotte_fw = NULL;
	mutex_unlock(&charlotte_fw_lock);
}

static const struct ffa_device_id charlotte_ffa_ids[] = {
	{ .uuid = { .b = { 0x33, 0x5f, 0xe7, 0xf9, 0x82, 0x4f, 0xca, 0xa1,
			    0x02, 0xc8, 0xe6, 0x98, 0xb4, 0xec, 0x20, 0x0f } } },
	{ }
};
MODULE_DEVICE_TABLE(ffa, charlotte_ffa_ids);

static struct ffa_driver charlotte_ffa_driver = {
	.name = "charlotte-usb-firmware",
	.probe = charlotte_ffa_probe,
	.remove = charlotte_ffa_remove,
	.id_table = charlotte_ffa_ids,
};

static int charlotte_usb_grant_access(struct device *dev)
{
	struct ffa_send_direct_data args = { .data0 = CHARLOTTE_USB_TZPC };
	struct device_link *link;
	int ret;

	mutex_lock(&charlotte_fw_lock);
	if (!charlotte_fw) {
		ret = -EPROBE_DEFER;
		goto out;
	}
	link = device_link_add(dev, &charlotte_fw->dev,
			       DL_FLAG_AUTOREMOVE_CONSUMER);
	if (!link) {
		ret = -ENOMEM;
		goto out;
	}
	/* The vendor uses a 32-bit FF-A direct request, not a legacy SMC. */
	ret = charlotte_fw->ops->msg_ops->sync_send_receive(charlotte_fw, &args);
	if (!ret)
		dev_info(dev, "USB_TZPC FF-A response %#lx\n", args.data0);
out:
	mutex_unlock(&charlotte_fw_lock);
	return ret;
}

static void charlotte_usb_update_bits(struct charlotte_usb *usb,
				     enum charlotte_usb_bank bank, u32 offset,
				     u32 mask, u32 value)
{
	void __iomem *reg = usb->bank[bank] + offset;

	writel((readl(reg) & ~mask) | value, reg);
}

static int charlotte_usb_power_on(struct charlotte_usb *usb)
{
	struct device *dev = usb->dwc.dev;
	u32 val, memory_status;
	int ret;

	/* clk_hsdt1_usbdp: FLL -> and-gate -> divider -> write-one gate. */
	writel(BIT(28) | BIT(12), usb->bank[PERI] + 0x70c);
	writel(GENMASK(9, 8) << 16, usb->bank[PERI] + 0x710);
	writel(BIT(1), usb->bank[PERI] + 0x040);

	/* Release the misc reset before enabling its two bus clocks. */
	writel(BIT(29), usb->bank[HSDT] + HSDT_PERRSTDIS0);
	writel(USB_MISC_CLOCKS, usb->bank[HSDT] + HSDT_PEREN0);
	writel(USB_NOC_IDLE << 16, usb->bank[PMC] + 0x380);
	ret = readl_poll_timeout(usb->bank[PMC] + 0x384, val,
				!(val & USB_NOC_IDLE), 10, 100);
	if (ret)
		return dev_err_probe(dev, ret, "USB NOC power-idle exit timed out\n");

	dev_info(dev, "USB NOC active; configuring misc memory\n");
	writel(0, usb->bank[MISC] + 0x35c);
	memory_status = readl(usb->bank[SYSCTRL] + 0x00c);
	if (!(memory_status & BIT(15))) {
		charlotte_usb_update_bits(usb, MISC, 0x3c4, 0x18001800, 0x08000800);
		charlotte_usb_update_bits(usb, MISC, 0x3c8, 0x18001800, 0x08000800);
		charlotte_usb_update_bits(usb, MISC, 0x3cc, 0x18001800, 0x08000800);
	}
	if (!(memory_status & BIT(20)))
		charlotte_usb_update_bits(usb, SYSQIC, 0x104, 0x1800, 0x0800);

	/* Independent UTMI PHY; no USB3/DP combophy is needed at high speed. */
	dev_info(dev, "USB misc initialized; enabling UTMI PHY\n");
	writel(BIT(18) | BIT(2), usb->bank[HSDT] + 0x0a8);
	writel(USB_PHY_CLOCKS, usb->bank[HSDT] + HSDT_PEREN0);
	writel(BIT(7), usb->bank[HSDT] + HSDT_PERRSTDIS0);
	writel(BIT(15), usb->bank[HSDT] + HSDT_PERRSTDIS0);
	writel(1, usb->bank[MISC] + 0x004);
	writel(3, usb->bank[MISC] + 0x0bc);
	writel(0, usb->bank[MISC] + 0x008);
	writel(1, usb->bank[MISC] + 0x008);
	charlotte_usb_update_bits(usb, UTMI, 0x028, 0x7fff, 0x1cf5);
	charlotte_usb_update_bits(usb, UTMI, 0x020, 0x7fff, 0x6302);
	charlotte_usb_update_bits(usb, UTMI, 0x038, 0x0780, 0x0200);

	/* The DWC3 controller is released only after all six clocks are on. */
	writel(USB_CORE_CLOCKS, usb->bank[HSDT] + HSDT_PEREN0);
	writel(USB_CORE_RESETS, usb->bank[HSDT] + HSDT_PERRSTDIS0);
	/* v182 PROBE: DEPSTARTCFG passes but SETEPCONFIG hangs (-110) even
	 * with the hi-domain gates on -- brute-force every PEREN0 gate to
	 * bisect whether another clock bit still hides the fault. */
	writel(0xFFFFFFFF, usb->bank[HSDT] + HSDT_PEREN0);
	/* v182 PROBE: all PEREN0 gates on did not clear the DEPCMD hang.
	 * v183: brute-force release every hsdt1 peripheral reset and drop
	 * any output isolation on the domain as well. */
	writel(0xFFFFFFFF, usb->bank[HSDT] + HSDT_PERRSTDIS0);
	writel(0xFFFFFFFF, usb->bank[HSDT] + 0x284 /* ISODIS */);
	msleep(10);
	usb->powered = true;
	dev_info(dev, "USB2 peripheral hardware initialized\n");
	return 0;
}

static void charlotte_usb_power_off(struct charlotte_usb *usb)
{
	u32 val;
	int ret;

	if (usb->powered) {
		writel(1, usb->bank[MISC] + 0x24c);
		writel(0, usb->bank[MISC] + 0x004);
		writel(0, usb->bank[MISC] + 0x0bc);
		writel(0, usb->bank[MISC] + 0x008);
		writel(1, usb->bank[MISC] + 0x008);
	}
	writel(USB_CORE_RESETS | USB_PHY_RESETS,
	       usb->bank[HSDT] + HSDT_PERRSTEN0);
	writel((USB_NOC_IDLE << 16) | USB_NOC_IDLE, usb->bank[PMC] + 0x380);
	ret = readl_poll_timeout(usb->bank[PMC] + 0x384, val,
				val & USB_NOC_IDLE, 10, 100);
	if (ret)
		dev_warn(usb->dwc.dev, "USB NOC power-idle entry timed out\n");
	writel(BIT(29), usb->bank[HSDT] + HSDT_PERRSTEN0);
	writel(USB_CORE_CLOCKS | USB_PHY_CLOCKS | USB_MISC_CLOCKS,
	       usb->bank[HSDT] + HSDT_PERDIS0);
	writel(BIT(1), usb->bank[PERI] + 0x044);
	usb->powered = false;
}

static int charlotte_usb_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct dwc3_probe_data data = { .properties = DWC3_DEFAULT_PROPERTIES };
	struct charlotte_usb *usb;
	int i, ret;

	ret = charlotte_usb_grant_access(dev);
	if (ret)
		return dev_err_probe(dev, ret, "USB firmware access configuration failed\n");
	usb = devm_kzalloc(dev, sizeof(*usb), GFP_KERNEL);
	if (!usb)
		return -ENOMEM;
	usb->dwc.dev = dev;
	for (i = 0; i < NUM_BANKS; i++) {
		usb->bank[i] = devm_platform_ioremap_resource_byname(pdev,
							charlotte_bank_names[i]);
		if (IS_ERR(usb->bank[i]))
			return PTR_ERR(usb->bank[i]);
	}
	data.res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dwc3");
	if (!data.res)
		return -EINVAL;
	ret = charlotte_usb_power_on(usb);
	if (ret)
		goto power_off;
	data.dwc = &usb->dwc;
	data.ignore_clocks_and_resets = true;
	ret = dwc3_core_probe(&data);
	if (!ret) {
		/* v184: connect/pullup and UTMI state for the silent-host
		 * problem (gadget ready but no host-side enumeration). */
		dev_info(dev, "DCTL=0x%08x DSTS=0x%08x GUSB2PHYCFG=0x%08x\n",
			 dwc3_readl(&usb->dwc, DWC3_DCTL),
			 dwc3_readl(&usb->dwc, DWC3_DSTS),
			 dwc3_readl(&usb->dwc, DWC3_GUSB2PHYCFG(0)));
		return 0;
	}
power_off:
	charlotte_usb_power_off(usb);
	return ret;
}

static void charlotte_usb_remove(struct platform_device *pdev)
{
	struct dwc3 *dwc = platform_get_drvdata(pdev);
	struct charlotte_usb *usb = container_of(dwc, struct charlotte_usb, dwc);

	dwc3_core_remove(dwc);
	charlotte_usb_power_off(usb);
}

static int charlotte_usb_suspend(struct device *dev)
{
	return dwc3_pm_suspend(dev_get_drvdata(dev));
}

static int charlotte_usb_resume(struct device *dev)
{
	return dwc3_pm_resume(dev_get_drvdata(dev));
}

static int charlotte_usb_runtime_suspend(struct device *dev)
{
	return dwc3_runtime_suspend(dev_get_drvdata(dev));
}

static int charlotte_usb_runtime_resume(struct device *dev)
{
	return dwc3_runtime_resume(dev_get_drvdata(dev));
}

static int charlotte_usb_runtime_idle(struct device *dev)
{
	return dwc3_runtime_idle(dev_get_drvdata(dev));
}

static const struct dev_pm_ops charlotte_usb_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(charlotte_usb_suspend, charlotte_usb_resume)
	RUNTIME_PM_OPS(charlotte_usb_runtime_suspend, charlotte_usb_runtime_resume,
		       charlotte_usb_runtime_idle)
};

static const struct of_device_id charlotte_usb_match[] = {
	{ .compatible = "hisilicon,charlotte-dwc3" },
	{ }
};
MODULE_DEVICE_TABLE(of, charlotte_usb_match);

static struct platform_driver charlotte_usb_driver = {
	.probe = charlotte_usb_probe,
	.remove = charlotte_usb_remove,
	.driver = {
		.name = "dwc3-charlotte",
		.of_match_table = charlotte_usb_match,
		.pm = pm_ptr(&charlotte_usb_pm_ops),
	},
};

static int __init charlotte_usb_init(void)
{
	int ret = ffa_register(&charlotte_ffa_driver);

	if (ret)
		return ret;
	ret = platform_driver_register(&charlotte_usb_driver);
	if (ret)
		ffa_unregister(&charlotte_ffa_driver);
	return ret;
}
module_init(charlotte_usb_init);

static void __exit charlotte_usb_exit(void)
{
	platform_driver_unregister(&charlotte_usb_driver);
	ffa_unregister(&charlotte_ffa_driver);
}
module_exit(charlotte_usb_exit);

MODULE_DESCRIPTION("Charlotte USB2 peripheral platform driver");
MODULE_LICENSE("GPL");
