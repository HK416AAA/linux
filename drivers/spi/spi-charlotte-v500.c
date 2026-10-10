// SPDX-License-Identifier: GPL-2.0-only
/* Charlotte SPI3 handoff, based on vendor spi_v500.c and clk-char-cs.c.
 * Retain the firmware's PLL/FLL divider and enable only the SPI3 clock gates.
 * Integrated SDMA handles raw frames; short register commands use PIO.
 * Autonomous clock/reset initialization and runtime PM are not provided.
 */
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>

#define COMMONCR 0x004
#define ENR 0x008
#define SR 0x01c
#define TNF BIT(1)
#define RNE BIT(2)
#define BSY BIT(4)
#define DMA_CTRL 0x030
#define DMA_START 0x054
#define DMA_MASK 0x058
#define DMA_CLEAR 0x05c
#define DMA_RAW 0x060
#define DMA_DONE BIT(12)
#define DMA_ALL 0x1fff
#define DMA_MAX 262144

static bool use_dma = true;
module_param(use_dma, bool, 0644);
MODULE_PARM_DESC(use_dma, "Use integrated SDMA for transfers of at least 32 bytes");

struct charlotte_spi {
	struct device *dev;
	void __iomem *base;
	void __iomem *sctrl;
	u32 input_hz;
	u8 *dma_tx, *dma_rx;
	dma_addr_t tx_addr, rx_addr;
	bool dma_reported;
};

static int charlotte_spi_dma(struct charlotte_spi *cs,
			     struct spi_transfer *xfer)
{
	u32 status, raw;
	ktime_t started;
	int ret;

	if (xfer->len > DMA_MAX)
		return -EMSGSIZE;
	/* Allocate on first use, after userspace has opened the transport.
	 * The firmware handoff and system boot need no DMA buffers.
	 * Streaming mappings keep the cached linear mapping and avoid aliases.
	 */
	if (!cs->dma_tx)
		cs->dma_tx = (void *)devm_get_free_pages(cs->dev,
				GFP_KERNEL | GFP_DMA32, get_order(DMA_MAX));
	if (!cs->dma_rx)
		cs->dma_rx = (void *)devm_get_free_pages(cs->dev,
				GFP_KERNEL | GFP_DMA32, get_order(DMA_MAX));
	if (!cs->dma_tx || !cs->dma_rx)
		return -ENOMEM;
	if (xfer->tx_buf)
		memcpy(cs->dma_tx, xfer->tx_buf, xfer->len);
	else
		memset(cs->dma_tx, 0, xfer->len);
	cs->tx_addr = dma_map_single(cs->dev, cs->dma_tx, xfer->len, DMA_TO_DEVICE);
	if (dma_mapping_error(cs->dev, cs->tx_addr))
		return -ENOMEM;
	cs->rx_addr = dma_map_single(cs->dev, cs->dma_rx, xfer->len, DMA_FROM_DEVICE);
	if (dma_mapping_error(cs->dev, cs->rx_addr)) {
		dma_unmap_single(cs->dev, cs->tx_addr, xfer->len, DMA_TO_DEVICE);
		return -ENOMEM;
	}
	writel(DMA_ALL, cs->base + DMA_MASK);
	writel(DMA_ALL, cs->base + DMA_CLEAR);
	writel(upper_32_bits(cs->tx_addr), cs->base + 0x034);
	writel(lower_32_bits(cs->tx_addr), cs->base + 0x038);
	writel(upper_32_bits(cs->rx_addr), cs->base + 0x03c);
	writel(lower_32_bits(cs->rx_addr), cs->base + 0x040);
	writel(BIT(21) | xfer->len, cs->base + DMA_CTRL);
	dma_wmb();
	started = ktime_get();
	writel(1, cs->base + ENR);
	writel(1, cs->base + DMA_START);
	ret = readl_poll_timeout(cs->base + DMA_RAW, status,
				 status & DMA_ALL, 2, 1000000);
	if (!ret && status != DMA_DONE)
		ret = -EIO;
	raw = status;
	writel(0, cs->base + DMA_CTRL);
	writel(DMA_ALL, cs->base + DMA_CLEAR);
	if (!ret) {
		ret = readl_poll_timeout(cs->base + SR, status,
					 !(status & BSY), 1, 10000);
	}
	writel(0, cs->base + ENR);
	if (ret || !cs->dma_reported) {
		dev_info(cs->dev,
			 "SDMA len=%u tx=%pad rx=%pad raw=%#x elapsed=%lld us result=%d\n",
			 xfer->len, &cs->tx_addr, &cs->rx_addr, raw,
			 ktime_us_delta(ktime_get(), started), ret);
		cs->dma_reported = true;
	}
	dma_unmap_single(cs->dev, cs->rx_addr, xfer->len, DMA_FROM_DEVICE);
	dma_unmap_single(cs->dev, cs->tx_addr, xfer->len, DMA_TO_DEVICE);
	if (!ret && xfer->rx_buf)
		memcpy(xfer->rx_buf, cs->dma_rx, xfer->len);
	return ret;
}

static void __iomem *charlotte_spi_shared_map(struct platform_device *pdev,
					     unsigned int index)
{
	struct resource *res = platform_get_resource(pdev, IORESOURCE_MEM, index);

	if (!res)
		return NULL;
	return devm_ioremap(&pdev->dev, res->start, resource_size(res));
}

static void charlotte_spi_update(void __iomem *base, u32 off, u32 mask, u32 val)
{
	writel((readl(base + off) & ~mask) | val, base + off);
}

static int charlotte_spi_pads(struct platform_device *pdev)
{
	void __iomem *mux = charlotte_spi_shared_map(pdev, 2);
	void __iomem *pad = charlotte_spi_shared_map(pdev, 3);
	void __iomem *reset = charlotte_spi_shared_map(pdev, 4);
	unsigned int i;
	static const u32 offsets[] = { 0x724, 0x728, 0x72c, 0x730, 0x778 };
	static const u32 drive[] = { 0x70, 0x30, 0x40, 0x10, 0x30 };

	if (!mux || !pad || !reset)
		return -ENOMEM;
	/* SPI3 clock/MISO/MOSI, GPIO177 CS, GPIO222 IRQ. V500 has
	 * separate mux and pad-routing registers; both must be set.
	 */
	for (i = 0; i < ARRAY_SIZE(offsets); i++) {
		charlotte_spi_update(mux, offsets[i], 7, i < 3 ? 4 : 0);
		charlotte_spi_update(pad, offsets[i], 7, i < 3 ? 4 : 0);
		charlotte_spi_update(pad, 0x800 + offsets[i],
				     i < 3 ? 0x70 : 0x73, drive[i]);
	}
	charlotte_spi_update(reset, 0x874, 0x73, 0x30);
	return 0;
}

static int charlotte_spi_clock(struct spi_controller *ctlr)
{
	struct charlotte_spi *cs = spi_controller_get_devdata(ctlr);
	u32 clock = readl(cs->sctrl + 0x2a8);
	unsigned int gate = clock & BIT(15) ? 13 : 14;

	writel(BIT(gate + 16) | BIT(gate), cs->sctrl + 0x2a8);
	writel(BIT(10), cs->sctrl + 0x1b0);
	if (clock & BIT(15))
		cs->input_hz = 208906485 / ((readl(cs->sctrl + 0x2ac) & 15) + 1);
	else
		cs->input_hz = 1671168000 / (((clock >> 7) & 63) + 1);
	return 0;
}

static int charlotte_spi_transfer(struct spi_controller *ctlr,
				  struct spi_device *spi,
				  struct spi_transfer *xfer)
{
	struct charlotte_spi *cs = spi_controller_get_devdata(ctlr);
	const u8 *tx = xfer->tx_buf;
	u8 *rx = xfer->rx_buf;
	u32 best = 0, config = 7 << 20, status;
	unsigned int pre, post, sent = 0, received = 0, loops = 0;
	ktime_t deadline;
	int ret;

	for (pre = 2; pre <= 254; pre += 2) {
		post = DIV_ROUND_UP_ULL(cs->input_hz, (u64)pre * xfer->speed_hz);
		if (!post || post > 256)
			continue;
		status = cs->input_hz / (pre * post);
		if (status > best) {
			best = status;
			config = (7 << 20) | (pre << 4) | ((post - 1) << 12);
		}
	}
	if (!best)
		return -EINVAL;
	xfer->effective_speed_hz = best;
	if (spi->mode & SPI_CPOL)
		config |= BIT(2);
	if (spi->mode & SPI_CPHA)
		config |= BIT(3);
	writel(0, cs->base + ENR);
	writel(config, cs->base + COMMONCR);
	/* Drain stale RX without unbounded polling. */
	for (pre = 0; pre < 256 && readl(cs->base + SR) & RNE; pre++)
		readl(cs->base + 0x018);
	if (readl(cs->base + SR) & RNE)
		return -EIO;
	if (xfer->len >= 32 && READ_ONCE(use_dma))
		return charlotte_spi_dma(cs, xfer);
	writel(1, cs->base + ENR);
	deadline = ktime_add_ms(ktime_get(), 1000);
	while (received < xfer->len) {
		status = readl(cs->base + SR);
		if ((status & RNE) && received < sent) {
			u8 value = readl(cs->base + 0x018);

			if (rx)
				rx[received] = value;
			received++;
		}
		if ((status & TNF) && sent < xfer->len && sent - received < 128) {
			writel(tx ? tx[sent] : 0, cs->base + 0x014);
			sent++;
		}
		if (!(++loops & 1023)) {
			if (ktime_after(ktime_get(), deadline)) {
				writel(0, cs->base + ENR);
				return -ETIMEDOUT;
			}
			cond_resched();
		}
	}
	ret = readl_poll_timeout(cs->base + SR, status, !(status & BSY), 1, 10000);
	writel(0, cs->base + ENR);
	return ret;
}

static int charlotte_spi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct spi_controller *ctlr;
	struct charlotte_spi *cs;
	int ret;

	ctlr = devm_spi_alloc_host(dev, sizeof(*cs));
	if (!ctlr)
		return -ENOMEM;
	cs = spi_controller_get_devdata(ctlr);
	cs->dev = dev;
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	cs->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(cs->base))
		return PTR_ERR(cs->base);
	/* SCTRL is also used by USB and the reboot driver. */
	cs->sctrl = charlotte_spi_shared_map(pdev, 1);
	if (!cs->sctrl)
		return -ENOMEM;
	charlotte_spi_clock(ctlr);
	ret = charlotte_spi_pads(pdev);
	if (ret)
		return ret;
	writel(0, cs->base + ENR);
	writel(0, cs->base); /* GPIO chip select, no automatic internal CS. */
	writel(0x3ff, cs->base + 0x010);
	writel(0x3ff, cs->base + 0x028);
	writel(0, cs->base + 0x00c);
	ctlr->bus_num = -1;
	ctlr->num_chipselect = 1;
	ctlr->use_gpio_descriptors = true;
	ctlr->mode_bits = SPI_CPOL | SPI_CPHA;
	ctlr->bits_per_word_mask = SPI_BPW_MASK(8);
	ctlr->min_speed_hz = DIV_ROUND_UP(cs->input_hz, 254 * 256);
	ctlr->max_speed_hz = 21000000;
	ctlr->transfer_one = charlotte_spi_transfer;
	ctlr->prepare_transfer_hardware = charlotte_spi_clock;
	dev_info(dev, "SPI3 PIO/SDMA: input %u Hz, version %#x\n",
		 cs->input_hz, readl(cs->base + 0xfe0));
	return devm_spi_register_controller(dev, ctlr);
}

static const struct of_device_id charlotte_spi_match[] = {
	{ .compatible = "hisilicon,charlotte-spi-v500" }, { }
};
static struct platform_driver charlotte_spi_driver = {
	.probe = charlotte_spi_probe,
	.driver = { .name = "charlotte-spi-v500", .of_match_table = charlotte_spi_match,
		    .suppress_bind_attrs = true },
};
builtin_platform_driver(charlotte_spi_driver);
