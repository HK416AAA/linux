/* SPDX-License-Identifier: GPL-2.0-only */
/* HI6526 V510 ACCP transport and SCP/FCP buck policy.
 * Register map: vendor soc_schargerV600_interface.h (Charlotte).
 * Protocol: vendor schargerV600 and adapter_protocol_{scp,fcp}.
 * Included by charlotte-hi6526.c; every transaction holds chg->lock.
 */
#ifndef _CHARLOTTE_HI6526_PROTOCOL_H
#define _CHARLOTTE_HI6526_PROTOCOL_H

#define HI6526_ACCP_STATUS	0x80
#define HI6526_ACCP_CTRL		0x81
#define HI6526_ACCP_CMD		0x84
#define HI6526_ACCP_ADDR		0x87
#define HI6526_ACCP_WDATA		0x88
#define HI6526_ACCP_RDATA		0x90
#define HI6526_ACCP_ISR1		0x99
#define HI6526_ACCP_ISR2		0x9a
#define HI6526_ACCP_IMR1		0x9b
#define HI6526_ACCP_IMR2		0x9c
#define HI6526_ACCP_IRQ5_MASK	0x9e
#define HI6526_ACCP_IMR1_BITS	0xde
#define HI6526_ACCP_IMR2_BITS	0x1a
#define HI6526_ACCP_DETECT		0x9f
#define HI6526_ACCP_SET		0xa1
#define HI6526_ACCP_RESET		0xa3
#define HI6526_ACCP_IRQ3		0x180
#define HI6526_ACCP_IRQ4		0x181
#define HI6526_ACCP_GLOBAL_MASK	0x1c8
#define HI6526_ACCP_ENABLE	BIT(3)
#define HI6526_ACCP_MASTER_RESET	BIT(2)
#define HI6526_ACCP_SEND		BIT(0)
#define HI6526_ACCP_GOOD		(BIT(7) | BIT(6) | BIT(0))
#define HI6526_ACCP_ACK		BIT(6)
#define HI6526_ACCP_COMPLETE	BIT(7)
#define HI6526_ACCP_NACK		BIT(4)
#define HI6526_ACCP_CRC_PAR	BIT(3)
#define HI6526_ACCP_RX_ERROR	(BIT(4) | BIT(3))
#define HI6526_ACCP_PROTSTAT	BIT(1)
#define HI6526_ACCP_READ		0x0c
#define HI6526_ACCP_WRITE		0x0b

#define HWSCP_TYPE0		0x7e
#define HWSCP_TYPE1		0x80
#define HWSCP_B_TYPE		0x81
#define HWSCP_MIN_VOUT		0x92
#define HWSCP_MAX_VOUT		0x93
#define HWSCP_MIN_IOUT		0x94
#define HWSCP_MAX_IOUT		0x95
#define HWSCP_CTRL0		0xa0
#define HWSCP_CTRL1		0xa1
#define HWSCP_STATUS1		0xa3
#define HWSCP_SSTS		0xa5
#define HWSCP_VBOUND		0xb0
#define HWSCP_IBOUND		0xb2
#define HWSCP_VSET		0xb8
#define HWSCP_ISSET		0xcb

static bool hi6526_protocol_can_probe(int uv, int temp)
{
	/* Identification never raises VBUS and remains useful near full charge.
	 * Keep the narrower battery window for the 9V charging mode.
	 */
	if (protocol_mode == 1)
		return temp >= 50 && temp <= 450 && uv >= 2800000 && uv <= 4430000;
	return protocol_mode == 2 && temp >= 120 && temp <= 400 &&
		uv >= 2800000 && uv <= 4300000;
}

static const char *hi6526_protocol_name(struct charlotte_hi6526 *chg)
{
	switch (chg->protocol) {
	case HI6526_PROTOCOL_FCP:
		return "FCP";
	case HI6526_PROTOCOL_SCP:
		return "SCP";
	default:
		return "none";
	}
}

static int hi6526_verify_update(struct charlotte_hi6526 *chg, u16 reg,
				u8 mask, u8 value)
{
	u8 actual;
	int ret;

	ret = hi6526_update(chg, reg, mask, value);
	if (ret)
		return ret;
	ret = hi6526_read(chg, reg, &actual, 1);
	if (ret)
		return ret;
	return (actual & mask) == value ? 0 : -EIO;
}

static int hi6526_adapter_attached(struct charlotte_hi6526 *chg)
{
	u8 power, alert[2];
	int ret;

	ret = hi6526_read(chg, HI6526_POWER_STATUS, &power, 1);
	if (ret)
		return ret;
	ret = hi6526_read(chg, HI6526_ALERT, alert, sizeof(alert));
	if (ret)
		return ret;
	if (!(power & BIT(2)) || (power & BIT(4)) ||
	    (alert[0] & HI6526_SOURCE_ALERT))
		return -ENOTCONN;
	return 0;
}

static int hi6526_accp_wait(struct charlotte_hi6526 *chg, u16 reg,
			   u8 mask, u8 expected)
{
	u8 value;
	int i, ret;

	for (i = 0; i < 20; i++) {
		ret = hi6526_adapter_attached(chg);
		if (ret)
			return ret;
		ret = hi6526_read(chg, reg, &value, 1);
		if (ret)
			return ret;
		if ((value & mask) == expected)
			return 0;
		msleep(100);
	}
	return -ETIMEDOUT;
}

static int hi6526_accp_detect(struct charlotte_hi6526 *chg)
{
	int ret;

	chg->protocol_stage = "detect-enable";
	/* Mark before the first write so even a partially failed setup is undone. */
	chg->protocol_engine = true;
	/* Vendor masks ACCP IRQ output while the command path polls raw ISR.
	 * Reserved IMR bits read as zero; only verify the documented fields.
	 */
	ret = hi6526_verify_update(chg, HI6526_ACCP_GLOBAL_MASK, BIT(0), BIT(0));
	if (ret)
		return ret;
	ret = hi6526_verify_update(chg, HI6526_ACCP_IMR1,
				   HI6526_ACCP_IMR1_BITS, HI6526_ACCP_IMR1_BITS);
	if (ret)
		return ret;
	ret = hi6526_verify_update(chg, HI6526_ACCP_IMR2,
				   HI6526_ACCP_IMR2_BITS, HI6526_ACCP_IMR2_BITS);
	if (ret)
		return ret;
	ret = hi6526_verify_update(chg, HI6526_ACCP_IRQ5_MASK, 0x03, 0x03);
	if (ret)
		return ret;
	ret = hi6526_update(chg, HI6526_ACCP_DETECT, BIT(3) | BIT(2),
			    BIT(3) | BIT(2));
	if (ret)
		return ret;
	chg->protocol_stage = "detect-set";
	ret = hi6526_accp_wait(chg, HI6526_ACCP_SET, BIT(0), BIT(0));
	if (ret)
		return ret;
	chg->protocol_stage = "ping-enable";
	ret = hi6526_update(chg, HI6526_ACCP_CTRL, HI6526_ACCP_ENABLE,
			    HI6526_ACCP_ENABLE);
	if (ret)
		return ret;
	chg->protocol_stage = "ping";
	return hi6526_accp_wait(chg, HI6526_ACCP_STATUS,
				HI6526_ACCP_GOOD, HI6526_ACCP_GOOD);
}

static int hi6526_accp_xfer_poll(struct charlotte_hi6526 *chg, u8 addr,
				u8 *value, bool write, unsigned int poll_us,
				bool source_first, bool source_poll)
{
	u8 isr1, isr2, irq3, irq4;
	/* Stock successful type reads took about 46ms. Shorter polling must
	 * retain the normal 120ms response window, not shorten it to 20ms.
	 */
	int polls = (120000 + poll_us - 1) / poll_us;
	int i, ret;

	chg->protocol_stage = write ? "adapter-write" : "adapter-read";
	chg->protocol_addr = addr;
	chg->protocol_cmd = write ? HI6526_ACCP_WRITE : HI6526_ACCP_READ;
	ret = hi6526_adapter_attached(chg);
	if (ret)
		return ret;
	ret = hi6526_read(chg, HI6526_ACCP_ISR1, &isr1, 1);
	if (ret)
		return ret;
	ret = hi6526_read(chg, HI6526_ACCP_ISR2, &isr2, 1);
	if (ret)
		return ret;
	/* Match the observed stock bus sequence: acknowledge latched flags,
	 * without writing zero to an idle interrupt register.
	 */
	if (isr1) {
		ret = hi6526_write(chg, HI6526_ACCP_ISR1, isr1);
		if (ret)
			return ret;
	}
	if (isr2) {
		ret = hi6526_write(chg, HI6526_ACCP_ISR2, isr2);
		if (ret)
			return ret;
	}
	ret = hi6526_write(chg, HI6526_ACCP_CMD,
			   write ? HI6526_ACCP_WRITE : HI6526_ACCP_READ);
	if (ret)
		return ret;
	ret = hi6526_write(chg, HI6526_ACCP_ADDR, addr);
	if (ret)
		return ret;
	if (write) {
		ret = hi6526_write(chg, HI6526_ACCP_WDATA, *value);
		if (ret)
			return ret;
	}
	ret = hi6526_update(chg, HI6526_ACCP_CTRL,
			    HI6526_ACCP_SEND, HI6526_ACCP_SEND);
	if (ret)
		return ret;
	for (i = 0; i < polls; i++) {
		usleep_range(poll_us, poll_us + 1000);
		if (source_poll && source_first) {
			ret = hi6526_adapter_attached(chg);
			if (ret)
				return ret;
		}
		ret = hi6526_read(chg, HI6526_ACCP_ISR1, &isr1, 1);
		if (ret)
			return ret;
		ret = hi6526_read(chg, HI6526_ACCP_ISR2, &isr2, 1);
		if (ret)
			return ret;
		ret = hi6526_read(chg, HI6526_ACCP_IRQ3, &irq3, 1);
		if (ret)
			return ret;
		ret = hi6526_read(chg, HI6526_ACCP_IRQ4, &irq4, 1);
		if (ret)
			return ret;
		if (chg->protocol_probe >= 0 && (!i || i == polls - 1 || isr1 || isr2))
			dev_info(&chg->client->dev,
				 "ACCP probe=%d poll=%d first=%d wait=%uus source_first=%d source_poll=%d isr=%02x/%02x irq=%02x/%02x\n",
				 chg->protocol_probe, i, !i, poll_us, source_first, source_poll,
				 isr1, isr2, irq3, irq4);
		/* Validate attachment even when flags are sampled before TCPC reads. */
		if (source_poll && !source_first) {
			ret = hi6526_adapter_attached(chg);
			if (ret)
				return ret;
		}
		if (isr1 & HI6526_ACCP_NACK)
			return -EOPNOTSUPP;
		if ((isr2 & HI6526_ACCP_RX_ERROR) || (irq3 & BIT(1)) ||
		    (((isr1 & HI6526_ACCP_CRC_PAR) || (irq3 & BIT(3)) ||
		      (irq4 & BIT(4))) && (isr2 & HI6526_ACCP_PROTSTAT)))
			return -EPROTO;
		if ((isr1 & (HI6526_ACCP_ACK | HI6526_ACCP_COMPLETE)) ==
		    (HI6526_ACCP_ACK | HI6526_ACCP_COMPLETE)) {
			/* The quiet read-only probe checks source state at both ends.
			 * Never accept a response from an adapter detached meanwhile.
			 */
			if (!source_poll) {
				ret = hi6526_adapter_attached(chg);
				if (ret)
					return ret;
			}
			return write ? 0 : hi6526_read(chg, HI6526_ACCP_RDATA, value, 1);
		}
	}
	if (!source_poll) {
		ret = hi6526_adapter_attached(chg);
		if (ret)
			return ret;
	}
	return -ETIMEDOUT;
}

static int hi6526_accp_xfer(struct charlotte_hi6526 *chg, u8 addr,
			   u8 *value, bool write)
{
	return hi6526_accp_xfer_poll(chg, addr, value, write, 12000, true, true);
}

static int hi6526_accp_restart(struct charlotte_hi6526 *chg);
static void hi6526_protocol_snapshot(struct charlotte_hi6526 *chg);

static int hi6526_adapter_read(struct charlotte_hi6526 *chg, u8 addr, u8 *val)
{
	int attempt, ret;

	/* Restart failed reads as in the vendor path, capped at 3 tries.
	 * Do not replay writes: a lost ACK may still have changed adapter output.
	 */
	for (attempt = 0; attempt < 3; attempt++) {
		ret = hi6526_accp_xfer(chg, addr, val, false);
		if (ret != -ETIMEDOUT && ret != -EPROTO)
			return ret;
		hi6526_protocol_snapshot(chg);
		dev_info(&chg->client->dev, "ACCP read addr=%#x attempt=%d error=%d\n",
			 addr, attempt + 1, ret);
		if (attempt == 2)
			return ret;
		ret = hi6526_accp_restart(chg);
		if (ret)
			return ret;
	}
	return -ETIMEDOUT;
}

static int hi6526_adapter_write(struct charlotte_hi6526 *chg, u8 addr, u8 val)
{
	return hi6526_accp_xfer(chg, addr, &val, true);
}

static int hi6526_adapter_write16(struct charlotte_hi6526 *chg, u8 addr, int val)
{
	int ret = hi6526_adapter_write(chg, addr, val >> 8);

	return ret ? ret : hi6526_adapter_write(chg, addr + 1, val & 0xff);
}

static int hi6526_scp_decimal(u8 val)
{
	static const int scale[] = { 1, 10, 100, 1000 };

	return (val & 0x3f) * scale[val >> 6];
}

static int hi6526_accp_restart(struct charlotte_hi6526 *chg)
{
	u8 status;
	int i, ret;

	chg->protocol_stage = "restart";
	ret = hi6526_update(chg, HI6526_ACCP_CTRL, HI6526_ACCP_ENABLE, 0);
	if (ret)
		return ret;
	usleep_range(9000, 10000);
	ret = hi6526_update(chg, HI6526_ACCP_CTRL,
			    HI6526_ACCP_ENABLE, HI6526_ACCP_ENABLE);
	if (ret)
		return ret;
	for (i = 0; i < 4; i++) {
		usleep_range(9000, 10000);
		ret = hi6526_adapter_attached(chg);
		if (ret)
			return ret;
		ret = hi6526_read(chg, HI6526_ACCP_STATUS, &status, 1);
		if (ret)
			return ret;
		if ((status & HI6526_ACCP_GOOD) == HI6526_ACCP_GOOD)
			return 0;
	}
	return -ETIMEDOUT;
}

static int hi6526_adapter_identify(struct charlotte_hi6526 *chg)
{
	u8 type = 0, value;
	int ret, i, min_ma;

	ret = hi6526_adapter_read(chg, HWSCP_TYPE0, &type);
	/* Vendor also tries 0x80 if optional SCP 0x7e has no response.
	 * Never infer FCP from that error; its ID and capability reads must ACK.
	 */
	if (ret && ret != -EOPNOTSUPP && ret != -ETIMEDOUT)
		return ret;
	if (ret) {
		ret = hi6526_accp_restart(chg);
		if (ret)
			return ret;
	}
	chg->adapter_type = type;
	if (!ret && (type & (BIT(7) | BIT(5) | BIT(4)))) {
		chg->protocol = HI6526_PROTOCOL_SCP;
	} else {
		ret = hi6526_adapter_read(chg, HWSCP_TYPE1, &value);
		if (ret && ret != -EOPNOTSUPP && ret != -ETIMEDOUT)
			return ret;
		if (ret) {
			ret = hi6526_accp_restart(chg);
			if (ret)
				return ret;
			value = 0;
		}
		if (!ret && (value & BIT(4))) {
			ret = hi6526_adapter_read(chg, HWSCP_B_TYPE, &value);
			if (ret)
				return ret;
			if (value == BIT(4))
				chg->protocol = HI6526_PROTOCOL_SCP;
		}
	}
	if (chg->protocol == HI6526_PROTOCOL_SCP) {
		ret = hi6526_adapter_read(chg, HWSCP_MIN_VOUT, &value);
		if (ret)
			return ret;
		chg->adapter_min_mv = hi6526_scp_decimal(value);
		ret = hi6526_adapter_read(chg, HWSCP_MAX_VOUT, &value);
		if (ret)
			return ret;
		chg->adapter_max_mv = hi6526_scp_decimal(value);
		ret = hi6526_adapter_read(chg, HWSCP_MIN_IOUT, &value);
		if (ret)
			return ret;
		min_ma = hi6526_scp_decimal(value);
		ret = hi6526_adapter_read(chg, HWSCP_MAX_IOUT, &value);
		if (ret)
			return ret;
		/* Saturating here deliberately ignores >6A extension capabilities. */
		chg->adapter_max_ma = hi6526_scp_decimal(value);
		ret = hi6526_adapter_read(chg, 0x90, &value);
		if (ret)
			return ret;
		chg->adapter_max_mw = (value & 0x7f) * ((value & BIT(7)) ? 1000 : 100);
		if (min_ma > 1500)
			return -EOPNOTSUPP;
		return 0;
	}
	/* FCP advertises a bounded list of discrete voltages and total power. */
	ret = hi6526_adapter_read(chg, 0x04, &value);
	if (ret)
		return ret;
	ret = hi6526_adapter_read(chg, 0x21, &value);
	if (ret)
		return ret;
	if (value > 2)
		return -EOPNOTSUPP;
	for (i = 0; i <= value; i++) {
		u8 voltage;

		ret = hi6526_adapter_read(chg, 0x30 + i, &voltage);
		if (ret)
			return ret;
		if (voltage == 90)
			chg->adapter_max_mv = 9000;
	}
	ret = hi6526_adapter_read(chg, 0x22, &value);
	if (ret)
		return ret;
	chg->adapter_min_mv = 5000;
	chg->adapter_max_mw = value * 500;
	chg->adapter_max_ma = value * 500 / 9;
	chg->protocol = HI6526_PROTOCOL_FCP;
	return 0;
}

static int hi6526_buck_voltage(struct charlotte_hi6526 *chg, bool high)
{
	int ret;

	/* V510 9V analogue preset, OVP=10.5V, UVP=3.8V, VBUS selector=1.
	 * All writes occur with CHG_EN cleared. Never enable LVC/SC or OTG.
	 */
	ret = hi6526_verify_update(chg, 0x2e7, 0xff, high ? 0xef : 0xaf);
	if (ret)
		return ret;
	ret = hi6526_verify_update(chg, 0x2a5, 0xc0, high ? BIT(6) : 0);
	if (ret)
		return ret;
	ret = hi6526_verify_update(chg, 0x2a6, 0x18, 0);
	if (ret)
		return ret;
	return hi6526_verify_update(chg, 0x2a4, 0x0c, high ? BIT(2) : 0);
}

static int hi6526_adapter_voltage_check(struct charlotte_hi6526 *chg, bool high)
{
	u8 power;
	int i, ret, uv;

	for (i = 0; i < 10; i++) {
		ret = hi6526_read(chg, HI6526_POWER_STATUS, &power, 1);
		if (ret)
			return ret;
		if (!(power & BIT(2)) || (power & BIT(4)))
			return high ? -ENOTCONN : 0;
		ret = hi6526_vbus(chg, &uv);
		if (ret)
			return ret;
		if (high ? (uv >= 8500000 && uv <= 9500000) :
			   (uv >= 4350000 && uv <= 5800000))
			return 0;
		msleep(100);
	}
	return -ETIMEDOUT;
}

static int hi6526_protocol_drop(struct charlotte_hi6526 *chg)
{
	int ret, err = 0;

	if (!chg->protocol_engine && !chg->adapter_changed)
		return 0;
	chg->fast = false;
	ret = hi6526_update(chg, HI6526_CHG_ENABLE, HI6526_CHG_EN, 0);
	if (ret)
		err = ret;
	if (chg->buck_reset_pending) {
		ret = hi6526_verify_update(chg, 0x2d2, BIT(7) | BIT(3), BIT(7));
		if (!ret)
			chg->buck_reset_pending = false;
		if (!err)
			err = ret;
	}
	if (chg->adapter_changed) {
		ret = hi6526_write(chg, HI6526_ACCP_CTRL,
				   HI6526_ACCP_ENABLE | HI6526_ACCP_MASTER_RESET);
		if (!err)
			err = ret;
		ret = hi6526_adapter_voltage_check(chg, false);
		if (!err)
			err = ret;
		/* Keep the wider OVP and retry if voltage has not returned to 5V. */
		if (err)
			return err;
	}
	ret = hi6526_write(chg, HI6526_ACCP_RESET, 0xac);
	if (ret)
		return ret;
	usleep_range(10000, 11000);
	ret = hi6526_write(chg, HI6526_ACCP_RESET, 0x5a);
	if (ret)
		return ret;
	ret = hi6526_write(chg, HI6526_ACCP_CTRL, 0);
	if (ret)
		return ret;
	ret = hi6526_update(chg, HI6526_ACCP_DETECT, BIT(3) | BIT(2), 0);
	if (ret)
		return ret;
	ret = hi6526_verify_update(chg, HI6526_ACCP_IMR1,
				   HI6526_ACCP_IMR1_BITS, HI6526_ACCP_IMR1_BITS);
	if (ret)
		return ret;
	ret = hi6526_verify_update(chg, HI6526_ACCP_IMR2,
				   HI6526_ACCP_IMR2_BITS, HI6526_ACCP_IMR2_BITS);
	if (ret)
		return ret;
	ret = hi6526_buck_voltage(chg, false);
	if (ret)
		return ret;
	chg->protocol_engine = false;
	chg->adapter_changed = false;
	chg->fast = false;
	return err;
}

static int hi6526_adapter_start(struct charlotte_hi6526 *chg)
{
	static const u16 fault_regs[] = {
		HI6526_POWER_STATUS, HI6526_BUCK_OK, HI6526_CHG_ENABLE,
		0xfb, 0xfe, 0x2d2, 0x2d9, 0x2de, 0x2e9, 0x2e0,
		0x1d2, 0x1d3, 0x1d4, 0x1d5, 0x1d6, 0x1d7, 0x1d8, 0x1d9,
		0xe3, 0xe4, 0xe5,
	};
	u8 value;
	int ret;

	if (chg->adapter_min_mv > 9000 || chg->adapter_max_mv < 9000 ||
	    chg->adapter_max_ma < 1500 || chg->adapter_max_mw < 13500 ||
	    (chg->protocol == HI6526_PROTOCOL_SCP && !(chg->adapter_type & BIT(3))))
		return -EOPNOTSUPP;
	ret = hi6526_buck_voltage(chg, true);
	if (ret)
		return ret;
	/* Set before any adapter write: rollback handles a lost write ACK. */
	chg->adapter_changed = true;
	if (chg->protocol == HI6526_PROTOCOL_SCP) {
		ret = hi6526_adapter_read(chg, HWSCP_CTRL0, &value);
		if (ret)
			return ret;
		ret = hi6526_adapter_write(chg, HWSCP_CTRL0, value | BIT(6));
		if (ret)
			return ret;
		ret = hi6526_adapter_write16(chg, HWSCP_VBOUND, 9000);
		if (ret)
			return ret;
		ret = hi6526_adapter_write16(chg, HWSCP_IBOUND, 1500);
		if (ret)
			return ret;
		ret = hi6526_adapter_write(chg, HWSCP_ISSET, 1500 / 50);
		if (ret)
			return ret;
		ret = hi6526_adapter_read(chg, HWSCP_CTRL1, &value);
		if (ret)
			return ret;
		/* 3s adapter watchdog; active charging polls every second. */
		ret = hi6526_adapter_write(chg, HWSCP_CTRL1, (value & ~7) | 6);
		if (ret)
			return ret;
		ret = hi6526_adapter_write16(chg, HWSCP_VSET, 9000);
		if (ret)
			return ret;
		ret = hi6526_adapter_read(chg, HWSCP_CTRL0, &value);
		if (ret)
			return ret;
		ret = hi6526_adapter_write(chg, HWSCP_CTRL0, value | BIT(7) | BIT(6));
	} else {
		ret = hi6526_adapter_write(chg, 0x2c, 90);
		if (ret)
			return ret;
		ret = hi6526_adapter_read(chg, 0x2c, &value);
		if (ret)
			return ret;
		if (value != 90)
			return -EIO;
		ret = hi6526_adapter_write(chg, 0x2b, 1);
	}
	if (ret)
		return ret;
	ret = hi6526_adapter_voltage_check(chg, true);
	if (ret)
		return ret;
	ret = hi6526_adapter_attached(chg);
	if (ret)
		return ret;
	/* VBUS can reach 9V before the buck power-good indication settles.
	 * Keep charging disabled and the USB current limits until both agree.
	 * A persistent missing power-good falls back through the normal reset.
	 */
	chg->protocol_stage = "buck-ready";
	for (int i = 0; i < 50; i++) {
		ret = hi6526_adapter_attached(chg);
		if (ret)
			return ret;
		ret = hi6526_read(chg, HI6526_BUCK_OK, &value, 1);
		if (ret)
			return ret;
		if (value & BIT(0))
			break;
		if (!i) {
			u8 flags, protection;

			ret = hi6526_read(chg, 0x1d4, &flags, 1);
			if (ret)
				return ret;
			if (flags & BIT(7)) {
				/* Raw bit7 was raised with missing power-good. Try
				 * the reverse-boost reset pulse from vendor buck mode
				 * setup, retaining protection and requiring live PG.
				 */
				chg->protocol_stage = "buck-reset";
				ret = hi6526_read(chg, 0x2d2, &protection, 1);
				if (ret)
					return ret;
				if (!(protection & BIT(7)))
					return -EIO;
				/* Also restore a possibly accepted pulse on any I/O
				 * error; drop keeps charging off until reset is low.
				 */
				chg->buck_reset_pending = true;
				ret = hi6526_update(chg, 0x2d2, BIT(3), BIT(3));
				if (ret)
					return ret;
				ret = hi6526_verify_update(chg, 0x2d2, BIT(7) | BIT(3),
							   protection & BIT(7));
				if (ret)
					return ret;
				chg->buck_reset_pending = false;
				dev_info(&chg->client->dev, "buck reverse-boost reset pulse\n");
				chg->protocol_stage = "buck-ready";
			}
		}
		if (i == 49) {
			/* Read-only buck state/latched IRQ evidence before reset.
			 * These groups remain masked; do not acknowledge their flags.
			 */
			for (int j = 0; j < ARRAY_SIZE(fault_regs); j++) {
				ret = hi6526_read(chg, fault_regs[j], &value, 1);
				if (ret)
					return ret;
				dev_info(&chg->client->dev, "buck timeout reg=%#x value=%#x\n",
					 fault_regs[j], value);
			}
			return -ETIMEDOUT;
		}
		msleep(20);
	}
	ret = hi6526_verify_update(chg, 0x2d0, 0xf8, HI6526_FAST_INPUT_CODE << 3);
	if (ret)
		return ret;
	ret = hi6526_verify_update(chg, 0x282, 0x1f, HI6526_FAST_CHARGE_CODE);
	if (ret)
		return ret;
	chg->input_ua = HI6526_FAST_INPUT_UA;
	chg->charge_ua = HI6526_FAST_CHARGE_UA;
	chg->fast = true;
	return 0;
}

/* Capture before protocol_drop resets the engine; never clear IRQs here. */
static void hi6526_protocol_snapshot(struct charlotte_hi6526 *chg)
{
	static const u16 regs[] = {
		HI6526_ACCP_STATUS, HI6526_ACCP_CTRL, HI6526_ACCP_DETECT,
		HI6526_ACCP_SET, HI6526_ACCP_ISR1, HI6526_ACCP_ISR2,
		HI6526_ACCP_IRQ3, HI6526_ACCP_IRQ4, HI6526_ACCP_CMD,
		HI6526_ACCP_ADDR, HI6526_ACCP_IMR1, HI6526_ACCP_IMR2,
		HI6526_ACCP_IRQ5_MASK, 0xa2,
		HI6526_ACCP_RDATA, HI6526_ACCP_GLOBAL_MASK,
	};
	int i;

	chg->protocol_diag_valid = 0;
	for (i = 0; i < ARRAY_SIZE(regs); i++)
		if (!hi6526_read(chg, regs[i], &chg->protocol_diag[i], 1))
			chg->protocol_diag_valid |= BIT(i);
	dev_info(&chg->client->dev,
		 "ACCP stage=%s addr=%#x cmd=%#x valid=%#x status/ctrl/detect/set/isr1/isr2/irq3/irq4/cmd/addr/imr1/imr2/irq5mask/ready/rdata/globalmask=%*phN\n",
		 chg->protocol_stage, chg->protocol_addr, chg->protocol_cmd,
		 chg->protocol_diag_valid, (int)sizeof(chg->protocol_diag),
		 chg->protocol_diag);
}

static int hi6526_accp_probe(struct charlotte_hi6526 *chg)
{
	static const struct {
		unsigned int poll_us;
		bool source_first;
		bool individual_unmask;
		bool group_unmask;
		bool source_poll;
		bool stock_mask;
	} probes[] = {
		{ 12000, true, false, false, true, false },
		{ 12000, false, false, false, true, false },
		{ 2000, false, false, false, true, false },
		{ 2000, false, true, false, true, false },
		{ 2000, false, true, true, true, false },
		/* Match stock polling: no TCPC access until ACK or timeout. */
		{ 12000, false, false, false, false, false },
		/* Stock I2C capture: compare the full group mask, then restore. */
		{ 12000, false, false, false, false, true },
	};
	u8 saved_mask, value = 0;
	int i, ret, restore_ret;

	ret = hi6526_read(chg, HI6526_ACCP_GLOBAL_MASK, &saved_mask, 1);
	if (ret)
		return ret;
	/* Read 0x7e only. Never use unconfirmed RDATA or change adapter voltage.
	 * Caller holds chg->lock: a shared IRQ thread waits until the group mask
	 * is restored. The full stock mask is limited to one read-only window;
	 * other IRQ groups are never left enabled for normal operation. Do not clear its ACK/error latches in the IRQ handler.
	 */
	for (i = 0; i < ARRAY_SIZE(probes); i++) {
		chg->protocol_probe = i;
		ret = hi6526_verify_update(chg, HI6526_ACCP_IMR1,
				HI6526_ACCP_IMR1_BITS,
				probes[i].individual_unmask ? 0 : HI6526_ACCP_IMR1_BITS);
		if (ret)
			break;
		ret = hi6526_verify_update(chg, HI6526_ACCP_IMR2,
				HI6526_ACCP_IMR2_BITS,
				probes[i].individual_unmask ? 0 : HI6526_ACCP_IMR2_BITS);
		if (ret)
			break;
		ret = hi6526_verify_update(chg, HI6526_ACCP_GLOBAL_MASK,
					   probes[i].stock_mask ? 0xff : BIT(0),
					   probes[i].stock_mask ? 0x05 :
					   probes[i].group_unmask ? 0 : BIT(0));
		if (ret)
			break;
		ret = hi6526_accp_xfer_poll(chg, HWSCP_TYPE0, &value, false,
					   probes[i].poll_us, probes[i].source_first,
					   probes[i].source_poll);
		if (!ret)
			chg->protocol_probe_acked |= BIT(i);
		dev_info(&chg->client->dev,
			 "ACCP probe=%d individual_unmask=%d group_unmask=%d stock_mask=%d result=%d acked_data=%#x\n",
			 i, probes[i].individual_unmask, probes[i].group_unmask,
			 probes[i].stock_mask, ret, ret ? 0 : value);
		hi6526_protocol_snapshot(chg);
		restore_ret = hi6526_verify_update(chg, HI6526_ACCP_GLOBAL_MASK,
						 0xff, saved_mask);
		if (restore_ret) {
			ret = restore_ret;
			break;
		}
		if (ret && ret != -ETIMEDOUT && ret != -EPROTO && ret != -EOPNOTSUPP)
			break;
		ret = hi6526_accp_restart(chg);
		if (ret)
			break;
	}
	chg->protocol_probe = -1;
	dev_info(&chg->client->dev, "ACCP probes ACKed=%#x\n", chg->protocol_probe_acked);
	/* Restore masks on every exit, including partial setup/I2C failure. */
	restore_ret = hi6526_verify_update(chg, HI6526_ACCP_GLOBAL_MASK, 0xff, saved_mask);
	if (!ret)
		ret = restore_ret;
	restore_ret = hi6526_verify_update(chg, HI6526_ACCP_IMR1,
					 HI6526_ACCP_IMR1_BITS, HI6526_ACCP_IMR1_BITS);
	if (!ret)
		ret = restore_ret;
	restore_ret = hi6526_verify_update(chg, HI6526_ACCP_IMR2,
					 HI6526_ACCP_IMR2_BITS, HI6526_ACCP_IMR2_BITS);
	return ret ? ret : restore_ret;
}

static int hi6526_protocol_run(void *data)
{
	struct charlotte_hi6526 *chg = data;
	int ret, drop_ret;

	chg->protocol_addr = 0;
	chg->protocol_cmd = 0;
	chg->protocol_probe = -1;
	chg->protocol_probe_acked = 0;
	chg->protocol_diag_valid = 0;
	memset(chg->protocol_diag, 0, sizeof(chg->protocol_diag));
	ret = hi6526_accp_detect(chg);
	if (!ret && protocol_diag && protocol_mode == 1)
		ret = hi6526_accp_probe(chg);
	if (!ret)
		ret = hi6526_adapter_identify(chg);
	if (!ret && protocol_mode == 2)
		ret = hi6526_adapter_start(chg);
	chg->protocol_error = ret;
	if (ret || protocol_mode == 1)
		hi6526_protocol_snapshot(chg);
	if (!ret)
		chg->protocol_stage = chg->fast ? "active" : "identified";
	dev_info(&chg->client->dev,
		 "adapter=%s type=%#x range=%d-%dmV max=%dmA fast=%d error=%d\n",
		 hi6526_protocol_name(chg), chg->adapter_type, chg->adapter_min_mv,
		 chg->adapter_max_mv, chg->adapter_max_ma, chg->fast, ret);
	if (!ret && chg->fast)
		return 0;
	drop_ret = hi6526_protocol_drop(chg);
	if (drop_ret) {
		chg->protocol_error = drop_ret;
		return drop_ret;
	}
	/* Unsupported chargers are an ordinary 5V DCP; other errors revoke charging. */
	return ret == -EOPNOTSUPP || ret == -ETIMEDOUT ? 0 : ret;
}

static int hi6526_protocol_keepalive(struct charlotte_hi6526 *chg)
{
	u8 value;
	int ret;

	ret = hi6526_adapter_attached(chg);
	if (ret)
		return ret;
	if (chg->protocol == HI6526_PROTOCOL_FCP) {
		ret = hi6526_adapter_read(chg, 0x28, &value);
		return ret ? ret : (value & 0x0f) ? -EIO : 0;
	}
	ret = hi6526_adapter_read(chg, HWSCP_STATUS1, &value);
	if (ret)
		return ret;
	if (value)
		return -EIO;
	ret = hi6526_adapter_read(chg, HWSCP_SSTS, &value);
	if (ret)
		return ret;
	/* Drop indication can reduce the advertised budget; fall back and recheck later. */
	if (value & BIT(0))
		return -EAGAIN;
	return hi6526_adapter_write(chg, HWSCP_ISSET, 1500 / 50);
}

#endif
