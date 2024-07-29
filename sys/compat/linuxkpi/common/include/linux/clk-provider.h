/*-
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2024 Aymeric Wibo <obiwac@freebsd.org>
 */

#ifndef _LINUXKPI_LINUX_CLK_PROVIDER_H
#define	_LINUXKPI_LINUX_CLK_PROVIDER_H

#include <linux/clk.h>

struct clk_fixed_factor {
	uint32_t	mult;
	uint32_t	div;
	struct clk_hw	hw;
};

struct of_phandle_args {
};

static inline int
of_clk_add_hw_provider(struct device_node *np, struct clk_hw *(*get)(struct of_phandle_args *, void *), void *data)
{

	pr_debug("%s: TODO\n", __func__);
	return (0);
}

static inline struct clk_hw *
of_clk_hw_onecell_get(struct of_phandle_args *clkspec, void *data)
{

	pr_debug("%s: TODO\n", __func__);
	return (NULL);
}

#endif	/* _LINUXKPI_LINUX_CLK_PROVIDER_H */
