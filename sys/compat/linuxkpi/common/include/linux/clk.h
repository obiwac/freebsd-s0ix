/*-
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2024 Aymeric Wibo <obiwac@freebsd.org>
 */

#ifndef _LINUXKPI_LINUX_CLK_H
#define	_LINUXKPI_LINUX_CLK_H

#include <linux/device.h>

struct clk {
};

struct clk_ops {
};

static struct clk_ops const clk_fixed_factor_ops = {
};

struct clk_init_data {
	size_t			num_parents;
	char const		**parent_names;
	struct clk_ops const	*ops;
	char			*name;
};

struct clk_hw {
	struct clk_init_data const	*init;
};

struct clk_hw_onecell_data {
	size_t	num;
	struct clk_hw	**hws;
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
clk_set_rate(struct clk *clk, uint64_t rate)
{

	pr_debug("%s: TODO\n", __func__);
	return (0);
}

static inline int
clk_set_min_rate(struct clk *clk, uint64_t rate)
{

	pr_debug("%s: TODO\n", __func__);
	return (0);
}

static inline uint64_t
clk_get_rate(struct clk *clk)
{

	pr_debug("%s: TODO\n", __func__);
	return (0);
}

static inline struct clk *
clk_get_parent(struct clk *clk)
{

	pr_debug("%s: TODO\n", __func__);
	return (NULL);
}

static inline char const *
__clk_get_name(struct clk *clk)
{

	pr_debug("%s: TODO\n", __func__);
	return (NULL);
}

static inline struct clk *
devm_clk_get(struct device *dev, char const *id)
{

	pr_debug("%s: TODO\n", __func__);
	return (NULL);
}

static inline int
devm_clk_hw_register(struct device *dev, struct clk_hw *hw)
{

	pr_debug("%s: TODO\n", __func__);
	return (0);
}

#endif	/* _LINUXKPI_LINUX_CLK_H */
