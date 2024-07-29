/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2020-2022 Bjoern A. Zeeb
 * Copyright (c) 2024 Aymeric Wibo <obiwac@freebsd.org>
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

#ifndef	_LINUXKPI_LINUX_PLATFORM_DEVICE_H
#define	_LINUXKPI_LINUX_PLATFORM_DEVICE_H

#include <linux/kernel.h>
#include <linux/device.h>

#define	PLATFORM_DEVID_AUTO	(-2)

struct platform_device {
	const char			*name;
	int				id;
	bool				id_auto;
	struct device			dev;
};

struct platform_driver {
	int				(*remove)(struct platform_device *);
	void				(*remove_new)(struct platform_device *);
	int				(*probe)(struct platform_device *);
	void				(*shutdown)(struct platform_device *);
	struct device_driver		driver;
};

#define	dev_is_platform(dev)	(false)
#define	to_platform_device(dev)	(NULL)

static __inline int
platform_driver_register(struct platform_driver *pdrv)
{

	pr_debug("%s: TODO\n", __func__);
	return (-ENXIO);
}

static __inline void *
dev_get_platdata(struct device *dev)
{

	pr_debug("%s: TODO\n", __func__);
	return (NULL);
}

static __inline int
platform_driver_probe(struct platform_driver *pdrv,
    int(*pd_probe_f)(struct platform_device *))
{

	pr_debug("%s: TODO\n", __func__);
	return (-ENODEV);
}

static __inline void
platform_driver_unregister(struct platform_driver *pdrv)
{

	pr_debug("%s: TODO\n", __func__);
	return;
}

static __inline int
platform_device_register(struct platform_device *pdev)
{
	pr_debug("%s: TODO\n", __func__);
	return (0);
}

static __inline void
platform_device_unregister(struct platform_device *pdev)
{

	pr_debug("%s: TODO\n", __func__);
	return;
}

static inline int
platform_get_irq(struct platform_device *pdev, uint32_t irq)
{

	pr_debug("%s: TODO\n", __func__);
	return (0);
}

static inline void
platform_set_drvdata(struct platform_device *pdev, void *data)
{

	dev_set_drvdata(&pdev->dev, data);
}

static inline void *
platform_get_drvdata(const struct platform_device *pdev)
{

	return (dev_get_drvdata(&pdev->dev));
}



static inline void*
devm_platform_get_and_ioremap_resource(struct platform_device *pdev, uint32_t index, struct resource **res)
{

	pr_debug("%s: TODO\n", __func__);
	return (NULL);
}

static inline void*
devm_platform_ioremap_resource(struct platform_device *pdev, uint32_t index)
{

	pr_debug("%s: TODO\n", __func__);
	return (NULL);
}

static inline struct device *
platform_find_device_by_driver(struct device *start, struct device_driver const *driver)
{

	pr_debug("%s: TODO\n", __func__);
	return (NULL);
}

static inline int
platform_register_drivers(struct platform_driver * const *drivers, size_t count)
{

	pr_debug("%s: TODO\n", __func__);
	return (0);
}

static inline void
platform_unregister_drivers(struct platform_driver * const *drivers, size_t count)
{

	pr_debug("%s: TODO\n", __func__);
	return;
}

static inline struct platform_device *
platform_device_register_data(
	struct device *parent,
	char const *name,
	int id,
	void const *data,
	size_t size
)
{

	pr_debug("%s: TODO\n", __func__);
	return (NULL);
}

static inline int
platform_get_irq_byname(struct platform_device *dev, char const *name)
{

	pr_debug("%s: TODO\n", __func__);
	return (0);
}

struct resource {
	resource_size_t	start;
};

static inline resource_size_t
resource_size(struct resource const *res)
{

	pr_debug("%s: TODO\n", __func__);
	return (0);
}

static inline struct resource *
platform_get_resource_byname(struct platform_device *dev, uint32_t io_resource, char const *name)
{

	pr_debug("%s: TODO\n", __func__);
	return (NULL);
}

#endif	/* _LINUXKPI_LINUX_PLATFORM_DEVICE_H */
