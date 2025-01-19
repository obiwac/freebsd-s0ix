/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2024 The FreeBSD Foundation
 *
 * This software was developed by Aymeric Wibo <obiwac@freebsd.org>
 * under sponsorship from the FreeBSD Foundation.
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/module.h>

#include <dev/pci/pcivar.h>

#define SMU_INDEX_ADDRESS	0xB8
#define SMU_INDEX_DATA		0xBC

#define SMU_BASE_ADDR_LO	0x13B102E8
#define SMU_BASE_ADDR_HI	0x13B102EC

/*
 * TODO These are in common with amdtemp; should we find a way to factor these
 * out?  Also there are way more of these.  I couldn't find a centralized place
 * which lists them though.
 */
#define VENDORID_AMD		0x1022
#define CPUID_AMD_REMBRANDT	0x14B5
#define CPUID_AMD_PHOENIX	0x14E8
#define CPUID_AMD_STRIX_POINT	0x14A4

static const struct amdpmc_product {
	uint16_t	amdpmc_vendorid;
	uint16_t	amdpmc_deviceid;
} amdpmc_products[] = {
	{ VENDORID_AMD,	CPUID_AMD_REMBRANDT },
	{ VENDORID_AMD,	CPUID_AMD_PHOENIX },
	{ VENDORID_AMD,	CPUID_AMD_STRIX_POINT },
};

struct amdpmc_softc {
};

static bool
amdpmc_match(device_t dev, const struct amdpmc_product **product_out)
{
	const uint16_t vendorid = pci_get_vendor(dev);
	const uint16_t deviceid = pci_get_device(dev);

	for (size_t i = 0; i < nitems(amdpmc_products); i++) {
		const struct amdpmc_product *prod = &amdpmc_products[i];

		if (vendorid == prod->amdpmc_vendorid &&
		    deviceid == prod->amdpmc_deviceid) {
			if (product_out != NULL)
				*product_out = prod;
			return (true);
		}
	}
	return (false);
}

static void
amdpmc_identify(driver_t *driver, device_t parent)
{

	if (device_find_child(parent, "amdpmc", -1) != NULL)
		return;

	if (amdpmc_match(parent, NULL)) {
		if (device_add_child(parent, "amdpmc", -1) == NULL)
			device_printf(parent, "add amdpmc child failed\n");
	}
}

static int
amdpmc_probe(device_t dev)
{
	uint32_t base_addr_lo, base_addr_hi;
	uint64_t base_addr;

	if (resource_disabled("amdpmc", 0))
		return (ENXIO);
	if (!amdpmc_match(device_get_parent(dev), NULL))
		return (ENXIO);

	pci_write_config(dev, SMU_INDEX_ADDRESS, SMU_BASE_ADDR_LO, 4);
	base_addr_lo = pci_read_config(dev, SMU_INDEX_DATA, 4);

	pci_write_config(dev, SMU_INDEX_ADDRESS, SMU_BASE_ADDR_HI, 4);
	base_addr_hi = pci_read_config(dev, SMU_INDEX_DATA, 4);

	base_addr = (uint64_t)base_addr_hi << 32 | base_addr_lo;
	device_printf(dev, "PMC base addr: 0x%lx\n", base_addr);

	return (BUS_PROBE_GENERIC);
}

static device_method_t amdpmc_methods[] = {
	DEVMETHOD(device_identify,	amdpmc_identify),
	DEVMETHOD(device_probe,		amdpmc_probe),
	// DEVMETHOD(device_attach,	amdpmc_attach),
	// DEVMETHOD(device_detach,	amdpmc_detach),
	DEVMETHOD_END
};

static driver_t amdpmc_driver = {
	"amdpmc",
	amdpmc_methods,
	sizeof(struct amdpmc_softc),
};

DRIVER_MODULE(amdpmc, hostb, amdpmc_driver, NULL, NULL);
MODULE_VERSION(amdpmc, 1);
MODULE_DEPEND(amdpmc, amdsmn, 1, 1, 1);
MODULE_PNP_INFO("U16:vendor;U16:device", pci, amdpmc, amdpmc_products,
    nitems(amdpmc_products));
