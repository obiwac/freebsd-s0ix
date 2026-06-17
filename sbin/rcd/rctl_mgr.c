/*
 * Copyright (c) 2026 Baptiste Daroussin <bapt@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Resource control integration.  Applies rctl(2) rules to services
 * based on their unit configuration, targeting either a jail subject
 * (when service jails are enabled) or a process subject.
 */

#include <sys/param.h>
#include <sys/procctl.h>
#include <sys/rctl.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rcd.h"

/*
 * Check if RACCT/RCTL is available in the running kernel.
 */
bool
rctl_available(void)
{
	char outbuf[64];

	/*
	 * Try a no-op query.  If RCTL is not compiled in, the syscall
	 * returns ENOSYS.
	 */
	if (rctl_get_rules(":::", 4, outbuf, sizeof(outbuf)) < 0 &&
	    errno == ENOSYS)
		return (false);
	return (true);
}

/*
 * Apply rctl rules for a service.
 * Returns 0 on success, -1 on fatal error (RCTL not available).
 */
int
rctl_apply(struct unit *u)
{
	struct rctl_conf *rc;
	struct rctl_active *ra;
	char rule[256];
	char outbuf[128];
	const char *subject_type, *subject_id;
	char pidbuf[32];

	if (STAILQ_EMPTY(&u->u_rctl))
		return (0);

	if (!rctl_available()) {
		log_warn("%s: RACCT/RCTL not available in kernel", u->u_name);
		return (-1);
	}

	/* Determine subject: jail or process */
	if (u->u_jail.jc_enable && u->u_jail.jc_name != NULL) {
		subject_type = "jail";
		subject_id = u->u_jail.jc_name;
	} else {
		subject_type = "process";
		snprintf(pidbuf, sizeof(pidbuf), "%d", u->u_pid);
		subject_id = pidbuf;
	}

	STAILQ_FOREACH(rc, &u->u_rctl, rc_entries) {
		if ((size_t)snprintf(rule, sizeof(rule), "%s:%s:%s:%s=%s",
		    subject_type, subject_id,
		    rc->rc_resource, rc->rc_action,
		    rc->rc_amount) >= sizeof(rule)) {
			log_warn("%s: rctl rule too long", u->u_name);
			continue;
		}

		if (rctl_add_rule(rule, strlen(rule) + 1,
		    outbuf, sizeof(outbuf)) != 0) {
			log_warn("%s: rctl_add_rule(%s): %s",
			    u->u_name, rule, strerror(errno));
			continue;
		}

		/* Track active rule for cleanup */
		ra = xcalloc(1, sizeof(*ra));
		strlcpy(ra->ra_rule, rule, sizeof(ra->ra_rule));
		STAILQ_INSERT_TAIL(&u->u_rctl_active, ra, ra_entries);

		log_debug("%s: rctl rule applied: %s", u->u_name, rule);
	}

	return (0);
}

/*
 * Remove all active rctl rules for a service.
 */
void
rctl_remove(struct unit *u)
{
	struct rctl_active *ra, *ra_tmp;
	char outbuf[128];

	STAILQ_FOREACH_SAFE(ra, &u->u_rctl_active, ra_entries, ra_tmp) {
		rctl_remove_rule(ra->ra_rule, strlen(ra->ra_rule) + 1,
		    outbuf, sizeof(outbuf));
		STAILQ_REMOVE(&u->u_rctl_active, ra, rctl_active, ra_entries);
		free(ra);
	}
}

/*
 * Collect resource usage for all PIDs under a subreaper process.
 *
 * rcd-exec (the subreaper) runs as a child of rcd and tracks forking
 * daemons and legacy scripts.  Given the PID of rcd-exec (u->u_pid),
 * use PROC_REAP_GETPIDS to discover all descendant PIDs (daemon,
 * workers, etc.) and query RACCT for each one.
 *
 * Returns 0 on success, -1 on failure.
 */
static int
rctl_get_usage_reaper(struct unit *u, char *outbuf, size_t outlen)
{
	struct procctl_reaper_status rs;
	struct procctl_reaper_pids rp;
	struct procctl_reaper_pidinfo *pids;
	char filter[32];
	char racctbuf[4096];
	char pidbuf[32];
	size_t pos;
	unsigned int i;

	pos = 0;

	/* Check if this process is a subreaper */
	if (procctl(P_PID, u->u_pid, PROC_REAP_STATUS, &rs) != 0)
		return (-1);

	if (rs.rs_descendants == 0)
		return (-1);

	pids = calloc(rs.rs_descendants, sizeof(*pids));
	if (pids == NULL)
		return (-1);

	rp.rp_count = rs.rs_descendants;
	rp.rp_pids = pids;

	if (procctl(P_PID, u->u_pid, PROC_REAP_GETPIDS, &rp) != 0) {
		free(pids);
		return (-1);
	}

	for (i = 0; i < rp.rp_count; i++) {
		const char *p;
		char *copy, *entry, *tofree;
		size_t slen;

		snprintf(filter, sizeof(filter), "process:%d",
		    pids[i].pi_pid);
		if (rctl_get_racct(filter,
		    strlen(filter) + 1, racctbuf, sizeof(racctbuf)) != 0) {
			log_debug("%s: reaper child %d rctl_get_racct: %s",
			    u->u_name, pids[i].pi_pid, strerror(errno));
			continue;
		}

		/* Skip empty racct responses */
		if (racctbuf[0] == '\0')
			continue;

		/* Prefix with PID label */
		snprintf(pidbuf, sizeof(pidbuf), "pid %d:\n",
		    pids[i].pi_pid);
		if (pos + strlen(pidbuf) + strlen(racctbuf) + 1 >= outlen)
			break;
		pos += strlcpy(outbuf + pos, pidbuf, outlen - pos);

		/* Indent each comma-separated entry on its own line */
		tofree = copy = strdup(racctbuf);
		if (copy == NULL)
			continue;
		while ((entry = strsep(&copy, ",")) != NULL) {
			if (*entry == '\0')
				continue;
			/* Strip the "process:PID:" prefix (3 colon-separated fields) */
			p = entry;
			for (int colons = 0; colons < 3; colons++) {
				while (*p != '\0' && *p != ':')
					p++;
				if (*p == ':')
					p++;
			}
			slen = strlen(p);
			if (pos + 4 + slen + 1 >= outlen)
				break;
			pos += snprintf(outbuf + pos, outlen - pos,
			    "  %s\n", *p != '\0' ? p : entry);
		}
		free(tofree);
	}

	free(pids);
	return (pos > 0 ? 0 : -1);
}

/*
 * Query resource usage for a service (for rcctl resources).
 * Returns 0 on success, -1 on failure.  On failure, outbuf contains
 * a human-readable reason if outlen is large enough.
 */
int
rctl_get_usage(struct unit *u, char *outbuf, size_t outlen)
{
	char filter[128];

	if (!rctl_available()) {
		snprintf(outbuf, outlen,
		    "resource usage tracking (RACCT/RCTL) is not "
		    "available in the running kernel");
		return (-1);
	}

	if (u->u_jail.jc_enable && u->u_jail.jc_name != NULL) {
		if ((size_t)snprintf(filter, sizeof(filter), "jail:%s",
		    u->u_jail.jc_name) >= sizeof(filter)) {
			snprintf(outbuf, outlen, "jail name too long");
			return (-1);
		}
	} else if (u->u_pid > 0) {
		/*
		 * First try to find descendant PIDs under the subreaper
		 * (rcd-exec).  For legacy scripts and forking daemons,
		 * u->u_pid is the subreaper PID, not the service itself.
		 * RACCT stats are attached to the actual daemon process(es).
		 */
		if (rctl_get_usage_reaper(u, outbuf, outlen) == 0)
			return (0);
		/* Fallback: query the tracked PID directly */
		snprintf(filter, sizeof(filter), "process:%d",
		    u->u_pid);
	} else {
		snprintf(outbuf, outlen,
		    "service not running");
		return (-1);
	}

	if (rctl_get_racct(filter, strlen(filter) + 1,
	    outbuf, outlen) != 0) {
		snprintf(outbuf, outlen,
		    "cannot read resource usage: %s", strerror(errno));
		return (-1);
	}

	return (0);
}
