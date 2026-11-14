/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Register initialisation data copied from the vendor kirin9010 5.10 tree
 * (drivers/video/fbdev/dkmd/dpu/begonia/composer/config/v700/
 *  dpu_comp_config_csv2.c) so it can be replayed through the DACC without
 * dragging in the whole dkmd stack.  The vendor composer builds the same
 * sequence in dpu_comp_init(); the ITF/PIPE_SW tail is what the bootloader
 * handoff does not provide on MRO-W00.
 */
#ifndef __CHARLOTTE_VENDOR_INIT_H__
#define __CHARLOTTE_VENDOR_INIT_H__

struct charlotte_reg_init {
	u32 addr;
	u32 val;
	const char *what;
};

#define CHARLOTTE_SEQ_CLOCKS_NUM 43
static const struct charlotte_reg_init charlotte_seq_clocks[] = {
	{ 0x13100, 0x00000000, "dpu_level1_clock_lp" },
	{ 0x12740, 0x00000000, "dpu_level2_clock_lp" },
	{ 0x6d0e4, 0x00000000, "dpu_level2_clock_lp" },
	{ 0x710e4, 0x00000000, "dpu_level2_clock_lp" },
	{ 0x19788, 0x00000000, "dpu_level2_clock_lp" },
	{ 0x1978c, 0x00000000, "dpu_level2_clock_lp" },
	{ 0x1b000, 0x00000000, "dpu_level2_clock_lp" },
	{ 0x1b008, 0x00000000, "dpu_level2_clock_lp" },
	{ 0xcd008, 0x00000002, "dpu_level2_clock_lp" },
	{ 0x78008, 0x00000002, "dpu_level2_clock_lp" },
	{ 0x7f008, 0x00000002, "dpu_level2_clock_lp" },
	{ 0x83008, 0x00000002, "dpu_level2_clock_lp" },
	{ 0x87008, 0x00000002, "dpu_level2_clock_lp" },
	{ 0x8d008, 0x00000000, "dpu_level2_clock_lp" },
	{ 0x75010, 0x00000000, "dpu_level2_clock_lp" },
	{ 0x76010, 0x00000000, "dpu_level2_clock_lp" },
	{ 0x1c000, 0x00000000, "dpu_level2_clock_lp" },
	{ 0x1c008, 0x00000000, "dpu_level2_clock_lp" },
	{ 0x1c010, 0x00000000, "dpu_level2_clock_lp" },
	{ 0x1c018, 0x00000000, "dpu_level2_clock_lp" },
	{ 0x1c020, 0x00000000, "dpu_level2_clock_lp" },
	{ 0xb4008, 0x00000000, "dpu_level2_clock_lp" },
	{ 0xb0008, 0x00000000, "dpu_level2_clock_lp" },
	{ 0xd407c, 0x00000000, "dpu_level2_clock_lp" },
	{ 0xd4248, 0x00000000, "dpu_level2_clock_lp" },
	{ 0x13124, 0x00000000, "dpu_level2_clock_lp" },
	{ 0x1312c, 0x00000000, "dpu_level2_clock_lp" },
	{ 0x13134, 0x00000000, "dpu_level2_clock_lp" },
	{ 0x1313c, 0x00000000, "dpu_level2_clock_lp" },
	{ 0x40400, 0x00000000, "dpu_level2_clock_lp" },
	{ 0x40800, 0x00000000, "dpu_level2_clock_lp" },
	{ 0x40c00, 0x00000000, "dpu_level2_clock_lp" },
	{ 0x41000, 0x00000000, "dpu_level2_clock_lp" },
	{ 0x41400, 0x00000000, "dpu_level2_clock_lp" },
	{ 0x41800, 0x00000000, "dpu_level2_clock_lp" },
	{ 0x41c00, 0x00000000, "dpu_level2_clock_lp" },
	{ 0xcc070, 0x00000001, "dpu_level2_clock_lp" },
	{ 0xcc170, 0x00000001, "dpu_level2_clock_lp" },
	{ 0xcc270, 0x00000001, "dpu_level2_clock_lp" },
	{ 0xcc370, 0x00000001, "dpu_level2_clock_lp" },
	{ 0xcc4f4, 0x00000000, "dpu_level2_clock_lp" },
	{ 0xdd008, 0x00000000, "dpu_ch1_level2_clock_lp" },
	{ 0xd9008, 0x00000000, "dpu_ch1_level2_clock_lp" },
};

#define CHARLOTTE_SEQ_MEMORY_NUM 37
static const struct charlotte_reg_init charlotte_seq_memory[] = {
	{ 0x1273c, 0x00000008, "dpu_memory_no_lp" },
	{ 0x6d03c, 0x88888888, "dpu_memory_no_lp" },
	{ 0x6d040, 0x88888888, "dpu_memory_no_lp" },
	{ 0x6d218, 0x00000088, "dpu_memory_no_lp" },
	{ 0x7103c, 0x88888888, "dpu_memory_no_lp" },
	{ 0x71040, 0x88888888, "dpu_memory_no_lp" },
	{ 0x71218, 0x00000088, "dpu_memory_no_lp" },
	{ 0x131a4, 0x00000008, "dpu_memory_no_lp" },
	{ 0xcd218, 0x00000088, "dpu_memory_no_lp" },
	{ 0xcd290, 0x00000008, "dpu_memory_no_lp" },
	{ 0xcd3e8, 0x00000008, "dpu_memory_no_lp" },
	{ 0xcd3ec, 0x00000088, "dpu_memory_no_lp" },
	{ 0x78218, 0x00000088, "dpu_memory_no_lp" },
	{ 0x78290, 0x00000008, "dpu_memory_no_lp" },
	{ 0x783e8, 0x00000008, "dpu_memory_no_lp" },
	{ 0x783ec, 0x00000088, "dpu_memory_no_lp" },
	{ 0x7f290, 0x00000008, "dpu_memory_no_lp" },
	{ 0x83290, 0x00000008, "dpu_memory_no_lp" },
	{ 0x87404, 0x00000008, "dpu_memory_no_lp" },
	{ 0x87418, 0x00000008, "dpu_memory_no_lp" },
	{ 0x87434, 0x00000008, "dpu_memory_no_lp" },
	{ 0x1c060, 0x00000008, "dpu_memory_no_lp" },
	{ 0x1c064, 0x00000008, "dpu_memory_no_lp" },
	{ 0x1c068, 0x00000008, "dpu_memory_no_lp" },
	{ 0x1c06c, 0x00000008, "dpu_memory_no_lp" },
	{ 0xb4500, 0x00000008, "dpu_memory_no_lp" },
	{ 0xb4704, 0x00000008, "dpu_memory_no_lp" },
	{ 0xb4804, 0x00000008, "dpu_memory_no_lp" },
	{ 0xb4904, 0x00000008, "dpu_memory_no_lp" },
	{ 0xb58a4, 0x00000008, "dpu_memory_no_lp" },
	{ 0xd4084, 0x88888888, "dpu_memory_no_lp" },
	{ 0xdd500, 0x00000008, "dpu_ch1_memory_no_lp" },
	{ 0xdd704, 0x00000008, "dpu_ch1_memory_no_lp" },
	{ 0xdd804, 0x00000008, "dpu_ch1_memory_no_lp" },
	{ 0xdd904, 0x00000008, "dpu_ch1_memory_no_lp" },
	{ 0xdd9a4, 0x00000008, "dpu_ch1_memory_no_lp" },
	{ 0xdd8a4, 0x00000008, "dpu_ch1_memory_no_lp" },
};

#define CHARLOTTE_SEQ_ITF_NUM 13
static const struct charlotte_reg_init charlotte_seq_itf[] = {
	{ 0xcc000, 0x0aef072f, "itfch: IMG_SIZE 1840x2800" },
	{ 0xcc068, 0x00000000, "itfch: ITFSW_DATA_SEL" },
	{ 0xcc05c, 0x00000001, "itfch: FLUSH_EN" },
	{ 0xcc074, 0x000001a1, "itfch: VSYNC_DELAY_CNT_CTRL" },
	{ 0xcc400, 0x00000001, "pipe_sw: SIG_CTRL ch0" },
	{ 0xcc404, 0x00000001, "pipe_sw: SIG_EN ch0" },
	{ 0xcc408, 0x00000001, "pipe_sw: DAT_CTRL ch0" },
	{ 0xcc40c, 0x00000001, "pipe_sw: DAT_EN ch0" },
	{ 0x1b104, 0x00000002, "lbuf: ov_auto_ctrl_en" },
	{ 0x13538, 0x80090001, "sdma: debug reserved1" },
	{ 0x13578, 0x80090001, "sdma: debug reserved1" },
	{ 0x135b8, 0x80090001, "sdma: debug reserved1" },
	{ 0x135f8, 0x80090001, "sdma: debug reserved1" },
};

#endif /* __CHARLOTTE_VENDOR_INIT_H__ */
