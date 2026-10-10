/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_USB_CHARLOTTE_H
#define _LINUX_USB_CHARLOTTE_H

struct device;

/* Returns a POWER_SUPPLY_USB_TYPE_* value, or a negative errno. */
int charlotte_usb_bc12_detect(struct device *dev);

/* Run a bounded D+/D- charger transaction with the gadget disconnected. */
int charlotte_usb_dpdm_run(struct device *dev, int (*run)(void *data), void *data);

#endif
