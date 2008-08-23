/* drivers/video/msm_fb/msm_fb.h
 *
 * Internal shared definitions for various MSM framebuffer parts.
 *
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

#ifndef _MSM_FB_H_
#define _MSM_FB_H_

struct mddi_info;

struct mddi_panel_info
{
	struct mddi_info *mddi;
	struct mddi_panel_ops *panel_ops;
	uint16_t width;
	uint16_t height;
	unsigned long fb_base;
	unsigned long fb_size;
	unsigned int ok;
};

struct msmfb_callback {
	void (*func)(struct msmfb_callback *);
};

struct mddi_panel_ops
{
	void (*power)(struct mddi_panel_info *panel, int on);
	void (*enable)(struct mddi_panel_info *panel);
	void (*disable)(struct mddi_panel_info *panel);
	void (*wait_vsync)(struct mddi_panel_info *panel);
	void (*request_vsync)(struct mddi_panel_info *panel,
			      struct msmfb_callback *callback);
};

int mddi_add_panel(struct mddi_info *mddi, struct mddi_panel_ops *ops);
void mddi_remote_write(struct mddi_info *mddi, unsigned val, unsigned reg);
unsigned mddi_remote_read(struct mddi_info *mddi, unsigned reg);
void mddi_activate_link(struct mddi_info *mddi);
void mddi_set_auto_hibernate(struct mddi_info *mddi, int on);
int mddi_check_status(struct mddi_info *mddi);

void mdp_dma_to_mddi(uint32_t addr, uint32_t stride, uint32_t width,
		     uint32_t height, uint32_t x, uint32_t y,
		     struct msmfb_callback* callback);
void mdp_dma_wait(void);
int mdp_ppp_wait(void);
int enable_mdp_irq(uint32_t mask);
int disable_mdp_irq(uint32_t mask);
void mdp_set_grp_disp(unsigned disp_id);

struct fb_info;
struct mdp_blit_req;
int mdp_blit(struct fb_info *info, struct mdp_blit_req *req);

#endif
