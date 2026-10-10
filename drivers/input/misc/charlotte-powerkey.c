// SPDX-License-Identifier: GPL-2.0-only
/* Stock pmic_spmi.c / ponkey: SID9, IRQ0 0x2b3, mask0 0x290,
 * bit7 down / bit6 up. GPIO237 carries the active-low PMIC IRQ.
 * This is the first primary-PMIC IRQ consumer in the Charlotte port. Like
 * vendor pmic_irq_prc(), mask unclaimed IRQs before enabling key events.
 * Other PMIC consumers will need a shared IRQ domain; fault/reset policy
 * registers are not touched here.
 */
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/spmi.h>
#include <linux/workqueue.h>

#define KEY_STATUS 0x2b3
#define KEY_MASK 0x290
#define KEY_DOWN BIT(7)
#define KEY_UP BIT(6)
#define KEY_BITS (KEY_DOWN | KEY_UP)
#define VOLUME_STATUS 0x2c1
#define VOLUME_MASK 0x29e
#define VOLUME_BITS GENMASK(3, 0)
#define VOLUME_LEVEL 0x008
#define VOLUME_UP_LEVEL BIT(1)
#define VOLUME_DOWN_LEVEL BIT(2)
#define VOLUME_DEBOUNCE_MS 15

static const u16 pmic_masks[] = { 0x290, 0x291, 0x29d, 0x29e, 0x29f };

struct charlotte_powerkey {
	struct spmi_device *pmic;
	struct input_dev *input;
	struct input_dev *volume;
	struct delayed_work volume_work;
	int irq;
	u8 saved_masks[ARRAY_SIZE(pmic_masks)];
};

static void charlotte_volume_work(struct work_struct *work)
{
	struct charlotte_powerkey *key = container_of(to_delayed_work(work),
					struct charlotte_powerkey, volume_work);
	u8 level;
	int ret;

	/* PMIC21v900: status3 bits0/1=up release/press, 2/3=down
	 * release/press; read live active-low levels after vendor's 15ms filter.
	 */
	ret = spmi_ext_register_readl(key->pmic, VOLUME_LEVEL, &level, 1);
	if (ret) {
		dev_err_ratelimited(key->input->dev.parent, "volume level read failed: %d\n", ret);
		/* Stop autorepeat if a bus failure prevents observing release. */
		input_report_key(key->volume, KEY_VOLUMEUP, 0);
		input_report_key(key->volume, KEY_VOLUMEDOWN, 0);
	} else {
		input_report_key(key->volume, KEY_VOLUMEUP, !(level & VOLUME_UP_LEVEL));
		input_report_key(key->volume, KEY_VOLUMEDOWN, !(level & VOLUME_DOWN_LEVEL));
	}
	input_sync(key->volume);
}

static void charlotte_volume_cancel(void *data)
{
	struct charlotte_powerkey *key = data;

	cancel_delayed_work_sync(&key->volume_work);
}

static void charlotte_pmic_put(void *data)
{
	spmi_device_put(data);
}

static void charlotte_key_restore_mask(void *data)
{
	struct charlotte_powerkey *key = data;
	int i;

	for (i = 0; i < ARRAY_SIZE(pmic_masks); i++)
		spmi_ext_register_writel(key->pmic, pmic_masks[i],
					&key->saved_masks[i], 1);
}

static irqreturn_t charlotte_key_irq(int irq, void *data)
{
	struct charlotte_powerkey *key = data;
	u8 pending, volume = 0;
	int ret;

	ret = spmi_ext_register_readl(key->pmic, KEY_STATUS, &pending, 1);
	if (ret)
		goto failed;
	pending &= KEY_BITS;
	if (key->volume) {
		ret = spmi_ext_register_readl(key->pmic, VOLUME_STATUS, &volume, 1);
		if (ret)
			goto failed;
		volume &= VOLUME_BITS;
	}
	if (!pending && !volume)
		return IRQ_NONE;
	/* W1C only our bits. If both latched, down must precede up. */
	if (pending) {
		ret = spmi_ext_register_writel(key->pmic, KEY_STATUS, &pending, 1);
		if (ret)
			goto failed;
	}
	if (volume) {
		ret = spmi_ext_register_writel(key->pmic, VOLUME_STATUS, &volume, 1);
		if (ret)
			goto failed;
		mod_delayed_work(system_wq, &key->volume_work,
				 msecs_to_jiffies(VOLUME_DEBOUNCE_MS));
	}
	pm_wakeup_event(key->input->dev.parent, 1000);
	if (pending & KEY_DOWN) {
		input_report_key(key->input, KEY_POWER, 1);
		input_sync(key->input);
	}
	if (pending & KEY_UP) {
		input_report_key(key->input, KEY_POWER, 0);
		input_sync(key->input);
	}
	return IRQ_HANDLED;
failed:
	/* A failed bus transaction cannot deassert a level IRQ. Avoid a storm. */
	dev_err(key->input->dev.parent, "PMIC key transaction failed: %d; IRQ disabled\n", ret);
	disable_irq_nosync(irq);
	return IRQ_HANDLED;
}

static int charlotte_key_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np;
	struct charlotte_powerkey *key;
	u8 mask, pending;
	int i, ret;

	key = devm_kzalloc(dev, sizeof(*key), GFP_KERNEL);
	if (!key)
		return -ENOMEM;
	np = of_parse_phandle(dev->of_node, "hisilicon,pmic", 0);
	if (!np)
		return -EINVAL;
	key->pmic = spmi_find_device_by_of_node(np);
	of_node_put(np);
	if (!key->pmic)
		return -EPROBE_DEFER;
	ret = devm_add_action_or_reset(dev, charlotte_pmic_put, key->pmic);
	if (ret)
		return ret;
	key->irq = platform_get_irq(pdev, 0);
	if (key->irq < 0)
		return key->irq;
	for (i = 0; i < ARRAY_SIZE(pmic_masks); i++) {
		ret = spmi_ext_register_readl(key->pmic, pmic_masks[i], &mask, 1);
		if (ret)
			return ret;
		dev_info(dev, "PMIC mask[%#x]=%#x\n", pmic_masks[i], mask);
		key->saved_masks[i] = mask;
	}
	key->input = devm_input_allocate_device(dev);
	if (!key->input)
		return -ENOMEM;
	key->input->name = "Charlotte PMIC Power Key";
	key->input->phys = "charlotte-pmic/input0";
	key->input->id.bustype = BUS_HOST;
	input_set_capability(key->input, EV_KEY, KEY_POWER);
	ret = input_register_device(key->input);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, charlotte_key_restore_mask, key);
	if (ret)
		return ret;
	if (of_property_read_bool(dev->of_node, "hisilicon,volume-keys")) {
		key->volume = devm_input_allocate_device(dev);
		if (!key->volume)
			return -ENOMEM;
		key->volume->name = "Charlotte PMIC Volume Keys";
		key->volume->phys = "charlotte-pmic/input1";
		key->volume->id.bustype = BUS_HOST;
		input_set_capability(key->volume, EV_KEY, KEY_VOLUMEUP);
		input_set_capability(key->volume, EV_KEY, KEY_VOLUMEDOWN);
		__set_bit(EV_REP, key->volume->evbit);
		ret = input_register_device(key->volume);
		if (ret)
			return ret;
		INIT_DELAYED_WORK(&key->volume_work, charlotte_volume_work);
		ret = devm_add_action_or_reset(dev, charlotte_volume_cancel, key);
		if (ret)
			return ret;
	}
	/* Vendor starts with all sources masked, then each IRQ consumer unmasks
	 * its own bits. Preserve unclaimed pending status for later diagnosis.
	 */
	mask = 0xff;
	for (i = 0; i < ARRAY_SIZE(pmic_masks); i++) {
		ret = spmi_ext_register_writel(key->pmic, pmic_masks[i], &mask, 1);
		if (ret)
			return ret;
	}
	pending = KEY_BITS;
	ret = spmi_ext_register_writel(key->pmic, KEY_STATUS, &pending, 1);
	if (ret)
		return ret;
	if (key->volume) {
		pending = VOLUME_BITS;
		ret = spmi_ext_register_writel(key->pmic, VOLUME_STATUS, &pending, 1);
		if (ret)
			return ret;
	}
	ret = devm_request_threaded_irq(dev, key->irq, NULL, charlotte_key_irq,
				       IRQF_ONESHOT, dev_name(dev), key);
	if (ret)
		return ret;
	mask &= ~KEY_BITS;
	ret = spmi_ext_register_writel(key->pmic, KEY_MASK, &mask, 1);
	if (ret)
		return ret;
	if (key->volume) {
		mask = 0xff & ~VOLUME_BITS;
		ret = spmi_ext_register_writel(key->pmic, VOLUME_MASK, &mask, 1);
		if (ret)
			return ret;
		mod_delayed_work(system_wq, &key->volume_work,
				 msecs_to_jiffies(VOLUME_DEBOUNCE_MS));
		dev_info(dev, "volume keys ready: IRQ3 bits0-3, debounce=%ums\n",
			 VOLUME_DEBOUNCE_MS);
	}
	platform_set_drvdata(pdev, key);
	dev_info(dev, "power key ready: SID%u down=7 up=6 IRQ=%d\n", key->pmic->usid, key->irq);
	return 0;
}

static void charlotte_key_shutdown(struct platform_device *pdev)
{
	struct charlotte_powerkey *key = platform_get_drvdata(pdev);
	u8 mask;

	disable_irq(key->irq);
	if (key->volume) {
		cancel_delayed_work_sync(&key->volume_work);
		if (!spmi_ext_register_readl(key->pmic, VOLUME_MASK, &mask, 1)) {
			mask |= VOLUME_BITS;
			spmi_ext_register_writel(key->pmic, VOLUME_MASK, &mask, 1);
		}
	}
	if (!spmi_ext_register_readl(key->pmic, KEY_MASK, &mask, 1)) {
		mask |= KEY_BITS;
		spmi_ext_register_writel(key->pmic, KEY_MASK, &mask, 1);
	}
}

static const struct of_device_id charlotte_key_match[] = {
	{ .compatible = "hisilicon,charlotte-powerkey" }, { }
};
MODULE_DEVICE_TABLE(of, charlotte_key_match);

static struct platform_driver charlotte_key_driver = {
	.probe = charlotte_key_probe,
	.shutdown = charlotte_key_shutdown,
	.driver = { .name = "charlotte-powerkey", .of_match_table = charlotte_key_match },
};
module_platform_driver(charlotte_key_driver);
MODULE_DESCRIPTION("Charlotte primary PMIC power and volume keys");
MODULE_LICENSE("GPL");
