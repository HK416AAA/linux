/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef HI11XX_OF_GPIO_H
#define HI11XX_OF_GPIO_H

#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/of.h>

/* Translate the vendor's exact property names, without claiming the line.
 * The board code subsequently owns it through gpio_request_one().
 */
static inline int of_get_named_gpio_flags(const struct device_node *np,
					 const char *name, int index, u32 *flags)
{
	struct of_phandle_args args;
	struct gpio_device *gdev;
	struct gpio_desc *desc;
	int ret;

	ret = of_parse_phandle_with_args(np, name, "#gpio-cells", index, &args);
	if (ret)
		return ret;
	if (args.args_count != 2) {
		of_node_put(args.np);
		return -EINVAL;
	}
	gdev = gpio_device_find_by_fwnode(of_fwnode_handle(args.np));
	of_node_put(args.np);
	if (!gdev)
		return -EPROBE_DEFER;
	if (IS_ERR(gdev))
		return PTR_ERR(gdev);
	desc = gpio_device_get_desc(gdev, args.args[0]);
	ret = IS_ERR(desc) ? PTR_ERR(desc) : desc_to_gpio(desc);
	if (ret >= 0 && flags)
		*flags = args.args[1];
	gpio_device_put(gdev);
	return ret;
}

static inline int of_get_named_gpio(const struct device_node *np,
				    const char *name, int index)
{
	return of_get_named_gpio_flags(np, name, index, NULL);
}

static inline int of_gpio_named_count(const struct device_node *np, const char *name)
{
    return of_count_phandle_with_args(np, name, "#gpio-cells");
}

#endif
