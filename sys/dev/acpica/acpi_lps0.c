#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/bus.h>
#include <sys/malloc.h>
#include <sys/uuid.h>
#include <sys/kdb.h>

#include <contrib/dev/acpica/include/acpi.h>
#include <contrib/dev/acpica/include/accommon.h>

#include <dev/acpica/acpivar.h>

ACPI_SERIAL_DECL(lps0, "Low Power S0 Idle");

/*
 * TODO How many AMD devices still have BIOS' without the DSM's for modern standby?
 * If it's not too much, it might be worth just focusing on the modern standby DSM's.
 */

static char *lps0_ids[] = {
	"PNP0D80",
	NULL
};

static struct uuid intel_dsm_uuid = { /* c4eb40a0-6cd2-11e2-bcfd-0800200c9a66 */
	0xc4eb40a0, 0x6cd2, 0x11e2, 0xbc, 0xfd,
	{0x08, 0x00, 0x20, 0x0c, 0x9a, 0x66},
};

static struct uuid ms_dsm_uuid = { /* 11e00d56-ce64-47ce-837b-1f898f9aa461 */
	0x11e00d56, 0xce64, 0x47ce, 0x83, 0x7b,
	{0x1f, 0x89, 0x8f, 0x9a, 0xa4, 0x61},
};

static struct uuid amd_dsm_uuid = { /* e3f32452-febc-43ce-9039-932122d37721 */
	0xe3f32452, 0xfebc, 0x43ce, 0x90, 0x39,
	{0x93, 0x21, 0x22, 0xd3, 0x77, 0x21},
};

enum dsm_set {
	DSM_SET_INTEL		= 0b001,
	DSM_SET_MS		= 0b010,
	DSM_SET_AMD		= 0b100,
};

enum intel_dsm_index {
	DSM_INDEX_ENUM_FUNCTIONS 		= 0,
	DSM_INDEX_GET_DEVICE_CONSTRAINTS	= 1,
	DSM_INDEX_GET_CRASH_DUMP_DEVICE		= 2,
	DSM_INDEX_DISPLAY_OFF_NOTIFICATION	= 3,
	DSM_INDEX_DISPLAY_ON_NOTIFICATION	= 4,
	DSM_INDEX_ENTRY_NOTIFICATION		= 5,
	DSM_INDEX_EXIT_NOTIFICATION		= 6,
	DSM_INDEX_MODERN_ENTRY_NOTIFICATION	= 5,
	DSM_INDEX_MODERN_EXIT_NOTIFICATION	= 6,
};

enum amd_dsm_index {
	AMD_DSM_INDEX_ENUM_FUNCTIONS		= 0,
	AMD_DSM_INDEX_GET_DEVICE_CONSTRAINTS	= 1,
	AMD_DSM_INDEX_ENTRY_NOTIFICATION	= 2,
	AMD_DSM_INDEX_EXIT_NOTIFICATION		= 3,
	AMD_DSM_INDEX_DISPLAY_OFF_NOTIFICATION	= 4,
	AMD_DSM_INDEX_DISPLAY_ON_NOTIFICATION	= 5,
};

union dsm_index {
	int			i;
	enum intel_dsm_index	regular;
	enum amd_dsm_index	amd;
};

enum lps0_sysctl {
	LPS0_SYSCTL_DISPLAY_ON,
};

/* Published specs only have rev 0, but Linux uses rev 1. */
#define LPS0_REV	1

struct acpi_lps0_private {
	enum dsm_set	dsm_sets;
};

struct acpi_lps0_softc {
	device_t 	dev;
	ACPI_HANDLE 	handle;
	ACPI_OBJECT	*obj;
	enum dsm_set	dsm_sets;
	struct uuid	*dsm_uuid;

	/* sysctl stuff. */

	struct sysctl_ctx_list	sysctl_ctx;
	struct sysctl_oid	*sysctl_tree;
};

static int acpi_lps0_sysctl_handler(SYSCTL_HANDLER_ARGS);

static int
acpi_lps0_probe(device_t dev)
{
	char *name;
	ACPI_HANDLE handle;
	struct acpi_lps0_private *private;

	/* Check that this is an enabled device. */
	if (acpi_get_type(dev) != ACPI_TYPE_DEVICE || acpi_disabled("lps0"))
		return (ENXIO);

	if (ACPI_ID_PROBE(device_get_parent(dev), dev, lps0_ids, &name) > 0)
		return (ENXIO);

	handle = acpi_get_handle(dev);
	if (handle == NULL)
		return (ENXIO);

	/* Check which sets of DSM's are supported. */
	enum dsm_set dsm_sets = 0;

	uint64_t const dsm_bits = acpi_DSMQuery(handle, (uint8_t *)&intel_dsm_uuid,
	    LPS0_REV);
	uint64_t const ms_dsm_bits = acpi_DSMQuery(handle, (uint8_t *)&ms_dsm_uuid,
	    LPS0_REV);
	uint64_t const amd_dsm_bits = acpi_DSMQuery(handle, (uint8_t *)&amd_dsm_uuid,
	    LPS0_REV);

	if ((dsm_bits & 1) != 0)
		dsm_sets |= DSM_SET_INTEL;
	if ((ms_dsm_bits & 1) != 0)
		dsm_sets |= DSM_SET_MS;
	if ((amd_dsm_bits & 1) != 0)
		dsm_sets |= DSM_SET_AMD;

	if (dsm_sets == 0)
		return (ENXIO);

	private = malloc(sizeof *private, M_TEMP, M_WAITOK | M_ZERO);
	private->dsm_sets = dsm_sets;
	acpi_set_private(dev, private);

	device_set_descf(dev, "Low Power S0 Idle (DSM sets 0x%x)", dsm_sets);

	return (BUS_PROBE_DEFAULT);
}

static int
acpi_lps0_attach(device_t dev)
{
	struct acpi_lps0_private *private;
	struct acpi_lps0_softc *sc;
	struct acpi_softc *acpi_sc;

	sc = device_get_softc(dev);
	sc->dev = dev;

	private = acpi_get_private(dev);
	sc->dsm_sets = private->dsm_sets;
	free(private, M_TEMP);

	/* Prefer original Intel DSM spec, then Microsoft, then, finally AMD. */
	if ((sc->dsm_sets & DSM_SET_INTEL) != 0)
		sc->dsm_uuid = &intel_dsm_uuid;
	else if ((sc->dsm_sets & DSM_SET_MS) != 0)
		sc->dsm_uuid = &ms_dsm_uuid;
	else if ((sc->dsm_sets & DSM_SET_AMD) != 0)
		sc->dsm_uuid = &amd_dsm_uuid;

	sc->handle = acpi_get_handle(dev);
	if (sc->handle == NULL)
		return (ENXIO);

	acpi_sc = acpi_device_get_parent_softc(sc->dev);

	/* Build sysctl tree. */

	sysctl_ctx_init(&sc->sysctl_ctx);
	sc->sysctl_tree = SYSCTL_ADD_NODE(&sc->sysctl_ctx,
	    SYSCTL_CHILDREN(acpi_sc->acpi_sysctl_tree),
	    OID_AUTO, "lps0", CTLFLAG_RD, NULL, "Low Power S0 Idle");

	SYSCTL_ADD_PROC(&sc->sysctl_ctx, SYSCTL_CHILDREN(sc->sysctl_tree),
	    OID_AUTO, "display_on", CTLTYPE_INT | CTLFLAG_WR | CTLFLAG_ANYBODY,
	    sc, LPS0_SYSCTL_DISPLAY_ON, acpi_lps0_sysctl_handler, "I",
	    "Turn the display on or off");

	return (0);
}

static int
acpi_lps0_detach(device_t dev)
{
	struct acpi_lps0_softc *sc;

	sc = device_get_softc(dev);
	sysctl_ctx_free(&sc->sysctl_ctx);

	return (0);
}

static int
parse_constraints_spec(ACPI_OBJECT *object)
{

	return (-1); /* TODO Haven't tested this on Intel, but the parsing is to-spec I believe. */

	for (size_t i = 0; i < object->Package.Count; i++) {
		printf("Device constraint %zu\n", i);

		ACPI_OBJECT *const constraint = &object->Package.Elements[i];
		printf("%d\n", constraint->Package.Count);

		ACPI_OBJECT *const device_name_obj = &constraint->Package.Elements[0];
		char const *const device_name = device_name_obj->String.Pointer;
		size_t const device_name_len = device_name_obj->String.Length;

		printf("Device name: %.*s\n", (int)device_name_len, device_name);

		uint32_t const device_enabled = constraint->Package.Elements[1].Integer.Value;
		printf("Device enabled: %u\n", device_enabled);

		ACPI_OBJECT *const device_constraint_detail = &constraint->Package.Elements[2];
		/* The first element in the device constraint detail package is the revision, always zero. */
		ACPI_OBJECT *const constraint_package = &device_constraint_detail->Package.Elements[1];

		uint32_t const lpi_uid = constraint_package->Package.Elements[0].Integer.Value;
		uint32_t const min_d_state = constraint_package->Package.Elements[1].Integer.Value;
		uint32_t const min_dev_specific_state = constraint_package->Package.Elements[2].Integer.Value;

		printf("LPI UID: %u\n", lpi_uid);
		printf("Min D state precondition: %u\n", min_d_state);
		printf("Min dev specific state precondition: %u\n", min_dev_specific_state);
	}

	return (0);
}

static int
parse_constraints_amd(struct acpi_lps0_softc *sc, ACPI_OBJECT *object)
{
	ACPI_STATUS status;

	/* First element in the package is unknown. */
	/* Second element is the number of device constraints. */
	/* Third element is the list of device constraints itself. */

	size_t const device_constraint_count = object->Package.Elements[1].Integer.Value;
	ACPI_OBJECT* const device_constraints = &object->Package.Elements[2];
	char msg[256];
	sprintf(msg, "device_contraints address: %p %p\n", &device_constraints->Package.Count, &device_constraint_count);
	// kdb_enter(msg, msg);

	(void) device_constraint_count;
	KASSERT(device_constraints->Package.Count == device_constraint_count, "Device constraint count mismatch");

	/* Should be able to use device_constraints->Package.Count here. */
	for (size_t i = 0; i < device_constraint_count; i++) {
		/* Parse the constraint package. */

		ACPI_OBJECT* const constraint = &device_constraints->Package.Elements[i];
		KASSERT(constraint->Package.Count == 4, "Device constraint package expected 4 elements");

		uint64_t const enabled = constraint->Package.Elements[0].Integer.Value;
		printf("Enabled: %lu\n", enabled);

		ACPI_OBJECT *const device_name_obj = &constraint->Package.Elements[1];
		char const *const device_name = device_name_obj->String.Pointer;
		size_t const device_name_len = device_name_obj->String.Length;

		printf("Device name: %.*s\n", (int)device_name_len, device_name);

		uint64_t const function_states = constraint->Package.Elements[2].Integer.Value;
		printf("Function states: %lu\n", function_states);

		uint64_t const min_d_state = constraint->Package.Elements[3].Integer.Value;
		printf("Min D state: %lu\n", min_d_state);

		/* Get the handle. */

		ACPI_HANDLE handle;
		status = acpi_GetHandleInScope(sc->handle, __DECONST(char *, device_name), &handle);

		if (ACPI_FAILURE(status))
			continue;

		int d_state;
		if (ACPI_FAILURE(acpi_pwr_get_consumer(handle, &d_state)))
			continue;

		printf("Current D state: %d - requirements %s\n", d_state, d_state >= min_d_state ? "met" : "unmet");
	}

	return (0);
}

static int
acpi_lps0_get_device_constraints(device_t dev)
{
	struct acpi_lps0_softc *sc;
	union dsm_index dsm_index;
	ACPI_STATUS status;
	ACPI_BUFFER result;
	ACPI_OBJECT *object;
	bool is_amd;

	sc = device_get_softc(dev);
	is_amd = (sc->dsm_sets & DSM_SET_AMD) != 0; /* XXX Assumes anything else (only Intel and MS right now) is to spec. */

	if (is_amd)
		dsm_index.amd = AMD_DSM_INDEX_GET_DEVICE_CONSTRAINTS;
	else
		dsm_index.regular = DSM_INDEX_GET_DEVICE_CONSTRAINTS;

	status = acpi_EvaluateDSMTyped(sc->handle, (uint8_t *)sc->dsm_uuid,
	    LPS0_REV, dsm_index.i, NULL, &result, ACPI_TYPE_PACKAGE);

	if (ACPI_FAILURE(status) || result.Pointer == NULL) {
		device_printf(dev, "failed to call DSM %d (%s)\n", dsm_index.i,
		    __func__);
		return (ENXIO);
	}

	object = (ACPI_OBJECT *)result.Pointer;

	if (is_amd)
		parse_constraints_amd(sc, object);
	else
		parse_constraints_spec(object);

	AcpiOsFree(object);

	return (0);
}

#include <sys/kernel.h>

static int
acpi_lps0_display_off(device_t dev)
{
	struct acpi_lps0_softc *sc;
	union dsm_index dsm_index;
	ACPI_STATUS status;
	ACPI_BUFFER result;

	sc = device_get_softc(dev);

	// acpi_lps0_get_device_constraints(dev);

	if ((sc->dsm_sets & DSM_SET_INTEL) != 0 || (sc->dsm_sets & DSM_SET_MS) != 0)
		dsm_index.regular = DSM_INDEX_DISPLAY_OFF_NOTIFICATION;
	else if ((sc->dsm_sets & DSM_SET_AMD) != 0)
		dsm_index.amd = AMD_DSM_INDEX_DISPLAY_OFF_NOTIFICATION;

	ACPI_HANDLE handle;
	printf("gethandleinscope %d\n", acpi_GetHandleInScope(sc->handle, "\\_SB.PCI0.GP17.VGA", &handle));
	printf("switch %d\n", acpi_pwr_switch_consumer(handle, ACPI_STATE_D3));

	status = acpi_EvaluateDSMTyped(sc->handle, (uint8_t *)sc->dsm_uuid,
	    LPS0_REV, dsm_index.i, NULL, &result, ACPI_TYPE_ANY);

	/* XXX Spec says this should return nothing, but Linux checks for this return value. */
	if (ACPI_FAILURE(status) || result.Pointer == NULL) {
		device_printf(dev, "failed to call DSM %d (%s)\n", dsm_index.i,
		    __func__);
		return (ENXIO);
	}

	device_printf(dev, "called DSM %d (%s) -> %p\n", dsm_index.i, __func__, result.Pointer);
	AcpiOsFree(result.Pointer);

	return (0);
}

static int
acpi_lps0_display_on(device_t dev)
{
	struct acpi_lps0_softc *sc;
	union dsm_index dsm_index;
	ACPI_STATUS status;
	ACPI_BUFFER result;
	ACPI_OBJECT* object;

	sc = device_get_softc(dev);

	if ((sc->dsm_sets & DSM_SET_INTEL) != 0 || (sc->dsm_sets & DSM_SET_MS) != 0)
		dsm_index.regular = DSM_INDEX_DISPLAY_ON_NOTIFICATION;
	else if ((sc->dsm_sets & DSM_SET_AMD) != 0)
		dsm_index.amd = AMD_DSM_INDEX_DISPLAY_ON_NOTIFICATION;

	status = acpi_EvaluateDSMTyped(sc->handle, (uint8_t *)sc->dsm_uuid,
	    LPS0_REV, dsm_index.i, NULL, &result, ACPI_TYPE_ANY);

	if (ACPI_FAILURE(status) || result.Pointer == NULL) {
		device_printf(dev, "failed to call DSM %d (%s)\n", dsm_index.i,
		    __func__);
		return (ENXIO);
	}

	object = (ACPI_OBJECT *)result.Pointer;
	device_printf(dev, "called DSM %d (%s) -> %lu\n", dsm_index.i, __func__, object->Integer.Value);
	AcpiOsFree(object);

	return (0);
}

__attribute__((unused))
static int
acpi_lps0_post_suspend(device_t dev)
{

	printf("TODO %s\n", __func__);
	return (0);
}

__attribute__((unused))
static int
acpi_lps0_post_resume(device_t dev)
{

	printf("TODO %s\n", __func__);
	return (0);
}

static int
acpi_lps0_sysctl_handler(SYSCTL_HANDLER_ARGS)
{
	struct acpi_lps0_softc	*sc = oidp->oid_arg1;
	int			sysctl = oidp->oid_arg2;
	int			rv = -1;
	int			display_on;

	ACPI_SERIAL_BEGIN(lps0);

	switch (sysctl) {
	case LPS0_SYSCTL_DISPLAY_ON:
		rv = sysctl_handle_int(oidp, &display_on, 0, req);
		if (rv != 0 || req->newptr == NULL)
			break;

		if (display_on) {
			rv = acpi_lps0_display_on(sc->dev);
			break;
		}

		rv = acpi_lps0_display_off(sc->dev);
		break;
	}

	ACPI_SERIAL_END(lps0);
	return (rv);
}

static device_method_t acpi_lps0_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,		acpi_lps0_probe),
	DEVMETHOD(device_attach,	acpi_lps0_attach),
	DEVMETHOD(device_detach,	acpi_lps0_detach),

	// TODO device_post_suspend & device_post_resume (don't forget to remove the __attribute__((unused))'s once this is done).
	// DEVMETHOD(device_post_suspend,	acpi_lps0_post_suspend),
	// DEVMETHOD(device_post_resume,	acpi_lps0_post_resume),

	DEVMETHOD_END
};

static driver_t acpi_lps0_driver = {
	"acpi_lps0",
	acpi_lps0_methods,
	sizeof(struct acpi_lps0_softc),
};

DRIVER_MODULE_ORDERED(acpi_lps0, acpi, acpi_lps0_driver, NULL, NULL, SI_ORDER_ANY);
MODULE_DEPEND(acpi_lps0, acpi, 1, 1, 1);
