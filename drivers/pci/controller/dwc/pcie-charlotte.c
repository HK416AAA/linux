// SPDX-License-Identifier: GPL-2.0-only
/* Charlotte PCIe0 bring-up, derived from Huawei pcie-jun.c/KPHY.
 * Copyright (c) Huawei Technologies Co., Ltd. 2016-2020.
 * Currently validates the RC/PHY independently of the WLAN driver.
 */
#include <linux/delay.h>
#include <linux/clk-provider.h>
#include <linux/clkdev.h>
#include <linux/firmware/charlotte-ffa.h>
#include <linux/gpio/consumer.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/spmi.h>
#include <linux/spinlock.h>
#include <linux/platform_drivers/pcie-kport-api.h>
#include "pcie-designware.h"

struct charlotte_pcie_phy {
	void __iomem *phy_base;
	u32 version, rtermk_v48;
};

struct charlotte_pcie {
	struct dw_pcie pci;
	void __iomem *apb_base, *phy_base, *apb_phy_base, *crg_base, *pmctrl_base;
	void __iomem *pcie_pll_base, *aux_crg;
	struct { bool ep_flag; } dtsinfo;
	u32 rc_id;
	bool powered, initialized;
	struct charlotte_pcie_phy phy;
	struct gpio_desc *chip_power, *wlan_power, *tcxo;
	struct spmi_device *pmic;
	u8 pmu32k_saved;
	bool pmu32k_owned;
	u32 gates_saved, aux_gates_saved, reset_saved;
	struct mutex init_lock;
	/* PCI config may run with IRQs disabled; never use init_lock there.
	 * Lock order is config_lock -> dbi_lock when both are needed.
	 */
	raw_spinlock_t config_lock, dbi_lock;
	struct pci_ops root_config_ops, child_config_ops;
	const struct pci_ops *root_config_parent, *child_config_parent;
	struct clk_hw pmu32k_hw;
	bool transport;
	int (*endpoint_on)(void *);
	int (*endpoint_off)(void *);
	void *endpoint_data;
	struct pci_dev *root_port;
	struct mutex event_lock;
	struct pcie_kport_register_event *event;
	struct work_struct linkdown_work;
	int linkdown_irq;
	struct mutex callback_lock;
};

static struct charlotte_pcie *charlotte_pcie0;

static u32 pcie_apb_ctrl_readl(struct charlotte_pcie *pcie, u32 reg)
{
	return readl(pcie->apb_base + reg);
}

static void pcie_apb_ctrl_writel(struct charlotte_pcie *pcie, u32 val, u32 reg)
{
	writel(val, pcie->apb_base + reg);
}

static u32 pcie_apb_phy_readl(struct charlotte_pcie *pcie, u32 reg)
{
	return readl(pcie->apb_phy_base + reg);
}

static void pcie_apb_phy_writel(struct charlotte_pcie *pcie, u32 val, u32 reg)
{
	writel(val, pcie->apb_phy_base + reg);
}

#include "pcie-charlotte-power.h"
#include "pcie-charlotte-kphy.h"

static int charlotte_pcie_32k_on(struct charlotte_pcie *pcie)
{
	u8 val;
	int ret;

	/* clk_pmu32kb: main PMIC register 0x4a, bit 0, clk-char-cs.c. */
	ret = spmi_ext_register_readl(pcie->pmic, 0x4a, &val, 1);
	if (ret)
		return ret;
	pcie->pmu32k_saved = val;
	val |= BIT(0);
	ret = spmi_ext_register_writel(pcie->pmic, 0x4a, &val, 1);
	if (!ret)
		pcie->pmu32k_owned = true;
	return ret;
}

static void charlotte_pcie_32k_off(struct charlotte_pcie *pcie)
{
	u8 val;

	if (!pcie->pmu32k_owned)
		return;
	if (!spmi_ext_register_readl(pcie->pmic, 0x4a, &val, 1)) {
		val = (val & ~BIT(0)) | (pcie->pmu32k_saved & BIT(0));
		spmi_ext_register_writel(pcie->pmic, 0x4a, &val, 1);
	}
	pcie->pmu32k_owned = false;
}

static void charlotte_pcie_put_pmic(void *data)
{
	struct spmi_device *pmic = data;

	put_device(&pmic->dev);
}

static int charlotte_pcie_32k_prepare(struct clk_hw *hw)
{
	return charlotte_pcie_32k_on(container_of(hw, struct charlotte_pcie, pmu32k_hw));
}

static void charlotte_pcie_32k_unprepare(struct clk_hw *hw)
{
	charlotte_pcie_32k_off(container_of(hw, struct charlotte_pcie, pmu32k_hw));
}

static unsigned long charlotte_pcie_32k_rate(struct clk_hw *hw, unsigned long parent)
{
	return 32768;
}

static const struct clk_ops charlotte_pcie_32k_ops = {
	.prepare = charlotte_pcie_32k_prepare,
	.unprepare = charlotte_pcie_32k_unprepare,
	.recalc_rate = charlotte_pcie_32k_rate,
};

static int charlotte_pcie_endpoint(struct charlotte_pcie *pcie, bool on)
{
	int (*callback)(void *);
	void *data;

	mutex_lock(&pcie->callback_lock);
	callback = on ? pcie->endpoint_on : pcie->endpoint_off;
	data = pcie->endpoint_data;
	mutex_unlock(&pcie->callback_lock);
	if (!callback)
		return on ? -ENODEV : 0;
	return callback(data);
}

/* W1S/W1C gate registers: preserve every unrelated peripheral/RC1 bit. */
static void charlotte_pcie_gates(struct charlotte_pcie *pcie, bool on)
{
	u32 mask = BIT(5) | BIT(10) | BIT(11);

	if (!on)
		mask &= ~pcie->gates_saved;
	writel(mask, pcie->crg_base + (on ? 0x10 : 0x14));
	/* clk_hsdt0_pcie_aux and clk_pcie0_aux, clk-char-cs.c. */
	mask = BIT(8) | BIT(19);
	if (!on)
		mask &= ~pcie->aux_gates_saved;
	writel(mask, pcie->aux_crg + (on ? 0x170 : 0x174));
}

static int charlotte_pcie_pll_on(struct charlotte_pcie *pcie)
{
	u32 val;
	int ret;

	pcie_fnpll_cfg_init_set(pcie);
	udelay(1);
	writel(2, pcie->crg_base + 0x190);
	writel(0x2ee00000, pcie->crg_base + 0x194);
	udelay(5);
	writel(3, pcie->crg_base + 0x190);
	ret = readl_poll_timeout_atomic(pcie->crg_base + 0x204, val,
				       val & BIT(4), 1, 200);
	if (ret) {
		writel(2, pcie->crg_base + 0x190);
		return ret;
	}
	writel(readl(pcie->crg_base + 0x190) & ~BIT(1), pcie->crg_base + 0x190);
	pcie_apb_phy_writel(pcie, pcie_apb_phy_readl(pcie, 0) & ~BIT(14), 0);
	pcie_hp_debounce_clk_gt(pcie, ENABLE);
	pcie_phy_ref_clk_gt(pcie, ENABLE);
	pcie_ioref_gt(pcie, ENABLE);
	return 0;
}

static void charlotte_pcie_pll_off(struct charlotte_pcie *pcie)
{
	pcie_hp_debounce_clk_gt(pcie, DISABLE);
	pcie_ioref_gt(pcie, DISABLE);
	pcie_phy_ref_clk_gt(pcie, DISABLE);
	writel(2, pcie->crg_base + 0x190);
}

static int charlotte_pcie_power_off(struct charlotte_pcie *pcie)
{
	unsigned long flags;

	if (!pcie->powered)
		return 0;
	/* Drain config/DBI accesses and refuse new ones before NOC idle.
	 * Even the RC's own config space hangs the CPU when its clocks are off.
	 */
	raw_spin_lock_irqsave(&pcie->config_lock, flags);
	raw_spin_lock(&pcie->dbi_lock);
	pcie->powered = false;
	raw_spin_unlock(&pcie->dbi_lock);
	raw_spin_unlock_irqrestore(&pcie->config_lock, flags);
	/* Stop new traffic before taking the interconnect into idle. */
	pcie_apb_ctrl_writel(pcie, pcie_apb_ctrl_readl(pcie, 0x1c) & ~BIT(11), 0x1c);
	if (pcie_noc_power_jun(pcie, 0)) {
		dev_err(pcie->pci.dev, "NOC idle failed; leave clocks on\n");
		raw_spin_lock_irqsave(&pcie->dbi_lock, flags);
		pcie->powered = true;
		raw_spin_unlock_irqrestore(&pcie->dbi_lock, flags);
		return -ETIMEDOUT;
	}
	if (pcie->transport) {
		charlotte_pcie_endpoint(pcie, false);
	} else {
		gpiod_set_value_cansleep(pcie->wlan_power, 0);
	}
	pcie_apb_ctrl_writel(pcie, pcie_apb_ctrl_readl(pcie, 0x30) & ~BIT(0), 0x30);
	/* TCU is shared with RC1; restore its pre-probe reset state. */
	writel(BIT(5) | (pcie->reset_saved & BIT(3)), pcie->crg_base + 0x60);
	pcie_ctrl_por_n_rst(pcie, ENABLE);
	charlotte_pcie_gates(pcie, false);
	charlotte_pcie_pll_off(pcie);
	writel(0x300, pcie->crg_base + 0x60);
	writel(BIT(3) | BIT(4), pcie->crg_base + 0x14);
	if (!pcie->transport) {
		gpiod_set_value_cansleep(pcie->chip_power, 0);
		charlotte_pcie_32k_off(pcie);
	}
	return 0;
}

static int charlotte_pcie_power_on(struct charlotte_pcie *pcie)
{
	unsigned long flags;
	u32 val, trim;
	int ret;

	/* Refuse to touch the powered-down fabric until the secure service exists. */
	ret = charlotte_ffa_pcie_phy_trim(&trim);
	if (ret)
		return ret;
	dev_info(pcie->pci.dev, "PHY efuse=%#x, initializing PCIe0\n", trim);
	pcie->gates_saved = readl(pcie->crg_base + 0x18);
	pcie->aux_gates_saved = readl(pcie->aux_crg + 0x178);
	pcie->reset_saved = readl(pcie->crg_base + 0x68);
	dev_info(pcie->pci.dev, "stage: APB clocks and reset\n");
	writel(BIT(3) | BIT(4), pcie->crg_base + 0x10);
	writel(0x300, pcie->crg_base + 0x64);
	pcie_reset_asyncbrg(pcie, DISABLE);
	pcie_config_axi_timeout(pcie);
	pcie_phy_por_n(pcie);
	pcie_natural_cfg(pcie);
	dev_info(pcie->pci.dev, "stage: FNPLL\n");
	ret = charlotte_pcie_pll_on(pcie);
	if (ret)
		goto apb_off;
	/* Select the stock 19.2 MHz AUX parent (clkmux_pcieaux, hiword mask). */
	writel(BIT(30), pcie->aux_crg + 0x288);
	charlotte_pcie_gates(pcie, true);
	dev_info(pcie->pci.dev, "stage: KPHY table\n");
	kphy_phy_init_v1(&pcie->phy);
	/* Same perst deassertion as pcie_port_phy_init(). */
	pcie_apb_ctrl_writel(pcie, pcie_apb_ctrl_readl(pcie, 0x30) | BIT(2), 0x30);
	udelay(10);
	pcie_ctrl_por_n_rst(pcie, DISABLE);
	dev_info(pcie->pci.dev, "stage: PMU32K and endpoint power\n");
	if (pcie->transport) {
		ret = charlotte_pcie_endpoint(pcie, true);
		if (ret)
			goto clocks_off;
	} else {
		ret = charlotte_pcie_32k_on(pcie);
		if (ret)
			goto clocks_off;
		gpiod_set_value_cansleep(pcie->tcxo, 1);
		gpiod_set_value_cansleep(pcie->chip_power, 1);
		msleep(20);
		gpiod_set_value_cansleep(pcie->wlan_power, 1);
	}
	usleep_range(21000, 23000);
	unrst_ctrl_perst(pcie);
	pcie_sp_mem_tra(pcie);
	pcie_tp_mem_tra(pcie);
	writel(BIT(3) | BIT(5), pcie->crg_base + 0x64);
	dev_info(pcie->pci.dev, "stage: PIPE clock and PHY ready\n");
	dev_info(pcie->pci.dev, "PHY APB ctrl0=%#x ctrl1=%#x state0=%#x PCS=%#x\n",
		pcie_apb_phy_readl(pcie, 0), pcie_apb_phy_readl(pcie, 4),
		pcie_apb_phy_readl(pcie, 0x400), readl(pcie->phy_base + 0x1400));
	ret = readl_poll_timeout(pcie->apb_phy_base + 0x400, val, !(val & BIT(19)), 1000, 100000);
	if (ret)
		goto clocks_off;
	kphy_phy_ffe_cfg(&pcie->phy);
	if (!kphy_phy_ready(&pcie->phy)) {
		ret = -ETIMEDOUT;
		goto clocks_off;
	}
	pcie_en_rx_term(pcie);
	dev_info(pcie->pci.dev, "stage: NOC active\n");
	ret = pcie_noc_power_jun(pcie, 1);
	if (ret)
		goto clocks_off;
	pcie_en_dbi_ep_splt(pcie);
	dev_info(pcie->pci.dev, "stage: secure TBU access\n");
	ret = charlotte_ffa_pcie_access(0);
	if (ret) {
		pcie_noc_power_jun(pcie, 0);
		goto clocks_off;
	}
	raw_spin_lock_irqsave(&pcie->dbi_lock, flags);
	pcie->powered = true;
	raw_spin_unlock_irqrestore(&pcie->dbi_lock, flags);
	dev_info(pcie->pci.dev, "stage: first DBI access\n");
	/* Vendor L1SS exit fix, accessible only after clocks/NOC/FFA succeed. */
	val = dw_pcie_readl_dbi(&pcie->pci, 0x890);
	dw_pcie_writel_dbi(&pcie->pci, 0x890, val & ~BIT(0));
	usleep_range(7000, 8000);
	dev_info(pcie->pci.dev, "PHY ready, APB=%#x DBI=%#x\n",
		pcie_apb_ctrl_readl(pcie, 0x400), dw_pcie_readl_dbi(&pcie->pci, PCI_VENDOR_ID));
	return 0;
clocks_off:
	if (pcie->transport) {
		charlotte_pcie_endpoint(pcie, false);
	} else {
		gpiod_set_value_cansleep(pcie->wlan_power, 0);
		gpiod_set_value_cansleep(pcie->chip_power, 0);
		charlotte_pcie_32k_off(pcie);
	}
	writel(BIT(5) | (pcie->reset_saved & BIT(3)), pcie->crg_base + 0x60);
	pcie_ctrl_por_n_rst(pcie, ENABLE);
	charlotte_pcie_gates(pcie, false);
	charlotte_pcie_pll_off(pcie);
apb_off:
	writel(0x300, pcie->crg_base + 0x60);
	writel(BIT(3) | BIT(4), pcie->crg_base + 0x14);
	return ret;
}

static bool charlotte_pcie_link_up(struct dw_pcie *pci)
{
	struct charlotte_pcie *pcie = container_of(pci, struct charlotte_pcie, pci);
	unsigned long flags;
	bool up;

	raw_spin_lock_irqsave(&pcie->dbi_lock, flags);
	up = pcie->powered && (pcie_apb_ctrl_readl(pcie, 0x400) & 0x8020) == 0x8020;
	raw_spin_unlock_irqrestore(&pcie->dbi_lock, flags);
	return up;
}

static u32 charlotte_pcie_read_dbi(struct dw_pcie *pci, void __iomem *base,
				 u32 reg, size_t size)
{
	struct charlotte_pcie *pcie = container_of(pci, struct charlotte_pcie, pci);
	unsigned long flags;
	u32 val = ~0U;

	raw_spin_lock_irqsave(&pcie->dbi_lock, flags);
	if (pcie->powered)
		dw_pcie_read(base + reg, size, &val);
	raw_spin_unlock_irqrestore(&pcie->dbi_lock, flags);
	return val;
}

static void charlotte_pcie_write_dbi(struct dw_pcie *pci, void __iomem *base,
				   u32 reg, size_t size, u32 val)
{
	struct charlotte_pcie *pcie = container_of(pci, struct charlotte_pcie, pci);
	unsigned long flags;

	raw_spin_lock_irqsave(&pcie->dbi_lock, flags);
	if (pcie->powered)
		dw_pcie_write(base + reg, size, val);
	raw_spin_unlock_irqrestore(&pcie->dbi_lock, flags);
}

static int charlotte_pcie_config_read(struct pci_bus *bus, unsigned int devfn,
				      int where, int size, u32 *val)
{
	struct dw_pcie_rp *pp = bus->sysdata;
	struct charlotte_pcie *pcie = container_of(to_dw_pcie_from_pp(pp), struct charlotte_pcie, pci);
	const struct pci_ops *parent = pci_is_root_bus(bus) ?
		pcie->root_config_parent : pcie->child_config_parent;
	unsigned long flags;
	int ret = PCIBIOS_DEVICE_NOT_FOUND;

	*val = ~0U;
	raw_spin_lock_irqsave(&pcie->config_lock, flags);
	if (READ_ONCE(pcie->powered))
		ret = parent->read(bus, devfn, where, size, val);
	raw_spin_unlock_irqrestore(&pcie->config_lock, flags);
	return ret;
}

static int charlotte_pcie_config_write(struct pci_bus *bus, unsigned int devfn,
				       int where, int size, u32 val)
{
	struct dw_pcie_rp *pp = bus->sysdata;
	struct charlotte_pcie *pcie = container_of(to_dw_pcie_from_pp(pp), struct charlotte_pcie, pci);
	const struct pci_ops *parent = pci_is_root_bus(bus) ?
		pcie->root_config_parent : pcie->child_config_parent;
	unsigned long flags;
	int ret = PCIBIOS_DEVICE_NOT_FOUND;

	raw_spin_lock_irqsave(&pcie->config_lock, flags);
	if (READ_ONCE(pcie->powered))
		ret = parent->write(bus, devfn, where, size, val);
	raw_spin_unlock_irqrestore(&pcie->config_lock, flags);
	return ret;
}

static int charlotte_pcie_start_link(struct dw_pcie *pci)
{
	struct charlotte_pcie *pcie = container_of(pci, struct charlotte_pcie, pci);

	pcie_apb_ctrl_writel(pcie, pcie_apb_ctrl_readl(pcie, 0x1c) | BIT(11), 0x1c);
	return 0;
}

static void charlotte_pcie_stop_link(struct dw_pcie *pci)
{
	struct charlotte_pcie *pcie = container_of(pci, struct charlotte_pcie, pci);

	if (pcie->powered)
		pcie_apb_ctrl_writel(pcie, pcie_apb_ctrl_readl(pcie, 0x1c) & ~BIT(11), 0x1c);
}

#include "pcie-charlotte-transport.h"

/* DBI split mode uses direct register accesses, as CONFIG_PCIE_KPORT_DBI_SPLT. */
static const struct dw_pcie_ops charlotte_pcie_ops = {
	.read_dbi = charlotte_pcie_read_dbi,
	.write_dbi = charlotte_pcie_write_dbi,
	.write_dbi2 = charlotte_pcie_write_dbi,
	.link_up = charlotte_pcie_link_up,
	.start_link = charlotte_pcie_start_link,
	.stop_link = charlotte_pcie_stop_link,
};

static int charlotte_pcie_host_init(struct dw_pcie_rp *pp)
{
	return charlotte_pcie_power_on(container_of(to_dw_pcie_from_pp(pp), struct charlotte_pcie, pci));
}

static void charlotte_pcie_host_deinit(struct dw_pcie_rp *pp)
{
	charlotte_pcie_power_off(container_of(to_dw_pcie_from_pp(pp), struct charlotte_pcie, pci));
}

static int charlotte_pcie_intx_only(struct dw_pcie_rp *pp)
{
	/* The stock Hi1105 transport uses INTx. No MSI domain is advertised. */
	pp->bridge->msi_domain = false;
	return 0;
}

static void charlotte_pcie_host_post_init(struct dw_pcie_rp *pp)
{
	struct charlotte_pcie *pcie = container_of(to_dw_pcie_from_pp(pp), struct charlotte_pcie, pci);
	struct pci_bus *bus;

	/* DWC generic PCI config accesses bypass read_dbi/write_dbi. Preserve
	 * its map_bus/iATU logic, but serialize the entire transaction with off.
	 * This controller has one directly attached endpoint and no PCI switch.
	 */
	pcie->root_config_parent = pp->bridge->ops;
	pcie->root_config_ops = *pcie->root_config_parent;
	pcie->root_config_ops.read = charlotte_pcie_config_read;
	pcie->root_config_ops.write = charlotte_pcie_config_write;
	pcie->child_config_parent = pp->bridge->child_ops;
	pcie->child_config_ops = *pcie->child_config_parent;
	pcie->child_config_ops.read = charlotte_pcie_config_read;
	pcie->child_config_ops.write = charlotte_pcie_config_write;
	pp->bridge->ops = &pcie->root_config_ops;
	pp->bridge->child_ops = &pcie->child_config_ops;
	pp->bridge->bus->ops = &pcie->root_config_ops;
	list_for_each_entry(bus, &pp->bridge->bus->children, node)
		bus->ops = &pcie->child_config_ops;

	pcie->root_port = pci_get_slot(pp->bridge->bus, PCI_DEVFN(0, 0));
	if (pcie->root_port)
		pci_save_state(pcie->root_port);
}

static const struct dw_pcie_host_ops charlotte_pcie_host_ops = {
	.init = charlotte_pcie_host_init,
	.deinit = charlotte_pcie_host_deinit,
	.msi_init = charlotte_pcie_intx_only,
	.post_init = charlotte_pcie_host_post_init,
};

static ssize_t enable_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct charlotte_pcie *pcie = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", READ_ONCE(pcie->initialized));
}

static ssize_t enable_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct charlotte_pcie *pcie = dev_get_drvdata(dev);
	bool on;
	int ret;

	ret = kstrtobool(buf, &on);
	if (ret)
		return ret;
	if (!on)
		return -EOPNOTSUPP;
	mutex_lock(&pcie->init_lock);
	ret = 0;
	if (!pcie->initialized) {
		dev_info(dev, "manual PCIe0 initialization\n");
		ret = dw_pcie_host_init(&pcie->pci.pp);
		if (!ret)
			pcie->initialized = true;
	}
	mutex_unlock(&pcie->init_lock);
	return ret ?: count;
}
static DEVICE_ATTR_RW(enable);

static int charlotte_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct charlotte_pcie *pcie;
	struct resource *res;
	struct device_node *np;
	u32 phy_layout[3];
	struct clk_init_data clk_init = {
		.name = "charlotte-wlan-32k", .ops = &charlotte_pcie_32k_ops,
	};
	int ret;

	pcie = devm_kzalloc(dev, sizeof(*pcie), GFP_KERNEL);
	if (!pcie)
		return -ENOMEM;
	pcie->pci.dev = dev;
	pcie->pci.ops = &charlotte_pcie_ops;
	pcie->pci.pp.ops = &charlotte_pcie_host_ops;
	pcie->pci.num_lanes = 1;
	pcie->pci.max_link_speed = 2;
	mutex_init(&pcie->init_lock);
	raw_spin_lock_init(&pcie->config_lock);
	raw_spin_lock_init(&pcie->dbi_lock);
	mutex_init(&pcie->callback_lock);
	mutex_init(&pcie->event_lock);
	INIT_WORK(&pcie->linkdown_work, charlotte_pcie_linkdown_work);
	pcie->transport = IS_ENABLED(CONFIG_CHARLOTTE_HI11XX_WIFI);
	platform_set_drvdata(pdev, pcie);
	np = of_parse_phandle(dev->of_node, "hisilicon,pmic", 0);
	if (!np)
		return -EINVAL;
	pcie->pmic = spmi_find_device_by_of_node(np);
	of_node_put(np);
	if (!pcie->pmic)
		return -EPROBE_DEFER;
	ret = devm_add_action_or_reset(dev, charlotte_pcie_put_pmic, pcie->pmic);
	if (ret)
		return ret;
	pcie->pmu32k_hw.init = &clk_init;
	ret = devm_clk_hw_register(dev, &pcie->pmu32k_hw);
	if (ret)
		return ret;
	ret = devm_clk_hw_register_clkdev(dev, &pcie->pmu32k_hw, "clk_pmu32kb", NULL);
	if (ret)
		return ret;
#define MAP_RESOURCE(field, name) \
	pcie->field = devm_platform_ioremap_resource_byname(pdev, name); \
	if (IS_ERR(pcie->field)) return PTR_ERR(pcie->field)
	MAP_RESOURCE(apb_base, "apb");
	MAP_RESOURCE(phy_base, "phy");
	MAP_RESOURCE(crg_base, "hsdt-crg");
	MAP_RESOURCE(pcie_pll_base, "fnpll");
#undef MAP_RESOURCE
	/* Stock phy_layout_info separates natural KPHY registers from APB control.
	 * TGR-W00 uses natural offset 0 and APB offset 0x2000 within the PHY bank.
	 */
	ret = of_property_read_u32_array(dev->of_node, "phy_layout_info", phy_layout, 3);
	if (ret)
		return ret;
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "phy");
	if (!res || phy_layout[0] || (phy_layout[2] & 3) ||
	    resource_size(res) < 0x1000 || phy_layout[2] > resource_size(res) - 0x1000)
		return -EINVAL;
	pcie->apb_phy_base = pcie->phy_base + phy_layout[2];
	/* These banks are shared with DWC3; do not claim them exclusively. */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "pmctrl");
	if (!res)
		return -EINVAL;
	pcie->pmctrl_base = devm_ioremap(dev, res->start, resource_size(res));
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "aux-crg");
	if (!res)
		return -EINVAL;
	pcie->aux_crg = devm_ioremap(dev, res->start, resource_size(res));
	if (!pcie->pmctrl_base || !pcie->aux_crg)
		return -ENOMEM;
	pcie->phy.phy_base = pcie->phy_base;
	if (!pcie->transport) {
		pcie->chip_power = devm_gpiod_get(dev, "chip-power", GPIOD_OUT_LOW);
		if (IS_ERR(pcie->chip_power))
			return dev_err_probe(dev, PTR_ERR(pcie->chip_power), "chip power GPIO\n");
		pcie->wlan_power = devm_gpiod_get(dev, "wlan-power", GPIOD_OUT_LOW);
		if (IS_ERR(pcie->wlan_power))
			return dev_err_probe(dev, PTR_ERR(pcie->wlan_power), "WLAN power GPIO\n");
		pcie->tcxo = devm_gpiod_get(dev, "tcxo", GPIOD_OUT_HIGH);
		if (IS_ERR(pcie->tcxo))
			return dev_err_probe(dev, PTR_ERR(pcie->tcxo), "TCXO strap GPIO\n");
	}
	pcie->linkdown_irq = platform_get_irq_byname_optional(pdev, "linkdown");
	if (pcie->linkdown_irq > 0) {
		ret = devm_request_irq(dev, pcie->linkdown_irq, charlotte_pcie_linkdown_irq,
				      IRQF_TRIGGER_RISING, "charlotte-pcie-linkdown", pcie);
		if (ret)
			return ret;
	}
	ret = dw_pcie_get_resources(&pcie->pci);
	if (ret)
		return ret;
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dbi");
	pcie->pci.atu_base = pcie->pci.dbi_base + 0x300000;
	pcie->pci.atu_size = 0x2000;
	pcie->pci.atu_phys_addr = res->start + 0x300000;
	/* Diagnose fabric access only after USB/dmesg are available. */
	if (!of_property_read_bool(dev->of_node, "hisilicon,probe-on-boot")) {
		dev_info(dev, "PCIe0 ready for manual initialization\n");
		ret = device_create_file(dev, &dev_attr_enable);
		if (!ret)
			WRITE_ONCE(charlotte_pcie0, pcie);
		return ret;
	}
	ret = dw_pcie_host_init(&pcie->pci.pp);
	if (ret) {
		charlotte_pcie_power_off(pcie);
		return dev_err_probe(dev, ret, "PCIe0 initialization\n");
	}
	pcie->initialized = true;
	WRITE_ONCE(charlotte_pcie0, pcie);
	return 0;
}

static void charlotte_pcie_remove(struct platform_device *pdev)
{
	struct charlotte_pcie *pcie = platform_get_drvdata(pdev);

	device_remove_file(&pdev->dev, &dev_attr_enable);
	WRITE_ONCE(charlotte_pcie0, NULL);
	cancel_work_sync(&pcie->linkdown_work);
	if (pcie->initialized)
		dw_pcie_host_deinit(&pcie->pci.pp);
	pci_dev_put(pcie->root_port);
}

static void charlotte_pcie_shutdown(struct platform_device *pdev)
{
	struct charlotte_pcie *pcie = platform_get_drvdata(pdev);

	charlotte_pcie_power_off(pcie);
}

static const struct of_device_id charlotte_pcie_match[] = {
	{ .compatible = "hisilicon,charlotte-pcie" },
	{ }
};
MODULE_DEVICE_TABLE(of, charlotte_pcie_match);

static struct platform_driver charlotte_pcie_driver = {
	.probe = charlotte_pcie_probe,
	.remove = charlotte_pcie_remove,
	.shutdown = charlotte_pcie_shutdown,
	.driver = { .name = "charlotte-pcie", .of_match_table = charlotte_pcie_match,
		    .suppress_bind_attrs = true },
};
builtin_platform_driver(charlotte_pcie_driver);
