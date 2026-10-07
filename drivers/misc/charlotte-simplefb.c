// SPDX-License-Identifier: GPL-2.0
/*
 * charlotte-simplefb -- fixed-address framebuffer handoff (v53).
 *
 * The vendor UEFI leaves the DPU scanning the logo buffer it programmed
 * last: /reserved-memory/logo-buffer @ 0x34380000 in the bootloader FDT.
 * The bootloader FDT carries no simple-framebuffer node, so this registers
 * one from code with the geometry measured on the vendor kernel's own fb0
 * (read-only FBIOGET ioctls via Magisk):
 *
 *   xres 1840, yres 2800, 32 bpp, stride 7360, memory order B,G,R,X
 *   (red 16:8, green 8:8, blue 0:8, transp 24:8  ->  "x8b8g8r8").
 *
 * The logo-buffer is inside the FDT's reserved-memory, so the page
 * allocator will not hand the scanout out from under fbcon.
 */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/platform_data/simplefb.h>

#define CHARLOTTE_FB_PA		0x34380000UL
#define CHARLOTTE_FB_WIDTH	1840U
#define CHARLOTTE_FB_HEIGHT	2800U
#define CHARLOTTE_FB_STRIDE	7360U

static int __init charlotte_simplefb_init(void)
{
	struct platform_device *pdev;
	struct resource res = {
		.start	= CHARLOTTE_FB_PA,
		.end	= CHARLOTTE_FB_PA + CHARLOTTE_FB_STRIDE * CHARLOTTE_FB_HEIGHT - 1,
		.flags	= IORESOURCE_MEM,
	};
	static struct simplefb_platform_data pd = {
		.width	= CHARLOTTE_FB_WIDTH,
		.height	= CHARLOTTE_FB_HEIGHT,
		.stride	= CHARLOTTE_FB_STRIDE,
		.format	= "x8b8g8r8",
	};

	pdev = platform_device_register_resndata(NULL, "simple-framebuffer", 0,
						 &res, 1, &pd, sizeof(pd));
	return PTR_ERR_OR_ZERO(pdev);
}
device_initcall(charlotte_simplefb_init);

MODULE_DESCRIPTION("charlotte fixed-address simple framebuffer handoff");
MODULE_LICENSE("GPL");
