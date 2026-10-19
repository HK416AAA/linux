// SPDX-License-Identifier: GPL-2.0
/*
 * HiSilicon I2C Controller Driver for Kunpeng SoC
 *
 * Copyright (c) 2021 HiSilicon Technologies Co., Ltd.
 */

#include <linux/bits.h>
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/of.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/units.h>

#define HISI_I2C_FRAME_CTRL		0x0000
#define   HISI_I2C_FRAME_CTRL_SPEED_MODE	GENMASK(1, 0)
#define   HISI_I2C_FRAME_CTRL_ADDR_TEN	BIT(2)
#define   HISI_I2C_FRAME_CTRL_RESTART_EN	BIT(3)
#define HISI_I2C_SLV_ADDR		0x0004
#define   HISI_I2C_SLV_ADDR_VAL		GENMASK(9, 0)
#define   HISI_I2C_SLV_ADDR_GC_S_MODE	BIT(10)
#define   HISI_I2C_SLV_ADDR_GC_S_EN	BIT(11)
#define HISI_I2C_CMD_TXDATA		0x0008
#define   HISI_I2C_CMD_TXDATA_DATA	GENMASK(7, 0)
#define   HISI_I2C_CMD_TXDATA_RW	BIT(8)
#define   HISI_I2C_CMD_TXDATA_P_EN	BIT(9)
#define   HISI_I2C_CMD_TXDATA_SR_EN	BIT(10)
#define HISI_I2C_RXDATA			0x000c
#define   HISI_I2C_RXDATA_DATA		GENMASK(7, 0)
#define HISI_I2C_SS_SCL_HCNT		0x0010
#define HISI_I2C_SS_SCL_LCNT		0x0014
#define HISI_I2C_FS_SCL_HCNT		0x0018
#define HISI_I2C_FS_SCL_LCNT		0x001c
#define HISI_I2C_HS_SCL_HCNT		0x0020
#define HISI_I2C_HS_SCL_LCNT		0x0024
#define HISI_I2C_FIFO_CTRL		0x0028
#define   HISI_I2C_FIFO_RX_CLR		BIT(0)
#define   HISI_I2C_FIFO_TX_CLR		BIT(1)
#define   HISI_I2C_FIFO_RX_AF_THRESH	GENMASK(7, 2)
#define   HISI_I2C_FIFO_TX_AE_THRESH	GENMASK(13, 8)
#define HISI_I2C_FIFO_STATE		0x002c
#define   HISI_I2C_FIFO_STATE_RX_RERR	BIT(0)
#define   HISI_I2C_FIFO_STATE_RX_WERR	BIT(1)
#define   HISI_I2C_FIFO_STATE_RX_EMPTY	BIT(3)
#define   HISI_I2C_FIFO_STATE_TX_RERR	BIT(6)
#define   HISI_I2C_FIFO_STATE_TX_WERR	BIT(7)
#define   HISI_I2C_FIFO_STATE_TX_FULL	BIT(11)
#define HISI_I2C_SDA_HOLD		0x0030
#define   HISI_I2C_SDA_HOLD_TX		GENMASK(15, 0)
#define   HISI_I2C_SDA_HOLD_RX		GENMASK(23, 16)
#define HISI_I2C_FS_SPK_LEN		0x0038
#define   HISI_I2C_FS_SPK_LEN_CNT	GENMASK(7, 0)
#define HISI_I2C_HS_SPK_LEN		0x003c
#define   HISI_I2C_HS_SPK_LEN_CNT	GENMASK(7, 0)
#define HISI_I2C_TX_INT_CLR		0x0040
#define   HISI_I2C_TX_AEMPTY_INT		BIT(0)
#define HISI_I2C_INT_MSTAT		0x0044
#define HISI_I2C_INT_CLR		0x0048
#define HISI_I2C_INT_MASK		0x004C
#define HISI_I2C_TRANS_STATE		0x0050
#define HISI_I2C_TRANS_ERR		0x0054
#define HISI_I2C_VERSION		0x0058

#define HISI_I2C_INT_ALL	GENMASK(4, 0)
#define HISI_I2C_INT_TRANS_CPLT	BIT(0)
#define HISI_I2C_INT_TRANS_ERR	BIT(1)
#define HISI_I2C_INT_FIFO_ERR	BIT(2)
#define HISI_I2C_INT_RX_FULL	BIT(3)
#define HISI_I2C_INT_TX_EMPTY	BIT(4)
#define HISI_I2C_INT_ERR \
	(HISI_I2C_INT_TRANS_ERR | HISI_I2C_INT_FIFO_ERR)

#define HISI_I2C_STD_SPEED_MODE		0
#define HISI_I2C_FAST_SPEED_MODE	1
#define HISI_I2C_HIGH_SPEED_MODE	2

#define HISI_I2C_TX_FIFO_DEPTH		64
#define HISI_I2C_RX_FIFO_DEPTH		64
#define HISI_I2C_TX_F_AE_THRESH		1
#define HISI_I2C_RX_F_AF_THRESH		60

#define NSEC_TO_CYCLES(ns, clk_rate_khz) \
	DIV_ROUND_UP_ULL((clk_rate_khz) * (ns), NSEC_PER_MSEC)

struct hisi_i2c_controller {
	struct i2c_adapter adapter;
	void __iomem *iobase;
	struct device *dev;
	struct clk *clk;
	int irq;
	bool polling;

	/* Intermediates for recording the transfer process */
	struct completion *completion;
	struct i2c_msg *msgs;
	int msg_num;
	int msg_tx_idx;
	int buf_tx_idx;
	int msg_rx_idx;
	int buf_rx_idx;
	u16 tar_addr;
	u32 xfer_err;

	/* I2C bus configuration */
	struct i2c_timings t;
	u32 clk_rate_khz;
	u32 spk_len;

	/* Board-local mux handling until Charlotte pinctrl is available. */
	void __iomem *mux;
	void __iomem *func;
	u32 saved_mux[2];
	u32 saved_func[2];
	struct i2c_bus_recovery_info recovery;
};

static bool charlotte_i2c_v400(struct device *dev)
{
	return of_device_is_compatible(dev->of_node, "hisilicon,charlotte-i2c-v400") ||
		of_device_is_compatible(dev->of_node, "hisilicon,charlotte-i2c7-v400");
}

static void hisi_i2c_enable_int(struct hisi_i2c_controller *ctlr, u32 mask)
{
	writel_relaxed(mask, ctlr->iobase + HISI_I2C_INT_MASK);
}

static void hisi_i2c_disable_int(struct hisi_i2c_controller *ctlr, u32 mask)
{
	writel_relaxed((~mask) & HISI_I2C_INT_ALL, ctlr->iobase + HISI_I2C_INT_MASK);
}

static void hisi_i2c_clear_int(struct hisi_i2c_controller *ctlr, u32 mask)
{
	writel_relaxed(mask, ctlr->iobase + HISI_I2C_INT_CLR);
}

static void hisi_i2c_clear_tx_int(struct hisi_i2c_controller *ctlr, u32 mask)
{
	/* B236 v400 and the working I2C7 diagnostic clear status through
	 * INT_CLR (0x48). Do not touch Kunpeng's separate TX-clear register.
	 */
	if (ctlr->polling)
		return;
	writel_relaxed(mask, ctlr->iobase + HISI_I2C_TX_INT_CLR);
}

static void hisi_i2c_handle_errors(struct hisi_i2c_controller *ctlr)
{
	u32 int_err = ctlr->xfer_err, reg;

	if (int_err & HISI_I2C_INT_FIFO_ERR) {
		reg = readl(ctlr->iobase + HISI_I2C_FIFO_STATE);

		if (reg & HISI_I2C_FIFO_STATE_RX_RERR)
			dev_err(ctlr->dev, "rx fifo error read\n");

		if (reg & HISI_I2C_FIFO_STATE_RX_WERR)
			dev_err(ctlr->dev, "rx fifo error write\n");

		if (reg & HISI_I2C_FIFO_STATE_TX_RERR)
			dev_err(ctlr->dev, "tx fifo error read\n");

		if (reg & HISI_I2C_FIFO_STATE_TX_WERR)
			dev_err(ctlr->dev, "tx fifo error write\n");
	}
}

static void hisi_i2c_xfer_msg(struct hisi_i2c_controller *ctlr);
static irqreturn_t hisi_i2c_irq(int irq, void *context);

static int hisi_i2c_start_xfer(struct hisi_i2c_controller *ctlr)
{
	struct i2c_msg *msg = ctlr->msgs;
	u32 reg;

	reg = readl(ctlr->iobase + HISI_I2C_FRAME_CTRL);
	reg &= ~HISI_I2C_FRAME_CTRL_ADDR_TEN;
	if (msg->flags & I2C_M_TEN)
		reg |= HISI_I2C_FRAME_CTRL_ADDR_TEN;
	writel(reg, ctlr->iobase + HISI_I2C_FRAME_CTRL);

	reg = readl(ctlr->iobase + HISI_I2C_SLV_ADDR);
	reg &= ~HISI_I2C_SLV_ADDR_VAL;
	reg |= FIELD_PREP(HISI_I2C_SLV_ADDR_VAL, msg->addr);
	writel(reg, ctlr->iobase + HISI_I2C_SLV_ADDR);

	reg = readl(ctlr->iobase + HISI_I2C_FIFO_CTRL);
	reg |= HISI_I2C_FIFO_RX_CLR | HISI_I2C_FIFO_TX_CLR;
	writel(reg, ctlr->iobase + HISI_I2C_FIFO_CTRL);
	reg &= ~(HISI_I2C_FIFO_RX_CLR | HISI_I2C_FIFO_TX_CLR);
	writel(reg, ctlr->iobase + HISI_I2C_FIFO_CTRL);

	hisi_i2c_clear_int(ctlr, HISI_I2C_INT_ALL);
	hisi_i2c_clear_tx_int(ctlr, HISI_I2C_TX_AEMPTY_INT);
	if (charlotte_i2c_v400(ctlr->dev)) {
		/* v400 does not raise TX_EMPTY for an initially empty FIFO.
		 * Seed commands before unmasking IRQs, as the vendor driver does.
		 * This also prevents IRQ and process context filling it together.
		 */
		/* Match the successful I2C7 polled sequence: status reporting
		 * must be enabled before enqueueing commands. No IRQ is used.
		 */
		if (ctlr->polling)
			hisi_i2c_enable_int(ctlr, HISI_I2C_INT_ALL & ~HISI_I2C_INT_TX_EMPTY);
		hisi_i2c_xfer_msg(ctlr);
		hisi_i2c_enable_int(ctlr, ctlr->msg_tx_idx == ctlr->msg_num ?
			HISI_I2C_INT_ALL & ~HISI_I2C_INT_TX_EMPTY : HISI_I2C_INT_ALL);
	} else {
		hisi_i2c_enable_int(ctlr, HISI_I2C_INT_ALL);
	}

	return 0;
}

static void hisi_i2c_reset_xfer(struct hisi_i2c_controller *ctlr)
{
	ctlr->msg_num = 0;
	ctlr->xfer_err = 0;
	ctlr->msg_tx_idx = 0;
	ctlr->msg_rx_idx = 0;
	ctlr->buf_tx_idx = 0;
	ctlr->buf_rx_idx = 0;
}

/*
 * Initialize the transfer information and start the I2C bus transfer.
 * We only configure the transfer and do some pre/post works here, and
 * wait for the transfer done. The major transfer process is performed
 * in the IRQ handler.
 */
static int hisi_i2c_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs,
			 int num)
{
	struct hisi_i2c_controller *ctlr = i2c_get_adapdata(adap);
	DECLARE_COMPLETION_ONSTACK(done);
	int ret = num;

	hisi_i2c_reset_xfer(ctlr);
	ctlr->completion = &done;
	ctlr->msg_num = num;
	ctlr->msgs = msgs;

	hisi_i2c_start_xfer(ctlr);

	/* I2C7's stock SPI does not deliver in the TGR handoff. Its status
	 * FIFO works; process the same status transitions with a bounded
	 * poll instead of registering a nonfunctional hardware IRQ.
	 */
	if (ctlr->polling) {
		unsigned long deadline = jiffies + adap->timeout;
		do {
			if (readl(ctlr->iobase + HISI_I2C_INT_MSTAT) & HISI_I2C_INT_ALL)
				hisi_i2c_irq(ctlr->irq, ctlr);
			if (completion_done(&done))
				break;
			usleep_range(100, 200);
		} while (time_before(jiffies, deadline));
	}

	if (ctlr->polling ? !try_wait_for_completion(ctlr->completion) :
	    !wait_for_completion_timeout(ctlr->completion, adap->timeout)) {
		if (ctlr->polling)
			dev_err(ctlr->dev, "poll timeout fifo=%x status=%x state=%x err=%x\n",
				readl(ctlr->iobase + HISI_I2C_FIFO_STATE),
				readl(ctlr->iobase + HISI_I2C_INT_MSTAT),
				readl(ctlr->iobase + HISI_I2C_TRANS_STATE),
				readl(ctlr->iobase + HISI_I2C_TRANS_ERR));
		hisi_i2c_disable_int(ctlr, HISI_I2C_INT_ALL);
		if (!ctlr->polling)
			synchronize_irq(ctlr->irq);
		i2c_recover_bus(&ctlr->adapter);
		dev_err(ctlr->dev, "bus transfer timeout\n");
		ret = -EIO;
	}

	if (ctlr->xfer_err) {
		hisi_i2c_handle_errors(ctlr);
		ret = -EIO;
	}

	/* Stop a late IRQ before discarding the on-stack completion/messages.
	 * On timeout, abandon queued commands before the next transfer.
	 */
	hisi_i2c_disable_int(ctlr, HISI_I2C_INT_ALL);
	if (!ctlr->polling)
		synchronize_irq(ctlr->irq);
	if (ret < 0) {
		u32 fifo = readl(ctlr->iobase + HISI_I2C_FIFO_CTRL);
		writel(fifo | 3, ctlr->iobase + HISI_I2C_FIFO_CTRL);
		writel(fifo & ~3U, ctlr->iobase + HISI_I2C_FIFO_CTRL);
		hisi_i2c_clear_int(ctlr, HISI_I2C_INT_ALL);
	}
	hisi_i2c_reset_xfer(ctlr);
	ctlr->completion = NULL;

	return ret;
}

static u32 hisi_i2c_functionality(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_10BIT_ADDR | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm hisi_i2c_algo = {
	.xfer = hisi_i2c_xfer,
	.functionality = hisi_i2c_functionality,
};

static int hisi_i2c_read_rx_fifo(struct hisi_i2c_controller *ctlr)
{
	struct i2c_msg *cur_msg;
	u32 fifo_state;

	while (ctlr->msg_rx_idx < ctlr->msg_num) {
		cur_msg = ctlr->msgs + ctlr->msg_rx_idx;

		if (!(cur_msg->flags & I2C_M_RD)) {
			ctlr->msg_rx_idx++;
			continue;
		}

		fifo_state = readl(ctlr->iobase + HISI_I2C_FIFO_STATE);
		while (!(fifo_state & HISI_I2C_FIFO_STATE_RX_EMPTY) &&
		       ctlr->buf_rx_idx < cur_msg->len) {
			cur_msg->buf[ctlr->buf_rx_idx++] = readl(ctlr->iobase + HISI_I2C_RXDATA);
			fifo_state = readl(ctlr->iobase + HISI_I2C_FIFO_STATE);
		}

		if (ctlr->buf_rx_idx == cur_msg->len) {
			ctlr->buf_rx_idx = 0;
			ctlr->msg_rx_idx++;
		}

		if (fifo_state & HISI_I2C_FIFO_STATE_RX_EMPTY)
			break;
	}

	return 0;
}

static void hisi_i2c_xfer_msg(struct hisi_i2c_controller *ctlr)
{
	int max_write = HISI_I2C_TX_FIFO_DEPTH - HISI_I2C_TX_F_AE_THRESH;
	bool need_restart = false, last_msg;
	struct i2c_msg *cur_msg;
	u32 cmd, fifo_state;

	while (ctlr->msg_tx_idx < ctlr->msg_num) {
		cur_msg = ctlr->msgs + ctlr->msg_tx_idx;
		last_msg = (ctlr->msg_tx_idx == ctlr->msg_num - 1);

		/* Signal the SR bit when we start transferring a new message */
		if (ctlr->msg_tx_idx && !ctlr->buf_tx_idx)
			need_restart = true;

		fifo_state = readl(ctlr->iobase + HISI_I2C_FIFO_STATE);
		while (!(fifo_state & HISI_I2C_FIFO_STATE_TX_FULL) &&
		       ctlr->buf_tx_idx < cur_msg->len && max_write) {
			cmd = 0;

			if (need_restart) {
				cmd |= HISI_I2C_CMD_TXDATA_SR_EN;
				need_restart = false;
			}

			/* Signal the STOP bit at the last frame of the last message */
			if (ctlr->buf_tx_idx == cur_msg->len - 1 && last_msg)
				cmd |= HISI_I2C_CMD_TXDATA_P_EN;

			if (cur_msg->flags & I2C_M_RD)
				cmd |= HISI_I2C_CMD_TXDATA_RW;
			else
				cmd |= FIELD_PREP(HISI_I2C_CMD_TXDATA_DATA,
						  cur_msg->buf[ctlr->buf_tx_idx]);

			writel(cmd, ctlr->iobase + HISI_I2C_CMD_TXDATA);
			ctlr->buf_tx_idx++;
			max_write--;

			fifo_state = readl(ctlr->iobase + HISI_I2C_FIFO_STATE);
		}

		/* Update the transfer index after per message transfer is done. */
		if (ctlr->buf_tx_idx == cur_msg->len) {
			ctlr->buf_tx_idx = 0;
			ctlr->msg_tx_idx++;
		}

		if ((fifo_state & HISI_I2C_FIFO_STATE_TX_FULL) ||
		    max_write == 0)
			break;
	}

	/*
	 * Disable the TX_EMPTY interrupt after finishing all the messages to
	 * avoid overwhelming the CPU.
	 */
	if (ctlr->msg_tx_idx == ctlr->msg_num)
		hisi_i2c_disable_int(ctlr, HISI_I2C_INT_TX_EMPTY);

	hisi_i2c_clear_tx_int(ctlr, HISI_I2C_TX_AEMPTY_INT);
}

static irqreturn_t hisi_i2c_irq(int irq, void *context)
{
	struct hisi_i2c_controller *ctlr = context;
	u32 int_stat;

	/*
	 * Don't handle the interrupt if cltr->completion is NULL. We may
	 * reach here because the interrupt is spurious or the transfer is
	 * started by another port (e.g. firmware) rather than us.
	 */
	if (!ctlr->completion)
		return IRQ_NONE;

	int_stat = readl(ctlr->iobase + HISI_I2C_INT_MSTAT);
	hisi_i2c_clear_int(ctlr, int_stat);
	if (!(int_stat & HISI_I2C_INT_ALL))
		return IRQ_NONE;

	if (int_stat & HISI_I2C_INT_TX_EMPTY)
		hisi_i2c_xfer_msg(ctlr);

	if (int_stat & HISI_I2C_INT_ERR) {
		ctlr->xfer_err = int_stat;
		goto out;
	}

	/* Drain the rx fifo before finish the transfer */
	if (int_stat & (HISI_I2C_INT_TRANS_CPLT | HISI_I2C_INT_RX_FULL))
		hisi_i2c_read_rx_fifo(ctlr);

out:
	/*
	 * Only use TRANS_CPLT to indicate the completion. On error cases we'll
	 * get two interrupts, INT_ERR first then TRANS_CPLT.
	 */
	if (int_stat & HISI_I2C_INT_TRANS_CPLT) {
		hisi_i2c_disable_int(ctlr, HISI_I2C_INT_ALL);
		hisi_i2c_clear_int(ctlr, HISI_I2C_INT_ALL);
		hisi_i2c_clear_tx_int(ctlr, HISI_I2C_TX_AEMPTY_INT);
		complete(ctlr->completion);
	}

	return IRQ_HANDLED;
}

/*
 * Helper function for calculating and configuring the HIGH and LOW
 * periods of SCL clock. The caller will pass the ratio of the
 * counts (divide / divisor) according to the target speed mode,
 * and the target registers.
 */
static void hisi_i2c_set_scl(struct hisi_i2c_controller *ctlr,
			     u32 divide, u32 divisor,
			     u32 reg_hcnt, u32 reg_lcnt)
{
	u32 total_cnt, t_scl_hcnt, t_scl_lcnt, scl_fall_cnt, scl_rise_cnt;
	u32 scl_hcnt, scl_lcnt;

	/* Total SCL clock cycles per speed period */
	total_cnt = DIV_ROUND_UP_ULL(ctlr->clk_rate_khz * HZ_PER_KHZ, ctlr->t.bus_freq_hz);
	/* Total HIGH level SCL clock cycles including edges */
	t_scl_hcnt = DIV_ROUND_UP_ULL(total_cnt * divide, divisor);
	/* Total LOW level SCL clock cycles including edges */
	t_scl_lcnt = total_cnt - t_scl_hcnt;
	/* Fall edge SCL clock cycles */
	scl_fall_cnt = NSEC_TO_CYCLES(ctlr->t.scl_fall_ns, ctlr->clk_rate_khz);
	/* Rise edge SCL clock cycles */
	scl_rise_cnt = NSEC_TO_CYCLES(ctlr->t.scl_rise_ns, ctlr->clk_rate_khz);

	/* Calculated HIGH and LOW periods of SCL clock */
	scl_hcnt = t_scl_hcnt - ctlr->spk_len - 7 - scl_fall_cnt;
	scl_lcnt = t_scl_lcnt - 1 - scl_rise_cnt;

	writel(scl_hcnt, ctlr->iobase + reg_hcnt);
	writel(scl_lcnt, ctlr->iobase + reg_lcnt);
}

static void hisi_i2c_configure_bus(struct hisi_i2c_controller *ctlr)
{
	u32 reg, sda_hold_cnt, speed_mode;

	i2c_parse_fw_timings(ctlr->dev, &ctlr->t, true);
	ctlr->spk_len = NSEC_TO_CYCLES(ctlr->t.digital_filter_width_ns, ctlr->clk_rate_khz);

	switch (ctlr->t.bus_freq_hz) {
	case I2C_MAX_FAST_MODE_FREQ:
		speed_mode = HISI_I2C_FAST_SPEED_MODE;
		hisi_i2c_set_scl(ctlr, 26, 76, HISI_I2C_FS_SCL_HCNT, HISI_I2C_FS_SCL_LCNT);
		break;
	case I2C_MAX_HIGH_SPEED_MODE_FREQ:
		speed_mode = HISI_I2C_HIGH_SPEED_MODE;
		hisi_i2c_set_scl(ctlr, 6, 22, HISI_I2C_HS_SCL_HCNT, HISI_I2C_HS_SCL_LCNT);
		break;
	case I2C_MAX_STANDARD_MODE_FREQ:
	default:
		speed_mode = HISI_I2C_STD_SPEED_MODE;

		/* For default condition force the bus speed to standard mode. */
		ctlr->t.bus_freq_hz = I2C_MAX_STANDARD_MODE_FREQ;
		hisi_i2c_set_scl(ctlr, 40, 87, HISI_I2C_SS_SCL_HCNT, HISI_I2C_SS_SCL_LCNT);
		break;
	}

	reg = readl(ctlr->iobase + HISI_I2C_FRAME_CTRL);
	reg &= ~HISI_I2C_FRAME_CTRL_SPEED_MODE;
	reg |= FIELD_PREP(HISI_I2C_FRAME_CTRL_SPEED_MODE, speed_mode);
	if (charlotte_i2c_v400(ctlr->dev))
		reg = FIELD_PREP(HISI_I2C_FRAME_CTRL_SPEED_MODE, speed_mode) |
			HISI_I2C_FRAME_CTRL_RESTART_EN;
	writel(reg, ctlr->iobase + HISI_I2C_FRAME_CTRL);

	sda_hold_cnt = NSEC_TO_CYCLES(ctlr->t.sda_hold_ns, ctlr->clk_rate_khz);

	reg = FIELD_PREP(HISI_I2C_SDA_HOLD_TX, sda_hold_cnt);
	if (charlotte_i2c_v400(ctlr->dev))
		reg |= FIELD_PREP(HISI_I2C_SDA_HOLD_RX, sda_hold_cnt);
	writel(reg, ctlr->iobase + HISI_I2C_SDA_HOLD);

	writel(ctlr->spk_len, ctlr->iobase + HISI_I2C_FS_SPK_LEN);

	reg = FIELD_PREP(HISI_I2C_FIFO_RX_AF_THRESH, HISI_I2C_RX_F_AF_THRESH);
	reg |= FIELD_PREP(HISI_I2C_FIFO_TX_AE_THRESH, HISI_I2C_TX_F_AE_THRESH);
	writel(reg, ctlr->iobase + HISI_I2C_FIFO_CTRL);
	if (ctlr->polling) {
		/* The TGR firmware profile used by the successful C diagnostic.
		 * Retain its fast-mode timing and one-byte RX threshold, rather
		 * than using the Kunpeng defaults on this handoff-only instance.
		 */
		writel(9, ctlr->iobase + HISI_I2C_FRAME_CTRL);
		writel(0x50, ctlr->iobase + HISI_I2C_FS_SCL_HCNT);
		writel(0xc0, ctlr->iobase + HISI_I2C_FS_SCL_LCNT);
		writel(0x3e003e, ctlr->iobase + HISI_I2C_SDA_HOLD);
		writel(0x3f04, ctlr->iobase + HISI_I2C_FIFO_CTRL);
	}
}

/* Charlotte I2C6 uses the same FIFO/command layout as Ascend910.
 * The full CRG/pinctrl providers are not yet ported. Touch only this gate,
 * reset and GPIO76/77 mux/pads, as in the stock board's I2C6 pin state.
 */
static int charlotte_i2c_prepare(struct platform_device *pdev,
			       struct hisi_i2c_controller *ctlr)
{
	void __iomem *crg, *mux, *pad, *func;
	struct resource *res;
	u32 status;
	int i, ret;

	/* Shared CRG: USB also maps this bank. Do not reserve the whole bank
	 * for I2C. Gate/reset writes are single-bit set/clear operations.
	 */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!res)
		return -EINVAL;
	crg = devm_ioremap(&pdev->dev, res->start, resource_size(res));
	if (!crg)
		return -ENOMEM;
	mux = devm_platform_ioremap_resource(pdev, 2);
	if (IS_ERR(mux))
		return PTR_ERR(mux);
	pad = devm_platform_ioremap_resource(pdev, 3);
	if (IS_ERR(pad))
		return PTR_ERR(pad);
	func = devm_platform_ioremap_resource(pdev, 4);
	if (IS_ERR(func))
		return PTR_ERR(func);
	ctlr->mux = mux;
	ctlr->func = func;
	writel(BIT(30), crg + 0x10);
	ret = readl_poll_timeout(crg + 0x18, status, status & BIT(30), 1, 1000);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "I2C6 gate not enabled\n");
	writel(BIT(19), crg + 0x60);
	ret = readl_poll_timeout(crg + 0x68, status, status & BIT(19), 1, 1000);
	if (ret)
		return ret;
	writel(BIT(19), crg + 0x64);
	ret = readl_poll_timeout(crg + 0x68, status, !(status & BIT(19)), 1, 1000);
	if (ret)
		return ret;
	for (i = 0; i < 2; i++) {
		status = readl(mux + i * 4);
		writel((status & ~7) | 1, mux + i * 4);
		status = readl(func + i * 4);
		writel((status & ~7) | 1, func + i * 4);
		status = readl(pad + i * 4);
		writel((status & ~0x73) | 1, pad + i * 4);
	}
	return 0;
}

/* TGR backlight I2C7: stock DT + clk-char-cs.c, verified with 32-bit
 * FIFO accesses. Keep this distinct from I2C6's gate/reset and pins.
 */
static int charlotte_i2c7_prepare(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	void __iomem *crg, *mux, *pad;
	u32 status;
	int i, ret;
	if (!res)
		return -EINVAL;
	crg = devm_ioremap(dev, res->start, resource_size(res));
	if (!crg)
		return -ENOMEM;
	mux = devm_platform_ioremap_resource(pdev, 2);
	if (IS_ERR(mux))
		return PTR_ERR(mux);
	pad = devm_platform_ioremap_resource(pdev, 3);
	if (IS_ERR(pad))
		return PTR_ERR(pad);
	/* Accessing the controller with its gate off stalls the interconnect. */
	writel(BIT(31), crg + 0x10);
	ret = readl_poll_timeout(crg + 0x18, status, status & BIT(31), 1, 1000);
	if (ret)
		return dev_err_probe(dev, ret, "I2C7 gate not enabled\n");
	/* The handoff leaves the pad functions idle. Route both pins before
	 * releasing reset, as in the successful C diagnostic. Resetting first
	 * left commands queued forever even after the pads were switched.
	 */
	for (i = 0; i < 2; i++) {
		status = readl(mux + i * 4);
		writel((status & ~7U) | 4, mux + i * 4);
		status = readl(pad + i * 4);
		writel((status & ~7U) | 4, pad + i * 4);
	}
	writel(BIT(14), crg + 0x60);
	ret = readl_poll_timeout(crg + 0x68, status, status & BIT(14), 1, 1000);
	if (ret)
		return ret;
	writel(BIT(14), crg + 0x64);
	ret = readl_poll_timeout(crg + 0x68, status, !(status & BIT(14)), 1, 1000);
	if (ret)
		return ret;
	return 0;
}

static void charlotte_i2c_recovery_prepare(struct i2c_adapter *adap)
{
	struct hisi_i2c_controller *ctlr = i2c_get_adapdata(adap);
	int i;

	/* Drop abandoned commands before borrowing the pins for recovery. */
	writel(readl(ctlr->iobase + HISI_I2C_FIFO_CTRL) | 3,
	       ctlr->iobase + HISI_I2C_FIFO_CTRL);
	writel(readl(ctlr->iobase + HISI_I2C_FIFO_CTRL) & ~3,
	       ctlr->iobase + HISI_I2C_FIFO_CTRL);
	for (i = 0; i < 2; i++) {
		ctlr->saved_mux[i] = readl(ctlr->mux + i * 4);
		ctlr->saved_func[i] = readl(ctlr->func + i * 4);
		writel(ctlr->saved_mux[i] & ~7, ctlr->mux + i * 4);
		writel(ctlr->saved_func[i] & ~7, ctlr->func + i * 4);
	}
}

static void charlotte_i2c_recovery_unprepare(struct i2c_adapter *adap)
{
	struct hisi_i2c_controller *ctlr = i2c_get_adapdata(adap);
	int i;

	gpiod_set_value_cansleep(ctlr->recovery.sda_gpiod, 1);
	gpiod_set_value_cansleep(ctlr->recovery.scl_gpiod, 1);
	for (i = 0; i < 2; i++) {
		writel(ctlr->saved_mux[i], ctlr->mux + i * 4);
		writel(ctlr->saved_func[i], ctlr->func + i * 4);
	}
}

static int charlotte_i2c_get_scl(struct i2c_adapter *adap)
{
	return gpiod_get_value_cansleep(adap->bus_recovery_info->scl_gpiod);
}

static void charlotte_i2c_set_scl(struct i2c_adapter *adap, int value)
{
	gpiod_set_value_cansleep(adap->bus_recovery_info->scl_gpiod, value);
}

static int charlotte_i2c_get_sda(struct i2c_adapter *adap)
{
	return gpiod_get_value_cansleep(adap->bus_recovery_info->sda_gpiod);
}

static void charlotte_i2c_set_sda(struct i2c_adapter *adap, int value)
{
	gpiod_set_value_cansleep(adap->bus_recovery_info->sda_gpiod, value);
}

static int charlotte_i2c_init_recovery(struct hisi_i2c_controller *ctlr)
{
	struct i2c_bus_recovery_info *r = &ctlr->recovery;

	r->scl_gpiod = devm_gpiod_get(ctlr->dev, "scl", GPIOD_OUT_HIGH_OPEN_DRAIN);
	if (IS_ERR(r->scl_gpiod))
		return dev_err_probe(ctlr->dev, PTR_ERR(r->scl_gpiod), "SCL recovery GPIO\n");
	r->sda_gpiod = devm_gpiod_get(ctlr->dev, "sda", GPIOD_OUT_HIGH_OPEN_DRAIN);
	if (IS_ERR(r->sda_gpiod))
		return dev_err_probe(ctlr->dev, PTR_ERR(r->sda_gpiod), "SDA recovery GPIO\n");
	r->recover_bus = i2c_generic_scl_recovery;
	r->prepare_recovery = charlotte_i2c_recovery_prepare;
	r->unprepare_recovery = charlotte_i2c_recovery_unprepare;
	r->get_scl = charlotte_i2c_get_scl;
	r->set_scl = charlotte_i2c_set_scl;
	r->get_sda = charlotte_i2c_get_sda;
	r->set_sda = charlotte_i2c_set_sda;
	ctlr->adapter.bus_recovery_info = r;
	return i2c_recover_bus(&ctlr->adapter);
}

static int hisi_i2c_probe(struct platform_device *pdev)
{
	struct hisi_i2c_controller *ctlr;
	struct device *dev = &pdev->dev;
	struct i2c_adapter *adapter;
	u64 clk_rate_hz;
	u32 hw_version;
	int ret;

	ctlr = devm_kzalloc(dev, sizeof(*ctlr), GFP_KERNEL);
	if (!ctlr)
		return -ENOMEM;
	ctlr->dev = dev;
	ctlr->polling = of_device_is_compatible(dev->of_node, "hisilicon,charlotte-i2c7-v400");
	if (of_device_is_compatible(dev->of_node, "hisilicon,charlotte-i2c-v400")) {
		ret = charlotte_i2c_prepare(pdev, ctlr);
		if (ret)
			return ret;
	} else if (of_device_is_compatible(dev->of_node, "hisilicon,charlotte-i2c7-v400")) {
		ret = charlotte_i2c7_prepare(pdev);
		if (ret)
			return ret;
	}

	ctlr->iobase = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(ctlr->iobase))
		return PTR_ERR(ctlr->iobase);

	ctlr->irq = platform_get_irq(pdev, 0);
	if (ctlr->irq < 0)
		return ctlr->irq;

	ctlr->dev = dev;

	hisi_i2c_disable_int(ctlr, HISI_I2C_INT_ALL);

	if (!ctlr->polling) {
		ret = devm_request_irq(dev, ctlr->irq, hisi_i2c_irq, 0, "hisi-i2c", ctlr);
		if (ret)
			return ret;
	}

	ctlr->clk = devm_clk_get_optional_enabled(&pdev->dev, NULL);
	if (IS_ERR_OR_NULL(ctlr->clk)) {
		ret = device_property_read_u64(dev, "clk_rate", &clk_rate_hz);
		if (ret)
			return dev_err_probe(dev, ret, "failed to get clock frequency\n");
	} else {
		clk_rate_hz = clk_get_rate(ctlr->clk);
	}

	ctlr->clk_rate_khz = DIV_ROUND_UP_ULL(clk_rate_hz, HZ_PER_KHZ);

	hisi_i2c_configure_bus(ctlr);

	adapter = &ctlr->adapter;
	snprintf(adapter->name, sizeof(adapter->name),
		 "HiSilicon I2C Controller %s", dev_name(dev));
	adapter->owner = THIS_MODULE;
	adapter->algo = &hisi_i2c_algo;
	adapter->dev.parent = dev;
	adapter->dev.of_node = dev->of_node;
	i2c_set_adapdata(adapter, ctlr);
	if (of_device_is_compatible(dev->of_node, "hisilicon,charlotte-i2c-v400")) {
		ret = charlotte_i2c_init_recovery(ctlr);
		if (ret)
			return dev_err_probe(dev, ret, "I2C6 initial bus recovery failed\n");
	}

	ret = devm_i2c_add_adapter(dev, adapter);
	if (ret)
		return ret;

	hw_version = readl(ctlr->iobase + HISI_I2C_VERSION);
	dev_info(ctlr->dev, "speed mode is %s. hw version 0x%x\n",
		 i2c_freq_mode_string(ctlr->t.bus_freq_hz), hw_version);
	if (ctlr->polling)
		dev_info(ctlr->dev, "bounded polled transactions for TGR backlight\n");

	return 0;
}

static const struct acpi_device_id hisi_i2c_acpi_ids[] = {
	{ "HISI03D1", 0 },
	{ }
};
MODULE_DEVICE_TABLE(acpi, hisi_i2c_acpi_ids);

static const struct of_device_id hisi_i2c_dts_ids[] = {
	{ .compatible = "hisilicon,ascend910-i2c", },
	{ .compatible = "hisilicon,charlotte-i2c-v400", },
	{ .compatible = "hisilicon,charlotte-i2c7-v400", },
	{ }
};
MODULE_DEVICE_TABLE(of, hisi_i2c_dts_ids);

static struct platform_driver hisi_i2c_driver = {
	.probe		= hisi_i2c_probe,
	.driver		= {
		.name	= "hisi-i2c",
		.acpi_match_table = hisi_i2c_acpi_ids,
		.of_match_table = hisi_i2c_dts_ids,
	},
};
module_platform_driver(hisi_i2c_driver);

MODULE_AUTHOR("Yicong Yang <yangyicong@hisilicon.com>");
MODULE_DESCRIPTION("HiSilicon I2C Controller Driver");
MODULE_LICENSE("GPL");
