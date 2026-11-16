// SPDX-License-Identifier: GPL-2.0-only
/* Charlotte DPU v700: atomic DMA-buffer scanout using the firmware's
 * single-layer scene/DSC/dual-DSI configuration. No framebuffer CPU blit.
 * Register layout checked against the B236 GPL definitions and live handoff.
 * Cold panel initialization, alternate modes and runtime PM are not provided.
 */
#include <linux/aperture.h>
#include <linux/arm-smccc.h>
#include <linux/arm_ffa.h>
#include <linux/firmware/charlotte-ffa.h>
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
#include <drm/drm_blend.h>
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
#include "charlotte_vendor_init.h"

#define DSI_INTS 0x248
#define DSI_MASK 0x24c
/* Phase A pristine snapshot: DSI framing state and DACC core state. */
#define DSI_MODE_CTRL 0x008
#define DSI_EDPI_CMD_SIZE 0x040
#define DSI_CMD_MODE_CTRL 0x044
/* 0x1c0 is CMD_MOD_CTRL (bit1 = dsi_halt_video_en), distinct from the
 * CMD_MODE_CTRL frame-format register at 0x044.
 */
#define DSI_CMD_MOD_CTRL 0x1c0
#define DSI_VSTATE 0x1fc
#define DSI_VINACT 0x1dc
#define DACC_CTRL 0x40000
#define DACC_INTMSK 0x40004
#define DACC_CFG 0x4000c
#define DACC_STATUS 0x40010
#define DACC_MONPC 0x40014
/*
 * BL31 display-channel firmware load: make_fid(FID_DISPLAY_GROUP=0x37, 0x05)
 * == FID_BL31_DISPLAY_CH_DEFAUL_SEC_CONFIG, called by the vendor composer as
 * dpu_dacc_load().  Without it the DACC core stays parked in WFI (mon_pc
 * constant) and never executes the DM cmdlist, so no frame is produced.
 */
#define CHARLOTTE_FID_DACC_LOAD 0xc3003705UL
/* Command-mode frame pump: vendor enable_dsi_ldi() writes both per TE. */
#define DSI_LDI_CTRL 0x1b8
#define DSI_LDI_FRM_MSK 0x1d4
#define DSI_LDI_FRM_MSK_UP 0x238
/* Frame-valid gate: with bit 29 clear the LDI keeps scanning but the DSI drops
 * every pixel, so a command-mode panel only refreshes its own GRAM.
 */
#define DSI_FRM_VALID_DBG 0x1d8
#define DSI_FRM_VALID_GATE BIT(29)
#define DSI_VSYNC BIT(4)
#define DSI_VACT_START BIT(7)
#define DSI_VACT_END BIT(8)
#define DSI_UNDERFLOW BIT(2)
#define DSI_LCD_TE0 BIT(12)
/* Vendor MIPI APB low-power command channel (mipi_dsi_host.c).  The payload is
 * staged four bytes per word in APB_WR_LP_PLD_DATA; storing dtype/vc/wc into
 * APB_WR_LP_HDR issues the packet.  CMD_PLD_BUF_STATUS reports FIFO occupancy
 * with 1 meaning empty: bit 0 gen_cmd_empty, bit 2 gen_pld_w_empty, so both set
 * means the host has taken the packet.  Bit 4 is gen_pld_r_empty, i.e. the
 * read-reply buffer, and is unrelated to a write.
 */
#define DSI_APB_WR_LP_HDR 0x00c
#define DSI_APB_WR_LP_PLD 0x010
#define DSI_CMD_PLD_BUF_STATUS 0x014
#define DSI_PLD_STATUS_TX_IDLE (BIT(0) | BIT(2))
#define DTYPE_DCS_LWRITE 0x39
/* Online scene control: latching the DM shadow window into the active scene. */
#define DACC_SCENE0_RELOAD 0x40408
/* DSC0 pipeline counters (soc_dpu_interface.h DSC_ST_*). */
#define DSC0_ST_DATAIN 0x088
#define DSC0_ST_DATAOUT 0x08c
#define DSC0_ST_FIFO 0x0a0
#define DSC0_ST_LINEBUF 0x0a8
#define ITF0_STATE 0x064
/* DSI video-mode timing block (mipi v700).  A video panel free-runs from these
 * instead of the TE-paced EDPI path.  The bootloader already programmed the
 * timing for this panel, so the videomode diagnostic keeps those values and
 * only seeds the transport-control fields the cmd-mode skip left at default.
 */
#define DSI_VIDEO_MODE_CTRL 0x050
#define DSI_VIDEO_MODE_LP_CMD_TIMING 0x03c
#define DSI_VIDEO_PKT_LEN 0x054
/* mipi_dsi_config_dpi_interface(): the DPI pin config that the command-mode
 * skip in mipi_dsi_on_sub2() never programs.  A video panel needs all three.
 */
#define DSI_VIDEO_VCID 0x030
#define DSI_VIDEO_COLOR_FORMAT 0x034
#define DSI_VIDEO_POL_CTRL 0x038
/* POL_CTRL bit1/bit2 = vsync/hsync active low, matching the vendor union
 * initialiser (dataen 0, vsync 1, hsync 1, shutd 0, colorm 0).
 */
#define DSI_VIDEO_POL_VENDOR 0x6
/* lcd-kit,mipi-burst-mode = 2 (burst with sync events). */
#define DSI_VIDEO_BURST_MODE 0x2
/* lcd-kit,mipi-color-mode = 0x0f, the RGB888 DPI packing this panel uses. */
#define DSI_VIDEO_COLOR_MODE 0xf
#define DSI_VIDEO_HSA 0x060
#define DSI_VIDEO_HBP 0x064
#define DSI_VIDEO_HLINE 0x068
#define DSI_VIDEO_VSA 0x06c
#define DSI_VIDEO_VBP 0x070
#define DSI_VIDEO_VFP 0x074
#define DSI_VIDEO_VACT 0x078
#define DSI_LP_CLK_CTRL 0x080
#define DSI_TO_TIME_CTRL 0x0f0
#define DSI_LDI_DPI0_HRZ_CTRL2 0x1ec
#define DSI_LDI_VRT_CTRL2 0x1f0
#define DSI_LDI_DPI0_HRZ_CTRL3 0x1f4
#define DSI_VCOUNT 0x204
#define DSI_HCOUNT 0x208
/* 0x110 is VIDEO_MODE_CTRL_ACT, not vactive; vactive lives in LDI_VRT_CTRL2. */
/* Vendor dpu_comp_clear_fastboot_config(): the bootloader freezes the pipeline
 * with REG_CTRL_DEBUG bit10 on the active pipe and the vendor kernel clears it
 * once it takes over.  Do the same so the pipeline actually advances.
 */
#define DPP0_REG_CTRL_DEBUG 0xb4064
#define DSC0_REG_CTRL_DEBUG 0xd4258
#define ITF0_REG_CTRL_DEBUG 0xcc060
/* Online DPP0 -> DSC0 -> ITF0 pipeline.  The bootloader leaves these modules
 * frozen with REG_CTRL_DEBUG bit10 and the DSI-facing ITF sized to 0, so the
 * panel only ever shows the frame still held in its GRAM.  Enabling the three
 * operators and routing DPP0 to ITF0 turns the handoff layer into a live scan
 * source.  Offsets match the B236 soc_dpu_interface.h definitions:
 *   DPP_REG_CTRL 0x58 / FLUSH_EN 0x60 / GMP_EN 0x9a0 / CLIP_EN 0x1610
 *   DSC_REG_CTRL 0x250 / FLUSH_EN 0x254
 *   ITF_CH_IMG_SIZE 0x00 / DPP_CLIP_EN 0x14 / DPP_CLRBAR_CTRL 0x30 /
 *   REG_CTRL 0x58 / FLUSH_EN 0x5c / ITFSW_DATA_SEL 0x68
 *   PIPE_SW SIG_CTRL 0x00 / SIG_EN 0x04 / DAT_CTRL 0x08 / DAT_EN 0x0c
 */
#define DPP0_OFFSET 0xb4000
#define DSC0_OFFSET 0xd4000
#define ITF0_OFFSET 0xcc000
#define PIPE_SW_OFFSET 0xcc400
/* Per-frame transport and output counters.  DPU_DPP_CRC_FRM_CNT (0x34) only
 * advances while DPU_DPP_CRC_CFG_EN (0x30 bit0) is set, so it is the one
 * register that proves the DPP actually walked a frame instead of holding the
 * bootloader picture in its line buffer.
 */
#define DPP0_CH_DBG_CNT (DPP0_OFFSET + 0x02c)
#define DPP0_CRC_CFG_EN (DPP0_OFFSET + 0x030)
#define DPP0_CRC_FRM_CNT (DPP0_OFFSET + 0x034)
/* Backdrop painted when no layer covers a pixel; a uniform panel with no
 * layer data looks exactly like this colour.
 */
#define DPP0_CLRBAR_CTRL (DPP0_OFFSET + 0x014)
/* Vendor get_dsc_out_width(): two slices of half the panel line plus the
 * four insert bytes each, mapped onto a 3x compressed 24bpp stream.
 */
#define DSC_CHUNK_SIZE (WIDTH / 2)
#define DSC_OUT_WIDTH ((DSC_CHUNK_SIZE + 4) * 2 / 3)
/* Vendor dpu_comp_itfch_config(): ITF consumes the DSC output geometry. */
#define ITF_IMG_SIZE (((HEIGHT - 1) << 16) | (DSC_OUT_WIDTH - 1))
#define CHARLOTTE_ONLINE_SNAP_MAX 40
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
/* struct dpu_dm_cmdlist_addr: 17 client pointers plus 3 reserved words. */
#define DM_CMDLIST_ADDR_SIZE 0x50
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
/* Write-1-to-clear: taskdone(7) timeout(6) badcmd(5) axi_err(2) alldone(1)
 * onedone(0).  INTE is 0xe4, so only the error and taskdone bits are enabled.
 */
#define CMDLIST_CH_INTS(i) (CMDLIST0 + 0x1c + 0x40 * (i))
#define CMDLIST_INTS_TASKDONE BIT(7)
#define CMDLIST_INTS_TIMEOUT BIT(6)
#define CMDLIST_INTS_BADCMD BIT(5)
#define CMDLIST_INTS_AXI_ERR BIT(2)
#define CMDLIST_START (CMDLIST0 + 0x714)
#define CMDLIST_ADDR_MASK_DIS (CMDLIST0 + 0x71c)
#define CMDLIST_TWO_SAME_FRAME_BYPASS (CMDLIST0 + 0x7a0)
#define CMDLIST_SCENE_MAX 9
#define CMDLIST_SCENE_INIT 7
/* CMD block debug/state: SCE0 exposes the scene engine FSM and whether its
 * descriptor fifo is empty, so it shows whether anything ever got fetched.
 */
#define CMDLIST_DBG (CMDLIST0 + 0x700)
#define CMDLIST_BUF_DBG_CNT_CLR (CMDLIST0 + 0x708)
#define CMDLIST_BUF_DBG_CNT (CMDLIST0 + 0x70c)
#define CMDLIST_ADDR_MASK_STATUS (CMDLIST0 + 0x720)
#define CMDLIST_TASK_STATUS (CMDLIST0 + 0x728)
#define CMDLIST_SYNC_MODE (CMDLIST0 + 0x74c)
#define CMDLIST_SCE0_CFG_SIGNAL (CMDLIST0 + 0x780)
/* DACC scene 0 command interfaces: the RISC-V side dispatches through these. */
#define DACC_SCENE0_SW_START (DACC_SCENE0 + 0x10)
#define DACC_SCENE0_ST_FRM_SEL (DACC_SCENE0 + 0x1c)
#define DACC_SCENE0_CFG_MODE (DACC_SCENE0 + 0x24)
#define DACC_SCENE0_SCMD_STATUS (DACC_SCENE0 + 0x6c)
#define DACC_SCENE0_DCMD_STATUS (DACC_SCENE0 + 0x78)
#define DACC_SCENE0_LAYER_NUM (DACC_SCENE0 + 0x7c)
#define DACC_GLB_DP0_STATUS (DACC_GLB0 + 0x240)
#define DACC_GLB_DP0_MASK (DACC_GLB0 + 0x244)
#define DACC_GLB_DP1_STATUS (DACC_GLB0 + 0x248)
#define DACC_GLB_DP1_MASK (DACC_GLB0 + 0x24c)
#define DACC_GLB_MDP_STATUS (DACC_GLB0 + 0x250)
#define DACC_GLB_MDP_MASK (DACC_GLB0 + 0x254)
#define DACC_GLB_SDP_STATUS (DACC_GLB0 + 0x258)
#define DACC_GLB_SDP_MASK (DACC_GLB0 + 0x25c)
/* Firmware handoff proves the panel and layer are 2800x1840 landscape:
 * DM layer height 0x072f0aef packs width-1 in the low word, stride 11200 is
 * 2800*4, DSC PIC_SIZE is 0x0aef072f, and DSI vactive reads 1840.
 */
#define WIDTH 2800
#define HEIGHT 1840
#define TEST_PITCH (WIDTH * 4)
#define TEST_SIZE PAGE_ALIGN(TEST_PITCH * HEIGHT)

/*
 * The stock firmware routes display security calls through the FF-A
 * "platdrv" secure partition instead of the raw BL31 SMC dispatcher
 * (CONFIG_DPU_FFA_SUPPORT=y).  The raw CHARLOTTE_FID_DACC_LOAD SMC comes back
 * with the FID echoed, i.e. an unsupported-function return.  That partition
 * has a single owner, drivers/firmware/charlotte-ffa.c, so the request goes
 * through its shared platform ABI rather than a second binding here.
 */

/* The bootloader's command line is not under our control and shipped a
 * stale probe_only=1, which silently kept simpledrm in charge. Keep the
 * parameter so that argument stays accepted, but let the device tree make
 * the decision: without "hisilicon,probe-only" the driver takes over.
 */
static bool probe_only;
module_param(probe_only, bool, 0444);
MODULE_PARM_DESC(probe_only, "Keep simpledrm and expose bounded handoff diagnostics instead of native KMS (device tree: hisilicon,probe-only)");
static bool strict_handoff;
module_param(strict_handoff, bool, 0444);
MODULE_PARM_DESC(strict_handoff, "Refuse to take over when the firmware register signature differs from the TGR one");

/* One coherent snapshot of the DPP0->DSC0->ITF0->DSI pipeline counters. */
struct charlotte_diag_sample {
	u32 itf_size, itf_state, dsc_in, dsc_out, dsc_fifo, dsc_lb;
	u32 ldi_h2, ldi_v2, vstate, pld_st;
	u64 frames, te;
};

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
	/* Serializes the MIPI APB low-power command channel only. */
	struct mutex dcs_lock;
	struct i2c_client *backlight;
	bool backlight_on;
	bool backlight_ro;
	int irq;
	bool irq_requested, irq_running, irq_storm;
	/* The starved-pipeline storm is muted rather than fatal: the pump is
	 * what actually moves pixels, so it has to keep running.
	 */
	bool underflow_muted;
	u64 irq_count, irq_window_ns;
	u32 irq_window_count;
	bool kms, kms_attempted, pending, latched;
	bool ldi_pump;
	u32 pump_div;
	/* V0 experiment: reversible DSI command->video transport flip. */
	bool video_active;
	u32 video_saved_mode, video_saved_cmdmod, video_saved_frmmsk;
	u32 video_saved_vidmode, video_saved_lpcmd, video_saved_totime;
	u32 video_saved_ldi_h2, video_saved_ldi_v2, video_saved_cmdmodctl;
	u32 video_saved_ldi_ctrl, video_saved_lpclk;
	u32 video_saved_hsa, video_saved_hbp, video_saved_hline;
	u32 video_saved_vsa, video_saved_vbp, video_saved_vact;
	u32 video_saved_pktlen, video_saved_ldi_h3;
	u32 video_saved_vfp;
	u32 video_saved_vcid, video_saved_colorfmt, video_saved_polctrl;
	u32 video_saved_dpp_dbg, video_saved_dsc_dbg, video_saved_itf_dbg;
	u32 boot_address_h, boot_address_l, boot_stride, boot_mask;
	u32 boot_select, boot_ov_fmt;
	u32 boot_vfp;
	u32 boot_h2, boot_v2, boot_vact;
	u32 boot_dpp[5];
	u32 boot_dm[3];
	/* Pipeline counters taken before this driver writes anything, at probe
	 * and again at KMS init.  Two samples tell whether the firmware's own
	 * scanout was still producing pixels when we took over.
	 */
	struct charlotte_diag_sample boot_pipe, pre_kms_pipe;
	u32 *dm_backup;
	void *cmdlist_buf;
	dma_addr_t cmdlist_dma;
	u32 *cmdlist_item;
	void *regcmd_buf;
	dma_addr_t regcmd_dma;
	void *seq_buf;
	dma_addr_t seq_dma;
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
	/* Online DPP0->DSC0->ITF0 continuous-scanout diagnostic. */
	bool online_active;
	bool online_saved_pump;
	u32 online_snap_n;
	u32 online_snap_off[CHARLOTTE_ONLINE_SNAP_MAX];
	u32 online_snap_val[CHARLOTTE_ONLINE_SNAP_MAX];
};
static const struct drm_driver charlotte_drm_driver;
static int charlotte_dm_snapshot(struct charlotte_dpu *dpu);
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

static bool charlotte_hw_format(u32 format, u32 *hw)
{
	/* DPU_SDMA_STATIC_FORMAT values from soc_dpu_format.h. */
	switch (format) {
	case DRM_FORMAT_RGB565:   *hw = 0x0; return true;
	case DRM_FORMAT_XRGB8888: *hw = 0x5; return true;
	case DRM_FORMAT_ARGB8888: *hw = 0x6; return true;
	case DRM_FORMAT_XBGR8888: *hw = 0xc; return true;
	case DRM_FORMAT_ABGR8888: *hw = 0xd; return true;
	}
	return false;
}

/*
 * Point the online scene at the cmdlist chain and put it in cmdlist config
 * mode.  The vendor reissues this on every present because the sequencer
 * clears CFG_MODE once the chain has run, so this is not a one-shot setup.
 */
static void charlotte_dacc_arm(struct charlotte_dpu *dpu)
{
	if (!dpu->cmdlist_buf)
		return;
	writel(lower_32_bits(dpu->cmdlist_dma), dpu->regs + DM0 + 4);
	writel(readl(dpu->regs + DACC_SCENE0) | 1, dpu->regs + DACC_SCENE0);
	writel(readl(dpu->regs + DACC_SCENE0 + 0x04) | 1,
	       dpu->regs + DACC_SCENE0 + 0x04);
	writel(readl(dpu->regs + DACC_SCENE0 + 0x24) | 1,
	       dpu->regs + DACC_SCENE0 + 0x24);
	/* Online scenes take their frame select from the panel, not SW_START. */
	writel(readl(dpu->regs + DACC_SCENE0 + 0x1c) & ~1U,
	       dpu->regs + DACC_SCENE0 + 0x1c);
	wmb();
}

static void charlotte_cmdlist_patch(struct charlotte_dpu *dpu, dma_addr_t addr,
				    u32 pitch, u32 format)
{
	u32 hw_format;

	if (!dpu->cmdlist_item)
		return;
	if (!charlotte_hw_format(format, &hw_format))
		hw_format = 0xd;
	dpu->cmdlist_item[DM_W_START_ADDR_H] = upper_32_bits(addr);
	dpu->cmdlist_item[DM_W_START_ADDR_L] = lower_32_bits(addr);
	dpu->cmdlist_item[DM_W_STRIDE] = pitch;
	dpu->cmdlist_item[DM_W_LAYER_HEIGHT] = ((HEIGHT - 1) << 16) | (WIDTH - 1);
	dpu->cmdlist_item[DM_W_DMA_SEL] = (dpu->cmdlist_item[DM_W_DMA_SEL] & ~0xff00U) |
		hw_format << 8;
	wmb();
}

static void program_scanout(struct charlotte_dpu *dpu, dma_addr_t addr, u32 pitch, u32 format)
{
	u32 layer_size = ((HEIGHT - 1) << 16) | (WIDTH - 1);
	u32 hw_format;
	if (!charlotte_hw_format(format, &hw_format))
		hw_format = 0xd;
	charlotte_cmdlist_patch(dpu, addr, pitch, format);
	/* GPL opr_set_sdma_layer_fmt sets both the DMA and dynamic DFC formats.
	 * Firmware uses ABGR (13) for the logo; opaque DRM buffers map to the
	 * matching SDMA format so 565/8888/RGB order stay correct.
	 */
	writel((dpu->boot_select & ~0xff00U) | hw_format << 8, dpu->regs + DM_LAYER_SELECT);
	writel((dpu->boot_ov_fmt & ~0xffU) | hw_format, dpu->regs + DM_LAYER_OV_FMT);
	writel(upper_32_bits(addr) & 0xff, dpu->regs + DM_ADDRESS_H);
	writel(lower_32_bits(addr), dpu->regs + DM_ADDRESS_L);
	writel(pitch, dpu->regs + DM_STRIDE);
	writel(layer_size, dpu->regs + DM_LAYER_SIZE);
	wmb();
	/* composer_scene_present(): hand the scene the chain for this frame. */
	charlotte_dacc_arm(dpu);
}
static void queue_scanout(struct charlotte_dpu *dpu, dma_addr_t addr, u32 pitch, u32 format)
{
	dpu->next_address = addr;
	dpu->next_stride = pitch;
	dpu->next_format = format;
	dpu->pending = true;
}
/* Command-mode frame pump.  The frame-valid bit gates LDI pixels into the DSI
 * and single_frm_update is edge triggered: a write of 1 while the bit already
 * reads 1 starts no new transfer, so the bit has to be toggled for every
 * transfer that is wanted.  One transfer per latched commit is what the panel
 * needs; toggling on every TE aborts the previous transfer mid-frame.
 */
static void charlotte_ldi_arm(struct charlotte_dpu *dpu)
{
	writel(readl(dpu->regs + DSI_FRM_VALID_DBG) | DSI_FRM_VALID_GATE,
		dpu->regs + DSI_FRM_VALID_DBG);
	writel(readl(dpu->regs + DSI_LDI_CTRL) | BIT(0), dpu->regs + DSI_LDI_CTRL);
	writel(0, dpu->regs + DSI_LDI_FRM_MSK_UP);
	writel(1, dpu->regs + DSI_LDI_FRM_MSK_UP);
}
static irqreturn_t charlotte_irq(int irq, void *data)
{
	struct charlotte_dpu *dpu = data;
	u32 raw = readl(dpu->regs + DSI_INTS);
	u32 status = raw & ~readl(dpu->regs + DSI_MASK);
	struct drm_pending_vblank_event *event = NULL;
	bool event_ref = false;
	bool arm_now = false;
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
		dpu->irq_window_ns = now;
		dpu->irq_window_count = 0;
		if (!dpu->underflow_muted && (readl(dpu->regs + DSI_MASK) &
					      DSI_UNDERFLOW) == 0) {
			/* First flood: mute the starved-FIFO source and stay
			 * alive, otherwise a stalled pipeline permanently
			 * kills the frame pump it needs to recover.
			 */
			writel(readl(dpu->regs + DSI_MASK) | DSI_UNDERFLOW,
				dpu->regs + DSI_MASK);
			dpu->underflow_muted = true;
			dpu->irq_storm = true;
			return IRQ_HANDLED;
		}
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
		/* The DSC serves frames from the last latched descriptor, so the
		 * chain has to be flushed again for the panel to fetch the
		 * buffer: without this, fbdev/fbcon writes and any commit that
		 * does not change the address stay invisible. Flush every frame
		 * while we own the pipeline, after any latch above.
		 */
		if (dpu->kms || dpu->online_active)
			writel(1, dpu->regs + DSC0_OFFSET + 0x0254);
		if (dpu->latched) {
			dpu->latched = false;
			/* One transfer for this freshly latched descriptor. */
			arm_now = true;
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
	if ((dpu->ldi_pump || dpu->pump_div) && (status & DSI_LCD_TE0) &&
	    (!dpu->pump_div || !(dpu->te_count % dpu->pump_div)))
		arm_now = true;
	if (arm_now)
		charlotte_ldi_arm(dpu);
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
	seq_printf(s, "kms=%u irq=%d irq_running=%u irq_storm=%u underflow_muted=%u ldi_pump=%u pump_div=%u irq_count=%llu frames=%llu vsyncs=%llu te=%llu period_ns=%llu underflows=%llu flips=%llu\n",
		dpu->kms, dpu->irq, dpu->irq_running, dpu->irq_storm,
		dpu->underflow_muted, dpu->ldi_pump, dpu->pump_div,
		dpu->irq_count, dpu->frame_count, dpu->vsync_count,
		dpu->te_count, dpu->frame_period_ns, dpu->underflows, dpu->flips);
	seq_printf(s, "address=%02x%08x stride=%u pending=%u test_active=%u test_restoring=%u\n",
		readl(dpu->regs + DM_ADDRESS_H) & 0xff,
		readl(dpu->regs + DM_ADDRESS_L), readl(dpu->regs + DM_STRIDE),
		dpu->pending, dpu->test_active, dpu->test_restoring);
	seq_printf(s, "boot_address=%02x%08x boot_stride=%u test_address=%pad\n",
		dpu->boot_address_h & 0xff, dpu->boot_address_l,
		dpu->boot_stride, &dpu->test_address);
	spin_unlock_irqrestore(&dpu->lock, flags);
	/* Read framing/core state without the spinlock: MMIO reads only, and this
	 * keeps the interrupt path untouched while collecting a snapshot.
	 * VSTATE: 0x1 idle, 0x2 vsw, 0x8 vactive0, 0x80 v_wait_te0.
	 */
	seq_printf(s, "snapshot dsi_mode=%08x cmdmod=%08x edpi=%08x vstate=%08x vinact=%08x\n",
		readl(dpu->regs + DSI_MODE_CTRL), readl(dpu->regs + DSI_CMD_MODE_CTRL),
		readl(dpu->regs + DSI_EDPI_CMD_SIZE), readl(dpu->regs + DSI_VSTATE),
		readl(dpu->regs + DSI_VINACT));
	seq_printf(s, "snapshot dacc_int=%08x intmsk=%08x cfg=%08x status=%08x monpc=%08x\n",
		readl(dpu->regs + DACC_CTRL), readl(dpu->regs + DACC_INTMSK),
		readl(dpu->regs + DACC_CFG), readl(dpu->regs + DACC_STATUS),
		readl(dpu->regs + DACC_MONPC));
	seq_printf(s, "snapshot sc0_clksel=%08x clken=%08x swstart=%08x stfrm=%08x cfgmode=%08x\n",
		readl(dpu->regs + DACC_SCENE0), readl(dpu->regs + DACC_SCENE0 + 0x04),
		readl(dpu->regs + DACC_SCENE0 + 0x10), readl(dpu->regs + DACC_SCENE0 + 0x1c),
		readl(dpu->regs + DACC_SCENE0 + 0x24));
	seq_printf(s, "snapshot cmdlist dm_cmd4=%08x dm0_w2=%08x s0_ints=%02x init_ints=%02x\n",
		readl(dpu->regs + DM0 + 4), readl(dpu->regs + DM0 + 8),
		readl(dpu->regs + CMDLIST_CH_INTS(0)),
		readl(dpu->regs + CMDLIST_CH_INTS(CMDLIST_SCENE_INIT)));
	seq_printf(s, "snapshot cmd sce0=%08x dbg=%08x sync=%08x taskst=%08x maskst=%08x bufdbg=%08x\n",
		readl(dpu->regs + CMDLIST_SCE0_CFG_SIGNAL),
		readl(dpu->regs + CMDLIST_DBG),
		readl(dpu->regs + CMDLIST_SYNC_MODE),
		readl(dpu->regs + CMDLIST_TASK_STATUS),
		readl(dpu->regs + CMDLIST_ADDR_MASK_STATUS),
		readl(dpu->regs + CMDLIST_BUF_DBG_CNT));
	seq_printf(s, "snapshot dacc scmd_st=%08x dcmd_st=%08x layers=%08x reload=%08x\n",
		readl(dpu->regs + DACC_SCENE0_SCMD_STATUS),
		readl(dpu->regs + DACC_SCENE0_DCMD_STATUS),
		readl(dpu->regs + DACC_SCENE0_LAYER_NUM),
		readl(dpu->regs + DACC_SCENE0_RELOAD));
	seq_printf(s, "snapshot dpp dbgcnt=%04x crc=%06x ctrl=%08x flush=%08x state=%08x\n",
		readl(dpu->regs + DPP0_CH_DBG_CNT) & 0xffff,
		readl(dpu->regs + DPP0_CRC_FRM_CNT),
		readl(dpu->regs + DPP0_OFFSET + 0x058),
		readl(dpu->regs + DPP0_OFFSET + 0x060),
		readl(dpu->regs + DPP0_OFFSET + 0x068));
	seq_printf(s, "pipeline itf=%08x state=%08x dsc_in=%08x out=%08x fifo=%08x lb=%08x\n",
		readl(dpu->regs + ITF0_OFFSET + 0x000),
		readl(dpu->regs + ITF0_OFFSET + ITF0_STATE),
		readl(dpu->regs + DSC0_OFFSET + DSC0_ST_DATAIN),
		readl(dpu->regs + DSC0_OFFSET + DSC0_ST_DATAOUT),
		readl(dpu->regs + DSC0_OFFSET + DSC0_ST_FIFO),
		readl(dpu->regs + DSC0_OFFSET + DSC0_ST_LINEBUF));
	seq_printf(s, "ldi h2=%08x v2=%08x vact=%08x vfp=%08x dsc_out_w=%u\n",
		readl(dpu->regs + DSI_LDI_DPI0_HRZ_CTRL2),
		readl(dpu->regs + DSI_LDI_VRT_CTRL2),
		readl(dpu->regs + DSI_VIDEO_VACT),
		readl(dpu->regs + DSI_VIDEO_VFP), DSC_OUT_WIDTH);
	seq_printf(s, "video active=%u mode=%08x vidctl=%08x pktlen=%u hsa=%u hbp=%u hline=%u vsa=%u vbp=%u vfp=%u vact=%u pol=%02x fmt=%02x vcount=%u hcount=%u\n",
		dpu->video_active, readl(dpu->regs + DSI_MODE_CTRL),
		readl(dpu->regs + DSI_VIDEO_MODE_CTRL),
		readl(dpu->regs + DSI_VIDEO_PKT_LEN),
		readl(dpu->regs + DSI_VIDEO_HSA), readl(dpu->regs + DSI_VIDEO_HBP),
		readl(dpu->regs + DSI_VIDEO_HLINE), readl(dpu->regs + DSI_VIDEO_VSA),
		readl(dpu->regs + DSI_VIDEO_VBP), readl(dpu->regs + DSI_VIDEO_VFP),
		readl(dpu->regs + DSI_VIDEO_VACT),
		readl(dpu->regs + DSI_VIDEO_POL_CTRL),
		readl(dpu->regs + DSI_VIDEO_COLOR_FORMAT),
		readl(dpu->regs + DSI_VCOUNT), readl(dpu->regs + DSI_HCOUNT));
	seq_printf(s, "boot_ldi h2=%08x v2=%08x vact=%08x vfp=%08x pld_st=%08x\n",
		dpu->boot_h2, dpu->boot_v2, dpu->boot_vact, dpu->boot_vfp,
		readl(dpu->regs + DSI_CMD_PLD_BUF_STATUS));
	seq_printf(s, "boot_dpp clken=%08x ctrl=%08x state=%08x gmp=%08x clrbar=%08x\n",
		dpu->boot_dpp[0], dpu->boot_dpp[1], dpu->boot_dpp[2],
		dpu->boot_dpp[3], dpu->boot_dpp[4]);
	seq_printf(s, "boot_dm num=%08x scene=%08x ovwidth=%08x\n",
		dpu->boot_dm[0], dpu->boot_dm[1], dpu->boot_dm[2]);
	seq_printf(s, "boot_pipe itf=%08x st=%08x dscin=%08x dscout=%08x fifo=%08x lb=%08x vstate=%08x pld=%08x\n",
		dpu->boot_pipe.itf_size, dpu->boot_pipe.itf_state,
		dpu->boot_pipe.dsc_in, dpu->boot_pipe.dsc_out,
		dpu->boot_pipe.dsc_fifo, dpu->boot_pipe.dsc_lb,
		dpu->boot_pipe.vstate, dpu->boot_pipe.pld_st);
	seq_printf(s, "pre_kms itf=%08x st=%08x dscin=%08x dscout=%08x fifo=%08x lb=%08x vstate=%08x pld=%08x\n",
		dpu->pre_kms_pipe.itf_size, dpu->pre_kms_pipe.itf_state,
		dpu->pre_kms_pipe.dsc_in, dpu->pre_kms_pipe.dsc_out,
		dpu->pre_kms_pipe.dsc_fifo, dpu->pre_kms_pipe.dsc_lb,
		dpu->pre_kms_pipe.vstate, dpu->pre_kms_pipe.pld_st);
	seq_printf(s, "dsc pic=%08x slice=%08x chunk=%08x out=%08x\n",
		readl(dpu->regs + DSC0_OFFSET + 0x010),
		readl(dpu->regs + DSC0_OFFSET + 0x014),
		readl(dpu->regs + DSC0_OFFSET + 0x018),
		readl(dpu->regs + DSC0_OFFSET + 0x008));
	seq_printf(s, "backlight_on=%u\n", READ_ONCE(dpu->backlight_on));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(charlotte_status);

struct charlotte_reg_desc {
	const char *name;
	u32 off;
};
static const struct charlotte_reg_desc charlotte_regs[] = {
	{ "dsi0_pm", 0x00198 }, { "dsi0_ldi", 0x001b8 }, { "dsi0_cmdmodctl", 0x001c0 },
	{ "dsi0_mode", 0x00008 }, { "dsi0_edpi", 0x00040 }, { "dsi0_cmdmod", 0x00044 },
	{ "dsi0_vidmode", 0x00050 }, { "dsi0_pktlen", 0x00054 },
	{ "dsi0_hsa", 0x00060 }, { "dsi0_hbp", 0x00064 }, { "dsi0_hline", 0x00068 },
	{ "dsi0_vsa", 0x0006c }, { "dsi0_vbp", 0x00070 }, { "dsi0_vfp", 0x00074 },
	{ "dsi0_vactnum", 0x00078 }, { "dsi0_lpclk", 0x00080 },
	{ "dsi0_lpcmd", 0x0003c }, { "dsi0_totime", 0x000f0 },
	{ "dsi0_ldi_h2", 0x001ec }, { "dsi0_ldi_v2", 0x001f0 }, { "dsi0_ldi_h3", 0x001f4 },
	{ "dsi0_vcount", 0x00204 }, { "dsi0_hcount", 0x00208 },
	{ "dsi0_te", 0x001c4 }, { "dsi0_frmmsk", 0x001d4 }, { "dsi0_frmvalid", 0x001d8 },
	{ "dsi0_vinact", 0x001dc }, { "dsi0_vstate", 0x001fc }, { "dsi0_ints", 0x00248 },
	{ "dsi0_mask", 0x0024c },
	{ "dsc_en", 0xd4008 }, { "dsc_ctrl", 0xd400c }, { "dsc_pic", 0xd4010 },
	{ "dsc_slice", 0xd4014 }, { "dsc_multi", 0xd4074 }, { "dsc_outctl", 0xd4078 },
	{ "dsc_dbg", 0xd4258 },
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
	{ "glb_pm", 0x13108 }, { "glb_rsclear", 0x13120 }, { "glb_ch0ken", 0x13128 },
	{ "glb_ch1ken", 0x13130 }, { "glb_avhrken", 0x13138 }, { "glb_dispken", 0x13140 },
	{ "glb_rst", 0x13144 }, { "glb_dfsok", 0x13200 },
	{ "dpp0_int", 0xb4000 }, { "dpp0_clksel", 0xb4008 }, { "dpp0_clken", 0xb400c },
	{ "dpp0_clrbar", 0xb4010 }, { "dpp0_ctrl", 0xb4058 }, { "dpp0_flush", 0xb4060 },
	{ "dpp0_dbg", 0xb4064 }, { "dpp0_state", 0xb4068 },
	{ "dpp0_gmp", 0xb49a0 }, { "dpp0_clip", 0xb5610 },
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
	{ "dsc0_ctrl", 0xd4250 }, { "dsc0_flush", 0xd4254 },
	{ "itf0_size", 0xcc000 }, { "itf0_clipen", 0xcc014 },
	{ "itf0_clrbar", 0xcc030 },
	{ "itf0_ctrl", 0xcc058 }, { "itf0_flush", 0xcc05c }, { "itf0_dbg", 0xcc060 },
	{ "itf0_datasel", 0xcc068 },
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
	/* Detach the DACC before the buffer disappears: a stale descriptor
	 * would make the sequencer read freed DMA memory on the next frame.
	 */
	writel(0, dpu->regs + DM0 + 4);
	wmb();
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
	if (charlotte_dm_snapshot(dpu))
		return -ENOMEM;
	dpu->cmdlist_buf = dma_alloc_coherent(dev, size, &dpu->cmdlist_dma, GFP_KERNEL);
	if (!dpu->cmdlist_buf)
		return -ENOMEM;
	memset(dpu->cmdlist_buf, 0, size);
	item = (u32 *)((u8 *)dpu->cmdlist_buf + CMDLIST_HDR_SIZE);
	dpu->cmdlist_item = item;
	/*
	 * Build the payload from the pristine probe-time image.  Re-reading the
	 * live window would fold in every later register poke, and the whole
	 * point of the transport is to feed the sequencer the descriptor the
	 * firmware itself validated.
	 */
	memcpy(item, dpu->dm_backup, DM_PARAM_SIZE);
	/* request_dm_client() blanks every cmdlist client slot to 0xFF (absent)
	 * before filling in only the DM chain pointer.  A zero slot reads as
	 * "follow chain at address 0", which stalls the sequencer.
	 */
	memset(item, 0xFF, DM_CMDLIST_ADDR_SIZE);
	item[0] = 0;
	item[1] = lower_32_bits(dpu->cmdlist_dma);
	/* The transport owns the DM window from now on, so the payload has to
	 * carry the buffer we are actually scanning out.
	 */
	charlotte_cmdlist_patch(dpu,
		dpu->next_address ? dpu->next_address : dpu->boot_address_l,
		dpu->next_stride ? dpu->next_stride : dpu->boot_stride,
		dpu->next_format);

	hdr = dpu->cmdlist_buf;
	/* valid_flag 0xA5, cmd_mode TRANSPORT, task_end, last. */
	hdr[0] = (0xa5U << 24) | BIT(23) | BIT(22) | BIT(1);
	hdr[1] = 0;
	/* dma_dst_addr is a word offset.  Vendor cmdlist_alloc_items() derives
	 * items_count as node_size / 16 - 1, where node_size covers the header
	 * plus the payload, so the count equals the payload item count.
	 */
	hdr[2] = ((DM0 >> 2) << 14) | DM_PARAM_ITEMS;
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
	charlotte_dacc_arm(dpu);

	/* Vendor dpu_cmdlist_init_commit(): run the INITIAL scene once. */
	writel(lower_32_bits(dpu->cmdlist_dma),
		dpu->regs + CMDLIST_CH_STAAD(CMDLIST_SCENE_INIT));
	writel((u32)~BIT(CMDLIST_SCENE_INIT), dpu->regs + CMDLIST_ADDR_MASK_DIS);
	writel(BIT(CMDLIST_SCENE_INIT), dpu->regs + CMDLIST_START);

	/* Hand the online scene the same buffer; it re-runs every frame. */
	writel(0, dpu->regs + DM0);
	writel(0, dpu->regs + DACC_GLB_DP0_MASK);
	writel(0, dpu->regs + DACC_GLB_DP1_MASK);
	writel(0, dpu->regs + DACC_GLB_MDP_MASK);
	writel(0, dpu->regs + DACC_GLB_SDP_MASK);
	charlotte_dacc_arm(dpu);
	dev_info(dev, "cmdlist started at %pad (%u items)\n", &dpu->cmdlist_dma,
		DM_PARAM_ITEMS);
	return 0;
}
/*
 * Vendor dkmd_set_reg() encodes register writes as CONFIG_MODE cmdlist items
 * and lets the DACC perform the store.  The ITF and pipe-switch blocks ignore
 * CPU stores while the firmware owns the display, so route those writes
 * through the same transport the vendor composer uses.
 */
#define CHARLOTTE_SEQ_MAX 128
static int charlotte_regseq_run(struct charlotte_dpu *dpu,
				const struct charlotte_reg_init *seq, size_t n)
{
	size_t items;
	u32 *hdr, *item;
	u32 status = 0;
	int ret = 0, i;

	if (!n || n > CHARLOTTE_SEQ_MAX)
		return -EINVAL;
	/*
	 * The vendor composer never emits a CONFIG_MODE node with a single
	 * item: the DACC then raises onedone but no taskdone.  Duplicate it.
	 */
	items = (n == 1) ? 2 : n;
	if (!dpu->seq_buf) {
		dpu->seq_buf = dma_alloc_coherent(dpu->dev,
			CMDLIST_HDR_SIZE + CHARLOTTE_SEQ_MAX * CMDLIST_ITEM_SIZE,
			&dpu->seq_dma, GFP_KERNEL);
		if (!dpu->seq_buf)
			return -ENOMEM;
	}
	hdr = dpu->seq_buf;
	item = hdr + 4;
	memset(hdr, 0, CMDLIST_HDR_SIZE + items * CMDLIST_ITEM_SIZE);
	/* last, task_end, CONFIG mode, valid_flag 0xA5.  exec stays 0. */
	hdr[0] = (0xa5U << 24) | BIT(22) | BIT(1);
	hdr[1] = 0;
	hdr[2] = items;	/* items_count, no transport destination */
	hdr[3] = lower_32_bits(dpu->seq_dma) + CMDLIST_HDR_SIZE;
	/* cmd_item: addr0 (18 bits, word aligned), data0, one register. */
	for (i = 0; i < items; i++) {
		const struct charlotte_reg_init *e = &seq[i % n];
		item[i * 4 + 0] = (e->addr >> 2) & 0x3ffff;
		item[i * 4 + 1] = e->val;
		item[i * 4 + 2] = 0;
		item[i * 4 + 3] = 0;
	}
	wmb();

	writel(0xe4, dpu->regs + CMDLIST_CH_INTE(CMDLIST_SCENE_INIT));
	writel(readl(dpu->regs + CMDLIST_CH_CTRL(CMDLIST_SCENE_INIT)) | 1,
		dpu->regs + CMDLIST_CH_CTRL(CMDLIST_SCENE_INIT));
	writel(0xffffffff, dpu->regs + CMDLIST0 + 0x1c + 0x40 * CMDLIST_SCENE_INIT);
	writel(lower_32_bits(dpu->seq_dma),
		dpu->regs + CMDLIST_CH_STAAD(CMDLIST_SCENE_INIT));
	writel((u32)~BIT(CMDLIST_SCENE_INIT), dpu->regs + CMDLIST_ADDR_MASK_DIS);
	writel(BIT(CMDLIST_SCENE_INIT), dpu->regs + CMDLIST_START);
	for (i = 0; i < 1000; i++) {
		status = readl(dpu->regs + CMDLIST0 + 0x1c +
			0x40 * CMDLIST_SCENE_INIT);
		if (status & BIT(7))
			break;
		udelay(50);
	}
	if (!(status & BIT(7))) {
		dev_warn(dpu->drm.dev,
			"regseq %zu items (%06x=%08x) not done, ints=%08x\n",
			n, seq[0].addr, seq[0].val, status);
		ret = -EIO;
	}
	return ret;
}

/*
 * Everything below exists because the DACC never dispatches a per-frame
 * command on this board: DACC_SCENE0_DCMD_STATUS stays 0 while the static
 * command bit is set, so the layer descriptor sitting in the DM window is
 * never transferred into the DPP.  The cmdlist engine itself works, so the
 * plan is to replace the DACC as the frame engine with the engine we already
 * have: one CONFIG_MODE chain that transfers the descriptor and then flushes
 * the DPP -> DSC -> ITF shadow registers.
 *
 * Vendor never touches these blocks with writel(): while the display is
 * firmware-owned the CPU stores are dropped, so the sequence runs through
 * charlotte_regseq_run().
 */
static const struct charlotte_reg_init charlotte_seq_dpp[] = {
	{ DPP0_OFFSET + 0x058, 0x00000000, "dpp0: REG_CTRL scene0" },
	{ DPP0_OFFSET + 0x06c, 0x00000000, "dpp0: REG_CTRL_LAYER_ID" },
	{ DPP0_OFFSET + 0x060, 0x00000001, "dpp0: FLUSH_EN" },
};

static const struct charlotte_reg_init charlotte_seq_dsc[] = {
	{ DSC0_OFFSET + 0x250, 0x00000000, "dsc0: REG_CTRL scene0" },
	{ DSC0_OFFSET + 0x254, 0x00000001, "dsc0: FLUSH_EN" },
};

static const struct charlotte_reg_init charlotte_seq_itf2[] = {
	{ ITF0_OFFSET + 0x000, ITF_IMG_SIZE, "itf0: IMG_SIZE (DSC out)" },
	{ ITF0_OFFSET + 0x014, 0x00000000, "itf0: DPP_CLIP_EN" },
	{ ITF0_OFFSET + 0x030, 0x00000000, "itf0: DPP_CLRBAR_CTRL" },
	{ ITF0_OFFSET + 0x068, 0x00000000, "itf0: ITFSW_DATA_SEL" },
	{ ITF0_OFFSET + 0x074, 0x000001a1, "itf0: VSYNC_DELAY_CNT_CTRL" },
	{ ITF0_OFFSET + 0x058, 0x00000000, "itf0: REG_CTRL scene0" },
	{ ITF0_OFFSET + 0x05c, 0x00000001, "itf0: FLUSH_EN" },
};

static const struct charlotte_reg_init charlotte_seq_pipesw[] = {
	{ PIPE_SW_OFFSET + 0x00, 0x00000001, "pipe_sw: SIG_CTRL ch0" },
	{ PIPE_SW_OFFSET + 0x04, 0x00000001, "pipe_sw: SIG_EN ch0" },
	{ PIPE_SW_OFFSET + 0x08, 0x00000001, "pipe_sw: DAT_CTRL ch0" },
	{ PIPE_SW_OFFSET + 0x0c, 0x00000001, "pipe_sw: DAT_EN ch0" },
};

static const struct charlotte_reg_init charlotte_seq_sdma[] = {
	{ DACC_GLB0 + 0x538, 0x80090001, "sdma0: dbg reserved1" },
	{ DACC_GLB0 + 0x578, 0x80090001, "sdma1: dbg reserved1" },
	{ DACC_GLB0 + 0x5b8, 0x80090001, "sdma2: dbg reserved1" },
	{ DACC_GLB0 + 0x5f8, 0x80090001, "sdma3: dbg reserved1" },
};

/*
 * Re-run the transport chain the DACC already handed to the INIT scene.  The
 * chain address lives in dpu->cmdlist_dma, so this is a pure "one more frame"
 * request that never rebuilds the payload.
 */
static int charlotte_chain_kick(struct charlotte_dpu *dpu)
{
	unsigned int i;
	u32 status = 0;

	if (!dpu->cmdlist_buf)
		return -ENODEV;
	writel(0xffffffff, dpu->regs + CMDLIST_CH_INTS(CMDLIST_SCENE_INIT));
	wmb();
	writel(lower_32_bits(dpu->cmdlist_dma),
		dpu->regs + CMDLIST_CH_STAAD(CMDLIST_SCENE_INIT));
	writel((u32)~BIT(CMDLIST_SCENE_INIT), dpu->regs + CMDLIST_ADDR_MASK_DIS);
	writel(BIT(CMDLIST_SCENE_INIT), dpu->regs + CMDLIST_START);
	for (i = 0; i < 200; i++) {
		status = readl(dpu->regs + CMDLIST_CH_INTS(CMDLIST_SCENE_INIT));
		if (status & CMDLIST_INTS_TASKDONE)
			return 0;
		udelay(50);
	}
	return -EIO;
}

static void charlotte_bypass_sample(struct charlotte_dpu *dpu, const char *stage)
{
	dev_info(dpu->drm.dev,
		 "bypass: %-20s dsc_in=%04x out=%04x fifo=%05x dppcnt=%04x crc=%06x crcen=%u dppst=%08x clrbar=%08x itfst=%08x vstate=%08x\n",
		 stage,
		 readl(dpu->regs + DSC0_OFFSET + DSC0_ST_DATAIN) & 0x1fff,
		 readl(dpu->regs + DSC0_OFFSET + DSC0_ST_DATAOUT) & 0x1fff,
		 readl(dpu->regs + DSC0_OFFSET + DSC0_ST_FIFO),
		 readl(dpu->regs + DPP0_CH_DBG_CNT) & 0xffff,
		 readl(dpu->regs + DPP0_CRC_FRM_CNT),
		 readl(dpu->regs + DPP0_CRC_CFG_EN) & 1,
		 readl(dpu->regs + DPP0_OFFSET + 0x068),
		 readl(dpu->regs + DPP0_CLRBAR_CTRL),
		 readl(dpu->regs + ITF0_OFFSET + ITF0_STATE),
		 readl(dpu->regs + DSI_VSTATE));
}

/*
 * Stage one block at a time so a single dead block shows up as "the counters
 * did not move here" instead of a single opaque failure.  Every stage is a
 * separate CONFIG_MODE chain, which also proves the chain engine survives
 * repeated use.
 */
static int charlotte_bypass_run(struct charlotte_dpu *dpu, bool push_frames)
{
	unsigned int i;
	int ret;

	/* Arm the DPP frame counter first: without CRC_CFG_EN the counter stays
	 * at zero no matter how many frames the DPP walks, which is exactly the
	 * ambiguity that made the previous run unreadable.
	 */
	writel(readl(dpu->regs + DPP0_CRC_CFG_EN) | BIT(0),
	       dpu->regs + DPP0_CRC_CFG_EN);
	charlotte_cmdlist_release(dpu);
	ret = charlotte_cmdlist_start(dpu);
	if (ret)
		return ret;
	charlotte_bypass_sample(dpu, "b0 transport");

	ret = charlotte_regseq_run(dpu, charlotte_seq_dpp,
				   ARRAY_SIZE(charlotte_seq_dpp));
	charlotte_bypass_sample(dpu, ret ? "b1 dpp (failed)" : "b1 +dpp flush");

	ret = charlotte_regseq_run(dpu, charlotte_seq_dsc,
				   ARRAY_SIZE(charlotte_seq_dsc));
	charlotte_bypass_sample(dpu, ret ? "b2 dsc (failed)" : "b2 +dsc flush");

	ret = charlotte_regseq_run(dpu, charlotte_seq_itf2,
				   ARRAY_SIZE(charlotte_seq_itf2));
	charlotte_bypass_sample(dpu, ret ? "b3 itf (failed)" : "b3 +itf latch");

	ret = charlotte_regseq_run(dpu, charlotte_seq_pipesw,
				   ARRAY_SIZE(charlotte_seq_pipesw));
	charlotte_bypass_sample(dpu, ret ? "b4 sw (failed)" : "b4 +pipe_sw route");

	ret = charlotte_regseq_run(dpu, charlotte_seq_sdma,
				   ARRAY_SIZE(charlotte_seq_sdma));
	charlotte_bypass_sample(dpu, ret ? "b5 sdma (failed)" : "b5 +sdma dbg");

	if (!push_frames)
		return 0;

	/* The DACC used to deliver one descriptor per frame; deliver 200. */
	for (i = 0; i < 200; i++) {
		if (charlotte_chain_kick(dpu))
			break;
	}
	charlotte_bypass_sample(dpu, "b6 200x transport");

	/* Real per-frame shape: transfer then latch then push. */
	charlotte_chain_kick(dpu);
	charlotte_regseq_run(dpu, charlotte_seq_dpp,
			     ARRAY_SIZE(charlotte_seq_dpp));
	charlotte_regseq_run(dpu, charlotte_seq_dsc,
			     ARRAY_SIZE(charlotte_seq_dsc));
	charlotte_regseq_run(dpu, charlotte_seq_itf2,
			     ARRAY_SIZE(charlotte_seq_itf2));
	charlotte_ldi_arm(dpu);
	msleep(40);
	charlotte_bypass_sample(dpu, "b7 frame push");
	return 0;
}

static ssize_t charlotte_bypass_write(struct file *file,
	const char __user *buf, size_t len, loff_t *pos)
{
	struct charlotte_dpu *dpu = file->private_data;
	unsigned int value;
	int ret;

	ret = kstrtouint_from_user(buf, len, 0, &value);
	if (ret)
		return ret;
	if (value != 1)
		return -EINVAL;
	mutex_lock(&dpu->diagnostic_lock);
	ret = charlotte_bypass_run(dpu, true);
	mutex_unlock(&dpu->diagnostic_lock);
	dev_info(dpu->drm.dev, "bypass run: %d\n", ret);
	return ret ? ret : len;
}

static const struct file_operations charlotte_bypass_fops = {
	.owner = THIS_MODULE, .open = simple_open,
	.write = charlotte_bypass_write, .llseek = noop_llseek,
};

/*
 * Vendor dpu_dacc_load(): ask BL31 to load the DACC firmware for display
 * channel 0 (configure_dss_service_security(DSS_CH_DEFAULT_SEC_CONFIG, 0, 0,
 * BIG_DPU) -> arm_smccc_smc(func_id, channel, mode, dpu_index, ...)).  Log the
 * SMC status together with the core state so the caller can see whether the
 * core left WFI; a status of -1 means BL31 has no display handler (FID not
 * supported) and is harmless.
 */
static void charlotte_dacc_load(struct charlotte_dpu *dpu)
{
	struct arm_smccc_res res;

	arm_smccc_smc(CHARLOTTE_FID_DACC_LOAD, 0, 0, 0, 0, 0, 0, 0, &res);
	dev_info(dpu->dev,
		"dacc_load smc ret=%d a1=%lu a2=%lu a3=%lu status=%08x monpc=%08x vstate=%08x\n",
		(int)res.a0, (unsigned long)res.a1, (unsigned long)res.a2,
		(unsigned long)res.a3, readl(dpu->regs + DACC_STATUS),
		readl(dpu->regs + DACC_MONPC), readl(dpu->regs + DSI_VSTATE));
}

/* Same load request, routed through the platdrv secure partition via FF-A. */
static void charlotte_dacc_ffa_load(struct charlotte_dpu *dpu)
{
	struct ffa_send_direct_data reply = { };
	u32 status = readl(dpu->regs + DACC_STATUS);
	u32 monpc = readl(dpu->regs + DACC_MONPC);
	int ret;

#if IS_ENABLED(CONFIG_CHARLOTTE_FFA)
	ret = charlotte_ffa_dss_sec_config(dpu->dev, CHARLOTTE_FID_DACC_LOAD,
					   0, 0, 0, &reply);
	if (ret)
		dev_warn(dpu->dev,
			 "dacc_load ffa: platform service failed (%d)\n", ret);
#else
	ret = -ENOTSUPP;
	dev_warn(dpu->dev, "dacc_load ffa: CONFIG_CHARLOTTE_FFA unavailable\n");
#endif
	dev_info(dpu->dev,
		 "dacc_load ffa ret=%d d0=%08lx d1=%08lx d2=%08lx d3=%08lx d4=%08lx status=%08x->%08x monpc=%08x->%08x vstate=%08x\n",
		 ret, reply.data0, reply.data1, reply.data2, reply.data3,
		 reply.data4, status, readl(dpu->regs + DACC_STATUS), monpc,
		 readl(dpu->regs + DACC_MONPC), readl(dpu->regs + DSI_VSTATE));
}

static int charlotte_regcmd_run(struct charlotte_dpu *dpu, u32 addr, u32 val)
{
	const struct charlotte_reg_init e = { addr, val, NULL };

	if (addr & 3 || addr >= DPU_WINDOW_SIZE)
		return -EINVAL;
	return charlotte_regseq_run(dpu, &e, 1);
}

struct charlotte_named_seq {
	const char *name;
	const struct charlotte_reg_init *seq;
	size_t n;
};
static const struct charlotte_named_seq charlotte_vendor_seqs[] = {
	{ "clocks", charlotte_seq_clocks, ARRAY_SIZE(charlotte_seq_clocks) },
	{ "memory", charlotte_seq_memory, ARRAY_SIZE(charlotte_seq_memory) },
	{ "itf", charlotte_seq_itf, ARRAY_SIZE(charlotte_seq_itf) },
};
static ssize_t charlotte_vendorseq_write(struct file *file,
	const char __user *buf, size_t len, loff_t *pos)
{
	struct charlotte_dpu *dpu = file->private_data;
	char tmp[16];
	size_t i;
	int ret = 0;
	bool ran = false;

	if (!len || len >= sizeof(tmp))
		return -EINVAL;
	if (copy_from_user(tmp, buf, len))
		return -EFAULT;
	tmp[len] = '\0';
	if (tmp[len - 1] == '\n')
		tmp[len - 1] = '\0';

	/* Vendor dpu_comp_init() order: clocks, memory, then ITF/pipe switch. */
	for (i = 0; i < ARRAY_SIZE(charlotte_vendor_seqs); i++) {
		const struct charlotte_named_seq *s = &charlotte_vendor_seqs[i];
		if (strcmp(tmp, "all") && strcmp(tmp, s->name))
			continue;
		ret = charlotte_regseq_run(dpu, s->seq, s->n);
		dev_info(dpu->drm.dev, "vendor seq %s (%zu writes): %d\n",
			s->name, s->n, ret);
		ran = true;
		if (ret)
			break;
	}
	if (!ran)
		return -EINVAL;
	return ret ? ret : len;
}
static const struct file_operations charlotte_vendorseq_fops = {
	.owner = THIS_MODULE, .open = simple_open,
	.write = charlotte_vendorseq_write, .llseek = noop_llseek,
};
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

/*
 * Unified bring-up console.
 *
 * Every path that can put pixels on this panel is reachable from debugfs on a
 * normal KMS boot, so one image can be probed without rebuilding:
 *
 *   dcs <hex bytes>        raw DCS long write over the APB LP channel
 *   region full|<x0 x1 y0 y1>
 *                          panel-side window (DCS 0x2A/0x2B, five-byte writes)
 *   window <w> <h>         LDI scan window registers (1ec/1f0/74/78)
 *   present <mode>         0 trigger only, 1 panel window, 2 vendor ITF seq,
 *                          3 vendor DM transport cmdlist, 4 DM window patch,
 *                          5 all of them, 8 restore the LDI window,
 *                          9 restore the DM snapshot
 *   dmparam                dump the whole dpu_dm_param window
 *   selftest               run every present mode and print counters
 *
 * The vendor MIPI host stages payload words first and issues the packet by
 * writing the header, with no pre-write busy check; the FIFO-empty bits are
 * only polled afterwards as a completion hint.
 */
static int charlotte_dsi_lwrite(struct charlotte_dpu *dpu, u8 dtype, u8 vc,
				const u8 *data, u32 len)
{
	u32 hdr, pld, status;
	u32 i, j;

	if (!len || len > 64)
		return -EINVAL;
	for (i = 0; i < len; i += 4) {
		pld = 0;
		for (j = 0; j < 4 && i + j < len; j++)
			pld |= (u32)data[i + j] << (8 * j);
		writel(pld, dpu->regs + DSI_APB_WR_LP_PLD);
	}
	hdr = (u32)(dtype & 0x3f) | ((u32)(vc & 0x3) << 6) |
		((len & 0xffff) << 8);
	writel(hdr, dpu->regs + DSI_APB_WR_LP_HDR);
	for (i = 0; i < 100; i++) {
		status = readl(dpu->regs + DSI_CMD_PLD_BUF_STATUS);
		if ((status & DSI_PLD_STATUS_TX_IDLE) == DSI_PLD_STATUS_TX_IDLE)
			return 0;
		udelay(50);
	}
	return -ETIMEDOUT;
}

/* Vendor lcd_set_display_region(): column then row address, DCS long write. */
static int charlotte_dcs_region(struct charlotte_dpu *dpu, u32 x0, u32 x1,
				u32 y0, u32 y1)
{
	u8 ca[5] = { 0x2a, (u8)(x0 >> 8), (u8)x0, (u8)(x1 >> 8), (u8)x1 };
	u8 ra[5] = { 0x2b, (u8)(y0 >> 8), (u8)y0, (u8)(y1 >> 8), (u8)y1 };
	int ret;

	/* One APB channel: keep the two address packets of a region update
	 * together.  This mutex is only ever taken around DCS traffic so it
	 * cannot deadlock against diagnostic_lock (takeover holds that across
	 * the modeset that calls this).
	 */
	mutex_lock(&dpu->dcs_lock);
	ret = charlotte_dsi_lwrite(dpu, DTYPE_DCS_LWRITE, 0, ca, sizeof(ca));
	if (!ret)
		ret = charlotte_dsi_lwrite(dpu, DTYPE_DCS_LWRITE, 0, ra,
					   sizeof(ra));
	mutex_unlock(&dpu->dcs_lock);
	return ret;
}

/* Vendor partial_update_set_reg(): window width/height plus the VFP delta,
 * expressed in the post-DSC domain (w is the compressed stream width).  The
 * fields are narrower than the registers, hence the read-modify-write.
 */
static int charlotte_ldi_window(struct charlotte_dpu *dpu, u32 w, u32 h)
{
	u32 vfp, v;

	if (!w || !h || w > DSC_OUT_WIDTH || h > HEIGHT)
		return -EINVAL;
	vfp = dpu->boot_vfp + HEIGHT - h;
	v = readl(dpu->regs + DSI_LDI_DPI0_HRZ_CTRL2);
	writel((v & ~GENMASK(12, 0)) | (w - 1), dpu->regs + DSI_LDI_DPI0_HRZ_CTRL2);
	v = readl(dpu->regs + DSI_LDI_VRT_CTRL2);
	writel((v & ~GENMASK(12, 0)) | (h - 1), dpu->regs + DSI_LDI_VRT_CTRL2);
	v = readl(dpu->regs + DSI_VIDEO_VFP);
	writel((v & ~GENMASK(19, 0)) | vfp, dpu->regs + DSI_VIDEO_VFP);
	v = readl(dpu->regs + DSI_VIDEO_VACT);
	writel((v & ~GENMASK(23, 0)) | h, dpu->regs + DSI_VIDEO_VACT);
	wmb();
	return 0;
}

/* Put the LDI window back exactly as the bootloader left it. */
static void charlotte_ldi_window_restore(struct charlotte_dpu *dpu)
{
	u32 v;

	/* Never write back an all-zero capture: a zero window would make the
	 * LDI wait for one pixel per line and starve the frame pump.
	 */
	if (!dpu->boot_v2)
		return;
	v = readl(dpu->regs + DSI_LDI_DPI0_HRZ_CTRL2);
	writel((v & ~GENMASK(12, 0)) | (dpu->boot_h2 & GENMASK(12, 0)),
	       dpu->regs + DSI_LDI_DPI0_HRZ_CTRL2);
	v = readl(dpu->regs + DSI_LDI_VRT_CTRL2);
	writel((v & ~GENMASK(12, 0)) | (dpu->boot_v2 & GENMASK(12, 0)),
	       dpu->regs + DSI_LDI_VRT_CTRL2);
	v = readl(dpu->regs + DSI_VIDEO_VFP);
	writel((v & ~GENMASK(19, 0)) | (dpu->boot_vfp & GENMASK(19, 0)),
	       dpu->regs + DSI_VIDEO_VFP);
	v = readl(dpu->regs + DSI_VIDEO_VACT);
	writel((v & ~GENMASK(23, 0)) | (dpu->boot_vact & GENMASK(23, 0)),
	       dpu->regs + DSI_VIDEO_VACT);
	wmb();
}

/* Snapshot the firmware DM descriptor once so a failed patch is reversible. */
static int charlotte_dm_snapshot(struct charlotte_dpu *dpu)
{
	unsigned int i;

	if (dpu->dm_backup)
		return 0;
	dpu->dm_backup = devm_kzalloc(dpu->dev, DM_PARAM_SIZE, GFP_KERNEL);
	if (!dpu->dm_backup)
		return -ENOMEM;
	for (i = 0; i < DM_PARAM_SIZE / 4; i++)
		dpu->dm_backup[i] = readl(dpu->regs + DM0 + i * 4);
	return 0;
}

/*
 * Patch the DACC-owned DM descriptor without knowing its field layout: only
 * words that still hold the bootloader framebuffer address or stride are
 * rewritten, so unrelated configuration stays untouched.
 */
static int charlotte_dm_patch(struct charlotte_dpu *dpu, dma_addr_t addr,
			      u32 stride)
{
	u32 old_l = dpu->boot_address_l;
	u32 old_h = dpu->boot_address_h & 0xff;
	u32 new_l = lower_32_bits(addr);
	u32 new_h = upper_32_bits(addr) & 0xff;
	unsigned int i, hit_addr = 0, hit_stride = 0;
	int ret;

	ret = charlotte_dm_snapshot(dpu);
	if (ret)
		return ret;
	for (i = 0; i < DM_PARAM_SIZE / 4; i++) {
		u32 v = readl(dpu->regs + DM0 + i * 4);

		if (old_l && v == old_l) {
			writel(new_l, dpu->regs + DM0 + i * 4);
			hit_addr++;
		} else if (old_h && v == old_h) {
			writel(new_h, dpu->regs + DM0 + i * 4);
			hit_addr++;
		} else if (dpu->boot_stride && stride != dpu->boot_stride &&
			   v == dpu->boot_stride) {
			writel(stride, dpu->regs + DM0 + i * 4);
			hit_stride++;
		}
	}
	wmb();
	writel(1, dpu->regs + DACC_SCENE0_RELOAD);
	dev_info(dpu->dev, "dm patch %pad/%u hits addr=%u stride=%u\n", &addr,
		 stride, hit_addr, hit_stride);
	return 0;
}

static int charlotte_dm_restore(struct charlotte_dpu *dpu)
{
	unsigned int i;

	if (!dpu->dm_backup)
		return -ENODATA;
	for (i = 0; i < DM_PARAM_SIZE / 4; i++)
		writel(dpu->dm_backup[i], dpu->regs + DM0 + i * 4);
	/* Hand the scene back exactly as the bootloader left it: no chain and
	 * register-config mode.
	 */
	writel(readl(dpu->regs + DACC_SCENE0 + 0x24) & ~1U,
	       dpu->regs + DACC_SCENE0 + 0x24);
	wmb();
	writel(1, dpu->regs + DACC_SCENE0_RELOAD);
	dev_info(dpu->dev, "dm descriptor restored\n");
	return 0;
}

/* DSC_ST_DATAIN/OUT are 13-bit counters, so deltas only make sense modulo
 * that width; frame and TE counts are the authoritative "did it advance".
 */
#define DSC_ST_MASK 0x1fff

static void charlotte_diag_sample(struct charlotte_dpu *dpu,
				  struct charlotte_diag_sample *s)
{
	s->itf_size = readl(dpu->regs + ITF0_OFFSET + 0x000);
	s->itf_state = readl(dpu->regs + ITF0_OFFSET + ITF0_STATE);
	s->dsc_in = readl(dpu->regs + DSC0_OFFSET + DSC0_ST_DATAIN);
	s->dsc_out = readl(dpu->regs + DSC0_OFFSET + DSC0_ST_DATAOUT);
	s->dsc_fifo = readl(dpu->regs + DSC0_OFFSET + DSC0_ST_FIFO);
	s->dsc_lb = readl(dpu->regs + DSC0_OFFSET + DSC0_ST_LINEBUF);
	s->ldi_h2 = readl(dpu->regs + DSI_LDI_DPI0_HRZ_CTRL2);
	s->ldi_v2 = readl(dpu->regs + DSI_LDI_VRT_CTRL2);
	s->vstate = readl(dpu->regs + DSI_VSTATE);
	s->pld_st = readl(dpu->regs + DSI_CMD_PLD_BUF_STATUS);
	s->frames = dpu->frame_count;
	s->te = dpu->te_count;
}

static int charlotte_present_run(struct charlotte_dpu *dpu, unsigned int mode)
{
	dma_addr_t addr;
	u32 stride;
	int ret = 0;

	switch (mode) {
	case 0:
		break;
	case 1:
		/* Panel-side address window only: cannot disturb the DPU path. */
		ret = charlotte_dcs_region(dpu, 0, WIDTH - 1, 0, HEIGHT - 1);
		break;
	case 2:
		ret = charlotte_regseq_run(dpu, charlotte_seq_itf,
					   ARRAY_SIZE(charlotte_seq_itf));
		break;
	case 3:
		if (!dpu->cmdlist_buf) {
			ret = charlotte_cmdlist_start(dpu);
			break;
		}
		/* Re-point the scene and re-enable cmdlist mode for this frame. */
		charlotte_cmdlist_patch(dpu, dpu->next_address, dpu->next_stride,
					dpu->next_format);
		charlotte_dacc_arm(dpu);
		break;
	case 4:
		addr = ((u64)(readl(dpu->regs + DM_ADDRESS_H) & 0xff) << 32) |
			readl(dpu->regs + DM_ADDRESS_L);
		stride = readl(dpu->regs + DM_STRIDE);
		ret = charlotte_dm_patch(dpu, addr, stride);
		break;
	case 5:
		ret = charlotte_dcs_region(dpu, 0, WIDTH - 1, 0, HEIGHT - 1);
		if (ret)
			break;
		ret = charlotte_ldi_window(dpu, DSC_OUT_WIDTH, HEIGHT);
		if (ret)
			break;
		ret = charlotte_regseq_run(dpu, charlotte_seq_itf,
					   ARRAY_SIZE(charlotte_seq_itf));
		if (!ret && !dpu->cmdlist_buf)
			ret = charlotte_cmdlist_start(dpu);
		if (!ret)
			charlotte_dacc_arm(dpu);
		break;
	case 6:
		/* LDI window alone, in the post-DSC domain: isolates the frame
		 * pump from the panel-side address window.
		 */
		ret = charlotte_ldi_window(dpu, DSC_OUT_WIDTH, HEIGHT);
		break;
	case 8:
		charlotte_ldi_window_restore(dpu);
		charlotte_ldi_arm(dpu);
		return 0;
	case 9:
		return charlotte_dm_restore(dpu);
	default:
		return -EINVAL;
	}
	/* Command-mode panels only move pixels on an LDI frame trigger. */
	charlotte_ldi_arm(dpu);
	return ret;
}

static ssize_t charlotte_present_write(struct file *file,
	const char __user *buf, size_t len, loff_t *pos)
{
	struct charlotte_dpu *dpu = file->f_inode->i_private;
	unsigned int mode;
	int ret;

	ret = kstrtouint_from_user(buf, len, 0, &mode);
	if (ret)
		return ret;
	mutex_lock(&dpu->diagnostic_lock);
	ret = charlotte_present_run(dpu, mode);
	mutex_unlock(&dpu->diagnostic_lock);
	dev_info(dpu->dev, "present %u: %d\n", mode, ret);
	return ret ? ret : len;
}

static const struct file_operations charlotte_present_fops = {
	.owner = THIS_MODULE, .open = simple_open,
	.write = charlotte_present_write, .llseek = noop_llseek,
};

static ssize_t charlotte_dcs_write(struct file *file, const char __user *buf,
	size_t len, loff_t *pos)
{
	struct charlotte_dpu *dpu = file->f_inode->i_private;
	char tmp[160], *p, *tok;
	u8 data[64];
	unsigned int v;
	int n = 0, ret;

	if (!len || len >= sizeof(tmp))
		return -EINVAL;
	if (copy_from_user(tmp, buf, len))
		return -EFAULT;
	tmp[len] = '\0';
	p = tmp;
	while ((tok = strsep(&p, " \t\n,")) != NULL) {
		if (!*tok)
			continue;
		if (n >= ARRAY_SIZE(data))
			return -EINVAL;
		if (kstrtouint(tok, 16, &v) || v > 0xff)
			return -EINVAL;
		data[n++] = (u8)v;
	}
	if (!n)
		return -EINVAL;
	mutex_lock(&dpu->dcs_lock);
	ret = charlotte_dsi_lwrite(dpu, DTYPE_DCS_LWRITE, 0, data, n);
	mutex_unlock(&dpu->dcs_lock);
	dev_info(dpu->dev, "dcs %d bytes: %d\n", n, ret);
	return ret ? ret : len;
}

static const struct file_operations charlotte_dcs_fops = {
	.owner = THIS_MODULE, .open = simple_open, .write = charlotte_dcs_write,
	.llseek = noop_llseek,
};

static ssize_t charlotte_region_write(struct file *file,
	const char __user *buf, size_t len, loff_t *pos)
{
	struct charlotte_dpu *dpu = file->f_inode->i_private;
	unsigned int x0, x1, y0, y1;
	char tmp[64];
	int ret;

	if (!len || len >= sizeof(tmp))
		return -EINVAL;
	if (copy_from_user(tmp, buf, len))
		return -EFAULT;
	tmp[len] = '\0';
	if (sscanf(tmp, "%u %u %u %u", &x0, &x1, &y0, &y1) != 4) {
		if (strncmp(tmp, "full", 4))
			return -EINVAL;
		x0 = 0;
		x1 = WIDTH - 1;
		y0 = 0;
		y1 = HEIGHT - 1;
	}
	if (x1 >= WIDTH || y1 >= HEIGHT || x0 > x1 || y0 > y1)
		return -EINVAL;
	mutex_lock(&dpu->diagnostic_lock);
	ret = charlotte_dcs_region(dpu, x0, x1, y0, y1);
	mutex_unlock(&dpu->diagnostic_lock);
	dev_info(dpu->dev, "panel region %u..%u/%u..%u: %d\n", x0, x1, y0, y1,
		 ret);
	return ret ? ret : len;
}

static const struct file_operations charlotte_region_fops = {
	.owner = THIS_MODULE, .open = simple_open,
	.write = charlotte_region_write, .llseek = noop_llseek,
};

static ssize_t charlotte_window_write(struct file *file,
	const char __user *buf, size_t len, loff_t *pos)
{
	struct charlotte_dpu *dpu = file->f_inode->i_private;
	unsigned int w, h;
	char tmp[32];
	int ret;

	if (!len || len >= sizeof(tmp))
		return -EINVAL;
	if (copy_from_user(tmp, buf, len))
		return -EFAULT;
	tmp[len] = '\0';
	if (sscanf(tmp, "%u %u", &w, &h) != 2)
		return -EINVAL;
	mutex_lock(&dpu->diagnostic_lock);
	ret = charlotte_ldi_window(dpu, w, h);
	if (!ret)
		charlotte_ldi_arm(dpu);
	mutex_unlock(&dpu->diagnostic_lock);
	if (ret)
		return ret;
	dev_info(dpu->dev, "ldi window %ux%u of %ux%u (h2=%08x v2=%08x vfp=%08x vact=%08x)\n",
		 w, h, DSC_OUT_WIDTH, HEIGHT,
		 readl(dpu->regs + DSI_LDI_DPI0_HRZ_CTRL2),
		 readl(dpu->regs + DSI_LDI_VRT_CTRL2),
		 readl(dpu->regs + DSI_VIDEO_VFP),
		 readl(dpu->regs + DSI_VIDEO_VACT));
	return len;
}

static const struct file_operations charlotte_window_fops = {
	.owner = THIS_MODULE, .open = simple_open,
	.write = charlotte_window_write, .llseek = noop_llseek,
};

static int charlotte_dmparam_show(struct seq_file *s, void *unused)
{
	struct charlotte_dpu *dpu = s->private;
	unsigned int i;

	seq_printf(s, "dpu_dm_param window %06x..%06x (%u bytes)\n", DM0,
		   DM0 + DM_PARAM_SIZE, DM_PARAM_SIZE);
	for (i = 0; i < DM_PARAM_SIZE / 16; i++) {
		u32 off = DM0 + i * 16;

		seq_printf(s, "%06x: %08x %08x %08x %08x\n", off,
			   readl(dpu->regs + off + 0x0),
			   readl(dpu->regs + off + 0x4),
			   readl(dpu->regs + off + 0x8),
			   readl(dpu->regs + off + 0xc));
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(charlotte_dmparam);

/* The probe-time descriptor, captured before this driver wrote anything.
 * Comparing it with "dmparam" shows whether readback is faithful and what
 * the firmware's working image actually looked like.
 */
static int charlotte_dmsnap_show(struct seq_file *s, void *unused)
{
	struct charlotte_dpu *dpu = s->private;
	const u32 *img = dpu->dm_backup;
	unsigned int i;

	if (!img)
		return -ENODATA;
	seq_printf(s, "dpu_dm_param probe snapshot %06x..%06x (%u bytes)\n", DM0,
		   DM0 + DM_PARAM_SIZE, DM_PARAM_SIZE);
	for (i = 0; i < DM_PARAM_SIZE / 16; i++)
		seq_printf(s, "%06x: %08x %08x %08x %08x\n", DM0 + i * 16,
			   img[i * 4 + 0], img[i * 4 + 1], img[i * 4 + 2],
			   img[i * 4 + 3]);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(charlotte_dmsnap);

static ssize_t charlotte_selftest_write(struct file *file,
	const char __user *buf, size_t len, loff_t *pos)
{
	struct charlotte_dpu *dpu = file->f_inode->i_private;
	struct charlotte_diag_sample a, b;
	unsigned int mode;
	int ret;

	/* Any value triggers the sweep; modes run in increasing escalation. */
	mutex_lock(&dpu->diagnostic_lock);
	charlotte_diag_sample(dpu, &a);
	dev_info(dpu->dev,
		 "selftest base itf=%08x state=%08x dsc in=%08x out=%08x fifo=%08x lb=%08x ldi=%08x/%08x vstate=%08x pld=%08x\n",
		 a.itf_size, a.itf_state, a.dsc_in, a.dsc_out, a.dsc_fifo,
		 a.dsc_lb, a.ldi_h2, a.ldi_v2, a.vstate, a.pld_st);
	for (mode = 0; mode <= 6; mode++) {
		ret = charlotte_present_run(dpu, mode);
		msleep(150);
		charlotte_diag_sample(dpu, &b);
		dev_info(dpu->dev,
			 "selftest mode=%u ret=%d itf=%08x state=%08x dsc in=%08x(%+d) out=%08x(%+d) fifo=%08x lb=%08x ldi=%08x/%08x vstate=%08x frames=%llu(+%llu) te=%llu(+%llu) pld=%08x\n",
			 mode, ret, b.itf_size, b.itf_state, b.dsc_in,
			 (int)((b.dsc_in - a.dsc_in) & DSC_ST_MASK), b.dsc_out,
			 (int)((b.dsc_out - a.dsc_out) & DSC_ST_MASK), b.dsc_fifo,
			 b.dsc_lb, b.ldi_h2, b.ldi_v2, b.vstate,
			 b.frames, b.frames - a.frames, b.te, b.te - a.te,
			 b.pld_st);
		a = b;
	}
	mutex_unlock(&dpu->diagnostic_lock);
	return len;
}

static const struct file_operations charlotte_selftest_fops = {
	.owner = THIS_MODULE, .open = simple_open,
	.write = charlotte_selftest_write, .llseek = noop_llseek,
};
static ssize_t charlotte_refresh_write(struct file *file, const char __user *buf,
	size_t len, loff_t *pos)
{
	struct charlotte_dpu *dpu = file->f_inode->i_private;
	unsigned int value;
	char tmp[16];
	int ret;

	if (!len || len >= sizeof(tmp))
		return -EINVAL;
	if (copy_from_user(tmp, buf, len))
		return -EFAULT;
	tmp[len] = '\0';
	ret = kstrtouint(tmp, 0, &value);
	if (ret)
		return ret;
	charlotte_ldi_arm(dpu);
	return len;
}
static const struct file_operations charlotte_refresh_fops = {
	.owner = THIS_MODULE, .open = simple_open, .write = charlotte_refresh_write,
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
static const char *charlotte_cmdlist_ints_str(u32 ints, char *buf, size_t len)
{
	snprintf(buf, len, "%s%s%s%s%s%s",
		 ints & CMDLIST_INTS_TASKDONE ? "taskdone " : "",
		 ints & CMDLIST_INTS_TIMEOUT ? "timeout " : "",
		 ints & CMDLIST_INTS_BADCMD ? "badcmd " : "",
		 ints & CMDLIST_INTS_AXI_ERR ? "axi_err " : "",
		 ints & BIT(1) ? "alldone " : "",
		 ints & BIT(0) ? "onedone " : "");
	return buf;
}
static int charlotte_cmdlist_show(struct seq_file *s, void *unused)
{
	struct charlotte_dpu *dpu = s->private;
	char d0[64], d7[64];
	u32 ints0 = readl(dpu->regs + CMDLIST_CH_INTS(0));
	u32 ints7 = readl(dpu->regs + CMDLIST_CH_INTS(CMDLIST_SCENE_INIT));

	seq_printf(s, "active=%u dma=%pad dm_cmd4=%08x\n", dpu->cmdlist_buf != NULL,
		&dpu->cmdlist_dma, readl(dpu->regs + DM0 + 4));
	/* The transport overwrites DM0+0/+4 with 0 and the chain address, then
	 * fills the client slots with 0xFF, so dm0+8 only reads 0xffffffff
	 * after the sequencer really performed the transfer.
	 */
	seq_printf(s, "dm0_w0=%08x dm0_w2=%08x (%s)\n",
		readl(dpu->regs + DM0), readl(dpu->regs + DM0 + 8),
		readl(dpu->regs + DM0 + 8) == 0xffffffff ? "transport ran" : "no transfer");
	seq_printf(s, "scene0 ints=%08x [%s] ctrl=%08x staad=%08x\n", ints0,
		charlotte_cmdlist_ints_str(ints0, d0, sizeof(d0)),
		readl(dpu->regs + CMDLIST_CH_CTRL(0)),
		readl(dpu->regs + CMDLIST_CH_STAAD(0)));
	seq_printf(s, "init7  ints=%08x [%s] ctrl=%08x staad=%08x\n", ints7,
		charlotte_cmdlist_ints_str(ints7, d7, sizeof(d7)),
		readl(dpu->regs + CMDLIST_CH_CTRL(CMDLIST_SCENE_INIT)),
		readl(dpu->regs + CMDLIST_CH_STAAD(CMDLIST_SCENE_INIT)));
	seq_printf(s, "monpc=%08x start=%08x mask_dis=%08x bypass=%08x\n",
		readl(dpu->regs + 0x40014),
		readl(dpu->regs + CMDLIST_START),
		readl(dpu->regs + CMDLIST_ADDR_MASK_DIS),
		readl(dpu->regs + CMDLIST_TWO_SAME_FRAME_BYPASS));
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

/*
 * Chain execution matrix.  The transport overwrites DM0+0/+4 with 0 and the
 * chain address and fills every client slot with 0xFF, so DM0+8 reading
 * 0xffffffff is a signature only the DMA transport can produce.  Restore the
 * pristine image, then try every plausible way of starting the scene and
 * report which one (if any) made the transfer happen.
 */
static void charlotte_cmdldiag_reset(struct charlotte_dpu *dpu)
{
	charlotte_dm_restore(dpu);
	writel(0, dpu->regs + DACC_SCENE0_SW_START);
	writel(0, dpu->regs + DACC_SCENE0_ST_FRM_SEL);
	writel(0xff, dpu->regs + CMDLIST_CH_INTS(CMDLIST_SCENE_INIT));
	writel(0xff, dpu->regs + CMDLIST_CH_INTS(0));
}
static int charlotte_cmdldiag_wait(struct charlotte_dpu *dpu, const char *label,
				   unsigned int ms)
{
	unsigned int i;
	u32 ints0, ints7, sce0;

	for (i = 0; i < ms; i++) {
		if (readl(dpu->regs + DM0 + 8) == 0xffffffff) {
			ints0 = readl(dpu->regs + CMDLIST_CH_INTS(0));
			ints7 = readl(dpu->regs + CMDLIST_CH_INTS(CMDLIST_SCENE_INIT));
			sce0 = readl(dpu->regs + CMDLIST_SCE0_CFG_SIGNAL);
			dev_info(dpu->drm.dev,
				 "cmdldiag: %s => TRANSPORT-RAN after %ums s0_ints=%02x init_ints=%02x sce0=%08x\n",
				 label, i, ints0, ints7, sce0);
			return 0;
		}
		msleep(1);
	}
	dev_info(dpu->drm.dev,
		 "cmdldiag: %s => no transfer (%ums) stfrm=%08x swstart=%08x cfgmode=%08x sce0=%08x\n",
		 label, ms, readl(dpu->regs + DACC_SCENE0_ST_FRM_SEL),
		 readl(dpu->regs + DACC_SCENE0_SW_START),
		 readl(dpu->regs + DACC_SCENE0_CFG_MODE),
		 readl(dpu->regs + CMDLIST_SCE0_CFG_SIGNAL));
	return -ETIMEDOUT;
}
static ssize_t charlotte_cmdldiag_write(struct file *file, const char __user *buf,
	size_t len, loff_t *pos)
{
	struct charlotte_dpu *dpu = file->private_data;
	unsigned int value;
	int ret;

	ret = kstrtouint_from_user(buf, len, 0, &value);
	if (ret)
		return ret;
	if (value != 1)
		return -EINVAL;

	charlotte_cmdlist_release(dpu);
	ret = charlotte_cmdlist_start(dpu);
	if (ret) {
		dev_info(dpu->drm.dev, "cmdldiag: chain start failed %d\n", ret);
		return ret;
	}
	dev_info(dpu->drm.dev, "cmdldiag: chain=%pad dm_cmd4=%08x\n",
		 &dpu->cmdlist_dma, readl(dpu->regs + DM0 + 4));

	/* 1. Exactly what the vendor present path does. */
	charlotte_cmdldiag_wait(dpu, "vendor cfgmode", 300);

	/* 2. Scene software start. */
	charlotte_cmdldiag_reset(dpu);
	charlotte_dacc_arm(dpu);
	writel(1, dpu->regs + DACC_SCENE0_SW_START);
	charlotte_cmdldiag_wait(dpu, "sw_start", 300);

	/* 3. Let the DACC generate the start frame itself. */
	charlotte_cmdldiag_reset(dpu);
	charlotte_dacc_arm(dpu);
	writel(1, dpu->regs + DACC_SCENE0_ST_FRM_SEL);
	charlotte_cmdldiag_wait(dpu, "st_frm_sel", 300);

	/* 4. Both triggers together. */
	charlotte_cmdldiag_reset(dpu);
	charlotte_dacc_arm(dpu);
	writel(1, dpu->regs + DACC_SCENE0_ST_FRM_SEL);
	writel(1, dpu->regs + DACC_SCENE0_SW_START);
	charlotte_cmdldiag_wait(dpu, "sw_start+st_frm_sel", 300);

	dev_info(dpu->drm.dev,
		 "cmdldiag: end dacc_status=%08x monpc=%08x scmd_st=%08x dcmd_st=%08x layers=%08x\n",
		 readl(dpu->regs + DACC_STATUS), readl(dpu->regs + DACC_MONPC),
		 readl(dpu->regs + DACC_SCENE0_SCMD_STATUS),
		 readl(dpu->regs + DACC_SCENE0_DCMD_STATUS),
		 readl(dpu->regs + DACC_SCENE0_LAYER_NUM));
	return len;
}
static const struct file_operations charlotte_cmdldiag_fops = {
	.owner = THIS_MODULE, .open = simple_open,
	.write = charlotte_cmdldiag_write, .llseek = noop_llseek,
};

static ssize_t charlotte_daccload_write(struct file *file, const char __user *buf,
	size_t len, loff_t *pos)
{
	struct charlotte_dpu *dpu = file->private_data;
	unsigned value;
	int ret = kstrtouint_from_user(buf, len, 0, &value);

	if (ret)
		return ret;
	if (value != 1)
		return -EINVAL;
	mutex_lock(&dpu->diagnostic_lock);
	charlotte_dacc_load(dpu);
	mutex_unlock(&dpu->diagnostic_lock);
	return len;
}
static const struct file_operations charlotte_daccload_fops = {
	.owner = THIS_MODULE, .open = simple_open, .write = charlotte_daccload_write,
	.llseek = noop_llseek,
};

static ssize_t charlotte_daccffa_write(struct file *file, const char __user *buf,
	size_t len, loff_t *pos)
{
	struct charlotte_dpu *dpu = file->private_data;
	unsigned value;
	int ret = kstrtouint_from_user(buf, len, 0, &value);

	if (ret)
		return ret;
	if (value != 1)
		return -EINVAL;
	mutex_lock(&dpu->diagnostic_lock);
	charlotte_dacc_ffa_load(dpu);
	mutex_unlock(&dpu->diagnostic_lock);
	return len;
}
static const struct file_operations charlotte_daccffa_fops = {
	.owner = THIS_MODULE, .open = simple_open, .write = charlotte_daccffa_write,
	.llseek = noop_llseek,
};

/*
 * Vendor enable_dsi_ldi(): in command mode the DSI fetches exactly one frame
 * per TE, so LDI_CTRL bit0 must be re-armed on every panel TE.  Arm it now so
 * the frame starts, then let charlotte_irq() keep it pumped.  This exercises
 * the DSI/ITF path without a full KMS takeover.
 */
static ssize_t charlotte_ldipump_write(struct file *file, const char __user *buf,
	size_t len, loff_t *pos)
{
	struct charlotte_dpu *dpu = file->private_data;
	unsigned value;
	int ret = kstrtouint_from_user(buf, len, 0, &value);

	if (ret)
		return ret;
	if (value > 1)
		return -EINVAL;
	mutex_lock(&dpu->diagnostic_lock);
	dpu->ldi_pump = value;
	if (value) {
		writel(1, dpu->regs + DSI_LDI_FRM_MSK_UP);
		writel(readl(dpu->regs + DSI_LDI_CTRL) | BIT(0),
			dpu->regs + DSI_LDI_CTRL);
	} else {
		writel(readl(dpu->regs + DSI_LDI_CTRL) & ~BIT(0),
			dpu->regs + DSI_LDI_CTRL);
	}
	mutex_unlock(&dpu->diagnostic_lock);
	return len;
}
static const struct file_operations charlotte_ldipump_fops = {
	.owner = THIS_MODULE, .open = simple_open, .write = charlotte_ldipump_write,
	.llseek = noop_llseek,
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
/* recover=true lets an operator clear a tripped storm guard; the plain start
 * path still refuses to run on top of a fault that was never explained.
 */
static int charlotte_irq_start(struct charlotte_dpu *dpu, bool recover)
{
	int ret;
	if (dpu->irq_storm && !recover)
		return -EIO;
	if (dpu->irq_running)
		return -EBUSY;
	if (recover) {
		dpu->irq_storm = false;
		dpu->underflow_muted = false;
		dpu->irq_window_count = 0;
		dpu->irq_window_ns = 0;
	}
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
	if (value > 2)
		return -EINVAL;
	/* 2 = clear a tripped storm guard and restart the pump; it is the only
	 * form allowed while KMS owns the pipeline, because a storm leaves the
	 * display frozen until someone re-enables the handler.
	 */
	if (value != 2 && (!probe_only || dpu->kms || dpu->test_active))
		return -EINVAL;
	cancel_delayed_work_sync(&dpu->irq_stop_work);
	mutex_lock(&dpu->diagnostic_lock);
	if (value) {
		/* Arm restoration before any interrupt mutation.  KMS owns the
		 * display, so its handler has to keep running.
		 */
		if (!dpu->kms)
			schedule_delayed_work(&dpu->irq_stop_work,
					      msecs_to_jiffies(60000));
		ret = charlotte_irq_start(dpu, value == 2);
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

/* V0: reversible DSI command->video transport flip and end-to-end bring-up.
 *
 * The new road needs the panel to accept a free-running video stream instead of
 * TE-paced EDPI.  This node flips the transport (MODE_CTRL bit0=0, CMD_MOD_CTRL
 * bit1=1, LDI_FRM_MSK=0, LDI enable), programs the vendor video-mode bits
 * (VIDEO_MODE_CTRL LP/burst, LP_CMD_TIMING, TO_TIME), seeds the authoritative
 * MRO timing only if the bootloader left it clear, and releases the frozen
 * fastboot pipeline on the active DPP0->DSC0->ITF0 pipe.  Everything is
 * snapshotted so "echo 0" restores the handoff state exactly.  Watching the
 * result:
 *   - vcount/hcount change across the two 20ms samples means the VTG free-runs;
 *   - vstate cycling VSW/VACTIVE (0x2/0x8) instead of stuck at IDLE;
 *   - vsyncs/frames rising, underflows flagging a starved FIFO.
 * If vcount stays put the DPU-side VTG/online path is not enabled yet; if the
 * stream runs but the panel stays dark the DDIC rejected video and the FF-A
 * command-mode route is the fallback.  "echo 2" halves the rate via VFP.
 */
static ssize_t charlotte_videomode_write(struct file *file, const char __user *buf,
	size_t len, loff_t *pos)
{
	struct charlotte_dpu *dpu = file->private_data;
	unsigned value;
	u32 mode, cmdmod, vstate, vinact, ints, h2, v2;
	u32 vcount0, vcount1, hcount0, hcount1;
	int ret = kstrtouint_from_user(buf, len, 0, &value);

	if (ret)
		return ret;
	/* 1 = video at the bootloader rate, 2 = video at roughly half rate by
	 * expanding VFP only (lane clock and PHY stay untouched).  3 = video
	 * plus the full DACC-free frame push, 4 = video plus a single latch so
	 * 3 and 4 together answer whether the DPP needs a per-frame commit.
	 * 5/6 repeat 4/3 after loading the real DACC firmware through FF-A:
	 * the core is the only consumer of the DM block, so if the frame
	 * counters wake up only in 5/6 the missing piece is the dispatcher.
	 */
	if (value > 6)
		return -EINVAL;
	cancel_delayed_work_sync(&dpu->irq_stop_work);
	mutex_lock(&dpu->diagnostic_lock);
	if (value && !dpu->video_active) {
		dpu->video_saved_mode = readl(dpu->regs + DSI_MODE_CTRL);
		dpu->video_saved_cmdmod = readl(dpu->regs + DSI_CMD_MODE_CTRL);
		dpu->video_saved_frmmsk = readl(dpu->regs + DSI_LDI_FRM_MSK);
		dpu->video_saved_vidmode = readl(dpu->regs + DSI_VIDEO_MODE_CTRL);
		dpu->video_saved_lpcmd = readl(dpu->regs + DSI_VIDEO_MODE_LP_CMD_TIMING);
		dpu->video_saved_totime = readl(dpu->regs + DSI_TO_TIME_CTRL);
		dpu->video_saved_ldi_h2 = readl(dpu->regs + DSI_LDI_DPI0_HRZ_CTRL2);
		dpu->video_saved_ldi_v2 = readl(dpu->regs + DSI_LDI_VRT_CTRL2);
		dpu->video_saved_cmdmodctl = readl(dpu->regs + DSI_CMD_MOD_CTRL);
		dpu->video_saved_ldi_ctrl = readl(dpu->regs + DSI_LDI_CTRL);
		dpu->video_saved_lpclk = readl(dpu->regs + DSI_LP_CLK_CTRL);
		dpu->video_saved_hsa = readl(dpu->regs + DSI_VIDEO_HSA);
		dpu->video_saved_hbp = readl(dpu->regs + DSI_VIDEO_HBP);
		dpu->video_saved_hline = readl(dpu->regs + DSI_VIDEO_HLINE);
		dpu->video_saved_vsa = readl(dpu->regs + DSI_VIDEO_VSA);
		dpu->video_saved_vbp = readl(dpu->regs + DSI_VIDEO_VBP);
		dpu->video_saved_vact = readl(dpu->regs + DSI_VIDEO_VACT);
		dpu->video_saved_pktlen = readl(dpu->regs + DSI_VIDEO_PKT_LEN);
		dpu->video_saved_ldi_h3 = readl(dpu->regs + DSI_LDI_DPI0_HRZ_CTRL3);
		dpu->video_saved_vfp = readl(dpu->regs + DSI_VIDEO_VFP);
		dpu->video_saved_vcid = readl(dpu->regs + DSI_VIDEO_VCID);
		dpu->video_saved_colorfmt = readl(dpu->regs + DSI_VIDEO_COLOR_FORMAT);
		dpu->video_saved_polctrl = readl(dpu->regs + DSI_VIDEO_POL_CTRL);
		dpu->video_saved_dpp_dbg = readl(dpu->regs + DPP0_REG_CTRL_DEBUG);
		dpu->video_saved_dsc_dbg = readl(dpu->regs + DSC0_REG_CTRL_DEBUG);
		dpu->video_saved_itf_dbg = readl(dpu->regs + ITF0_REG_CTRL_DEBUG);
		dev_info(dpu->drm.dev,
			"video: timing vidctl=%08x pktlen=%u hsa=%u hbp=%u hline=%u vsa=%u vbp=%u vfp=%u vact=%u ldi_h2=%08x ldi_v2=%08x ldi_h3=%08x lpclk=%08x cmdmodctl=%08x\n",
			readl(dpu->regs + DSI_VIDEO_MODE_CTRL),
			readl(dpu->regs + DSI_VIDEO_PKT_LEN),
			readl(dpu->regs + DSI_VIDEO_HSA), readl(dpu->regs + DSI_VIDEO_HBP),
			readl(dpu->regs + DSI_VIDEO_HLINE), readl(dpu->regs + DSI_VIDEO_VSA),
			readl(dpu->regs + DSI_VIDEO_VBP), readl(dpu->regs + DSI_VIDEO_VFP),
			readl(dpu->regs + DSI_VIDEO_VACT),
			readl(dpu->regs + DSI_LDI_DPI0_HRZ_CTRL2),
			readl(dpu->regs + DSI_LDI_VRT_CTRL2),
			readl(dpu->regs + DSI_LDI_DPI0_HRZ_CTRL3),
			readl(dpu->regs + DSI_LP_CLK_CTRL),
			readl(dpu->regs + DSI_CMD_MOD_CTRL));
		/* Same bounded IRQ lifetime as irq_test so a stuck source cannot
		 * keep the peripheral enabled forever.
		 */
		schedule_delayed_work(&dpu->irq_stop_work, msecs_to_jiffies(60000));
		if (!dpu->irq_running) {
			ret = charlotte_irq_start(dpu, false);
			/* A storm trip returns -EIO and leaves the IRQ disabled.
			 * Video mode can still be evaluated by polling vcount, so
			 * never block the transport flip on the counter source.
			 */
			if (ret)
				dev_info(dpu->drm.dev,
					 "video: irq not started (%d); using vcount polling\n",
					 ret);
		}
		/* mipi_dsi_video_mode_config(): LP mode on all lanes (bits
		 * 13:8), send read command in LP (bit15), burst mode in bits
		 * 1:0.  The command-mode skip in mipi_dsi_on_sub2() left these
		 * at default, so program the DT values rather than inherit them.
		 */
		writel((dpu->video_saved_vidmode &
			~(GENMASK(13, 8) | GENMASK(1, 0))) |
		       (0x3f << 8) | BIT(15) | DSI_VIDEO_BURST_MODE,
			dpu->regs + DSI_VIDEO_MODE_CTRL);
		/* outvact_lpcmd_time is bits [23:16], the same field the
		 * vendor sets via set_reg(..., 0x4, 8, 16).
		 */
		writel((dpu->video_saved_lpcmd & ~GENMASK(23, 16)) |
		       (0x4 << 16), dpu->regs + DSI_VIDEO_MODE_LP_CMD_TIMING);
		writel((dpu->video_saved_totime & ~GENMASK(15, 0)) | 0x7ff,
			dpu->regs + DSI_TO_TIME_CTRL);
		/* LDI horizontal/vertical active are derived from the DSC output
		 * geometry; only seed them when the bootloader left them clear so
		 * we never clobber a valid handoff value.
		 */
		h2 = readl(dpu->regs + DSI_LDI_DPI0_HRZ_CTRL2);
		v2 = readl(dpu->regs + DSI_LDI_VRT_CTRL2);
		if ((h2 & GENMASK(12, 0)) == 0)
			writel((h2 & ~GENMASK(12, 0)) | 0x3a7,
				dpu->regs + DSI_LDI_DPI0_HRZ_CTRL2);
		if ((v2 & GENMASK(12, 0)) == 0)
			writel((v2 & ~GENMASK(12, 0)) | 0x72f,
				dpu->regs + DSI_LDI_VRT_CTRL2);
		/* If the bootloader left the video timing empty, seed the exact
		 * MRO values recovered from the stock lcd_config DT: HSA 10,
		 * HBP 12, HLINE 878, VSA 18, VBP 70, VFP 42, VACT 1840,
		 * PKT_LEN 936 (single DSI, DSC 3.75x, dsc_out_width 936).
		 * Read-modify-write keeps any reserved upper bits intact.
		 */
		if (readl(dpu->regs + DSI_VIDEO_HLINE) == 0 ||
		    readl(dpu->regs + DSI_VIDEO_VACT) == 0 ||
		    readl(dpu->regs + DSI_LDI_DPI0_HRZ_CTRL3) == 0) {
			writel((readl(dpu->regs + DSI_VIDEO_HSA) & ~GENMASK(11, 0)) | 10,
				dpu->regs + DSI_VIDEO_HSA);
			writel((readl(dpu->regs + DSI_VIDEO_HBP) & ~GENMASK(11, 0)) | 12,
				dpu->regs + DSI_VIDEO_HBP);
			writel((readl(dpu->regs + DSI_VIDEO_HLINE) & ~GENMASK(14, 0)) | 878,
				dpu->regs + DSI_VIDEO_HLINE);
			writel((readl(dpu->regs + DSI_VIDEO_VSA) & ~GENMASK(19, 0)) | 18,
				dpu->regs + DSI_VIDEO_VSA);
			writel((readl(dpu->regs + DSI_VIDEO_VBP) & ~GENMASK(19, 0)) | 70,
				dpu->regs + DSI_VIDEO_VBP);
			writel((readl(dpu->regs + DSI_VIDEO_VFP) & ~GENMASK(19, 0)) | 42,
				dpu->regs + DSI_VIDEO_VFP);
			writel((readl(dpu->regs + DSI_VIDEO_VACT) & ~GENMASK(23, 0)) | 1840,
				dpu->regs + DSI_VIDEO_VACT);
			writel((readl(dpu->regs + DSI_VIDEO_PKT_LEN) & ~GENMASK(13, 0)) | 936,
				dpu->regs + DSI_VIDEO_PKT_LEN);
			writel((readl(dpu->regs + DSI_LDI_DPI0_HRZ_CTRL3) & ~GENMASK(12, 0)) | 701,
				dpu->regs + DSI_LDI_DPI0_HRZ_CTRL3);
			dev_info(dpu->drm.dev,
				"video: seeded MRO video timing (bootloader left it clear)\n");
		}
		/* value 2: halve the frame rate by adding one full frame of idle
		 * lines to VFP.  This keeps the 845MHz lane clock and the PHY
		 * untouched while giving underflow/timing more margin.
		 */
		if (value == 2) {
			u32 vsa = readl(dpu->regs + DSI_VIDEO_VSA);
			u32 vbp = readl(dpu->regs + DSI_VIDEO_VBP);
			u32 vact = readl(dpu->regs + DSI_VIDEO_VACT);
			u32 vfp = readl(dpu->regs + DSI_VIDEO_VFP);
			u32 vtotal = vsa + vbp + vact + vfp;
			writel((vfp & ~GENMASK(19, 0)) + vtotal,
				dpu->regs + DSI_VIDEO_VFP);
			dev_info(dpu->drm.dev,
				"video: half-rate vfp %u -> %u (vtotal %u -> %u)\n",
				vfp, vfp + vtotal, vtotal, vtotal * 2);
		}
		/* Release the bootloader's frozen fastboot pipeline state on the
		 * active pipe (DPP0 -> DSC0 -> ITF0), as the vendor composer does
		 * once the kernel boots.
		 */
		writel(dpu->video_saved_dpp_dbg & ~BIT(10),
			dpu->regs + DPP0_REG_CTRL_DEBUG);
		writel(dpu->video_saved_dsc_dbg & ~BIT(10),
			dpu->regs + DSC0_REG_CTRL_DEBUG);
		writel(dpu->video_saved_itf_dbg & ~BIT(10),
			dpu->regs + ITF0_REG_CTRL_DEBUG);
		dev_info(dpu->drm.dev,
			"video: fastboot dbg dpp=%08x dsc=%08x itf=%08x -> clear bit10\n",
			dpu->video_saved_dpp_dbg, dpu->video_saved_dsc_dbg,
			dpu->video_saved_itf_dbg);
		/* Backlight is intentionally left alone here so this stays a
		 * single-variable test; the separate "backlight" node toggles it.
		 */
		/* mipi_dsi_config_dpi_interface(): virtual channel, DPI colour
		 * packing and pin polarity.  Command mode never programs these,
		 * so a video flip without them shows wrong colours or a shifted
		 * image even when the timing is right.
		 */
		writel(0, dpu->regs + DSI_VIDEO_VCID);
		writel(DSI_VIDEO_COLOR_MODE, dpu->regs + DSI_VIDEO_COLOR_FORMAT);
		writel(DSI_VIDEO_POL_VENDOR, dpu->regs + DSI_VIDEO_POL_CTRL);
		dev_info(dpu->drm.dev,
			 "video: dpi vcid=0 colorfmt=%02x pol=%02x burst=%u\n",
			 DSI_VIDEO_COLOR_MODE, DSI_VIDEO_POL_VENDOR,
			 DSI_VIDEO_BURST_MODE);
		/* Vendor video path only clears MODE_CTRL bit0; CMD_MODE_CTRL
		 * keeps the mipi_init values for video panels.  mipi_ldi_init()
		 * additionally sets CMD_MOD_CTRL bit1 (dsi_halt_video_en) and
		 * clears LDI_FRM_MSK for a video panel.
		 */
		writel(dpu->video_saved_mode & ~BIT(0), dpu->regs + DSI_MODE_CTRL);
		writel(dpu->video_saved_cmdmodctl | BIT(1),
			dpu->regs + DSI_CMD_MOD_CTRL);
		writel(0, dpu->regs + DSI_LDI_FRM_MSK);
		writel(dpu->video_saved_ldi_ctrl | BIT(0),
			dpu->regs + DSI_LDI_CTRL);
		/* phy_txrequestclkhs must be on for video.  sub2 already turns it
		 * on for command mode, so this is normally a no-op; keep it
		 * explicit so a handoff that left it clear still transmits.
		 */
		writel(dpu->video_saved_lpclk | BIT(0), dpu->regs + DSI_LP_CLK_CTRL);
		dpu->video_active = true;
		msleep(30);
		/* 3/4: hand the DACC's job to the cmdlist engine while the DSI
		 * timing generator is running.  3 pushes frames, 4 latches the
		 * pipeline once; comparing the two says whether the DPP needs a
		 * per-frame commit in video mode.
		 */
		if (value >= 3) {
			bool push = (value == 3 || value == 6);
			int bypass_ret;

			if (value >= 5)
				charlotte_dacc_ffa_load(dpu);
			bypass_ret = charlotte_bypass_run(dpu, push);
			dev_info(dpu->drm.dev,
				 "video: bypass(%s%s) ret=%d monpc=%08x dacc_int=%08x\n",
				 value >= 5 ? "dacc-loaded " : "",
				 push ? "frame push" : "single latch", bypass_ret,
				 readl(dpu->regs + DACC_MONPC),
				 readl(dpu->regs + DACC_CTRL));
		}
	} else if (value && dpu->video_active) {
		dev_info(dpu->drm.dev,
			"video: already active; echo 0 first to re-arm\n");
	} else if (dpu->video_active) {
		writel(dpu->video_saved_mode, dpu->regs + DSI_MODE_CTRL);
		writel(dpu->video_saved_cmdmod, dpu->regs + DSI_CMD_MODE_CTRL);
		writel(dpu->video_saved_frmmsk, dpu->regs + DSI_LDI_FRM_MSK);
		writel(dpu->video_saved_vidmode, dpu->regs + DSI_VIDEO_MODE_CTRL);
		writel(dpu->video_saved_lpcmd, dpu->regs + DSI_VIDEO_MODE_LP_CMD_TIMING);
		writel(dpu->video_saved_totime, dpu->regs + DSI_TO_TIME_CTRL);
		writel(dpu->video_saved_ldi_h2, dpu->regs + DSI_LDI_DPI0_HRZ_CTRL2);
		writel(dpu->video_saved_ldi_v2, dpu->regs + DSI_LDI_VRT_CTRL2);
		writel(dpu->video_saved_cmdmodctl, dpu->regs + DSI_CMD_MOD_CTRL);
		writel(dpu->video_saved_ldi_ctrl, dpu->regs + DSI_LDI_CTRL);
		writel(dpu->video_saved_lpclk, dpu->regs + DSI_LP_CLK_CTRL);
		writel(dpu->video_saved_hsa, dpu->regs + DSI_VIDEO_HSA);
		writel(dpu->video_saved_hbp, dpu->regs + DSI_VIDEO_HBP);
		writel(dpu->video_saved_hline, dpu->regs + DSI_VIDEO_HLINE);
		writel(dpu->video_saved_vsa, dpu->regs + DSI_VIDEO_VSA);
		writel(dpu->video_saved_vbp, dpu->regs + DSI_VIDEO_VBP);
		writel(dpu->video_saved_vact, dpu->regs + DSI_VIDEO_VACT);
		writel(dpu->video_saved_pktlen, dpu->regs + DSI_VIDEO_PKT_LEN);
		writel(dpu->video_saved_ldi_h3, dpu->regs + DSI_LDI_DPI0_HRZ_CTRL3);
		writel(dpu->video_saved_vfp, dpu->regs + DSI_VIDEO_VFP);
		writel(dpu->video_saved_vcid, dpu->regs + DSI_VIDEO_VCID);
		writel(dpu->video_saved_colorfmt, dpu->regs + DSI_VIDEO_COLOR_FORMAT);
		writel(dpu->video_saved_polctrl, dpu->regs + DSI_VIDEO_POL_CTRL);
		writel(dpu->video_saved_itf_dbg, dpu->regs + ITF0_REG_CTRL_DEBUG);
		writel(dpu->video_saved_dsc_dbg, dpu->regs + DSC0_REG_CTRL_DEBUG);
		writel(dpu->video_saved_dpp_dbg, dpu->regs + DPP0_REG_CTRL_DEBUG);
		/* The video underflow flood trips the storm guard.  Back in
		 * command mode the sources are TE-paced again, so clear the flag
		 * and re-arm the diagnostic IRQ instead of forcing a reboot.
		 */
		if (dpu->irq_storm) {
			dpu->irq_storm = false;
			ret = charlotte_irq_start(dpu, false);
			if (ret)
				dev_warn(dpu->drm.dev,
					 "video: cmd-mode irq recovery failed: %d\n",
					 ret);
		}
		dpu->video_active = false;
		msleep(20);
	}
	/* Keep the diagnostic IRQ lifetime bounded even on a restore or a
	 * no-op write; the entry cancelled any pending auto-stop.
	 */
	if (dpu->irq_running)
		schedule_delayed_work(&dpu->irq_stop_work, msecs_to_jiffies(60000));
	vcount0 = readl(dpu->regs + DSI_VCOUNT);
	hcount0 = readl(dpu->regs + DSI_HCOUNT);
	msleep(20);
	vcount1 = readl(dpu->regs + DSI_VCOUNT);
	hcount1 = readl(dpu->regs + DSI_HCOUNT);
	mode = readl(dpu->regs + DSI_MODE_CTRL);
	cmdmod = readl(dpu->regs + DSI_CMD_MODE_CTRL);
	vstate = readl(dpu->regs + DSI_VSTATE);
	vinact = readl(dpu->regs + DSI_VINACT);
	ints = readl(dpu->regs + DSI_INTS);
	dev_info(dpu->drm.dev,
		"video: active=%u mode=%08x cmdmod=%08x frmmsk=%08x vstate=%08x vinact=%08x ints=%08x vcount=%u->%u hcount=%u->%u vsyncs=%llu frames=%llu te=%llu underflows=%llu\n",
		dpu->video_active, mode, cmdmod,
		readl(dpu->regs + DSI_LDI_FRM_MSK), vstate, vinact, ints,
		vcount0, vcount1, hcount0, hcount1,
		dpu->vsync_count, dpu->frame_count, dpu->te_count, dpu->underflows);
	mutex_unlock(&dpu->diagnostic_lock);
	return len;
}
static const struct file_operations charlotte_videomode_fops = {
	.owner = THIS_MODULE, .open = simple_open, .write = charlotte_videomode_write,
	.llseek = noop_llseek,
};

/* Registers snapshotted for the online bring-up.  Reverse order restores. */
static const u32 charlotte_online_regs[] = {
	DPP0_OFFSET + 0x0014,	/* DPP_CLRBAR_CTRL */
	DPP0_OFFSET + 0x09a0,	/* GMP_EN */
	DPP0_OFFSET + 0x0800,	/* ROIGAMA0_EN */
	DPP0_OFFSET + 0x0900,	/* ROIGAMA1_EN */
	DPP0_OFFSET + 0x1730,	/* NLXCC_EN */
	DPP0_OFFSET + 0x1780,	/* XCC_EN */
	DPP0_OFFSET + 0x1610,	/* CLIP_EN */
	DPP0_OFFSET + 0x1900,	/* ALSC_DEGAMMA_EN */
	DPP0_OFFSET + 0x1914,	/* ALSC_EN */
	DPP0_OFFSET + 0x0058,	/* REG_CTRL (scene id) */
	DPP0_OFFSET + 0x0060,	/* REG_CTRL_FLUSH_EN */
	DPP0_REG_CTRL_DEBUG,
	DSC0_OFFSET + 0x0250,	/* REG_CTRL (scene id) */
	DSC0_OFFSET + 0x0254,	/* REG_CTRL_FLUSH_EN */
	DSC0_REG_CTRL_DEBUG,
	ITF0_OFFSET + 0x0000,	/* ITF_CH_IMG_SIZE */
	ITF0_OFFSET + 0x0014,	/* DPP_CLIP_EN */
	ITF0_OFFSET + 0x0030,	/* DPP_CLRBAR_CTRL */
	ITF0_OFFSET + 0x0058,	/* REG_CTRL (scene id) */
	ITF0_OFFSET + 0x005c,	/* REG_CTRL_FLUSH_EN */
	ITF0_REG_CTRL_DEBUG,
	ITF0_OFFSET + 0x0068,	/* ITFSW_DATA_SEL */
	PIPE_SW_OFFSET + 0x0000,/* SIG_CTRL */
	PIPE_SW_OFFSET + 0x0004,/* SIG_EN */
	PIPE_SW_OFFSET + 0x0008,/* DAT_CTRL */
	PIPE_SW_OFFSET + 0x000c,/* DAT_EN */
	DM_LAYER_SIZE,
	DM_LAYER_SELECT,
	DM_LAYER_OV_FMT,
	DM_ADDRESS_H,
	DM_ADDRESS_L,
	DM_STRIDE,
};

static void charlotte_online_restore(struct charlotte_dpu *dpu)
{
	unsigned long flags;
	u32 i;

	/* The snapshot also holds the bootloader layer address, so the SDMA
	 * stops touching the diagnostic buffer the moment this completes.
	 * Serialise the layer words against the IRQ scanout latch.
	 */
	spin_lock_irqsave(&dpu->lock, flags);
	for (i = dpu->online_snap_n; i-- > 0; )
		writel(dpu->online_snap_val[i], dpu->regs + dpu->online_snap_off[i]);
	wmb();
	spin_unlock_irqrestore(&dpu->lock, flags);
	dpu->online_snap_n = 0;
}

static void charlotte_online_fill(struct charlotte_dpu *dpu, u32 rgb)
{
	u32 x, y;

	rgb &= 0x00ffffffU;
	for (y = 0; y < HEIGHT; y++) {
		u32 *row = (u32 *)((u8 *)dpu->test_vaddr + y * TEST_PITCH);
		for (x = 0; x < WIDTH; x++)
			row[x] = rgb;
	}
	wmb();
}

/* Bring the DPP0 -> DSC0 -> ITF0 chain up from the bootloader handoff.
 *
 * The bootloader leaves every operator frozen (REG_CTRL_DEBUG bit10) and the
 * pipe unrouted, so config commits and flushes are ignored until this runs.
 * The diagnostic online path proved this is what makes the panel take our
 * frames; the KMS path needs the same sequence. Register programming only:
 * the layer descriptor is owned by the atomic commit.
 */
static void charlotte_pipeline_commit(struct charlotte_dpu *dpu)
{
	unsigned long flags;

	spin_lock_irqsave(&dpu->lock, flags);
	writel(readl(dpu->regs + DPP0_REG_CTRL_DEBUG) & ~BIT(10),
		dpu->regs + DPP0_REG_CTRL_DEBUG);
	writel(readl(dpu->regs + DSC0_REG_CTRL_DEBUG) & ~BIT(10),
		dpu->regs + DSC0_REG_CTRL_DEBUG);
	writel(readl(dpu->regs + ITF0_REG_CTRL_DEBUG) & ~BIT(10),
		dpu->regs + ITF0_REG_CTRL_DEBUG);

	/* DPP0: bypass every optional block, then commit scene 0. */
	writel(0x2, dpu->regs + DPP0_OFFSET + 0x09a0);
	writel(0, dpu->regs + DPP0_OFFSET + 0x0800);
	writel(0, dpu->regs + DPP0_OFFSET + 0x0900);
	writel(0, dpu->regs + DPP0_OFFSET + 0x1730);
	writel(0, dpu->regs + DPP0_OFFSET + 0x1780);
	writel(0, dpu->regs + DPP0_OFFSET + 0x1610);
	writel(0, dpu->regs + DPP0_OFFSET + 0x1900);
	writel(0, dpu->regs + DPP0_OFFSET + 0x1914);
	writel(0, dpu->regs + DPP0_OFFSET + 0x0014);
	writel(0, dpu->regs + DPP0_OFFSET + 0x0058);
	writel(1, dpu->regs + DPP0_OFFSET + 0x0060);

	/* DSC0: the bootloader already programmed PIC/slice; only commit it. */
	writel(0, dpu->regs + DSC0_OFFSET + 0x0250);
	writel(1, dpu->regs + DSC0_OFFSET + 0x0254);

	/* ITF0: it now consumes the DSC output geometry instead of zero. */
	writel(0, dpu->regs + ITF0_OFFSET + 0x0014);
	writel(0, dpu->regs + ITF0_OFFSET + 0x0030);
	writel(ITF_IMG_SIZE, dpu->regs + ITF0_OFFSET + 0x0000);
	writel(0, dpu->regs + ITF0_OFFSET + 0x0068);
	writel(0, dpu->regs + ITF0_OFFSET + 0x0058);
	writel(1, dpu->regs + ITF0_OFFSET + 0x005c);

	/* Route DPP0 onto ITF0 (single panel: SIG_EN=1, DAT_EN=1). */
	writel(BIT(0), dpu->regs + PIPE_SW_OFFSET + 0x0000);
	writel(1, dpu->regs + PIPE_SW_OFFSET + 0x0004);
	writel(BIT(0), dpu->regs + PIPE_SW_OFFSET + 0x0008);
	writel(1, dpu->regs + PIPE_SW_OFFSET + 0x000c);
	wmb();
	spin_unlock_irqrestore(&dpu->lock, flags);
}

static int charlotte_online_enable(struct charlotte_dpu *dpu, u32 rgb)
{
	unsigned long flags;
	u32 i;

	BUILD_BUG_ON(ARRAY_SIZE(charlotte_online_regs) > CHARLOTTE_ONLINE_SNAP_MAX);
	if (!dpu->test_vaddr) {
		dpu->test_vaddr = dma_alloc_wc(dpu->dev, TEST_SIZE,
			&dpu->test_address, GFP_KERNEL | __GFP_NOWARN);
		if (!dpu->test_vaddr)
			return -ENOMEM;
	}
	charlotte_online_fill(dpu, rgb);

	for (i = 0; i < ARRAY_SIZE(charlotte_online_regs); i++) {
		dpu->online_snap_off[i] = charlotte_online_regs[i];
		dpu->online_snap_val[i] = readl(dpu->regs + charlotte_online_regs[i]);
	}
	dpu->online_snap_n = ARRAY_SIZE(charlotte_online_regs);

	/* DM first: point layer 0 at the solid-color buffer. */
	spin_lock_irqsave(&dpu->lock, flags);
	program_scanout(dpu, dpu->test_address, TEST_PITCH, DRM_FORMAT_XRGB8888);
	spin_unlock_irqrestore(&dpu->lock, flags);

	charlotte_pipeline_commit(dpu);

	/* Command-mode panels need one LDI frame per TE; video panels free-run.
	 * Keep the pump armed either way so the node works in both transports.
	 */
	dpu->online_saved_pump = dpu->ldi_pump;
	dpu->ldi_pump = true;
	dpu->online_active = true;
	dpu->test_active = true;
	return 0;
}

/* Online bring-up: a single command that builds the whole DPP0 -> DSC0 -> ITF0
 * path and scans one solid-color frame out of the handoff layer.
 *   echo 1        red
 *   echo 2        green
 *   echo 3        blue
 *   echo 0xRRGGBB any other colour
 *   echo 0        restore every touched register
 * The transport (command vs video) is orthogonal: run "videomode 1" first for
 * a free-running stream, or leave it alone for TE-paced command mode.
 */
static ssize_t charlotte_online_write(struct file *file, const char __user *buf,
	size_t len, loff_t *pos)
{
	struct charlotte_dpu *dpu = file->private_data;
	u32 rgb = 0, mode, vstate, vcount0, vcount1, hcount0, hcount1;
	u64 frames0, te0, under0;
	unsigned value;
	int ret = kstrtouint_from_user(buf, len, 0, &value);

	if (ret)
		return ret;
	if (value > 0xffffffU || !probe_only || dpu->kms)
		return -EINVAL;
	cancel_delayed_work_sync(&dpu->irq_stop_work);
	mutex_lock(&dpu->diagnostic_lock);
	if (value == 0) {
		if (dpu->online_active) {
			charlotte_online_restore(dpu);
			dpu->ldi_pump = dpu->online_saved_pump;
			dpu->test_active = false;
			dpu->online_active = false;
			msleep(20);
		}
	} else if (dpu->online_active) {
		/* Live recolour: charlotte_irq() flushes the DSC every frame, so
		 * rewriting the buffer content is enough.
		 */
		rgb = (value == 1) ? 0xff0000 :
		      (value == 2) ? 0x00ff00 :
		      (value == 3) ? 0x0000ff : value;
		charlotte_online_fill(dpu, rgb);
	} else {
		if (dpu->test_active) {
			ret = -EBUSY;
			goto out;
		}
		if (value == 1)
			rgb = 0xff0000;
		else if (value == 2)
			rgb = 0x00ff00;
		else if (value == 3)
			rgb = 0x0000ff;
		else
			rgb = value;
		ret = charlotte_online_enable(dpu, rgb);
		if (ret)
			goto out;
		if (!dpu->irq_running) {
			ret = charlotte_irq_start(dpu, false);
			if (ret)
				dev_info(dpu->drm.dev,
					 "online: irq not started (%d); checked by polling\n",
					 ret);
			/* A storm trip only disables the counter source; the
			 * register path above is still programmed.
			 */
			ret = 0;
		}
		msleep(30);
	}
	if (dpu->irq_running)
		schedule_delayed_work(&dpu->irq_stop_work, msecs_to_jiffies(60000));
	vcount0 = readl(dpu->regs + DSI_VCOUNT);
	hcount0 = readl(dpu->regs + DSI_HCOUNT);
	frames0 = dpu->frame_count;
	te0 = dpu->te_count;
	under0 = dpu->underflows;
	msleep(20);
	vcount1 = readl(dpu->regs + DSI_VCOUNT);
	hcount1 = readl(dpu->regs + DSI_HCOUNT);
	mode = readl(dpu->regs + DSI_MODE_CTRL);
	vstate = readl(dpu->regs + DSI_VSTATE);
	/* Self-check: every block must read back the value just committed, and
	 * the frame/TE counters must advance for the panel to be refreshing.
	 */
	dev_info(dpu->drm.dev,
		 "online: active=%u rgb=%06x itf_size=%08x itf_ctrl=%08x dsc_ctrl=%08x dpp_ctrl=%08x psw=%08x/%08x/%08x/%08x\n",
		 dpu->online_active, rgb,
		 readl(dpu->regs + ITF0_OFFSET + 0x0000),
		 readl(dpu->regs + ITF0_OFFSET + 0x0058),
		 readl(dpu->regs + DSC0_OFFSET + 0x0250),
		 readl(dpu->regs + DPP0_OFFSET + 0x0058),
		 readl(dpu->regs + PIPE_SW_OFFSET + 0x0000),
		 readl(dpu->regs + PIPE_SW_OFFSET + 0x0004),
		 readl(dpu->regs + PIPE_SW_OFFSET + 0x0008),
		 readl(dpu->regs + PIPE_SW_OFFSET + 0x000c));
	dev_info(dpu->drm.dev,
		 "online: mode=%08x vstate=%08x vcount=%u->%u hcount=%u->%u frames=%llu(+%llu) te=%llu(+%llu) underflows=%llu(+%llu)\n",
		 mode, vstate, vcount0, vcount1, hcount0, hcount1,
		 dpu->frame_count, dpu->frame_count - frames0,
		 dpu->te_count, dpu->te_count - te0,
		 dpu->underflows, dpu->underflows - under0);
	if (dpu->online_active && dpu->frame_count == frames0)
		dev_warn(dpu->drm.dev,
			 "online: no frame advanced; the ITF is still starved\n");
out:
	mutex_unlock(&dpu->diagnostic_lock);
	return ret ? ret : len;
}
static const struct file_operations charlotte_online_fops = {
	.owner = THIS_MODULE, .open = simple_open, .write = charlotte_online_write,
	.llseek = noop_llseek,
};

/* The MRO panel hangs off a Richtek RT4539 at i2c 0x3c.  Register 0x0b bit7 is
 * the LED enable the firmware path used.  Writing 2 dumps 0x00..0x15 to dmesg so
 * the real chip state can be read back before toggling anything.
 */
static ssize_t charlotte_backlight_write(struct file *file, const char __user *buf,
	size_t len, loff_t *pos)
{
	struct charlotte_dpu *dpu = file->private_data;
	unsigned value;
	int i, ret = kstrtouint_from_user(buf, len, 0, &value);

	if (ret)
		return ret;
	if (value > 2 || !probe_only || dpu->kms)
		return -EINVAL;
	mutex_lock(&dpu->diagnostic_lock);
	if (!dpu->backlight) {
		/* probe_only skips charlotte_kms_init(), which is the only other
		 * caller.  Resolve the supplier lazily here so the diagnostic
		 * works before the native KMS path takes over.
		 */
		ret = charlotte_backlight_init(dpu);
		if (ret)
			dev_info(dpu->drm.dev,
				 "backlight: supplier not ready (%d)\n", ret);
	}
	if (!dpu->backlight) {
		dev_info(dpu->drm.dev, "backlight: no RT4539 supplier bound\n");
	} else if (value == 2) {
		dev_info(dpu->drm.dev, "backlight: reg dump");
		for (i = 0; i <= 0x15; i++) {
			int v = i2c_smbus_read_byte_data(dpu->backlight, i);
			pr_cont(" %02x=%02x", i, v < 0 ? 0xff : v);
		}
		pr_cont("\n");
	} else {
		charlotte_backlight_power(dpu, value);
		dev_info(dpu->drm.dev, "backlight: on=%d ro=%d\n",
			dpu->backlight_on, dpu->backlight_ro);
	}
	mutex_unlock(&dpu->diagnostic_lock);
	return len;
}
static const struct file_operations charlotte_backlight_fops = {
	.owner = THIS_MODULE, .open = simple_open, .write = charlotte_backlight_write,
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
	u32 hw_format;
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
		plane->crtc_w != WIDTH || plane->crtc_h != HEIGHT)
		return -EINVAL;
	if (!plane->fb->format || !charlotte_hw_format(plane->fb->format->format, &hw_format))
		return -EINVAL;
	if (plane->fb->pitches[0] < WIDTH * plane->fb->format->cpp[0] ||
	    plane->fb->pitches[0] % 16)
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
	struct charlotte_dpu *dpu = crtc_to_dpu(crtc);

	/* Loading the DACC scene is a BL31/firmware handshake, so it stays out
	 * of the default enable path: the firmware sequencer that the
	 * bootloader started keeps running unless userspace asks for a reload
	 * through the "daccload" (SMC) or "daccffa" (secure partition) node.
	 */
	/* Take the handoff chain out of the bootloader freeze and route it to
	 * the panel; without this every commit and flush is silently dropped.
	 */
	charlotte_pipeline_commit(dpu);
	/* The bootloader left the online scene in register-config mode with no
	 * cmdlist (cfgmode=0, cmdlist_addr0=0), which is why the panel can hold
	 * the last frame but never accepts a new one.  Start the vendor
	 * composer chain from the probe-time descriptor so the sequencer has
	 * something valid to fetch.
	 */
	if (!dpu->cmdlist_buf && charlotte_cmdlist_start(dpu))
		dev_warn(dpu->dev, "cmdlist start failed; keeping register-config mode\n");
	charlotte_dacc_arm(dpu);
	/* The bootloader leaves a partial column/row address window in the
	 * panel controller, so a full-frame scanout only ever lands in the old
	 * dirty region.  The vendor panel driver re-sends the whole-panel
	 * window on every region change; do the same once per enable.
	 */
	if (charlotte_dcs_region(dpu, 0, WIDTH - 1, 0, HEIGHT - 1))
		dev_warn(dpu->dev, "panel window update failed; keeping firmware region\n");
	/* The LDI window is expressed in the post-DSC domain, so ask for the
	 * compressed stream width with the full panel height.  A window left
	 * in panel pixels would make the LDI carry more data per line than
	 * the DSC produces.
	 */
	charlotte_ldi_window(dpu, DSC_OUT_WIDTH, HEIGHT);
	/* Open the LDI frame-valid gate before the first commit so the panel
	 * receives pixels instead of only internal timing.
	 */
	charlotte_ldi_arm(dpu);
	drm_crtc_vblank_on(crtc);
	charlotte_backlight_power(dpu, true);
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
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};
static const struct drm_encoder_funcs charlotte_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};
static int charlotte_kms_init(struct charlotte_dpu *dpu)
{
	struct drm_device *drm = &dpu->drm;
	static const u32 formats[] = {
		DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888,
		DRM_FORMAT_XBGR8888, DRM_FORMAT_ABGR8888,
		DRM_FORMAT_RGB565,
	};
	static const u64 modifiers[] = { DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_MOD_INVALID };
	int ret;
	/* Second look at the handed-over pipeline, now that probe has finished
	 * but before KMS programs anything.
	 */
	charlotte_diag_sample(dpu, &dpu->pre_kms_pipe);
	ret = charlotte_backlight_init(dpu);
	if (ret)
		dev_warn(drm->dev, "backlight unavailable (%d), continuing without brightness control\n",
			ret);
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
	/* The single DM layer cannot blend, so alpha is ignored at scanout.
	 * Advertising the modes keeps alpha-capable clients working and
	 * silences the core's "alpha exposed but blend mode not setup" check.
	 */
	drm_plane_create_blend_mode_property(&dpu->plane,
		BIT(DRM_MODE_BLEND_PIXEL_NONE) | BIT(DRM_MODE_BLEND_PREMULTI) |
		BIT(DRM_MODE_BLEND_COVERAGE));
	/* Full-frame scanout ignores damage, but advertising the property lets
	 * compositors submit partial-update commits without failing the check.
	 */
	drm_plane_enable_fb_damage_clips(&dpu->plane);
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
	ret = charlotte_irq_start(dpu, false);
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
	/* Device tree decides, so a stale probe_only=1 on the bootloader
	 * command line cannot keep simpledrm in charge of the panel.
	 */
	probe_only = of_property_read_bool(dev->of_node, "hisilicon,probe-only");
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
	dpu->boot_vfp = readl(dpu->regs + DSI_VIDEO_VFP);
	dpu->boot_h2 = readl(dpu->regs + DSI_LDI_DPI0_HRZ_CTRL2);
	dpu->boot_v2 = readl(dpu->regs + DSI_LDI_VRT_CTRL2);
	dpu->boot_vact = readl(dpu->regs + DSI_VIDEO_VACT);
	/* Keep the handoff composer state: it is the only sample of a working
	 * DPP/DM image once this driver has touched the pipeline.
	 */
	dpu->boot_dpp[0] = readl(dpu->regs + DPP0_OFFSET + 0x00c);
	dpu->boot_dpp[1] = readl(dpu->regs + DPP0_OFFSET + 0x058);
	dpu->boot_dpp[2] = readl(dpu->regs + DPP0_OFFSET + 0x068);
	dpu->boot_dpp[3] = readl(dpu->regs + DPP0_OFFSET + 0x9a0);
	dpu->boot_dpp[4] = readl(dpu->regs + DPP0_OFFSET + 0x010);
	dpu->boot_dm[0] = readl(dpu->regs + DM0 + 0x58);
	dpu->boot_dm[1] = readl(dpu->regs + DM0 + 0x5c);
	dpu->boot_dm[2] = readl(dpu->regs + DM0 + 0x60);
	/* Still untouched by this driver: the firmware's own pipeline state. */
	charlotte_diag_sample(dpu, &dpu->boot_pipe);
	spin_lock_init(&dpu->lock);
	mutex_init(&dpu->diagnostic_lock);
	mutex_init(&dpu->dcs_lock);
	INIT_DELAYED_WORK(&dpu->restore_work, charlotte_restore_work);
	INIT_DELAYED_WORK(&dpu->irq_stop_work, charlotte_irq_stop_work);
	INIT_WORK(&dpu->retire_work, charlotte_retire_work);
	init_completion(&dpu->flip_done);
	dpu->dev = dev;
	/* Keep the firmware DM descriptor from probe time so "present 9" always
	 * restores the handoff state, even after the transport cmdlist path has
	 * rewritten the live window.
	 */
	if (charlotte_dm_snapshot(dpu))
		dev_warn(dev, "cannot snapshot the DM descriptor; present 9 unavailable\n");
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
	debugfs_create_file("cmdldiag", 0200, dpu->debugfs, dpu,
		&charlotte_cmdldiag_fops);
	debugfs_create_file("bypass", 0200, dpu->debugfs, dpu,
		&charlotte_bypass_fops);
	debugfs_create_file("daccw", 0200, dpu->debugfs, dpu, &charlotte_regcmd_fops);
	debugfs_create_file("vendorseq", 0200, dpu->debugfs, dpu,
		&charlotte_vendorseq_fops);
	debugfs_create_file("refresh", 0200, dpu->debugfs, dpu,
		&charlotte_refresh_fops);
	/* 0 = one transfer per latched commit, N = also every Nth TE. */
	debugfs_create_u32("pump", 0600, dpu->debugfs, &dpu->pump_div);
	/* Bring-up console: registered on every boot so one image probes all. */
	debugfs_create_file("present", 0200, dpu->debugfs, dpu,
		&charlotte_present_fops);
	debugfs_create_file("dcs", 0200, dpu->debugfs, dpu, &charlotte_dcs_fops);
	debugfs_create_file("region", 0200, dpu->debugfs, dpu,
		&charlotte_region_fops);
	debugfs_create_file("window", 0200, dpu->debugfs, dpu,
		&charlotte_window_fops);
	debugfs_create_file("dmparam", 0400, dpu->debugfs, dpu,
		&charlotte_dmparam_fops);
	debugfs_create_file("dmsnap", 0400, dpu->debugfs, dpu,
			    &charlotte_dmsnap_fops);
	debugfs_create_file("selftest", 0200, dpu->debugfs, dpu,
		&charlotte_selftest_fops);
	/* Diagnostics that reprogram the pipeline stay available as well: the
	 * handoff is identical in probe-only and KMS mode, and having them in
	 * one build avoids a rebuild per experiment.
	 */
	debugfs_create_file("irq_test", 0200, dpu->debugfs, dpu, &charlotte_irq_fops);
	debugfs_create_file("pattern_test", 0200, dpu->debugfs, dpu, &charlotte_pattern_fops);
	debugfs_create_file("takeover", 0200, dpu->debugfs, dpu, &charlotte_takeover_fops);
	debugfs_create_file("daccload", 0200, dpu->debugfs, dpu,
		&charlotte_daccload_fops);
	debugfs_create_file("daccffa", 0200, dpu->debugfs, dpu,
		&charlotte_daccffa_fops);
	debugfs_create_file("ldipump", 0200, dpu->debugfs, dpu,
		&charlotte_ldipump_fops);
	debugfs_create_file("videomode", 0200, dpu->debugfs, dpu,
		&charlotte_videomode_fops);
	debugfs_create_file("online", 0200, dpu->debugfs, dpu,
		&charlotte_online_fops);
	debugfs_create_file("backlight", 0200, dpu->debugfs, dpu,
		&charlotte_backlight_fops);
	if (probe_only) {
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
static int charlotte_pm_suspend(struct device *dev)
{
	struct charlotte_dpu *dpu = dev_get_drvdata(dev);
	if (!dpu || !dpu->kms)
		return 0;
	return drm_mode_config_helper_suspend(&dpu->drm);
}
static int charlotte_pm_resume(struct device *dev)
{
	struct charlotte_dpu *dpu = dev_get_drvdata(dev);
	if (!dpu || !dpu->kms)
		return 0;
	return drm_mode_config_helper_resume(&dpu->drm);
}
static DEFINE_SIMPLE_DEV_PM_OPS(charlotte_pm_ops, charlotte_pm_suspend, charlotte_pm_resume);

static struct platform_driver charlotte_platform_driver = {
	.probe = charlotte_probe, .remove = charlotte_remove, .shutdown = charlotte_shutdown,
	.driver = { .name = "charlotte-dpu", .of_match_table = charlotte_match,
		.pm = &charlotte_pm_ops, .suppress_bind_attrs = true },
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
	if (ret) {
		i2c_del_driver(&charlotte_backlight_driver);
		return ret;
	}
	return 0;
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
