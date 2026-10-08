// SPDX-License-Identifier: GPL-2.0-only
/* Huawei Charlotte HUFS, using the PHY initialized by the boot firmware. */
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <ufs/ufshcd.h>
#include <ufs/unipro.h>
#include "ufshcd-pltfrm.h"

#define HUFS_UTRD_SIZE	48
#define HUFS_AHIT_CTRL	0x110
#define HUFS_AH8_STATE	0x130
#define HUFS_DME_LINK_REQUEST	0xd008
#define HUFS_DME_LINK_STATE	0xd009
#define HUFS_DME_PWR_STATE	0xd011
#define HUFS_DME_INTR_RAW	0xd049
#define HUFS_DME_INTR_ENABLE	0xd050
#define HUFS_DME_INTR_CLEAR	0xd051
#define HUFS_DME_STATE	0xd053
#define HUFS_DME_LOCAL_STATE	0xd066
#define HUFS_DME_PEER_CTRL	0xd060
#define HUFS_DME_PEER_STATE	0xd061
#define HUFS_DME_PEER_WRITE	0xd062
#define HUFS_DME_PEER_READ	0xd063

struct charlotte_ufs {
	void __iomem *unipro;
	bool firmware_adopted;
};

static bool probe_only;
module_param(probe_only, bool, 0444);
MODULE_PARM_DESC(probe_only, "Only capture the firmware register state");

static int charlotte_ufs_snapshot(struct platform_device *pdev)
{
	void __iomem *hci, *unipro;
	u32 offset;

	hci = devm_platform_ioremap_resource_byname(pdev, "hci");
	if (IS_ERR(hci))
		return PTR_ERR(hci);
	unipro = devm_platform_ioremap_resource_byname(pdev, "unipro");
	if (IS_ERR(unipro))
		return PTR_ERR(unipro);

	dev_info(&pdev->dev, "HUFS firmware handoff register snapshot\n");
	for (offset = 0; offset <= 0x70; offset += 4)
		dev_info(&pdev->dev, "HCI[%#04x]=%#010x\n", offset, readl(hci + offset));
	dev_info(&pdev->dev, "AH8 state=%#x; UniPro layer=%#x state=%#x\n",
		 readl(hci + 0x130), readl(unipro + (0xd003 << 2)),
		 readl(unipro + (0xd053 << 2)));
	dev_info(&pdev->dev, "snapshot complete; transport integration pending\n");
	return 0;
}

static u32 charlotte_uic_read(struct ufs_hba *hba, u32 address)
{
	struct charlotte_ufs *host = ufshcd_get_variant(hba);

	return readl(host->unipro + (address << 2));
}

static int charlotte_uic_access(struct ufs_hba *hba, u32 address, u32 *value,
				bool write)
{
	struct charlotte_ufs *host = ufshcd_get_variant(hba);
	u32 state;
	int tries;

	for (tries = 0; tries < 32; tries++) {
		if (write)
			writel(*value, host->unipro + (address << 2));
		else
			*value = readl(host->unipro + (address << 2));
		state = charlotte_uic_read(hba, HUFS_DME_LOCAL_STATE) & 0xf;
		if (state != 9)
			return state;
		udelay(2);
	}
	return -EBUSY;
}

static int charlotte_uic_write(struct ufs_hba *hba, u32 address, u32 value)
{
	return charlotte_uic_access(hba, address, &value, true);
}

static u32 charlotte_uic_address(u32 selector, bool peer)
{
	u32 lane = selector & 0xffff;
	u32 attr = selector >> 16;

	if (selector == 0xd0850000)
		return 0xd014;
	if (!peer && lane >= 4 && lane <= 5)
		lane -= 2;
	return (lane << 16) | attr;
}

static int charlotte_uic_poll(struct ufs_hba *hba, u32 address,
			      u32 mask, u32 expected)
{
	u32 value;
	int ret;

	ret = read_poll_timeout(charlotte_uic_read, value,
				(value & mask) == expected, 20, 1000000,
				false, hba, address);
	if (ret)
		dev_err(hba->dev, "UniPro[%#x]=%#x, expected %#x/%#x\n",
			address, value, expected, mask);
	return ret;
}

static int charlotte_peer_cmd(struct ufs_hba *hba, struct uic_command *cmd,
			       u32 address)
{
	bool write = cmd->command == UIC_CMD_DME_PEER_SET;
	u32 value;
	int ret;

	charlotte_uic_write(hba, HUFS_DME_INTR_CLEAR, BIT(23));
	if (write) {
		ret = charlotte_uic_write(hba, HUFS_DME_PEER_WRITE, cmd->argument3);
		if (ret)
			return ret;
	}
	ret = charlotte_uic_write(hba, HUFS_DME_PEER_CTRL,
				address | BIT(31) | (write ? BIT(28) : 0));
	if (ret)
		return ret;
	ret = charlotte_uic_poll(hba, HUFS_DME_INTR_RAW, BIT(23), BIT(23));
	if (ret)
		return ret;
	value = charlotte_uic_read(hba, HUFS_DME_PEER_STATE);
	if (value & 0xf)
		return value & 0xf;
	if (!write)
		cmd->argument3 = charlotte_uic_read(hba, HUFS_DME_PEER_READ);
	charlotte_uic_write(hba, HUFS_DME_INTR_CLEAR, BIT(23));
	return 0;
}

static int charlotte_uic_cmd(struct ufs_hba *hba, struct uic_command *cmd)
{
	struct charlotte_ufs *host = ufshcd_get_variant(hba);
	u32 address = charlotte_uic_address(cmd->argument1,
			cmd->command == UIC_CMD_DME_PEER_GET ||
			cmd->command == UIC_CMD_DME_PEER_SET);
	u32 state;
	int ret;

	switch (cmd->command) {
	case UIC_CMD_DME_RESET:
		/* Initial handoff only: a real error reset needs the SoC reset path. */
		if (host->firmware_adopted ||
		    !(ufshcd_readl(hba, REG_CONTROLLER_ENABLE) & CONTROLLER_ENABLE) ||
		    (charlotte_uic_read(hba, HUFS_DME_STATE) & 0x3f80) != 0x100)
			return -EOPNOTSUPP;
		host->firmware_adopted = true;
		dev_info(hba->dev, "adopting the firmware's active UniPro link\n");
		return 0;
	case UIC_CMD_DME_ENABLE:
		return charlotte_uic_poll(hba, 0xd003, BIT(0), BIT(0));
	case UIC_CMD_DME_GET:
		return charlotte_uic_access(hba, address, &cmd->argument3, false);
	case UIC_CMD_DME_SET:
		if (address == PA_PWRMODE)
			charlotte_uic_write(hba, HUFS_DME_INTR_CLEAR, BIT(5));
		ret = charlotte_uic_write(hba, address, cmd->argument3);
		if (ret || address != PA_PWRMODE)
			return ret;
		ret = charlotte_uic_poll(hba, HUFS_DME_INTR_RAW, BIT(5), BIT(5));
		if (ret)
			return ret;
		state = charlotte_uic_read(hba, HUFS_DME_PWR_STATE);
		charlotte_uic_write(hba, HUFS_DME_INTR_CLEAR, BIT(5));
		ufshcd_writel(hba, UIC_POWER_MODE, REG_INTERRUPT_STATUS);
		/* HUFS confirms host-initiated PMC with LOCAL_SUCC (vendor ISR). */
		return state & BIT(1) ? 0 : -EIO;
	case UIC_CMD_DME_PEER_GET:
	case UIC_CMD_DME_PEER_SET:
		return charlotte_peer_cmd(hba, cmd, address);
	case UIC_CMD_DME_LINK_STARTUP:
		state = charlotte_uic_read(hba, HUFS_DME_STATE) & 0x3f80;
		if (state == 0x100)
			return 0;
		charlotte_uic_write(hba, HUFS_DME_INTR_CLEAR, BIT(19));
		ret = charlotte_uic_write(hba, HUFS_DME_LINK_REQUEST, 1);
		if (ret)
			return ret;
		ret = charlotte_uic_poll(hba, HUFS_DME_LINK_STATE, 3, 1);
		charlotte_uic_write(hba, HUFS_DME_INTR_CLEAR, BIT(19));
		return ret;
	case UIC_CMD_DME_HIBER_ENTER:
		charlotte_uic_write(hba, HUFS_DME_INTR_CLEAR, BIT(3) | BIT(20));
		ret = charlotte_uic_write(hba, 0xd00b, 1);
		if (ret)
			return ret;
		ret = charlotte_uic_poll(hba, 0xd00c, BIT(0), BIT(0));
		if (!ret)
			ret = charlotte_uic_poll(hba, 0xd00d, BIT(1), BIT(1));
		charlotte_uic_write(hba, HUFS_DME_INTR_CLEAR, BIT(3) | BIT(20));
		ufshcd_writel(hba, UIC_HIBERNATE_ENTER | UIC_ERROR, REG_INTERRUPT_STATUS);
		return ret;
	case UIC_CMD_DME_HIBER_EXIT:
		charlotte_uic_write(hba, HUFS_DME_INTR_CLEAR, BIT(4) | BIT(21));
		ret = charlotte_uic_write(hba, 0xd00e, 1);
		if (ret)
			return ret;
		ret = charlotte_uic_poll(hba, 0xd00f, BIT(0), BIT(0));
		if (!ret)
			ret = charlotte_uic_poll(hba, 0xd010, BIT(1), BIT(1));
		charlotte_uic_write(hba, HUFS_DME_INTR_CLEAR, BIT(4) | BIT(21));
		ufshcd_writel(hba, UIC_HIBERNATE_EXIT | UIC_ERROR, REG_INTERRUPT_STATUS);
		return ret;
	default:
		return -ENOIOCTLCMD;
	}
}

static int charlotte_ufs_variant_init(struct ufs_hba *hba)
{
	struct platform_device *pdev = to_platform_device(hba->dev);
	struct ufs_hba_variant_params *params;
	struct charlotte_ufs *host;

	host = devm_kzalloc(hba->dev, sizeof(*host), GFP_KERNEL);
	if (!host)
		return -ENOMEM;
	host->unipro = devm_platform_ioremap_resource_byname(pdev, "unipro");
	if (IS_ERR(host->unipro))
		return PTR_ERR(host->unipro);
	params = devm_kmemdup(hba->dev, hba->vps, sizeof(*params), GFP_KERNEL);
	if (!params)
		return -ENOMEM;
	params->utrd_size = HUFS_UTRD_SIZE;
	hba->vps = params;
	ufshcd_set_variant(hba, host);
	hba->quirks |= UFSHCI_QUIRK_BROKEN_HCE |
		UFSHCD_QUIRK_BROKEN_AUTO_HIBERN8 |
		UFSHCD_QUIRK_SKIP_DEF_UNIPRO_TIMEOUT_SETTING;
	/* Keep the boot firmware's rails and PHY clocks on during bring-up. */
	hba->rpm_lvl = UFS_PM_LVL_0;
	hba->spm_lvl = UFS_PM_LVL_0;
	ufshcd_writel(hba, ufshcd_readl(hba, HUFS_AHIT_CTRL) & ~BIT(0), HUFS_AHIT_CTRL);
	charlotte_uic_write(hba, HUFS_DME_INTR_ENABLE, 0);
	charlotte_uic_write(hba, HUFS_DME_INTR_CLEAR, ~0U);
	dev_info(hba->dev, "HUFS transport: 48-byte UTRD, direct UniPro, firmware PHY\n");
	return 0;
}

static int charlotte_ufs_dma_mask(struct ufs_hba *hba)
{
	return dma_set_mask_and_coherent(hba->dev, DMA_BIT_MASK(32));
}

static int charlotte_ufs_hce_notify(struct ufs_hba *hba,
				  enum ufs_notify_change_status status)
{
	if (status == POST_CHANGE) {
		/* Direct UniPro commands poll their own completion registers. */
		ufshcd_writel(hba, ufshcd_readl(hba, REG_INTERRUPT_ENABLE) &
			      ~UFSHCD_UIC_MASK, REG_INTERRUPT_ENABLE);
		ufshcd_writel(hba, UFSHCD_UIC_MASK, REG_INTERRUPT_STATUS);
	}
	return 0;
}

static int charlotte_ufs_negotiate(struct ufs_hba *hba,
				 const struct ufs_pa_layer_attr *desired,
				 struct ufs_pa_layer_attr *final)
{
	/* Retain the firmware's calibrated PHY operating point during handoff. */
	*final = hba->pwr_info;
	return 0;
}

static int charlotte_ufs_link_notify(struct ufs_hba *hba,
				   enum ufs_notify_change_status status)
{
	struct ufs_pa_layer_attr *pwr = &hba->pwr_info;
	u32 mode;
	int ret;

	if (status != POST_CHANGE)
		return 0;
#define READ_PWR(field, attr) do { \
	ret = charlotte_uic_access(hba, attr, &pwr->field, false); \
	if (ret) \
		return ret; \
} while (0)
	READ_PWR(gear_rx, PA_RXGEAR);
	READ_PWR(gear_tx, PA_TXGEAR);
	READ_PWR(lane_rx, PA_ACTIVERXDATALANES);
	READ_PWR(lane_tx, PA_ACTIVETXDATALANES);
	READ_PWR(hs_rate, PA_HSSERIES);
#undef READ_PWR
	ret = charlotte_uic_access(hba, PA_PWRMODE, &mode, false);
	if (ret)
		return ret;
	pwr->pwr_rx = mode >> 4;
	pwr->pwr_tx = mode & 0xf;
	if (!pwr->gear_rx || !pwr->gear_tx || !pwr->lane_rx || !pwr->lane_tx)
		return -EINVAL;
	dev_info(hba->dev, "firmware PHY: RX G%u/%u lanes, TX G%u/%u lanes, mode %#x rate %u\n",
		 pwr->gear_rx, pwr->lane_rx, pwr->gear_tx, pwr->lane_tx,
		 mode, pwr->hs_rate);
	return 0;
}

static void charlotte_ufs_debug(struct ufs_hba *hba)
{
	dev_err(hba->dev, "HUFS HCS=%#x HCE=%#x DB=%#x AH8=%#x DME=%#x local=%#x\n",
		ufshcd_readl(hba, REG_CONTROLLER_STATUS),
		ufshcd_readl(hba, REG_CONTROLLER_ENABLE),
		ufshcd_readl(hba, REG_UTP_TRANSFER_REQ_DOOR_BELL),
		ufshcd_readl(hba, HUFS_AH8_STATE),
		charlotte_uic_read(hba, HUFS_DME_STATE),
		charlotte_uic_read(hba, HUFS_DME_LOCAL_STATE));
}

static const struct ufs_hba_variant_ops charlotte_ufs_vops = {
	.name = "charlotte-hufs",
	.init = charlotte_ufs_variant_init,
	.set_dma_mask = charlotte_ufs_dma_mask,
	.hce_enable_notify = charlotte_ufs_hce_notify,
	.link_startup_notify = charlotte_ufs_link_notify,
	.uic_cmd = charlotte_uic_cmd,
	.negotiate_pwr_mode = charlotte_ufs_negotiate,
	.dbg_register_dump = charlotte_ufs_debug,
};

static int charlotte_ufs_probe(struct platform_device *pdev)
{
	if (probe_only)
		return charlotte_ufs_snapshot(pdev);
	return ufshcd_pltfrm_init(pdev, &charlotte_ufs_vops);
}

static void charlotte_ufs_remove(struct platform_device *pdev)
{
	if (!probe_only)
		ufshcd_pltfrm_remove(pdev);
}

static const struct of_device_id charlotte_ufs_of_match[] = {
	{ .compatible = "hisilicon,charlotte-ufs" },
	{ }
};
MODULE_DEVICE_TABLE(of, charlotte_ufs_of_match);

static struct platform_driver charlotte_ufs_driver = {
	.probe = charlotte_ufs_probe,
	.remove = charlotte_ufs_remove,
	.driver = {
		.name = "ufs-charlotte",
		.of_match_table = charlotte_ufs_of_match,
	},
};

static int __init charlotte_ufs_init(void)
{
	return platform_driver_register(&charlotte_ufs_driver);
}
late_initcall(charlotte_ufs_init);

static void __exit charlotte_ufs_exit(void)
{
	platform_driver_unregister(&charlotte_ufs_driver);
}
module_exit(charlotte_ufs_exit);

MODULE_DESCRIPTION("Huawei Charlotte HUFS controller");
MODULE_LICENSE("GPL");
