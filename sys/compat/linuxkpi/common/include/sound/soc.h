/*-
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2024 Aymeric Wibo <obiwac@freebsd.org>
 */

#ifndef _LINUXKPI_SOUND_SOC_H
#define	_LINUXKPI_SOUND_SOC_H

#include <linux/device.h>

struct snd_soc_dai {
	struct device	*dev;
};

struct snd_soc_dai_link_component {
	char const	*dai_name;
	char const	*name;
};

struct snd_soc_dai_link {
	struct snd_soc_dai_link_component	*cpus;
	struct snd_soc_dai_link_component	*codecs;
	struct snd_soc_dai_link_component	*platforms;

	size_t	num_cpus;
	size_t	num_codecs;
	size_t	num_platforms;

	char const	*name;
	char const	*stream_name;
};

struct snd_soc_card {
	struct device		*dev;
	struct snd_soc_dai_link	*dai_link;
	size_t			num_links;
	char const		*name;
	char const		*driver_name;
	struct module		*owner;
};

struct snd_soc_dai_ops {
	int	(*probe)(struct snd_soc_dai *);
};

struct snd_soc_component_driver {
	char const	*name;
	uint32_t	legacy_dai_naming;
};

struct snd_soc_pcm_stream {
	char const	*stream_name;
	size_t		channels_min;
	size_t		channels_max;
	uint32_t		rates;
	uint32_t		formats;
};

struct snd_soc_dai_driver {
	char const							*name;
	struct snd_soc_dai_ops const	*ops;
	struct snd_soc_pcm_stream		playback;
};

struct snd_pcm_substream;
struct snd_pcm_hw_params;
struct dma_slave_config;

struct snd_dmaengine_pcm_config {
	char const	*chan_names[2];
	int			(*prepare_slave_config)(
		struct snd_pcm_substream *,
		struct snd_pcm_hw_params *,
		struct dma_slave_config *
	);
};

static inline int
snd_dmaengine_pcm_prepare_slave_config(
	struct snd_pcm_substream *substream,
	struct snd_pcm_hw_params *params,
	struct dma_slave_config *slave_config
)
{

	pr_debug("%s: TODO\n", __func__);
	return (0);
}

static inline void *
snd_soc_dai_get_drvdata(struct snd_soc_dai *dai)
{

	return (dev_get_drvdata(dai->dev));
}

static inline void *
snd_soc_card_get_drvdata(struct snd_soc_card *card)
{

	return (dev_get_drvdata(card->dev));
}

static inline int
devm_snd_soc_register_component(
	struct device *dev,
	struct snd_soc_component_driver const *component_driver,
	struct snd_soc_dai_driver *dai_drv, int num_dai
)
{

	pr_debug("%s: TODO\n", __func__);
	return (0);
}

static inline void
snd_soc_dai_init_dma_data(struct snd_soc_dai *dai, void *playback, void *capture)
{

	pr_debug("%s: TODO\n", __func__);
}

static inline void
snd_soc_card_set_drvdata(struct snd_soc_card *card, void *data)
{

	pr_debug("%s: TODO\n", __func__);
}

static inline int
devm_snd_soc_register_card(struct device *dev, struct snd_soc_card *card)
{

	pr_debug("%s: TODO\n", __func__);
	return (0);
}

#endif	/* _LINUXKPI_SOUND_SOC_H */
