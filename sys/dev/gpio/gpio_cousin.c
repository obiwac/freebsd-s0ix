/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The FreeBSD Foundation
 *
 * This software was developed by Aymeric Wibo <obiwac@freebsd.org>
 * under sponsorship from the FreeBSD Foundation.
 */

/*
 * gpio_cousin: child device of acpi_gpiobus that sets up an interrupt on a
 * GPIO pin and forwards it to an acpi_cousin_intr consumer.
 *
 * acpi_gpiobus adds one gpio_cousin child per GPIO interrupt pin discovered
 * during _CRS enumeration.  gpio_cousin owns the pin and its interrupt
 * resource for its entire lifetime.
 */

#include <sys/types.h>
#include <sys/bus.h>
#include <sys/gpio.h>
#include <sys/kernel.h>
#include <sys/module.h>

#include "gpiobus_if.h"

#include <contrib/dev/acpica/include/acpi.h>
#include <dev/acpica/acpivar.h>

#include <dev/gpio/gpiobusvar.h>
#include <dev/gpio/acpi_gpiobusvar.h>

#ifdef INTRNG
# error "INTRNG supported; you should not be using this!"
#endif

struct gpio_cousin_softc {
	gpio_pin_t				 pin;
	int					 intr_rid;
	struct resource				*intr_res;
	void					*intr_cookie;
	struct acpi_cousin_intr_consumers	*cons;
};

static int
gpio_cousin_probe(device_t dev)
{
	/* Only match when acpi_gpiobus explicitly adds us. */
	return (BUS_PROBE_NOWILDCARD);
}

static void
gpio_cousin_intr(void *arg)
{
	struct acpi_cousin_intr_consumers *cons = arg;

	acpi_cousin_intr_trigger(cons);
}

static int
gpio_cousin_attach(device_t dev)
{
	struct gpio_cousin_softc *sc = device_get_softc(dev);
	struct acpi_gpiobus_ivar *super_ivar = device_get_ivars(dev);
	struct gpio_cousin_ivar *ivar = &super_ivar->cousin;
	device_t busdev = device_get_parent(dev);
	uint32_t intr_mode;
	int err;

	device_set_desc(dev, "GPIO cousin interrupt proxy");

	err = gpio_pin_get_by_bus_pinnum(busdev, ivar->pinnum, &sc->pin);
	if (err != 0) {
		device_printf(dev, "cannot acquire pin %d\n", ivar->pinnum);
		return (err);
	}

	err = gpio_pin_setflags(sc->pin, ivar->flags & ~GPIO_INTR_MASK);
	if (err != 0) {
		device_printf(dev, "cannot set flags for pin %d\n",
		    ivar->pinnum);
		goto fail_pin;
	}

	sc->cons = acpi_cousin_intr_provide(NULL, ivar->consumer);
	if (sc->cons == NULL) {
		/*
		 * No consumer attached yet.
		 * TODO: defer attachment until consumer attaches.
		 * Maybe register device_attach eventhandler and move most of this logic in there?
		 */
		err = ENXIO;
		goto fail_pin;
	}

	intr_mode = ivar->flags & GPIO_INTR_MODE_MASK;
	sc->intr_rid = 0;
	sc->intr_res = gpio_alloc_intr_resource(dev, sc->intr_rid, RF_ACTIVE,
	    sc->pin, intr_mode);
	if (sc->intr_res == NULL) {
		device_printf(dev, "cannot allocate interrupt resource for "
		    "pin %d\n", ivar->pinnum);
		err = ENXIO;
		goto fail_cons;
	}

	err = bus_setup_intr(dev, sc->intr_res, INTR_TYPE_TTY | INTR_MPSAFE,
	    NULL, gpio_cousin_intr, sc->cons, &sc->intr_cookie);
	if (err != 0) {
		device_printf(dev, "cannot setup interrupt for pin %d (%d)\n",
		    ivar->pinnum, err);
		goto fail_res;
	}

	return (0);

fail_res:
	bus_release_resource(dev, SYS_RES_IRQ, sc->intr_rid, sc->intr_res);
fail_cons:
	acpi_cousin_intr_detach(sc->cons);
	acpi_cousin_intr_free_cons(sc->cons);
fail_pin:
	gpio_pin_release(sc->pin);
	return (err);
}

static int
gpio_cousin_detach(device_t dev)
{
	struct gpio_cousin_softc *sc = device_get_softc(dev);

	if (sc->intr_cookie != NULL)
		bus_teardown_intr(dev, sc->intr_res, sc->intr_cookie);
	if (sc->intr_res != NULL)
		bus_release_resource(dev, SYS_RES_IRQ, sc->intr_rid,
		    sc->intr_res);
	if (sc->cons != NULL) {
		acpi_cousin_intr_detach(sc->cons);
		acpi_cousin_intr_free_cons(sc->cons);
	}
	if (sc->pin != NULL)
		gpio_pin_release(sc->pin);
	return (0);
}

static device_method_t gpio_cousin_methods[] = {
	DEVMETHOD(device_probe,		gpio_cousin_probe),
	DEVMETHOD(device_attach,	gpio_cousin_attach),
	DEVMETHOD(device_detach,	gpio_cousin_detach),
	DEVMETHOD_END
};

DEFINE_CLASS_0(gpio_cousin, gpio_cousin_driver, gpio_cousin_methods,
    sizeof(struct gpio_cousin_softc));
DRIVER_MODULE(gpio_cousin, gpiobus, gpio_cousin_driver, NULL, NULL);
MODULE_DEPEND(gpio_cousin, acpi_gpiobus, 1, 1, 1);
