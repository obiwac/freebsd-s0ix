/*
 * Copyright (c) 2026 Baptiste Daroussin <bapt@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Service jail management.  Creates lightweight jails for service
 * isolation using jail(2) directly.
 */

#include <sys/param.h>
#include <sys/jail.h>

#include <jail.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rcd.h"

/*
 * Map option names to jail parameter strings.
 */
static const struct {
	const char	*name;
	const char	*param;
	const char	*value;
} jail_option_map[] = {
	{ "netv4",		"ip4",			"new" },
	{ "netv6",		"ip6",			"new" },
	{ "mlock",		"allow.mlock",		"true" },
	{ "sysvipc",		"allow.sysvipc",	"true" },
	{ "allow.routing",	"allow.socket_af",	"true" },
	{ "vmm",		"allow.vmm",		"true" },
	{ NULL, NULL, NULL }
};

/*
 * Create a jail for the service.
 */
int
jail_svc_create(struct unit *u)
{
	struct jailparam *params;
	int nparams, maxparams;
	char namebuf[64];
	int jid, i, j;

	if (!u->u_jail.jc_enable)
		return (0);

	/* Generate jail name if not specified */
	if (u->u_jail.jc_name == NULL) {
		snprintf(namebuf, sizeof(namebuf), "svcj-%s", u->u_name);
		u->u_jail.jc_name = xstrdup(namebuf);
	}

	/* Allocate params array sized for all possible entries */
	maxparams = 5 +
	    (int)u->u_jail.jc_options.len +
	    (int)u->u_jail.jc_ip4addr.len +
	    (int)u->u_jail.jc_ip6addr.len;
	params = xcalloc(maxparams, sizeof(*params));
	nparams = 0;

	/* Jail name */
	jailparam_init(&params[nparams], "name");
	jailparam_import(&params[nparams], u->u_jail.jc_name);
	nparams++;

	/* Path (default: /) */
	jailparam_init(&params[nparams], "path");
	jailparam_import(&params[nparams],
	    u->u_jail.jc_path != NULL ? u->u_jail.jc_path : "/");
	nparams++;

	/* Inherit host */
	jailparam_init(&params[nparams], "host");
	jailparam_import(&params[nparams], "inherit");
	nparams++;

	/* No devfs by default */
	if (!u->u_jail.jc_devfs) {
		jailparam_init(&params[nparams], "mount.nodevfs");
		jailparam_import(&params[nparams], "true");
		nparams++;
	}

	/* persist so the jail survives between operations */
	jailparam_init(&params[nparams], "persist");
	jailparam_import(&params[nparams], "true");
	nparams++;

	/* Apply option map */
	for (i = 0; i < (int)u->u_jail.jc_options.len; i++) {
		for (j = 0; jail_option_map[j].name != NULL; j++) {
			if (strcmp(
			    u->u_jail.jc_options.d[i],
			    jail_option_map[j].name) == 0) {
				jailparam_init(&params[nparams],
				    jail_option_map[j].param);
				jailparam_import(&params[nparams],
				    jail_option_map[j].value);
				nparams++;
				break;
			}
		}
	}

	/* IPv4 addresses */
	for (i = 0; i < (int)u->u_jail.jc_ip4addr.len; i++) {
		jailparam_init(&params[nparams], "ip4.addr");
		jailparam_import(&params[nparams],
		    u->u_jail.jc_ip4addr.d[i]);
		nparams++;
	}

	/* IPv6 addresses */
	for (i = 0; i < (int)u->u_jail.jc_ip6addr.len; i++) {
		jailparam_init(&params[nparams], "ip6.addr");
		jailparam_import(&params[nparams],
		    u->u_jail.jc_ip6addr.d[i]);
		nparams++;
	}

	/* Create the jail */
	jid = jailparam_set(params, nparams, JAIL_CREATE);

	jailparam_free(params, nparams);
	free(params);

	if (jid < 0) {
		log_warn("%s: jail creation failed: %s",
		    u->u_name, strerror(errno));
		return (-1);
	}

	u->u_jail.jc_jid = jid;
	log_info("%s: created jail %s (jid %d)",
	    u->u_name, u->u_jail.jc_name, jid);
	return (0);
}

/*
 * Destroy a service jail.
 */
int
jail_svc_destroy(struct unit *u)
{
	struct jailparam params[1];
	int jid;

	if (u->u_jail.jc_jid <= 0)
		return (0);

	jailparam_init(&params[0], "name");
	jailparam_import(&params[0], u->u_jail.jc_name);

	jid = jailparam_set(params, 1, JAIL_DYING);
	jailparam_free(params, 1);

	if (jid < 0 && errno != ENOENT) {
		log_warn("%s: jail destroy failed: %s",
		    u->u_name, strerror(errno));
		return (-1);
	}

	log_info("%s: destroyed jail %s", u->u_name, u->u_jail.jc_name);
	u->u_jail.jc_jid = 0;
	return (0);
}
