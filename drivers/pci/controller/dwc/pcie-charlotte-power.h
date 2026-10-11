/* SPDX-License-Identifier: GPL-2.0-only */
/* Register definitions and sequences adapted from Huawei pcie-jun.c,
 * pcie-kport-common.c and pcie-kport-phy-kphy.c.
 * Copyright (c) Huawei Technologies Co., Ltd. 2016-2020.
 * Generated subset: PCIe0 RC, KPHY v1, no PHY MCU or digital capture.
 */
#define DISABLE 0
#define ENABLE 1
#define FNPLL_PCIE_ACQ_CNT_THR_CFG 0x0068
#define FNPLL_PCIE_ADPLL_ATUNE_CFG 0x0018
#define FNPLL_PCIE_ADPLL_CLK_EN_CFG0 0x0054
#define FNPLL_PCIE_ADPLL_DCO_TMODE 0x003C
#define FNPLL_PCIE_ADPLL_DLY_CFG 0x006C
#define FNPLL_PCIE_ADPLL_DSM_CFG 0x0044
#define FNPLL_PCIE_ADPLL_FTDC_LOCK_THR 0x0048
#define FNPLL_PCIE_ADPLL_FTDC_UNLOCK_THR 0x004C
#define FNPLL_PCIE_ADPLL_LPF_TEST_CFG 0x0040
#define FNPLL_PCIE_ADPLL_PTUNE_CFG 0x001C
#define FNPLL_PCIE_ANALOG_CFG0 0x0008
#define FNPLL_PCIE_ANALOG_CFG1 0x000C
#define FNPLL_PCIE_ANALOG_CFG2 0x0010
#define FNPLL_PCIE_ANALOG_CFG3 0x0014
#define FNPLL_PCIE_CHAOS_CNT_THR_CFG 0x0050
#define FNPLL_PCIE_DTC_CFG 0x0024
#define FNPLL_PCIE_DTC_LINEAR_TEST_MODE 0x0060
#define FNPLL_PCIE_DTC_LMS_CFG 0x0030
#define FNPLL_PCIE_FDTC_LMS_CFG 0x002C
#define FNPLL_PCIE_FREZ_LMS_PTHR 0x0064
#define FNPLL_PCIE_FTDC_CFG 0x0028
#define FNPLL_PCIE_LPF_BW_I_T01 0x0034
#define FNPLL_PCIE_LPF_BW_P_T01 0x0038
#define FNPLL_PCIE_MAN_TRIG_LMS_CNT_CFG 0x005C
#define FNPLL_PCIE_PTUNE_FBDIV 0x0070
#define HSDTCRG_PCIECTRL0 0x300
#define HSDTCRG_PCIECTRL1 0x304
#define HSDTCRG_PERDIS0 0x4
#define HSDTCRG_PERDIS1 0x14
#define HSDTCRG_PEREN0 0x0
#define HSDTCRG_PEREN1 0x10
#define HSDTCRG_PERRSTDIS0 0x64
#define HSDTCRG_PERRSTEN0 0x60
#define IO_CLK_FROM_CIO (0x0 << 17)
#define IO_CLK_SEL_CLEAR (0x3 << 17)
#define NOC_PCIE0_POWER (0x1 << 9)
#define NOC_PCIE0_POWER_MASK (0x1 << 25)
#define NOC_PCIE1_POWER (0x1 << 10)
#define NOC_PCIE1_POWER_MASK (0x1 << 26)
#define NOC_POWER_IDLE 0x384
#define NOC_POWER_IDLEREQ 0x380
#define NOC_TIMEOUT_VAL 1000
#define PCICTRL_SP_MEM_TRA_BIT (0x1 << 16)
#define PCICTRL_SP_MEM_TRA_MASK (0x3 << 16)
#define PCICTRL_TP_MEM_TRA_BIT (0x1 << 27)
#define PCICTRL_TP_MEM_TRA_MASK (0x3 << 27)
#define PCIE0_CLK_DEBUNCE_GATE (0x1 << 2)
#define PCIE0_CLK_HP_GATE (0x1 << 1)
#define PCIE0_CLK_IO_GATE (0x1 << 7)
#define PCIE0_CLK_PHYREF_GATE (0x1 << 6)
#define PCIE1_CLK_DEBUNCE_GATE (0x1 << 7)
#define PCIE1_CLK_HP_GATE (0x1 << 6)
#define PCIE1_CLK_IO_GATE (0x1 << 5)
#define PCIE1_CLK_PHYREF_GATE (0x1 << 4)
#define PCIE1_CTRL_POR_N (0x1 << 21)
#define PCIEIO_HW_BYPASS (0x1 << 0)
#define PCIEIO_IE_EN_HARD_BYPASS (0x1 << 27)
#define PCIEIO_IE_EN_SOFT (0x1 << 28)
#define PCIEIO_IE_POLAR (0x1 << 29)
#define PCIEIO_OE_EN_HARD_BYPASS (0x1 << 11)
#define PCIEIO_OE_EN_SOFT (0x1 << 6)
#define PCIEIO_OE_POLAR (0x1 << 9)
#define PCIEPHY_POR_N_RESET_BIT (0x1 << 1)
#define PCIEPHY_REF_HW_BYPASS (0x1 << 1)
#define PCIEPHY_RX_TERMINATION_BIT (0x1 << 10)
#define PCIE_CLKREQ_OUT_MASK (0x1 << 0)
#define PCIE_CTRL_POR_N (0x1 << 7)
#define PCIE_DBI_EP_SPLT_BIT (0x1 << 0)
#define PCIE_PERST_IN_N_CTRL_11B (0x3 << 2)
#define PCIE_PERST_IN_N_CTRL_BIT2 (0x1 << 2)
#define PCIE_PERST_IN_N_CTRL_BIT3 (0x1 << 3)
#define PCIE_PERST_OE_EN (0x1 << 1)
#define PCIE_PERST_OUT_N (0x1 << 0)
#define PCIE_RST_PCIE0_ASYNCBRG (0x1 << 4)
#define PCIE_RST_PCIE1_ASYNCBRG (0x1 << 1)
#define PCIE_TYPE_MASK 0xFu
#define PCIE_TYPE_RC 4
#define PCIE_TYPE_SHIFT 28
#define SOC_PCIECTRL_CTRL0_ADDR 0x000
#define SOC_PCIECTRL_CTRL12_ADDR 0x030
#define SOC_PCIECTRL_CTRL20_ADDR 0x050
#define SOC_PCIECTRL_CTRL21_ADDR 0x054
#define SOC_PCIECTRL_CTRL22_ADDR 0x058
#define SOC_PCIECTRL_CTRL23_ADDR 0x05C
#define SOC_PCIECTRL_CTRL25_ADDR 0x064
#define SOC_PCIECTRL_CTRL30_ADDR 0x078
#define SOC_PCIECTRL_CTRL7_ADDR 0x01c
#define SOC_PCIEPHY_CTRL1_ADDR 0x004
#define SOC_PCIEPHY_CTRL39_ADDR 0x009C
#define PCIE_CTRL_BUS_TIMEOUT_MASK (0x3f << 1)
#define PCIE_CTRL_BUS_TIMEOUT_VALUE (0x36 << 1)
#define PCIEPHY_RESET_BIT BIT(17)
#define PCIEPHY_PIPE_LINE0_RESET_BIT BIT(19)
#define PCIE_RC0 0
#define DISABLE 0
#define ENABLE 1
#define PCIE_PR_E(fmt, ...) dev_err(pcie->pci.dev, fmt "\n", ##__VA_ARGS__)
#define PCIE_PR_I(fmt, ...) dev_info(pcie->pci.dev, fmt "\n", ##__VA_ARGS__)

static u32 hsdt_crg_reg_read(struct charlotte_pcie *pcie, u32 reg)
{
	return readl(pcie->crg_base + reg);
}

static void hsdt_crg_reg_write(struct charlotte_pcie *pcie, u32 val, u32 reg)
{
	writel(val, pcie->crg_base + reg);
}

static int pcie_noc_power_jun(struct charlotte_pcie *pcie, u32 exit)
{
	u32 mask_bits, val_bits, val;
	u32 time = NOC_TIMEOUT_VAL;

	if (pcie->rc_id == PCIE_RC0) {
		val_bits = NOC_PCIE0_POWER;
		mask_bits = NOC_PCIE0_POWER_MASK;
	} else {
		val_bits = NOC_PCIE1_POWER;
		mask_bits = NOC_PCIE1_POWER_MASK;
	}

	/*
	 * bits in mask_bits set to write the bit
	 * if bit in val_bits is 0, exit noc power idle
	 * or enter noc power idle
	 */
	if (exit) {
		writel(mask_bits, pcie->pmctrl_base + NOC_POWER_IDLEREQ);
		val = readl(pcie->pmctrl_base + NOC_POWER_IDLE);
		while (val & val_bits) {
			udelay(1);
			if (!time) {
				PCIE_PR_E("Exit failed :%u", val);
				return -1;
			}
			time--;
			val = readl(pcie->pmctrl_base + NOC_POWER_IDLE);
		}
	} else {
		writel(mask_bits | val_bits, pcie->pmctrl_base + NOC_POWER_IDLEREQ);
		val = readl(pcie->pmctrl_base + NOC_POWER_IDLE);
		while ((val & val_bits) != val_bits) {
			udelay(1);
			if (!time) {
				PCIE_PR_E("Enter failed :%u", val);
				return -1;
			}
			time--;
			val = readl(pcie->pmctrl_base + NOC_POWER_IDLE);
		}
	}

	return 0;
}

static void pcie_fnpll_cfg_init_set(struct charlotte_pcie *pcie)
{
	writel(0x0000008A, pcie->pcie_pll_base + FNPLL_PCIE_ANALOG_CFG0);
	writel(0x0000000C, pcie->pcie_pll_base + FNPLL_PCIE_ANALOG_CFG1);
	writel(0x000002BE, pcie->pcie_pll_base + FNPLL_PCIE_ANALOG_CFG2);
	writel(0x00000000, pcie->pcie_pll_base + FNPLL_PCIE_ANALOG_CFG3);
	writel(0x00300000, pcie->pcie_pll_base + FNPLL_PCIE_ADPLL_ATUNE_CFG);
	writel(0x026020A0, pcie->pcie_pll_base + FNPLL_PCIE_ADPLL_PTUNE_CFG);
	writel(0x00012ABC, pcie->pcie_pll_base + FNPLL_PCIE_DTC_CFG);
	writel(0x03666E1F, pcie->pcie_pll_base + FNPLL_PCIE_FTDC_CFG);
	writel(0x6420030A, pcie->pcie_pll_base + FNPLL_PCIE_FDTC_LMS_CFG);
	writel(0x5620153F, pcie->pcie_pll_base + FNPLL_PCIE_DTC_LMS_CFG);
	writel(0x2005B08E, pcie->pcie_pll_base + FNPLL_PCIE_LPF_BW_I_T01);
	writel(0x00003543, pcie->pcie_pll_base + FNPLL_PCIE_LPF_BW_P_T01);
	writel(0x00000000, pcie->pcie_pll_base + FNPLL_PCIE_ADPLL_DCO_TMODE);
	writel(0x00010000, pcie->pcie_pll_base + FNPLL_PCIE_ADPLL_LPF_TEST_CFG);
	writel(0x00000003, pcie->pcie_pll_base + FNPLL_PCIE_ADPLL_DSM_CFG);
	writel(0x000120FF, pcie->pcie_pll_base + FNPLL_PCIE_ADPLL_FTDC_LOCK_THR);
	writel(0x0080103F, pcie->pcie_pll_base + FNPLL_PCIE_ADPLL_FTDC_UNLOCK_THR);
	writel(0x80800A0A, pcie->pcie_pll_base + FNPLL_PCIE_CHAOS_CNT_THR_CFG);
	writel(0x8848D00F, pcie->pcie_pll_base + FNPLL_PCIE_ADPLL_CLK_EN_CFG0);
	writel(0x00000000, pcie->pcie_pll_base + FNPLL_PCIE_MAN_TRIG_LMS_CNT_CFG);
	writel(0x00100010, pcie->pcie_pll_base + FNPLL_PCIE_DTC_LINEAR_TEST_MODE);
	writel(0x0020A420, pcie->pcie_pll_base + FNPLL_PCIE_FREZ_LMS_PTHR);
	writel(0x01FE1FFF, pcie->pcie_pll_base + FNPLL_PCIE_ACQ_CNT_THR_CFG);
	writel(0x000FF102, pcie->pcie_pll_base + FNPLL_PCIE_ADPLL_DLY_CFG);
	writel(0x00000000, pcie->pcie_pll_base + FNPLL_PCIE_PTUNE_FBDIV);
}

static void pcie_phy_por_n(struct charlotte_pcie *pcie)
{
	u32 val;

	val = pcie_apb_phy_readl(pcie, SOC_PCIEPHY_CTRL1_ADDR);
	val |= PCIEPHY_POR_N_RESET_BIT;
	pcie_apb_phy_writel(pcie, val, SOC_PCIEPHY_CTRL1_ADDR);
}

static void pcie_ctrl_por_n_rst(struct charlotte_pcie *pcie, u32 enable)
{
	u32 reg, mask;

	if (enable)
		reg = HSDTCRG_PERRSTEN0;
	else
		reg = HSDTCRG_PERRSTDIS0;

	if (pcie->rc_id == PCIE_RC0)
		mask = PCIE_CTRL_POR_N;
	else
		mask = PCIE1_CTRL_POR_N;

	hsdt_crg_reg_write(pcie, mask, reg);
}

static void pcie_en_rx_term(struct charlotte_pcie *pcie)
{
	u32 val;

	val = pcie_apb_phy_readl(pcie, SOC_PCIEPHY_CTRL39_ADDR);
	val |= PCIEPHY_RX_TERMINATION_BIT;
	pcie_apb_phy_writel(pcie, val, SOC_PCIEPHY_CTRL39_ADDR);
}

static void pcie_phyio_hard_bypass(struct charlotte_pcie *pcie, bool enable)
{
	u32 val, reg_addr;

	if (pcie->rc_id == PCIE_RC0)
		reg_addr = HSDTCRG_PCIECTRL0;
	else
		reg_addr = HSDTCRG_PCIECTRL1;

	val = hsdt_crg_reg_read(pcie, reg_addr);
	if (enable)
		val &= ~PCIEIO_HW_BYPASS;
	else
		val |= PCIEIO_HW_BYPASS;
	hsdt_crg_reg_write(pcie, val, reg_addr);
}

static void pcie_phyref_hard_bypass(struct charlotte_pcie *pcie, bool enable)
{
	u32 val, reg_addr;

	if (pcie->rc_id == PCIE_RC0)
		reg_addr = HSDTCRG_PCIECTRL0;
	else
		reg_addr = HSDTCRG_PCIECTRL1;

	val = hsdt_crg_reg_read(pcie, reg_addr);
	if (enable)
		val &= ~PCIEPHY_REF_HW_BYPASS;
	else
		val |= PCIEPHY_REF_HW_BYPASS;
	hsdt_crg_reg_write(pcie, val, reg_addr);
}

static void pcie_phy_ref_clk_gt(struct charlotte_pcie *pcie, u32 enable)
{
	u32  mask, reg;

	/* HW bypass cfg */
	pcie_phyref_hard_bypass(pcie, enable);

	/* soft ref cfg,Always disable SW control */
	if (pcie->rc_id == PCIE_RC0) {
		mask = PCIE0_CLK_PHYREF_GATE;
		reg = HSDTCRG_PERDIS1;
	} else {
		mask = PCIE1_CLK_PHYREF_GATE;
		reg = HSDTCRG_PERDIS0;
	}
	hsdt_crg_reg_write(pcie, mask, reg);
}

static void pcie_oe_config(struct charlotte_pcie *pcie, bool enable)
{
	u32 val, reg_addr;

	if (pcie->rc_id == PCIE_RC0)
		reg_addr = HSDTCRG_PCIECTRL0;
	else
		reg_addr = HSDTCRG_PCIECTRL1;

	val = hsdt_crg_reg_read(pcie, reg_addr);
	val &= ~PCIEIO_OE_POLAR;
	val &= ~PCIEIO_OE_EN_SOFT;
	if (enable)
		val &= ~PCIEIO_OE_EN_HARD_BYPASS;
	else
		val |= PCIEIO_OE_EN_HARD_BYPASS;
	hsdt_crg_reg_write(pcie, val, reg_addr);
}

static void pcie_ie_config(struct charlotte_pcie *pcie, bool enable)
{
	u32 val, reg_addr;

	if (pcie->rc_id == PCIE_RC0)
		reg_addr = HSDTCRG_PCIECTRL0;
	else
		reg_addr = HSDTCRG_PCIECTRL1;

	val = hsdt_crg_reg_read(pcie, reg_addr);
	val &= ~PCIEIO_IE_POLAR;
	val &= ~PCIEIO_IE_EN_SOFT;
	if (enable)
		val &= ~PCIEIO_IE_EN_HARD_BYPASS;
	else
		val |= PCIEIO_IE_EN_HARD_BYPASS;
	hsdt_crg_reg_write(pcie, val, reg_addr);
}

static void pcie_ioref_gt(struct charlotte_pcie *pcie, u32 enable)
{
	u32 val, mask, reg;

	/* selcet cio */
	val = pcie_apb_ctrl_readl(pcie, SOC_PCIECTRL_CTRL21_ADDR);
	val &= ~IO_CLK_SEL_CLEAR;
	val |= IO_CLK_FROM_CIO;
	pcie_apb_ctrl_writel(pcie, val, SOC_PCIECTRL_CTRL21_ADDR);

	/*
	 * HW bypass: DISABLE:HW don't control
	 *            ENABLE:clkreq_n is one of controller
	 */
	pcie_phyio_hard_bypass(pcie, ENABLE);

	/* disable SW control */
	if (pcie->rc_id == PCIE_RC0) {
		mask = PCIE0_CLK_IO_GATE;
		reg = HSDTCRG_PERDIS1;
	} else {
		mask = PCIE1_CLK_IO_GATE;
		reg = HSDTCRG_PERDIS0;
	}
	hsdt_crg_reg_write(pcie, mask, reg);

	/* enable/disable ie/oe according mode */
	if (unlikely(pcie->dtsinfo.ep_flag)) {
		pcie_oe_config(pcie, DISABLE);
		pcie_ie_config(pcie, enable);
	} else {
		pcie_oe_config(pcie, enable);
		pcie_ie_config(pcie, DISABLE);
	}
}

static void pcie_hp_debounce_clk_gt(struct charlotte_pcie *pcie, u32 enable)
{
	u32 mask, reg;

	if (pcie->rc_id == PCIE_RC0) {
		mask = PCIE0_CLK_HP_GATE | PCIE0_CLK_DEBUNCE_GATE;
		if (enable)
			reg = HSDTCRG_PEREN1;
		else
			reg = HSDTCRG_PERDIS1;
	} else {
		mask = PCIE1_CLK_HP_GATE | PCIE1_CLK_DEBUNCE_GATE;
		if (enable)
			reg = HSDTCRG_PEREN0;
		else
			reg = HSDTCRG_PERDIS0;
	}

	hsdt_crg_reg_write(pcie, mask, reg);
}

static void pcie_reset_asyncbrg(struct charlotte_pcie *pcie, u32 enable)
{
	u32 mask, reg;

	if (enable)
		reg = HSDTCRG_PERRSTEN0;
	else
		reg = HSDTCRG_PERRSTDIS0;

	if (pcie->rc_id == PCIE_RC0)
		mask = PCIE_RST_PCIE0_ASYNCBRG;
	else
		mask = PCIE_RST_PCIE1_ASYNCBRG;

	hsdt_crg_reg_write(pcie, mask, reg);
}

static void unrst_ctrl_perst(struct charlotte_pcie *pcie)
{
	u32 val;

	val = pcie_apb_ctrl_readl(pcie, SOC_PCIECTRL_CTRL12_ADDR);
	val |= PCIE_PERST_IN_N_CTRL_BIT2;
	val &= ~PCIE_PERST_IN_N_CTRL_BIT3;
	pcie_apb_ctrl_writel(pcie, val, SOC_PCIECTRL_CTRL12_ADDR);
}

static void pcie_tp_mem_tra(struct charlotte_pcie *pcie)
{
	u32 val;

	val = pcie_apb_ctrl_readl(pcie, SOC_PCIECTRL_CTRL20_ADDR);
	val &= ~PCICTRL_TP_MEM_TRA_MASK;
	val |= PCICTRL_TP_MEM_TRA_BIT;
	pcie_apb_ctrl_writel(pcie, val, SOC_PCIECTRL_CTRL20_ADDR);
}

static void pcie_sp_mem_tra(struct charlotte_pcie *pcie)
{
	u32 val;

	val = pcie_apb_ctrl_readl(pcie, SOC_PCIECTRL_CTRL23_ADDR);
	val &= ~PCICTRL_SP_MEM_TRA_MASK;
	val |= PCICTRL_SP_MEM_TRA_BIT;
	pcie_apb_ctrl_writel(pcie, val, SOC_PCIECTRL_CTRL23_ADDR);
}

static void pcie_en_dbi_ep_splt(struct charlotte_pcie *pcie)
{
	u32 val;

	val = pcie_apb_ctrl_readl(pcie, SOC_PCIECTRL_CTRL30_ADDR);
	if (pcie->rc_id == PCIE_RC0)
		val &= ~PCIE_DBI_EP_SPLT_BIT;
	else
		val |= PCIE_DBI_EP_SPLT_BIT;
	pcie_apb_ctrl_writel(pcie, val, SOC_PCIECTRL_CTRL30_ADDR);
}

static void pcie_config_axi_timeout(struct charlotte_pcie *pcie)
{
	u32 val;

	val = pcie_apb_ctrl_readl(pcie, SOC_PCIECTRL_CTRL25_ADDR);
	val &= ~PCIE_CTRL_BUS_TIMEOUT_MASK;
	val |= PCIE_CTRL_BUS_TIMEOUT_VALUE;
	pcie_apb_ctrl_writel(pcie, val, SOC_PCIECTRL_CTRL25_ADDR);
}

static void pcie_natural_cfg(struct charlotte_pcie *pcie)
{
	u32 val;

	/* set clkreq low(not enable l1ss) */
	val = pcie_apb_ctrl_readl(pcie, SOC_PCIECTRL_CTRL22_ADDR);
	val &= ~PCIE_CLKREQ_OUT_MASK;
	pcie_apb_ctrl_writel(pcie, val, SOC_PCIECTRL_CTRL22_ADDR);

	/* pull up sys_aux_pwr_det bit[10] */
	val = pcie_apb_ctrl_readl(pcie, SOC_PCIECTRL_CTRL7_ADDR);
	val |= (0x1 << 10);
	pcie_apb_ctrl_writel(pcie, val, SOC_PCIECTRL_CTRL7_ADDR);

	if (pcie->dtsinfo.ep_flag) {
		/* cfg as ep */
		val = pcie_apb_ctrl_readl(pcie, SOC_PCIECTRL_CTRL0_ADDR);
		val &= 0xFFFFFFF;
		pcie_apb_ctrl_writel(pcie, val, SOC_PCIECTRL_CTRL0_ADDR);
		/* input */
		val = pcie_apb_ctrl_readl(pcie, SOC_PCIECTRL_CTRL12_ADDR);
		val |= PCIE_PERST_IN_N_CTRL_11B;
		pcie_apb_ctrl_writel(pcie, val, SOC_PCIECTRL_CTRL12_ADDR);
	} else {
		/* cfg as rc */
		val = pcie_apb_ctrl_readl(pcie, SOC_PCIECTRL_CTRL0_ADDR);
		val &= ~(PCIE_TYPE_MASK << PCIE_TYPE_SHIFT);
		val |= (PCIE_TYPE_RC << PCIE_TYPE_SHIFT);
		pcie_apb_ctrl_writel(pcie, val, SOC_PCIECTRL_CTRL0_ADDR);

		/* output, pull down */
		val = pcie_apb_ctrl_readl(pcie, SOC_PCIECTRL_CTRL12_ADDR);
		val &= ~PCIE_PERST_IN_N_CTRL_11B;
		val |= PCIE_PERST_OE_EN;
		val &= ~PCIE_PERST_OUT_N;
		pcie_apb_ctrl_writel(pcie, val, SOC_PCIECTRL_CTRL12_ADDR);
	}

	/* Handle phy_reset and lane0_reset to HW */
	val = pcie_apb_phy_readl(pcie, SOC_PCIEPHY_CTRL1_ADDR);
	val |= PCIEPHY_RESET_BIT;
	val &= ~PCIEPHY_PIPE_LINE0_RESET_BIT;
	pcie_apb_phy_writel(pcie, val, SOC_PCIEPHY_CTRL1_ADDR);
}

