/*-
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2024 Aymeric Wibo <obiwac@freebsd.org>
 */

#ifndef _LINUXKPI_SOUND_DMAENGINE_PCM_H
#define	_LINUXKPI_SOUND_DMAENGINE_PCM_H

#include <linux/dmaengine.h>
#include <sound/soc.h>

struct snd_dmaengine_dai_dma_data {
	uint32_t	addr;
	size_t	addr_width;
	uint32_t	maxburst;
};

static inline int
devm_snd_dmaengine_pcm_register(
	struct device *dev,
	struct snd_dmaengine_pcm_config const *config,
	uint32_t flags
)
{

	pr_debug("%s: TODO\n", __func__);
	return (0);
}

#endif	/* _LINUXKPI_SOUND_DMAENGINE_PCM_H */
