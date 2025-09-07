/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2009 Oleksandr Tymoshenko <gonzo@freebsd.org>
 * All rights reserved.
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

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/gpio.h>
#ifdef INTRNG
#include <sys/intr.h>
#else
#include <sys/interrupt.h>
#include <sys/proc.h>
#include <sys/sbuf.h>
#include <sys/sx.h>
#include <sys/sysctl.h>
#include <sys/syslog.h>
#endif
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/sbuf.h>

#include <dev/gpio/gpiobusvar.h>
#include <dev/gpio/gpiobus_internal.h>

#include "gpiobus_if.h"

#undef GPIOBUS_DEBUG
#ifdef GPIOBUS_DEBUG
#define	dprintf printf
#else
#define	dprintf(x, arg...)
#endif

static void gpiobus_print_pins(struct gpiobus_ivar *, struct sbuf *);
static int gpiobus_parse_pins(struct gpiobus_softc *, device_t, int);
static int gpiobus_probe(device_t);
static int gpiobus_suspend(device_t);
static int gpiobus_resume(device_t);
static void gpiobus_probe_nomatch(device_t, device_t);
static int gpiobus_print_child(device_t, device_t);
static device_t gpiobus_add_child(device_t, u_int, const char *, int);
static void gpiobus_hinted_child(device_t, const char *, int);
#ifndef INTRNG
static void gpiobus_pic_init(struct gpiobus_softc *);
static void gpiobus_pic_destroy(struct gpiopic *);
static void gpiobus_pic_resume(struct gpiopic *);
#endif

/*
 * GPIOBUS interface
 */
static int gpiobus_acquire_bus(device_t, device_t, int);
static void gpiobus_release_bus(device_t, device_t);
static int gpiobus_pin_setflags(device_t, device_t, uint32_t, uint32_t);
static int gpiobus_pin_getflags(device_t, device_t, uint32_t, uint32_t*);
static int gpiobus_pin_getcaps(device_t, device_t, uint32_t, uint32_t*);
static int gpiobus_pin_set(device_t, device_t, uint32_t, unsigned int);
static int gpiobus_pin_get(device_t, device_t, uint32_t, unsigned int*);
static int gpiobus_pin_toggle(device_t, device_t, uint32_t);

/*
 * gpiobus_pin flags
 *  The flags in struct gpiobus_pin are not related to the flags used by the
 *  low-level controller driver in struct gpio_pin.  Currently, only pins
 *  acquired via FDT data have gpiobus_pin.flags set, sourced from the flags in
 *  the FDT properties.  In theory, these flags are defined per-platform.  In
 *  practice they are always the flags from the dt-bindings/gpio/gpio.h file.
 *  The only one of those flags we currently support is for handling active-low
 *  pins, so we just define that flag here instead of including a GPL'd header.
 */
#define	GPIO_ACTIVE_LOW 1

/*
 * XXX -> Move me to better place - gpio_subr.c?
 * Also, this function must be changed when interrupt configuration
 * data will be moved into struct resource.
 */
#ifdef INTRNG

struct resource *
gpio_alloc_intr_resource(device_t consumer_dev, int rid, u_int alloc_flags,
    gpio_pin_t pin, uint32_t intr_mode)
{
	u_int irq;
	struct intr_map_data_gpio *gpio_data;
	struct resource *res;

	gpio_data = (struct intr_map_data_gpio *)intr_alloc_map_data(
	    INTR_MAP_DATA_GPIO, sizeof(*gpio_data), M_WAITOK | M_ZERO);
	gpio_data->gpio_pin_num = pin->pin;
	gpio_data->gpio_pin_flags = pin->flags;
	gpio_data->gpio_intr_mode = intr_mode;

	irq = intr_map_irq(pin->dev, 0, (struct intr_map_data *)gpio_data);
	res = bus_alloc_resource(consumer_dev, SYS_RES_IRQ, rid, irq, irq, 1,
	    alloc_flags);
	if (res == NULL) {
		intr_unmap_irq(irq);
		return (NULL);
	}
	return (res);
}
#endif

int
gpio_check_flags(uint32_t caps, uint32_t flags)
{

	/* Filter unwanted flags. */
	flags &= caps;

	/* Cannot mix input/output together. */
	if (flags & GPIO_PIN_INPUT && flags & GPIO_PIN_OUTPUT)
		return (EINVAL);
	/* Cannot mix pull-up/pull-down together. */
	if (flags & GPIO_PIN_PULLUP && flags & GPIO_PIN_PULLDOWN)
		return (EINVAL);
	/* Cannot mix output and interrupt flags together */
	if (flags & GPIO_PIN_OUTPUT && flags & GPIO_INTR_MASK)
		return (EINVAL);
	/* Only one interrupt flag can be defined at once */
	if ((flags & GPIO_INTR_MASK) & ((flags & GPIO_INTR_MASK) - 1))
		return (EINVAL);
	/* The interrupt attached flag cannot be set */
	if (flags & GPIO_INTR_ATTACHED)
		return (EINVAL);

	return (0);
}

int
gpio_pin_get_by_bus_pinnum(device_t busdev, uint32_t pinnum, gpio_pin_t *ppin)
{
	gpio_pin_t pin;
	int err;

	err = gpiobus_acquire_pin(busdev, pinnum);
	if (err != 0)
		return (EBUSY);

	pin = malloc(sizeof(*pin), M_DEVBUF, M_WAITOK | M_ZERO);

	pin->dev = device_get_parent(busdev);
	pin->pin = pinnum;
	pin->flags = 0;

	*ppin = pin;
	return (0);
}

int
gpio_pin_get_by_child_index(device_t childdev, uint32_t idx, gpio_pin_t *ppin)
{
	struct gpiobus_ivar *devi;

	devi = GPIOBUS_IVAR(childdev);
	if (idx >= devi->npins)
		return (EINVAL);

	return (gpio_pin_get_by_bus_pinnum(device_get_parent(childdev),
	    devi->pins[idx], ppin));
}

int
gpio_pin_getcaps(gpio_pin_t pin, uint32_t *caps)
{

	KASSERT(pin != NULL, ("GPIO pin is NULL."));
	KASSERT(pin->dev != NULL, ("GPIO pin device is NULL."));
	return (GPIO_PIN_GETCAPS(pin->dev, pin->pin, caps));
}

int
gpio_pin_is_active(gpio_pin_t pin, bool *active)
{
	int rv;
	uint32_t tmp;

	KASSERT(pin != NULL, ("GPIO pin is NULL."));
	KASSERT(pin->dev != NULL, ("GPIO pin device is NULL."));
	rv = GPIO_PIN_GET(pin->dev, pin->pin, &tmp);
	if (rv  != 0) {
		return (rv);
	}

	if (pin->flags & GPIO_ACTIVE_LOW)
		*active = tmp == 0;
	else
		*active = tmp != 0;
	return (0);
}

/*
 * Note that this function should only
 * be used in cases where a pre-existing
 * gpiobus_pin structure exists. In most
 * cases, the gpio_pin_get_by_* functions
 * suffice.
 */
int
gpio_pin_acquire(gpio_pin_t gpio)
{
	device_t busdev;

	KASSERT(gpio != NULL, ("GPIO pin is NULL."));
	KASSERT(gpio->dev != NULL, ("GPIO pin device is NULL."));

	busdev = GPIO_GET_BUS(gpio->dev);
	if (busdev == NULL)
		return (ENXIO);

	return (gpiobus_acquire_pin(busdev, gpio->pin));
}

void
gpio_pin_release(gpio_pin_t gpio)
{
	device_t busdev;

	KASSERT(gpio != NULL, ("GPIO pin is NULL."));
	KASSERT(gpio->dev != NULL, ("GPIO pin device is NULL."));

	busdev = GPIO_GET_BUS(gpio->dev);
	KASSERT(busdev != NULL, ("gpiobus dev is NULL."));

	gpiobus_release_pin(busdev, gpio->pin);
	free(gpio, M_DEVBUF);
}

int
gpio_pin_set_active(gpio_pin_t pin, bool active)
{
	int rv;
	uint32_t tmp;

	if (pin->flags & GPIO_ACTIVE_LOW)
		tmp = active ? 0 : 1;
	else
		tmp = active ? 1 : 0;

	KASSERT(pin != NULL, ("GPIO pin is NULL."));
	KASSERT(pin->dev != NULL, ("GPIO pin device is NULL."));
	rv = GPIO_PIN_SET(pin->dev, pin->pin, tmp);
	return (rv);
}

int
gpio_pin_setflags(gpio_pin_t pin, uint32_t flags)
{
	int rv;

	KASSERT(pin != NULL, ("GPIO pin is NULL."));
	KASSERT(pin->dev != NULL, ("GPIO pin device is NULL."));

	rv = GPIO_PIN_SETFLAGS(pin->dev, pin->pin, flags);
	return (rv);
}

static void
gpiobus_print_pins(struct gpiobus_ivar *devi, struct sbuf *sb)
{
	int i, range_start, range_stop, need_coma;

	if (devi->npins == 0)
		return;

	need_coma = 0;
	range_start = range_stop = devi->pins[0];
	for (i = 1; i < devi->npins; i++) {
		if (devi->pins[i] != (range_stop + 1)) {
			if (need_coma)
				sbuf_cat(sb, ",");
			if (range_start != range_stop)
				sbuf_printf(sb, "%d-%d", range_start, range_stop);
			else
				sbuf_printf(sb, "%d", range_start);
			range_start = range_stop = devi->pins[i];
			need_coma = 1;
		}
		else
			range_stop++;
	}

	if (need_coma)
		sbuf_cat(sb, ",");
	if (range_start != range_stop)
		sbuf_printf(sb, "%d-%d", range_start, range_stop);
	else
		sbuf_printf(sb, "%d", range_start);
}

device_t
gpiobus_add_bus(device_t dev)
{
	device_t busdev;

	busdev = device_add_child(dev, "gpiobus", DEVICE_UNIT_ANY);
	if (busdev == NULL)
		return (NULL);
#ifdef FDT
	ofw_gpiobus_register_provider(dev);
#endif
	return (busdev);
}

int
gpiobus_detach_bus(device_t dev)
{
#ifdef FDT
	ofw_gpiobus_unregister_provider(dev);
#endif
	return (bus_generic_detach(dev));
}

int
gpiobus_init_softc(device_t dev)
{
	struct gpiobus_softc *sc;

	sc = GPIOBUS_SOFTC(dev);
	sc->sc_busdev = dev;
	sc->sc_dev = device_get_parent(dev);

	if (GPIO_PIN_MAX(sc->sc_dev, &sc->sc_npins) != 0)
		return (ENXIO);
	KASSERT(sc->sc_npins >= 0, ("GPIO device with no pins"));

	/* Pins = GPIO_PIN_MAX() + 1 */
	sc->sc_npins++;

	sc->sc_intr_rman.rm_type = RMAN_ARRAY;
	sc->sc_intr_rman.rm_descr = "GPIO Interrupts";
#ifndef INTRNG
	sc->sc_intr_rman.rm_start = 0;
	sc->sc_intr_rman.rm_end = sc->sc_npins - 1;
#endif
	if (rman_init(&sc->sc_intr_rman) != 0)
		panic("%s: failed to set up rman.", __func__);
#ifdef INTRNG
	if (rman_manage_region(&sc->sc_intr_rman, 0, ~0) != 0)
#else
	if (rman_manage_region(&sc->sc_intr_rman, 0, sc->sc_npins - 1) != 0)
#endif
		panic("%s: failed to set up rman.", __func__);

	sc->sc_pins = malloc(sizeof(*sc->sc_pins) * sc->sc_npins, M_DEVBUF,
	    M_NOWAIT | M_ZERO);
	if (sc->sc_pins == NULL)
		return (ENOMEM);

	/* Initialize the bus lock. */
	GPIOBUS_LOCK_INIT(sc);

#ifndef INTRNG
	gpiobus_pic_init(sc);
#endif
	return (0);
}

int
gpiobus_add_gpioc(device_t dev)
{
	struct gpiobus_ivar *devi;
	struct gpiobus_softc *sc;
	device_t gpioc;
	int err;

	gpioc = BUS_ADD_CHILD(dev, 0, "gpioc", DEVICE_UNIT_ANY);
	if (gpioc == NULL)
		return (ENXIO);

	sc = device_get_softc(dev);
	devi = device_get_ivars(gpioc);

	devi->npins = sc->sc_npins;
	err = gpiobus_alloc_ivars(devi);
	if (err != 0) {
		device_delete_child(dev, gpioc);
		return (err);
	}

	err = GPIO_GET_PIN_LIST(sc->sc_dev, devi->pins);
	if (err != 0) {
		device_delete_child(dev, gpioc);
		gpiobus_free_ivars(devi);
	}

	return (err);
}

int
gpiobus_alloc_ivars(struct gpiobus_ivar *devi)
{

	/* Allocate pins and flags memory. */
	devi->pins = malloc(sizeof(uint32_t) * devi->npins, M_DEVBUF,
	    M_NOWAIT | M_ZERO);
	if (devi->pins == NULL)
		return (ENOMEM);
	return (0);
}

void
gpiobus_free_ivars(struct gpiobus_ivar *devi)
{

	if (devi->pins) {
		free(devi->pins, M_DEVBUF);
		devi->pins = NULL;
	}
	devi->npins = 0;
}

int
gpiobus_acquire_pin(device_t bus, uint32_t pin)
{
	struct gpiobus_softc *sc;

	sc = device_get_softc(bus);
	/* Consistency check. */
	if (pin >= sc->sc_npins) {
		panic("%s: invalid pin %d, max: %d",
		    device_get_nameunit(bus), pin, sc->sc_npins - 1);
	}
	/* Mark pin as mapped and give warning if it's already mapped. */
	if (sc->sc_pins[pin].mapped) {
		device_printf(bus, "warning: pin %d is already mapped\n", pin);
		return (EBUSY);
	}
	sc->sc_pins[pin].mapped = 1;

	return (0);
}

/* Release mapped pin */
void
gpiobus_release_pin(device_t bus, uint32_t pin)
{
	struct gpiobus_softc *sc;

	sc = device_get_softc(bus);
	/* Consistency check. */
	if (pin >= sc->sc_npins) {
		panic("%s: invalid pin %d, max: %d",
		    device_get_nameunit(bus), pin, sc->sc_npins - 1);
	}

	if (!sc->sc_pins[pin].mapped)
		panic("%s: pin %d is not mapped", device_get_nameunit(bus),
		    pin);

	sc->sc_pins[pin].mapped = 0;
}

static int
gpiobus_acquire_child_pins(device_t dev, device_t child)
{
	struct gpiobus_ivar *devi = GPIOBUS_IVAR(child);
	int i;

	for (i = 0; i < devi->npins; i++) {
		/* Reserve the GPIO pin. */
		if (gpiobus_acquire_pin(dev, devi->pins[i]) != 0) {
			device_printf(child, "cannot acquire pin %d\n",
			    devi->pins[i]);
			while (--i >= 0) {
				gpiobus_release_pin(dev, devi->pins[i]);
			}
			gpiobus_free_ivars(devi);
			return (EBUSY);
		}
	}
	for (i = 0; i < devi->npins; i++) {
		/* Use the child name as pin name. */
		GPIOBUS_PIN_SETNAME(dev, devi->pins[i],
		    device_get_nameunit(child));

		/* Set pin as a potential interrupt resource for the child. */
		resource_list_add(&devi->rl, SYS_RES_IRQ, i, devi->pins[i],
		    devi->pins[i], 1);

	}
	return (0);
}

static int
gpiobus_parse_pins(struct gpiobus_softc *sc, device_t child, int mask)
{
	struct gpiobus_ivar *devi = GPIOBUS_IVAR(child);
	int i, npins;

	npins = 0;
	for (i = 0; i < 32; i++) {
		if (mask & (1 << i))
			npins++;
	}
	if (npins == 0) {
		device_printf(child, "empty pin mask\n");
		return (EINVAL);
	}
	devi->npins = npins;
	if (gpiobus_alloc_ivars(devi) != 0) {
		device_printf(child, "cannot allocate device ivars\n");
		return (EINVAL);
	}
	npins = 0;
	for (i = 0; i < 32; i++) {
		if ((mask & (1 << i)) == 0)
			continue;
		devi->pins[npins++] = i;
	}

	return (0);
}

static int
gpiobus_parse_pin_list(struct gpiobus_softc *sc, device_t child,
    const char *pins)
{
	struct gpiobus_ivar *devi = GPIOBUS_IVAR(child);
	const char *p;
	char *endp;
	unsigned long pin;
	int i, npins;

	npins = 0;
	p = pins;
	for (;;) {
		pin = strtoul(p, &endp, 0);
		if (endp == p)
			break;
		npins++;
		if (*endp == '\0')
			break;
		p = endp + 1;
	}

	if (*endp != '\0') {
		device_printf(child, "garbage in the pin list: %s\n", endp);
		return (EINVAL);
	}
	if (npins == 0) {
		device_printf(child, "empty pin list\n");
		return (EINVAL);
	}

	devi->npins = npins;
	if (gpiobus_alloc_ivars(devi) != 0) {
		device_printf(child, "cannot allocate device ivars\n");
		return (EINVAL);
	}

	i = 0;
	p = pins;
	for (;;) {
		pin = strtoul(p, &endp, 0);

		devi->pins[i] = pin;

		if (*endp == '\0')
			break;
		i++;
		p = endp + 1;
	}

	return (0);
}

static int
gpiobus_probe(device_t dev)
{
	device_set_desc(dev, "GPIO bus");

	return (BUS_PROBE_GENERIC);
}

int
gpiobus_attach(device_t dev)
{
	int err;

	err = gpiobus_init_softc(dev);
	if (err != 0)
		return (err);

	err = gpiobus_add_gpioc(dev);
	if (err != 0)
		return (err);

	/*
	 * Get parent's pins and mark them as unmapped
	 */
	bus_identify_children(dev);
	bus_enumerate_hinted_children(dev);

	bus_attach_children(dev);
	return (0);
}

/*
 * Since this is not a self-enumerating bus, and since we always add
 * children in attach, we have to always delete children here.
 */
int
gpiobus_detach(device_t dev)
{
	struct gpiobus_softc *sc;
	struct gpiopic *pic;
	int i, err;

	sc = GPIOBUS_SOFTC(dev);
	pic = sc->sc_pic;
	KASSERT(mtx_initialized(&sc->sc_mtx),
	    ("gpiobus mutex not initialized"));

	if ((err = bus_generic_detach(dev)) != 0)
		return (err);

	// MPASS(rman_fini(&sc->sc_intr_rman) != EBUSY); // TODO Is this kind of assert okay?
	rman_fini(&sc->sc_intr_rman);
	if (sc->sc_pins) {
		for (i = 0; i < sc->sc_npins; i++) {
			if (sc->sc_pins[i].name != NULL)
				free(sc->sc_pins[i].name, M_DEVBUF);
			sc->sc_pins[i].name = NULL;
		}
		free(sc->sc_pins, M_DEVBUF);
		sc->sc_pins = NULL;
	}

#ifndef INTRNG
	gpiobus_pic_destroy(pic);
#endif
	GPIOBUS_LOCK_DESTROY(sc);
	return (0);
}

static int
gpiobus_suspend(device_t dev)
{

	return (bus_generic_suspend(dev));
}

static int
gpiobus_resume(device_t dev)
{
	struct gpiobus_softc *sc;

	sc = GPIOBUS_SOFTC(dev);
	gpiobus_pic_resume(sc->sc_pic);
	return (bus_generic_resume(dev));
}

static void
gpiobus_probe_nomatch(device_t dev, device_t child)
{
	char pins[128];
	struct sbuf sb;
	struct gpiobus_ivar *devi;

	devi = GPIOBUS_IVAR(child);
	sbuf_new(&sb, pins, sizeof(pins), SBUF_FIXEDLEN);
	gpiobus_print_pins(devi, &sb);
	sbuf_finish(&sb);
	device_printf(dev, "<unknown device> at pin%s %s",
	    devi->npins > 1 ? "s" : "", sbuf_data(&sb));
	resource_list_print_type(&devi->rl, "irq", SYS_RES_IRQ, "%jd");
	printf("\n");
}

static int
gpiobus_print_child(device_t dev, device_t child)
{
	char pins[128];
	struct sbuf sb;
	int retval = 0;
	struct gpiobus_ivar *devi;

	devi = GPIOBUS_IVAR(child);
	retval += bus_print_child_header(dev, child);
	if (devi->npins > 0) {
		if (devi->npins > 1)
			retval += printf(" at pins ");
		else
			retval += printf(" at pin ");
		sbuf_new(&sb, pins, sizeof(pins), SBUF_FIXEDLEN);
		gpiobus_print_pins(devi, &sb);
		sbuf_finish(&sb);
		retval += printf("%s", sbuf_data(&sb));
	}
	resource_list_print_type(&devi->rl, "irq", SYS_RES_IRQ, "%jd");
	retval += bus_print_child_footer(dev, child);

	return (retval);
}

int
gpiobus_child_location(device_t bus, device_t child, struct sbuf *sb)
{
	struct gpiobus_ivar *devi;

	devi = GPIOBUS_IVAR(child);
	sbuf_printf(sb, "pins=");
	gpiobus_print_pins(devi, sb);

	return (0);
}

device_t
gpiobus_add_child_common(device_t dev, u_int order, const char *name, int unit,
    size_t ivars_size)
{
	device_t child;
	struct gpiobus_ivar *devi;

	KASSERT(ivars_size >= sizeof(struct gpiobus_ivar),
	    ("child ivars must include gpiobus_ivar as their first member"));
	child = device_add_child_ordered(dev, order, name, unit);
	if (child == NULL) 
		return (child);
	devi = malloc(ivars_size, M_DEVBUF, M_NOWAIT | M_ZERO);
	if (devi == NULL) {
		device_delete_child(dev, child);
		return (NULL);
	}
	resource_list_init(&devi->rl);
	device_set_ivars(child, devi);

	return (child);
}

static device_t
gpiobus_add_child(device_t dev, u_int order, const char *name, int unit)
{
	return (gpiobus_add_child_common(dev, order, name, unit,
	    sizeof(struct gpiobus_ivar)));
}

void
gpiobus_child_deleted(device_t dev, device_t child)
{
	struct gpiobus_ivar *devi;

	devi = GPIOBUS_IVAR(child);
	if (devi == NULL)
		return;
	gpiobus_free_ivars(devi);
	resource_list_free(&devi->rl);
	free(devi, M_DEVBUF);
}

static int
gpiobus_rescan(device_t dev)
{

	/*
	 * Re-scan is supposed to remove and add children, but if someone has
	 * deleted the hints for a child we attached earlier, we have no easy
	 * way to handle that.  So this just attaches new children for whom new
	 * hints or drivers have arrived since we last tried.
	 */
	bus_enumerate_hinted_children(dev);
	bus_attach_children(dev);
	return (0);
}

static void
gpiobus_hinted_child(device_t bus, const char *dname, int dunit)
{
	struct gpiobus_softc *sc = GPIOBUS_SOFTC(bus);
	device_t child;
	const char *pins;
	int irq, pinmask;

	if (device_find_child(bus, dname, dunit) != NULL) {
		return;
	}

	child = BUS_ADD_CHILD(bus, 0, dname, dunit);
	if (resource_int_value(dname, dunit, "pins", &pinmask) == 0) {
		if (gpiobus_parse_pins(sc, child, pinmask)) {
			device_delete_child(bus, child);
			return;
		}
	}
	else if (resource_string_value(dname, dunit, "pin_list", &pins) == 0) {
		if (gpiobus_parse_pin_list(sc, child, pins)) {
			device_delete_child(bus, child);
			return;
		}
	}
	if (resource_int_value(dname, dunit, "irq", &irq) == 0) {
		if (bus_set_resource(child, SYS_RES_IRQ, 0, irq, 1) != 0)
			device_printf(bus,
			    "warning: bus_set_resource() failed\n");
	}
}

int
gpiobus_read_ivar(device_t dev, device_t child, int which, uintptr_t *result)
{
	struct gpiobus_ivar *devi;

	devi = GPIOBUS_IVAR(child);
        switch (which) {
	case GPIOBUS_IVAR_NPINS:
		*result = devi->npins;
		break;
	case GPIOBUS_IVAR_PINS:
		/* Children do not ever need to directly examine this. */
		return (ENOTSUP);
        default:
                return (ENOENT);
        }

	return (0);
}

static int
gpiobus_write_ivar(device_t dev, device_t child, int which, uintptr_t value)
{
	struct gpiobus_ivar *devi;
	const uint32_t *ptr;
	int i;

	devi = GPIOBUS_IVAR(child);
        switch (which) {
	case GPIOBUS_IVAR_NPINS:
		/* GPIO ivars are set once. */
		if (devi->npins != 0) {
			return (EBUSY);
		}
		devi->npins = value;
		if (gpiobus_alloc_ivars(devi) != 0) {
			device_printf(child, "cannot allocate device ivars\n");
			devi->npins = 0;
			return (ENOMEM);
		}
		break;
	case GPIOBUS_IVAR_PINS:
		ptr = (const uint32_t *)value;
		for (i = 0; i < devi->npins; i++)
			devi->pins[i] = ptr[i];
		if (gpiobus_acquire_child_pins(dev, child) != 0)
			return (EBUSY);
		break;
        default:
                return (ENOENT);
        }

        return (0);
}

static struct rman *
gpiobus_get_rman(device_t bus, int type, u_int flags)
{
	struct gpiobus_softc *sc;

	sc = device_get_softc(bus);
	switch (type) {
	case SYS_RES_IRQ:
		return (&sc->sc_intr_rman);
	default:
		return (NULL);
	}
}

/*
 * For now, this method supports only requests for GPIO IRQ resources.
 * The requesting device, a consumer, does not have to be a child or
 * a descendant of the bus device.
 *
 * FIXME
 * The method does not support non-IRQ resources or non-GPIO IRQ resources
 * even for its children.  This should be fixed as soon as there is
 * a child driver needs access to system resources.
 */
static struct resource *
gpiobus_alloc_resource(device_t bus, device_t child, int type, int rid,
    rman_res_t start, rman_res_t end, rman_res_t count, u_int flags)
{
	struct rman *rm;
	struct resource *r;
	struct resource_list *rl;
	struct resource_list_entry *rle;
	int isdefault;

	isdefault = RMAN_IS_DEFAULT_RANGE(start, end) && count == 1 &&
	    device_get_parent(child) == bus;
	if (isdefault) {
		rl = BUS_GET_RESOURCE_LIST(bus, child);
		if (rl == NULL)
			return (NULL);
		rle = resource_list_find(rl, type, rid);
		if (rle == NULL)
			return (NULL);
		start = rle->start;
		count = rle->count;
		end = rle->end;
	}

	rm = gpiobus_get_rman(bus, type, flags);
	r = rman_reserve_resource(rm, start, end, count, flags & ~RF_ACTIVE,
	    child);
	if (r == NULL)
		return (NULL);
	rman_set_rid(r, rid);
	rman_set_type(r, type);

	if ((flags & RF_ACTIVE) != 0 && rman_activate_resource(r) != 0) {
		rman_release_resource(r);
		return (NULL);
	}

	return (r);
}

int
gpiobus_release_resource(device_t dev, device_t child, struct resource *r);

int
gpiobus_release_resource(device_t dev, device_t child, struct resource *r)
{
	struct rman *rm;
	int err;
	u_int flags;
#ifdef INTRNG
	u_int irq;

	irq = rman_get_start(r);
	MPASS(irq == rman_get_end(r));
#endif
	/* TODO Can I just use bus_generic_rman_release_resource? */

	flags = rman_get_flags(r);
	rm = gpiobus_get_rman(dev, rman_get_type(r), flags);
	if (!rman_is_region_manager(r, rm)) {
		device_printf(dev, "release_resource: bad resource\n");
		return (EINVAL);
	}
	if (flags & RF_ACTIVE) {
		err = rman_deactivate_resource(r);
		if (err != 0)
			return (err);
	}

#ifdef INTRNG
	intr_unmap_irq(irq);
#endif
	return (rman_release_resource(r));
}

static struct resource_list *
gpiobus_get_resource_list(device_t bus __unused, device_t child)
{
	struct gpiobus_ivar *ivar;

	ivar = GPIOBUS_IVAR(child);

	return (&ivar->rl);
}

static int
gpiobus_acquire_bus(device_t busdev, device_t consumer, int how)
{
	struct gpiobus_softc *sc;

	sc = device_get_softc(busdev);
	GPIOBUS_ASSERT_UNLOCKED(sc);
	GPIOBUS_LOCK(sc);
	if (sc->sc_owner != NULL) {
		if (sc->sc_owner == consumer)
			panic("%s: %s still owns the bus.",
			    device_get_nameunit(busdev),
			    device_get_nameunit(consumer));
		if (how == GPIOBUS_DONTWAIT) {
			GPIOBUS_UNLOCK(sc);
			return (EWOULDBLOCK);
		}
		while (sc->sc_owner != NULL)
			mtx_sleep(sc, &sc->sc_mtx, 0, "gpiobuswait", 0);
	}
	sc->sc_owner = consumer;
	GPIOBUS_UNLOCK(sc);

	return (0);
}

static void
gpiobus_release_bus(device_t busdev, device_t consumer)
{
	struct gpiobus_softc *sc;

	sc = device_get_softc(busdev);
	GPIOBUS_ASSERT_UNLOCKED(sc);
	GPIOBUS_LOCK(sc);
	if (sc->sc_owner == NULL)
		panic("%s: %s releasing unowned bus.",
		    device_get_nameunit(busdev),
		    device_get_nameunit(consumer));
	if (sc->sc_owner != consumer)
		panic("%s: %s trying to release bus owned by %s",
		    device_get_nameunit(busdev),
		    device_get_nameunit(consumer),
		    device_get_nameunit(sc->sc_owner));
	sc->sc_owner = NULL;
	wakeup(sc);
	GPIOBUS_UNLOCK(sc);
}

static int
gpiobus_pin_setflags(device_t dev, device_t child, uint32_t pin, 
    uint32_t flags)
{
	struct gpiobus_softc *sc = GPIOBUS_SOFTC(dev);
	struct gpiobus_ivar *devi = GPIOBUS_IVAR(child);
	uint32_t caps;

	if (pin >= devi->npins)
		return (EINVAL);
	if (GPIO_PIN_GETCAPS(sc->sc_dev, devi->pins[pin], &caps) != 0)
		return (EINVAL);
	if (gpio_check_flags(caps, flags) != 0)
		return (EINVAL);

	return (GPIO_PIN_SETFLAGS(sc->sc_dev, devi->pins[pin], flags));
}

static int
gpiobus_pin_getflags(device_t dev, device_t child, uint32_t pin, 
    uint32_t *flags)
{
	struct gpiobus_softc *sc = GPIOBUS_SOFTC(dev);
	struct gpiobus_ivar *devi = GPIOBUS_IVAR(child);

	if (pin >= devi->npins)
		return (EINVAL);

	return (GPIO_PIN_GETFLAGS(sc->sc_dev, devi->pins[pin], flags));
}

static int
gpiobus_pin_getcaps(device_t dev, device_t child, uint32_t pin, 
    uint32_t *caps)
{
	struct gpiobus_softc *sc = GPIOBUS_SOFTC(dev);
	struct gpiobus_ivar *devi = GPIOBUS_IVAR(child);

	if (pin >= devi->npins)
		return (EINVAL);

	return (GPIO_PIN_GETCAPS(sc->sc_dev, devi->pins[pin], caps));
}

static int
gpiobus_pin_set(device_t dev, device_t child, uint32_t pin, 
    unsigned int value)
{
	struct gpiobus_softc *sc = GPIOBUS_SOFTC(dev);
	struct gpiobus_ivar *devi = GPIOBUS_IVAR(child);

	if (pin >= devi->npins)
		return (EINVAL);

	return (GPIO_PIN_SET(sc->sc_dev, devi->pins[pin], value));
}

static int
gpiobus_pin_get(device_t dev, device_t child, uint32_t pin, 
    unsigned int *value)
{
	struct gpiobus_softc *sc = GPIOBUS_SOFTC(dev);
	struct gpiobus_ivar *devi = GPIOBUS_IVAR(child);

	if (pin >= devi->npins)
		return (EINVAL);

	return (GPIO_PIN_GET(sc->sc_dev, devi->pins[pin], value));
}

static int
gpiobus_pin_toggle(device_t dev, device_t child, uint32_t pin)
{
	struct gpiobus_softc *sc = GPIOBUS_SOFTC(dev);
	struct gpiobus_ivar *devi = GPIOBUS_IVAR(child);

	if (pin >= devi->npins)
		return (EINVAL);

	return (GPIO_PIN_TOGGLE(sc->sc_dev, devi->pins[pin]));
}

/*
 * Verify that a child has all the pins they are requesting
 * to access in their ivars.
 */
static bool
gpiobus_pin_verify_32(struct gpiobus_ivar *devi, uint32_t first_pin,
    uint32_t num_pins)
{
	if (first_pin + num_pins > devi->npins)
		return (false);

	/* Make sure the pins are consecutive. */
	for (uint32_t pin = first_pin; pin < first_pin + num_pins - 1; pin++) {
		if (devi->pins[pin] + 1 != devi->pins[pin + 1])
			return (false);
	}

	return (true);
}

static int
gpiobus_pin_access_32(device_t dev, device_t child, uint32_t first_pin,
    uint32_t clear_pins, uint32_t change_pins, uint32_t *orig_pins)
{
	struct gpiobus_softc *sc = GPIOBUS_SOFTC(dev);
	struct gpiobus_ivar *devi = GPIOBUS_IVAR(child);

	if (!gpiobus_pin_verify_32(devi, first_pin, 32))
		return (EINVAL);

	return (GPIO_PIN_ACCESS_32(sc->sc_dev, devi->pins[first_pin],
	    clear_pins, change_pins, orig_pins));
}

static int
gpiobus_pin_config_32(device_t dev, device_t child, uint32_t first_pin,
    uint32_t num_pins, uint32_t *pin_flags)
{
	struct gpiobus_softc *sc = GPIOBUS_SOFTC(dev);
	struct gpiobus_ivar *devi = GPIOBUS_IVAR(child);

	if (num_pins > 32)
		return (EINVAL);
	if (!gpiobus_pin_verify_32(devi, first_pin, num_pins))
		return (EINVAL);

	return (GPIO_PIN_CONFIG_32(sc->sc_dev,
	    devi->pins[first_pin], num_pins, pin_flags));
}

static int
gpiobus_pin_getname(device_t dev, uint32_t pin, char *name)
{
	struct gpiobus_softc *sc;

	sc = GPIOBUS_SOFTC(dev);
	if (pin > sc->sc_npins)
		return (EINVAL);
	/* Did we have a name for this pin ? */
	if (sc->sc_pins[pin].name != NULL) {
		memcpy(name, sc->sc_pins[pin].name, GPIOMAXNAME);
		return (0);
	}

	/* Return the default pin name. */
	return (GPIO_PIN_GETNAME(device_get_parent(dev), pin, name));
}

static int
gpiobus_pin_setname(device_t dev, uint32_t pin, const char *name)
{
	struct gpiobus_softc *sc;

	sc = GPIOBUS_SOFTC(dev);
	if (pin > sc->sc_npins)
		return (EINVAL);
	if (name == NULL)
		return (EINVAL);
	/* Save the pin name. */
	if (sc->sc_pins[pin].name == NULL)
		sc->sc_pins[pin].name = malloc(GPIOMAXNAME, M_DEVBUF,
		    M_WAITOK | M_ZERO);
	strlcpy(sc->sc_pins[pin].name, name, GPIOMAXNAME);

	return (0);
}

#ifndef INTRNG

struct gpiopic;

struct gpiopic_intsrc {
	struct gpiopic *is_pic;
	struct intr_event *is_event;
	u_long *is_count;	/* TODO expose to userland. */
	u_long *is_straycount;	/* TODO expose to userland. */
	int is_handlers;
	u_int is_pin;
	u_int is_mode;
	u_int is_enabled:1;
	u_int is_masked:1;
};

struct gpiopic {
	struct sx intrsrc_lock;
	struct gpiobus_softc *sc;
	struct gpiopic_intsrc *intr_srcs;
	u_long *intr_counts;
};

static int
sysctl_gpio_intrs(SYSCTL_HANDLER_ARGS)
{
	struct sbuf sbuf;
	struct gpiopic *pic = arg1;
	struct gpiopic_intsrc *isrc;
	u_int i;
	int error;

	error = sysctl_wire_old_buffer(req, 0);
	if (error != 0)
		return (error);

	sbuf_new_for_sysctl(&sbuf, NULL, 128, req);
	sx_slock(&pic->intrsrc_lock);
	for (i = 0; i < pic->sc->sc_npins; i++) {
		isrc = &pic->intr_srcs[i];
		if (isrc->is_event == NULL)
			continue;
		sbuf_printf(&sbuf, "%s:%u (mode 0x%08x): %lu\n",
		    isrc->is_event->ie_fullname,
		    isrc->is_pin,
		    isrc->is_mode,
		    *isrc->is_count);
	}

	sx_sunlock(&pic->intrsrc_lock);
	error = sbuf_finish(&sbuf);
	sbuf_delete(&sbuf);
	return (error);
}

static int
gpiopic_assign_cpu(void *arg, int cpu)
{
	/*
	 * This could be made to work if only a single pin is configured
	 * to be an interrupt source but not in general case.
	 * At this time there does not appear to be a case for binding
	 * GPIO interrupts, so not bothering with the only case that could
	 * be made to work.
	 */
	return (EOPNOTSUPP);
}

static void
gpiopic_unmask_intr(void *arg)
{
	struct gpiopic_intsrc *gisrc = arg;

	gisrc->is_masked = 0;
	GPIO_PIN_UNMASK_INTR(gisrc->is_pic->sc->sc_dev, gisrc->is_pin);
}

static void
gpiopic_mask_intr(void *arg)
{
	struct gpiopic_intsrc *gisrc = arg;

	gisrc->is_masked = 1;
	GPIO_PIN_MASK_INTR(gisrc->is_pic->sc->sc_dev, gisrc->is_pin);
}

static void
gpiopic_eoi(void *arg)
{
	struct gpiopic_intsrc *gisrc = arg;

	GPIO_PIN_EOI(gisrc->is_pic->sc->sc_dev, gisrc->is_pin);
}

static void
gpiopic_enable_intr(struct gpiopic_intsrc *gisrc)
{
	gisrc->is_enabled = 1;
	GPIO_PIN_ENABLE_INTR(gisrc->is_pic->sc->sc_dev, gisrc->is_pin);
}

static void
gpiopic_disable_intr(struct gpiopic_intsrc *gisrc)
{
	gisrc->is_enabled = 0;
	GPIO_PIN_DISABLE_INTR(gisrc->is_pic->sc->sc_dev, gisrc->is_pin);
}

static void
gpiopic_config_intr(struct gpiopic_intsrc *gisrc, uint32_t intr_mode) {
	gisrc->is_mode = intr_mode;
	GPIO_PIN_CONFIG_INTR(gisrc->is_pic->sc->sc_dev, gisrc->is_pin,
	    intr_mode);
}

static int
gpiopic_check_intr_pin(struct gpiopic *gpiopic, uint32_t pin)
{
	struct gpiopic_intsrc *gisrc;

	if (gpiopic == NULL)
		return (ENOENT);
	if (pin >= gpiopic->sc->sc_npins)
		return (ENOENT);
	gisrc = &gpiopic->intr_srcs[pin];
	if (gisrc->is_event == NULL)
		return (ENOENT);
	return (0);
}

static int
gpiopic_register_sources(struct gpiopic *gpiopic)
{
	char name[GPIOMAXNAME];
	struct gpiobus_softc *sc = gpiopic->sc;
	struct gpiopic_intsrc *gisrc;
	int i, count;
	int err;

	count = 0;
	for (i = 0; i < sc->sc_npins; i++) {
		uint32_t pincaps;

		err = GPIO_PIN_GETCAPS(sc->sc_dev, i, &pincaps);
		if (err != 0)
			continue;
		if ((pincaps & GPIO_INTR_MASK) == GPIO_INTR_NONE)
			continue;

		gisrc = &gpiopic->intr_srcs[i];
		gisrc->is_pic = gpiopic;
		gisrc->is_pin = i;
		gisrc->is_count = &gpiopic->intr_counts[i * 2];
		gisrc->is_straycount = &gpiopic->intr_counts[i * 2 + 1];
		gisrc->is_enabled = 0;
		gisrc->is_mode = GPIO_INTR_CONFORM;

		/*
		 * We use IE_BUS_PRIV to indicate that this interrupt event is
		 * completely private to this bus.  It's managed by the bus and
		 * it is not visible to the global interrupt management.
		 * As a consequence, its number / vector is in the private
		 * space and is meaningless in the global space.
		 */
		(void)gpiobus_pin_getname(gpiopic->sc->sc_busdev, i, name);
		err = intr_event_create(&gisrc->is_event, gisrc, IE_BUS_PRIV,
		    i,
		    gpiopic_mask_intr,		/* pre_ithread */
		    gpiopic_unmask_intr,	/* post_ithread */
		    gpiopic_eoi,		/* post_filter */
		    gpiopic_assign_cpu,
		    "%s", name);
		if (err != 0) {
			device_printf(sc->sc_busdev, "gpiopic failed to "
			    "create interrupt event for pin %u: %d\n", i, err);
			continue;
		}
		count++;
	}
	return (count);
}

static void
gpiobus_pic_init(struct gpiobus_softc *sc)
{

	struct sysctl_ctx_list *ctx;
	struct sysctl_oid *tree_node;
	struct sysctl_oid_list *tree;
	struct gpiopic *gpiopic;
	int i, count;
	int err;

	for (i = 0; i < sc->sc_npins; i++) {
		uint32_t pincaps;

		err = GPIO_PIN_GETCAPS(sc->sc_dev, i, &pincaps);
		if (err != 0)
			continue;
		if ((pincaps & GPIO_INTR_MASK) != GPIO_INTR_NONE)
			break;
	}
	if (i == sc->sc_npins)
		return;

	gpiopic = malloc(sizeof(struct gpiopic), M_DEVBUF, M_NOWAIT | M_ZERO);
	if (gpiopic == NULL) {
		device_printf(sc->sc_busdev, "gpiopic allocation failed\n");
		return;
	}

	gpiopic->intr_srcs = malloc(sc->sc_npins *
	    sizeof(struct gpiopic_intsrc), M_DEVBUF, M_NOWAIT | M_ZERO);
	if (gpiopic->intr_srcs == NULL) {
		free(gpiopic, M_DEVBUF);
		device_printf(sc->sc_busdev, "gpiopic allocation failed\n");
		return;
	}

	/* Two interrupt counters per pin: total and stray. */
	gpiopic->intr_counts = malloc(2 * sc->sc_npins *
	    sizeof(*gpiopic->intr_counts), M_DEVBUF, M_NOWAIT | M_ZERO);
	if (gpiopic->intr_counts == NULL) {
		free(gpiopic->intr_srcs, M_DEVBUF);
		free(gpiopic, M_DEVBUF);
		device_printf(sc->sc_busdev, "gpiopic allocation failed\n");
		return;
	}

	sx_init(&gpiopic->intrsrc_lock, "gpiopic lock");
	gpiopic->sc = sc;
	count = gpiopic_register_sources(gpiopic);
	sc->sc_pic = gpiopic;
	device_printf(sc->sc_busdev, "initialized %d interrupt sources\n",
	    count);

	ctx = device_get_sysctl_ctx(sc->sc_busdev);
	tree_node = device_get_sysctl_tree(sc->sc_busdev);
	tree = SYSCTL_CHILDREN(tree_node);
	SYSCTL_ADD_PROC(ctx, tree, OID_AUTO, "interrupts",
	    CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_MPSAFE,
	    gpiopic, 0, sysctl_gpio_intrs, "A",
	    "interrupt:pin (current mode): count");
}

static void
gpiobus_pic_destroy(struct gpiopic *gpiopic)
{
	struct gpiopic_intsrc *gisrc;
	int i;

	if (gpiopic == NULL)
		return;

	sx_xlock(&gpiopic->intrsrc_lock);
	for (i = 0; i < gpiopic->sc->sc_npins; i++) {
		gisrc = &gpiopic->intr_srcs[i];
		if (gisrc->is_event == NULL)
			continue;
		/* XXX what to do if there is a busy event? */
		(void)intr_event_destroy(gisrc->is_event);
	}
	sx_xunlock(&gpiopic->intrsrc_lock);

	sx_destroy(&gpiopic->intrsrc_lock);
	free(gpiopic->intr_counts, M_DEVBUF);
	free(gpiopic->intr_srcs, M_DEVBUF);
	free(gpiopic, M_DEVBUF);
}

static void
gpiobus_pic_resume(struct gpiopic *gpiopic)
{
	struct gpiopic_intsrc *gisrc;
	int i;

	if (gpiopic == NULL)
		return;

	sx_xlock(&gpiopic->intrsrc_lock);
	for (i = 0; i < gpiopic->sc->sc_npins; i++) {
		gisrc = &gpiopic->intr_srcs[i];
		if (gisrc->is_event == NULL)
			continue;

		/* Just in case. */
		gpiopic_disable_intr(gisrc);

		if (!gisrc->is_enabled)
			continue;

		if (gisrc->is_mode != GPIO_INTR_CONFORM)
			gpiopic_config_intr(gisrc, gisrc->is_mode);
		if (gisrc->is_masked)
			gpiopic_mask_intr(gisrc);
		else
			gpiopic_unmask_intr(gisrc);
		gpiopic_enable_intr(gisrc);
	}
	sx_xunlock(&gpiopic->intrsrc_lock);
}

#define	GPIO_MAX_STRAY	10

void
gpiobus_handle_intr(device_t busdev, uint32_t pin)
{
	struct gpiobus_softc *sc;
	struct gpiopic *gpiopic;
	struct gpiopic_intsrc *gisrc;
	struct intr_event *ie;

	sc = device_get_softc(busdev);
	gpiopic = sc->sc_pic;

	KASSERT(pin < sc->sc_npins,
	    ("%s for unsupported pin %u", __func__, pin));
	gisrc = &gpiopic->intr_srcs[pin];
	ie = gisrc->is_event;

	(*gisrc->is_count)++;

	/*
	 * For stray interrupts, mask the source, bump the
	 * stray count, and log the condition.
	 */
	if (intr_event_handle(ie, curthread->td_intr_frame) != 0) {
		gpiopic_mask_intr(gisrc);
		gpiopic_eoi(gisrc);
		(*gisrc->is_straycount)++;
		if (*gisrc->is_straycount < GPIO_MAX_STRAY) {
			log(LOG_ERR, "stray irq on pin %u\n", pin);
		} else if (*gisrc->is_straycount == GPIO_MAX_STRAY) {
			log(LOG_CRIT, "too many stray irq's on pin %u: "
			    "not logging anymore\n", pin);
		}
	}
}

static int
gpiopic_add_handler(const char *name, struct gpiopic *gpiopic, uint32_t pin,
    driver_filter_t filter, driver_intr_t handler,
    void *arg, enum intr_type flags, void **cookiep)
{
	struct gpiopic_intsrc *gisrc;
	int err;

	KASSERT(gpiopic_check_intr_pin(gpiopic, pin) == 0,
	    ("setup_intr for unsupported pin %u", pin));
	gisrc = &gpiopic->intr_srcs[pin];
	err = intr_event_add_handler(gisrc->is_event,
	    name, filter, handler, arg,
	    intr_priority(flags), flags, cookiep);
	if (err == 0) {
		sx_xlock(&gpiopic->intrsrc_lock);
		gisrc->is_handlers++;
		if (gisrc->is_handlers == 1) {
			gpiopic_enable_intr(gisrc);
			gpiopic_unmask_intr(gisrc);	/* unmask source */
		}
		sx_xunlock(&gpiopic->intrsrc_lock);
	}
	return (err);
}
int
gpiobus_setup_intr(device_t bus, device_t dev, struct resource *irq,
		 int flags, driver_filter_t filter, void (*ihand)(void *),
		 void *arg, void **cookiep);

int
gpiobus_setup_intr(device_t bus, device_t dev, struct resource *irq,
		 int flags, driver_filter_t filter, void (*ihand)(void *),
		 void *arg, void **cookiep)
{
	struct gpiobus_softc *sc;
	int err;

	printf("%s %d\n", __func__, __LINE__);

	sc = device_get_softc(bus);
	if (!rman_is_region_manager(irq, &sc->sc_intr_rman))
		return (EINVAL);
	printf("%s %d\n", __func__, __LINE__);

	*cookiep = NULL;
	if ((rman_get_flags(irq) & RF_SHAREABLE) == 0)
		flags |= INTR_EXCL;
	printf("%s %d\n", __func__, __LINE__);

	err = rman_activate_resource(irq);
	if (err != 0)
		return (err);
	printf("%s %d\n", __func__, __LINE__);

	err = gpiopic_add_handler(device_get_nameunit(dev), sc->sc_pic,
	    rman_get_start(irq), filter, ihand, arg, flags, cookiep);
	printf("%s %d\n", __func__, __LINE__);
	return (err);
}

static int
gpiopic_remove_handler(void *cookie)
{
	struct gpiopic *gpiopic;
	struct gpiopic_intsrc *gisrc;
	int err;

	gisrc = intr_handler_source(cookie);
	if (gisrc == NULL)
		return (EINVAL);

	gpiopic = gisrc->is_pic;
	err = intr_event_remove_handler(cookie);
	if (err == 0) {
		sx_xlock(&gpiopic->intrsrc_lock);
		gisrc->is_handlers--;
		if (gisrc->is_handlers == 0) {
			gpiopic_mask_intr(gisrc);	/* mask source */
			gpiopic_disable_intr(gisrc);
		}
		sx_xunlock(&gpiopic->intrsrc_lock);
	}
	return (err);
}

static int
gpiobus_teardown_intr(device_t bus, device_t dev, struct resource *r,
    void *ih)
{
	struct gpiobus_softc *sc;
	int err;

	sc = device_get_softc(bus);
	if (!rman_is_region_manager(r, &sc->sc_intr_rman))
		return (EINVAL);
	err = gpiopic_remove_handler(ih);
	return (err);
}

static int
gpiopic_describe(void *ih, const char *descr)
{
	struct gpiopic_intsrc *gisrc;
	int err;

	gisrc = intr_handler_source(ih);
	if (gisrc == NULL)
		return (EINVAL);
	err = intr_event_describe_handler(gisrc->is_event, ih, descr);
	return (err);
}

static int
gpiobus_describe_intr(device_t bus, device_t child, struct resource *irq,
    void *cookie, const char *descr)
{
	struct gpiobus_softc *sc;
	int err;

	sc = device_get_softc(bus);
	if (!rman_is_region_manager(irq, &sc->sc_intr_rman))
		return (EINVAL);
	err = gpiopic_describe(cookie, descr);
	return (err);
}

static int
gpiobus_config_intr(device_t dev, int irq, enum intr_trigger trig,
    enum intr_polarity pol)
{
	return (EOPNOTSUPP);
}

struct resource *
gpio_alloc_intr_resource(device_t consumer_dev, int rid, u_int alloc_flags,
    gpio_pin_t pin, uint32_t intr_mode)
{
	struct resource *res;
	struct gpiobus_softc *sc;
	struct gpiopic_intsrc *gisrc;
	device_t busdev;
	uint32_t caps;
	int err;

	switch (intr_mode) {
	case GPIO_INTR_EDGE_FALLING:
	case GPIO_INTR_EDGE_RISING:
	case GPIO_INTR_EDGE_BOTH:
	case GPIO_INTR_LEVEL_LOW:
	case GPIO_INTR_LEVEL_HIGH:
	case GPIO_INTR_CONFORM:
		break;
	default:
		device_printf(consumer_dev,
		    "invalid interrupt mode 0x%08x\n", intr_mode);
		return (NULL);
	}

	err = GPIO_PIN_GETCAPS(pin->dev, pin->pin, &caps);
	if (err != 0) {
		device_printf(consumer_dev,
		    "cannot get pin %u capabilities\n", pin->pin);
		return (NULL);
	}
	if ((intr_mode & caps) == 0) {
		device_printf(consumer_dev,
		    "pin %u does not support interrupt mode 0x%08x "
		    "(caps 0x%08x)\n", pin->pin, intr_mode, caps);
		return (NULL);
	}

	busdev = GPIO_GET_BUS(pin->dev);
	sc = device_get_softc(busdev);
	err = gpiopic_check_intr_pin(sc->sc_pic, pin->pin);
	if (err != 0) {
		device_printf(busdev, "PIC not set up or bad pin %u\n",
		    pin->pin);
		return (NULL);
	}

	KASSERT(sc->sc_pins[pin->pin].mapped,
	    ("%s: unmapped pin %u", __func__, pin->pin));
	res = BUS_ALLOC_RESOURCE(busdev, consumer_dev, SYS_RES_IRQ, rid,
	    pin->pin, pin->pin, 1, alloc_flags);
	printf("allocated pin %u as irq %d %p\n", pin->pin, rid, res);
	if (res != NULL && intr_mode != GPIO_INTR_CONFORM) {
		gisrc = &sc->sc_pic->intr_srcs[pin->pin];
		gpiopic_config_intr(gisrc, intr_mode);
	}
	return (res);
}
#endif /* !INTRNG */

static device_method_t gpiobus_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,		gpiobus_probe),
	DEVMETHOD(device_attach,	gpiobus_attach),
	DEVMETHOD(device_detach,	gpiobus_detach),
	DEVMETHOD(device_shutdown,	bus_generic_shutdown),
	DEVMETHOD(device_suspend,	gpiobus_suspend),
	DEVMETHOD(device_resume,	gpiobus_resume),

	/* Bus interface */
#ifdef INTRNG
	DEVMETHOD(bus_setup_intr,	bus_generic_setup_intr),
	DEVMETHOD(bus_config_intr,	bus_generic_config_intr),
	DEVMETHOD(bus_teardown_intr,	bus_generic_teardown_intr),
#else
	DEVMETHOD(bus_setup_intr,	gpiobus_setup_intr),
	DEVMETHOD(bus_config_intr,	gpiobus_config_intr),
	DEVMETHOD(bus_teardown_intr,	gpiobus_teardown_intr),
#endif
	DEVMETHOD(bus_delete_resource,	bus_generic_rl_delete_resource),
	DEVMETHOD(bus_get_resource,	bus_generic_rl_get_resource),
	DEVMETHOD(bus_set_resource,	bus_generic_rl_set_resource),
	DEVMETHOD(bus_alloc_resource,	gpiobus_alloc_resource),
	DEVMETHOD(bus_release_resource,	gpiobus_release_resource),
	DEVMETHOD(bus_activate_resource,	bus_generic_rman_activate_resource),
	DEVMETHOD(bus_deactivate_resource,	bus_generic_rman_deactivate_resource),
	DEVMETHOD(bus_get_resource_list,	gpiobus_get_resource_list),
	DEVMETHOD(bus_get_rman,		gpiobus_get_rman),
	DEVMETHOD(bus_add_child,	gpiobus_add_child),
	DEVMETHOD(bus_child_deleted,	gpiobus_child_deleted),
	DEVMETHOD(bus_rescan,		gpiobus_rescan),
	DEVMETHOD(bus_probe_nomatch,	gpiobus_probe_nomatch),
	DEVMETHOD(bus_print_child,	gpiobus_print_child),
	DEVMETHOD(bus_child_location,	gpiobus_child_location),
	DEVMETHOD(bus_hinted_child,	gpiobus_hinted_child),
	DEVMETHOD(bus_read_ivar,        gpiobus_read_ivar),
	DEVMETHOD(bus_write_ivar,       gpiobus_write_ivar),

	/* GPIO protocol */
	DEVMETHOD(gpiobus_acquire_bus,	gpiobus_acquire_bus),
	DEVMETHOD(gpiobus_release_bus,	gpiobus_release_bus),
	DEVMETHOD(gpiobus_pin_getflags,	gpiobus_pin_getflags),
	DEVMETHOD(gpiobus_pin_getcaps,	gpiobus_pin_getcaps),
	DEVMETHOD(gpiobus_pin_setflags,	gpiobus_pin_setflags),
	DEVMETHOD(gpiobus_pin_get,	gpiobus_pin_get),
	DEVMETHOD(gpiobus_pin_set,	gpiobus_pin_set),
	DEVMETHOD(gpiobus_pin_toggle,	gpiobus_pin_toggle),
	DEVMETHOD(gpiobus_pin_access_32,gpiobus_pin_access_32),
	DEVMETHOD(gpiobus_pin_config_32,gpiobus_pin_config_32),
	DEVMETHOD(gpiobus_pin_getname,	gpiobus_pin_getname),
	DEVMETHOD(gpiobus_pin_setname,	gpiobus_pin_setname),

	DEVMETHOD_END
};

driver_t gpiobus_driver = {
	"gpiobus",
	gpiobus_methods,
	sizeof(struct gpiobus_softc)
};

EARLY_DRIVER_MODULE(gpiobus, gpio, gpiobus_driver, 0, 0,
    BUS_PASS_BUS + BUS_PASS_ORDER_MIDDLE);
MODULE_VERSION(gpiobus, 1);
