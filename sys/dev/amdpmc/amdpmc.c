/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2024 The FreeBSD Foundation
 *
 * This software was developed by Aymeric Wibo <obiwac@freebsd.org>
 * under sponsorship from the FreeBSD Foundation.
 */

/* TODO Should this be called amdsmu instead? */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/rman.h>

#include <machine/bus.h>

#include <dev/pci/pcivar.h>

#define SMU_INDEX_ADDRESS	0xB8
#define SMU_INDEX_DATA		0xBC

#define SMU_PHYSBASE_ADDR_LO	0x13B102E8
#define SMU_PHYSBASE_ADDR_HI	0x13B102EC

#define SMU_MEM_SIZE		0x1000
#define SMU_REG_OFF		0x10000
#define SMU_FW_VERSION		0x0

#define SMU_REG_MESSAGE		0x538
#define SMU_REG_RESPONSE	0x980
#define SMU_REG_ARGUMENT	0x9BC

#define SMU_RES_OK		0x01
#define SMU_RES_REJECT_BUSY	0xFC
#define SMU_RES_REJECT_PREREQ	0xFD
#define SMU_RES_UNKNOWN		0xFE
#define SMU_RES_FAILED		0xFF

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
	struct resource		*res;
	bus_space_tag_t 	bus_tag;
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

	if (resource_disabled("amdpmc", 0))
		return (ENXIO);
	if (!amdpmc_match(device_get_parent(dev), NULL))
		return (ENXIO);
	return (BUS_PROBE_GENERIC);
}

static int
amdpmc_attach(device_t dev)
{
	struct amdpmc_softc *sc = device_get_softc(dev);
	uint32_t physbase_addr_lo, physbase_addr_hi;
	uint64_t physbase_addr;
	int rid = 0;
	bus_space_handle_t smu, reg;
	uint32_t fw_vers;

	/*
	 * Find physical base address for SMU.
	 * XXX I am a little confused about the masks here.  I'm just copying
	 * what Linux does in the amd-pmc driver to get the base address.
	 */
	pci_write_config(dev, SMU_INDEX_ADDRESS, SMU_PHYSBASE_ADDR_LO, 4);
	physbase_addr_lo = pci_read_config(dev, SMU_INDEX_DATA, 4) & 0xFFF00000;

	pci_write_config(dev, SMU_INDEX_ADDRESS, SMU_PHYSBASE_ADDR_HI, 4);
	physbase_addr_hi = pci_read_config(dev, SMU_INDEX_DATA, 4) & 0x0000FFFF;

	physbase_addr = (uint64_t)physbase_addr_hi << 32 | physbase_addr_lo;
	device_printf(dev, "SMU physical base address: 0x%016lx\n", physbase_addr);

	/* Map memory for SMU and its registers. */
	sc->res = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid, RF_ACTIVE);
	if (sc->res == NULL) {
		device_printf(dev, "could not allocate resource\n");
		return (ENXIO);
	}

	sc->bus_tag = rman_get_bustag(sc->res);

	if (bus_space_map(sc->bus_tag, physbase_addr,
	    SMU_MEM_SIZE, 0, &smu) != 0) {
		device_printf(dev, "could not map bus space for SMU\n");
		bus_release_resource(dev, SYS_RES_MEMORY, rid, sc->res);
		return (ENXIO);
	}
	if (bus_space_map(sc->bus_tag, physbase_addr + SMU_REG_OFF,
	    SMU_MEM_SIZE, 0, &reg) != 0) {
		device_printf(dev, "could not map bus space for SMU regs\n");
		bus_release_resource(dev, SYS_RES_MEMORY, rid, sc->res);
		return (ENXIO);
	}

	/* TODO (not working) Read basic SMU info. */
	fw_vers = bus_space_read_4(sc->bus_tag, smu, SMU_FW_VERSION);
	device_printf(dev, "SMU firmware version: 0x%08x\n", fw_vers);

	device_printf(dev, "SMU message reg: %08x\n",
	    bus_space_read_4(sc->bus_tag, reg, SMU_REG_MESSAGE));
	device_printf(dev, "SMU response reg: %08x\n",
	    bus_space_read_4(sc->bus_tag, reg, SMU_REG_RESPONSE));
	device_printf(dev, "SMU argument reg: %08x\n",
	    bus_space_read_4(sc->bus_tag, reg, SMU_REG_ARGUMENT));

	/* See https://lore.kernel.org/all/8ff4fcb8-36c9-f9e4-d05f-730e5379ec9c@redhat.com */
	if (bus_space_read_4(sc->bus_tag, reg, SMU_REG_RESPONSE) == SMU_RES_OK)
		device_printf(dev, "SMU is ready\n");
	else
		device_printf(dev, "SMU is not ready\n");

	return (0);
}

static int
amdpmc_detach(device_t dev)
{
	struct amdpmc_softc *sc = device_get_softc(dev);
	int rid = 0;

	if (sc->res != NULL) {
		bus_release_resource(dev, SYS_RES_MEMORY, rid, sc->res);
		sc->res = NULL;
	}

	return (0);
}

static device_method_t amdpmc_methods[] = {
	DEVMETHOD(device_identify,	amdpmc_identify),
	DEVMETHOD(device_probe,		amdpmc_probe),
	DEVMETHOD(device_attach,	amdpmc_attach),
	DEVMETHOD(device_detach,	amdpmc_detach),
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
