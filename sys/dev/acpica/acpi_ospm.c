#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/bus.h>
#include <sys/malloc.h>

#include <contrib/dev/acpica/include/acpi.h>
#include <contrib/dev/acpica/include/accommon.h>

#include <dev/acpica/acpivar.h>

// TODO Maybe OSPM is a bit too generic a name for this. Should we call this lps0 or s0ix? In any case I'm not sure what Ben was referring to with SPMC, so I'd rather not we continue using that name.

ACPI_MODULE_NAME("OSPM");

enum {
	LPS0_ENUM_FUNCTIONS		= 0,
	LPS0_DEVICE_CONSTRAINTS		= 1,
	LPS0_GET_CRASH_DUMP_DEVICE	= 2,
	LPS0_DISPLAY_OFF_NOTIFICATION	= 3,
	LPS0_DISPLAY_ON_NOTIFICATION	= 4,
	LPS0_ENTRY_NOTIFICATION		= 5,
	LPS0_EXIT_NOTIFICATION		= 6,
};

static char *ospm_ids[] = {
	"PNP0D80",
	NULL
};

static uint8_t dsm_uuid[16] = { /* c4eb40a0-6cd2-11e2-bcfd-0800200c9a66 */
	0xa0, 0x40, 0xeb, 0xc4, 0xd2, 0x6c, 0xe2, 0x11,
	0xbc, 0xfd, 0x08, 0x00, 0x20, 0x0c, 0x9a, 0x66};

/* Published specs only have rev 0, but Linux uses rev 1. */
#define OSPM_REV	1

struct acpi_ospm_private {
	uint8_t dsm_bits;
};

struct acpi_ospm_softc {
	device_t 	dev;
	ACPI_HANDLE 	handle;
	ACPI_OBJECT	*obj;
};

static struct sysctl_ctx_list acpi_ospm_sysctl_ctx;
static struct sysctl_oid *ospm_sysctl_tree;

static int
acpi_ospm_probe(device_t dev)
{
	char *name;
	uint8_t dsm_bits;
	ACPI_HANDLE handle;
	struct acpi_ospm_private *private;

	/* Check that this is an enabled device. */
	if (acpi_get_type(dev) != ACPI_TYPE_DEVICE || acpi_disabled("ospm"))
		return (ENXIO);

	if (ACPI_ID_PROBE(device_get_parent(dev), dev, ospm_ids, &name) > 0)
		return (ENXIO);

	handle = acpi_get_handle(dev);
	if (handle == NULL)
		return (ENXIO);

	/* First bit tells us if any functions are supported at all. */
	dsm_bits = acpi_DSMQuery(handle, dsm_uuid, OSPM_REV);
	if ((dsm_bits & 1) == 0)
		return (ENXIO);

	private = malloc(sizeof *private, M_TEMP, M_WAITOK | M_ZERO);
	private->dsm_bits = dsm_bits;
	acpi_set_private(dev, private);

	device_set_descf(dev, "OS Power Management (DSM 0x%02x)", dsm_bits);

	return (BUS_PROBE_DEFAULT);
}

static int
acpi_ospm_attach(device_t dev)
{
	struct acpi_ospm_private *private;
	struct acpi_ospm_softc *sc;
	struct acpi_softc *acpi_sc;
	uint8_t dsm_bits;

	private = acpi_get_private(dev);
	dsm_bits = private->dsm_bits;
	free(private, M_TEMP);

	sc = device_get_softc(dev);
	sc->dev = dev;

	sc->handle = acpi_get_handle(dev);
	if (sc->handle == NULL)
		return (ENXIO);

	acpi_sc = acpi_device_get_parent_softc(sc->dev);

	(void) dsm_bits;

	ospm_sysctl_tree = SYSCTL_ADD_NODE(&acpi_ospm_sysctl_ctx,
	    SYSCTL_CHILDREN(acpi_sc->acpi_sysctl_tree),
	    OID_AUTO, "ospm", CTLFLAG_RD, NULL, "OS Power Management");

	return (0);
}

__attribute__((unused))
static int
acpi_ospm_post_suspend(device_t dev)
{
	printf("TODO %s\n", __func__);
	return (0);
}

__attribute__((unused))
static int
acpi_ospm_post_resume(device_t dev)
{
	printf("TODO %s\n", __func__);
	return (0);
}

static device_method_t acpi_ospm_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,		acpi_ospm_probe),
	DEVMETHOD(device_attach,	acpi_ospm_attach),

	// TODO device_post_suspend & device_post_resume (don't forget to remove the __attribute__((unused))'s once this is done).
	// DEVMETHOD(device_post_suspend,	acpi_ospm_post_suspend),
	// DEVMETHOD(device_post_resume,	acpi_ospm_post_resume),

	DEVMETHOD_END
};

static driver_t acpi_ospm_driver = {
	"acpi_ospm",
	acpi_ospm_methods,
	sizeof(struct acpi_ospm_softc),
};

DRIVER_MODULE_ORDERED(acpi_ospm, acpi, acpi_ospm_driver, NULL, NULL, SI_ORDER_ANY);
MODULE_DEPEND(acpi_ospm, acpi, 1, 1, 1);
