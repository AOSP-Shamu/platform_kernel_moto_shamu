/*
 * Copyright (c) 2009 Google, Inc.
 * Copyright (c) 2008 QUALCOMM Incorporated.
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

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/errno.h>
#include <linux/cpufreq.h>
#include <linux/regulator/consumer.h>

#include <mach/board.h>
#include <mach/msm_iomap.h>

#include "acpuclock.h"

#if 0
#define DEBUG(x...) pr_info(x)
#else
#define DEBUG(x...) do {} while (0)
#endif

#define SHOT_SWITCH	4
#define HOP_SWITCH	5
#define SIMPLE_SLEW	6
#define COMPLEX_SLEW	7

#define SPSS_CLK_CNTL_ADDR	(MSM_CSR_BASE + 0x100)
#define SPSS_CLK_SEL_ADDR	(MSM_CSR_BASE + 0x104)

/* Scorpion PLL registers */
#define SCPLL_CTL_ADDR		(MSM_SCPLL_BASE + 0x4)
#define SCPLL_STATUS_ADDR	(MSM_SCPLL_BASE + 0x18)
#define SCPLL_FSM_CTL_EXT_ADDR	(MSM_SCPLL_BASE + 0x10)

struct clkctl_acpu_speed {
	unsigned acpu_khz;
	unsigned clk_cfg;
	unsigned clk_sel;
	unsigned sc_l_value;
	unsigned lpj;
	int      vdd;
};

/* clock sources */
#define CLK_TCXO	0 /* 19.2 MHz */
#define CLK_GLOBAL_PLL	1 /* 768 MHz */
#define CLK_MODEM_PLL	4 /* 245 MHz (UMTS) or 235.93 MHz (CDMA) */

#define CCTL(src, div) (((src) << 4) | (div - 1))

/* core sources */
#define SRC_RAW		0 /* clock from SPSS_CLK_CNTL */
#define SRC_SCPLL	1 /* output of scpll 128-998 MHZ */
#define SRC_AXI		2 /* 128 MHz */
#define SRC_PLL1	3 /* 768 MHz */

struct clkctl_acpu_speed acpu_freq_tbl[] = {
	{  19200, CCTL(CLK_TCXO, 1),		SRC_RAW, 0, 0, 1000 },
	{ 128000, CCTL(CLK_TCXO, 1),		SRC_AXI, 0, 0, 1000 },
	{ 245000, CCTL(CLK_MODEM_PLL, 1),	SRC_RAW, 0, 0, 1000 },
	{ 256000, CCTL(CLK_GLOBAL_PLL, 3),	SRC_RAW, 0, 0, 1000 },
	{ 384000, CCTL(CLK_TCXO, 1),		SRC_SCPLL, 0x0A, 0, 1000 },
	{ 422400, CCTL(CLK_TCXO, 1),		SRC_SCPLL, 0x0B, 0, 1000 },
	{ 460800, CCTL(CLK_TCXO, 1),		SRC_SCPLL, 0x0C, 0, 1000 },
	{ 499200, CCTL(CLK_TCXO, 1),		SRC_SCPLL, 0x0D, 0, 1025 },
	{ 537600, CCTL(CLK_TCXO, 1),		SRC_SCPLL, 0x0E, 0, 1050 },
	{ 576000, CCTL(CLK_TCXO, 1),		SRC_SCPLL, 0x0F, 0, 1050 },
	{ 614400, CCTL(CLK_TCXO, 1),		SRC_SCPLL, 0x10, 0, 1075 },
	{ 652800, CCTL(CLK_TCXO, 1),		SRC_SCPLL, 0x11, 0, 1100 },
	{ 691200, CCTL(CLK_TCXO, 1),		SRC_SCPLL, 0x12, 0, 1125 },
	{ 729600, CCTL(CLK_TCXO, 1),		SRC_SCPLL, 0x13, 0, 1150 },
	{ 768000, CCTL(CLK_TCXO, 1),		SRC_SCPLL, 0x14, 0, 1150 },
	{ 806400, CCTL(CLK_TCXO, 1),		SRC_SCPLL, 0x15, 0, 1175 },
	{ 844800, CCTL(CLK_TCXO, 1),		SRC_SCPLL, 0x16, 0, 1200 },
	{ 883200, CCTL(CLK_TCXO, 1),		SRC_SCPLL, 0x17, 0, 1225 },
	{ 921600, CCTL(CLK_TCXO, 1),		SRC_SCPLL, 0x18, 0, 1250 },
	{ 960000, CCTL(CLK_TCXO, 1),		SRC_SCPLL, 0x19, 0, 1250 },
	{ 998400, CCTL(CLK_TCXO, 1),		SRC_SCPLL, 0x1A, 0, 1275 },
	{ 0 },
};

/* select the standby clock that is used when switching scpll
 * frequencies
 *
 * Currently: TCXO
 */
struct clkctl_acpu_speed *acpu_stby = &acpu_freq_tbl[0];
#define IS_ACPU_STANDBY(x)	(((x)->clk_cfg == acpu_stby->clk_cfg) && \
				 ((x)->clk_sel == acpu_stby->clk_sel))

#ifdef CONFIG_CPU_FREQ_TABLE
static struct cpufreq_frequency_table freq_table[ARRAY_SIZE(acpu_freq_tbl)];

static void __init acpuclk_init_cpufreq_table(void)
{
	int i;
	for (i = 0; acpu_freq_tbl[i].acpu_khz; i++) {
		freq_table[i].index = i;
		/* Skip speeds using the global pll */
		if (acpu_freq_tbl[i].acpu_khz == 256000 ||
				acpu_freq_tbl[i].acpu_khz == 19200) {
			freq_table[i].frequency = CPUFREQ_ENTRY_INVALID;
			continue;
		}
		freq_table[i].frequency = acpu_freq_tbl[i].acpu_khz;
	}

	freq_table[i].index = i;
	freq_table[i].frequency = CPUFREQ_TABLE_END;

	cpufreq_frequency_table_get_attr(freq_table, smp_processor_id());
}
#else
#define acpuclk_init_cpufreq_table() do {} while (0);
#endif

struct clock_state {
	struct clkctl_acpu_speed	*current_speed;
	struct mutex			lock;
	uint32_t			acpu_switch_time_us;
	uint32_t			max_speed_delta_khz;
	uint32_t			vdd_switch_time_us;
	unsigned long			power_collapse_khz;
	unsigned long			wait_for_irq_khz;
	struct regulator                *regulator;
};

static struct clock_state drv_state = { 0 };

static DEFINE_SPINLOCK(acpu_lock);

static void scpll_set_freq(uint32_t lval)
{
	uint32_t regval;

	if (lval > 33)
		lval = 33;
	if (lval < 10)
		lval = 10;

	/* wait for any calibrations or frequency switches to finish */
	while (readl(SCPLL_STATUS_ADDR) & 0x3)
		;

	/* write the new L val and switch mode */
	regval = readl(SCPLL_FSM_CTL_EXT_ADDR);
	regval &= ~(0x3f << 3);
	regval |= (lval << 3);

	regval &= ~(0x3 << 0);
	regval |= (HOP_SWITCH << 0);
	writel(regval, SCPLL_FSM_CTL_EXT_ADDR);

	dmb();

	/* put in normal mode */
	regval = readl(SCPLL_CTL_ADDR);
	regval |= 0x7;
	writel(regval, SCPLL_CTL_ADDR);

	dmb();

	/* wait for frequency switch to finish */
	while (readl(SCPLL_STATUS_ADDR) & 0x1)
		;

	/* status bit seems to clear early, requires at least
	 * ~8 microseconds to settle, using 100uS based on stability
	 * tests across tempeperature/process  */
	udelay(100);
}

static void scpll_apps_enable(bool state)
{
	uint32_t regval;

	/* Wait for any frequency switches to finish. */
	while (readl(SCPLL_STATUS_ADDR) & 0x1)
		;

	/* put the pll in standby mode */
	regval = readl(SCPLL_CTL_ADDR);
	regval &= ~(0x7);
	regval |= (0x2);
	writel(regval, SCPLL_CTL_ADDR);

	dmb();

	if (state) {
		/* put the pll in normal mode */
		regval = readl(SCPLL_CTL_ADDR);
		regval |= (0x7);
		writel(regval, SCPLL_CTL_ADDR);
	} else {
		/* put the pll in power down mode */
		regval = readl(SCPLL_CTL_ADDR);
		regval &= ~(0x7);
		writel(regval, SCPLL_CTL_ADDR);
	}
	udelay(drv_state.vdd_switch_time_us);
}

static void scpll_init(uint32_t lval)
{
	/* power down scpll */
	writel(0x0, SCPLL_CTL_ADDR);

	dmb();

	/* set bypassnl, put into standby */
	writel(0x00400002, SCPLL_CTL_ADDR);

	/* set bypassnl, reset_n, full calibration */
	writel(0x00600004, SCPLL_CTL_ADDR);

	/* Ensure register write to initiate calibration has taken
	effect before reading status flag */
	dmb();

	/* wait for cal_all_done */
	while (readl(SCPLL_STATUS_ADDR) & 0x2)
		;

	/* power down scpll */
	writel(0x0, SCPLL_CTL_ADDR);

	/* switch scpll to desired freq */
	scpll_set_freq(lval);
}

/* this is still a bit weird... */
static void select_clock(unsigned src, unsigned config)
{
	uint32_t val;

	if (src == SRC_RAW) {
		uint32_t sel = readl(SPSS_CLK_SEL_ADDR);
		unsigned shift = (sel & 1) ? 8 : 0;

		/* set other clock source to the new configuration */
		val = readl(SPSS_CLK_CNTL_ADDR);
		val = (val & (~(0x7F << shift))) | (config << shift);
		writel(val, SPSS_CLK_CNTL_ADDR);

		/* switch to other clock source */
		writel(sel ^ 1, SPSS_CLK_SEL_ADDR);

		dmb(); /* necessary? */
	}

	/* switch to new source */
	val = readl(SPSS_CLK_SEL_ADDR) & (~6);
	writel(val | ((src & 3) << 1), SPSS_CLK_SEL_ADDR);
}

static int acpuclk_set_vdd_level(int vdd)
{
	if (!drv_state.regulator || IS_ERR(drv_state.regulator)) {
		drv_state.regulator = regulator_get(NULL, "acpu_vcore");
		if (IS_ERR(drv_state.regulator)) {
			pr_info("acpuclk_set_vdd_level %d no regulator\n", vdd);
			/* Assume that the PMIC supports scaling the processor
			 * to its maximum frequency at its default voltage.
			 */
			return 0;
		}
		pr_info("acpuclk_set_vdd_level got regulator\n");
	}
	vdd *= 1000; /* mV -> uV */
	return regulator_set_voltage(drv_state.regulator, vdd, vdd);
}

int acpuclk_set_rate(unsigned long rate, int for_power_collapse)
{
	struct clkctl_acpu_speed *cur, *next;
	unsigned long flags;

	cur = drv_state.current_speed;

	/* convert to KHz */
	rate /= 1000;

	DEBUG("acpuclk_set_rate(%d,%d)\n", (int) rate, for_power_collapse);

	if (rate == cur->acpu_khz)
		return 0;

	next = acpu_freq_tbl;
	for (;;) {
		if (next->acpu_khz == rate)
			break;
		if (next->acpu_khz == 0)
			return -EINVAL;
		next++;
	}

	if (!for_power_collapse) {
		mutex_lock(&drv_state.lock);
		/* Increase VDD if needed. */
		if (next->vdd > cur->vdd) {
			if (acpuclk_set_vdd_level(next->vdd)) {
				pr_err("acpuclock: Unable to increase ACPU VDD.\n");
				mutex_unlock(&drv_state.lock);
				return -EINVAL;
			}
		}
	}

	spin_lock_irqsave(&acpu_lock, flags);

	DEBUG("sel=%d cfg=%02x lv=%02x -> sel=%d, cfg=%02x lv=%02x\n",
	      cur->clk_sel, cur->clk_cfg, cur->sc_l_value,
	      next->clk_sel, next->clk_cfg, next->sc_l_value);

	if (next->clk_sel == SRC_SCPLL) {
		if (cur->clk_sel != SRC_SCPLL)
			scpll_apps_enable(1);
		if (!IS_ACPU_STANDBY(cur))
			select_clock(acpu_stby->clk_sel, acpu_stby->clk_cfg);
		loops_per_jiffy = next->lpj;
		scpll_set_freq(next->sc_l_value);
		select_clock(SRC_SCPLL, 0);
	} else {
		loops_per_jiffy = next->lpj;
		if (cur->clk_sel == SRC_SCPLL) {
			select_clock(acpu_stby->clk_sel, acpu_stby->clk_cfg);
			select_clock(next->clk_sel, next->clk_cfg);
			scpll_apps_enable(0);
		} else {
			select_clock(next->clk_sel, next->clk_cfg);
		}
	}

	drv_state.current_speed = next;

	spin_unlock_irqrestore(&acpu_lock, flags);
	if (!for_power_collapse) {
		/* Drop VDD level if we can. */
		if (next->vdd < cur->vdd) {
			if (acpuclk_set_vdd_level(next->vdd))
				pr_err("acpuclock: Unable to drop ACPU VDD.\n");
		}
		mutex_unlock(&drv_state.lock);
	}

	return 0;
}

static unsigned __init acpuclk_find_speed(void)
{
	uint32_t sel, val;

	sel = readl(SPSS_CLK_SEL_ADDR);
	switch ((sel & 6) >> 1) {
	case 1:
		val = readl(SCPLL_FSM_CTL_EXT_ADDR);
		val = (val >> 3) & 0x3f;
		return val * 38400;
	case 2:
		return 128000;
	default:
		pr_err("acpu_find_speed: failed\n");
		BUG();
		return 0;
	}
}

static void __init acpuclk_init(void)
{
	struct clkctl_acpu_speed *speed;
	unsigned init_khz;

	init_khz = acpuclk_find_speed();

	/* Move to 768MHz for boot, which is a safe frequency
	 * for all versions of Scorpion at the moment.
	 */
	speed = acpu_freq_tbl;
	for (;;) {
		if (speed->acpu_khz == 768000)
			break;
		if (speed->acpu_khz == 0) {
			pr_err("acpuclk_init: cannot find 768MHz\n");
			BUG();
		}
		speed++;
	}

	if (init_khz != speed->acpu_khz) {
		/* Force over to standby clock so we can init the SCPLL
		 * even if it was already running when we started.
		 */
		select_clock(acpu_stby->clk_sel, acpu_stby->clk_cfg);

		scpll_init(0x14);

		scpll_apps_enable(1);
		scpll_set_freq(speed->sc_l_value);
		select_clock(SRC_SCPLL, 0);
	}
	drv_state.current_speed = speed;

	for (speed = acpu_freq_tbl; speed->acpu_khz; speed++)
		speed->lpj = cpufreq_scale(loops_per_jiffy,
					   init_khz, speed->acpu_khz);

	loops_per_jiffy = drv_state.current_speed->lpj;
}

unsigned long acpuclk_get_rate(void)
{
	return drv_state.current_speed->acpu_khz;
}

uint32_t acpuclk_get_switch_time(void)
{
	return drv_state.acpu_switch_time_us;
}

unsigned long acpuclk_power_collapse(void)
{
	int ret = acpuclk_get_rate();
	acpuclk_set_rate(drv_state.power_collapse_khz, 1);
	return ret * 1000;
}

unsigned long acpuclk_wait_for_irq(void)
{
	int ret = acpuclk_get_rate();
	acpuclk_set_rate(drv_state.wait_for_irq_khz, 1);
	return ret * 1000;
}

void __init msm_acpu_clock_init(struct msm_acpu_clock_platform_data *clkdata)
{
	spin_lock_init(&acpu_lock);
	mutex_init(&drv_state.lock);

	drv_state.acpu_switch_time_us = clkdata->acpu_switch_time_us;
	drv_state.max_speed_delta_khz = clkdata->max_speed_delta_khz;
	drv_state.vdd_switch_time_us = clkdata->vdd_switch_time_us;
	drv_state.power_collapse_khz = clkdata->power_collapse_khz;
	drv_state.wait_for_irq_khz = clkdata->wait_for_irq_khz;

	acpuclk_init();
	acpuclk_init_cpufreq_table();
}
