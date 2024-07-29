/*-
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2024 Aymeric Wibo <obiwac@freebsd.org>
 */

#ifndef _LINUXKPI_MEDIA_CEC_H
#define	_LINUXKPI_MEDIA_CEC_H

#include <linux/kernel.h>

#define	CEC_PHYS_ADDR_INVALID	0xFFFF

struct cec_msg {
};

struct cec_adapter {
};

static inline void
cec_s_phys_addr(struct cec_adapter *adapter, uint16_t phys_addr, bool block)
{

	pr_debug("%s: TODO\n", __func__);
}

static inline void
cec_phys_addr_invalidate(struct cec_adapter *adapter)
{

	cec_s_phys_addr(adapter, CEC_PHYS_ADDR_INVALID, false);
}

#endif	/* _LINUXKPI_MEDIA_CEC_H */
