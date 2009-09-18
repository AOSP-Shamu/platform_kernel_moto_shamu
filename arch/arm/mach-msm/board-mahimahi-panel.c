/* linux/arch/arm/mach-msm/board-mahimahi-panel.c
 *
 * Copyright (c) 2009 Google Inc.
 * Author: Dima Zavin <dima@android.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/leds.h>
#include <linux/platform_device.h>

#include <asm/io.h>
#include <asm/mach-types.h>

#include <mach/msm_fb.h>
#include <mach/msm_iomap.h>

#include "board-mahimahi.h"
#include "devices.h"


#define SPI_CONFIG              (0x00000000)
#define SPI_IO_CONTROL          (0x00000004)
#define SPI_OPERATIONAL         (0x00000030)
#define SPI_ERROR_FLAGS_EN      (0x00000038)
#define SPI_ERROR_FLAGS         (0x00000038)
#define SPI_OUTPUT_FIFO         (0x00000100)

static void __iomem *spi_base;
static struct clk *spi_clk ;

#define CLK_NS_TO_RATE(ns)			(1000000000UL / (ns))

static int qspi_send(uint32_t id, uint8_t data)
{
	uint32_t err;

	/* bit-5: OUTPUT_FIFO_NOT_EMPTY */
	while (readl(spi_base + SPI_OPERATIONAL) & (1<<5)) {
		if ((err = readl(spi_base + SPI_ERROR_FLAGS))) {
			pr_err("%s: ERROR: SPI_ERROR_FLAGS=0x%08x\n", __func__,
			       err);
			return -EIO;
		}
	}
	writel((0x7000 | (id << 9) | data) << 16, spi_base + SPI_OUTPUT_FIFO);
	udelay(100);

	return 0;
}

static int lcm_writeb(uint8_t reg, uint8_t val)
{
	qspi_send(0x0, reg);
	qspi_send(0x1, val);
	return 0;
}

static DEFINE_MUTEX(panel_lock);

static int samsung_oled_panel_blank(struct msm_lcdc_panel_ops *ops)
{
	pr_info("%s: +()\n", __func__);
	mutex_lock(&panel_lock);

	clk_enable(spi_clk);
	lcm_writeb(0x14, 0x1);
	lcm_writeb(0x1d, 0xa1);
	msleep(200);
	clk_disable(spi_clk);

	mutex_unlock(&panel_lock);
	pr_info("%s: -()\n", __func__);
	return 0;
}

static int samsung_oled_panel_unblank(struct msm_lcdc_panel_ops *ops)
{
	pr_info("%s: +()\n", __func__);
	mutex_lock(&panel_lock);

	clk_enable(spi_clk);
	lcm_writeb(0x1d, 0xa0);
	lcm_writeb(0x14, 0x03);
	msleep(200);
	clk_disable(spi_clk);

	mutex_unlock(&panel_lock);
	pr_info("%s: -()\n", __func__);
	return 0;
}

static struct resource resources_msm_fb[] = {
	{
		.start = MSM_FB_BASE,
		.end = MSM_FB_BASE + MSM_FB_SIZE - 1,
		.flags = IORESOURCE_MEM,
	},
};

struct lcm_tbl {
	uint8_t		reg;
	uint8_t		val;
};

static struct lcm_tbl samsung_oled_init_table[] = {
	{ 0x31, 0x08 },
	{ 0x32, 0x14 },
	{ 0x30, 0x2 },
	{ 0x27, 0x1 },
	{ 0x12, 0x8 },
	{ 0x13, 0x8 },
	{ 0x15, 0x0 },
	{ 0x16, 0x02 },
	{ 0x39, 0x44 },
	{ 0x17, 0x22 },
	{ 0x18, 0x33 },
	{ 0x19, 0x3 },
	{ 0x1A, 0x1 },
	{ 0x22, 0xA4 },
	{ 0x23, 0x0 },
	{ 0x26, 0xA0 },
};

#define OLED_GAMMA_TABLE_SIZE		(7 * 3)
static struct lcm_tbl samsung_oled_gamma_table[][OLED_GAMMA_TABLE_SIZE] = {
	/* level 10 */
	{
		/* Gamma-R */
		{ 0x40, 0x0 },
		{ 0x41, 0x3f },
		{ 0x42, 0x3f },
		{ 0x43, 0x35 },
		{ 0x44, 0x30 },
		{ 0x45, 0x2c },
		{ 0x46, 0x13 },
		/* Gamma -G */
		{ 0x50, 0x0 },
		{ 0x51, 0x0 },
		{ 0x52, 0x0 },
		{ 0x53, 0x0 },
		{ 0x54, 0x27 },
		{ 0x55, 0x2b },
		{ 0x56, 0x12 },
		/* Gamma -B */
		{ 0x60, 0x0 },
		{ 0x61, 0x3f },
		{ 0x62, 0x3f },
		{ 0x63, 0x34 },
		{ 0x64, 0x2f },
		{ 0x65, 0x2b },
		{ 0x66, 0x1b },
	},

	/* level 40 */
	{
		/* Gamma -R */
		{ 0x40, 0x0 },
		{ 0x41, 0x3f },
		{ 0x42, 0x3e },
		{ 0x43, 0x2e },
		{ 0x44, 0x2d },
		{ 0x45, 0x28 },
		{ 0x46, 0x21 },
		/* Gamma -G */
		{ 0x50, 0x0 },
		{ 0x51, 0x0 },
		{ 0x52, 0x0 },
		{ 0x53, 0x21 },
		{ 0x54, 0x2a },
		{ 0x55, 0x28 },
		{ 0x56, 0x20 },
		/* Gamma -B */
		{ 0x60, 0x0 },
		{ 0x61, 0x3f },
		{ 0x62, 0x3e },
		{ 0x63, 0x2d },
		{ 0x64, 0x2b },
		{ 0x65, 0x26 },
		{ 0x66, 0x2d },
	},

	/* level 70 */
	{
		/* Gamma -R */
		{ 0x40, 0x0 },
		{ 0x41, 0x3f },
		{ 0x42, 0x35 },
		{ 0x43, 0x2c },
		{ 0x44, 0x2b },
		{ 0x45, 0x26 },
		{ 0x46, 0x29 },
		/* Gamma -G */
		{ 0x50, 0x0 },
		{ 0x51, 0x0 },
		{ 0x52, 0x0 },
		{ 0x53, 0x25 },
		{ 0x54, 0x29 },
		{ 0x55, 0x26 },
		{ 0x56, 0x28 },
		/* Gamma -B */
		{ 0x60, 0x0 },
		{ 0x61, 0x3f },
		{ 0x62, 0x34 },
		{ 0x63, 0x2b },
		{ 0x64, 0x2a },
		{ 0x65, 0x23 },
		{ 0x66, 0x37 },
	},

	/* level 100 */
	{
		/* Gamma -R */
		{ 0x40, 0x0 },
		{ 0x41, 0x3f },
		{ 0x42, 0x30 },
		{ 0x43, 0x2a },
		{ 0x44, 0x2b },
		{ 0x45, 0x24 },
		{ 0x46, 0x2f },
		/* Gamma -G */
		{ 0x50, 0x0 },
		{ 0x51, 0x0 },
		{ 0x52, 0x0 },
		{ 0x53, 0x25 },
		{ 0x54, 0x29 },
		{ 0x55, 0x24 },
		{ 0x56, 0x2e },
		/* Gamma -B */
		{ 0x60, 0x0 },
		{ 0x61, 0x3f },
		{ 0x62, 0x2f },
		{ 0x63, 0x29 },
		{ 0x64, 0x29 },
		{ 0x65, 0x21 },
		{ 0x66, 0x3f },
	},

	/* level 130 */
	{
		/* Gamma -R */
		{ 0x40, 0x0 },
		{ 0x41, 0x3f },
		{ 0x42, 0x2e },
		{ 0x43, 0x29 },
		{ 0x44, 0x2a },
		{ 0x45, 0x23 },
		{ 0x46, 0x34 },
		/* Gamma -G */
		{ 0x50, 0x0 },
		{ 0x51, 0x0 },
		{ 0x52, 0xa },
		{ 0x53, 0x25 },
		{ 0x54, 0x28 },
		{ 0x55, 0x23 },
		{ 0x56, 0x33 },
		/* Gamma -B */
		{ 0x60, 0x0 },
		{ 0x61, 0x3f },
		{ 0x62, 0x2d },
		{ 0x63, 0x28 },
		{ 0x64, 0x27 },
		{ 0x65, 0x20 },
		{ 0x66, 0x46 },
	},

	/* level 160 */
	{
		/* Gamma -R */
		{ 0x40, 0x0 },
		{ 0x41, 0x3f },
		{ 0x42, 0x2b },
		{ 0x43, 0x29 },
		{ 0x44, 0x28 },
		{ 0x45, 0x23 },
		{ 0x46, 0x38 },
		/* Gamma -G */
		{ 0x50, 0x0 },
		{ 0x51, 0x0 },
		{ 0x52, 0xb },
		{ 0x53, 0x25 },
		{ 0x54, 0x27 },
		{ 0x55, 0x23 },
		{ 0x56, 0x37 },
		/* Gamma -B */
		{ 0x60, 0x0 },
		{ 0x61, 0x3f },
		{ 0x62, 0x29 },
		{ 0x63, 0x28 },
		{ 0x64, 0x25 },
		{ 0x65, 0x20 },
		{ 0x66, 0x4b },
	},

	/* level 190 */
	{
		/* Gamma -R */
		{ 0x40, 0x0 },
		{ 0x41, 0x3f },
		{ 0x42, 0x29 },
		{ 0x43, 0x29 },
		{ 0x44, 0x27 },
		{ 0x45, 0x22 },
		{ 0x46, 0x3c },
		/* Gamma -G */
		{ 0x50, 0x0 },
		{ 0x51, 0x0 },
		{ 0x52, 0x10 },
		{ 0x53, 0x26 },
		{ 0x54, 0x26 },
		{ 0x55, 0x22 },
		{ 0x56, 0x3b },
		/* Gamma -B */
		{ 0x60, 0x0 },
		{ 0x61, 0x3f },
		{ 0x62, 0x28 },
		{ 0x63, 0x28 },
		{ 0x64, 0x24 },
		{ 0x65, 0x1f },
		{ 0x66, 0x50 },
	},

	/* level 220 */
	{
		/* Gamma -R */
		{ 0x40, 0x0 },
		{ 0x41, 0x3f },
		{ 0x42, 0x28 },
		{ 0x43, 0x28 },
		{ 0x44, 0x28 },
		{ 0x45, 0x20 },
		{ 0x46, 0x40 },
		/* Gamma -G */
		{ 0x50, 0x0 },
		{ 0x51, 0x0 },
		{ 0x52, 0x11 },
		{ 0x53, 0x25 },
		{ 0x54, 0x27 },
		{ 0x55, 0x20 },
		{ 0x56, 0x3f },
		/* Gamma -B */
		{ 0x60, 0x0 },
		{ 0x61, 0x3f },
		{ 0x62, 0x27 },
		{ 0x63, 0x26 },
		{ 0x64, 0x26 },
		{ 0x65, 0x1c },
		{ 0x66, 0x56 },
	},

	/* level 250 */
	{
		/* Gamma -R */
		{ 0x40, 0x0 },
		{ 0x41, 0x3f },
		{ 0x42, 0x2a },
		{ 0x43, 0x27 },
		{ 0x44, 0x27 },
		{ 0x45, 0x1f },
		{ 0x46, 0x44 },
		/* Gamma -G */
		{ 0x50, 0x0 },
		{ 0x51, 0x0 },
		{ 0x52, 0x17 },
		{ 0x53, 0x24 },
		{ 0x54, 0x26 },
		{ 0x55, 0x1f },
		{ 0x56, 0x43 },
		/* Gamma -B */
		{ 0x60, 0x0 },
		{ 0x61, 0x3f },
		{ 0x62, 0x2a },
		{ 0x63, 0x25 },
		{ 0x64, 0x24 },
		{ 0x65, 0x1b },
		{ 0x66, 0x5c },
	},
};
#define SAMSUNG_OLED_NUM_LEVELS		ARRAY_SIZE(samsung_oled_gamma_table)

static uint8_t table_sel_vals[] = { 0x43, 0x34 };
static int table_sel_idx = 0;
static void gamma_table_bank_select(void)
{
	lcm_writeb(0x39, table_sel_vals[table_sel_idx]);
	table_sel_idx ^= 1;
}

static void samsung_oled_set_gamma_level(int level)
{
	int i;

	pr_info("%s: +(): new gamma level = %d\n", __func__, level);
	clk_enable(spi_clk);

	for (i = 0; i < OLED_GAMMA_TABLE_SIZE; ++i)
		lcm_writeb(samsung_oled_gamma_table[level][i].reg,
			   samsung_oled_gamma_table[level][i].val);
	gamma_table_bank_select();
	clk_disable(spi_clk);
	pr_info("%s: -()\n", __func__);
}

static int samsung_oled_panel_init(struct msm_lcdc_panel_ops *ops)
{
	int i;

	pr_info("%s: +()\n", __func__);
	mutex_lock(&panel_lock);

	clk_enable(spi_clk);
	for (i = 0; i < ARRAY_SIZE(samsung_oled_init_table); i++)
		lcm_writeb(samsung_oled_init_table[i].reg,
			   samsung_oled_init_table[i].val);
	gamma_table_bank_select();
	clk_disable(spi_clk);

	mutex_unlock(&panel_lock);
	pr_info("%s: -()\n", __func__);
	return 0;
}


static struct msm_lcdc_panel_ops mahimahi_lcdc_panel_ops = {
	.init		= samsung_oled_panel_init,
	.blank		= samsung_oled_panel_blank,
	.unblank	= samsung_oled_panel_unblank,
};

static struct msm_lcdc_timing mahimahi_lcdc_timing = {
		.clk_rate		= CLK_NS_TO_RATE(26),
		.hsync_pulse_width	= 4,
		.hsync_back_porch	= 8,
		.hsync_front_porch	= 8,
		.hsync_skew		= 0,
		.vsync_pulse_width	= 2,
		.vsync_back_porch	= 8,
		.vsync_front_porch	= 8,
		.vsync_act_low		= 1,
		.hsync_act_low		= 1,
		.den_act_low		= 1,
};

static struct msm_fb_data mahimahi_lcdc_fb_data = {
		.xres		= 480,
		.yres		= 800,
		.width		= 48,
		.height		= 80,
		.output_format	= 0,
};

static struct msm_lcdc_platform_data mahimahi_lcdc_platform_data = {
	.panel_ops	= &mahimahi_lcdc_panel_ops,
	.timing		= &mahimahi_lcdc_timing,
	.fb_id		= 0,
	.fb_data	= &mahimahi_lcdc_fb_data,
	.fb_resource	= &resources_msm_fb[0],
};

static struct platform_device mahimahi_lcdc_device = {
	.name	= "msm_mdp_lcdc",
	.id	= -1,
	.dev	= {
		.platform_data = &mahimahi_lcdc_platform_data,
	},
};

static int mahimahi_init_spi_hack(void)
{
	int ret;

	spi_base = ioremap(MSM_SPI_PHYS, MSM_SPI_SIZE);
	if (!spi_base)
		return -1;

	spi_clk = clk_get(&msm_device_spi.dev, "spi_clk");
	if (IS_ERR(spi_clk)) {
		pr_err("%s: unable to get spi_clk\n", __func__);
		ret = PTR_ERR(spi_clk);
		goto err_clk_get;
	}

	clk_enable(spi_clk);

	printk("spi: SPI_CONFIG=%x\n", readl(spi_base + SPI_CONFIG));
	printk("spi: SPI_IO_CONTROL=%x\n", readl(spi_base + SPI_IO_CONTROL));
	printk("spi: SPI_OPERATIONAL=%x\n", readl(spi_base + SPI_OPERATIONAL));
	printk("spi: SPI_ERROR_FLAGS_EN=%x\n",
	       readl(spi_base + SPI_ERROR_FLAGS_EN));
	printk("spi: SPI_ERROR_FLAGS=%x\n", readl(spi_base + SPI_ERROR_FLAGS));
	printk("-%s()\n", __FUNCTION__);
	clk_disable(spi_clk);

	return 0;

err_clk_get:
	iounmap(spi_base);
	return ret;
}

static void mahimahi_brightness_set(struct led_classdev *led_cdev,
				    enum led_brightness val)
{
	uint8_t new_level = (val * (SAMSUNG_OLED_NUM_LEVELS - 1)) / LED_FULL;

	mutex_lock(&panel_lock);
	samsung_oled_set_gamma_level(new_level);
	mutex_unlock(&panel_lock);
}

static struct led_classdev mahimahi_brightness_led = {
	.name = "lcd-backlight",
	.brightness = LED_FULL,
	.brightness_set = mahimahi_brightness_set,
};

int __init mahimahi_init_panel(void)
{
	int ret;

	if (!machine_is_mahimahi())
		return 0;

	ret = platform_device_register(&msm_device_mdp);
	if (ret != 0)
		return ret;

	ret = mahimahi_init_spi_hack();
	if (ret != 0)
		return ret;

	ret = platform_device_register(&mahimahi_lcdc_device);
	if (ret != 0)
		return ret;

	ret = led_classdev_register(NULL, &mahimahi_brightness_led);
	if (ret != 0) {
		pr_err("%s: Cannot register brightness led\n", __func__);
		return ret;
	}

	return 0;
}

late_initcall(mahimahi_init_panel);
