// SPDX-License-Identifier: GPL-2.0-only
/* Charlotte HI6526 V510 USB charging, based on vendor schargerV600. */
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/unaligned.h>
#include <linux/usb/charlotte.h>
#include <linux/workqueue.h>

#define HI6526_PAGE		0x7f
#define HI6526_POWER_STATUS	0x1e
#define HI6526_CHG_ENABLE	0xf1
#define HI6526_CHG_EN		BIT(4)
#define HI6526_WDT_KICK		0xec
#define HI6526_WDT_CTRL		0xed
#define HI6526_WDT_STATUS	0xf4
#define HI6526_CHG_STATUS	0x1e0
#define HI6526_BUCK_OK		0x2ea
#define HI6526_INPUT_UA		475000
#define HI6526_CHARGE_UA		480000
#define HI6526_BC12_INPUT_CODE	15
#define HI6526_BC12_CHARGE_CODE	8
#define HI6526_BC12_INPUT_UA	(HI6526_BC12_INPUT_CODE * 100000)
#define HI6526_BC12_CHARGE_UA	((HI6526_BC12_CHARGE_CODE + 1) * 160000)
#define HI6526_CV_UV		4398400
#define HI6526_ALERT		0x10
#define HI6526_ALERT_MASK	0x12
#define HI6526_POWER_MASK	0x14
#define HI6526_GLOBAL_MASK	0x1c8
/* Vendor IRQ groups occupy bits 0..4. Do not set the upper mask bits:
 * fb suppressed raw ACCP completion; the stock 05 probe restored it.
 * Keep FCP/others/LVC/SC/buck masked and the TCPC source group enabled.
 */
#define HI6526_SOURCE_ONLY_MASK	0x1b
#define HI6526_SOURCE_ALERT	(BIT(0) | BIT(1))
#define HI6526_FAST_INPUT_CODE	14 /* Stock FCP: 1400mA. */
#define HI6526_FAST_CHARGE_CODE	14 /* Stock 2500mA rounded down to 2400mA. */
#define HI6526_FAST_INPUT_UA	1400000
#define HI6526_FAST_CHARGE_UA	2400000

/* Opt-in until charger/cable and rollback have passed hardware validation. */
static unsigned int protocol_mode;
module_param(protocol_mode, uint, 0444);
MODULE_PARM_DESC(protocol_mode, "0=BC1.2 only, 1=SCP/FCP identify, 2=9V buck charging");
static bool protocol_diag;
module_param(protocol_diag, bool, 0444);
MODULE_PARM_DESC(protocol_diag, "Run bounded read-only ACCP timing/mask probes in identify mode");

enum hi6526_protocol {
	HI6526_PROTOCOL_NONE,
	HI6526_PROTOCOL_FCP,
	HI6526_PROTOCOL_SCP,
};

struct charlotte_hi6526 {
	struct i2c_client *client;
	struct power_supply *battery;
	struct power_supply *psy;
	struct device *usb_dev;
	struct gpio_desc *irq_gpio;
	struct mutex lock;
	u8 page;
	bool page_valid;
	struct delayed_work work;
	char *supplied_to[1];
	bool stopped;
	bool setup_pending;
	bool valid;
	bool online;
	bool enabled;
	bool detect;
	bool irq_ready;
	bool irq_failed;
	u8 saved_alert_mask[2];
	u8 saved_power_mask;
	u8 saved_global_mask;
	int usb_type;
	int input_ua;
	int charge_ua;
	int health;
	int status;
	int vbus_uv;
	bool protocol_tried;
	unsigned long protocol_retry_after;
	bool protocol_engine;
	bool buck_reset_pending;
	bool adapter_changed;
	bool fast;
	enum hi6526_protocol protocol;
	u8 adapter_type;
	int adapter_min_mv;
	int adapter_max_mv;
	int adapter_max_ma;
	int adapter_max_mw;
	int protocol_error;
	const char *protocol_stage;
	u8 protocol_addr;
	u8 protocol_cmd;
	u8 protocol_diag[16];
	u16 protocol_diag_valid;
	int protocol_probe;
	u8 protocol_probe_acked;
};

#include "charlotte-hi6526-i2c.h"

static int hi6526_update(struct charlotte_hi6526 *chg, u16 reg, u8 mask, u8 val)
{
	u8 old;
	int ret = hi6526_read(chg, reg, &old, 1);

	if (ret)
		return ret;
	return hi6526_write(chg, reg, (old & ~mask) | (val & mask));
}

static int hi6526_vbus(struct charlotte_hi6526 *chg, int *uv)
{
	static const u16 regs[] = { 0xa9, 0xab, 0xac, 0xad };
	u8 saved[ARRAY_SIZE(regs)], data[2], status;
	int i, ret, restore_ret;

	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		ret = hi6526_read(chg, regs[i], &saved[i], 1);
		if (ret)
			return ret;
	}
	ret = hi6526_write(chg, 0xa9, 0x10);
	if (ret)
		goto restore;
	ret = hi6526_write(chg, 0xab, 0x63);
	if (ret)
		goto restore;
	ret = hi6526_write(chg, 0xac, 0);
	if (ret)
		goto restore;
	ret = hi6526_write(chg, 0xad, BIT(2)); /* Single VBUS channel. */
	if (ret)
		goto restore;
	ret = hi6526_update(chg, 0xaa, BIT(0), 0);
	if (ret)
		goto restore;
	ret = hi6526_write(chg, 0xa9, 0x90);
	if (ret)
		goto restore;
	ret = hi6526_update(chg, 0xaa, BIT(0), BIT(0));
	if (ret)
		goto restore;
	for (i = 0; i < 10; i++) {
		usleep_range(1000, 1500);
		ret = hi6526_read(chg, 0xb2, &status, 1);
		if (ret)
			goto restore;
		if ((status & BIT(4)) && !(status & BIT(0)))
			break;
	}
	if (i == 10) {
		ret = -ETIMEDOUT;
		goto restore;
	}
	ret = hi6526_read(chg, 0xb7, data, sizeof(data));
	if (!ret)
		*uv = (get_unaligned_le16(data) & 0x3fff) * 1000;
restore:
	/* Keep unrelated HKADC setup; never leave a continuous conversion on. */
	restore_ret = hi6526_write(chg, 0xa9, 0);
	if (!ret)
		ret = restore_ret;
	for (i = 1; i < ARRAY_SIZE(regs); i++) {
		restore_ret = hi6526_write(chg, regs[i], saved[i]);
		if (!ret)
			ret = restore_ret;
	}
	restore_ret = hi6526_write(chg, regs[0], saved[0]);
	return ret ? ret : restore_ret;
}

#include "charlotte-hi6526-protocol.h"

/* Exact V510 common/buck/5V entries from hisi_scharger_v600_include.h.
 * Apply only with charging disabled; the USB limits below override defaults.
 */
static int hi6526_analog_init(struct charlotte_hi6526 *chg)
{
	static const struct { u16 reg; u8 mask; u8 val; } init[] = {
		{ 0x2f7, 0xff, 0x01 },
		{ 0xab, 0xff, 0x63 },
		{ 0x2e2, 0xff, 0x85 },
		{ 0x2e5, 0xff, 0x58 },
		{ 0x293, 0xff, 0xa0 },
		{ 0x284, 0xff, 0x08 },
		{ 0x287, 0xff, 0x1f },
		{ 0x280, 0xff, 0x0d },
		{ 0x2ab, 0xff, 0x20 },
		{ 0x3b5, 0xff, 0x80 },
		{ 0x2ac, 0xff, 0x1e },
		{ 0x298, 0xff, 0x01 },
		{ 0x2bd, 0xff, 0x00 },
		{ 0x29d, 0xff, 0x03 },
		{ 0x290, 0x20, 0x20 },
		{ 0x2d3, 0xff, 0x15 },
		{ 0x2d4, 0xff, 0x95 },
		{ 0x2e1, 0xff, 0x6c },
		{ 0x2e3, 0xff, 0x16 },
		{ 0x2e4, 0xff, 0x35 },
		{ 0x2da, 0xff, 0x8c },
		{ 0x2db, 0xff, 0x94 },
		{ 0x2d6, 0xff, 0x0f },
		{ 0x2d9, 0xff, 0x11 },
		{ 0x2de, 0xff, 0x1e },
		{ 0x2e9, 0xff, 0x1b },
		{ 0x2e0, 0xff, 0x3f },
		{ 0x2d9, 0xff, 0x11 },
		{ 0x2e7, 0xff, 0xaf },
		{ 0x2de, 0xff, 0x1e },
		{ 0x2e9, 0xff, 0x1b },
		{ 0x2e0, 0xff, 0x3f },
		{ 0x2f2, 0xff, 0xa6 },
		{ 0x2ed, 0xff, 0x27 },
		{ 0x286, 0xff, 0x06 },
		{ 0x2e7, 0xff, 0xaf },
		{ 0x2a4, 0x0c, 0x00 },
		{ 0x2a5, 0xc0, 0x00 },
		{ 0x2a6, 0x18, 0x00 },
		{ 0x2d6, 0x01, 0x01 },
		{ 0x2d7, 0xf0, 0x60 },
		{ 0x297, 0x01, 0x01 },
	};
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(init); i++) {
		ret = hi6526_update(chg, init[i].reg, init[i].mask, init[i].val);
		if (ret)
			return ret;
	}
	return 0;
}

static int hi6526_setup(struct charlotte_hi6526 *chg)
{
	/* Fixed stock USB policy, never an unnegotiated AC/PD current limit.
	 * V510: ICHG steps 160mA, CV base 4V / steps 16.6mV. V511 differs.
	 */
	static const struct { u16 reg; u8 mask; u8 val; } settings[] = {
		{ HI6526_CHG_ENABLE, HI6526_CHG_EN, 0 },
		{ 0x2d0, 0xf8, 5 << 3 }, /* USB input 475mA. */
		{ 0x282, 0x1f, 2 }, /* Charge limit 480mA. */
		{ 0x283, 0x7f, (24 << 2) | 3 }, /* 4.3984V, 20h safety timer. */
		{ 0x284, 0xe0, 0 }, /* No IR voltage compensation. */
		{ 0x288, 0xd7, BIT(4) }, /* 150mA EOC, timer on, no IR clamp. */
		{ 0x285, 0x0f, 1 << 2 }, /* Precharge 200mA below 2.8V. */
		{ 0x281, BIT(1), 0 }, /* Gate termination near CV in poll. */
		{ 0xfb, BIT(0), BIT(0) }, /* Buck mode. */
		{ 0xfe, BIT(0), 0 }, /* Buck not high impedance. */
		{ 0x2a4, 0x0c, 0 }, /* 5V buck input selector. */
		{ 0x2a5, 0xc0, 0 }, /* 6.5V hardware input OVP. */
		{ 0x2a6, 0x18, 0 }, /* 3.8V hardware input UVP. */
		{ HI6526_WDT_KICK, BIT(0), BIT(0) },
		{ HI6526_WDT_CTRL, 7, 6 }, /* 40s watchdog, refreshed every 5s. */
	};
	u8 mode0, mode1, actual;
	int i, ret;

	ret = hi6526_update(chg, HI6526_CHG_ENABLE, HI6526_CHG_EN, 0);
	if (ret)
		return ret;
	ret = hi6526_read(chg, 0xf1, &mode0, 1);
	if (ret)
		return ret;
	ret = hi6526_read(chg, 0xf2, &mode1, 1);
	if (ret)
		return ret;
	/* Leave proprietary direct-charge/OTG negotiation to its own driver. */
	if ((mode0 & BIT(0)) || (mode1 & BIT(5)))
		return -EBUSY;
	ret = hi6526_analog_init(chg);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(settings); i++) {
		ret = hi6526_update(chg, settings[i].reg, settings[i].mask, settings[i].val);
		if (ret)
			return ret;
		if (settings[i].reg == HI6526_WDT_KICK)
			continue;
		ret = hi6526_read(chg, settings[i].reg, &actual, 1);
		if (ret)
			return ret;
		if ((actual & settings[i].mask) != settings[i].val) {
			dev_warn_ratelimited(&chg->client->dev,
				"setup reg %#x readback %#x mask %#x expected %#x\n",
				settings[i].reg, actual, settings[i].mask, settings[i].val);
			return -EIO;
		}
	}
	return 0;
}

static int hi6526_policy_check(struct charlotte_hi6526 *chg)
{
	static const struct { u16 reg; u8 mask; u8 val; } checks[] = {
		{ 0x283, 0x7f, (24 << 2) | 3 }, { 0x284, 0xe0, 0 },
		{ 0x288, 0xd7, BIT(4) }, { 0x285, 0x0f, 1 << 2 },
		{ 0xfb, BIT(0), BIT(0) }, { 0xfe, BIT(0), 0 },
		{ HI6526_WDT_CTRL, 7, 6 },
	};
	u8 actual;
	int i, ret;
	u16 reg = 0x2d0;
	u8 mask = 0xf8;
	u8 expected = (chg->input_ua == HI6526_BC12_INPUT_UA ?
		       HI6526_BC12_INPUT_CODE : chg->fast ? hi6526_fast_input_code(chg) : 5) << 3;

	ret = hi6526_read(chg, 0x2d0, &actual, 1);
	if (ret)
		return ret;
	if ((actual & mask) != expected)
		goto reset;
	reg = 0x282;
	mask = 0x1f;
	expected = chg->charge_ua == HI6526_BC12_CHARGE_UA ?
		HI6526_BC12_CHARGE_CODE : chg->fast ? HI6526_FAST_CHARGE_CODE : 2;
	ret = hi6526_read(chg, 0x282, &actual, 1);
	if (ret)
		return ret;
	if ((actual & mask) != expected)
		goto reset;
	for (i = 0; i < ARRAY_SIZE(checks); i++) {
		reg = checks[i].reg;
		mask = checks[i].mask;
		expected = checks[i].val;
		ret = hi6526_read(chg, reg, &actual, 1);
		if (ret)
			return ret;
		if ((actual & mask) != expected)
			goto reset;
	}
	ret = hi6526_read(chg, 0x2a5, &actual, 1);
	if (ret)
		return ret;
	reg = 0x2a5;
	mask = 0xc0;
	expected = chg->fast ? BIT(6) : 0;
	if ((actual & mask) != expected)
		goto reset;
	ret = hi6526_read(chg, 0x2a4, &actual, 1);
	if (ret)
		return ret;
	reg = 0x2a4;
	mask = 0x0c;
	expected = chg->fast ? BIT(2) : 0;
	if ((actual & mask) != expected)
		goto reset;
	reg = 0x2e7;
	mask = 0xff;
	expected = chg->fast ? 0xef : 0xaf;
	ret = hi6526_read(chg, reg, &actual, 1);
	if (ret)
		return ret;
	if (actual != expected)
		goto reset;
	return 0;
reset:
	dev_warn_ratelimited(&chg->client->dev,
		"charge reg %#x changed: %#x mask %#x expected %#x; restoring USB limits\n",
		reg, actual, mask, expected);
	ret = hi6526_protocol_drop(chg);
	if (ret)
		return ret;
	chg->usb_type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
	chg->input_ua = HI6526_INPUT_UA;
	chg->charge_ua = HI6526_CHARGE_UA;
	chg->detect = true;
	return hi6526_setup(chg);
}

/* Revoke higher input limits before acknowledging any attachment event. */
static int hi6526_source_event(struct charlotte_hi6526 *chg)
{
	u8 alert[2];
	int ret;

	ret = hi6526_read(chg, HI6526_ALERT, alert, sizeof(alert));
	if (ret || !(alert[0] & HI6526_SOURCE_ALERT))
		return ret;
	ret = hi6526_protocol_drop(chg);
	if (ret)
		return ret;
	chg->protocol_tried = false;
	chg->protocol = HI6526_PROTOCOL_NONE;
	chg->adapter_type = 0;
	chg->adapter_min_mv = 0;
	chg->adapter_max_mv = 0;
	chg->adapter_max_ma = 0;
	chg->adapter_max_mw = 0;
	chg->protocol_error = 0;
	chg->usb_type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
	chg->detect = true;
	chg->input_ua = HI6526_INPUT_UA;
	chg->charge_ua = HI6526_CHARGE_UA;
	ret = hi6526_update(chg, HI6526_CHG_ENABLE, HI6526_CHG_EN, 0);
	if (ret)
		return ret;
	ret = hi6526_update(chg, 0x2d0, 0xf8, 5 << 3);
	if (ret)
		return ret;
	ret = hi6526_update(chg, 0x282, 0x1f, 2);
	if (ret)
		return ret;
	return hi6526_tcpc_write16(chg, HI6526_ALERT, alert[0] & HI6526_SOURCE_ALERT);
}

static int hi6526_alert_setup(struct charlotte_hi6526 *chg)
{
	static const struct { u16 reg; u8 value; } masks[] = {
		{ HI6526_POWER_MASK, BIT(2) },
		{ HI6526_GLOBAL_MASK, HI6526_SOURCE_ONLY_MASK },
	};
	u8 actual, alert[2];
	int i, ret;

	ret = hi6526_tcpc_write16(chg, HI6526_ALERT_MASK, HI6526_SOURCE_ALERT);
	if (ret)
		return ret;
	ret = hi6526_read(chg, HI6526_ALERT_MASK, alert, sizeof(alert));
	if (ret)
		return ret;
	if (get_unaligned_le16(alert) != HI6526_SOURCE_ALERT)
		return -EIO;
	for (i = 0; i < ARRAY_SIZE(masks); i++) {
		ret = hi6526_write(chg, masks[i].reg, masks[i].value);
		if (ret)
			return ret;
		ret = hi6526_read(chg, masks[i].reg, &actual, 1);
		if (ret)
			return ret;
		if (actual != masks[i].value)
			return -EIO;
	}
	return 0;
}

static int hi6526_detect_source(struct charlotte_hi6526 *chg)
{
	u8 alert[2], input, charge;
	int type, ret;

	/* No input increase during PHY detection or an unstable cable event. */
	ret = hi6526_update(chg, HI6526_CHG_ENABLE, HI6526_CHG_EN, 0);
	if (ret)
		return ret;
	type = charlotte_usb_bc12_detect(chg->usb_dev);
	if (type < 0)
		return type;
	ret = hi6526_read(chg, HI6526_ALERT, alert, sizeof(alert));
	if (ret)
		return ret;
	if (alert[0] & HI6526_SOURCE_ALERT)
		return hi6526_source_event(chg);
	if (type == POWER_SUPPLY_USB_TYPE_CDP || type == POWER_SUPPLY_USB_TYPE_DCP) {
		/* Use BC1.2's 1.5A budget only after CDP/DCP detection. */
		ret = hi6526_update(chg, 0x2d0, 0xf8, HI6526_BC12_INPUT_CODE << 3);
		if (ret)
			return ret;
		ret = hi6526_update(chg, 0x282, 0x1f, HI6526_BC12_CHARGE_CODE);
		if (ret)
			return ret;
		ret = hi6526_read(chg, 0x2d0, &input, 1);
		if (ret)
			return ret;
		ret = hi6526_read(chg, 0x282, &charge, 1);
		if (ret)
			return ret;
		if ((input & 0xf8) != HI6526_BC12_INPUT_CODE << 3 ||
		    (charge & 0x1f) != HI6526_BC12_CHARGE_CODE)
			return -EIO;
		chg->input_ua = HI6526_BC12_INPUT_UA;
		chg->charge_ua = HI6526_BC12_CHARGE_UA;
	}
	chg->usb_type = type;
	chg->detect = false;
	dev_info(&chg->client->dev, "BC1.2 type=%d input=%duA charge=%duA\n",
		 type, chg->input_ua, chg->charge_ua);
	return 0;
}

static int hi6526_battery_get(struct charlotte_hi6526 *chg,
			      enum power_supply_property prop, int *value)
{
	union power_supply_propval val;
	int ret = power_supply_get_property(chg->battery, prop, &val);

	if (!ret)
		*value = val.intval;
	return ret;
}

static int hi6526_protocol_refresh(void *data)
{
	return hi6526_protocol_keepalive(data);
}

/* All guards are based on live readings; a failed sample disables charging. */
static int hi6526_poll(struct charlotte_hi6526 *chg)
{
	u8 power, pg, state, wdt, dpm, therm, control;
	bool online, enable, term;
	int ret, health = POWER_SUPPLY_HEALTH_GOOD;
	int uv, ua, temp, vbus = 0;

	/* Re-select on each poll in case a source event reset the chip. */
	chg->page_valid = false;
	/* A failed rollback leaves charging disabled until 5V is confirmed. */
	if (chg->adapter_changed && !chg->fast) {
		ret = hi6526_protocol_drop(chg);
		if (ret)
			goto fail;
	}
	/* A startup readback failure must not permanently lose the charger.
	 * Retry the complete disabled-charge setup before handling the source;
	 * every setting still has to pass its normal readback check.
	 */
	if (chg->setup_pending) {
		ret = hi6526_setup(chg);
		if (ret)
			goto fail;
		chg->setup_pending = false;
		dev_info(&chg->client->dev, "startup charging setup recovered\n");
	}
	ret = hi6526_source_event(chg);
	if (ret)
		goto fail;
	ret = hi6526_read(chg, HI6526_POWER_STATUS, &power, 1);
	if (ret)
		goto fail;
	online = (power & BIT(2)) && !(power & BIT(4));
	if (!online) {
		ret = hi6526_protocol_drop(chg);
		if (ret)
			goto fail;
		chg->usb_type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
		chg->detect = true;
		chg->protocol_tried = false;
		chg->protocol = HI6526_PROTOCOL_NONE;
	}
	ret = hi6526_battery_get(chg, POWER_SUPPLY_PROP_TEMP, &temp);
	if (ret)
		goto fail;
	ret = hi6526_battery_get(chg, POWER_SUPPLY_PROP_VOLTAGE_NOW, &uv);
	if (ret)
		goto fail;
	ret = hi6526_battery_get(chg, POWER_SUPPLY_PROP_CURRENT_NOW, &ua);
	if (ret)
		goto fail;
	if (online) {
		if (chg->fast && (temp < 120 || temp > 400 || uv > 4300000)) {
			ret = hi6526_protocol_drop(chg);
			if (ret)
				goto fail;
			chg->input_ua = HI6526_INPUT_UA;
			chg->charge_ua = HI6526_CHARGE_UA;
			chg->detect = true;
			ret = hi6526_setup(chg);
			if (ret)
				goto fail;
		}
		ret = hi6526_alert_setup(chg);
		if (ret)
			goto fail;
		ret = hi6526_policy_check(chg);
		if (ret)
			goto fail;
		if (chg->detect && !chg->irq_failed) {
			ret = hi6526_detect_source(chg);
			if (ret)
				goto fail;
		}
		if (protocol_mode && !chg->protocol_tried && !chg->irq_failed &&
		    (!chg->protocol_retry_after ||
		     time_after_eq(jiffies, chg->protocol_retry_after)) &&
		    chg->usb_type == POWER_SUPPLY_USB_TYPE_DCP &&
		    hi6526_protocol_can_probe(uv, temp)) {
			chg->protocol_tried = true;
			/* Adapter reset can generate a source event of its own.
			 * Do not immediately negotiate again after that event.
			 */
			chg->protocol_retry_after = jiffies + msecs_to_jiffies(60000);
			ret = hi6526_update(chg, HI6526_CHG_ENABLE, HI6526_CHG_EN, 0);
			if (ret)
				goto fail;
			ret = charlotte_usb_dpdm_run(chg->usb_dev, hi6526_protocol_run, chg);
			if (ret)
				goto fail;
		}
		if (chg->fast) {
			ret = charlotte_usb_dpdm_run(chg->usb_dev, hi6526_protocol_refresh, chg);
			if (ret) {
				chg->protocol_error = ret;
				goto fail;
			}
		}
	}
	ret = hi6526_read(chg, HI6526_BUCK_OK, &pg, 1);
	if (ret)
		goto fail;
	ret = hi6526_read(chg, HI6526_CHG_STATUS, &state, 1);
	if (ret)
		goto fail;
	ret = hi6526_read(chg, HI6526_WDT_STATUS, &wdt, 1);
	if (ret)
		goto fail;
	if (online) {
		ret = hi6526_vbus(chg, &vbus);
		if (ret)
			goto fail;
	}
	if (temp < 50)
		health = POWER_SUPPLY_HEALTH_COLD;
	else if (temp > 450)
		health = POWER_SUPPLY_HEALTH_OVERHEAT;
	else if (uv > 4430000 || (online && vbus > (chg->fast ? 9500000 : 5800000)))
		health = POWER_SUPPLY_HEALTH_OVERVOLTAGE;
	else if (wdt & BIT(0))
		health = POWER_SUPPLY_HEALTH_WATCHDOG_TIMER_EXPIRE;
	else if (uv < 2800000)
		health = POWER_SUPPLY_HEALTH_UNSPEC_FAILURE;
	enable = online && (power & BIT(0)) && (pg & BIT(0)) &&
		vbus >= (chg->fast ? 8500000 : 4350000) &&
		vbus <= (chg->fast ? 9500000 : 5800000) && health == POWER_SUPPLY_HEALTH_GOOD;
	if (chg->fast && !enable) {
		dev_warn_ratelimited(&chg->client->dev,
			"9V charge rejected: power=%#x buck=%#x VBUS=%duV health=%d\n",
			power, pg, vbus, health);
		ret = -ERANGE;
		goto fail;
	}
	ret = hi6526_update(chg, 0x280, BIT(6), BIT(6)); /* Latch ACL report. */
	if (ret)
		goto fail;
	ret = hi6526_read(chg, 0x28a, &dpm, 1);
	if (ret)
		goto fail;
	ret = hi6526_update(chg, 0x280, BIT(6), BIT(6));
	if (ret)
		goto fail;
	ret = hi6526_read(chg, 0x28b, &therm, 1);
	if (ret)
		goto fail;
	/* Near-CV gating avoids false completion while input-limited. */
	term = enable && uv >= HI6526_CV_UV - 100000 && ua >= 0 &&
		!(dpm & (BIT(6) | BIT(7))) && !(therm & BIT(1));
	ret = hi6526_update(chg, 0x281, BIT(1), term ? BIT(1) : 0);
	if (ret)
		goto fail;
	ret = hi6526_update(chg, HI6526_CHG_ENABLE, HI6526_CHG_EN,
			    enable ? HI6526_CHG_EN : 0);
	if (ret)
		goto fail;
	ret = hi6526_update(chg, HI6526_WDT_KICK, BIT(0), BIT(0));
	if (ret)
		goto fail;
	ret = hi6526_read(chg, HI6526_CHG_ENABLE, &control, 1);
	if (ret)
		goto fail;
	if (!!(control & HI6526_CHG_EN) != enable) {
		ret = -EIO;
		goto fail;
	}
	if (!chg->valid || chg->online != online || chg->enabled != enable ||
	    chg->health != health)
		dev_info(&chg->client->dev,
			 "USB online=%d charge=%d VBUS=%duV battery=%duV %duA %d deciC health=%d\n",
			 online, enable, vbus, uv, ua, temp, health);
	chg->online = online;
	chg->enabled = enable;
	chg->vbus_uv = vbus;
	chg->health = health;
	chg->status = !online ? POWER_SUPPLY_STATUS_DISCHARGING :
		!enable ? POWER_SUPPLY_STATUS_NOT_CHARGING :
		term && (state & 3) == 3 ? POWER_SUPPLY_STATUS_FULL :
		POWER_SUPPLY_STATUS_CHARGING;
	chg->valid = true;
	return 0;
fail:
	hi6526_update(chg, HI6526_CHG_ENABLE, HI6526_CHG_EN, 0);
	if (chg->protocol_engine) {
		int drop_ret;

		chg->fast = false;
		drop_ret = hi6526_protocol_drop(chg);
		if (drop_ret)
			dev_warn_ratelimited(&chg->client->dev, "adapter rollback error %d\n",
					    drop_ret);
	}
	hi6526_update(chg, 0x2d0, 0xf8, 5 << 3);
	hi6526_update(chg, 0x282, 0x1f, 2);
	chg->input_ua = HI6526_INPUT_UA;
	chg->charge_ua = HI6526_CHARGE_UA;
	chg->usb_type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
	chg->detect = true;
	/* No watchdog feed on an error: hardware also times out within 40s. */
	chg->enabled = false;
	chg->health = POWER_SUPPLY_HEALTH_UNSPEC_FAILURE;
	chg->status = POWER_SUPPLY_STATUS_UNKNOWN;
	chg->valid = false;
	return ret;
}

static const enum power_supply_property hi6526_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_USB_TYPE,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_MODEL_NAME,
};

static int hi6526_get_property(struct power_supply *psy,
			       enum power_supply_property prop,
			       union power_supply_propval *val)
{
	struct charlotte_hi6526 *chg = power_supply_get_drvdata(psy);
	int ret = 0;

	mutex_lock(&chg->lock);
	if (!chg->valid && (prop == POWER_SUPPLY_PROP_ONLINE ||
			    prop == POWER_SUPPLY_PROP_VOLTAGE_NOW)) {
		ret = -ENODATA;
		goto out;
	}
	switch (prop) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = chg->online;
		break;
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = chg->status;
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		val->intval = chg->health;
		break;
	case POWER_SUPPLY_PROP_USB_TYPE:
		val->intval = chg->usb_type;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = chg->vbus_uv;
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		val->intval = chg->input_ua;
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
		val->intval = chg->charge_ua;
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX:
		val->intval = HI6526_CV_UV;
		break;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = "HI6526 V510";
		break;
	default:
		ret = -EINVAL;
	}
out:
	mutex_unlock(&chg->lock);
	return ret;
}

static const struct power_supply_desc hi6526_desc = {
	.name = "charlotte-usb",
	.type = POWER_SUPPLY_TYPE_USB,
	.properties = hi6526_props,
	.num_properties = ARRAY_SIZE(hi6526_props),
	.get_property = hi6526_get_property,
	.usb_types = BIT(POWER_SUPPLY_USB_TYPE_UNKNOWN) | BIT(POWER_SUPPLY_USB_TYPE_SDP) |
		     BIT(POWER_SUPPLY_USB_TYPE_CDP) | BIT(POWER_SUPPLY_USB_TYPE_DCP),
};

static ssize_t fast_charge_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct charlotte_hi6526 *chg = dev_get_drvdata(dev);
	ssize_t len;

	mutex_lock(&chg->lock);
	len = sysfs_emit(buf,
			 "mode=%u protocol=%s tried=%d active=%d rollback_pending=%d type=0x%02x min_mv=%d max_mv=%d max_ma=%d max_mw=%d error=%d stage=%s addr=0x%02x cmd=0x%02x probes_acked=0x%02x diag_valid=0x%03x diag=%*phN\n",
			 protocol_mode, hi6526_protocol_name(chg), chg->protocol_tried,
			 chg->fast, chg->adapter_changed && !chg->fast, chg->adapter_type,
			 chg->adapter_min_mv, chg->adapter_max_mv, chg->adapter_max_ma,
			 chg->adapter_max_mw,
			 chg->protocol_error, chg->protocol_stage ?: "idle",
			 chg->protocol_addr, chg->protocol_cmd, chg->protocol_probe_acked,
			 chg->protocol_diag_valid,
			 (int)sizeof(chg->protocol_diag), chg->protocol_diag);
	mutex_unlock(&chg->lock);
	return len;
}
static DEVICE_ATTR_RO(fast_charge);

static struct attribute *hi6526_attrs[] = {
	&dev_attr_fast_charge.attr,
	NULL,
};
static const struct attribute_group hi6526_group = {
	.attrs = hi6526_attrs,
};

static irqreturn_t hi6526_irq(int irq, void *data)
{
	struct charlotte_hi6526 *chg = data;
	int ret;

	mutex_lock(&chg->lock);
	chg->page_valid = false;
	ret = hi6526_source_event(chg);
	if (!ret && gpiod_get_value_cansleep(chg->irq_gpio))
		ret = -EIO;
	if (ret) {
		/* Keep a stuck alert masked; the poll path will retry safely. */
		disable_irq_nosync(irq);
		chg->irq_failed = true;
		chg->fast = false;
		hi6526_protocol_drop(chg);
		hi6526_update(chg, HI6526_CHG_ENABLE, HI6526_CHG_EN, 0);
		hi6526_update(chg, 0x2d0, 0xf8, 5 << 3);
		hi6526_update(chg, 0x282, 0x1f, 2);
		chg->input_ua = HI6526_INPUT_UA;
		chg->charge_ua = HI6526_CHARGE_UA;
		chg->usb_type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
		chg->detect = true;
		chg->valid = false;
	}
	mutex_unlock(&chg->lock);
	if (ret)
		dev_err(&chg->client->dev, "source IRQ error %d; polling to recover\n", ret);
	if (!READ_ONCE(chg->stopped))
		mod_delayed_work(system_percpu_wq, &chg->work, msecs_to_jiffies(250));
	return IRQ_HANDLED;
}

static void hi6526_work(struct work_struct *work)
{
	struct charlotte_hi6526 *chg = container_of(to_delayed_work(work),
					 struct charlotte_hi6526, work);
	unsigned int delay_ms;
	int ret;

	mutex_lock(&chg->lock);
	ret = hi6526_poll(chg);
	if (!ret && chg->irq_failed && !gpiod_get_value_cansleep(chg->irq_gpio)) {
		chg->irq_failed = false;
		enable_irq(chg->client->irq);
	}
	delay_ms = chg->fast ? 1000 : 5000;
	mutex_unlock(&chg->lock);
	if (ret)
		dev_warn_ratelimited(&chg->client->dev, "charging disabled: telemetry error %d\n", ret);
	power_supply_changed(chg->psy);
	if (!READ_ONCE(chg->stopped))
		queue_delayed_work(system_percpu_wq, &chg->work,
				   msecs_to_jiffies(delay_ms));
}

static void hi6526_stop(void *data)
{
	struct charlotte_hi6526 *chg = data;

	if (READ_ONCE(chg->stopped))
		return;
	WRITE_ONCE(chg->stopped, true);
	if (chg->irq_ready)
		disable_irq(chg->client->irq);
	cancel_delayed_work_sync(&chg->work);
	mutex_lock(&chg->lock);
	chg->page_valid = false;
	hi6526_protocol_drop(chg);
	hi6526_update(chg, HI6526_CHG_ENABLE, HI6526_CHG_EN, 0);
	hi6526_update(chg, HI6526_WDT_CTRL, 7, 0);
	if (chg->irq_ready) {
		hi6526_tcpc_write16(chg, HI6526_ALERT_MASK,
				    get_unaligned_le16(chg->saved_alert_mask));
		hi6526_write(chg, HI6526_POWER_MASK, chg->saved_power_mask);
		hi6526_write(chg, HI6526_GLOBAL_MASK, chg->saved_global_mask);
	}
	mutex_unlock(&chg->lock);
}

static void hi6526_usb_put(void *data)
{
	put_device(data);
}

static int hi6526_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct power_supply_config cfg = {};
	struct platform_device *usb_pdev;
	struct device_node *usb_node;
	struct charlotte_hi6526 *chg;
	u8 id[6];
	int ret;

	if (protocol_mode > 2)
		return dev_err_probe(dev, -EINVAL, "protocol_mode must be 0, 1 or 2\n");
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C | I2C_FUNC_SMBUS_BYTE_DATA))
		return -EOPNOTSUPP;
	chg = devm_kzalloc(dev, sizeof(*chg), GFP_KERNEL);
	if (!chg)
		return -ENOMEM;
	chg->client = client;
	chg->input_ua = HI6526_INPUT_UA;
	chg->charge_ua = HI6526_CHARGE_UA;
	chg->usb_type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
	chg->detect = true;
	mutex_init(&chg->lock);
	INIT_DELAYED_WORK(&chg->work, hi6526_work);
	i2c_set_clientdata(client, chg);
	chg->battery = devm_power_supply_get_by_reference(dev, "hisilicon,battery");
	if (IS_ERR(chg->battery))
		return dev_err_probe(dev, PTR_ERR(chg->battery), "battery reference\n");
	if (!chg->battery)
		return -EPROBE_DEFER;
	usb_node = of_parse_phandle(dev->of_node, "hisilicon,usb-controller", 0);
	if (!usb_node)
		return -EINVAL;
	usb_pdev = of_find_device_by_node(usb_node);
	of_node_put(usb_node);
	if (!usb_pdev)
		return -EPROBE_DEFER;
	chg->usb_dev = &usb_pdev->dev;
	ret = devm_add_action_or_reset(dev, hi6526_usb_put, chg->usb_dev);
	if (ret)
		return ret;
	if (!device_is_bound(chg->usb_dev))
		return -EPROBE_DEFER;
	if (!device_link_add(dev, chg->usb_dev, DL_FLAG_AUTOREMOVE_CONSUMER))
		return -ENOMEM;
	chg->irq_gpio = devm_gpiod_get(dev, "irq", GPIOD_IN);
	if (IS_ERR(chg->irq_gpio))
		return dev_err_probe(dev, PTR_ERR(chg->irq_gpio), "charger interrupt GPIO\n");
	if (client->irq <= 0)
		return -EINVAL;
	mutex_lock(&chg->lock);
	ret = hi6526_read(chg, 0x2c9, id, sizeof(id));
	/* Only the verified board revision: V511 has a different CV encoding. */
	if (!ret && (get_unaligned_le32(id) != 0x36323536 ||
		     get_unaligned_le16(id + 4) != 0x1005))
		ret = -ENODEV;
	if (!ret) {
		ret = hi6526_setup(chg);
		if (ret)
			hi6526_update(chg, HI6526_CHG_ENABLE, HI6526_CHG_EN, 0);
		if (ret == -EIO) {
			/* The chip ID is verified. Keep charging disabled and let
			 * the worker retry transient startup readback failures.
			 */
			chg->setup_pending = true;
			chg->health = POWER_SUPPLY_HEALTH_UNSPEC_FAILURE;
			chg->status = POWER_SUPPLY_STATUS_UNKNOWN;
			dev_warn(dev, "startup setup failed; charging disabled until retry succeeds\n");
			ret = 0;
		}
	}
	if (!ret)
		ret = hi6526_read(chg, HI6526_ALERT_MASK, chg->saved_alert_mask, 2);
	if (!ret)
		ret = hi6526_read(chg, HI6526_POWER_MASK, &chg->saved_power_mask, 1);
	if (!ret)
		ret = hi6526_read(chg, HI6526_GLOBAL_MASK, &chg->saved_global_mask, 1);
	mutex_unlock(&chg->lock);
	if (ret) {
		hi6526_stop(chg);
		return dev_err_probe(dev, ret, "HI6526 V510 setup failed\n");
	}
	cfg.drv_data = chg;
	cfg.fwnode = dev_fwnode(dev);
	chg->supplied_to[0] = "charlotte-battery";
	cfg.supplied_to = chg->supplied_to;
	cfg.num_supplicants = ARRAY_SIZE(chg->supplied_to);
	chg->psy = devm_power_supply_register(dev, &hi6526_desc, &cfg);
	if (IS_ERR(chg->psy)) {
		hi6526_stop(chg);
		return PTR_ERR(chg->psy);
	}
	/* Only CC/VBUS alerts drive this sink-only implementation. Mask the
	 * unported PD/fast-charge IRQ groups; their hardware protections remain.
	 */
	ret = devm_request_threaded_irq(dev, client->irq, NULL, hi6526_irq,
					IRQF_ONESHOT, dev_name(dev), chg);
	if (ret) {
		hi6526_stop(chg);
		return dev_err_probe(dev, ret, "charger source IRQ\n");
	}
	chg->irq_ready = true;
	ret = devm_add_action_or_reset(dev, hi6526_stop, chg);
	if (ret)
		return ret;
	mutex_lock(&chg->lock);
	ret = hi6526_alert_setup(chg);
	mutex_unlock(&chg->lock);
	if (ret)
		return dev_err_probe(dev, ret, "charger source alert masks\n");
	ret = devm_device_add_group(dev, &hi6526_group);
	if (ret)
		return ret;
	queue_delayed_work(system_percpu_wq, &chg->work, 0);
	dev_info(dev, "V510: USB input 475mA, charge 480mA, CV 4398.4mV, watchdog 40s\n");
	return 0;
}

static void hi6526_shutdown(struct i2c_client *client)
{
	hi6526_stop(i2c_get_clientdata(client));
}

static const struct of_device_id hi6526_match[] = {
	{ .compatible = "huawei,hi6526-charger" },
	{}
};
MODULE_DEVICE_TABLE(of, hi6526_match);

static struct i2c_driver hi6526_driver = {
	.driver = {
		.name = "charlotte-hi6526",
		.of_match_table = hi6526_match,
	},
	.probe = hi6526_probe,
	.shutdown = hi6526_shutdown,
};
module_i2c_driver(hi6526_driver);
MODULE_DESCRIPTION("Charlotte HI6526 V510 basic USB charger");
MODULE_LICENSE("GPL");
