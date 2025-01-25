/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2025 The FreeBSD Foundation
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

#include <contrib/dev/acpica/include/acpi.h>
#include <dev/acpica/acpivar.h>

#define SMU_INDEX_ADDRESS	0xB8
#define SMU_INDEX_DATA		0xBC

#define SMU_PHYSBASE_ADDR_LO	0x13B102E8
#define SMU_PHYSBASE_ADDR_HI	0x13B102EC

#define SMU_MEM_SIZE		0x1000
#define SMU_REG_SPACE_OFF	0x10000

#define SMU_REG_MESSAGE		0x538
#define SMU_REG_RESPONSE	0x980
#define SMU_REG_ARGUMENT	0x9BC

enum amdsmu_res {
	SMU_RES_WAIT		= 0x00,
	SMU_RES_OK		= 0x01,
	SMU_RES_REJECT_BUSY	= 0xFC,
	SMU_RES_REJECT_PREREQ	= 0xFD,
	SMU_RES_UNKNOWN		= 0xFE,
	SMU_RES_FAILED		= 0xFF,
};

#define SMU_RES_READ_PERIOD_US	50
#define SMU_RES_READ_MAX	20000

enum amdsmu_msg {
	SMU_MSG_GETSMUVERSION		= 0x02,
	SMU_MSG_LOG_GETDRAM_ADDR_HI	= 0x04,
	SMU_MSG_LOG_GETDRAM_ADDR_LO	= 0x05,
	SMU_MSG_LOG_START		= 0x06,
	SMU_MSG_LOG_RESET		= 0x07,
	SMU_MSG_LOG_DUMP_DATA		= 0x08,
	SMU_MSG_GET_SUP_CONSTRAINTS	= 0x09,
};

/*
 * TODO These are in common with amdtemp; should we find a way to factor these
 * out?  Also, there are way more of these.  I couldn't find a centralized place
 * which lists them though.
 */
#define VENDORID_AMD		0x1022
#define CPUID_AMD_REMBRANDT	0x14B5
#define CPUID_AMD_PHOENIX	0x14E8
#define CPUID_AMD_STRIX_POINT	0x14A4

static const struct amdsmu_product {
	uint16_t	amdsmu_vendorid;
	uint16_t	amdsmu_deviceid;
} amdsmu_products[] = {
	{ VENDORID_AMD,	CPUID_AMD_REMBRANDT },
	{ VENDORID_AMD,	CPUID_AMD_PHOENIX },
	{ VENDORID_AMD,	CPUID_AMD_STRIX_POINT },
};

struct amdsmu_softc {
	struct resource		*res;
	bus_space_tag_t 	bus_tag;

	bus_space_handle_t	smu_space;
	bus_space_handle_t	reg_space;
};

static bool
amdsmu_match(device_t dev, const struct amdsmu_product **product_out)
{
	const uint16_t vendorid = pci_get_vendor(dev);
	const uint16_t deviceid = pci_get_device(dev);

	for (size_t i = 0; i < nitems(amdsmu_products); i++) {
		const struct amdsmu_product *prod = &amdsmu_products[i];

		if (vendorid == prod->amdsmu_vendorid &&
		    deviceid == prod->amdsmu_deviceid) {
			if (product_out != NULL)
				*product_out = prod;
			return (true);
		}
	}
	return (false);
}

static void
amdsmu_identify(driver_t *driver, device_t parent)
{

	if (device_find_child(parent, "amdsmu", -1) != NULL)
		return;

	if (amdsmu_match(parent, NULL)) {
		if (device_add_child(parent, "amdsmu", -1) == NULL)
			device_printf(parent, "add amdsmu child failed\n");
	}
}

static int
amdsmu_probe(device_t dev)
{

	if (resource_disabled("amdsmu", 0))
		return (ENXIO);
	if (!amdsmu_match(device_get_parent(dev), NULL))
		return (ENXIO);
	return (BUS_PROBE_GENERIC);
}

static enum amdsmu_res
amdsmu_wait_res(device_t dev)
{
	struct amdsmu_softc	*sc = device_get_softc(dev);
	enum amdsmu_res	res;

	/* TODO Remove comment?
	 * To know whether the SMU is ready to accept commands, we must wait
	 * for the response register to contain "1" (SMU_RES_OK).
	 * See https://lore.kernel.org/all/8ff4fcb8-36c9-f9e4-d05f-730e5379ec9c@redhat.com
	 */

	for (size_t i = 0; i < SMU_RES_READ_MAX; i++) {
		res = bus_space_read_4(sc->bus_tag, sc->reg_space,
		    SMU_REG_RESPONSE);
		if (res != SMU_RES_WAIT)
			return (res);
		pause_sbt("amdsmu", ustosbt(SMU_RES_READ_PERIOD_US), 0,
		    C_HARDCLOCK);
	}
	device_printf(dev, "timed out waiting for response from SMU\n");
	return SMU_RES_WAIT;
}

static int
amdsmu_cmd(device_t dev, uint32_t msg, uint32_t arg, uint32_t *ret)
{
	struct amdsmu_softc	*sc = device_get_softc(dev);
	enum amdsmu_res	res;

	/* Wait for SMU to be ready. */
	if (amdsmu_wait_res(dev) == SMU_RES_WAIT)
		return (ETIMEDOUT);

	/* Write out command to registers. */
	bus_space_write_4(sc->bus_tag, sc->reg_space, SMU_REG_RESPONSE,
	    SMU_RES_WAIT);
	bus_space_write_4(sc->bus_tag, sc->reg_space, SMU_REG_MESSAGE, msg);
	bus_space_write_4(sc->bus_tag, sc->reg_space, SMU_REG_ARGUMENT, arg);

	res = amdsmu_wait_res(dev);

	switch (res) {
	case SMU_RES_WAIT:
		return (ETIMEDOUT);
	case SMU_RES_OK:
		if (ret != NULL)
			*ret = bus_space_read_4(sc->bus_tag, sc->reg_space,
			    SMU_REG_ARGUMENT);
		return (0);
	case SMU_RES_REJECT_BUSY:
		device_printf(dev, "SMU is busy\n");
		return (EBUSY);
	case SMU_RES_REJECT_PREREQ:
	case SMU_RES_UNKNOWN:
	case SMU_RES_FAILED:
		device_printf(dev, "SMU error: %02x\n", res);
	}

	return (EINVAL);
}

static void
amdsmu_print_vers(device_t dev)
{
	uint32_t	fw_vers;
	uint8_t		smu_program;
	uint8_t		smu_maj, smu_min, smu_rev;

	if (amdsmu_cmd(dev, SMU_MSG_GETSMUVERSION, 0, &fw_vers) != 0) {
		device_printf(dev, "failed to get SMU version\n");
		return;
	}
	smu_program = (fw_vers >> 24) & 0xFF;
	smu_maj = (fw_vers >> 16) & 0xFF;
	smu_min = (fw_vers >> 8) & 0xFF;
	smu_rev = fw_vers & 0xFF;
	device_printf(dev, "SMU version: %d.%d.%d (program %d)\n",
	    smu_maj, smu_min, smu_rev, smu_program);
}

static int
amdsmu_attach(device_t dev)
{
	struct amdsmu_softc *sc = device_get_softc(dev);
	uint32_t physbase_addr_lo, physbase_addr_hi;
	uint64_t physbase_addr;
	int rid = 0;
	uint32_t log_addr_lo, log_addr_hi;

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

	/* Map memory for SMU and its registers. */
	sc->res = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid, RF_ACTIVE);
	if (sc->res == NULL) {
		device_printf(dev, "could not allocate resource\n");
		return (ENXIO);
	}

	sc->bus_tag = rman_get_bustag(sc->res);

	if (bus_space_map(sc->bus_tag, physbase_addr,
	    SMU_MEM_SIZE, 0, &sc->smu_space) != 0) {
		device_printf(dev, "could not map bus space for SMU\n");
		bus_release_resource(dev, SYS_RES_MEMORY, rid, sc->res);
		return (ENXIO);
	}
	if (bus_space_map(sc->bus_tag, physbase_addr + SMU_REG_SPACE_OFF,
	    SMU_MEM_SIZE, 0, &sc->reg_space) != 0) {
		device_printf(dev, "could not map bus space for SMU regs\n");
		bus_release_resource(dev, SYS_RES_MEMORY, rid, sc->res);
		return (ENXIO);
	}

	amdsmu_print_vers(dev);

	/* Setup SMU logging. */
	amdsmu_cmd(dev, SMU_MSG_LOG_GETDRAM_ADDR_LO, 0, &log_addr_lo);
	amdsmu_cmd(dev, SMU_MSG_LOG_GETDRAM_ADDR_HI, 0, &log_addr_hi);

	amdsmu_cmd(dev, SMU_MSG_LOG_RESET, 0, NULL);
	amdsmu_cmd(dev, SMU_MSG_LOG_START, 0, NULL);

	printf("SMU log addr: %08x - %08x\n", log_addr_lo, log_addr_hi);

	// TODO acpi_amdsmu_enter/exit hooks.
	// These can then either be called in acpi_spmc or in ACPI itself, I don't know yet (probably ACPI itself).

	struct acpi_softc *acpi_sc = acpi_device_get_parent_softc(dev);
	printf("acpi_sc: %p\n", acpi_sc);

	return (0);
}

static int
amdsmu_detach(device_t dev)
{
	struct amdsmu_softc *sc = device_get_softc(dev);
	int rid = 0;

	if (sc->res != NULL) {
		bus_release_resource(dev, SYS_RES_MEMORY, rid, sc->res);
		sc->res = NULL;
	}

	return (0);
}

static device_method_t amdsmu_methods[] = {
	DEVMETHOD(device_identify,	amdsmu_identify),
	DEVMETHOD(device_probe,		amdsmu_probe),
	DEVMETHOD(device_attach,	amdsmu_attach),
	DEVMETHOD(device_detach,	amdsmu_detach),
	DEVMETHOD_END
};

static driver_t amdsmu_driver = {
	"amdsmu",
	amdsmu_methods,
	sizeof(struct amdsmu_softc),
};

DRIVER_MODULE(amdsmu, hostb, amdsmu_driver, NULL, NULL);
MODULE_VERSION(amdsmu, 1);
MODULE_DEPEND(amdsmu, amdsmn, 1, 1, 1);
MODULE_PNP_INFO("U16:vendor;U16:device", pci, amdsmu, amdsmu_products,
    nitems(amdsmu_products));
