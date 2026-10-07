// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for Hisilicon UART V500 serial ports (Kirin 9000s / charlotte)
 *
 * Ported from the HarmonyOS 4.2 vendor kernel (5.10.43) driver:
 *   platform_source/basicplatform/drivers/tty/serial/uart_v500.c
 *   (Copyright (c) Huawei Technologies Co., Ltd. 2020)
 *
 * The V500 UART has 16550-like features but is not register compatible.
 * This port is PIO-only (no DMA) and makes the input clock optional so the
 * console works before a clock driver exists (clock-rate DT property or
 * 19.2 MHz default).
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/console.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/serial.h>
#include <linux/serial_core.h>
#include <linux/slab.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>

#define DRV_NAME			"uart-v500"
#define SERIAL_V500_NR			8

/* Registers */
#define UART_V500_BAUD_CFG		0x000
#define UART_V500_FRAME_CFG		0x004
#define UART_V500_FIFO_CFG		0x008
#define UART_V500_TX_FIFO		0x00c
#define UART_V500_RX_FIFO		0x010
#define UART_V500_STAT			0x014
#define UART_V500_INT_CLR		0x018
#define UART_V500_INT_MSK		0x01c
#define UART_V500_INT_RAW		0x020
#define UART_V500_INT_STAT		0x024
#define UART_V500_CONFIG		0x028

#define UART_V500_BAUD_INT_MASK		0xffff
#define UART_V500_BAUD_FRAC_MASK		0x3f

/* CONFIG register: each field has a mirrored enable/disable half-word */
#define UART_V500_CFG_FIFO_EN		0x04000400
#define UART_V500_CFG_CTS_EN		0x00080008
#define UART_V500_CFG_CTS_DIS		0x00080000
#define UART_V500_CFG_RTS_EN		0x00100010
#define UART_V500_CFG_RTS_DIS		0x00100000
#define UART_V500_CFG_LOOP_EN		0x02000200
#define UART_V500_CFG_LOOP_DIS		0x02000000
#define UART_V500_CFG_UART_EN		0x00010001
#define UART_V500_CFG_RX_EN		0x00040004
#define UART_V500_CFG_TX_EN		0x00020002
#define UART_V500_CFG_BRK_EN		0x00800080
#define UART_V500_CFG_BRK_DIS		0x00800000
#define UART_V500_CFG_DIS_ALL		0xffff0000

#define UART_V500_STAT_TX_BUSY		0x001
#define UART_V500_STAT_RX_BUSY		0x002
#define UART_V500_STAT_TXFE		0x004
#define UART_V500_STAT_TXFF		0x008
#define UART_V500_STAT_RXFE		0x010
#define UART_V500_STAT_RXFF		0x020
#define UART_V500_STAT_CTS		0x040

#define UART_V500_FRAME_LEN_5		0x0
#define UART_V500_FRAME_LEN_6		0x1
#define UART_V500_FRAME_LEN_7		0x2
#define UART_V500_FRAME_LEN_8		0x3
#define UART_V500_FRAME_DATA_BIT		0x3
#define UART_V500_FRAME_STOP		0x20
#define UART_V500_FRAME_PARITY		0x1c
#define UART_V500_FRAME_PES		0x10
#define UART_V500_FRAME_POS		0x14

#define UART_V500_FIFO_TX_1_2		(2 << 0)
#define UART_V500_FIFO_RX_1_4		(1 << 3)
#define UART_V500_FIFO_RTS_7_8		(4 << 6)
#define UART_V500_FIFO_CFG_DEF		(UART_V500_FIFO_RX_1_4 | \
					 UART_V500_FIFO_TX_1_2 | \
					 UART_V500_FIFO_RTS_7_8)

/* Data error bits in the RX FIFO word */
#define UART_V500_DR_OE			BIT(11)
#define UART_V500_DR_BE			BIT(10)
#define UART_V500_DR_PE			BIT(9)
#define UART_V500_DR_FE			BIT(8)
#define UART_V500_DR_ERROR		(UART_V500_DR_OE | UART_V500_DR_BE | \
					 UART_V500_DR_PE | UART_V500_DR_FE)
#define UART_V500_DR_DATA		0xff

/* Interrupt bits: status / mask / clear share the same layout */
#define UART_V500_INT_RXS		BIT(0)
#define UART_V500_INT_TXS		BIT(1)
#define UART_V500_INT_RTS		BIT(2)
#define UART_V500_INT_FES		BIT(3)
#define UART_V500_INT_PES		BIT(4)
#define UART_V500_INT_BES		BIT(5)
#define UART_V500_INT_OES		BIT(6)
#define UART_V500_INT_CTSS		BIT(7)
#define UART_V500_INT_RIS		BIT(8)
#define UART_V500_INT_DCDS		BIT(9)
#define UART_V500_INT_DSRS		BIT(10)
#define UART_V500_INT_STAUS		(UART_V500_INT_OES | UART_V500_INT_BES | \
					 UART_V500_INT_PES | UART_V500_INT_FES | \
					 UART_V500_INT_RTS | UART_V500_INT_RXS)
#define UART_V500_INT_MASK		(UART_V500_INT_RXS | UART_V500_INT_TXS | \
					 UART_V500_INT_RTS | UART_V500_INT_FES | \
					 UART_V500_INT_PES | UART_V500_INT_BES | \
					 UART_V500_INT_OES)
#define UART_V500_MSK_INT_ALL		0x7ff
#define UART_V500_CLR_INT_ALL		0xffff

#define UART_V500_FIFO_SIZE_DEF		64
#define UART_V500_ISR_PASS_LIMIT		256
#define UART_V500_DEFAULT_CLK		19200000
#define UART_V500_TX_TIMEOUT_US		100000

struct uart_v500_port {
	struct uart_port		port;
	struct clk			*clk;
	u32				fifosize;
	u32				int_msk;
	u32				old_status;
};

static struct uart_driver uart_v500_driver;

static struct uart_v500_port *uart_v500_ports[SERIAL_V500_NR];

static void uart_v500_write(struct uart_v500_port *uap, u32 val, u32 reg)
{
	writel_relaxed(val, uap->port.membase + reg);
}

static u32 uart_v500_read(struct uart_v500_port *uap, u32 reg)
{
	return readl_relaxed(uap->port.membase + reg);
}

static void uart_v500_drain_tx(struct uart_v500_port *uap)
{
	unsigned int count = 0;

	while ((uart_v500_read(uap, UART_V500_STAT) &
		UART_V500_STAT_TX_BUSY) &&
		(count < UART_V500_TX_TIMEOUT_US)) {
		udelay(1);
		count++;
		barrier();
	}
}

/* ---------------------------------------------------------- IRQ / RX path */

static void uart_v500_rx_chars(struct uart_v500_port *uap)
{
	u32 status, ch, flag;
	unsigned int max_count = uap->fifosize * 4;

	while (max_count--) {
		status = uart_v500_read(uap, UART_V500_STAT);
		if (status & UART_V500_STAT_RXFE)
			break;

		ch = uart_v500_read(uap, UART_V500_RX_FIFO);
		flag = TTY_NORMAL;
		uap->port.icount.rx++;

		if (unlikely(ch & UART_V500_DR_ERROR)) {
			if (ch & UART_V500_DR_BE) {
				ch &= ~(UART_V500_DR_FE | UART_V500_DR_PE);
				uap->port.icount.brk++;
				if (uart_handle_break(&uap->port))
					continue;
			} else if (ch & UART_V500_DR_PE) {
				uap->port.icount.parity++;
			} else if (ch & UART_V500_DR_FE) {
				uap->port.icount.frame++;
			}
			if (ch & UART_V500_DR_OE)
				uap->port.icount.overrun++;

			ch &= uap->port.read_status_mask;

			if (ch & UART_V500_DR_BE)
				flag = TTY_BREAK;
			else if (ch & UART_V500_DR_PE)
				flag = TTY_PARITY;
			else if (ch & UART_V500_DR_FE)
				flag = TTY_FRAME;
		}

		if (uart_handle_sysrq_char(&uap->port, ch & UART_V500_DR_DATA))
			continue;

		uart_insert_char(&uap->port, ch, UART_V500_DR_OE,
				 ch & UART_V500_DR_DATA, flag);
	}

	spin_unlock(&uap->port.lock);
	tty_flip_buffer_push(&uap->port.state->port);
	spin_lock(&uap->port.lock);
}

static void uart_v500_stop_tx_port(struct uart_port *port);

static unsigned int uart_v500_tx_chars(struct uart_v500_port *uap)
{
	unsigned char ch;

	return uart_port_tx_limited(&uap->port, ch, uap->fifosize >> 1,
		!(uart_v500_read(uap, UART_V500_STAT) & UART_V500_STAT_TXFF),
		uart_v500_write(uap, ch, UART_V500_TX_FIFO),
		({}));
}

static void uart_v500_modem_status(struct uart_v500_port *uap)
{
	u32 status, delta;

	status = uart_v500_read(uap, UART_V500_STAT) & UART_V500_STAT_CTS;
	delta = status ^ uap->old_status;
	uap->old_status = status;

	if (!delta)
		return;

	if (delta & UART_V500_STAT_CTS)
		uart_handle_cts_change(&uap->port, !!(status &
						       UART_V500_STAT_CTS));

	wake_up_interruptible(&uap->port.state->port.delta_msr_wait);
}

static irqreturn_t uart_v500_int(int irq, void *dev_id)
{
	struct uart_v500_port *uap = dev_id;
	unsigned int status, int_clr;
	unsigned int pass_counter = UART_V500_ISR_PASS_LIMIT;

	spin_lock(&uap->port.lock);

	status = uart_v500_read(uap, UART_V500_INT_STAT);
	if (!status) {
		spin_unlock(&uap->port.lock);
		return IRQ_NONE;
	}

	do {
		int_clr = status & ~(UART_V500_INT_TXS | UART_V500_INT_RTS |
				     UART_V500_INT_RXS);
		uart_v500_write(uap, int_clr, UART_V500_INT_CLR);

		if (status & (UART_V500_INT_RTS | UART_V500_INT_RXS))
			uart_v500_rx_chars(uap);

		if (status & (UART_V500_INT_CTSS | UART_V500_INT_RIS |
			      UART_V500_INT_DCDS | UART_V500_INT_DSRS))
			uart_v500_modem_status(uap);

		if (status & UART_V500_INT_TXS)
			uart_v500_tx_chars(uap);

		if (pass_counter-- == 0)
			break;

		status = uart_v500_read(uap, UART_V500_INT_STAT);
	} while (status);

	spin_unlock(&uap->port.lock);

	return IRQ_HANDLED;
}

/* ------------------------------------------------------------ port ops */

static unsigned int uart_v500_tx_empty(struct uart_port *port)
{
	struct uart_v500_port *uap =
		container_of(port, struct uart_v500_port, port);

	return (uart_v500_read(uap, UART_V500_STAT) & UART_V500_STAT_TXFE) ?
		TIOCSER_TEMT : 0;
}

static unsigned int uart_v500_get_mctrl(struct uart_port *port)
{
	struct uart_v500_port *uap =
		container_of(port, struct uart_v500_port, port);
	unsigned int result = 0;

	if (uart_v500_read(uap, UART_V500_STAT) & UART_V500_STAT_CTS)
		result |= TIOCM_CTS;

	return result;
}

static void uart_v500_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
	struct uart_v500_port *uap =
		container_of(port, struct uart_v500_port, port);

	if (mctrl & TIOCM_LOOP)
		uart_v500_write(uap, UART_V500_CFG_LOOP_EN, UART_V500_CONFIG);
	else
		uart_v500_write(uap, UART_V500_CFG_LOOP_DIS, UART_V500_CONFIG);
}

static void uart_v500_stop_tx_port(struct uart_port *port)
{
	struct uart_v500_port *uap =
		container_of(port, struct uart_v500_port, port);

	uap->int_msk |= UART_V500_INT_TXS;
	uart_v500_write(uap, uap->int_msk, UART_V500_INT_MSK);
}

static void uart_v500_start_tx(struct uart_port *port)
{
	struct uart_v500_port *uap =
		container_of(port, struct uart_v500_port, port);

	if (uart_v500_tx_chars(uap)) {
		uap->int_msk &= ~UART_V500_INT_TXS;
		uart_v500_write(uap, uap->int_msk, UART_V500_INT_MSK);
	}
}

static void uart_v500_stop_rx(struct uart_port *port)
{
	struct uart_v500_port *uap =
		container_of(port, struct uart_v500_port, port);

	uap->int_msk |= UART_V500_INT_MASK;
	uart_v500_write(uap, uap->int_msk, UART_V500_INT_MSK);
}

static void uart_v500_enable_ms(struct uart_port *port)
{
	struct uart_v500_port *uap =
		container_of(port, struct uart_v500_port, port);

	uap->int_msk &= ~(UART_V500_INT_CTSS | UART_V500_INT_RIS |
			  UART_V500_INT_DCDS | UART_V500_INT_DSRS);
	uart_v500_write(uap, uap->int_msk, UART_V500_INT_MSK);
}

static void uart_v500_break_ctl(struct uart_port *port, int break_state)
{
	struct uart_v500_port *uap =
		container_of(port, struct uart_v500_port, port);
	unsigned long flags;
	u32 cfg;

	spin_lock_irqsave(&port->lock, flags);
	cfg = (break_state == -1) ? UART_V500_CFG_BRK_EN :
		UART_V500_CFG_BRK_DIS;
	uart_v500_write(uap, cfg, UART_V500_CONFIG);
	spin_unlock_irqrestore(&port->lock, flags);
}

static void uart_v500_config_by_termios(struct uart_v500_port *uap,
					 struct ktermios *termios)
{
	u32 frame_cfg;

	switch (termios->c_cflag & CSIZE) {
	case CS5:
		frame_cfg = UART_V500_FRAME_LEN_5;
		break;
	case CS6:
		frame_cfg = UART_V500_FRAME_LEN_6;
		break;
	case CS7:
		frame_cfg = UART_V500_FRAME_LEN_7;
		break;
	default:
		frame_cfg = UART_V500_FRAME_LEN_8;
		break;
	}
	if (termios->c_cflag & CSTOPB)
		frame_cfg |= UART_V500_FRAME_STOP;
	if (termios->c_cflag & PARENB) {
		if (!(termios->c_cflag & PARODD))
			frame_cfg |= UART_V500_FRAME_POS;
	}
	uart_v500_write(uap, frame_cfg, UART_V500_FRAME_CFG);
}

static void uart_v500_set_termios(struct uart_port *port,
				   struct ktermios *termios,
				   const struct ktermios *old)
{
	struct uart_v500_port *uap =
		container_of(port, struct uart_v500_port, port);
	unsigned int baud, quot, quotn;
	u32 cfg;

	baud = uart_get_baud_rate(port, termios, old, 0, port->uartclk / 16);
	if (!baud) {
		dev_err(port->dev, "%s: invalid baud\n", __func__);
		return;
	}

	/* BAUD register is a 22.14 fixed-point of uartclk/(4*baud) */
	if (baud > port->uartclk / 16)
		quot = DIV_ROUND_CLOSEST(port->uartclk * 8, baud);
	else
		quot = DIV_ROUND_CLOSEST(port->uartclk * 4, baud);

	uart_v500_config_by_termios(uap, termios);

	spin_lock_irq(&port->lock);

	/* CONFIG needs the mirrored-disable write before re-enabling */
	cfg = uart_v500_read(uap, UART_V500_CONFIG);
	cfg |= (cfg << 16);
	uart_v500_write(uap, UART_V500_CFG_DIS_ALL, UART_V500_CONFIG);
	cfg |= UART_V500_CFG_RX_EN | UART_V500_CFG_TX_EN |
	       UART_V500_CFG_UART_EN | UART_V500_CFG_FIFO_EN;
	uart_v500_write(uap, cfg, UART_V500_CONFIG);
	if (termios->c_cflag & CRTSCTS) {
		uart_v500_write(uap, UART_V500_CFG_RTS_EN |
				UART_V500_CFG_CTS_EN, UART_V500_CONFIG);
	} else {
		uart_v500_write(uap, UART_V500_CFG_RTS_DIS |
				UART_V500_CFG_CTS_DIS, UART_V500_CONFIG);
	}

	port->read_status_mask = UART_V500_DR_OE | UART_V500_DR_DATA;
	if (termios->c_iflag & INPCK)
		port->read_status_mask |= UART_V500_DR_FE | UART_V500_DR_PE;
	if (termios->c_iflag & (IGNBRK | BRKINT | PARMRK))
		port->read_status_mask |= UART_V500_DR_BE;

	port->ignore_status_mask = 0;
	if (termios->c_iflag & IGNPAR)
		port->ignore_status_mask |= UART_V500_DR_FE | UART_V500_DR_PE;
	if (termios->c_iflag & IGNBRK) {
		port->ignore_status_mask |= UART_V500_DR_BE;
		if (termios->c_iflag & IGNPAR)
			port->ignore_status_mask |= UART_V500_DR_OE;
	}

	uart_update_timeout(port, termios->c_cflag, baud);

	uart_v500_drain_tx(uap);

	/* BAUD register: [21:16] fractional, [15:0] integer */
	quotn = quot;
	quot = (quot & UART_V500_BAUD_FRAC_MASK) << 16;
	quotn = (quotn >> 6) & UART_V500_BAUD_INT_MASK;
	uart_v500_write(uap, quot | quotn, UART_V500_BAUD_CFG);

	spin_unlock_irq(&port->lock);
}

static int uart_v500_startup(struct uart_port *port)
{
	struct uart_v500_port *uap =
		container_of(port, struct uart_v500_port, port);
	int ret;

	ret = request_irq(port->irq, uart_v500_int, 0, DRV_NAME, uap);
	if (ret)
		return ret;

	uart_v500_write(uap, UART_V500_CLR_INT_ALL, UART_V500_INT_CLR);

	/* RX (and RX timeout) unmasked, TX masked */
	uap->int_msk = UART_V500_MSK_INT_ALL &
		~(UART_V500_INT_RXS | UART_V500_INT_RTS);
	uart_v500_write(uap, uap->int_msk, UART_V500_INT_MSK);

	uart_v500_write(uap, UART_V500_FIFO_CFG_DEF, UART_V500_FIFO_CFG);

	uart_v500_write(uap, UART_V500_CFG_UART_EN | UART_V500_CFG_RX_EN |
			UART_V500_CFG_TX_EN | UART_V500_CFG_FIFO_EN,
			UART_V500_CONFIG);

	uap->old_status = uart_v500_read(uap, UART_V500_STAT) &
		UART_V500_STAT_CTS;

	return 0;
}

static void uart_v500_shutdown(struct uart_port *port)
{
	struct uart_v500_port *uap =
		container_of(port, struct uart_v500_port, port);

	uart_v500_drain_tx(uap);
	uart_v500_write(uap, UART_V500_MSK_INT_ALL, UART_V500_INT_MSK);
	uart_v500_write(uap, UART_V500_CLR_INT_ALL, UART_V500_INT_CLR);
	free_irq(port->irq, uap);
}

static const char *uart_v500_type(struct uart_port *port)
{
	return port->type == PORT_UART_V500 ? DRV_NAME : NULL;
}

static int uart_v500_request_port(struct uart_port *port)
{
	/* I/O region mapped via devm in probe */
	return 0;
}

static void uart_v500_release_port(struct uart_port *port)
{
	/* released via devm */
}

static void uart_v500_config_port(struct uart_port *port, int flags)
{
	if (flags & UART_CONFIG_TYPE)
		port->type = PORT_UART_V500;
}

static int uart_v500_verify_port(struct uart_port *port,
				  struct serial_struct *ser)
{
	int ret = 0;

	if (ser->type != PORT_UNKNOWN && ser->type != PORT_UART_V500)
		ret = -EINVAL;
	if (ser->baud_base < 9600)
		ret = -EINVAL;
	return ret;
}

static const struct uart_ops uart_v500_ops = {
	.tx_empty	= uart_v500_tx_empty,
	.set_mctrl	= uart_v500_set_mctrl,
	.get_mctrl	= uart_v500_get_mctrl,
	.stop_tx	= uart_v500_stop_tx_port,
	.start_tx	= uart_v500_start_tx,
	.stop_rx	= uart_v500_stop_rx,
	.enable_ms	= uart_v500_enable_ms,
	.break_ctl	= uart_v500_break_ctl,
	.startup	= uart_v500_startup,
	.shutdown	= uart_v500_shutdown,
	.set_termios	= uart_v500_set_termios,
	.type		= uart_v500_type,
	.release_port	= uart_v500_release_port,
	.request_port	= uart_v500_request_port,
	.config_port	= uart_v500_config_port,
	.verify_port	= uart_v500_verify_port,
};

/* --------------------------------------------------------------- console */

static void uart_v500_console_putchar(struct uart_port *port,
					   unsigned char ch)
{
	struct uart_v500_port *uap =
		container_of(port, struct uart_v500_port, port);
	unsigned int count = 0;

	while ((uart_v500_read(uap, UART_V500_STAT) & UART_V500_STAT_TXFF) &&
	       (count < UART_V500_TX_TIMEOUT_US)) {
		udelay(1);
		count++;
		barrier();
	}

	uart_v500_write(uap, ch & UART_V500_DR_DATA, UART_V500_TX_FIFO);
}

static void uart_v500_console_write(struct console *co, const char *s,
				    unsigned int count)
{
	struct uart_v500_port *uap;
	unsigned long flags;
	int locked = 1;

#ifdef CONFIG_CHARLOTTE_BBOXLOG
	/* Mirror the real console too: the ramdisk /init and shell output
	 * only reach this driver once earlycon is demoted. */
	{
		extern void charlotte_ramlog_mirror_console(const char *s,
							   unsigned int n);
		charlotte_ramlog_mirror_console(s, count);
	}
#endif
	if (co->index < 0 || co->index >= SERIAL_V500_NR)
		return;

	uap = uart_v500_ports[co->index];
	if (!uap)
		return;

	if (oops_in_progress)
		locked = spin_trylock_irqsave(&uap->port.lock, flags);
	else
		spin_lock_irqsave(&uap->port.lock, flags);

	uart_console_write(&uap->port, s, count, uart_v500_console_putchar);

	if (locked)
		spin_unlock_irqrestore(&uap->port.lock, flags);
}

static void uart_v500_console_get_options(struct uart_v500_port *uap,
					  int *baud, int *parity, int *bits)
{
	u32 frame_cfg, baud_cfg, ibrd, fbrd;

	if (!(uart_v500_read(uap, UART_V500_CONFIG) & UART_V500_CFG_UART_EN))
		return;

	frame_cfg = uart_v500_read(uap, UART_V500_FRAME_CFG);
	*parity = 'n';
	if ((frame_cfg & UART_V500_FRAME_PARITY) == UART_V500_FRAME_PES)
		*parity = 'e';
	else if ((frame_cfg & UART_V500_FRAME_PARITY) == UART_V500_FRAME_POS)
		*parity = 'o';

	*bits = (frame_cfg & UART_V500_FRAME_DATA_BIT) ==
		UART_V500_FRAME_LEN_7 ? 7 : 8;

	baud_cfg = uart_v500_read(uap, UART_V500_BAUD_CFG);
	ibrd = baud_cfg & UART_V500_BAUD_INT_MASK;
	fbrd = (baud_cfg >> 16) & UART_V500_BAUD_FRAC_MASK;
	if (ibrd || fbrd)
		*baud = uap->port.uartclk * 4 / (64 * ibrd + fbrd);
}

static int uart_v500_console_setup(struct console *co, char *options)
{
	struct uart_v500_port *uap;
	int baud = 115200;
	int bits = 8;
	int parity = 'n';
	int flow = 'n';

	if (co->index < 0 || co->index >= SERIAL_V500_NR)
		co->index = 0;

	uap = uart_v500_ports[co->index];
	if (!uap)
		return -ENODEV;

	if (options)
		uart_parse_options(options, &baud, &parity, &bits, &flow);
	else
		uart_v500_console_get_options(uap, &baud, &parity, &bits);

	int ret = uart_set_options(&uap->port, co, baud, parity, bits, flow);

#ifdef CONFIG_CHARLOTTE_BBOXLOG
	/* The real console is up from here; stop mirroring earlycon to
	 * avoid duplicating every printk line in the ramlog. */
	if (!ret) {
		extern void charlotte_mirror_handover(void);
		charlotte_mirror_handover();
	}
#endif
	return ret;
}

static struct console uart_v500_console = {
	.name		= "ttyHW",
	.write		= uart_v500_console_write,
	.device		= uart_console_device,
	.setup		= uart_v500_console_setup,
	.flags		= CON_PRINTBUFFER | CON_ANYTIME,
	.index		= -1,
	.data		= &uart_v500_driver,
};

static struct uart_driver uart_v500_driver = {
	.owner		= THIS_MODULE,
	.driver_name	= DRV_NAME,
	.dev_name	= "ttyHW",
	.major		= 0,
	.minor		= 64,
	.nr		= SERIAL_V500_NR,
	.cons		= &uart_v500_console,
};

/* -------------------------------------------------------------- earlycon */

static void uart_v500_earlycon_putchar(struct uart_port *port,
					    unsigned char ch)
{
	while (readl(port->membase + UART_V500_STAT) & UART_V500_STAT_TXFF)
		cpu_relax();

	writel(ch & UART_V500_DR_DATA, port->membase + UART_V500_TX_FIFO);

	while (readl(port->membase + UART_V500_STAT) & UART_V500_STAT_TX_BUSY)
		cpu_relax();
}

static void uart_v500_earlycon_write(struct console *con,
				     const char *s, unsigned int n)
{
	struct earlycon_device *dev = con->data;

#ifdef CONFIG_CHARLOTTE_BBOXLOG
	/* Mirror the early console into the pstore ramlog so the boot
	 * trace is recoverable from stock's console-ramoops-0. */
	{
		extern void charlotte_ramlog_mirror(const char *s, unsigned int n);
		charlotte_ramlog_mirror(s, n);
	}
#endif
	uart_console_write(&dev->port, s, n, uart_v500_earlycon_putchar);
}

static int uart_v500_earlycon_setup(struct earlycon_device *device,
				    const char *opt)
{
	if (!device->port.membase)
		return -ENODEV;

	device->con->write = uart_v500_earlycon_write;

#ifdef CONFIG_CHARLOTTE_BBOXLOG
	/* Record that early console setup has been reached. */
	{
		extern void charlotte_bboxlog_early(const char *tag);
		charlotte_bboxlog_early("E:EARLYCON_SETUP");
	}
#endif

	return 0;
}

OF_EARLYCON_DECLARE(uart_v500, "hisilicon,uart", uart_v500_earlycon_setup);
EARLYCON_DECLARE(uart_v500, uart_v500_earlycon_setup);

/* ----------------------------------------------------------------- probe */

static int uart_v500_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct uart_v500_port *uap;
	struct resource *res;
	int index, ret;
	u32 val;
	u64 rate = 0;

	index = of_alias_get_id(np, "serial");
	if (index < 0 || index >= SERIAL_V500_NR)
		index = 0;
	if (uart_v500_ports[index]) {
		dev_err(&pdev->dev, "port %d already claimed\n", index);
		return -EBUSY;
	}

	uap = devm_kzalloc(&pdev->dev, sizeof(*uap), GFP_KERNEL);
	if (!uap)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;

	ret = platform_get_irq(pdev, 0);
	if (ret < 0)
		return ret;

	uap->clk = devm_clk_get_optional_enabled(&pdev->dev, NULL);
	if (IS_ERR(uap->clk))
		return PTR_ERR(uap->clk);

	uap->port.dev = &pdev->dev;
	uap->port.mapbase = res->start;
	uap->port.membase = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(uap->port.membase))
		return PTR_ERR(uap->port.membase);
	uap->port.irq = ret;
	uap->port.iotype = UPIO_MEM32;
	uap->port.ops = &uart_v500_ops;
	uap->port.flags = UPF_BOOT_AUTOCONF;
	uap->port.line = index;
	uap->port.fifosize = UART_V500_FIFO_SIZE_DEF;
	uap->fifosize = UART_V500_FIFO_SIZE_DEF;

	uap->port.uartclk = UART_V500_DEFAULT_CLK;
	of_property_read_u64(np, "clock-rate", &rate);
	if (rate)
		uap->port.uartclk = rate;
	if (uap->clk) {
		unsigned long rate = clk_get_rate(uap->clk);

		if (rate)
			uap->port.uartclk = rate;
	}
	if (of_property_read_u32(np, "fifosize", &val) == 0 && val)
		uap->port.fifosize = uap->fifosize = val;

	/* Silence any interrupts left pending by the firmware */
	uart_v500_write(uap, UART_V500_MSK_INT_ALL, UART_V500_INT_MSK);
	uart_v500_write(uap, UART_V500_CLR_INT_ALL, UART_V500_INT_CLR);

	uart_v500_ports[index] = uap;

	ret = uart_add_one_port(&uart_v500_driver, &uap->port);
	if (ret) {
		uart_v500_ports[index] = NULL;
		return ret;
	}

	platform_set_drvdata(pdev, uap);

	return 0;
}

static void uart_v500_remove(struct platform_device *pdev)
{
	struct uart_v500_port *uap = platform_get_drvdata(pdev);

	uart_remove_one_port(&uart_v500_driver, &uap->port);
	if (uart_v500_ports[uap->port.line] == uap)
		uart_v500_ports[uap->port.line] = NULL;
}

static int uart_v500_suspend(struct device *dev)
{
	struct uart_v500_port *uap = dev_get_drvdata(dev);

	if (uart_console(&uap->port) && console_suspend_enabled)
		return 0;

	return uart_suspend_port(&uart_v500_driver, &uap->port);
}

static int uart_v500_resume(struct device *dev)
{
	struct uart_v500_port *uap = dev_get_drvdata(dev);

	if (uart_console(&uap->port) && console_suspend_enabled)
		return 0;

	return uart_resume_port(&uart_v500_driver, &uap->port);
}

static DEFINE_SIMPLE_DEV_PM_OPS(uart_v500_dev_pm_ops, uart_v500_suspend,
				uart_v500_resume);

static const struct of_device_id uart_v500_of_match[] = {
	{ .compatible = "hisilicon,uart" },
	{ },
};
MODULE_DEVICE_TABLE(of, uart_v500_of_match);

static struct platform_driver uart_v500_platform_driver = {
	.probe		= uart_v500_probe,
	.remove		= uart_v500_remove,
	.driver		= {
		.name	= DRV_NAME,
		.pm	= pm_sleep_ptr(&uart_v500_dev_pm_ops),
		.of_match_table = uart_v500_of_match,
	},
};

static int __init uart_v500_init(void)
{
	int ret;

	ret = uart_register_driver(&uart_v500_driver);
	if (ret)
		return ret;

	ret = platform_driver_register(&uart_v500_platform_driver);
	if (ret)
		uart_unregister_driver(&uart_v500_driver);

	return ret;
}

static void __exit uart_v500_exit(void)
{
	platform_driver_unregister(&uart_v500_platform_driver);
	uart_unregister_driver(&uart_v500_driver);
}

module_init(uart_v500_init);
module_exit(uart_v500_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Hisilicon UART V500 serial driver (PIO)");
MODULE_AUTHOR("Ported from Huawei HarmonyOS 4.2 kernel source");
