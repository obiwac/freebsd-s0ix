/*-
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2024 Aymeric Wibo <obiwac@freebsd.org>
 */

#ifndef	_LINUXKPI_LINUX_COMPONENT_H
#define	_LINUXKPI_LINUX_COMPONENT_H

#include <linux/device.h>

struct component_match {
};

struct component_master_ops {
	int	(*bind)(struct device *);
	void	(*unbind)(struct device *);
};

static inline void
component_match_add(struct device *parent, struct component_match **needle, int (*cmp)(struct device *, void *), void *data)
{

	pr_debug("%s: TODO\n", __func__);
}

static inline int
component_master_add_with_match(struct device *parent, struct component_master_ops const *master_ops, struct component_match *match)
{

	pr_debug("%s: TODO\n", __func__);
	return (0);
}

static inline void
component_master_del(struct device *dev, struct component_master_ops const *master_ops)
{

	pr_debug("%s: TODO\n", __func__);
}

static inline int
component_compare_dev(struct device *dev, void *data)
{

	pr_debug("%s: TODO\n", __func__);
	return (0);
}

static inline void
component_unbind_all(struct device *parent, void *data)
{

	pr_debug("%s: TODO\n", __func__);
}

static inline int
component_bind_all(struct device *parent, void *data)
{

	pr_debug("%s: TODO\n", __func__);
	return (0);
}

#endif	/* _LINUXKPI_LINUX_COMPONENT_H */
