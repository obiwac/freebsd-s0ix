/*-
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2024 Aymeric Wibo <obiwac@freebsd.org>
 */

#ifndef	_LINUXKPI_LINUX_OF_DEVICE_H
#define	_LINUXKPI_LINUX_OF_DEVICE_H

#include <linux/device.h>

static inline int
of_dma_configure(struct device *dev, struct device_node *np, bool force_dma)
{

	pr_debug("%s: TODO\n", __func__);
	return (0);
}

#endif	/* _LINUXKPI_LINUX_OF_DEVICE_H */
