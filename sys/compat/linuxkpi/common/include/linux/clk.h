/*-
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2024 Aymeric Wibo <obiwac@freebsd.org>
 */

#ifndef _LINUXKPI_LINUX_CLK_H
#define	_LINUXKPI_LINUX_CLK_H

#include <linux/device.h>

struct clk {
};

static inline void
clk_enable(struct clk *clk)
{

	pr_debug("%s: TODO\n", __func__);
}

static inline void
clk_disable(struct clk *clk)
{

	pr_debug("%s: TODO\n", __func__);
}

static inline void
clk_prepare(struct clk *clk)
{

	pr_debug("%s: TODO\n", __func__);
}

static inline void
clk_unprepare(struct clk *clk)
{

	pr_debug("%s: TODO\n", __func__);
}

static inline void
clk_disable_unprepare(struct clk *clk)
{

	clk_disable(clk);
	clk_unprepare(clk);
}

static inline int
clk_prepare_enable(struct clk *clk)
{

	clk_prepare(clk);
	clk_enable(clk);

	return (0);
}

static inline int
clk_set_rate(struct clk *clk, uint32_t rate)
{
	
	pr_debug("%s: TODO\n", __func__);
	return (0);
}

static inline struct clk *
devm_clk_get(struct device *dev, char const *id)
{

	pr_debug("%s: TODO\n", __func__);
	return (NULL);
}

#endif	/* _LINUXKPI_LINUX_CLK_H */
