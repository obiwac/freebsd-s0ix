/*
 * Copyright (c) 2026 Baptiste Daroussin <bapt@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * rcctl — Control utility for rcd(8).
 *
 * Communicates with the rcd daemon via a UNIX domain socket using
 * UCL-encoded messages.  Provides a CLI for managing services.
 *
 * Usage:
 *   rcctl status [service]
 *   rcctl start <service>
 *   rcctl stop <service>
 *   rcctl restart <service>
 *   rcctl reload <service>
 *   rcctl enable <service>
 *   rcctl disable <service>
 *   rcctl resources <service>
 *   rcctl deps <service>
 *   rcctl list
 */

#include <sys/param.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <netinet/in.h>

#include <err.h>
#include <errno.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>

#include <ucl.h>

#include "xio.h"

#define RCD_CONTROL_SOCK	"/var/run/rcd.sock"

/* Command name constants — shared vocabulary with rcd(8). */
static const char cmd_status[]  = "status";
static const char cmd_list[]    = "list";
static const char cmd_suspend[] = "suspend";
static const char cmd_resume[]  = "resume";
static const char cmd_show[]    = "show";
static const char cmd_resources[] = "resources";

static const char *control_socket = RCD_CONTROL_SOCK;

/*
 * Connect to the rcd control socket.
 */
static int
ctl_connect(void)
{
	struct sockaddr_un sun;
	int fd;

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		err(1, "socket");

	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_UNIX;
	strlcpy(sun.sun_path, control_socket, sizeof(sun.sun_path));

	/*
	 * Retry connect(2) on EINTR.  For a blocking UNIX socket, EINTR
	 * means the system call was interrupted before the connection
	 * completed — simply retry.
	 */
	while (connect(fd, (struct sockaddr *)&sun, SUN_LEN(&sun)) != 0) {
		if (errno == EINTR)
			continue;
		err(1, "connect to %s", control_socket);
	}

	return (fd);
}

/*
 * Send a UCL request and read the response.
 */
static ucl_object_t *
ctl_request(int fd, const ucl_object_t *req)
{
	unsigned char *buf;
	size_t len;
	uint32_t nlen;
	char *rbuf;
	ssize_t n;
	struct ucl_parser *parser;
	ucl_object_t *resp;

	/* Send request */
	buf = ucl_object_emit(req, UCL_EMIT_JSON_COMPACT);
	if (buf == NULL)
		return (NULL);

	len = strlen((char *)buf);
	nlen = htonl((uint32_t)len);

	if (xwrite(fd, &nlen, sizeof(nlen)) < 0 ||
	    xwrite(fd, buf, len) < 0) {
		warn("write to control socket");
		free(buf);
		return (NULL);
	}
	free(buf);

	/* Read response */
	n = xread(fd, &nlen, sizeof(nlen));
	if (n != (ssize_t)sizeof(nlen)) {
		if (n < 0)
			warn("read from control socket");
		else
			warnx("short read from control socket");
		return (NULL);
	}

	nlen = ntohl(nlen);
	if (nlen > 1024 * 1024) /* Cap at 1MB */
		return (NULL);
	rbuf = malloc(nlen + 1);
	if (rbuf == NULL)
		return (NULL);

	n = xread(fd, rbuf, nlen);
	if (n != (ssize_t)nlen) {
		if (n < 0)
			warn("read from control socket");
		else
			warnx("short read from control socket (%zd/%u)",
			    n, nlen);
		free(rbuf);
		return (NULL);
	}
	rbuf[nlen] = '\0';

	parser = ucl_parser_new(UCL_PARSER_DEFAULT);
	ucl_parser_add_string(parser, rbuf, nlen);
	resp = ucl_parser_get_object(parser);
	ucl_parser_free(parser);
	free(rbuf);

	return (resp);
}

/*
 * Print status table.
 */
static void
print_status(const ucl_object_t *resp)
{
	const ucl_object_t *services, *svc;
	ucl_object_iter_t it;

	printf("%-20s %-12s %8s  %s\n",
	    "SERVICE", "STATE", "PID", "TYPE");
	printf("%-20s %-12s %8s  %s\n",
	    "-------", "-----", "---", "----");

	services = ucl_object_lookup(resp, "services");
	if (services == NULL)
		return;

	it = ucl_object_iterate_new(services);
	while ((svc = ucl_object_iterate_safe(it, true)) != NULL) {
		const ucl_object_t *name, *state, *pid, *type;

		name = ucl_object_lookup(svc, "name");
		state = ucl_object_lookup(svc, "state");
		pid = ucl_object_lookup(svc, "pid");
		type = ucl_object_lookup(svc, "type");

		{
			const ucl_object_t *en;
			const char *ststr;
			long long p = pid ? (long long)ucl_object_toint(pid) : -1;
			char pbuf[16];

			en = ucl_object_lookup(svc, "enabled");
			ststr = state ? ucl_object_tostring(state) : "?";

			/* Show reason for services that are off */
			if (en != NULL && !ucl_object_toboolean(en)) {
				const ucl_object_t *nj, *ns;

				nj = ucl_object_lookup(svc, "nojail");
				ns = ucl_object_lookup(svc, "nostart");
				if (nj != NULL && ucl_object_toboolean(nj))
					ststr = "nojail";
				else if (ns != NULL && ucl_object_toboolean(ns))
					ststr = "nostart";
				else
					ststr = "disabled";
			}

			if (p <= 0)
				strlcpy(pbuf, "-", sizeof(pbuf));
			else
				snprintf(pbuf, sizeof(pbuf), "%lld", p);
			printf("%-20s %-12s %8s  %s\n",
			    name ? ucl_object_tostring(name) : "?",
			    ststr, pbuf,
			    type ? ucl_object_tostring(type) : "?");
		}
	}
	ucl_object_iterate_free(it);
}

static void
usage(void)
{

	fprintf(stderr,
	    "usage: rcctl [-s socket] status [service]\n"
	    "       rcctl [-s socket] start|stop|restart|reload <service>\n"
	    "       rcctl [-s socket] enable|disable <service>\n"
	    "       rcctl [-s socket] show <service>\n"
	    "       rcctl [-s socket] resources <service>\n"
	    "       rcctl [-s socket] suspend|resume\n"
	    "       rcctl [-s socket] list\n");
	exit(EX_USAGE);
}

int
main(int argc, char *argv[])
{
	ucl_object_t *req, *resp;
	const ucl_object_t *status_obj, *msg_obj;
	int fd, ch;

	while ((ch = getopt(argc, argv, "s:")) != -1) {
		switch (ch) {
		case 's':
			control_socket = optarg;
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	if (argc < 1)
		usage();

	/*
	 * For commands that take multiple services (start, stop, restart,
	 * reload, enable, disable, show), loop over argv[1..].
	 * For global commands (status, list, suspend, resume), single request.
	 */
	if (argc < 2 ||
	    strcmp(argv[0], cmd_status) == 0 ||
	    strcmp(argv[0], cmd_list) == 0 ||
	    strcmp(argv[0], cmd_suspend) == 0 ||
	    strcmp(argv[0], cmd_resume) == 0 ||
	    strcmp(argv[0], cmd_show) == 0 ||
	    strcmp(argv[0], cmd_resources) == 0) {
		/* Single request, optionally with one service */
		fd = ctl_connect();
		req = ucl_object_typed_new(UCL_OBJECT);
		ucl_object_insert_key(req,
		    ucl_object_fromstring(argv[0]), "command", 0, false);
		if (argc >= 2)
			ucl_object_insert_key(req,
			    ucl_object_fromstring(argv[1]),
			    "service", 0, false);
		resp = ctl_request(fd, req);
		ucl_object_unref(req);
		close(fd);
	} else {
		/*
		 * Multi-service: group template@instance args by
		 * template name so legacy scripts get all instances
		 * in one call (e.g., /bin/sh netif restart em0 em1).
		 * Non-template services are sent individually.
		 */
		int si;

		resp = NULL;
		for (si = 1; si < argc; ) {
			const char *arg, *at;
			char tmpl[256];

			arg = argv[si];
			at = strchr(arg, '@');

			if (at != NULL) {
				/*
				 * Template instance: collect all
				 * instances of the same template.
				 */
				size_t tlen;
				ucl_object_t *insts;
				int sj;

				tlen = (size_t)(at - arg);
				if (tlen >= sizeof(tmpl))
					tlen = sizeof(tmpl) - 1;
				memcpy(tmpl, arg, tlen);
				tmpl[tlen] = '\0';

				insts = ucl_object_typed_new(UCL_ARRAY);
				for (sj = si; sj < argc; sj++) {
					const char *a2 = argv[sj];
					const char *at2 = strchr(a2, '@');

					if (at2 == NULL)
						break;
					if ((size_t)(at2 - a2) != tlen ||
					    memcmp(a2, tmpl, tlen) != 0)
						break;
					ucl_array_append(insts,
					    ucl_object_fromstring(at2 + 1));
				}

				fd = ctl_connect();
				req = ucl_object_typed_new(UCL_OBJECT);
				ucl_object_insert_key(req,
				    ucl_object_fromstring(argv[0]),
				    "command", 0, false);
				ucl_object_insert_key(req,
				    ucl_object_fromstring(arg),
				    "service", 0, false);
				ucl_object_insert_key(req, insts,
				    "instances", 0, false);
				resp = ctl_request(fd, req);
				ucl_object_unref(req);
				close(fd);

				si = sj;	/* skip grouped args */
			} else {
				/* Regular service */
				fd = ctl_connect();
				req = ucl_object_typed_new(UCL_OBJECT);
				ucl_object_insert_key(req,
				    ucl_object_fromstring(argv[0]),
				    "command", 0, false);
				ucl_object_insert_key(req,
				    ucl_object_fromstring(arg),
				    "service", 0, false);
				resp = ctl_request(fd, req);
				ucl_object_unref(req);
				close(fd);

				si++;
			}

			if (resp == NULL) {
				warnx("%s: no response", arg);
				continue;
			}

			status_obj = ucl_object_lookup(resp, "status");
			if (status_obj != NULL &&
			    strcmp(ucl_object_tostring(status_obj),
			    "error") == 0) {
				msg_obj = ucl_object_lookup(resp, "message");
				warnx("%s: %s", arg,
				    msg_obj ?
				    ucl_object_tostring(msg_obj) :
				    "unknown error");
			}
			ucl_object_unref(resp);
		}
		return (0);
	}

	if (resp == NULL)
		errx(1, "no response from rcd");

	/* Check for errors */
	status_obj = ucl_object_lookup(resp, "status");
	if (status_obj != NULL &&
	    strcmp(ucl_object_tostring(status_obj), "error") == 0) {
		msg_obj = ucl_object_lookup(resp, "message");
		errx(1, "%s",
		    msg_obj ? ucl_object_tostring(msg_obj) : "unknown error");
	}

	/* Display output based on command */
	if (strcmp(argv[0], cmd_status) == 0 ||
	    strcmp(argv[0], cmd_list) == 0) {
		print_status(resp);
	} else if (strcmp(argv[0], cmd_resources) == 0) {
		const ucl_object_t *res;
		res = ucl_object_lookup(resp, "resources");
		if (res != NULL)
			printf("%s\n", ucl_object_tostring(res));
	} else if (strcmp(argv[0], cmd_suspend) == 0) {
		const ucl_object_t *n;
		n = ucl_object_lookup(resp, "stopped");
		if (n != NULL)
			printf("suspended %lld services\n",
			    (long long)ucl_object_toint(n));
	} else if (strcmp(argv[0], cmd_resume) == 0) {
		const ucl_object_t *n;
		n = ucl_object_lookup(resp, "started");
		if (n != NULL)
			printf("resumed %lld services\n",
			    (long long)ucl_object_toint(n));
	} else if (strcmp(argv[0], cmd_show) == 0) {
		const ucl_object_t *cfg;
		cfg = ucl_object_lookup(resp, "config");
		if (cfg != NULL) {
			unsigned char *out;
			out = ucl_object_emit(cfg, UCL_EMIT_CONFIG);
			if (out != NULL) {
				printf("%s", out);
				free(out);
			}
		}
	} else {
		/* Simple OK/error response */
		msg_obj = ucl_object_lookup(resp, "message");
		if (msg_obj != NULL)
			printf("%s\n", ucl_object_tostring(msg_obj));
	}

	ucl_object_unref(resp);
	return (0);
}
