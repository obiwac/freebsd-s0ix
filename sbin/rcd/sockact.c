/*
 * Copyright (c) 2026 Baptiste Daroussin <bapt@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Socket activation.  Pre-bind sockets before services start, register
 * them in kqueue, and pass them as inherited fds via posix_spawn file
 * actions when the service is launched (either at boot or on first
 * connection).
 */

#include <sys/param.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rcd.h"

#define LISTEN_FD_START	3

/*
 * Parse a listen address string into sockaddr.
 * Formats:
 *   "tcp:addr:port"    → AF_INET, SOCK_STREAM
 *   "tcp6:addr:port"   → AF_INET6, SOCK_STREAM
 *   "udp:addr:port"    → AF_INET, SOCK_DGRAM
 *   "unix:/path"       → AF_UNIX, SOCK_STREAM
 */
static int
parse_host_port(const char *rest, int family, int socktype,
    struct sockaddr_storage *ss, socklen_t *sslen)
{
	struct addrinfo hints, *res;
	char host[256], port[32];
	const char *colon;

	colon = strrchr(rest, ':');
	if (colon == NULL)
		return (-1);

	strlcpy(host, rest, MIN((size_t)(colon - rest + 1), sizeof(host)));
	strlcpy(port, colon + 1, sizeof(port));

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = family;
	hints.ai_socktype = socktype;
	hints.ai_flags = AI_PASSIVE;

	if (getaddrinfo(strcmp(host, "*") == 0 ? NULL : host,
	    port, &hints, &res) != 0)
		return (-1);

	memcpy(ss, res->ai_addr, res->ai_addrlen);
	*sslen = res->ai_addrlen;
	freeaddrinfo(res);
	return (0);
}

int
parse_listen_addr(const char *spec, struct sockaddr_storage *ss,
    socklen_t *sslen, int *domain, int *socktype)
{
	struct sockaddr_un *sun;
	const char *rest;

	if (strncmp(spec, "tcp:", 4) == 0) {
		rest = spec + 4;
		*domain = AF_INET;
		*socktype = SOCK_STREAM;
	} else if (strncmp(spec, "tcp6:", 5) == 0) {
		rest = spec + 5;
		*domain = AF_INET6;
		*socktype = SOCK_STREAM;
	} else if (strncmp(spec, "udp:", 4) == 0) {
		rest = spec + 4;
		*domain = AF_INET;
		*socktype = SOCK_DGRAM;
	} else if (strncmp(spec, "unix:", 5) == 0) {
		*domain = AF_UNIX;
		*socktype = SOCK_STREAM;
		sun = (struct sockaddr_un *)ss;
		memset(sun, 0, sizeof(*sun));
		sun->sun_family = AF_UNIX;
		strlcpy(sun->sun_path, spec + 5, sizeof(sun->sun_path));
		sun->sun_len = SUN_LEN(sun);
		*sslen = SUN_LEN(sun);
		return (0);
	} else {
		return (-1);
	}

	return (parse_host_port(rest, *domain, *socktype, ss, sslen));
}

/*
 * Create, bind, and listen on a socket.
 */
int
sockact_bind(struct unit_socket *us)
{
	struct sockaddr_storage ss;
	socklen_t sslen;
	int domain, socktype;
	int fd, opt;

	if (us->us_address == NULL)
		return (-1);

	if (parse_listen_addr(us->us_address, &ss, &sslen,
	    &domain, &socktype) != 0) {
		log_warn("invalid listen address: %s", us->us_address);
		return (-1);
	}

	fd = socket(domain, socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		log_warn("socket: %s", strerror(errno));
		return (-1);
	}

	opt = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) != 0)
		log_warn("setsockopt SO_REUSEADDR: %s", strerror(errno));
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) != 0)
		log_warn("setsockopt SO_REUSEPORT: %s", strerror(errno));

	/* Remove stale unix socket */
	if (domain == AF_UNIX)
		unlink(((struct sockaddr_un *)&ss)->sun_path);

	if (bind(fd, (struct sockaddr *)&ss, sslen) != 0) {
		log_warn("bind %s: %s", us->us_address, strerror(errno));
		close(fd);
		return (-1);
	}

	/* Set permissions for unix sockets */
	if (domain == AF_UNIX)
		chmod(((struct sockaddr_un *)&ss)->sun_path,
		    us->us_permissions);

	if (socktype == SOCK_STREAM || socktype == SOCK_SEQPACKET) {
		if (listen(fd, us->us_backlog) != 0) {
			log_warn("listen %s: %s", us->us_address,
			    strerror(errno));
			close(fd);
			return (-1);
		}
	}

	us->us_fd = fd;
	log_info("bound socket %s: %s (fd %d)", us->us_name,
	    us->us_address, fd);
	return (0);
}

/*
 * Register socket-activated service sockets in kqueue.
 * On first connection, the main loop will start the service.
 */
void
sockact_register(struct rcd_ctx *ctx, struct unit *u)
{
	struct unit_socket *us;
	struct kevent kev;

	TAILQ_FOREACH(us, &u->u_sockets, us_entries) {
		if (us->us_fd < 0)
			continue;
		EV_SET(&kev, us->us_fd, EVFILT_READ, EV_ADD, 0, 0, u);
		if (kevent(ctx->ctx_kq, &kev, 1, NULL, 0, NULL) < 0)
			log_warn("%s: kevent socket %s: %s",
			    u->u_name, us->us_name, strerror(errno));
	}
}

/*
 * Defer socket registration for READY_SOCKET units.
 * Store the kevent in us_kev; it will be applied when the
 * service signals readiness.
 */
void
sockact_register_deferred(struct unit *u)
{
	struct unit_socket *us;

	TAILQ_FOREACH(us, &u->u_sockets, us_entries) {
		if (us->us_fd < 0)
			continue;
		EV_SET(&us->us_kev, us->us_fd, EVFILT_READ, EV_ADD, 0, 0, u);
	}
}

/*
 * Register all deferred sockets for a unit in kqueue.
 * Called when the service signals readiness (eventfd notification).
 */
void
sockact_deferred_register_all(struct rcd_ctx *ctx, struct unit *u)
{
	struct unit_socket *us;

	TAILQ_FOREACH(us, &u->u_sockets, us_entries) {
		if (us->us_fd < 0)
			continue;
		if (kevent(ctx->ctx_kq, &us->us_kev, 1, NULL, 0, NULL) < 0)
			log_warn("%s: kevent socket %s: %s",
			    u->u_name, us->us_name, strerror(errno));
	}
}

/*
 * Set up posix_spawn file actions to pass socket fds to the child.
 * Sockets are placed at fd LISTEN_FD_START, LISTEN_FD_START+1, ...
 */
int
sockact_setup_fds(struct unit *u, posix_spawn_file_actions_t *fa,
    int *listen_count)
{
	struct unit_socket *us;
	int target_fd;

	target_fd = LISTEN_FD_START;
	*listen_count = 0;

	TAILQ_FOREACH(us, &u->u_sockets, us_entries) {
		if (us->us_fd < 0)
			continue;

		/*
		 * dup2 the bound socket to the expected fd number.
		 * The child inherits it; rcd keeps the original.
		 */
		posix_spawn_file_actions_adddup2(fa, us->us_fd, target_fd);
		target_fd++;
		(*listen_count)++;
	}

	return (0);
}

/*
 * Close all sockets for a unit.
 */
void
sockact_close(struct unit *u)
{
	struct unit_socket *us;

	TAILQ_FOREACH(us, &u->u_sockets, us_entries) {
		if (us->us_fd >= 0) {
			close(us->us_fd);
			us->us_fd = -1;
		}
	}
}
