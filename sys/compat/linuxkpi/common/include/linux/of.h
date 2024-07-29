/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2023 Serenity Cyber Security, LLC.
 * Copyright (c) 2024 Aymeric Wibo <obiwac@freebsd.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef _LINUXKPI_LINUX_OF_H
#define	_LINUXKPI_LINUX_OF_H

#include <linux/kobject.h>
#include <linux/device.h>

struct device_node {
};

static inline int
of_device_is_compatible(const struct device_node *device, const char *name)
{

	pr_debug("%s: TODO\n", __func__);
	return 0;
}

static inline const void *
of_device_get_match_data(const struct device *dev)
{

	pr_debug("%s: TODO\n", __func__);
	return NULL;
}


static inline struct device_node *
of_find_matching_node_and_match(struct device_node *from, const struct of_device_id *matches, const struct of_device_id **match)
{

	pr_debug("%s: TODO\n", __func__);
	return NULL;
}

static inline void
of_node_put(struct device_node *node)
{

	pr_debug("%s: TODO\n", __func__);
}

static inline struct device_node *
of_find_compatible_node(struct device_node *from, const char *type, const char *compat)
{

	pr_debug("%s: TODO\n", __func__);
	return NULL;
}

static inline uint32_t *
of_get_address(struct device_node *dev, int index, uint64_t *size, uint32_t *flags)
{

	pr_debug("%s: TODO\n", __func__);
	return (0);
}

static inline bool
of_find_property(struct device_node *node, char const *prop, int *len)
{

	pr_debug("%s: TODO\n", __func__);
	return (false);
}

static inline int
of_property_match_string(struct device_node *node, const char *prop, const char *string)
{

	pr_debug("%s: TODO\n", __func__);
	return (0);
}

static inline struct device_node *
of_parse_phandle(struct device_node *node, char const *name, int index)
{

	pr_debug("%s: TODO\n", __func__);
	return (NULL);
}

static inline struct i2c_adapter *
of_find_i2c_adapter_by_node(struct device_node *node)
{

	pr_debug("%s: TODO\n", __func__);
	return (NULL);
}

static inline bool
of_property_read_bool(struct device_node *node, char const *prop)
{

	pr_debug("%s: TODO\n", __func__);
	return (false);
}

#endif
