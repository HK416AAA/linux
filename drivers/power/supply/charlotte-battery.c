// SPDX-License-Identifier: GPL-2.0-only
/* PMIC21v900 coulometer, based on vendor hi6xxx_coul.c/hi6421v900_coul.h. */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/spmi.h>
#include <linux/workqueue.h>
#include "charlotte-battery-data.h"

#define COUL_CURRENT 0x4037
#define COUL_VOLTAGE 0x403a
#define SOH_TBAT 0x8017

struct charlotte_battery {
	struct spmi_device *pmic;
	struct power_supply *psy;
	struct delayed_work work;
	u32 sense_uohm;
	u32 design_uah;
	int ocv_uv;
	int ocv_uah;
	int remaining_uah;
};

/* Vendor 10k NTC ADC table, degrees C / 12-bit ADC code. */
static const int ntc[][2] = {
	{ -40, 3764 }, { -36, 3689 }, { -32, 3602 }, { -28, 3500 },
	{ -24, 3387 }, { -20, 3261 }, { -16, 3122 }, { -12, 2973 },
	{ -8, 2814 }, { -4, 2650 }, { 0, 2480 }, { 4, 2308 },
	{ 8, 2136 }, { 12, 1967 }, { 16, 1803 }, { 20, 1646 },
	{ 24, 1497 }, { 28, 1360 }, { 32, 1230 }, { 36, 1111 },
	{ 40, 1001 }, { 44, 903 }, { 48, 812 }, { 52, 729 },
	{ 56, 655 }, { 60, 590 }, { 64, 531 }, { 74, 406 },
	{ 84, 313 }, { 125, 110 },
};

static int charlotte_coul_read(struct charlotte_battery *bat, u16 reg,
			       int len, u64 *value)
{
	u8 data[8];
	int i, ret;

	*value = 0;
	/* Match vendor pmic_array_read(): PMIC sampling registers are read
	 * byte by byte, not as one SPMI extended-long burst.
	 */
	for (i = 0; i < len; i++) {
		ret = spmi_ext_register_readl(bat->pmic, reg + i, &data[i], 1);
		if (ret)
			return ret;
		*value |= (u64)data[i] << (8 * i);
	}
	return 0;
}

static int charlotte_coul_voltage(struct charlotte_battery *bat, int *uv)
{
	u64 raw;
	int ret = charlotte_coul_read(bat, COUL_VOLTAGE, 3, &raw);

	if (ret)
		return ret;
	*uv = div_s64((s64)sign_extend32(raw, 23) * 13000000, 16777216);
	if (*uv < 2000000 || *uv > 5000000)
		return -ENODATA;
	return 0;
}

static int charlotte_coul_current(struct charlotte_battery *bat, int *ua)
{
	u64 raw;
	int ret = charlotte_coul_read(bat, COUL_CURRENT, 3, &raw);

	if (ret)
		return ret;
	/* Vendor positive = discharge. Linux positive = charge.
	 * PMIC21v900 uses half the v700 gain and this board a 1mOhm shunt.
	 */
	*ua = -div_s64(div_s64((s64)sign_extend32(raw, 23) * 65000000000LL,
			     bat->sense_uohm), 8388608);
	return 0;
}

static int charlotte_coul_temp(struct charlotte_battery *bat, int *temp)
{
	u64 raw;
	int i, ret = charlotte_coul_read(bat, SOH_TBAT, 2, &raw);

	if (ret)
		return ret;
	/* Reject open/short sensor and stale/uninitialised conversions. */
	if (raw > ntc[0][1] || raw < ntc[ARRAY_SIZE(ntc) - 1][1])
		return -ENODATA;
	for (i = 1; i < ARRAY_SIZE(ntc); i++) {
		if (raw >= ntc[i][1]) {
			*temp = ntc[i - 1][0] * 10 +
				((int)raw - ntc[i - 1][1]) *
				(ntc[i][0] - ntc[i - 1][0]) * 10 /
				(ntc[i][1] - ntc[i - 1][1]);
			return 0;
		}
	}
	return -ENODATA;
}

static int charlotte_ocv_soc(int uv, int temp)
{
	int col, row, upper_mv, lower_mv;
	int mv[ARRAY_SIZE(ocv_profile)];

	temp = clamp(temp, ocv_temps[0], ocv_temps[ARRAY_SIZE(ocv_temps) - 1]);
	for (col = 1; col < ARRAY_SIZE(ocv_temps) - 1; col++)
		if (temp <= ocv_temps[col])
			break;
	for (row = 0; row < ARRAY_SIZE(ocv_profile); row++) {
		lower_mv = ocv_profile[row].mv[col - 1];
		upper_mv = ocv_profile[row].mv[col];
		mv[row] = lower_mv + (upper_mv - lower_mv) *
			(temp - ocv_temps[col - 1]) /
			(ocv_temps[col] - ocv_temps[col - 1]);
	}
	if (uv >= mv[0] * 1000)
		return 1000;
	for (row = 1; row < ARRAY_SIZE(ocv_profile); row++) {
		if (uv >= mv[row] * 1000) {
			if (mv[row - 1] <= mv[row])
				return ocv_profile[row].soc;
			return ocv_profile[row].soc +
				(uv - mv[row] * 1000) *
				(ocv_profile[row - 1].soc - ocv_profile[row].soc) /
				((mv[row - 1] - mv[row]) * 1000);
		}
	}
	return 0;
}

static int charlotte_battery_capacity_update(struct charlotte_battery *bat)
{
	u64 in, out;
	s64 remaining, net_uah;
	int ret;

	ret = charlotte_coul_read(bat, 0x4010, 5, &out);
	if (ret)
		return ret;
	ret = charlotte_coul_read(bat, 0x4015, 5, &in);
	if (ret)
		return ret;
	/* Vendor v900 gain: code * 10763 / (Rsense_uohm * 20000) uAh.
	 * These counters and the saved OCV form one retained baseline.
	 * Never clear them or substitute a saved SOC with unknown age.
	 */
	net_uah = div_s64(((s64)out - (s64)in) * 10763,
			 bat->sense_uohm * 20000);
	remaining = bat->ocv_uah - net_uah;
	if (remaining < -(s64)bat->design_uah / 5 ||
	    remaining > (s64)bat->design_uah * 6 / 5)
		return -ENODATA;
	WRITE_ONCE(bat->remaining_uah, clamp_t(s64, remaining, 0, bat->design_uah));
	return 0;
}

static int charlotte_battery_capacity_init(struct charlotte_battery *bat)
{
	u64 raw, ocv_temp;
	int soc, ret, temp;

	ret = charlotte_coul_read(bat, 0x041d, 2, &raw);
	if (ret)
		return ret;
	/* Bit15 is the vendor invalid-to-update-FCC flag, not ADC data. */
	bat->ocv_uv = div_s64((raw & 0x7fff) * 13000000LL, 65536);
	if (bat->ocv_uv < 2800000 || bat->ocv_uv > 4600000)
		return -ENODATA;
	ret = charlotte_coul_read(bat, 0x041f, 2, &ocv_temp);
	if (ret)
		return ret;
	temp = (s16)ocv_temp;
	if (temp < -400 || temp > 850) {
		ret = charlotte_coul_temp(bat, &temp);
		if (ret)
			return ret;
	}
	soc = charlotte_ocv_soc(bat->ocv_uv, temp);
	bat->ocv_uah = div_s64((s64)soc * bat->design_uah, 1000);
	return charlotte_battery_capacity_update(bat);
}

static enum power_supply_property charlotte_battery_props[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_SCOPE,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_OCV,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_MODEL_NAME,
};

static int charlotte_battery_get(struct power_supply *psy,
				enum power_supply_property prop,
				union power_supply_propval *val)
{
	struct charlotte_battery *bat = power_supply_get_drvdata(psy);
	struct power_supply *source;
	union power_supply_propval source_status;
	int ua, ret;

	switch (prop) {
	case POWER_SUPPLY_PROP_PRESENT:
		ret = charlotte_coul_voltage(bat, &ua);
		if (ret)
			return ret;
		val->intval = 1;
		return 0;
	case POWER_SUPPLY_PROP_STATUS:
		ret = charlotte_coul_current(bat, &ua);
		if (ret)
			return ret;
		val->intval = ua > 20000 ? POWER_SUPPLY_STATUS_CHARGING :
			ua < -20000 ? POWER_SUPPLY_STATUS_DISCHARGING :
			POWER_SUPPLY_STATUS_UNKNOWN;
		source = power_supply_get_by_name("charlotte-usb");
		if (source) {
			ret = power_supply_get_property(source, POWER_SUPPLY_PROP_STATUS,
						       &source_status);
			if (!ret && source_status.intval == POWER_SUPPLY_STATUS_FULL &&
			    ua >= -20000)
				val->intval = POWER_SUPPLY_STATUS_FULL;
			else if (!ret && ua >= -20000 && ua <= 20000)
				val->intval = source_status.intval;
			power_supply_put(source);
		}
		return 0;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LIPO;
		return 0;
	case POWER_SUPPLY_PROP_SCOPE:
		val->intval = POWER_SUPPLY_SCOPE_SYSTEM;
		return 0;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		return charlotte_coul_voltage(bat, &val->intval);
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		return charlotte_coul_current(bat, &val->intval);
	case POWER_SUPPLY_PROP_VOLTAGE_OCV:
		val->intval = bat->ocv_uv;
		return 0;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = DIV_ROUND_CLOSEST((s64)READ_ONCE(bat->remaining_uah) * 100,
					      bat->design_uah);
		return 0;
	case POWER_SUPPLY_PROP_CHARGE_NOW:
		val->intval = READ_ONCE(bat->remaining_uah);
		return 0;
	case POWER_SUPPLY_PROP_TEMP:
		return charlotte_coul_temp(bat, &val->intval);
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		val->intval = bat->design_uah;
		return 0;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = "Charlotte 8700mAh";
		return 0;
	default:
		return -EINVAL;
	}
}

static void charlotte_battery_external_changed(struct power_supply *psy)
{
	power_supply_changed(psy);
}

static const struct power_supply_desc charlotte_battery_desc = {
	.name = "charlotte-battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = charlotte_battery_props,
	.num_properties = ARRAY_SIZE(charlotte_battery_props),
	.get_property = charlotte_battery_get,
	.external_power_changed = charlotte_battery_external_changed,
};

static void charlotte_battery_work(struct work_struct *work)
{
	struct charlotte_battery *bat = container_of(to_delayed_work(work),
				struct charlotte_battery, work);

	if (charlotte_battery_capacity_update(bat))
		dev_warn_ratelimited(bat->psy->dev.parent,
				     "coulometer read failed; retaining last valid capacity\n");
	power_supply_changed(bat->psy);
	queue_delayed_work(system_percpu_wq, &bat->work, msecs_to_jiffies(10000));
}

static void charlotte_battery_stop(void *data)
{
	struct charlotte_battery *bat = data;

	cancel_delayed_work_sync(&bat->work);
}

static void charlotte_battery_put(void *data)
{
	spmi_device_put(data);
}

static ssize_t telemetry_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct charlotte_battery *bat = dev_get_drvdata(dev);
	static const struct { u16 reg; u8 len; } registers[] = {
		{ 0x041b, 8 }, { 0x0475, 2 }, { 0x4000, 8 },
		{ 0x4010, 5 }, { 0x4015, 5 }, { 0x401a, 8 }, { 0x4036, 7 },
		{ 0x8010, 1 }, { 0x8017, 4 }, { 0x8040, 1 },
	};
	u64 raw;
	int i, ret, value;
	ssize_t len = 0;

	ret = charlotte_coul_voltage(bat, &value);
	len += sysfs_emit_at(buf, len, "voltage_uv=%d error=%d\n", ret ? 0 : value, ret);
	ret = charlotte_coul_current(bat, &value);
	len += sysfs_emit_at(buf, len, "current_ua=%d error=%d\n", ret ? 0 : value, ret);
	ret = charlotte_coul_temp(bat, &value);
	len += sysfs_emit_at(buf, len, "temp_deciC=%d error=%d\n", ret ? 0 : value, ret);
	for (i = 0; i < ARRAY_SIZE(registers); i++) {
		ret = charlotte_coul_read(bat, registers[i].reg, registers[i].len, &raw);
		len += sysfs_emit_at(buf, len, "%04x/%u=%#llx error=%d\n",
			registers[i].reg, registers[i].len, ret ? 0 : raw, ret);
	}
	return len;
}
static DEVICE_ATTR_RO(telemetry);

static struct attribute *charlotte_battery_attrs[] = {
	&dev_attr_telemetry.attr,
	NULL,
};
static const struct attribute_group charlotte_battery_group = {
	.attrs = charlotte_battery_attrs,
};

static int charlotte_battery_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct power_supply_config cfg = {};
	struct device_node *np;
	struct charlotte_battery *bat;
	static const struct { u16 reg; u8 len; } diagnostic[] = {
		{ 0x0000, 1 }, { 0x041b, 8 }, { 0x0475, 2 }, { 0x4000, 8 },
		{ 0x4010, 5 }, { 0x4015, 5 }, { 0x401a, 8 }, { 0x4036, 7 },
		{ 0x8010, 1 }, { 0x8017, 4 }, { 0x8040, 1 },
	};
	u64 raw;
	int i, ret, uv, ua, temp;

	bat = devm_kzalloc(dev, sizeof(*bat), GFP_KERNEL);
	if (!bat)
		return -ENOMEM;
	np = of_parse_phandle(dev->of_node, "hisilicon,pmic", 0);
	if (!np)
		return -EINVAL;
	bat->pmic = spmi_find_device_by_of_node(np);
	of_node_put(np);
	if (!bat->pmic)
		return -EPROBE_DEFER;
	ret = devm_add_action_or_reset(dev, charlotte_battery_put, bat->pmic);
	if (ret)
		return ret;
	ret = of_property_read_u32(dev->of_node, "shunt-resistor-micro-ohms", &bat->sense_uohm);
	if (ret || bat->sense_uohm < 100 || bat->sense_uohm > 10000)
		return -EINVAL;
	ret = of_property_read_u32(dev->of_node, "charge-full-design-microamp-hours", &bat->design_uah);
	if (ret || !bat->design_uah || bat->design_uah > 100000000)
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(diagnostic); i++) {
		ret = charlotte_coul_read(bat, diagnostic[i].reg, diagnostic[i].len, &raw);
		if (!ret)
			dev_info(dev, "PMIC[%04x/%u]=%#llx\n",
				 diagnostic[i].reg, diagnostic[i].len, raw);
	}
	ret = charlotte_coul_voltage(bat, &uv);
	if (ret)
		return dev_err_probe(dev, ret, "no valid coulometer voltage\n");
	ret = charlotte_coul_current(bat, &ua);
	if (ret)
		return ret;
	ret = charlotte_coul_temp(bat, &temp);
	dev_info(dev, "battery %duV %duA, NTC %s %d deciC\n",
		 uv, ua, ret ? "unavailable" : "valid", ret ? 0 : temp);
	platform_set_drvdata(pdev, bat);
	INIT_DELAYED_WORK(&bat->work, charlotte_battery_work);
	ret = devm_device_add_group(dev, &charlotte_battery_group);
	if (ret)
		return ret;
	/* No userspace battery until a valid capacity estimate is available.
	 * Otherwise UPower interprets missing capacity as 0% and may power off.
	 */
	if (of_property_read_bool(dev->of_node, "hisilicon,telemetry-only")) {
		dev_info(dev, "telemetry only; userspace battery not registered\n");
		return 0;
	}
	ret = charlotte_battery_capacity_init(bat);
	if (ret) {
		dev_warn(dev, "invalid OCV/counter baseline (%d); telemetry only\n", ret);
		return 0;
	}
	dev_info(dev, "saved OCV %duV; estimated remaining %duAh / %uuAh\n",
		 bat->ocv_uv, bat->remaining_uah, bat->design_uah);
	cfg.drv_data = bat;
	cfg.fwnode = dev_fwnode(dev);
	bat->psy = devm_power_supply_register(dev, &charlotte_battery_desc, &cfg);
	if (IS_ERR(bat->psy))
		return PTR_ERR(bat->psy);
	ret = devm_add_action_or_reset(dev, charlotte_battery_stop, bat);
	if (ret)
		return ret;
	queue_delayed_work(system_percpu_wq, &bat->work, msecs_to_jiffies(10000));
	return 0;
}

static void charlotte_battery_shutdown(struct platform_device *pdev)
{
	charlotte_battery_stop(platform_get_drvdata(pdev));
}

static void charlotte_battery_remove(struct platform_device *pdev)
{
	charlotte_battery_stop(platform_get_drvdata(pdev));
}

static const struct of_device_id charlotte_battery_of_match[] = {
	{ .compatible = "hisilicon,charlotte-battery" },
	{}
};
MODULE_DEVICE_TABLE(of, charlotte_battery_of_match);

static struct platform_driver charlotte_battery_driver = {
	.probe = charlotte_battery_probe,
	.shutdown = charlotte_battery_shutdown,
	.remove = charlotte_battery_remove,
	.driver = {
		.name = "charlotte-battery",
		.of_match_table = charlotte_battery_of_match,
	},
};
module_platform_driver(charlotte_battery_driver);
MODULE_DESCRIPTION("Charlotte PMIC21v900 battery telemetry");
MODULE_LICENSE("GPL");
