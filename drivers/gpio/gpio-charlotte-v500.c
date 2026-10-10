// SPDX-License-Identifier: GPL-2.0-only
/* Charlotte GPIO v500. Register protocol: vendor gpio_hiv500.c/hiv500_gpio.h.
 * Unlike gpio-hisi.c (Ascend), each pin has a masked control register and
 * two masked data registers cover the 32 lines. Firmware-owned lines are
 * excluded by gpio-reserved-ranges; never reset an entire GPIO bank.
 */
#include <linux/gpio/driver.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#define V500_CTRL(n) (0x008 + 4 * (n))
#define V500_MASK 0x088
#define V500_CLEAR 0x0a8
#define V500_PENDING 0x0b0
#define V500_INPUT 0x0dc

struct v500_gpio {
	struct gpio_chip gc;
	void __iomem *base;
	raw_spinlock_t lock;
	u32 owned;
	unsigned int parent_irq;
};

static int v500_get(struct gpio_chip *gc, unsigned int pin)
{
	struct v500_gpio *vg = gpiochip_get_data(gc);

	return !!(readl(vg->base + V500_INPUT) & BIT(pin));
}

static int v500_set(struct gpio_chip *gc, unsigned int pin, int value)
{
	struct v500_gpio *vg = gpiochip_get_data(gc);
	unsigned int bit = pin % 16;

	writel(BIT(bit + 16) | (value ? BIT(bit) : 0),
	       vg->base + (pin < 16 ? 4 : 0));
	return 0;
}

static int v500_direction_input(struct gpio_chip *gc, unsigned int pin)
{
	struct v500_gpio *vg = gpiochip_get_data(gc);

	writel(BIT(20), vg->base + V500_CTRL(pin));
	return 0;
}

static int v500_direction_output(struct gpio_chip *gc, unsigned int pin,
				 int value)
{
	struct v500_gpio *vg = gpiochip_get_data(gc);

	v500_set(gc, pin, value);
	writel(BIT(20) | BIT(4), vg->base + V500_CTRL(pin));
	return 0;
}

static int v500_get_direction(struct gpio_chip *gc, unsigned int pin)
{
	struct v500_gpio *vg = gpiochip_get_data(gc);

	return readl(vg->base + V500_CTRL(pin)) & BIT(4) ?
		GPIO_LINE_DIRECTION_OUT : GPIO_LINE_DIRECTION_IN;
}

static void v500_irq_mask(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct v500_gpio *vg = gpiochip_get_data(gc);
	unsigned long flags;

	raw_spin_lock_irqsave(&vg->lock, flags);
	writel(readl(vg->base + V500_MASK) | BIT(irqd_to_hwirq(d)),
	       vg->base + V500_MASK);
	raw_spin_unlock_irqrestore(&vg->lock, flags);
	gpiochip_disable_irq(gc, irqd_to_hwirq(d));
}

static void v500_irq_unmask(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct v500_gpio *vg = gpiochip_get_data(gc);
	unsigned long flags;

	gpiochip_enable_irq(gc, irqd_to_hwirq(d));
	raw_spin_lock_irqsave(&vg->lock, flags);
	writel(readl(vg->base + V500_MASK) & ~BIT(irqd_to_hwirq(d)),
	       vg->base + V500_MASK);
	raw_spin_unlock_irqrestore(&vg->lock, flags);
}

static void v500_irq_ack(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct v500_gpio *vg = gpiochip_get_data(gc);

	writel(BIT(irqd_to_hwirq(d)), vg->base + V500_CLEAR);
}

static int v500_irq_type(struct irq_data *d, unsigned int type)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct v500_gpio *vg = gpiochip_get_data(gc);
	void __iomem *ctrl = vg->base + V500_CTRL(irqd_to_hwirq(d));

	switch (type & IRQ_TYPE_SENSE_MASK) {
	case IRQ_TYPE_EDGE_RISING:
		writel(0x00010000, ctrl);
		writel(0x00060002, ctrl);
		break;
	case IRQ_TYPE_EDGE_FALLING:
		writel(0x00010000, ctrl);
		writel(0x00060000, ctrl);
		break;
	case IRQ_TYPE_EDGE_BOTH:
		writel(0x00010000, ctrl);
		writel(0x00060006, ctrl);
		break;
	case IRQ_TYPE_LEVEL_HIGH:
		writel(0x00010001, ctrl);
		writel(0x00080008, ctrl);
		break;
	case IRQ_TYPE_LEVEL_LOW:
		writel(0x00010001, ctrl);
		writel(0x00080000, ctrl);
		break;
	default:
		return -EINVAL;
	}
	irq_set_handler_locked(d, type & IRQ_TYPE_LEVEL_MASK ?
			       handle_level_irq : handle_edge_irq);
	return 0;
}

static const struct irq_chip v500_irqchip = {
	.name = "charlotte-gpio-v500",
	.irq_ack = v500_irq_ack,
	.irq_mask = v500_irq_mask,
	.irq_unmask = v500_irq_unmask,
	.irq_set_type = v500_irq_type,
	.flags = IRQCHIP_IMMUTABLE,
	GPIOCHIP_IRQ_RESOURCE_HELPERS,
};

static void v500_irq_handler(struct irq_desc *desc)
{
	struct v500_gpio *vg = irq_desc_get_handler_data(desc);
	struct irq_chip *chip = irq_desc_get_chip(desc);
	unsigned long pending;
	unsigned int pin;

	chained_irq_enter(chip, desc);
	pending = readl(vg->base + V500_PENDING) & vg->owned;
	for_each_set_bit(pin, &pending, 32)
		generic_handle_domain_irq(vg->gc.irq.domain, pin);
	chained_irq_exit(chip, desc);
}

static int v500_init_mask(struct gpio_chip *gc, unsigned long *mask,
			  unsigned int ngpios)
{
	struct v500_gpio *vg = gpiochip_get_data(gc);

	/* Gpiolib applies gpio-reserved-ranges before this callback. */
	vg->owned = (u32)mask[0];
	writel(readl(vg->base + V500_MASK) | vg->owned,
	       vg->base + V500_MASK);
	writel(vg->owned, vg->base + V500_CLEAR);
	return 0;
}

static int v500_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct v500_gpio *vg;
	struct resource *res;
	int irq;

	vg = devm_kzalloc(dev, sizeof(*vg), GFP_KERNEL);
	if (!vg)
		return -ENOMEM;
	/* Bank 5 is shared with the existing powerhold driver. Ownership is
	 * per line, enforced by gpio-reserved-ranges and masked data writes.
	 */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;
	vg->base = devm_ioremap(dev, res->start, resource_size(res));
	if (!vg->base)
		return -ENOMEM;
	raw_spin_lock_init(&vg->lock);
	vg->gc = (struct gpio_chip) {
		.label = dev_name(dev), .parent = dev, .owner = THIS_MODULE,
		.base = -1, .ngpio = 32,
		.get = v500_get, .set = v500_set,
		.get_direction = v500_get_direction,
		.direction_input = v500_direction_input,
		.direction_output = v500_direction_output,
		.init_valid_mask = v500_init_mask,
	};
	irq = platform_get_irq_optional(pdev, 0);
	if (irq == -EPROBE_DEFER)
		return irq;
	if (irq > 0) {
		vg->parent_irq = irq;
		gpio_irq_chip_set_chip(&vg->gc.irq, &v500_irqchip);
		vg->gc.irq.parent_handler = v500_irq_handler;
		vg->gc.irq.parent_handler_data = vg;
		vg->gc.irq.num_parents = 1;
		vg->gc.irq.parents = &vg->parent_irq;
		vg->gc.irq.default_type = IRQ_TYPE_NONE;
		vg->gc.irq.handler = handle_bad_irq;
	}
	return devm_gpiochip_add_data(dev, &vg->gc, vg);
}

static const struct of_device_id v500_match[] = {
	{ .compatible = "hisilicon,charlotte-gpio-v500" }, { }
};

static struct platform_driver v500_driver = {
	.probe = v500_probe,
	.driver = { .name = "charlotte-gpio-v500", .of_match_table = v500_match,
		    .suppress_bind_attrs = true },
};
builtin_platform_driver(v500_driver);
