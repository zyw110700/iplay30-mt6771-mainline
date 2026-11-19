// SPDX-License-Identifier: GPL-2.0
/*
 * mtk-lkfb - take over the framebuffer that the MT6771 LK bootloader left
 * enabled.  LK programs the DSI panel and the DDP overlay, then jumps to the
 * kernel without tearing anything down, so the panel keeps scanning out the
 * buffers below.  Registering them as a legacy fbdev device lets fbcon draw
 * the kernel console on the panel.
 *
 * LK configures two overlay layers, both 1200x1920 eBGRA8888.  We mirror
 * every fbcon update into the second buffer so the log is visible no matter
 * which layer ends up on top.
 */

#include <linux/fb.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>

#define LKFB_WIDTH		1200
#define LKFB_HEIGHT		1920
#define LKFB_BPP		32

/* layer 0: base 0x7dfe0000, pitch 4864 (1216 pixels), from LK DDP log */
#define LKFB_PRIMARY_PA		0x7dfe0000UL
#define LKFB_PRIMARY_STRIDE	4864
#define LKFB_PRIMARY_SIZE	(LKFB_HEIGHT * LKFB_PRIMARY_STRIDE)

/* layer 3: base 0x7e8c8000, pitch 4800 (1200 pixels), from LK DDP log */
#define LKFB_ALT_PA		0x7e8c8000UL
#define LKFB_ALT_STRIDE		4800
#define LKFB_ALT_SIZE		(LKFB_HEIGHT * LKFB_ALT_STRIDE)

static void __iomem *lkfb_alt;
static u32 lkfb_palette[16];

static void lkfb_mirror(struct fb_info *info, u32 dx, u32 dy, u32 width,
			u32 height)
{
	u32 y;

	if (!lkfb_alt)
		return;
	if (dx >= LKFB_WIDTH || dy >= LKFB_HEIGHT)
		return;
	if (dx + width > LKFB_WIDTH)
		width = LKFB_WIDTH - dx;
	if (dy + height > LKFB_HEIGHT)
		height = LKFB_HEIGHT - dy;
	if (!width || !height)
		return;

	for (y = 0; y < height; y++) {
		void __iomem *src = info->screen_base +
				    (dy + y) * LKFB_PRIMARY_STRIDE + dx * 4;
		void __iomem *dst = lkfb_alt +
				    (dy + y) * LKFB_ALT_STRIDE + dx * 4;

		memcpy_toio(dst, (const void *)src, width * 4);
	}
}

static void lkfb_fillrect(struct fb_info *info, const struct fb_fillrect *rect)
{
	cfb_fillrect(info, rect);
	lkfb_mirror(info, rect->dx, rect->dy, rect->width, rect->height);
}

static void lkfb_copyarea(struct fb_info *info, const struct fb_copyarea *area)
{
	cfb_copyarea(info, area);
	lkfb_mirror(info, area->dx, area->dy, area->width, area->height);
}

static void lkfb_imageblit(struct fb_info *info, const struct fb_image *image)
{
	cfb_imageblit(info, image);
	lkfb_mirror(info, image->dx, image->dy, image->width, image->height);
}

static int lkfb_setcolreg(u_int regno, u_int red, u_int green, u_int blue,
			  u_int transp, struct fb_info *info)
{
	u32 value;

	if (regno >= ARRAY_SIZE(lkfb_palette))
		return -EINVAL;

	value = ((red >> 8) << info->var.red.offset) |
		((green >> 8) << info->var.green.offset) |
		((blue >> 8) << info->var.blue.offset) |
		(0xff << info->var.transp.offset);
	lkfb_palette[regno] = value;
	return 0;
}

static int lkfb_check_var(struct fb_var_screeninfo *var, struct fb_info *info)
{
	if (var->xres != LKFB_WIDTH || var->yres != LKFB_HEIGHT)
		return -EINVAL;

	var->xres_virtual = LKFB_WIDTH;
	var->yres_virtual = LKFB_HEIGHT;
	var->xoffset = 0;
	var->yoffset = 0;
	var->bits_per_pixel = LKFB_BPP;
	return 0;
}

static int lkfb_set_par(struct fb_info *info)
{
	info->fix.line_length = LKFB_PRIMARY_STRIDE;
	return 0;
}

static int lkfb_blank(int blank, struct fb_info *info)
{
	return 0;
}

static void lkfb_destroy(struct fb_info *info)
{
	if (info->screen_base)
		iounmap(info->screen_base);
	if (lkfb_alt) {
		iounmap(lkfb_alt);
		lkfb_alt = NULL;
	}
	framebuffer_release(info);
}

static const struct fb_ops lkfb_ops = {
	.owner		= THIS_MODULE,
	.fb_check_var	= lkfb_check_var,
	.fb_set_par	= lkfb_set_par,
	.fb_setcolreg	= lkfb_setcolreg,
	.fb_blank	= lkfb_blank,
	.fb_fillrect	= lkfb_fillrect,
	.fb_copyarea	= lkfb_copyarea,
	.fb_imageblit	= lkfb_imageblit,
	.fb_destroy	= lkfb_destroy,
};

static const struct fb_fix_screeninfo lkfb_fix = {
	.id		= "mtk-lkfb",
	.type		= FB_TYPE_PACKED_PIXELS,
	.visual		= FB_VISUAL_TRUECOLOR,
	.accel		= FB_ACCEL_NONE,
	.line_length	= LKFB_PRIMARY_STRIDE,
};

static const struct fb_var_screeninfo lkfb_var = {
	.xres		= LKFB_WIDTH,
	.yres		= LKFB_HEIGHT,
	.xres_virtual	= LKFB_WIDTH,
	.yres_virtual	= LKFB_HEIGHT,
	.bits_per_pixel	= LKFB_BPP,
	.red		= { .offset = 16, .length = 8 },
	.green		= { .offset = 8,  .length = 8 },
	.blue		= { .offset = 0,  .length = 8 },
	.transp		= { .offset = 24, .length = 8 },
	.vmode		= FB_VMODE_NONINTERLACED,
	.activate	= FB_ACTIVATE_NOW,
};

static int __init lkfb_init(void)
{
	struct fb_info *info;
	int ret;

	info = framebuffer_alloc(0, NULL);
	if (!info)
		return -ENOMEM;

	info->screen_base = ioremap_wc(LKFB_PRIMARY_PA, LKFB_PRIMARY_SIZE);
	if (!info->screen_base) {
		pr_err("mtk-lkfb: cannot map primary framebuffer\n");
		framebuffer_release(info);
		return -ENOMEM;
	}
	info->screen_size = LKFB_PRIMARY_SIZE;

	lkfb_alt = ioremap_wc(LKFB_ALT_PA, LKFB_ALT_SIZE);
	if (!lkfb_alt)
		pr_warn("mtk-lkfb: cannot map alternate framebuffer\n");

	info->fix = lkfb_fix;
	info->fix.smem_start = LKFB_PRIMARY_PA;
	info->fix.smem_len = LKFB_PRIMARY_SIZE;
	info->var = lkfb_var;
	info->fbops = &lkfb_ops;
	info->pseudo_palette = lkfb_palette;

	memset_io(info->screen_base, 0, LKFB_PRIMARY_SIZE);
	if (lkfb_alt)
		memset_io(lkfb_alt, 0, LKFB_ALT_SIZE);

	ret = register_framebuffer(info);
	if (ret) {
		pr_err("mtk-lkfb: register_framebuffer failed: %d\n", ret);
		lkfb_destroy(info);
		return ret;
	}

	pr_info("mtk-lkfb: %ux%u %dbpp at 0x%lx stride %d, alt %s\n",
		LKFB_WIDTH, LKFB_HEIGHT, LKFB_BPP, LKFB_PRIMARY_PA,
		LKFB_PRIMARY_STRIDE, lkfb_alt ? "mapped" : "absent");
	return 0;
}
device_initcall(lkfb_init);

MODULE_DESCRIPTION("MT6771 LK framebuffer takeover");
MODULE_LICENSE("GPL");
