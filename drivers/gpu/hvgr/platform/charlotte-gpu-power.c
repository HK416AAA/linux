// SPDX-License-Identifier: GPL-2.0-only
/* Charlotte's g3d IP power vote, matching stock ip-regulator-atf ID 13. */
#include <linux/firmware/charlotte-ffa.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>

struct charlotte_gpu_power {
	struct device *dev;
	bool enabled;
};

static int charlotte_gpu_power_enable(struct regulator_dev *rdev)
{
	struct charlotte_gpu_power *power = rdev_get_drvdata(rdev);
	int ret = charlotte_ffa_gpu_power(power->dev, true);

	if (!ret)
		power->enabled = true;
	return ret;
}

static int charlotte_gpu_power_disable(struct regulator_dev *rdev)
{
	struct charlotte_gpu_power *power = rdev_get_drvdata(rdev);
	int ret = charlotte_ffa_gpu_power(power->dev, false);

	if (!ret)
		power->enabled = false;
	return ret;
}

static int charlotte_gpu_power_is_enabled(struct regulator_dev *rdev)
{
	struct charlotte_gpu_power *power = rdev_get_drvdata(rdev);

	/* The firmware API has no state query; this is our successful vote. */
	return power->enabled;
}

static const struct regulator_ops charlotte_gpu_power_ops = {
	.enable = charlotte_gpu_power_enable,
	.disable = charlotte_gpu_power_disable,
	.is_enabled = charlotte_gpu_power_is_enabled,
};

static const struct regulator_desc charlotte_gpu_power_desc = {
	.name = "charlotte-g3d", .id = 13,
	.type = REGULATOR_VOLTAGE, .owner = THIS_MODULE,
	.ops = &charlotte_gpu_power_ops,
};

static int charlotte_gpu_power_probe(struct platform_device *pdev)
{
	struct regulator_config config = { .dev = &pdev->dev };
	struct charlotte_gpu_power *power;
	struct regulator_dev *rdev;

	power = devm_kzalloc(&pdev->dev, sizeof(*power), GFP_KERNEL);
	if (!power)
		return -ENOMEM;
	power->dev = &pdev->dev;
	config.driver_data = power;
	config.of_node = pdev->dev.of_node;
	config.init_data = of_get_regulator_init_data(&pdev->dev,
		pdev->dev.of_node, &charlotte_gpu_power_desc);
	rdev = devm_regulator_register(&pdev->dev, &charlotte_gpu_power_desc, &config);
	return PTR_ERR_OR_ZERO(rdev);
}

static const struct of_device_id charlotte_gpu_power_match[] = {
	{ .compatible = "hisilicon,charlotte-gpu-power" }, { }
};

static struct platform_driver charlotte_gpu_power_driver = {
	.probe = charlotte_gpu_power_probe,
	.driver = {
		.name = "charlotte-gpu-power",
		.of_match_table = charlotte_gpu_power_match,
	},
};

static int __init charlotte_gpu_power_init(void)
{
	return platform_driver_register(&charlotte_gpu_power_driver);
}
subsys_initcall(charlotte_gpu_power_init);
