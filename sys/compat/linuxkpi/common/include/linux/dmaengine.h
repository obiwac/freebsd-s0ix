/*-
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2024 Aymeric Wibo <obiwac@freebsd.org>
 */

#ifndef _LINUXKPI_LINUX_DMAENGINE_H
#define	_LINUXKPI_LINUX_DMAENGINE_H

#include <linux/device.h>

typedef int32_t dma_cookie_t;

struct dma_chan;

struct dma_async_tx_descriptor {
	dma_cookie_t	(*tx_submit)(struct dma_async_tx_descriptor *);
};

struct dma_device {
	struct dma_async_tx_descriptor	*(*device_prep_dma_memcpy)(struct dma_chan *, dma_addr_t, dma_addr_t, size_t, uint32_t);
};

struct dma_chan {
	struct dma_device	*device;
};

typedef struct {
} dma_cap_mask_t;

enum dma_transaction_type {
	DMA_MEMCPY,
};

static inline int
dma_submit_error(dma_cookie_t cookie)
{
	if (cookie < 0)
		return (cookie);
	return (0);
}

static inline int
dma_sync_wait(struct dma_chan *chan, dma_cookie_t cookie)
{

	pr_debug("%s: TODO\n", __func__);
	return (0);
}

static inline void
dma_release_channel(struct dma_chan *chan)
{

	pr_debug("%s: TODO\n", __func__);
}

static inline void
dma_cap_zero(dma_cap_mask_t mask)
{

	pr_debug("%s: TODO\n", __func__);
}

static inline void
dma_cap_set(enum dma_transaction_type type, dma_cap_mask_t mask)
{

	pr_debug("%s: TODO\n", __func__);
}

static inline struct dma_chan *
dma_request_chan_by_mask(dma_cap_mask_t const *mask)
{

	pr_debug("%s: TODO\n", __func__);
	return (NULL);
}

#endif	/* _LINUXKPI_LINUX_DMAENGINE_H */
