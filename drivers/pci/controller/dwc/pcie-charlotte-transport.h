/* SPDX-License-Identifier: GPL-2.0-only */
/* Native implementations of the RC services consumed by Hi1105. */
u32 show_link_state(u32 rc_id)
{
	struct charlotte_pcie *pcie = READ_ONCE(charlotte_pcie0);
	u32 state;

	if (rc_id || !pcie)
		return 0xffff;
	mutex_lock(&pcie->init_lock);
	if (!pcie->powered) {
		state = 0;
	} else {
		state = pcie_apb_ctrl_readl(pcie, 0x410) & 0x3f;
		if (state == 0x14) {
			u32 substate = pcie_apb_ctrl_readl(pcie, 0x414) & 0xc000;

			if (substate == 0x4000 || substate == 0xc000)
				state = substate;
		} else if (state != 0x3 && state != 0x11 &&
			   state != 0x12 && state != 0x1b) {
			state = 0xffff;
		}
	}
	mutex_unlock(&pcie->init_lock);
	return state;
}
EXPORT_SYMBOL_GPL(show_link_state);

static void charlotte_pcie_linkdown_work(struct work_struct *work)
{
	struct charlotte_pcie *pcie = container_of(work, struct charlotte_pcie, linkdown_work);
	struct pcie_kport_register_event *event;

	mutex_lock(&pcie->event_lock);
	event = pcie->event;
	if (event && (event->events & PCIE_KPORT_EVENT_LINKDOWN) && event->callback) {
		event->notify.event = PCIE_KPORT_EVENT_LINKDOWN;
		event->notify.user = event->user;
		event->callback(&event->notify);
	}
	mutex_unlock(&pcie->event_lock);
}

static irqreturn_t charlotte_pcie_linkdown_irq(int irq, void *data)
{
	struct charlotte_pcie *pcie = data;

	dev_warn(pcie->pci.dev, "PCIe0 link down\n");
	schedule_work(&pcie->linkdown_work);
	return IRQ_HANDLED;
}

int pcie_kport_register_event(struct pcie_kport_register_event *event)
{
	struct charlotte_pcie *pcie = READ_ONCE(charlotte_pcie0);

	if (!pcie)
		return -EPROBE_DEFER;
	if (!event || !event->user || !event->callback ||
	    event->mode != PCIE_KPORT_TRIGGER_CALLBACK)
		return -EINVAL;
	if (event->events != PCIE_KPORT_EVENT_LINKDOWN)
		return -EOPNOTSUPP;
	if (pcie->linkdown_irq <= 0)
		return -EOPNOTSUPP;
	mutex_lock(&pcie->event_lock);
	if (pcie->event && pcie->event != event) {
		mutex_unlock(&pcie->event_lock);
		return -EBUSY;
	}
	pcie->event = event;
	mutex_unlock(&pcie->event_lock);
	return 0;
}
EXPORT_SYMBOL_GPL(pcie_kport_register_event);

int pcie_kport_deregister_event(struct pcie_kport_register_event *event)
{
	struct charlotte_pcie *pcie = READ_ONCE(charlotte_pcie0);

	if (!pcie)
		return -ENODEV;
	mutex_lock(&pcie->event_lock);
	if (pcie->event == event)
		pcie->event = NULL;
	mutex_unlock(&pcie->event_lock);
	cancel_work_sync(&pcie->linkdown_work);
	return 0;
}
EXPORT_SYMBOL_GPL(pcie_kport_deregister_event);

int pcie_kport_power_notifiy_register(u32 rc_id, int (*on)(void *),
				    int (*off)(void *), void *data)
{
	struct charlotte_pcie *pcie = READ_ONCE(charlotte_pcie0);

	if (rc_id)
		return -EINVAL;
	if (!pcie)
		return -EPROBE_DEFER;
	mutex_lock(&pcie->callback_lock);
	pcie->endpoint_on = on;
	pcie->endpoint_off = off;
	pcie->endpoint_data = data;
	mutex_unlock(&pcie->callback_lock);
	return 0;
}
EXPORT_SYMBOL_GPL(pcie_kport_power_notifiy_register);

int pcie_kport_enumerate(u32 rc_id)
{
	struct charlotte_pcie *pcie = READ_ONCE(charlotte_pcie0);
	int ret = 0;

	if (rc_id)
		return -EINVAL;
	if (!pcie)
		return -EPROBE_DEFER;
	mutex_lock(&pcie->init_lock);
	if (!pcie->initialized) {
		ret = dw_pcie_host_init(&pcie->pci.pp);
		if (!ret)
			pcie->initialized = true;
	} else if (!pcie->powered) {
		ret = -EHOSTDOWN;
	}
	mutex_unlock(&pcie->init_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(pcie_kport_enumerate);

int pcie_kport_pm_control(int operation, u32 rc_id)
{
	struct charlotte_pcie *pcie = READ_ONCE(charlotte_pcie0);
	int ret = 0;

	if (rc_id || operation < 0 || operation > 3)
		return -EINVAL;
	if (!pcie || !pcie->initialized)
		return -ENODEV;
	/* PME/L2 handshake is not implemented yet; the transport can request 2. */
	if (!operation)
		return -EOPNOTSUPP;
	mutex_lock(&pcie->init_lock);
	if (operation == 2) {
		if (pcie->powered && pcie->root_port) {
			ret = pci_save_state(pcie->root_port);
			if (ret)
				goto out;
		}
		ret = charlotte_pcie_power_off(pcie);
	} else {
		if (!pcie->powered) {
			ret = charlotte_pcie_power_on(pcie);
			if (ret)
				goto out;
		}
		if (operation == 3 || dw_pcie_link_up(&pcie->pci))
			goto out;
		ret = dw_pcie_setup_rc(&pcie->pci.pp);
		if (ret)
			goto failed;
		if (pcie->root_port)
			pci_restore_state(pcie->root_port);
		ret = charlotte_pcie_start_link(&pcie->pci);
		if (!ret)
			ret = dw_pcie_wait_for_link(&pcie->pci);
		if (ret)
			goto failed;
	}
	goto out;
failed:
	charlotte_pcie_power_off(pcie);
out:
	mutex_unlock(&pcie->init_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(pcie_kport_pm_control);

int pcie_kport_lp_ctrl(u32 rc_id, u32 enable)
{
	struct charlotte_pcie *pcie = READ_ONCE(charlotte_pcie0);
	struct pci_dev *ep;
	int ret;

	if (rc_id || enable > 1)
		return -EINVAL;
	if (!pcie || !pcie->powered || !pcie->root_port ||
	    !pcie->root_port->subordinate)
		return -ENODEV;
	ep = pci_get_slot(pcie->root_port->subordinate, PCI_DEVFN(0, 0));
	if (!ep)
		return -ENODEV;
	/* PCI core owns capability checks and ordering; no private L1SS writes. */
	if (enable)
		ret = pci_enable_link_state(ep, PCIE_LINK_STATE_L1);
	else
		ret = pci_disable_link_state(ep, PCIE_LINK_STATE_L0S | PCIE_LINK_STATE_L1 |
					     PCIE_LINK_STATE_L1_1 | PCIE_LINK_STATE_L1_2);
	pci_dev_put(ep);
	return ret;
}
EXPORT_SYMBOL_GPL(pcie_kport_lp_ctrl);
