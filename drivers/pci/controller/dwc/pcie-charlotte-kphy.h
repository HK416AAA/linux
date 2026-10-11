/* SPDX-License-Identifier: GPL-2.0-only */
/* Huawei KPHY v1 table/FFE setup, adapted from pcie-kport-phy-kphy.c.
 * Copyright (c) Huawei Technologies Co., Ltd. 2020.
 */
#include "pcie-charlotte-kphy-regs.h"
static u32 pcie_phy_init_tbl[][2] =
{
	{PHY_PLL_REFCLK_SEL_VAL, PHY_PLL_REFCLK_SEL_ADDR},           /* PLL ref clk select */
	{PHY_REG_DA_DFE_CTRL_EN_VAL, PHY_REG_DA_DFE_CTRL_EN_ADDR},   /* dfe_en = 0 */
	{PHY_REG_DA_DFE_CTRL_VAL_VAL, PHY_REG_DA_DFE_CTRL_VAL_ADDR},
	{PHY_RG_BG_VREF_SEL_VAL, PHY_RG_BG_VREF_SEL_ADDR},           /* bg mt en = 1 */
	{PHY_PLL_UPDATE_CTRL_VAL, PHY_PLL_UPDATE_CTRL_ADDR},         /* bias_stben/ldo_stben */
	{PHY_REG_DA_TX_EQ_CTRL_VAL_VAL, PHY_REG_DA_TX_EQ_CTRL_ADDR}, /* close tx ffe */
	{PHY_REG_DA_TX_EQ_FFE_VAL, PHY_REG_DA_TX_EQ_FFE},
	{PHY_RXUPDTCTRL_VAL, PHY_RXUPDTCTRL_EN_ADDR},                /* gs eq en */
	{PHY_DFE_MANUAL_EN_VAL, PHY_DFE_MANUAL_EN_ADDR},             /* sel_r and sel_gain manual controls */
	{PHY_RG_RX_CFG_ADDED4_VAL, PHY_RG_RX_CFG_ADDED4_ADDR},
	{PHY_DFE_EN_VAL, PHY_DFE_EN_ADDR},                           /* dfe tap3/2/1_en = 0 */
	{PHY_PMA_RX_ATT_RG_RX0_VAL, PHY_PMA_RX_ATT_RG_RX0_ADDR},     /* PMA RX ATT GEN1-GEN3     PMA RG_RX0_EQ_R GEN1-GEN3 */
	{PHY_RX_EQA_CTRL_SEL_C_VAL, PHY_RX_EQA_CTRL_ADDR},           /* rxctrl eqa sel c */
	{PHY_RX_EQA_TH_VAL, PHY_RX_EQA_CTRL_ADDR},                   /* rxctrl eqa th */
	{PHY_RG_RX0_GAIN_SEL_VAL, PHY_RG_RX0_GAIN_SEL_ADDR},         /* reg RX0 GAIN select */
	{PHY_RG_G1_KI_FINE_VAL, PHY_RG_G1_KI_FINE_ADDR},             /* GEN1 KI fine */
	{PHY_RG_G2_G3_KI_FINE_VAL, PHY_RG_G2_G3_KI_FINE_ADDR},       /* GEN2 GEN3 KI fine */
	{PHY_RG_RX0_LOS_HYS_VAL, PHY_RG_RX0_LOS_HYS_ADDR},           /* reg RX0 LOS HYS */
	{PHY_RG_RX0_GAIN_VREF_VAL, PHY_RG_RX0_GAIN_VREF_ADDR},       /* reg RX0 GAIN VREF */
	{PHY_RG_RX_CTRL_GSA_TH_VAL, PHY_RG_RX0_GAIN_VREF_ADDR},      /* rxctrl gsa th */
	{PHY_RG_RX0_EQ_PAT_MODE_CFG_VAL, PHY_RG_RX0_EQ_PAT_MODE_CFG_ADDR}, /* EQ pattern and EQ CFG */
	{PHY_DCC_WAITING_TIME_VAL, PHY_DCC_WAITING_TIME_ADDR},       /* DCC waiting time */
	{PHY_RXOFSTCTRL_VAL, PHY_RXOFSTCTRL_ADDR},
	{PHY_TX_TERMK_WATING_TIME_VAL, PHY_TX_TERMK_WATING_TIME_ADDR}, /* TX TERMK waiting time */
	{PHY_TX_TERMK_WATING_TIME1_2_VAL, PHY_TX_TERMK_WATING_TIME1_2_ADDR},
#if 1
	{PHY_TX_DRV_TRANSMIT_TIME_VAL,PHY_TX_DRV_TRANSMIT_TIME_ADDR},
#else
	{PHY_PLL_LOCK_WAIT_TIME_VAL, PHY_PLL_LOCK_WAIT_TIME_ADDR},  /* pll lock keep time */
	{PHY_PMA_CTRL_OTHER_TMR0_VAL, PHY_PMA_CTRL_OTHER_TMR0_ADDR},
#endif
	{PHY_TERMKCTRL0_VAL, PHY_TERMKCTRL0_ADDR},
	{PHY_PHY_TXEQVBST_VAL, PHY_TXEQVBST_ADDR},
};

static inline void kphy_phy_write(struct charlotte_pcie_phy *phy, uint32_t val, uint32_t reg)
{
	writel(val, phy->phy_base + reg);
}

static inline uint32_t kphy_phy_read(struct charlotte_pcie_phy *phy, uint32_t reg)
{
	return readl(phy->phy_base + reg);
}

static void collect_phy3_info(struct charlotte_pcie_phy *phy)
{
	uint32_t reg_val;

	reg_val = kphy_phy_read(phy, PHY_DCC_INTR_FLAG);
	pr_debug("phy3: dcc overflow/underflow intr flag: 0x%x %d\n", reg_val,
			(reg_val & PHY_DCC_INTR_FLAG_MASK) >> PHY_DCC_INTR_FLAG_OFFSET);

	reg_val = kphy_phy_read(phy, PHY_TX_RX_TERMK_INTR_FLAG);
	pr_debug("phy3: tx termk overflow/underflow intr: 0x%x %d\n", reg_val,
			(reg_val & PHY_TX_TERMK_INTR_FLAG_MASK) >> PHY_TX_TERMK_INTR_FLAG_OFFSET);
	pr_debug("phy3: rx termk overflow/underflow intr: 0x%x %d\n", reg_val,
			(reg_val & PHY_RX_TERMK_INTR_FLAG_MASK) >> PHY_RX_TERMK_INTR_FLAG_OFFSET);

	reg_val = kphy_phy_read(phy, PHY_PLL_GEN12_VCO_BAND);
	pr_debug("phy3: vco band value in GEN1/2: 0x%x\n", reg_val);

	reg_val = kphy_phy_read(phy, PHY_PLL_GEN3_VCO_BAND);
	pr_debug("phy3: vco band value in GEN3: 0x%x\n", reg_val);

	reg_val = kphy_phy_read(phy, PHY_DCC_CALI_RES);
	pr_debug("phy3: dcc calibration result: 0x%x\n", reg_val);

	reg_val = kphy_phy_read(phy, PHY_TX_TERMK_RES);
	pr_debug("phy3: tx termk result: 0x%x\n", reg_val);

	reg_val = kphy_phy_read(phy, PHY_RX_TERMK_RES);
	pr_debug("phy3: rx termk result: 0x%x\n", reg_val);
}

static void kphy_phy_init_v1(struct charlotte_pcie_phy *phy)
{
	uint32_t i;
	size_t array_size;

	

	array_size = ARRAY_SIZE(pcie_phy_init_tbl);
	for (i = 0; i < array_size; i++)
		kphy_phy_write(phy, pcie_phy_init_tbl[i][0], pcie_phy_init_tbl[i][1]);

	
}

static bool kphy_phy_ready(struct charlotte_pcie_phy *phy)
{
	uint32_t reg_val, lock_res;
	int timeout = 0;

	while(1) {
		reg_val = kphy_phy_read(phy, PHY_PCS_POWER_STATUS_ADDR);
		reg_val &= ~PHY_PCS_STATUS_MASK;
		if ((reg_val >> PHY_POWERON_STATUS_BIT) & 1) {
			pr_debug("PHY3 power on in %d us\n", timeout);

			lock_res = kphy_phy_read(phy, PHY_DIGITAL_PLL_LOCK_RES);
			pr_debug("phy3: digital pll lock result: 0x%x\n", lock_res);
			return true;
		}

		udelay(5);
		timeout++;
		if (timeout > PHY_POWERON_TIMEOUT) {
			pr_err("PHY3 power on timeout!\n");
			return false;
		}
	}

	return true;
}

static void kphy_phy_ffe_cfg(struct charlotte_pcie_phy *phy)
{
	uint32_t reg_tx_rtermk;

	udelay(350);
	reg_tx_rtermk = kphy_phy_read(phy, PHY_RG_TX_RTERMK_V48_ADDR);
	reg_tx_rtermk = (reg_tx_rtermk >> PHY_RG_TX_RTERMK_V48_BIT) & 1;
	if (reg_tx_rtermk) {
		kphy_phy_write(phy, PHY_TXDEEMPHCTRL6DB_VAL, PHY_TXDEEMPHCTRL6DB_ADDR);
		kphy_phy_write(phy, PHY_TXDEEMPHCTRL3D5DB_VAL, PHY_TXDEEMPHCTRL3D5DB_ADDR);
		kphy_phy_write(phy, PHY_TXDEEMPHCTRL0DB_VAL, PHY_TXDEEMPHCTRL0DB_ADDR);
		if (phy->version == 0)
			kphy_phy_write(phy, PHY_RG_PIPE_TXDEEMPH_CTRL_VAL, PHY_RG_PIPE_TXDEEMPH_CTRL_ADDR);
		/* config FS, LF */
		kphy_phy_write(phy, PHY_RG_LOCAL_LF_FS_VAL, PHY_RG_LOCAL_LF_FS_ADDR);
	} else {
		/* config FS, LF */
		kphy_phy_write(phy, PHY_RG_LOCAL_LF_FS_V48_VAL, PHY_RG_LOCAL_LF_FS_ADDR);
	}

	collect_phy3_info(phy);

	phy->rtermk_v48 = reg_tx_rtermk;
}

