/* drivers/video/msm_fb/mdp.c
 *
 * MSM MDP Interface (used by framebuffer core)
 *
 * Copyright (C) 2007 QUALCOMM Incorporated
 * Copyright (C) 2007 Google Incorporated
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/fb.h>
#include <linux/msm_mdp.h>
#include <linux/interrupt.h>
#include <linux/wait.h>
#include <linux/clk.h>

#include <asm/io.h>
#include <asm/arch/msm_iomap.h>
#include <asm/arch/msm_fb.h>

#include "mdp_hw.h"

#define MDP_CMD_DEBUG_ACCESS_BASE (MSM_MDP_BASE + 0x10000)

static uint16_t mdp_default_ccs[] = {
	0x254, 0x000, 0x331, 0x254, 0xF38, 0xE61, 0x254, 0x409, 0x000,
	0x010, 0x080, 0x080
};

/* setup color conversion coefficients */
void mdp_set_ccs(uint16_t *ccs)
{
	int n;
	for (n = 0; n < 9; n++)
		writel(ccs[n], MSM_MDP_BASE + 0x40440 + 4 * n);
	writel(ccs[9], MSM_MDP_BASE + 0x40500 + 4 * 0);
	writel(ccs[10], MSM_MDP_BASE + 0x40500 + 4 * 0);
	writel(ccs[11], MSM_MDP_BASE + 0x40500 + 4 * 0);
}

static DECLARE_WAIT_QUEUE_HEAD(mdp_dma2_waitqueue);
static DECLARE_WAIT_QUEUE_HEAD(mdp_ppp_waitqueue);
static struct msmfb_callback *dma_callback;
static struct clk *clk;
static unsigned int mdp_irq_mask;
static DEFINE_SPINLOCK(mdp_lock);

int enable_mdp_irq(uint32_t mask)
{
	unsigned long irq_flags;
	int ret = 0;

	BUG_ON(!mask);

	spin_lock_irqsave(&mdp_lock, irq_flags);
	/* if the mask bits are already set return an error, this interrupt
	 * is already enabled */
	if (mdp_irq_mask & mask) {
		printk(KERN_ERR "mdp irq already on already on %x %x\n",
		       mdp_irq_mask, mask);
		ret = -1;
	}
	/* if the mdp irq is not already enabled enable it */
	if (!mdp_irq_mask) {
		if (clk)
			clk_enable(clk);
		enable_irq(INT_MDP);
	}

	/* update the irq mask to reflect the fact that the interrupt is
	 * enabled */
	mdp_irq_mask |= mask;
	spin_unlock_irqrestore(&mdp_lock, irq_flags);
	return ret;
}

static int locked_disable_mdp_irq(uint32_t mask)
{
	/* this interrupt is already disabled! */
	if (!(mdp_irq_mask & mask)) {
		printk(KERN_ERR "mdp irq already off %x %x\n",
		       mdp_irq_mask, mask);
		return -1;
	}
	/* update the irq mask to reflect the fact that the interrupt is
	 * disabled */
	mdp_irq_mask &= ~(mask);
	/* if no one is waiting on the interrupt, disable it */
	if (!mdp_irq_mask) {
		disable_irq(INT_MDP);
		if (clk)
			clk_disable(clk);
	}
	return 0;
}

int disable_mdp_irq(uint32_t mask)
{
	unsigned long irq_flags;
	int ret;

	spin_lock_irqsave(&mdp_lock, irq_flags);
	ret = locked_disable_mdp_irq(mask);
	spin_unlock_irqrestore(&mdp_lock, irq_flags);
	return ret;
}


static irqreturn_t mdp_isr(int irq, void *data)
{
	uint32_t status;
	unsigned long irq_flags;

	spin_lock_irqsave(&mdp_lock, irq_flags);

	status = readl(MDP_INTR_STATUS);
	writel(status, MDP_INTR_CLEAR);

	status &= mdp_irq_mask;
	if (status & DL0_DMA2_TERM_DONE) {
		if (dma_callback) {
			dma_callback->func(dma_callback);
			dma_callback = NULL;
		}
		wake_up(&mdp_dma2_waitqueue);
	}

	if (status & DL0_ROI_DONE)
		wake_up(&mdp_ppp_waitqueue);

	if (status)
		locked_disable_mdp_irq(status);

	spin_unlock_irqrestore(&mdp_lock, irq_flags);
	return IRQ_HANDLED;
}

uint32_t mdp_check_mask(uint32_t mask)
{
	uint32_t ret;
	unsigned long irq_flags;

	spin_lock_irqsave(&mdp_lock, irq_flags);
	ret = mdp_irq_mask & mask;
	spin_unlock_irqrestore(&mdp_lock, irq_flags);
	return ret;
}

static int mdp_wait(uint32_t mask, wait_queue_head_t *wq)
{
	int ret = 0;
	unsigned int irq_flags;

	wait_event_timeout(*wq, !mdp_check_mask(mask), HZ);

	spin_lock_irqsave(&mdp_lock, irq_flags);
	if (mdp_irq_mask & mask) {
		locked_disable_mdp_irq(mask);
		printk(KERN_WARNING "timeout waiting for mdp to complete %x\n",
		       mask);
		ret = -ETIMEDOUT;
	}
	spin_unlock_irqrestore(&mdp_lock, irq_flags);

	return ret;
}

void mdp_dma_wait(void)
{
	mdp_wait(DL0_DMA2_TERM_DONE, &mdp_dma2_waitqueue);
}

int mdp_ppp_wait(void)
{
	return mdp_wait(DL0_ROI_DONE, &mdp_ppp_waitqueue);
}


void mdp_dma_to_mddi(uint32_t addr, uint32_t stride, uint32_t width,
		     uint32_t height, uint32_t x, uint32_t y,
		     struct msmfb_callback *callback)

{

	uint32_t dma2_cfg;
	uint16_t ld_param = 0; /* 0=PRIM, 1=SECD, 2=EXT */

	if (enable_mdp_irq(DL0_DMA2_TERM_DONE)) {
		printk(KERN_ERR "mdp_dma_to_mddi: busy\n");
		return;
	}

	dma_callback = callback;

	dma2_cfg = DMA_PACK_TIGHT |
		DMA_PACK_ALIGN_LSB |
		DMA_PACK_PATTERN_RGB |
		DMA_OUT_SEL_AHB |
		DMA_IBUF_NONCONTIGUOUS;

	dma2_cfg |= DMA_IBUF_FORMAT_RGB565;

	dma2_cfg |= DMA_OUT_SEL_MDDI;

	dma2_cfg |= DMA_MDDI_DMAOUT_LCD_SEL_PRIMARY;

	dma2_cfg |= DMA_DITHER_EN;

	/* setup size, address, and stride */
	writel((height << 16) | (width), MDP_CMD_DEBUG_ACCESS_BASE + 0x0184);
	writel(addr, MDP_CMD_DEBUG_ACCESS_BASE + 0x0188);
	writel(stride, MDP_CMD_DEBUG_ACCESS_BASE + 0x018C);

	/* 666 18BPP */
	dma2_cfg |= DMA_DSTC0G_6BITS | DMA_DSTC1B_6BITS | DMA_DSTC2R_6BITS;

	/* set y & x offset and MDDI transaction parameters */
	writel((y << 16) | (x), MDP_CMD_DEBUG_ACCESS_BASE + 0x0194);
	writel(ld_param, MDP_CMD_DEBUG_ACCESS_BASE + 0x01a0);
	writel((MDDI_VDO_PACKET_DESC << 16) | MDDI_VDO_PACKET_PRIM,
		   MDP_CMD_DEBUG_ACCESS_BASE + 0x01a4);

	writel(dma2_cfg, MDP_CMD_DEBUG_ACCESS_BASE + 0x0180);

	/* start DMA2 */
	writel(0, MDP_CMD_DEBUG_ACCESS_BASE + 0x0044);
}

void mdp_set_grp_disp(unsigned disp_id)
{
	disp_id &= 0xf;
	writel(disp_id, MDP_FULL_BYPASS_WORD43);
}

#include "mdp_csc_table.h"
#include "mdp_scale_tables.h"

int mdp_init(struct fb_info *info)
{
	int ret;
	int n;
#if !defined(CONFIG_MSM7X00A_6056_COMPAT)

	clk = clk_get(0, "mdp_clk");
#if 0
	if (clk)
		clk_enable(clk);
#endif
#endif

	ret = request_irq(INT_MDP, mdp_isr, IRQF_DISABLED, "msm_mdp", NULL);
	disable_irq(INT_MDP);
	mdp_irq_mask = 0;

	/* debug interface write access */
	writel(1, MSM_MDP_BASE + 0x60);

	writel(MDP_ANY_INTR_MASK, MDP_INTR_ENABLE);
	writel(1, MDP_EBI2_PORTMAP_MODE);

	writel(0, MDP_CMD_DEBUG_ACCESS_BASE + 0x01f8);
	writel(0, MDP_CMD_DEBUG_ACCESS_BASE + 0x01fc);

	for (n = 0; n < ARRAY_SIZE(csc_table); n++)
		writel(csc_table[n].val, csc_table[n].reg);

	/* clear up unused fg/main registers */
	/* comp.plane 2&3 ystride */
	writel(0, MDP_CMD_DEBUG_ACCESS_BASE + 0x0120);

	/* unpacked pattern */
	writel(0, MDP_CMD_DEBUG_ACCESS_BASE + 0x012c);
	writel(0, MDP_CMD_DEBUG_ACCESS_BASE + 0x0130);
	writel(0, MDP_CMD_DEBUG_ACCESS_BASE + 0x0134);
	writel(0, MDP_CMD_DEBUG_ACCESS_BASE + 0x0158);
	writel(0, MDP_CMD_DEBUG_ACCESS_BASE + 0x015c);
	writel(0, MDP_CMD_DEBUG_ACCESS_BASE + 0x0160);
	writel(0, MDP_CMD_DEBUG_ACCESS_BASE + 0x0170);
	writel(0, MDP_CMD_DEBUG_ACCESS_BASE + 0x0174);
	writel(0, MDP_CMD_DEBUG_ACCESS_BASE + 0x017c);

	/* comp.plane 2 & 3 */
	writel(0, MDP_CMD_DEBUG_ACCESS_BASE + 0x0114);
	writel(0, MDP_CMD_DEBUG_ACCESS_BASE + 0x0118);

	/* clear unused bg registers */
	writel(0, MDP_CMD_DEBUG_ACCESS_BASE + 0x01c8);
	writel(0, MDP_CMD_DEBUG_ACCESS_BASE + 0x01d0);
	writel(0, MDP_CMD_DEBUG_ACCESS_BASE + 0x01dc);
	writel(0, MDP_CMD_DEBUG_ACCESS_BASE + 0x01e0);
	writel(0, MDP_CMD_DEBUG_ACCESS_BASE + 0x01e4);

	for (n = 0; n < ARRAY_SIZE(mdp_upscale_table); n++)
		writel(mdp_upscale_table[n].val, mdp_upscale_table[n].reg);

	mdp_set_ccs(mdp_default_ccs);

	return 0;
}


