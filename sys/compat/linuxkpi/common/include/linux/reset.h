/*-
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2024 Aymeric Wibo <obiwac@freebsd.com>
 */

#ifndef	_LINUXKPI_LINUX_RESET_H
#define	_LINUXKPI_LINUX_RESET_H

#include <linux/device.h>

struct reset_control {
};

static inline int
reset_control_reset(struct reset_control *rstc)
{

	pr_debug("%s: TODO\n", __func__);
	return (0);
}

static inline struct reset_control *
devm_reset_control_get(struct device *dev, char const *id)
{

	pr_debug("%s: TODO\n", __func__);
	return (NULL);
}

#endif	/* _LINUXKPI_LINUX_RESET_H */
