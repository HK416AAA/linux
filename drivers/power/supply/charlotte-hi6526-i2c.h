/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _CHARLOTTE_HI6526_I2C_H
#define _CHARLOTTE_HI6526_I2C_H

/* Caller holds chg->lock across the cache and all bus transactions.
 * Match vendor __switch_page(): do not rewrite the selector on every read.
 */
static int hi6526_select_page(struct charlotte_hi6526 *chg, u16 reg)
{
	u8 page = reg >> 8;
	int ret;

	if (reg < 0x80 || (chg->page_valid && chg->page == page))
		return 0;
	chg->page_valid = false;
	ret = i2c_smbus_write_byte_data(chg->client, HI6526_PAGE, page);
	if (ret)
		return ret;
	chg->page = page;
	chg->page_valid = true;
	return 0;
}
static int hi6526_read(struct charlotte_hi6526 *chg, u16 reg, void *buf, u16 len)
{
	u8 addr = reg;
	struct i2c_msg msgs[] = {
		{ .addr = chg->client->addr, .len = 1, .buf = &addr },
		{ .addr = chg->client->addr, .flags = I2C_M_RD, .len = len, .buf = buf },
	};
	int ret;

	ret = hi6526_select_page(chg, reg);
	if (ret)
		return ret;
	ret = i2c_transfer(chg->client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret == ARRAY_SIZE(msgs))
		return 0;
	chg->page_valid = false;
	return ret < 0 ? ret : -EIO;
}

static int hi6526_write(struct charlotte_hi6526 *chg, u16 reg, u8 val)
{
	int ret;

	ret = hi6526_select_page(chg, reg);
	if (ret)
		return ret;
	ret = i2c_smbus_write_byte_data(chg->client, reg, val);
	if (ret)
		chg->page_valid = false;
	return ret;
}

static int hi6526_tcpc_write16(struct charlotte_hi6526 *chg, u8 reg, u16 val)
{
	u8 data[] = { reg, val & 0xff, val >> 8 };
	struct i2c_msg msg = {
		.addr = chg->client->addr, .len = sizeof(data), .buf = data,
	};
	int ret = i2c_transfer(chg->client->adapter, &msg, 1);

	if (ret == 1)
		return 0;
	chg->page_valid = false;
	return ret < 0 ? ret : -EIO;
}

#endif
