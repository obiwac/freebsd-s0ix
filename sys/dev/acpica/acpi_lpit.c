#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/bus.h>

#include <contrib/dev/acpica/include/acpi.h>
#include <contrib/dev/acpica/include/accommon.h>

#include <dev/acpica/acpivar.h>

static int
acpi_lpit_init(void *data)
{
	struct acpi_softc *sc;
	ACPI_TABLE_LPIT *lpit;
	ACPI_STATUS status;
	ssize_t lpi_state_count;
	ACPI_LPIT_NATIVE *lpi_states;

	sc = devclass_get_softc(devclass_find("acpi"), 0);
	if (sc == NULL)
		return (ENXIO);

	/*
	 * If The system doesn't have low power idle states, we don't want to bother
	 * exposing any of the residency information since BIOS vendors tend to copy
	 * and paste code and we might get non-functional residency registers in the
	 * LPIT. Perhaps in the future a quirk table would be best.
	 */
	if ((sc->acpi_s0ix_level & ACPI_S0IX_LEVEL_FADT21) == 0)
		return (ENODEV);

	status = AcpiGetTable(ACPI_SIG_LPIT, 0, __DECONST(ACPI_TABLE_HEADER**, &lpit));
	if (ACPI_FAILURE(status))
		return (ENXIO);

	lpi_state_count = (lpit->Header.Length - sizeof(*lpit)) / sizeof(*lpi_states);
	if (lpi_state_count <= 0)
		return (ENXIO);

	lpi_states = (ACPI_LPIT_NATIVE *)(lpit + 1);

	for (ssize_t i = 0; i < lpi_state_count; i++) {
		ACPI_LPIT_NATIVE *const lpi_state = &lpi_states[i];
		if (lpi_state->Header.Type != ACPI_LPIT_TYPE_NATIVE_CSTATE) {
			device_printf(sc->acpi_dev, "Unsupported LPI state type %u\n", lpi_state->Header.Type);
			continue;
		}

		printf("TODO Found LPI state UniqueId = %u, Residency = %u, Latency = %u, CounterFrequency = %lu\n", lpi_state->Header.UniqueId, lpi_state->Residency, lpi_state->Latency, lpi_state->CounterFrequency);
	}

	return 0;
}

SYSINIT(acpi_lpit, SI_SUB_INTRINSIC_POST, SI_ORDER_ANY, acpi_lpit_init, NULL);
