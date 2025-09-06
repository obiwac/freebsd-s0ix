#-
# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (c) 2025 The FreeBSD Foundation
#
# This software was developed by Aymeric Wibo <obiwac@freebsd.org>
# under sponsorship from the FreeBSD Foundation.
#

#include <sys/rman.h>

INTERFACE gpio_intr;

#
# Pass an already allocated GPIO interrupt resource to a GPIO interrupt
# consumer for it to use.
#
# If the consumer successfully set up the interrupt, it takes ownership of the
# resource and the caller must not free it.  On failure, the caller retains
# ownership of the resource and must free it.
#
METHOD int give {
	device_t dev;
	device_t intr_dev;
	struct resource *intr_res;
};
