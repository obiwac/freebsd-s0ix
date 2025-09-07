/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2018 Advanced Micro Devices
 * All rights reserved.
 * Copyright (c) 2025 The FreeBSD Foundation
 *
 * Portions of this software were developed by Aymeric Wibo
 * <obiwac@freebsd.org> under sponsorship from the FreeBSD Foundation.
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

#include <sys/cdefs.h>
#include "opt_acpi.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/gpio.h>
#include <sys/interrupt.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/rman.h>
#include <sys/sysctl.h>

#include <machine/bus.h>
#include <machine/resource.h>

#include <contrib/dev/acpica/include/acpi.h>
#include <contrib/dev/acpica/include/accommon.h>

#include <dev/acpica/acpivar.h>
#include <dev/gpio/gpiobusvar.h>

#include "amdgpio.h"

static struct resource_spec amdgpio_spec[] = {
	{ SYS_RES_MEMORY,	0,	RF_ACTIVE },
	{ SYS_RES_IRQ,		0,	RF_ACTIVE | RF_SHAREABLE },
	{ -1, 0, 0 }
};

static inline uint32_t
amdgpio_read_4(struct amdgpio_softc *sc, bus_size_t off)
{
	return (bus_read_4(sc->sc_res[0], off));
}

static inline void
amdgpio_write_4(struct amdgpio_softc *sc, bus_size_t off,
		uint32_t val)
{
	bus_write_4(sc->sc_res[0], off, val);
}

static bool
amdgpio_is_pin_output(struct amdgpio_softc *sc, uint32_t pin)
{
	uint32_t reg, val;
	bool ret;

	/* Get the current pin state */
	AMDGPIO_LOCK(sc);

	reg = AMDGPIO_PIN_REGISTER(pin);
	val = amdgpio_read_4(sc, reg);

	if (val & BIT(OUTPUT_ENABLE_OFF))
		ret = true;
	else
		ret = false;

	AMDGPIO_UNLOCK(sc);

	return (ret);
}

static device_t
amdgpio_get_bus(device_t dev)
{
	struct amdgpio_softc *sc;

	sc = device_get_softc(dev);

	dprintf("busdev %p\n", sc->sc_busdev);
	return (sc->sc_busdev);
}

static int
amdgpio_pin_max(device_t dev, int *maxpin)
{
	struct amdgpio_softc *sc;

	sc = device_get_softc(dev);

	*maxpin = sc->sc_npins - 1;
	dprintf("npins %d maxpin %d\n", sc->sc_npins, *maxpin);

	return (0);
}

static bool
amdgpio_valid_pin(struct amdgpio_softc *sc, int pin)
{
	dprintf("pin %d\n", pin);
	if (sc->sc_res[0] == NULL)
		return (false);

	if ((sc->sc_gpio_pins[pin].gp_pin == pin) &&
		(sc->sc_gpio_pins[pin].gp_caps != 0))
		return (true);

	return (false);
}

static int
amdgpio_pin_getname(device_t dev, uint32_t pin, char *name)
{
	struct amdgpio_softc *sc;

	dprintf("pin %d\n", pin);
	sc = device_get_softc(dev);

	if (!amdgpio_valid_pin(sc, pin))
		return (EINVAL);

	strlcpy(name, sc->sc_gpio_pins[pin].gp_name, GPIOMAXNAME);

	dprintf("pin %d name %s\n", pin, name);

	return (0);
}

static int
amdgpio_pin_getcaps(device_t dev, uint32_t pin, uint32_t *caps)
{
	struct amdgpio_softc *sc;

	sc = device_get_softc(dev);

	dprintf("pin %d\n", pin);
	if (!amdgpio_valid_pin(sc, pin))
		return (EINVAL);

	*caps = sc->sc_gpio_pins[pin].gp_caps;

	dprintf("pin %d caps 0x%x\n", pin, *caps);

	return (0);
}

static int
amdgpio_pin_getflags(device_t dev, uint32_t pin, uint32_t *flags)
{
	struct amdgpio_softc *sc;

	sc = device_get_softc(dev);

	dprintf("pin %d\n", pin);
	if (!amdgpio_valid_pin(sc, pin))
		return (EINVAL);

	AMDGPIO_LOCK(sc);

	*flags = sc->sc_gpio_pins[pin].gp_flags;

	dprintf("pin %d flags 0x%x\n", pin, *flags);

	AMDGPIO_UNLOCK(sc);

	return (0);
}

static int
amdgpio_pin_setflags(device_t dev, uint32_t pin, uint32_t flags)
{
	struct amdgpio_softc *sc;
	uint32_t reg, val;

	sc = device_get_softc(dev);

	dprintf("pin %d flags 0x%x\n", pin, flags);
	if (!amdgpio_valid_pin(sc, pin))
		return (EINVAL);

	if ((flags & ~AMDGPIO_DEFAULT_CAPS) != 0) {
		device_printf(dev, "disallowed flags (0x%x) trying to be set "
		    "(allowed is 0x%x)\n", flags, AMDGPIO_DEFAULT_CAPS);
		return (EINVAL);
	}

	/* Either input or output must be selected. */
	if ((flags & (GPIO_PIN_INPUT | GPIO_PIN_OUTPUT)) == 0)
		return (EINVAL);

	/* Not both directions simultaneously. */
	if ((flags & (GPIO_PIN_INPUT | GPIO_PIN_OUTPUT)) ==
	    (GPIO_PIN_INPUT | GPIO_PIN_OUTPUT))
		return (EINVAL);

	/* Set the GPIO mode and state */
	AMDGPIO_LOCK(sc);

	reg = AMDGPIO_PIN_REGISTER(pin);
	val = amdgpio_read_4(sc, reg);

	if ((flags & GPIO_PIN_INPUT) != 0)
		val &= ~BIT(OUTPUT_ENABLE_OFF);
	else
		val |= BIT(OUTPUT_ENABLE_OFF);

	val &= ~(BIT(PULL_DOWN_ENABLE_OFF) | BIT(PULL_UP_ENABLE_OFF));

	if ((flags & GPIO_PIN_PULLDOWN) != 0)
		val |= BIT(PULL_DOWN_ENABLE_OFF);
	if ((flags & GPIO_PIN_PULLUP) != 0)
		val |= BIT(PULL_UP_ENABLE_OFF);

	amdgpio_write_4(sc, reg, val);

	sc->sc_gpio_pins[pin].gp_flags = flags;
	dprintf("pin %d flags 0x%x val 0x%x gp_flags 0x%x\n",
		pin, flags, val, sc->sc_gpio_pins[pin].gp_flags);

	AMDGPIO_UNLOCK(sc);

	return (0);
}

static int
amdgpio_pin_get(device_t dev, uint32_t pin, unsigned int *value)
{
	struct amdgpio_softc *sc;
	uint32_t reg, val;

	sc = device_get_softc(dev);

	dprintf("pin %d\n", pin);
	if (!amdgpio_valid_pin(sc, pin))
		return (EINVAL);

	*value = 0;

	AMDGPIO_LOCK(sc);

	reg = AMDGPIO_PIN_REGISTER(pin);
	val = amdgpio_read_4(sc, reg);

	if ((sc->sc_gpio_pins[pin].gp_flags & GPIO_PIN_OUTPUT) != 0) {
		if (val & BIT(OUTPUT_VALUE_OFF))
			*value = GPIO_PIN_HIGH;
		else
			*value = GPIO_PIN_LOW;
	} else {
		if (val & BIT(PIN_STS_OFF))
			*value = GPIO_PIN_HIGH;
		else
			*value = GPIO_PIN_LOW;
	}

	dprintf("pin %d value 0x%x\n", pin, *value);

	AMDGPIO_UNLOCK(sc);

	return (0);
}

static int
amdgpio_pin_set(device_t dev, uint32_t pin, unsigned int value)
{
	struct amdgpio_softc *sc;
	uint32_t reg, val;

	sc = device_get_softc(dev);

	dprintf("pin %d value 0x%x\n", pin, value);
	if (!amdgpio_valid_pin(sc, pin))
		return (EINVAL);

	if (!amdgpio_is_pin_output(sc, pin))
		return (EINVAL);

	AMDGPIO_LOCK(sc);

	reg = AMDGPIO_PIN_REGISTER(pin);
	val = amdgpio_read_4(sc, reg);

	if (value == GPIO_PIN_LOW)
		val &= ~BIT(OUTPUT_VALUE_OFF);
	else
		val |= BIT(OUTPUT_VALUE_OFF);

	amdgpio_write_4(sc, reg, val);

	dprintf("pin %d value 0x%x val 0x%x\n", pin, value, val);

	AMDGPIO_UNLOCK(sc);

	return (0);
}

static int
amdgpio_pin_toggle(device_t dev, uint32_t pin)
{
	struct amdgpio_softc *sc;
	uint32_t reg, val;

	sc = device_get_softc(dev);

	dprintf("pin %d\n", pin);
	if (!amdgpio_valid_pin(sc, pin))
		return (EINVAL);

	if (!amdgpio_is_pin_output(sc, pin))
		return (EINVAL);

	/* Toggle the pin */
	AMDGPIO_LOCK(sc);

	reg = AMDGPIO_PIN_REGISTER(pin);
	val = amdgpio_read_4(sc, reg);
	dprintf("pin %d value before 0x%x\n", pin, val);
	val = val ^ BIT(OUTPUT_VALUE_OFF);
	dprintf("pin %d value after 0x%x\n", pin, val);
	amdgpio_write_4(sc, reg, val);

	AMDGPIO_UNLOCK(sc);

	return (0);
}

static bool
amdgpio_valid_intr_pin(struct amdgpio_softc *sc, int pin)
{
	if (!amdgpio_valid_pin(sc, pin))
		return (false);
	if ((sc->sc_gpio_pins[pin].gp_caps & GPIO_INTR_MASK) == GPIO_INTR_NONE)
		return (false);
	return (true);
}

static void
amdgpio_pin_config_intr(device_t dev, uint32_t pin, uint32_t intr_mode)
{
	struct amdgpio_softc *sc;
	uint32_t reg, val;
	int trig, act;

	sc = device_get_softc(dev);

	dprintf("pin %d mode 0x%x\n", pin, mode);
	KASSERT(amdgpio_valid_intr_pin(sc, pin), ("invalid pin"));

	/* XXX Linux also sets various debounce modes. */
	switch (intr_mode) {
	case GPIO_INTR_EDGE_FALLING:
		trig = EDGE_TRIGGER;
		act = ACTIVE_LOW;
		break;
	case GPIO_INTR_EDGE_RISING:
		trig = EDGE_TRIGGER;
		act = ACTIVE_HIGH;
		break;
	case GPIO_INTR_EDGE_BOTH:
		trig = EDGE_TRIGGER;
		act = BOTH_EDGE;
		break;
	case GPIO_INTR_LEVEL_LOW:
		trig = LEVEL_TRIGGER;
		act = ACTIVE_LOW;
		break;
	case GPIO_INTR_LEVEL_HIGH:
		trig = LEVEL_TRIGGER;
		act = ACTIVE_HIGH;
		break;
	default:
		KASSERT(0, ("invalid intr mode 0x%x", intr_mode));
		return;
	}

	reg = AMDGPIO_PIN_REGISTER(pin);

	/* Set the GPIO mode and state */
	AMDGPIO_LOCK(sc);
	val = amdgpio_read_4(sc, reg);
	val &= ~(1 << LEVEL_TRIG_OFF);
	val |= trig << LEVEL_TRIG_OFF;
	val &= ~(3 << ACTIVE_LEVEL_OFF);
	val |= act << ACTIVE_LEVEL_OFF;
	amdgpio_write_4(sc, reg, val);
	AMDGPIO_UNLOCK(sc);
}

static void
amdgpio_pin_enable_intr(device_t dev, uint32_t pin)
{
	struct amdgpio_softc *sc;
	uint32_t reg, val;

	sc = device_get_softc(dev);

	dprintf("pin %d enable intr\n", pin);
	KASSERT(amdgpio_valid_intr_pin(sc, pin), ("invalid pin"));

	reg = AMDGPIO_PIN_REGISTER(pin);

	AMDGPIO_LOCK(sc);
	val = amdgpio_read_4(sc, reg);
	val |= BIT(INTERRUPT_STS_OFF);	/* clear previous status as well */
	val |= BIT(INTERRUPT_ENABLE_OFF);
	amdgpio_write_4(sc, reg, val);
	AMDGPIO_UNLOCK(sc);
}

static void
amdgpio_pin_disable_intr(device_t dev, uint32_t pin)
{
	struct amdgpio_softc *sc;
	uint32_t reg, val;

	sc = device_get_softc(dev);

	dprintf("pin %d disable intr\n", pin);
	KASSERT(amdgpio_valid_intr_pin(sc, pin), ("invalid pin"));

	reg = AMDGPIO_PIN_REGISTER(pin);

	AMDGPIO_LOCK(sc);
	val = amdgpio_read_4(sc, reg);
	val &= ~BIT(INTERRUPT_ENABLE_OFF);
	amdgpio_write_4(sc, reg, val);
	AMDGPIO_UNLOCK(sc);
}

static void
amdgpio_pin_unmask_intr(device_t dev, uint32_t pin)
{
	struct amdgpio_softc *sc;
	uint32_t reg, val;

	sc = device_get_softc(dev);

	dprintf("pin %d unmask intr\n", pin);
	KASSERT(amdgpio_valid_intr_pin(sc, pin), ("invalid pin"));

	reg = AMDGPIO_PIN_REGISTER(pin);

	AMDGPIO_LOCK(sc);
	val = amdgpio_read_4(sc, reg);
	val |= BIT(INTERRUPT_MASK_OFF);
	amdgpio_write_4(sc, reg, val);
	AMDGPIO_UNLOCK(sc);
}

static void
amdgpio_pin_mask_intr(device_t dev, uint32_t pin)
{
	struct amdgpio_softc *sc;
	uint32_t reg, val;

	sc = device_get_softc(dev);

	dprintf("pin %d mask intr\n", pin);
	KASSERT(amdgpio_valid_intr_pin(sc, pin), ("invalid pin"));

	reg = AMDGPIO_PIN_REGISTER(pin);

	AMDGPIO_LOCK(sc);
	val = amdgpio_read_4(sc, reg);
	val &= ~BIT(INTERRUPT_MASK_OFF);
	val |= BIT(INTERRUPT_STS_OFF);	/* clear status as well */
	amdgpio_write_4(sc, reg, val);
	AMDGPIO_UNLOCK(sc);
}

static void
amdgpio_pin_eoi(device_t dev, uint32_t pin)
{
	struct amdgpio_softc *sc;
	uint32_t reg, val;

	sc = device_get_softc(dev);

	dprintf("pin %d eoi intr\n", pin);
	KASSERT(amdgpio_valid_intr_pin(sc, pin), ("invalid pin"));

	reg = AMDGPIO_PIN_REGISTER(pin);

	AMDGPIO_LOCK(sc);
	/* XXX Linux does EOI via WAKE_INT_MASTER_REG and EIO_MASK. */
	val = amdgpio_read_4(sc, reg);
	val |= BIT(INTERRUPT_STS_OFF);
	amdgpio_write_4(sc, reg, val);
	AMDGPIO_UNLOCK(sc);
}

static int
amdgpio_probe(device_t dev)
{
	static char *gpio_ids[] = { "AMD0030", "AMDI0030", NULL };
	int rv;

	if (acpi_disabled("gpio"))
		return (ENXIO);
	rv = ACPI_ID_PROBE(device_get_parent(dev), dev, gpio_ids, NULL);
	if (rv <= 0)
		device_set_desc(dev, "AMD GPIO Controller");

	return (rv);
}

static void
amdgpio_eoi_locked(struct amdgpio_softc *sc)
{
	uint32_t master_reg = amdgpio_read_4(sc, WAKE_INT_MASTER_REG);

	AMDGPIO_ASSERT_LOCKED(sc);
	master_reg |= EOI_MASK;
	amdgpio_write_4(sc, WAKE_INT_MASTER_REG, master_reg);
}

static void
amdgpio_eoi(struct amdgpio_softc *sc)
{
	AMDGPIO_LOCK(sc);
	amdgpio_eoi_locked(sc);
	AMDGPIO_UNLOCK(sc);
}

static int
amdgpio_intr_filter(void *arg)
{
	struct amdgpio_softc *sc = arg;
	int off, rv = FILTER_STRAY;
	uint32_t reg, pin;
	uint64_t status;

	/*
	 * We set up interrupts before attaching the gpiobus, so we might get
	 * interrupts before sc_busdev is set.
	 */
	if (sc->sc_busdev == NULL)
		return (rv);

	/* We can lock in the filter routine as it is MTX_SPIN. */
	AMDGPIO_LOCK(sc);

	/*
	 * Merge into a single 46-bit status.
	 *
	 * See GPIO_Interrupt_Status_Index_0 in BKDG.
	 */
	status = ((uint64_t)amdgpio_read_4(sc, WAKE_INT_STATUS_REG1)) << 32;
	status |= amdgpio_read_4(sc, WAKE_INT_STATUS_REG0);
	status &= AMD_GPIO_INTR_MASK;
	for (size_t i = 0; i < AMD_GPIO_NUM_INTR_BITS; i++) {
		if ((status & (1ULL << i)) == 0)
			continue;
		for (int j = 0; j < AMD_GPIO_PINS_PER_INTR_BIT; j++) {
			pin = i * AMD_GPIO_PINS_PER_INTR_BIT + j;
			off = AMDGPIO_PIN_REGISTER(pin);
			reg = amdgpio_read_4(sc, off);
			if ((reg & UNSERVICED_INTERRUPT_MASK) == 0)
				continue;
			gpiobus_handle_intr(sc->sc_busdev, pin);
			rv = FILTER_HANDLED;

			/*
			 * Must write 1's to wake/interrupt status bits to clear them.
			 * We can do this simply by writing back to the register.
			 */
			amdgpio_write_4(sc, off, reg);
		}
	}

	amdgpio_eoi_locked(sc);
	AMDGPIO_UNLOCK(sc);
	return (rv);
}

static int
amdgpio_attach(device_t dev)
{
	struct amdgpio_softc *sc;
	int i, pin, bank, reg, intrbit;
	uint32_t flags;

	sc = device_get_softc(dev);
	sc->sc_dev = dev;
	sc->sc_handle = acpi_get_handle(dev);

	AMDGPIO_LOCK_INIT(sc);

	sc->sc_nbanks = AMD_GPIO_NUM_PIN_BANK;
	sc->sc_npins = AMD_GPIO_PINS_MAX;
	sc->sc_bank_prefix = AMD_GPIO_PREFIX;
	sc->sc_pin_info = kernzp_pins;
	sc->sc_ngroups = nitems(kernzp_groups);
	sc->sc_groups = kernzp_groups;

	if (bus_alloc_resources(dev, amdgpio_spec, sc->sc_res)) {
		device_printf(dev, "could not allocate resources\n");
		goto err_rsrc;
	}

	sc->sc_bst = rman_get_bustag(sc->sc_res[0]);
	sc->sc_bsh = rman_get_bushandle(sc->sc_res[0]);

	/* Set up interrupt handler. */
	if (bus_setup_intr(dev, sc->sc_res[1], INTR_TYPE_MISC | INTR_MPSAFE,
	    amdgpio_intr_filter, NULL, sc, &sc->sc_intr_handle)
	    != 0) {
		device_printf(dev, "couldn't set up interrupt\n");
		goto err_intr;
	}

	/* Initialize all possible pins to be Invalid */
	for (i = 0; i < AMD_GPIO_PINS_MAX ; i++) {
		snprintf(sc->sc_gpio_pins[i].gp_name, GPIOMAXNAME,
			"Unexposed PIN %d", i);
		sc->sc_gpio_pins[i].gp_pin = -1;
		sc->sc_gpio_pins[i].gp_caps = 0;
		sc->sc_gpio_pins[i].gp_flags = 0;
	}

	/*
	 * Initialize only driver exposed pins with appropriate capabilities.
	 *
	 * XXX Also mask and disable interrupts on all pins, since we don't
	 * support them at the moment.
	 */
	for (i = 0; i < AMD_GPIO_PINS_EXPOSED ; i++) {
		pin = kernzp_pins[i].pin_num;
		bank = pin / AMD_GPIO_PINS_PER_BANK;
		intrbit = pin / AMD_GPIO_PINS_PER_INTR_BIT;
		snprintf(sc->sc_gpio_pins[pin].gp_name, GPIOMAXNAME, "%s%d_%s",
			AMD_GPIO_PREFIX, bank, kernzp_pins[i].pin_name);
		sc->sc_gpio_pins[pin].gp_pin = pin;
		sc->sc_gpio_pins[pin].gp_caps = AMDGPIO_DEFAULT_CAPS;
		if (sc->sc_intr_handle != NULL &&
		    intrbit < AMD_GPIO_NUM_INTR_BITS &&
		    intrbit != AMD_GPIO_RESERVED_INTR_BIT)
			sc->sc_gpio_pins[pin].gp_caps |= AMDGPIO_INTR_CAPS;
		sc->sc_gpio_pins[pin].gp_flags =
		    amdgpio_is_pin_output(sc, pin) ?
		    GPIO_PIN_OUTPUT : GPIO_PIN_INPUT;

		reg = AMDGPIO_PIN_REGISTER(pin);
		flags = amdgpio_read_4(sc, reg);
		flags &= ~(1 << INTERRUPT_ENABLE_OFF);
		flags &= ~(1 << INTERRUPT_MASK_OFF);
		amdgpio_write_4(sc, reg, flags);
	}
	amdgpio_eoi(sc);

	pin = 8;
	reg = AMDGPIO_PIN_REGISTER(pin);
	flags = amdgpio_read_4(sc, reg);
	flags |= (1 << INTERRUPT_ENABLE_OFF);
	flags |= (1 << INTERRUPT_MASK_OFF);
	amdgpio_write_4(sc, reg, flags);
	amdgpio_pin_config_intr(dev, 8, GPIO_INTR_LEVEL_LOW);

	sc->sc_busdev = gpiobus_add_bus(dev);
	if (sc->sc_busdev == NULL) {
		device_printf(dev, "could not attach gpiobus\n");
		goto err_bus;
	}

	bus_attach_children(dev);
	return (0);

err_bus:
	bus_teardown_intr(dev, sc->sc_res[1], sc->sc_intr_handle);
err_intr:
	bus_release_resources(dev, amdgpio_spec, sc->sc_res);
err_rsrc:
	AMDGPIO_LOCK_DESTROY(sc);

	return (ENXIO);
}

static int
amdgpio_detach(device_t dev)
{
	struct amdgpio_softc *sc;
	sc = device_get_softc(dev);

	if (sc->sc_busdev)
		gpiobus_detach_bus(dev);
	if (sc->sc_intr_handle)
		bus_teardown_intr(dev, sc->sc_res[1], sc->sc_intr_handle);
	// TODO bus_release_resource(sc->sc_intr_rid, sc->sc_intr_res)?
	bus_release_resources(dev, amdgpio_spec, sc->sc_res);

	AMDGPIO_LOCK_DESTROY(sc);

	return (0);
}

static int
amdgpio_suspend(device_t dev)
{
	struct amdgpio_softc *sc = device_get_softc(dev);
	uint32_t reg;
	size_t i;
	int pin, off;
	uint32_t unserviced_mask = (1 << INTERRUPT_STS_OFF) |
	    (1 << WAKE_STS_OFF);

	AMDGPIO_LOCK(sc);

	for (i = 0; i < AMD_GPIO_PINS_EXPOSED; i++) {
		pin = kernzp_pins[i].pin_num;
		off = AMDGPIO_PIN_REGISTER(pin);

		reg = amdgpio_read_4(sc, off);
		if ((reg & unserviced_mask) != 0)
			device_printf(dev, "unserviced interrupt on pin %d!\n",
			    pin);
		/*
		 * "Service" interrupt and mask.  In the future we should only
		 * mask non-wake interrupts (see WAKE_CNTRL_OFF_S0I3 &c).
		 *
		 * (From BKDG) write 1 to interrupt & wake status registers to
		 * clear them.  We can do this by just writing back to them.
		 */
		reg &= ~(1 << INTERRUPT_MASK_OFF);
		reg &= ~(1 << INTERRUPT_ENABLE_OFF);
		amdgpio_write_4(sc, off, reg);
	}

	amdgpio_eoi_locked(sc);
	AMDGPIO_UNLOCK(sc);

	return (0);
}

static int
amdgpio_resume(device_t dev)
{
	struct amdgpio_softc *sc = device_get_softc(dev);
	uint32_t reg, flags;

	/*
	 * XXX Will have to resume properly once we're actually able to use
	 * amdgpio for interrupts.
	 */

	AMDGPIO_LOCK(sc);

	reg = AMDGPIO_PIN_REGISTER(8);
	flags = amdgpio_read_4(sc, reg);
	flags |= (1 << INTERRUPT_ENABLE_OFF);
	flags |= (1 << INTERRUPT_MASK_OFF);
	amdgpio_write_4(sc, reg, flags);

	AMDGPIO_UNLOCK(sc);

	return (0);
}

static device_method_t amdgpio_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,		amdgpio_probe),
	DEVMETHOD(device_attach,	amdgpio_attach),
	DEVMETHOD(device_detach,	amdgpio_detach),
	DEVMETHOD(device_suspend,	amdgpio_suspend),
	DEVMETHOD(device_resume,	amdgpio_resume),

	/* GPIO protocol */
	DEVMETHOD(gpio_get_bus,		amdgpio_get_bus),
	DEVMETHOD(gpio_pin_max,		amdgpio_pin_max),
	DEVMETHOD(gpio_pin_getname,	amdgpio_pin_getname),
	DEVMETHOD(gpio_pin_getcaps,	amdgpio_pin_getcaps),
	DEVMETHOD(gpio_pin_getflags,	amdgpio_pin_getflags),
	DEVMETHOD(gpio_pin_setflags,	amdgpio_pin_setflags),
	DEVMETHOD(gpio_pin_get,		amdgpio_pin_get),
	DEVMETHOD(gpio_pin_set,		amdgpio_pin_set),
	DEVMETHOD(gpio_pin_toggle,	amdgpio_pin_toggle),

	DEVMETHOD_END
};

static driver_t amdgpio_driver = {
	"gpio",
	amdgpio_methods,
	sizeof(struct amdgpio_softc),
};

DRIVER_MODULE(amdgpio, acpi, amdgpio_driver, 0, 0);
MODULE_DEPEND(amdgpio, acpi, 1, 1, 1);
MODULE_DEPEND(amdgpio, gpiobus, 1, 1, 1);
MODULE_VERSION(amdgpio, 1);
