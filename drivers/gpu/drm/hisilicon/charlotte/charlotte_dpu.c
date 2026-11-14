// SPDX-License-Identifier: GPL-2.0-only
/* Charlotte DPU v700: atomic DMA-buffer scanout using the firmware's
 * single-layer scene/DSC/dual-DSI configuration. No framebuffer CPU blit.
 * Register layout checked against the B236 GPL definitions and live handoff.
 * Cold panel initialization, alternate modes and runtime PM are not provided.
 */
#include <linux/aperture.h>
#include <linux/debugfs.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/workqueue.h>
#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic.h>
#include <drm/drm_encoder.h>
#include <drm/drm_drv.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_print.h>
#include <drm/drm_plane.h>
#include <drm/drm_vblank.h>

#define DSI_INTS 0x248
#define DSI_MASK 0x24c
#define DSI_VSYNC BIT(4)
#define DSI_VACT_START BIT(7)
#define DSI_VACT_END BIT(8)
#define DSI_UNDERFLOW BIT(2)
#define DSI_LCD_TE0 BIT(12)
#define DM0 0x35400
#define DM_LAYER_SIZE (DM0 + 0x1a0)
#define DM_LAYER_SELECT (DM0 + 0x1ac)
#define DM_LAYER_OV_FMT (DM0 + 0x1b4)
#define DM_ADDRESS_H (DM0 + 0x1dc)
#define DM_ADDRESS_L (DM0 + 0x1e0)
#define DM_STRIDE (DM0 + 0x1f0)
#define DMC0 0x40400
/* Vendor dpu_dm_param image: 170 sixteen-byte items starting at DM0. */
#define DM_PARAM_SIZE 0xaa0
#define CMDLIST_HDR_SIZE 16
#define CMDLIST_ITEM_SIZE 16
#define DM_PARAM_ITEMS ((DM_PARAM_SIZE) / (CMDLIST_ITEM_SIZE))
/* Layer0 fields inside the DM image, as word indices. */
#define DM_W_START_ADDR_H ((0x1dc) / 4)
#define DM_W_START_ADDR_L ((0x1e0) / 4)
#define DM_W_STRIDE ((0x1f0) / 4)
#define DM_W_DMA_SEL ((0x1ac) / 4)
#define DM_W_LAYER_HEIGHT ((0x1a0) / 4)
#define DPU_WINDOW_SIZE 0x100000
#define DACC_SCENE0 0x40400
#define DACC_GLB0 0x13000
/* Command transport: nine scenes, INITIAL(7) is the one-shot loader. */
#define CMDLIST0 0x12000
#define CMDLIST_CH_CTRL(i) (CMDLIST0 + 0x04 + 0x40 * (i))
#define CMDLIST_CH_STAAD(i) (CMDLIST0 + 0x0c + 0x40 * (i))
#define CMDLIST_CH_INTE(i) (CMDLIST0 + 0x14 + 0x40 * (i))
#define CMDLIST_START (CMDLIST0 + 0x714)
#define CMDLIST_ADDR_MASK_DIS (CMDLIST0 + 0x71c)
#define CMDLIST_TWO_SAME_FRAME_BYPASS (CMDLIST0 + 0x7a0)
#define CMDLIST_SCENE_MAX 9
#define CMDLIST_SCENE_INIT 7
#define DACC_GLB_DP0_STATUS (DACC_GLB0 + 0x240)
#define DACC_GLB_DP0_MASK (DACC_GLB0 + 0x244)
#define DACC_GLB_DP1_STATUS (DACC_GLB0 + 0x248)
#define DACC_GLB_DP1_MASK (DACC_GLB0 + 0x24c)
#define DACC_GLB_MDP_STATUS (DACC_GLB0 + 0x250)
#define DACC_GLB_MDP_MASK (DACC_GLB0 + 0x254)
#define DACC_GLB_SDP_STATUS (DACC_GLB0 + 0x258)
#define DACC_GLB_SDP_MASK (DACC_GLB0 + 0x25c)
#define WIDTH 1840
#define HEIGHT 2800
#define TEST_PITCH 7424
#define TEST_SIZE PAGE_ALIGN(TEST_PITCH * HEIGHT)

static bool probe_only = true;
module_param(probe_only, bool, 0444);
MODULE_PARM_DESC(probe_only, "Keep simpledrm and expose bounded handoff diagnostics instead of native KMS");
static bool strict_handoff;
module_param(strict_handoff, bool, 0444);
MODULE_PARM_DESC(strict_handoff, "Refuse to take over when the firmware register signature differs from the TGR one");

struct charlotte_dpu {
	struct drm_device drm;
	struct device *dev;
	struct drm_plane plane;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector connector;
	void __iomem *regs;
	spinlock_t lock;
	struct mutex diagnostic_lock;
	struct i2c_client *backlight;
	bool backlight_on;
	bool backlight_ro;
	int irq;
	bool irq_requested, irq_running, irq_storm;
	u64 irq_count, irq_window_ns;
	u32 irq_window_count;
	bool kms, kms_attempted, pending, latched;
	u32 boot_address_h, boot_address_l, boot_stride, boot_mask;
	u32 boot_select, boot_ov_fmt;
	void *cmdlist_buf;
	dma_addr_t cmdlist_dma;
	u32 *cmdlist_item;
	void *regcmd_buf;
	dma_addr_t regcmd_dma;
	u64 frame_count, vsync_count, te_count, underflows, flips;
	u64 last_frame_ns, frame_period_ns;
	dma_addr_t next_address;
	u32 next_stride;
	u32 next_format;
	struct drm_pending_vblank_event *event;
	bool event_ref;
	struct drm_framebuffer *active_fb, *pending_fb, *retire_fb;
	struct work_struct retire_work;
	struct completion flip_done;
	struct dentry *debugfs;
	struct delayed_work restore_work;
	struct delayed_work irq_stop_work;
	void *test_vaddr;
	dma_addr_t test_address;
	void *blank_vaddr;
	dma_addr_t blank_address;
	bool test_active, test_restoring;
};
static const struct drm_driver charlotte_drm_driver;
static struct charlotte_dpu *crtc_to_dpu(struct drm_crtc *crtc)
{ return container_of(crtc, struct charlotte_dpu, crtc); }
static int charlotte_kms_init(struct charlotte_dpu *dpu);

/* Retain the RT4539's firmware brightness configuration. Only switch BL_EN;
 * keep the LED channel mask, PWM/current/boost settings and GPIO enable.
 * A bound I2C supplier also gives fw_devlink proper probe/shutdown ordering.
 */
static int charlotte_backlight_probe(struct i2c_client *client)
{
	int value;
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE_DATA))
		return -EOPNOTSUPP;
	value = i2c_smbus_read_byte_data(client, 0x0b);
	/* Bind even when the LED control register cannot be read.  The DPU
	 * consumer is linked to this supplier through the backlight phandle,
	 * so a failing probe would keep the whole display deferred forever.
	 * The firmware already left the panel lit; the KMS handoff must not
	 * depend on gaining backlight control first.
	 */
	if (value < 0)
		dev_warn(&client->dev,
			 "LED control read failed: %d; keeping firmware brightness\n", value);
	else if (!(value & 0x7e))
		dev_warn(&client->dev,
			 "no enabled LED channels (0x%02x); keeping firmware brightness\n", value);
	else
		dev_info(&client->dev, "RT4539 LED control=%02x; retaining firmware brightness\n", value);
	i2c_set_clientdata(client, client);
	return 0;
}
static const struct of_device_id charlotte_backlight_match[] = {
	{ .compatible = "richtek,rt4539" }, {}
};
MODULE_DEVICE_TABLE(of, charlotte_backlight_match);
static struct i2c_driver charlotte_backlight_driver = {
	.probe = charlotte_backlight_probe,
	.driver = { .name = "charlotte-rt4539", .of_match_table = charlotte_backlight_match,
		.suppress_bind_attrs = true },
};
static void charlotte_backlight_put(void *data)
{ put_device(&((struct i2c_client *)data)->dev); }
static int charlotte_backlight_init(struct charlotte_dpu *dpu)
{
	struct device_node *node;
	struct i2c_client *client;
	int value, ret;
	node = of_parse_phandle(dpu->drm.dev->of_node, "backlight", 0);
	if (!node)
		return -ENODEV;
	client = of_find_i2c_device_by_node(node);
	of_node_put(node);
	if (!client)
		return -EPROBE_DEFER;
	if (!i2c_get_clientdata(client)) {
		put_device(&client->dev);
		return -EPROBE_DEFER;
	}
	ret = devm_add_action_or_reset(dpu->drm.dev, charlotte_backlight_put, client);
	if (ret)
		return ret;
	value = i2c_smbus_read_byte_data(client, 0x0b);
	if (value < 0) {
		dev_warn(dpu->drm.dev,
			 "backlight LED control read failed: %d; preserving firmware state\n",
			 value);
		dpu->backlight = client;
		dpu->backlight_on = true;
		dpu->backlight_ro = true;
		return 0;
	}
	dpu->backlight = client;
	dpu->backlight_on = !!(value & BIT(7));
	return 0;
}
static void charlotte_backlight_power(struct charlotte_dpu *dpu, bool on)
{
	int value, ret;
	/* Leave the panel at the brightness the firmware programmed when the
	 * LED control register never became readable.
	 */
	if (!dpu->backlight || dpu->backlight_ro)
		return;
	/* Atomic commits serialize enable/disable; all I2C operations are in
	 * process context and use the adapter lock, never raw MMIO in the IRQ.
	 */
	value = i2c_smbus_read_byte_data(dpu->backlight, 0x0b);
	if (value < 0) {
		ret = value;
		goto error;
	}
	value = (value & ~BIT(7)) | (on ? BIT(7) : 0);
	ret = i2c_smbus_write_byte_data(dpu->backlight, 0x0b, value);
	if (ret < 0)
		goto error;
	ret = i2c_smbus_read_byte_data(dpu->backlight, 0x0b);
	if (ret != value) {
		ret = ret < 0 ? ret : -EIO;
		goto error;
	}
	WRITE_ONCE(dpu->backlight_on, on);
	return;
error:
	drm_err(&dpu->drm, "backlight %s failed: %d\n", on ? "on" : "off", ret);
}

static void program_scanout(struct charlotte_dpu *dpu, dma_addr_t addr, u32 pitch, u32 format)
{
	if (dpu->cmdlist_item) {
		dpu->cmdlist_item[DM_W_START_ADDR_H] = upper_32_bits(addr);
		dpu->cmdlist_item[DM_W_START_ADDR_L] = lower_32_bits(addr);
		dpu->cmdlist_item[DM_W_STRIDE] = pitch;
		dpu->cmdlist_item[DM_W_DMA_SEL] = (dpu->cmdlist_item[DM_W_DMA_SEL] & ~0xff00U) |
			(format == DRM_FORMAT_XRGB8888 ? 5U : 13U) << 8;
		wmb();
	}
	u32 hw_format = format == DRM_FORMAT_XRGB8888 ? 5 : 13;
	/* GPL opr_set_sdma_layer_fmt sets both the DMA and dynamic DFC formats.
	 * Firmware uses ABGR (13); GNOME's opaque BGRA buffers use XRGB (5).
	 */
	writel((dpu->boot_select & ~0xff00U) | hw_format << 8, dpu->regs + DM_LAYER_SELECT);
	writel((dpu->boot_ov_fmt & ~0xffU) | hw_format, dpu->regs + DM_LAYER_OV_FMT);
	writel(upper_32_bits(addr) & 0xff, dpu->regs + DM_ADDRESS_H);
	writel(lower_32_bits(addr), dpu->regs + DM_ADDRESS_L);
	writel(pitch, dpu->regs + DM_STRIDE);
	wmb();
}
static void queue_scanout(struct charlotte_dpu *dpu, dma_addr_t addr, u32 pitch, u32 format)
{
	dpu->next_address = addr;
	dpu->next_stride = pitch;
	dpu->next_format = format;
	dpu->pending = true;
}
static irqreturn_t charlotte_irq(int irq, void *data)
{
	struct charlotte_dpu *dpu = data;
	u32 raw = readl(dpu->regs + DSI_INTS);
	u32 status = raw & ~readl(dpu->regs + DSI_MASK);
	struct drm_pending_vblank_event *event = NULL;
	bool event_ref = false;
	unsigned long flags;
	u64 now;
	/* Command-mode panels pace frames with TE; video panels use LDI VACT. */
	u32 frame_bit = DSI_VACT_START | DSI_LCD_TE0;
	/* A wrong/stuck source must not monopolize a CPU until the watchdog fires. */
	now = ktime_get_ns();
	dpu->irq_count++;
	if (now - dpu->irq_window_ns > NSEC_PER_MSEC) {
		dpu->irq_window_ns = now;
		dpu->irq_window_count = 0;
	}
	if (++dpu->irq_window_count > 100) {
		writel(~0U, dpu->regs + DSI_MASK);
		disable_irq_nosync(irq);
		dpu->irq_storm = true;
		dpu->irq_running = false;
		return IRQ_HANDLED;
	}
	if (!status)
		return IRQ_NONE;
	writel(raw, dpu->regs + DSI_INTS);
	spin_lock_irqsave(&dpu->lock, flags);
	if (status & DSI_UNDERFLOW)
		dpu->underflows++;
	if (status & (DSI_VACT_END | DSI_LCD_TE0) && dpu->pending) {
		program_scanout(dpu, dpu->next_address, dpu->next_stride, dpu->next_format);
		dpu->pending = false;
		dpu->latched = true;
		dpu->flips++;
	}
	if (status & DSI_LCD_TE0)
		dpu->te_count++;
	if (status & frame_bit) {
		now = ktime_get_ns();
		if (dpu->last_frame_ns)
			dpu->frame_period_ns = now - dpu->last_frame_ns;
		dpu->last_frame_ns = now;
		dpu->frame_count++;
		if (dpu->latched) {
			dpu->latched = false;
			event = dpu->event;
			dpu->event = NULL;
			event_ref = dpu->event_ref;
			dpu->event_ref = false;
			/* A confirmed new frame retires the previous scanout. Framebuffer
			 * references are released by work, never from hardirq context.
			 */
			if (dpu->kms) {
				dpu->retire_fb = dpu->active_fb;
				dpu->active_fb = dpu->pending_fb;
				dpu->pending_fb = NULL;
				if (dpu->retire_fb)
					schedule_work(&dpu->retire_work);
			}
			complete_all(&dpu->flip_done);
			if (dpu->test_restoring) {
				dpu->test_restoring = false;
				dpu->test_active = false;
			}
		}
	}
	if (status & DSI_VSYNC)
		dpu->vsync_count++;
	spin_unlock_irqrestore(&dpu->lock, flags);
	if (dpu->kms && status & frame_bit) {
		drm_crtc_handle_vblank(&dpu->crtc);
		if (event) {
			spin_lock_irqsave(&dpu->drm.event_lock, flags);
			drm_crtc_send_vblank_event(&dpu->crtc, event);
			if (event_ref)
				drm_crtc_vblank_put(&dpu->crtc);
			spin_unlock_irqrestore(&dpu->drm.event_lock, flags);
		}
	}
	return IRQ_HANDLED;
}
static int charlotte_status_show(struct seq_file *s, void *unused)
{
	struct charlotte_dpu *dpu = s->private;
	unsigned long flags;
	spin_lock_irqsave(&dpu->lock, flags);
	seq_printf(s, "kms=%u irq=%d irq_running=%u irq_storm=%u irq_count=%llu frames=%llu vsyncs=%llu te=%llu period_ns=%llu underflows=%llu flips=%llu\n",
		dpu->kms, dpu->irq, dpu->irq_running, dpu->irq_storm, dpu->irq_count, dpu->frame_count, dpu->vsync_count,
		dpu->te_count, dpu->frame_period_ns, dpu->underflows, dpu->flips);
	seq_printf(s, "address=%02x%08x stride=%u pending=%u test_active=%u test_restoring=%u\n",
		readl(dpu->regs + DM_ADDRESS_H) & 0xff,
		readl(dpu->regs + DM_ADDRESS_L), readl(dpu->regs + DM_STRIDE),
		dpu->pending, dpu->test_active, dpu->test_restoring);
	seq_printf(s, "boot_address=%02x%08x boot_stride=%u test_address=%pad\n",
		dpu->boot_address_h & 0xff, dpu->boot_address_l,
		dpu->boot_stride, &dpu->test_address);
	spin_unlock_irqrestore(&dpu->lock, flags);
	seq_printf(s, "backlight_on=%u\n", READ_ONCE(dpu->backlight_on));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(charlotte_status);

struct charlotte_reg_desc {
	const char *name;
	u32 off;
};
static const struct charlotte_reg_desc charlotte_regs[] = {
	{ "dsi0_pm", 0x00198 }, { "dsi0_ldi", 0x001b8 }, { "dsi0_cmdmod", 0x001c0 },
	{ "dsi0_te", 0x001c4 }, { "dsi0_frmmsk", 0x001d4 }, { "dsi0_frmvalid", 0x001d8 },
	{ "dsi0_vinact", 0x001dc }, { "dsi0_vstate", 0x001fc }, { "dsi0_ints", 0x00248 },
	{ "dsi0_mask", 0x0024c },
	{ "dsi1_pm", 0x00598 }, { "dsi1_ldi", 0x005b8 }, { "dsi1_ints", 0x00648 },
	{ "dsi1_mask", 0x0064c },
	{ "dsi2_pm", 0x00998 }, { "dsi2_ldi", 0x009b8 }, { "dsi2_ints", 0x00a48 },
	{ "dsi2_mask", 0x00a4c },
	{ "dsiglb_dualhdr", 0x00c1c }, { "dsiglb_dualwr", 0x00c24 },
	{ "cmd0_ctrl", 0x12004 }, { "cmd0_status", 0x12008 }, { "cmd0_curad", 0x12010 },
	{ "dm_cmdh", 0x35400 }, { "dm_cmd0", 0x35404 }, { "dm_cmd1", 0x35408 },
	{ "dm_cmd2", 0x3540c }, { "dm_cmd3", 0x35410 }, { "dm_cmd4", 0x35414 },
	{ "dm_cmd5", 0x35418 }, { "dm_cmd6", 0x3541c }, { "dm_cmd7", 0x35420 },
	{ "dm_srotnum", 0x35450 }, { "dm_ddicnum", 0x35454 },
	{ "glb_tag", 0x13000 }, { "glb_jtag", 0x13008 }, { "glb_clken", 0x13104 },
	{ "glb_pm", 0x13108 }, { "glb_ch0ken", 0x13128 }, { "glb_ch1ken", 0x13130 },
	{ "glb_avhrken", 0x13138 }, { "glb_dispken", 0x13140 }, { "glb_rst", 0x13144 },
	{ "glb_dfsok", 0x13200 },
	{ "dp0_stat", 0x13240 }, { "dp0_mask", 0x13244 }, { "dp1_stat", 0x13248 },
	{ "dp1_mask", 0x1324c }, { "mdp_stat", 0x13250 }, { "mdp_mask", 0x13254 },
	{ "sdp_stat", 0x13258 }, { "sdp_mask", 0x1325c },
	{ "dacc_int", 0x40000 }, { "dacc_intmsk", 0x40004 }, { "dacc_resetpc", 0x40008 },
	{ "dacc_cfg", 0x4000c }, { "dacc_status", 0x40010 }, { "dacc_monpc", 0x40014 },
	{ "dacc_monlr", 0x40018 }, { "dacc_ckgen", 0x40048 },
	{ "sc0_clksel", 0x40400 }, { "sc0_clken", 0x40404 }, { "sc0_swstart", 0x40410 },
	{ "sc0_stfrm", 0x4041c }, { "sc0_cfgmode", 0x40424 },
	{ "dm_num", 0x35458 }, { "dm_scene", 0x3545c }, { "dm_ovwidth", 0x35460 },
	{ "dm_srot0_w", 0x354a0 }, { "dm_srot0_ow", 0x354a4 }, { "dm_srot0_fmt", 0x354a8 },
	{ "dm_srot1_w", 0x354b0 }, { "dm_srot1_ow", 0x354b4 },
	{ "l0_size", 0x355a0 }, { "l0_maskx", 0x355a4 }, { "l0_masky", 0x355a8 },
	{ "l0_sel", 0x355ac }, { "l0_sblk", 0x355b0 }, { "l0_crop", 0x355b4 },
	{ "l0_addrh", 0x355dc }, { "l0_addrl", 0x355e0 }, { "l0_stride", 0x355f0 },
	{ "l1_sel", 0x3562c }, { "l1_addrl", 0x35660 }, { "l1_stride", 0x35670 },
	{ "itf0_size", 0xcc000 }, { "itf0_flush", 0xcc05c }, { "itf0_datasel", 0xcc068 },
	{ "psw_sig0", 0xcc400 }, { "psw_sigen0", 0xcc404 }, { "psw_dat0", 0xcc408 },
	{ "psw_daten0", 0xcc40c }, { "psw_splith", 0xcc48c }, { "psw_splitc", 0xcc490 },
};
static int charlotte_reg_show(struct seq_file *s, void *unused)
{
	struct charlotte_dpu *dpu = s->private;
	unsigned int i;
	for (i = 0; i < ARRAY_SIZE(charlotte_regs); i++)
		seq_printf(s, "%-13s %06x %08x\n", charlotte_regs[i].name,
			charlotte_regs[i].off, readl(dpu->regs + charlotte_regs[i].off));
	return 0;
}
static ssize_t charlotte_reg_write(struct file *file, const char __user *buf,
	size_t len, loff_t *pos)
{
	struct charlotte_dpu *dpu = file->f_inode->i_private;
	char tmp[32];
	unsigned int off, val;
	if (!len || len >= sizeof(tmp))
		return -EINVAL;
	if (copy_from_user(tmp, buf, len))
		return -EFAULT;
	tmp[len] = '\0';
	if (sscanf(tmp, "%x %x", &off, &val) == 1) {
		if ((off & 3) || off >= DPU_WINDOW_SIZE)
			return -EINVAL;
		dev_info(dpu->drm.dev, "reg read %06x = %08x\n", off,
			readl(dpu->regs + off));
		return len;
	}
	if (sscanf(tmp, "%x %x", &off, &val) != 2)
		return -EINVAL;
	if ((off & 3) || off >= DPU_WINDOW_SIZE)
		return -EINVAL;
	writel(val, dpu->regs + off);
	return len;
}
static int charlotte_reg_open(struct inode *inode, struct file *file)
{
	return single_open(file, charlotte_reg_show, inode->i_private);
}
static const struct file_operations charlotte_reg_fops = {
	.owner = THIS_MODULE, .open = charlotte_reg_open, .write = charlotte_reg_write,
	.read = seq_read, .llseek = seq_lseek, .release = single_release,
};
static ssize_t charlotte_dacc_write(struct file *file, const char __user *buf,
	size_t len, loff_t *pos)
{
	struct charlotte_dpu *dpu = file->private_data;
	unsigned int value;
	int ret = kstrtouint_from_user(buf, len, 0, &value);
	if (ret)
		return ret;
	if (value > 1)
		return -EINVAL;
	/* Vendor dpu_dacc_config_scene(): online scene keeps ST_FRM_SEL0 cleared.
	 * value 0 selects direct mode, value 1 selects cmdlist mode like vendor.
	 */
	writel(readl(dpu->regs + DACC_SCENE0) | 1, dpu->regs + DACC_SCENE0);
	writel(readl(dpu->regs + DACC_SCENE0 + 0x04) | 1, dpu->regs + DACC_SCENE0 + 0x04);
	if (value)
		writel(readl(dpu->regs + DACC_SCENE0 + 0x24) | 1, dpu->regs + DACC_SCENE0 + 0x24);
	else
		writel(readl(dpu->regs + DACC_SCENE0 + 0x24) & ~1U, dpu->regs + DACC_SCENE0 + 0x24);
	writel(readl(dpu->regs + DACC_SCENE0 + 0x1c) & ~1U, dpu->regs + DACC_SCENE0 + 0x1c);
	writel(0, dpu->regs + DACC_GLB_DP0_MASK);
	writel(0, dpu->regs + DACC_GLB_DP1_MASK);
	writel(0, dpu->regs + DACC_GLB_MDP_MASK);
	writel(0, dpu->regs + DACC_GLB_SDP_MASK);
	dev_info(dpu->drm.dev, "dacc scene0 enabled (%s mode), DPU interrupt outputs unmasked\n",
		value ? "cmdlist" : "direct");
	return len;
}
static const struct file_operations charlotte_dacc_fops = {
	.owner = THIS_MODULE, .open = simple_open, .write = charlotte_dacc_write,
	.llseek = noop_llseek,
};

static void charlotte_cmdlist_release(struct charlotte_dpu *dpu)
{
	if (dpu->regcmd_buf) {
		dma_free_coherent(dpu->dev, 32, dpu->regcmd_buf, dpu->regcmd_dma);
		dpu->regcmd_buf = NULL;
		dpu->regcmd_dma = 0;
	}
	if (!dpu->cmdlist_buf)
		return;
	dma_free_coherent(dpu->dev, CMDLIST_HDR_SIZE + DM_PARAM_SIZE,
		dpu->cmdlist_buf, dpu->cmdlist_dma);
	dpu->cmdlist_buf = NULL;
	dpu->cmdlist_item = NULL;
	dpu->cmdlist_dma = 0;
}

/*
 * Vendor composer present path: build a DM transport cmdlist whose payload is
 * the dpu_dm_param register image, point its own cmdlist_addr0 back at itself
 * so the DACC re-runs it every frame, then hand the header to DM_CMDLIST_ADDR0.
 */
static int charlotte_cmdlist_start(struct charlotte_dpu *dpu)
{
	size_t size = CMDLIST_HDR_SIZE + DM_PARAM_SIZE;
	struct device *dev = dpu->dev;
	u32 *hdr, *item;
	unsigned int i;

	if (dpu->cmdlist_buf)
		return -EBUSY;
	dpu->cmdlist_buf = dma_alloc_coherent(dev, size, &dpu->cmdlist_dma, GFP_KERNEL);
	if (!dpu->cmdlist_buf)
		return -ENOMEM;
	memset(dpu->cmdlist_buf, 0, size);
	item = (u32 *)((u8 *)dpu->cmdlist_buf + CMDLIST_HDR_SIZE);
	dpu->cmdlist_item = item;
	for (i = 0; i < DM_PARAM_SIZE / 4; i++)
		item[i] = readl(dpu->regs + DM0 + i * 4);
	/* Self-linked DM node: cmdlist_h_addr and cmdlist_addr0 (DM0 + 0/4). */
	item[0] = upper_32_bits(dpu->cmdlist_dma);
	item[1] = lower_32_bits(dpu->cmdlist_dma);
	item[2] = 0;
	item[3] = 0;

	hdr = dpu->cmdlist_buf;
	/* valid_flag 0xA5, cmd_mode TRANSPORT, task_end, last. */
	hdr[0] = (0xa5U << 24) | BIT(23) | BIT(22) | BIT(1);
	hdr[1] = 0;
	/* dma_dst_addr is a word offset, items_count excludes the header item. */
	hdr[2] = ((DM0 >> 2) << 14) | (DM_PARAM_ITEMS - 1);
	hdr[3] = lower_32_bits(dpu->cmdlist_dma) + CMDLIST_HDR_SIZE;
	wmb();

	/* Vendor dpu_cmdlist_init_ch(): arm every transport channel. */
	writel(readl(dpu->regs + CMDLIST_TWO_SAME_FRAME_BYPASS) | 1,
		dpu->regs + CMDLIST_TWO_SAME_FRAME_BYPASS);
	for (i = 0; i < CMDLIST_SCENE_MAX; i++) {
		writel(0xe4, dpu->regs + CMDLIST_CH_INTE(i));
		writel(readl(dpu->regs + CMDLIST_CH_CTRL(i)) | 1,
			dpu->regs + CMDLIST_CH_CTRL(i));
	}

	/* Online scene keeps the firmware clocks; only the fetch mode changes. */
	writel(readl(dpu->regs + DACC_SCENE0) | 1, dpu->regs + DACC_SCENE0);
	writel(readl(dpu->regs + DACC_SCENE0 + 0x04) | 1, dpu->regs + DACC_SCENE0 + 0x04);
	writel(readl(dpu->regs + DACC_SCENE0 + 0x1c) & ~1U, dpu->regs + DACC_SCENE0 + 0x1c);
	writel(readl(dpu->regs + DACC_SCENE0 + 0x24) | 1, dpu->regs + DACC_SCENE0 + 0x24);

	/* Vendor dpu_cmdlist_init_commit(): run the INITIAL scene once. */
	writel(lower_32_bits(dpu->cmdlist_dma),
		dpu->regs + CMDLIST_CH_STAAD(CMDLIST_SCENE_INIT));
	writel((u32)~BIT(CMDLIST_SCENE_INIT), dpu->regs + CMDLIST_ADDR_MASK_DIS);
	writel(BIT(CMDLIST_SCENE_INIT), dpu->regs + CMDLIST_START);

	/* Hand the online scene the same buffer; it re-runs every frame. */
	writel(0, dpu->regs + DM0);
	writel(lower_32_bits(dpu->cmdlist_dma), dpu->regs + DM0 + 4);
	writel(0, dpu->regs + DACC_GLB_DP0_MASK);
	writel(0, dpu->regs + DACC_GLB_DP1_MASK);
	writel(0, dpu->regs + DACC_GLB_MDP_MASK);
	writel(0, dpu->regs + DACC_GLB_SDP_MASK);
	dev_info(dev, "cmdlist started at %pad (%u items)\n", &dpu->cmdlist_dma,
		DM_PARAM_ITEMS - 1);
	return 0;
}
/*
 * Vendor dkmd_set_reg() encodes a single register write as a CONFIG_MODE
 * cmdlist item and lets the DACC perform the store.  The ITF and pipe-switch
 * blocks ignore CPU stores while the firmware owns the display, so route those
 * writes through the same transport the vendor composer uses.
 */
static int charlotte_regcmd_run(struct charlotte_dpu *dpu, u32 addr, u32 val)
{
	u32 *hdr, *item;
	u32 status;
	int ret = 0;

	if (addr & 3 || addr >= DPU_WINDOW_SIZE)
		return -EINVAL;
	if (!dpu->regcmd_buf) {
		dpu->regcmd_buf = dma_alloc_coherent(dpu->dev, 32,
			&dpu->regcmd_dma, GFP_KERNEL);
		if (!dpu->regcmd_buf)
			return -ENOMEM;
	}
	hdr = dpu->regcmd_buf;
	item = hdr + 4;
	memset(hdr, 0, 32);
	/* exec, last, CONFIG mode, valid_flag 0xA5. */
	hdr[0] = (0xa5U << 24) | BIT(1) | BIT(0);
	hdr[1] = 0;
	hdr[2] = 1;	/* one item, no transport destination */
	hdr[3] = lower_32_bits(dpu->regcmd_dma) + CMDLIST_HDR_SIZE;
	/* addr0 (18 bits, word aligned), data0, one register. */
	item[0] = ((addr >> 2) & 0x3ffff) | 0;
	item[1] = val;
	item[2] = 0;
	item[3] = 0;
	wmb();

	writel(0xffffffff, dpu->regs + CMDLIST0 + 0x1c + 0x40 * CMDLIST_SCENE_INIT);
	writel(lower_32_bits(dpu->regcmd_dma),
		dpu->regs + CMDLIST_CH_STAAD(CMDLIST_SCENE_INIT));
	writel((u32)~BIT(CMDLIST_SCENE_INIT), dpu->regs + CMDLIST_ADDR_MASK_DIS);
	writel(BIT(CMDLIST_SCENE_INIT), dpu->regs + CMDLIST_START);
	udelay(50);
	status = readl(dpu->regs + CMDLIST0 + 0x1c + 0x40 * CMDLIST_SCENE_INIT);
	if (!(status & BIT(7))) {
		dev_warn(dpu->drm.dev, "regcmd %06x=%08x not done, ints=%08x\n",
			addr, val, status);
		ret = -EIO;
	}
	return ret;
}
static ssize_t charlotte_regcmd_write(struct file *file, const char __user *buf,
	size_t len, loff_t *pos)
{
	struct charlotte_dpu *dpu = file->f_inode->i_private;
	char tmp[40];
	unsigned int addr, val;
	int ret;

	if (!len || len >= sizeof(tmp))
		return -EINVAL;
	if (copy_from_user(tmp, buf, len))
		return -EFAULT;
	tmp[len] = '\0';
	if (sscanf(tmp, "%x %x", &addr, &val) != 2)
		return -EINVAL;
	ret = charlotte_regcmd_run(dpu, addr, val);
	return ret ? ret : len;
}
static const struct file_operations charlotte_regcmd_fops = {
	.owner = THIS_MODULE, .open = simple_open, .write = charlotte_regcmd_write,
	.llseek = noop_llseek,
};
static ssize_t charlotte_cmdlist_write(struct file *file, const char __user *buf,
	size_t len, loff_t *pos)
{
	struct charlotte_dpu *dpu = file->f_inode->i_private;
	unsigned int value;
	int ret = kstrtouint_from_user(buf, len, 0, &value);
	if (ret)
		return ret;
	if (value > 1)
		return -EINVAL;
	if (value)
		ret = charlotte_cmdlist_start(dpu);
	else
		charlotte_cmdlist_release(dpu);
	return ret ? ret : len;
}
static int charlotte_cmdlist_show(struct seq_file *s, void *unused)
{
	struct charlotte_dpu *dpu = s->private;
	seq_printf(s, "active=%u dma=%pad dm_cmd0=%08x\n", dpu->cmdlist_buf != NULL,
		&dpu->cmdlist_dma, readl(dpu->regs + DM0 + 4));
	seq_printf(s, "monpc=%08x ch0_ctrl=%08x ch0_status=%08x ch0_curad=%08x\n",
		readl(dpu->regs + 0x40014), readl(dpu->regs + CMDLIST_CH_CTRL(0)),
		readl(dpu->regs + CMDLIST0 + 0x08), readl(dpu->regs + CMDLIST0 + 0x10));
	seq_printf(s, "ch7_ctrl=%08x ch7_status=%08x start=%08x mask_dis=%08x\n",
		readl(dpu->regs + CMDLIST_CH_CTRL(7)),
		readl(dpu->regs + CMDLIST0 + 0x08 + 0x40 * 7),
		readl(dpu->regs + CMDLIST_START),
		readl(dpu->regs + CMDLIST_ADDR_MASK_DIS));
	return 0;
}
static int charlotte_cmdlist_open(struct inode *inode, struct file *file)
{
	return single_open(file, charlotte_cmdlist_show, inode->i_private);
}
static const struct file_operations charlotte_cmdlist_fops = {
	.owner = THIS_MODULE, .open = charlotte_cmdlist_open,
	.write = charlotte_cmdlist_write, .read = seq_read,
	.llseek = seq_lseek, .release = single_release,
};

static ssize_t charlotte_takeover_write(struct file *file, const char __user *buf,
	size_t len, loff_t *pos)
{
	struct charlotte_dpu *dpu = file->private_data;
	unsigned value;
	int ret = kstrtouint_from_user(buf, len, 0, &value);
	if (ret)
		return ret;
	if (value != 1)
		return -EINVAL;
	cancel_delayed_work_sync(&dpu->irq_stop_work);
	mutex_lock(&dpu->diagnostic_lock);
	if (dpu->kms_attempted || dpu->test_active || dpu->irq_running ||
	    dpu->irq_storm || dpu->flips < 2) {
		ret = -EBUSY;
	} else {
		dpu->kms_attempted = true;
		ret = charlotte_kms_init(dpu);
	}
	mutex_unlock(&dpu->diagnostic_lock);
	return ret ? ret : len;
}
static const struct file_operations charlotte_takeover_fops = {
	.owner = THIS_MODULE, .open = simple_open, .write = charlotte_takeover_write,
	.llseek = noop_llseek,
};

/* Called with diagnostic_lock held, outside IRQ context. */
static void charlotte_irq_stop(struct charlotte_dpu *dpu)
{
	if (!dpu->irq_requested)
		return;
	writel(~0U, dpu->regs + DSI_MASK);
	if (dpu->irq_running)
		disable_irq(dpu->irq);
	else
		synchronize_irq(dpu->irq);
	dpu->irq_running = false;
	writel(readl(dpu->regs + DSI_INTS), dpu->regs + DSI_INTS);
	writel(dpu->boot_mask, dpu->regs + DSI_MASK);
}
static void charlotte_irq_stop_work(struct work_struct *work)
{
	struct charlotte_dpu *dpu = container_of(to_delayed_work(work), struct charlotte_dpu, irq_stop_work);
	mutex_lock(&dpu->diagnostic_lock);
	charlotte_irq_stop(dpu);
	mutex_unlock(&dpu->diagnostic_lock);
}
static int charlotte_irq_start(struct charlotte_dpu *dpu)
{
	int ret;
	if (dpu->irq_storm)
		return -EIO;
	if (dpu->irq_running)
		return -EBUSY;
	if (!dpu->irq_requested) {
		ret = devm_request_irq(dpu->drm.dev, dpu->irq, charlotte_irq,
			IRQF_NO_AUTOEN, "charlotte-dpu", dpu);
		if (ret)
			return ret;
		dpu->irq_requested = true;
	}
	/* The handler exists before the peripheral or GIC source is enabled. */
	writel(~0U, dpu->regs + DSI_MASK);
	writel(readl(dpu->regs + DSI_INTS), dpu->regs + DSI_INTS);
	dpu->irq_running = true;
	writel(~(u32)(DSI_VSYNC | DSI_VACT_START | DSI_VACT_END | DSI_UNDERFLOW | DSI_LCD_TE0),
		dpu->regs + DSI_MASK);
	enable_irq(dpu->irq);
	return 0;
}
static ssize_t charlotte_irq_write(struct file *file, const char __user *buf,
	size_t len, loff_t *pos)
{
	struct charlotte_dpu *dpu = file->private_data;
	unsigned value;
	int ret = kstrtouint_from_user(buf, len, 0, &value);
	if (ret)
		return ret;
	if (value > 1 || !probe_only || dpu->kms || dpu->test_active)
		return -EINVAL;
	cancel_delayed_work_sync(&dpu->irq_stop_work);
	mutex_lock(&dpu->diagnostic_lock);
	if (value) {
		/* Arm restoration before any interrupt mutation. */
		schedule_delayed_work(&dpu->irq_stop_work, msecs_to_jiffies(60000));
		ret = charlotte_irq_start(dpu);
	} else {
		charlotte_irq_stop(dpu);
	}
	mutex_unlock(&dpu->diagnostic_lock);
	return ret ? ret : len;
}
static const struct file_operations charlotte_irq_fops = {
	.owner = THIS_MODULE, .open = simple_open, .write = charlotte_irq_write,
	.llseek = noop_llseek,
};

static void charlotte_restore_work(struct work_struct *work)
{
	struct charlotte_dpu *dpu = container_of(to_delayed_work(work), struct charlotte_dpu, restore_work);
	unsigned long flags;
	spin_lock_irqsave(&dpu->lock, flags);
	dpu->test_restoring = true;
	queue_scanout(dpu, ((u64)(dpu->boot_address_h & 0xff) << 32) | dpu->boot_address_l,
		dpu->boot_stride, DRM_FORMAT_XBGR8888);
	spin_unlock_irqrestore(&dpu->lock, flags);
}
static ssize_t charlotte_pattern_write(struct file *file, const char __user *buf,
	size_t len, loff_t *pos)
{
	struct charlotte_dpu *dpu = file->private_data;
	unsigned long flags;
	unsigned value, x, y;
	int ret = kstrtouint_from_user(buf, len, 0, &value);
	if (ret)
		return ret;
	if (value != 1 || !probe_only || dpu->kms || !dpu->irq_running || dpu->irq_storm ||
	    dpu->test_active || dpu->frame_count < 10)
		return -EINVAL;
	if (!dpu->test_vaddr) {
		dpu->test_vaddr = dma_alloc_wc(dpu->dev, TEST_SIZE, &dpu->test_address,
			GFP_KERNEL | __GFP_NOWARN);
		if (!dpu->test_vaddr)
			return -ENOMEM;
	}
	/* Diagnostic only: prepare bands once, then hardware scans this allocation.
	 * Keep it pinned until removal; restoration is scheduled before activation.
	 */
	for (y = 0; y < HEIGHT; y++) {
		u32 *row = (u32 *)((u8 *)dpu->test_vaddr + y * TEST_PITCH);
		for (x = 0; x < WIDTH; x++)
			row[x] = y < HEIGHT / 2 ? 0xff0000ff : 0xffff0000;
	}
	wmb();
	spin_lock_irqsave(&dpu->lock, flags);
	dpu->test_active = true;
	schedule_delayed_work(&dpu->restore_work, msecs_to_jiffies(15000));
	queue_scanout(dpu, dpu->test_address, TEST_PITCH, DRM_FORMAT_XRGB8888);
	spin_unlock_irqrestore(&dpu->lock, flags);
	return len;
}
static const struct file_operations charlotte_pattern_fops = {
	.owner = THIS_MODULE, .open = simple_open, .write = charlotte_pattern_write,
	.llseek = noop_llseek,
};

static const struct drm_display_mode charlotte_mode = {
	.clock = WIDTH * HEIGHT * 60 / 1000,
	.hdisplay = WIDTH, .hsync_start = WIDTH, .hsync_end = WIDTH, .htotal = WIDTH,
	.vdisplay = HEIGHT, .vsync_start = HEIGHT, .vsync_end = HEIGHT, .vtotal = HEIGHT,
	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};
static int charlotte_get_modes(struct drm_connector *connector)
{
	struct drm_display_mode *mode = drm_mode_duplicate(connector->dev, &charlotte_mode);
	if (!mode)
		return 0;
	/* Logical fixed mode; real DSI blanking/timing is retained from firmware. */
	drm_mode_set_name(mode);
	drm_mode_probed_add(connector, mode);
	connector->display_info.width_mm = 173;
	connector->display_info.height_mm = 258;
	return 1;
}
static enum drm_mode_status charlotte_mode_valid(struct drm_crtc *crtc,
	const struct drm_display_mode *mode)
{
	return drm_mode_equal(mode, &charlotte_mode) ? MODE_OK : MODE_BAD;
}
static const struct drm_connector_helper_funcs charlotte_connector_helpers = {
	.get_modes = charlotte_get_modes,
};
static enum drm_connector_status charlotte_detect(struct drm_connector *connector, bool force)
{ return connector_status_connected; }
static const struct drm_connector_funcs charlotte_connector_funcs = {
	.fill_modes = drm_helper_probe_single_connector_modes, .destroy = drm_connector_cleanup,
	.detect = charlotte_detect,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};
static int charlotte_plane_check(struct drm_plane *drm_plane, struct drm_atomic_commit *state)
{
	struct charlotte_dpu *dpu = container_of(drm_plane, struct charlotte_dpu, plane);
	struct drm_plane_state *plane = drm_atomic_get_new_plane_state(state, drm_plane);
	struct drm_crtc_state *crtc;
	struct drm_gem_dma_object *obj;
	u64 end;
	int ret;
	if (!plane->fb)
		return 0;
	if (dpu->irq_storm || !dpu->irq_running)
		return -EIO;
	if (!plane->crtc)
		return -EINVAL;
	crtc = drm_atomic_get_new_crtc_state(state, plane->crtc);
	if (!crtc)
		return -EINVAL;
	ret = drm_atomic_helper_check_plane_state(plane, crtc, DRM_PLANE_NO_SCALING,
		DRM_PLANE_NO_SCALING, false, true);
	if (ret)
		return ret;
	if (plane->src_x || plane->src_y || plane->src_w != WIDTH << 16 ||
		plane->src_h != HEIGHT << 16 || plane->crtc_x || plane->crtc_y ||
		plane->crtc_w != WIDTH || plane->crtc_h != HEIGHT ||
		plane->fb->pitches[0] < WIDTH * 4 || plane->fb->pitches[0] % 16)
		return -EINVAL;
	obj = drm_fb_dma_get_gem_obj(plane->fb, 0);
	end = (u64)plane->fb->offsets[0] + (u64)plane->fb->pitches[0] * HEIGHT;
	if (!obj || end > obj->base.size || obj->dma_addr + end > BIT_ULL(40))
		return -EINVAL;
	return 0;
}
static void charlotte_retire_work(struct work_struct *work)
{
	struct charlotte_dpu *dpu = container_of(work, struct charlotte_dpu, retire_work);
	struct drm_framebuffer *fb;
	unsigned long flags;
	spin_lock_irqsave(&dpu->lock, flags);
	fb = dpu->retire_fb;
	dpu->retire_fb = NULL;
	spin_unlock_irqrestore(&dpu->lock, flags);
	if (fb)
		drm_framebuffer_put(fb);
}
static void charlotte_crtc_flush(struct drm_crtc *crtc, struct drm_atomic_commit *state)
{
	struct charlotte_dpu *dpu = crtc_to_dpu(crtc);
	struct drm_crtc_state *new_crtc = drm_atomic_get_new_crtc_state(state, crtc);
	struct drm_plane_state *plane = drm_atomic_get_new_plane_state(state, &dpu->plane);
	unsigned long flags;
	if (!plane)
		plane = dpu->plane.state;
	if (!new_crtc->active || !plane->fb)
		return;
	/* The previous commit completed at the frame which latched its buffer.
	 * Drain deferred release before handing the IRQ a new retirement slot.
	 */
	flush_work(&dpu->retire_work);
	drm_framebuffer_get(plane->fb);
	spin_lock_irqsave(&dpu->drm.event_lock, flags);
	spin_lock(&dpu->lock);
	dpu->pending_fb = plane->fb;
	reinit_completion(&dpu->flip_done);
	queue_scanout(dpu, drm_fb_dma_get_gem_addr(plane->fb, plane, 0),
		plane->fb->pitches[0], plane->fb->format->format);
	if (new_crtc->event) {
		dpu->event_ref = drm_crtc_vblank_get(crtc) == 0;
		dpu->event = new_crtc->event;
		new_crtc->event = NULL;
	}
	spin_unlock(&dpu->lock);
	spin_unlock_irqrestore(&dpu->drm.event_lock, flags);
}
static void charlotte_crtc_enable(struct drm_crtc *crtc, struct drm_atomic_commit *state)
{
	drm_crtc_vblank_on(crtc);
	charlotte_backlight_power(crtc_to_dpu(crtc), true);
}
static void charlotte_crtc_disable(struct drm_crtc *crtc, struct drm_atomic_commit *state)
{
	struct charlotte_dpu *dpu = crtc_to_dpu(crtc);
	struct drm_crtc_state *new_crtc = drm_atomic_get_new_crtc_state(state, crtc);
	unsigned long flags;
	flush_work(&dpu->retire_work);
	spin_lock_irqsave(&dpu->lock, flags);
	reinit_completion(&dpu->flip_done);
	/* DPMS-off must not reveal the firmware framebuffer/boot console.
	 * Keep an opaque black DMA allocation scanned until the next enable.
	 * Panel power/DSI timing remain retained in this handoff driver.
	 */
	queue_scanout(dpu, dpu->blank_address, TEST_PITCH, DRM_FORMAT_XRGB8888);
	spin_unlock_irqrestore(&dpu->lock, flags);
	if (!wait_for_completion_timeout(&dpu->flip_done, msecs_to_jiffies(100)))
		drm_err(&dpu->drm, "black scanout latch timed out; retaining framebuffer reference\n");
	charlotte_backlight_power(dpu, false);
	drm_crtc_vblank_off(crtc);
	if (new_crtc->event) {
		spin_lock_irqsave(&dpu->drm.event_lock, flags);
		drm_crtc_send_vblank_event(crtc, new_crtc->event);
		new_crtc->event = NULL;
		spin_unlock_irqrestore(&dpu->drm.event_lock, flags);
	}
}
static int charlotte_enable_vblank(struct drm_crtc *crtc) { return 0; }
static void charlotte_disable_vblank(struct drm_crtc *crtc) {}
static const struct drm_crtc_funcs charlotte_crtc_funcs = {
	.reset = drm_atomic_helper_crtc_reset, .destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config, .page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
	.enable_vblank = charlotte_enable_vblank, .disable_vblank = charlotte_disable_vblank,
};
static const struct drm_crtc_helper_funcs charlotte_crtc_helpers = {
	.mode_valid = charlotte_mode_valid, .atomic_enable = charlotte_crtc_enable,
	.atomic_disable = charlotte_crtc_disable, .atomic_flush = charlotte_crtc_flush,
};
static void charlotte_plane_update(struct drm_plane *plane, struct drm_atomic_commit *state)
{ /* The CRTC flush queues address, format and event together. */ }
static const struct drm_plane_funcs charlotte_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane, .destroy = drm_plane_cleanup,
	.reset = drm_atomic_helper_plane_reset,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
};
static const struct drm_plane_helper_funcs charlotte_plane_helpers = {
	.prepare_fb = drm_gem_plane_helper_prepare_fb, .atomic_check = charlotte_plane_check,
	.atomic_update = charlotte_plane_update,
};
static void charlotte_commit_tail(struct drm_atomic_commit *state)
{
	struct drm_device *dev = state->dev;
	drm_atomic_helper_commit_modeset_disables(dev, state);
	drm_atomic_helper_commit_modeset_enables(dev, state);
	drm_atomic_helper_commit_planes(dev, state, DRM_PLANE_COMMIT_ACTIVE_ONLY);
	drm_atomic_helper_fake_vblank(state);
	drm_atomic_helper_commit_hw_done(state);
	/* A generic next vblank can precede our VACT_END latch. Wait for the
	 * event from the actual latched frame before releasing old planes.
	 */
	drm_atomic_helper_wait_for_flip_done(dev, state);
	drm_atomic_helper_cleanup_planes(dev, state);
}
static const struct drm_mode_config_helper_funcs charlotte_mode_helpers = {
	.atomic_commit_tail = charlotte_commit_tail,
};
static const struct drm_mode_config_funcs charlotte_mode_funcs = {
	.fb_create = drm_gem_fb_create, .atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};
static const struct drm_encoder_funcs charlotte_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};
static int charlotte_kms_init(struct charlotte_dpu *dpu)
{
	struct drm_device *drm = &dpu->drm;
	static const u32 formats[] = { DRM_FORMAT_XRGB8888 };
	static const u64 modifiers[] = { DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_MOD_INVALID };
	int ret;
	ret = charlotte_backlight_init(dpu);
	if (ret)
		return dev_err_probe(drm->dev, ret, "backlight supplier unavailable\n");
	dpu->blank_vaddr = dma_alloc_wc(dpu->dev, TEST_SIZE, &dpu->blank_address,
		GFP_KERNEL | __GFP_NOWARN);
	if (!dpu->blank_vaddr)
		return -ENOMEM;
	memset(dpu->blank_vaddr, 0, TEST_SIZE);
	wmb();
	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;
	drm->mode_config.funcs = &charlotte_mode_funcs;
	drm->mode_config.helper_private = &charlotte_mode_helpers;
	drm->mode_config.min_width = drm->mode_config.max_width = WIDTH;
	drm->mode_config.min_height = drm->mode_config.max_height = HEIGHT;
	drm->mode_config.preferred_depth = 24;
	ret = drm_universal_plane_init(drm, &dpu->plane, 1, &charlotte_plane_funcs,
		formats, ARRAY_SIZE(formats), modifiers, DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		return ret;
	drm_plane_helper_add(&dpu->plane, &charlotte_plane_helpers);
	ret = drm_crtc_init_with_planes(drm, &dpu->crtc, &dpu->plane, NULL, &charlotte_crtc_funcs, NULL);
	if (ret)
		return ret;
	drm_crtc_helper_add(&dpu->crtc, &charlotte_crtc_helpers);
	ret = drm_encoder_init(drm, &dpu->encoder, &charlotte_encoder_funcs, DRM_MODE_ENCODER_DSI, NULL);
	if (ret)
		return ret;
	dpu->encoder.possible_crtcs = 1;
	ret = drm_connector_init(drm, &dpu->connector, &charlotte_connector_funcs, DRM_MODE_CONNECTOR_DSI);
	if (ret)
		return ret;
	drm_connector_helper_add(&dpu->connector, &charlotte_connector_helpers);
	ret = drm_connector_attach_encoder(&dpu->connector, &dpu->encoder);
	if (ret)
		return ret;
	ret = drm_vblank_init(drm, 1);
	if (ret)
		return ret;
	drm_mode_config_reset(drm);
	ret = charlotte_irq_start(dpu);
	if (ret)
		return ret;
	dpu->kms = true;
	ret = aperture_remove_all_conflicting_devices(charlotte_drm_driver.name);
	if (ret)
		return ret;
	ret = drm_dev_register(drm, 0);
	if (ret)
		return ret;
	drm_client_setup(drm, NULL);
	dev_info(drm->dev, "native DMA scanout KMS registered; retaining firmware DSI/DSC timing\n");
	return 0;
}
DEFINE_DRM_GEM_DMA_FOPS(charlotte_fops);
static const struct drm_driver charlotte_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	DRM_GEM_DMA_DRIVER_OPS,
	DRM_FBDEV_DMA_DRIVER_OPS,
	.fops = &charlotte_fops, .name = "charlotte-dpu", .desc = "Charlotte v700 DPU KMS handoff",
	.major = 0, .minor = 1,
};
static void charlotte_stop(void *data)
{
	struct charlotte_dpu *dpu = data;
	bool restored = false;
	cancel_delayed_work_sync(&dpu->restore_work);
	cancel_delayed_work_sync(&dpu->irq_stop_work);
	mutex_lock(&dpu->diagnostic_lock);
	if (dpu->irq_running && (dpu->test_active || dpu->kms)) {
		unsigned long flags;
		flush_work(&dpu->retire_work);
		spin_lock_irqsave(&dpu->lock, flags);
		reinit_completion(&dpu->flip_done);
		queue_scanout(dpu, ((u64)(dpu->boot_address_h & 0xff) << 32) | dpu->boot_address_l,
			dpu->boot_stride, DRM_FORMAT_XBGR8888);
		spin_unlock_irqrestore(&dpu->lock, flags);
		if (wait_for_completion_timeout(&dpu->flip_done, msecs_to_jiffies(100))) {
			restored = true;
			dpu->test_active = false;
		}
	}
	charlotte_irq_stop(dpu);
	mutex_unlock(&dpu->diagnostic_lock);
	flush_work(&dpu->retire_work);
	debugfs_remove_recursive(dpu->debugfs);
	if (dpu->test_vaddr && !dpu->test_active)
		dma_free_wc(dpu->dev, TEST_SIZE, dpu->test_vaddr, dpu->test_address);
	/* A stopped/storming IRQ does not prove the black buffer is no longer
	 * scanned. Keep it pinned if firmware restoration was not confirmed.
	 */
	if (dpu->blank_vaddr && restored)
		dma_free_wc(dpu->dev, TEST_SIZE, dpu->blank_vaddr, dpu->blank_address);
}
static int charlotte_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct charlotte_dpu *dpu;
	u32 size, select, dm0_58, dmc0_24;
	int ret;
	dpu = devm_drm_dev_alloc(dev, &charlotte_drm_driver, struct charlotte_dpu, drm);
	if (IS_ERR(dpu))
		return PTR_ERR(dpu);
	platform_set_drvdata(pdev, dpu);
	dpu->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(dpu->regs))
		return PTR_ERR(dpu->regs);
	/* No SMMU/clock/secure-register probing: only confirmed active DPU RAM. */
	size = readl(dpu->regs + DM_LAYER_SIZE);
	select = readl(dpu->regs + DM_LAYER_SELECT);
	dm0_58 = readl(dpu->regs + DM0 + 0x58);
	dmc0_24 = readl(dpu->regs + DMC0 + 0x24);
	dev_info(dev, "handoff signature size=0x%08x select=0x%08x dm0_58=0x%08x dmc0_24=0x%08x address=0x%08x%08x stride=0x%08x ov_fmt=0x%08x\n",
		size, select, dm0_58, dmc0_24,
		readl(dpu->regs + DM_ADDRESS_H), readl(dpu->regs + DM_ADDRESS_L),
		readl(dpu->regs + DM_STRIDE), readl(dpu->regs + DM_LAYER_OV_FMT));
	if (size != ((HEIGHT - 1) << 16 | (WIDTH - 1)) || select != 0x02000d00 ||
		dm0_58 != 0x01010001 || dmc0_24 != 0) {
		if (strict_handoff)
			return dev_err_probe(dev, -ENODEV, "unsupported firmware handoff configuration\n");
		dev_warn(dev, "firmware signature differs from the TGR handoff state; taking over anyway\n");
	}
	dpu->boot_address_h = readl(dpu->regs + DM_ADDRESS_H);
	dpu->boot_address_l = readl(dpu->regs + DM_ADDRESS_L);
	dpu->boot_stride = readl(dpu->regs + DM_STRIDE);
	dpu->boot_mask = readl(dpu->regs + DSI_MASK);
	dpu->boot_select = select;
	dpu->boot_ov_fmt = readl(dpu->regs + DM_LAYER_OV_FMT);
	spin_lock_init(&dpu->lock);
	mutex_init(&dpu->diagnostic_lock);
	INIT_DELAYED_WORK(&dpu->restore_work, charlotte_restore_work);
	INIT_DELAYED_WORK(&dpu->irq_stop_work, charlotte_irq_stop_work);
	INIT_WORK(&dpu->retire_work, charlotte_retire_work);
	init_completion(&dpu->flip_done);
	dpu->dev = dev;
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(40));
	if (ret)
		return ret;
	dma_set_max_seg_size(dev, UINT_MAX);
	ret = of_reserved_mem_device_init(dev);
	if (ret && ret != -ENODEV)
		return ret;
	dpu->irq = platform_get_irq(pdev, 0);
	if (dpu->irq < 0)
		return dpu->irq;
	ret = devm_add_action_or_reset(dev, charlotte_stop, dpu);
	if (ret)
		return ret;
	dpu->debugfs = debugfs_create_dir("charlotte-dpu", NULL);
	debugfs_create_file("status", 0400, dpu->debugfs, dpu, &charlotte_status_fops);
	debugfs_create_file("reg", 0600, dpu->debugfs, dpu, &charlotte_reg_fops);
	debugfs_create_file("dacc", 0200, dpu->debugfs, dpu, &charlotte_dacc_fops);
	debugfs_create_file("cmdl", 0600, dpu->debugfs, dpu, &charlotte_cmdlist_fops);
	debugfs_create_file("daccw", 0200, dpu->debugfs, dpu, &charlotte_regcmd_fops);
	if (probe_only) {
		debugfs_create_file("irq_test", 0200, dpu->debugfs, dpu, &charlotte_irq_fops);
		debugfs_create_file("pattern_test", 0200, dpu->debugfs, dpu, &charlotte_pattern_fops);
		debugfs_create_file("takeover", 0200, dpu->debugfs, dpu, &charlotte_takeover_fops);
		dev_info(dev, "read-only handoff probe: %ux%u pitch=%u IRQ=%d; KMS retained by simpledrm\n",
			WIDTH, HEIGHT, dpu->boot_stride, dpu->irq);
		return 0;
	}
	dpu->kms_attempted = true;
	return charlotte_kms_init(dpu);
}
static void charlotte_remove(struct platform_device *pdev)
{
	struct charlotte_dpu *dpu = platform_get_drvdata(pdev);
	if (dpu->kms) {
		drm_dev_unregister(&dpu->drm);
		drm_atomic_helper_shutdown(&dpu->drm);
	}
	charlotte_cmdlist_release(dpu);
	of_reserved_mem_device_release(&pdev->dev);
}
static void charlotte_shutdown(struct platform_device *pdev)
{
	struct charlotte_dpu *dpu = platform_get_drvdata(pdev);
	if (dpu->kms)
		drm_atomic_helper_shutdown(&dpu->drm);
	charlotte_stop(dpu);
}
static const struct of_device_id charlotte_match[] = {
	{ .compatible = "hisilicon,charlotte-dpu-handoff" }, {}
};
MODULE_DEVICE_TABLE(of, charlotte_match);
static struct platform_driver charlotte_platform_driver = {
	.probe = charlotte_probe, .remove = charlotte_remove, .shutdown = charlotte_shutdown,
	.driver = { .name = "charlotte-dpu", .of_match_table = charlotte_match,
		.suppress_bind_attrs = true },
};
static int __init charlotte_init(void)
{
	int ret;
	/* DT creates the simple-framebuffer device independently of sysfb.
	 * Let its driver bind first, then remove that device at KMS takeover;
	 * otherwise it can bind after us and expose the same panel twice.
	 */
	ret = i2c_add_driver(&charlotte_backlight_driver);
	if (ret)
		return ret;
	ret = platform_driver_register(&charlotte_platform_driver);
	if (ret)
		i2c_del_driver(&charlotte_backlight_driver);
	return ret;
}
#ifdef MODULE
module_init(charlotte_init);
#else
late_initcall_sync(charlotte_init);
#endif
static void __exit charlotte_exit(void)
{
	platform_driver_unregister(&charlotte_platform_driver);
	i2c_del_driver(&charlotte_backlight_driver);
}
module_exit(charlotte_exit);
MODULE_DESCRIPTION("Charlotte v700 DPU atomic KMS firmware handoff");
MODULE_LICENSE("GPL");
