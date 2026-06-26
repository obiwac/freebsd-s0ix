/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The FreeBSD Foundation
 *
 * This software was developed by Aymeric Wibo <obiwac@freebsd.org>
 * under sponsorship from the FreeBSD Foundation.
 */

/*
 * On x86, devices cannot use INTRNG like on ARM platforms to consume
 * interrupts from interrupt sources which are cousins to it rather than its
 * parent.
 *
 * This file provides a mechanism for such devices to advertise that they are
 * looking for an interrupt source with a given ACPI handle. When an interrupt
 * source device with that ACPI handle attaches, it will call back to the
 * interrupt consumer device to set up interrupts.
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/systm.h>
#include <sys/queue.h>

#include <contrib/dev/acpica/include/acpi.h>
#include <contrib/dev/acpica/include/accommon.h>

#include <dev/acpica/acpivar.h>

#ifdef INTRNG
# error "INTRNG supported; you should not be using this!"
#endif

struct acpi_cousin_intr_req {
	TAILQ_ENTRY(acpi_cousin_intr_req)	link;
	device_t			consumer;
	acpi_cousin_intr_setup_cb_t	setup_cb;
	acpi_cousin_intr_detach_cb_t	detach_cb;
	acpi_cousin_intr_intr_cb_t	intr_cb;
};

struct acpi_cousin_intr_consumer {
	device_t			device;
	acpi_cousin_intr_intr_cb_t	intr_cb;
	acpi_cousin_intr_detach_cb_t	detach_cb;
};

struct acpi_cousin_intr_consumers {
	size_t					count;
	struct acpi_cousin_intr_consumer	*consumers;
};

/*
 * We don't actually need to have a separate lock for this as this will only
 * ever be touched during attach/detach routines of drivers (so bus_topo_lock
 * should always be held).
 */
TAILQ_HEAD(, acpi_cousin_intr_req) acpi_cousin_intr_reqs =
    TAILQ_HEAD_INITIALIZER(acpi_cousin_intr_reqs);

/*
 * Called by the interrupt consumer device to advertise that we are looking for
 * cousin interrupt sources.
 */
device_t
acpi_cousin_intr_req(device_t consumer,
    acpi_cousin_intr_setup_cb_t setup_cb,
    acpi_cousin_intr_detach_cb_t detach_cb,
    acpi_cousin_intr_intr_cb_t intr_cb)
{
	struct acpi_cousin_intr_req *reqp;

	bus_topo_assert();

	struct acpi_cousin_intr_req req = {
		.consumer = consumer,
		.setup_cb = setup_cb,
		.detach_cb = detach_cb,
		.intr_cb = intr_cb,
	};

	reqp = malloc(sizeof(req), M_TEMP, M_WAITOK);
	*reqp = req;

	TAILQ_INSERT_TAIL(&acpi_cousin_intr_reqs, reqp, link);
	/*
	 * TODO I would like to immediately return something if we find a
	 * cousin interrupt source straight away.
	 */
	return (NULL);
}

/*
 * Called when a driver knows it should act as an interrupt source for a
 * device.  A driver could be aware of this by e.g. seeing that a device has a
 * _CRS in it's ACPI descriptor.  We could have multiple consumers, which is
 * why we accept an existing consumer list.
 */
struct acpi_cousin_intr_consumers *
acpi_cousin_intr_provide(struct acpi_cousin_intr_consumers *cons_list,
    ACPI_HANDLE cons_handle)
{
	struct acpi_cousin_intr_req *req;
	struct acpi_cousin_intr_consumer *cons;
	device_t cons_dev;

	bus_topo_assert();

	cons_dev = acpi_get_device(cons_handle);
	if (cons_dev == NULL)
		goto done;

	if (cons_list == NULL)
		cons_list = malloc(sizeof(*cons_list),
		    M_DEVBUF, M_WAITOK | M_ZERO);

	TAILQ_FOREACH(req, &acpi_cousin_intr_reqs, link) {
		printf("acpi cousin intr request: %s, %s\n", device_get_nameunit(req->consumer), device_get_nameunit(cons_dev));

		/* Remove requests from devices no longer attached. */
		if (!device_is_attached(req->consumer)) {
			TAILQ_REMOVE(&acpi_cousin_intr_reqs, req, link);
			free(req, M_TEMP);
			continue;
		}

		if (req->consumer != cons_dev)
			continue;
		req->setup_cb(cons_dev);

		cons_list->consumers = realloc(cons_list->consumers,
		    ++cons_list->count * sizeof(*cons_list->consumers),
		    M_DEVBUF, M_WAITOK);
		cons = &cons_list->consumers[cons_list->count - 1];

		cons->device = cons_dev;
		cons->intr_cb = req->intr_cb;
		cons->detach_cb = req->detach_cb;

		/*
		 * TODO Should we actually remove requests?  What if the
		 * interrupt source detaches then reattaches?  Should we
		 * just have the consumer re-log a request for a cousin
		 * interrupt source on some "de"-setup callback?
		 */
		TAILQ_REMOVE(&acpi_cousin_intr_reqs, req, link);
		free(req, M_TEMP);
		/* We should not have more than one request per consumer. */
		break;
	}

done:
	if (cons_list != NULL && cons_list->count == 0) {
		free(cons_list, M_DEVBUF);
		return (NULL);
	}
	return (cons_list);
}

/*
 * Called by the cousin interrupt source device when it is unable to provide
 * interrupts anymore.
 *
 * It is assumed that it cannot trigger interrupts while detaching.
 *
 * We should hold the bus topology lock because the detach callback may well
 * want to call acpi_cousin_intr_req() again.
 */
void
acpi_cousin_intr_detach(struct acpi_cousin_intr_consumers *cons_list)
{
	struct acpi_cousin_intr_consumer *cons;

	bus_topo_assert();

	for (size_t i = 0; i < cons_list->count; i++) {
		cons = &cons_list->consumers[i];
		cons->detach_cb(cons->device);
	}
}

/*
 * Called by interrupt source on its consumer list when interrupt is triggered.
 */
void
acpi_cousin_intr_trigger(struct acpi_cousin_intr_consumers *cons_list)
{
	struct acpi_cousin_intr_consumer *cons;

	for (size_t i = 0; i < cons_list->count; i++) {
		cons = &cons_list->consumers[i];
		cons->intr_cb(cons->device);
	}
}

void
acpi_cousin_intr_free_cons(struct acpi_cousin_intr_consumers *cons_list)
{
	free(cons_list->consumers, M_DEVBUF);
}
