// SPDX-License-Identifier: GPL-2.0-only
/* Charlotte stores the boot target in the main PMIC before an EL3 reset. */
#include <linux/arm-smccc.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/printk.h>
#include <linux/reboot.h>
#include <linux/reboot-mode.h>
#include <linux/spmi.h>
#include <uapi/linux/psci.h>

#define CHARLOTTE_PMIC_REBOOT_REASON	0x428

struct charlotte_reboot {
	struct spmi_device *sdev;
	struct reboot_mode_driver mode;
	struct notifier_block prepare;
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
	if (action != SYS_RESTART)
		return NOTIFY_DONE;

	/* Keep the shutdown callback names visible while bringing up restart. */
	console_verbose();
	initcall_debug = true;
	pr_emerg("charlotte-reboot: preparing '%s'; next device shutdown\n",
		 cmd ? (char *)cmd : "normal");
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
	ret = devm_reboot_mode_register(dev, &reboot->mode);
	if (ret)
		return ret;

	reboot->prepare.notifier_call = charlotte_reboot_prepare;
	reboot->prepare.priority = 255;
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

	dev_info(dev, "PMIC reason %#x; normal/bootloader modes and SMC reset ready\n",
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

MODULE_DESCRIPTION("Charlotte PMIC boot target and system restart");
MODULE_LICENSE("GPL");
