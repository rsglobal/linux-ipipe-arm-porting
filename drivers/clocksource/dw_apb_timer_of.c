/*
 * Copyright (C) 2012 Altera Corporation
 * Copyright (c) 2011 Picochip Ltd., Jamie Iles
 *
 * Modified from mach-picoxcell/time.c
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <linux/delay.h>
#include <linux/dw_apb_timer.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/clk.h>
#include <linux/reset.h>
#include <linux/sched_clock.h>

static void __init timer_get_base_and_rate(struct device_node *np,
					   void __iomem **base, unsigned long *phys,
					   u32 *rate)
{
	struct clk *timer_clk;
	struct resource res;
	struct clk *pclk;
	struct reset_control *rstc;

	*base = of_iomap(np, 0);

	if (!*base || of_address_to_resource(np, 0, &res))
		panic("Unable to map regs for %s", np->name);

	*phys = res.start;

	/*
	 * Reset the timer if the reset control is available, wiping
	 * out the state the firmware may have left it
	 */
	rstc = of_reset_control_get(np, NULL);
	if (!IS_ERR(rstc)) {
		reset_control_assert(rstc);
		reset_control_deassert(rstc);
	}

	/*
	 * Not all implementations use a periphal clock, so don't panic
	 * if it's not present
	 */
	pclk = of_clk_get_by_name(np, "pclk");
	if (!IS_ERR(pclk))
		if (clk_prepare_enable(pclk))
			pr_warn("pclk for %pOFn is present, but could not be activated\n",
				np);

	timer_clk = of_clk_get_by_name(np, "timer");
	if (IS_ERR(timer_clk))
		goto try_clock_freq;

	if (!clk_prepare_enable(timer_clk)) {
		*rate = clk_get_rate(timer_clk);
		return;
	}

try_clock_freq:
	if (of_property_read_u32(np, "clock-freq", rate) &&
	    of_property_read_u32(np, "clock-frequency", rate))
		panic("No clock nor clock-frequency property for %pOFn", np);
}

static void __init add_clockevent(struct device_node *event_timer)
{
	void __iomem *iobase;
	struct dw_apb_clock_event_device *ced;
	unsigned long phys;
	u32 irq, rate;

	irq = irq_of_parse_and_map(event_timer, 0);
	if (irq == 0)
		panic("No IRQ for clock event timer");

	timer_get_base_and_rate(event_timer, &iobase, &phys, &rate);

	ced = dw_apb_clockevent_init(0, event_timer->name, 300, iobase, irq,
				     rate);
	if (!ced)
		panic("Unable to initialise clockevent device");

	dw_apb_clockevent_register(ced);
}

static void __iomem *sched_io_base;
static u32 sched_rate;

static void __init add_clocksource(struct device_node *source_timer)
{
	void __iomem *iobase;
	struct dw_apb_clocksource *cs;
	unsigned long phys;
	u32 rate;

	timer_get_base_and_rate(source_timer, &iobase, &phys, &rate);

	cs = dw_apb_clocksource_init(300, source_timer->name, iobase, phys, rate);
	if (!cs)
		panic("Unable to initialise clocksource device");

	dw_apb_clocksource_start(cs);
	dw_apb_clocksource_register(cs);

	/*
	 * Fallback to use the clocksource as sched_clock if no separate
	 * timer is found. sched_io_base then points to the current_value
	 * register of the clocksource timer.
	 */
	sched_io_base = iobase + 0x04;
	sched_rate = rate;
}

static u64 notrace read_sched_clock(void)
{
	return ~readl_relaxed(sched_io_base);
}

static const struct of_device_id sptimer_ids[] __initconst = {
	{ .compatible = "picochip,pc3x2-rtc" },
	{ /* Sentinel */ },
};

static void __init init_sched_clock(void)
{
	struct device_node *sched_timer;
	unsigned long phys;

	sched_timer = of_find_matching_node(NULL, sptimer_ids);
	if (sched_timer) {
		timer_get_base_and_rate(sched_timer, &sched_io_base,
					&phys, &sched_rate);
		of_node_put(sched_timer);
	}

	sched_clock_register(read_sched_clock, 32, sched_rate);
}

#ifdef CONFIG_ARM
static unsigned long dw_apb_delay_timer_read(void)
{
	return ~readl_relaxed(sched_io_base);
}

static struct delay_timer dw_apb_delay_timer = {
	.read_current_timer	= dw_apb_delay_timer_read,
};
#endif

static int num_called;
static int __init dw_apb_timer_init(struct device_node *timer)
{
	switch (num_called) {
	case 0:
		pr_debug("%s: found clockevent timer\n", __func__);
		add_clockevent(timer);
		break;
	case 1:
		pr_debug("%s: found clocksource timer\n", __func__);
		add_clocksource(timer);
		init_sched_clock();
#ifdef CONFIG_ARM
		dw_apb_delay_timer.freq = sched_rate;
		register_current_timer_delay(&dw_apb_delay_timer);
#endif
		break;
	default:
		break;
	}

	num_called++;

	return 0;
}
TIMER_OF_DECLARE(pc3x2_timer, "picochip,pc3x2-timer", dw_apb_timer_init);
TIMER_OF_DECLARE(apb_timer_osc, "snps,dw-apb-timer-osc", dw_apb_timer_init);
TIMER_OF_DECLARE(apb_timer_sp, "snps,dw-apb-timer-sp", dw_apb_timer_init);
TIMER_OF_DECLARE(apb_timer, "snps,dw-apb-timer", dw_apb_timer_init);
