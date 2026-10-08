// SPDX-License-Identifier: GPL-2.0-only
/* Charlotte stores the boot target in the main PMIC before an EL3 reset. */
#include <linux/arm-smccc.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/printk.h>
#include <linux/reboot.h>
#include <linux/reboot-mode.h>
#include <linux/spmi.h>
#include <uapi/linux/psci.h>

#define CHARLOTTE_PMIC_REBOOT_REASON	0x428
#define CHARLOTTE_POWERHOLD_DATA		0x000
#define CHARLOTTE_POWERHOLD_CTRL		0x048
#define CHARLOTTE_GPIO_SOURCE_DATA	0x0dc

struct charlotte_reboot {
	struct spmi_device *sdev;
	struct reboot_mode_driver mode;
	struct notifier_block prepare;
	void __iomem *powerhold_gpio;
	void __iomem *powerhold_protect;
	bool mode_written;
};

static int charlotte_reboot_mode_write(struct reboot_mode_driver *mode,
				       unsigned int magic)
{
	struct charlotte_reboot *reboot = container_of(mode, struct charlotte_reboot,
						     mode);
	u8 reason = magic, readback;
	int ret;

	if (magic > U8_MAX)
		return -EINVAL;

	ret = spmi_ext_register_writel(reboot->sdev, CHARLOTTE_PMIC_REBOOT_REASON,
				      &reason, 1);
	if (!ret)
		ret = spmi_ext_register_readl(reboot->sdev,
					     CHARLOTTE_PMIC_REBOOT_REASON,
					     &readback, 1);
	if (!ret && readback != reason)
		ret = -EIO;
	reboot->mode_written = !ret;
	if (ret)
		dev_err(mode->dev, "cannot record reboot reason %#x: %d\n", magic, ret);
	else
		dev_emerg(mode->dev, "reboot reason %#x recorded and verified\n", magic);

	return ret;
}

static int charlotte_reboot_prepare(struct notifier_block *nb,
				    unsigned long action, void *cmd)
{
	struct charlotte_reboot *reboot = container_of(nb, struct charlotte_reboot,
						     prepare);

	if (action != SYS_RESTART && action != SYS_POWER_OFF)
		return NOTIFY_DONE;

	/* Keep the shutdown callback names visible while bringing up restart. */
	console_verbose();
	initcall_debug = true;
	if (action == SYS_POWER_OFF) {
		pr_emerg("charlotte-poweroff: preparing; next device shutdown\n");
		/* Vendor dfx_pm_system_off records AP_S_COLDBOOT (0x00). */
		charlotte_reboot_mode_write(&reboot->mode, 0x00);
	} else {
		pr_emerg("charlotte-reboot: preparing '%s'; next device shutdown\n",
			 cmd ? (char *)cmd : "normal");
	}
	return NOTIFY_DONE;
}

static int charlotte_reboot_devices_done(struct sys_off_data *data)
{
	pr_emerg("charlotte-reboot: device shutdown complete; next syscore shutdown\n");
	return NOTIFY_DONE;
}

static int charlotte_restart(struct sys_off_data *data)
{
	struct charlotte_reboot *reboot = data->cb_data;
	struct arm_smccc_res res;

	if (!reboot->mode_written) {
		pr_emerg("charlotte-reboot: boot target was not recorded; reset skipped\n");
		return NOTIFY_DONE;
	}

	/* The vendor Charlotte reset path uses SMC, independently of CPU PSCI. */
	pr_emerg("charlotte-reboot: issuing PSCI SYSTEM_RESET via SMC\n");
	arm_smccc_1_1_smc(PSCI_0_2_FN_SYSTEM_RESET, 0, 0, 0, &res);
	pr_emerg("charlotte-reboot: SYSTEM_RESET returned %#lx\n", res.a0);
	return NOTIFY_DONE;
}

static int charlotte_power_off(struct sys_off_data *data)
{
	struct charlotte_reboot *reboot = data->cb_data;
	struct arm_smccc_res res;
	u32 value;

	/*
	 * Charlotte's vendor kernel clears SCTRL powerhold protection, then
	 * drives V500 GPIO176 low. EL1 SYSTEM_OFF is unsupported on this firmware.
	 * Use the firmware's pinmux/clock setup until the GPIO driver is ported.
	 */
	pr_emerg("charlotte-poweroff: releasing GPIO176 power hold\n");
	value = readl(reboot->powerhold_protect);
	writel((value & ~BIT(15)) | BIT(31), reboot->powerhold_protect);
	readl(reboot->powerhold_protect);
	/* High-half GPIO data: bit 16 masks the value for GPIO offset 16. */
	writel(BIT(16), reboot->powerhold_gpio + CHARLOTTE_POWERHOLD_DATA);
	/* V500 HW_SW_SEL=4, with its write mask at bit 20. */
	writel(BIT(20) | BIT(4), reboot->powerhold_gpio + CHARLOTTE_POWERHOLD_CTRL);
	readl(reboot->powerhold_gpio + CHARLOTTE_GPIO_SOURCE_DATA);
	mdelay(1000);

	/* Vendor also falls back to restart if powerhold does not remove power. */
	pr_emerg("charlotte-poweroff: power remains; GPIO source=%#x ctrl=%#x protect=%#x\n",
		 readl(reboot->powerhold_gpio + CHARLOTTE_GPIO_SOURCE_DATA),
		 readl(reboot->powerhold_gpio + CHARLOTTE_POWERHOLD_CTRL),
		 readl(reboot->powerhold_protect));
	/* Distinguish the restart fallback from a charger-driven cold power-on. */
	charlotte_reboot_mode_write(&reboot->mode, 0x01);
	pr_emerg("charlotte-poweroff: issuing fallback SYSTEM_RESET to bootloader\n");
	arm_smccc_1_1_smc(PSCI_0_2_FN_SYSTEM_RESET, 0, 0, 0, &res);
	pr_emerg("charlotte-poweroff: fallback SYSTEM_RESET returned %#lx\n", res.a0);
	return NOTIFY_DONE;
}

static int charlotte_powerhold_init(struct charlotte_reboot *reboot)
{
	struct device *dev = &reboot->sdev->dev;
	struct device_node *np;
	struct resource gpio, protect;
	int ret;

	np = of_parse_phandle(dev->of_node, "hisilicon,powerhold", 0);
	if (!np)
		return -ENODEV;
	ret = of_address_to_resource(np, 0, &gpio);
	if (!ret)
		ret = of_address_to_resource(np, 1, &protect);
	of_node_put(np);
	if (ret)
		return ret;
	if (resource_size(&gpio) <= CHARLOTTE_GPIO_SOURCE_DATA ||
	    resource_size(&protect) < sizeof(u32))
		return -EINVAL;
	/* SCTRL is shared with the USB glue; map only the protection register. */
	reboot->powerhold_gpio = devm_ioremap(dev, gpio.start, resource_size(&gpio));
	reboot->powerhold_protect = devm_ioremap(dev, protect.start,
					      resource_size(&protect));
	if (!reboot->powerhold_gpio || !reboot->powerhold_protect)
		return -ENOMEM;
	dev_info(dev, "powerhold GPIO176: source=%#x ctrl=%#x protect=%#x\n",
		 readl(reboot->powerhold_gpio + CHARLOTTE_GPIO_SOURCE_DATA),
		 readl(reboot->powerhold_gpio + CHARLOTTE_POWERHOLD_CTRL),
		 readl(reboot->powerhold_protect));
	return 0;
}

static int charlotte_reboot_probe(struct spmi_device *sdev)
{
	struct device *dev = &sdev->dev;
	struct charlotte_reboot *reboot;
	u8 reason;
	int ret;

	reboot = devm_kzalloc(dev, sizeof(*reboot), GFP_KERNEL);
	if (!reboot)
		return -ENOMEM;
	reboot->sdev = sdev;

	ret = spmi_ext_register_readl(sdev, CHARLOTTE_PMIC_REBOOT_REASON, &reason, 1);
	if (ret)
		return dev_err_probe(dev, ret, "cannot read PMIC reboot reason\n");

	reboot->mode.dev = dev;
	reboot->mode.write = charlotte_reboot_mode_write;
	/*
	 * Arm the boot target while SPMI is healthy, before risky device probes.
	 * Normal reboot-mode notifications replace this target before reset.
	 */
	if (of_property_read_bool(dev->of_node, "hisilicon,watchdog-recovery-bootloader")) {
		ret = charlotte_reboot_mode_write(&reboot->mode, 0x01);
		if (ret)
			return dev_err_probe(dev, ret, "cannot arm watchdog fastboot recovery\n");
		dev_info(dev, "watchdog recovery target: bootloader\n");
	}
	ret = devm_reboot_mode_register(dev, &reboot->mode);
	if (ret)
		return ret;

	reboot->prepare.notifier_call = charlotte_reboot_prepare;
	/* Run after reboot-mode so poweroff's COLDBOOT reason is not overwritten. */
	reboot->prepare.priority = -255;
	ret = devm_register_reboot_notifier(dev, &reboot->prepare);
	if (ret)
		return ret;

	ret = devm_register_sys_off_handler(dev, SYS_OFF_MODE_RESTART_PREPARE,
					  SYS_OFF_PRIO_DEFAULT,
					  charlotte_reboot_devices_done, reboot);
	if (ret)
		return ret;
	ret = devm_register_sys_off_handler(dev, SYS_OFF_MODE_RESTART, 192,
					  charlotte_restart, reboot);
	if (ret)
		return ret;
	ret = charlotte_powerhold_init(reboot);
	if (ret)
		return dev_err_probe(dev, ret, "cannot map powerhold resources\n");
	ret = devm_register_sys_off_handler(dev, SYS_OFF_MODE_POWER_OFF, 192,
					  charlotte_power_off, reboot);
	if (ret)
		return ret;

	dev_info(dev, "PMIC reason %#x; boot modes, SMC reset and GPIO poweroff ready\n",
		 reason);
	return 0;
}

static const struct of_device_id charlotte_reboot_of_match[] = {
	{ .compatible = "hisilicon,charlotte-reboot" },
	{ }
};
MODULE_DEVICE_TABLE(of, charlotte_reboot_of_match);

static struct spmi_driver charlotte_reboot_driver = {
	.probe = charlotte_reboot_probe,
	.driver = {
		.name = "charlotte-reboot",
		.of_match_table = charlotte_reboot_of_match,
	},
};
module_spmi_driver(charlotte_reboot_driver);

MODULE_DESCRIPTION("Charlotte PMIC boot target, restart and poweroff");
MODULE_LICENSE("GPL");
