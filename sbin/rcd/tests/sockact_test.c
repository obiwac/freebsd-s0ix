/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Baptiste Daroussin <bapt@FreeBSD.org>
 */

/*
 * Unit tests for socket activation address parsing.
 */

#include <sys/param.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <atf-c.h>

#include "rcd.h"

ATF_TC_WITHOUT_HEAD(parse_tcp_wildcard);
ATF_TC_BODY(parse_tcp_wildcard, tc)
{
	struct sockaddr_storage ss;
	struct sockaddr_in *sin;
	socklen_t sslen;
	int domain, socktype;

	ATF_REQUIRE(parse_listen_addr("tcp:*:80", &ss, &sslen,
	    &domain, &socktype) == 0);
	ATF_CHECK_EQ(domain, AF_INET);
	ATF_CHECK_EQ(socktype, SOCK_STREAM);

	sin = (struct sockaddr_in *)&ss;
	ATF_CHECK_EQ(sin->sin_family, AF_INET);
	ATF_CHECK_EQ(ntohs(sin->sin_port), 80);
	ATF_CHECK_EQ(sin->sin_addr.s_addr, htonl(INADDR_ANY));
}

ATF_TC_WITHOUT_HEAD(parse_tcp_localhost);
ATF_TC_BODY(parse_tcp_localhost, tc)
{
	struct sockaddr_storage ss;
	struct sockaddr_in *sin;
	socklen_t sslen;
	int domain, socktype;

	ATF_REQUIRE(parse_listen_addr("tcp:127.0.0.1:8080", &ss, &sslen,
	    &domain, &socktype) == 0);
	ATF_CHECK_EQ(domain, AF_INET);
	ATF_CHECK_EQ(socktype, SOCK_STREAM);

	sin = (struct sockaddr_in *)&ss;
	ATF_CHECK_EQ(ntohs(sin->sin_port), 8080);
	ATF_CHECK_EQ(sin->sin_addr.s_addr, htonl(INADDR_LOOPBACK));
}

ATF_TC_WITHOUT_HEAD(parse_tcp6);
ATF_TC_BODY(parse_tcp6, tc)
{
	struct sockaddr_storage ss;
	struct sockaddr_in6 *sin6;
	socklen_t sslen;
	int domain, socktype;

	ATF_REQUIRE(parse_listen_addr("tcp6:::1:443", &ss, &sslen,
	    &domain, &socktype) == 0);
	ATF_CHECK_EQ(domain, AF_INET6);
	ATF_CHECK_EQ(socktype, SOCK_STREAM);

	sin6 = (struct sockaddr_in6 *)&ss;
	ATF_CHECK_EQ(ntohs(sin6->sin6_port), 443);
}

ATF_TC_WITHOUT_HEAD(parse_udp);
ATF_TC_BODY(parse_udp, tc)
{
	struct sockaddr_storage ss;
	struct sockaddr_in *sin;
	socklen_t sslen;
	int domain, socktype;

	ATF_REQUIRE(parse_listen_addr("udp:*:514", &ss, &sslen,
	    &domain, &socktype) == 0);
	ATF_CHECK_EQ(domain, AF_INET);
	ATF_CHECK_EQ(socktype, SOCK_DGRAM);

	sin = (struct sockaddr_in *)&ss;
	ATF_CHECK_EQ(ntohs(sin->sin_port), 514);
}

ATF_TC_WITHOUT_HEAD(parse_unix);
ATF_TC_BODY(parse_unix, tc)
{
	struct sockaddr_storage ss;
	struct sockaddr_un *sun;
	socklen_t sslen;
	int domain, socktype;

	ATF_REQUIRE(parse_listen_addr("unix:/var/run/test.sock", &ss, &sslen,
	    &domain, &socktype) == 0);
	ATF_CHECK_EQ(domain, AF_UNIX);
	ATF_CHECK_EQ(socktype, SOCK_STREAM);

	sun = (struct sockaddr_un *)&ss;
	ATF_CHECK_STREQ(sun->sun_path, "/var/run/test.sock");
}

ATF_TC_WITHOUT_HEAD(parse_invalid_proto);
ATF_TC_BODY(parse_invalid_proto, tc)
{
	struct sockaddr_storage ss;
	socklen_t sslen;
	int domain, socktype;

	ATF_CHECK(parse_listen_addr("sctp:*:80", &ss, &sslen,
	    &domain, &socktype) != 0);
}

ATF_TC_WITHOUT_HEAD(parse_missing_port);
ATF_TC_BODY(parse_missing_port, tc)
{
	struct sockaddr_storage ss;
	socklen_t sslen;
	int domain, socktype;

	ATF_CHECK(parse_listen_addr("tcp:127.0.0.1", &ss, &sslen,
	    &domain, &socktype) != 0);
}

ATF_TC_WITHOUT_HEAD(parse_empty_spec);
ATF_TC_BODY(parse_empty_spec, tc)
{
	struct sockaddr_storage ss;
	socklen_t sslen;
	int domain, socktype;

	ATF_CHECK(parse_listen_addr("", &ss, &sslen,
	    &domain, &socktype) != 0);
}

/*
 * Helper: create a minimal unit with one TCP socket on a random port.
 * The socket is bound; the caller must close it via sockact_close + unit_free.
 */
static struct unit *
make_test_unit(void)
{
	struct unit *u;
	struct unit_socket *us;

	u = calloc(1, sizeof(*u));
	ATF_REQUIRE(u != NULL);
	u->u_state = STATE_INACTIVE;
	u->u_procdesc_fd = -1;
	u->u_pid = -1;
	u->u_ready_method = READY_IMMEDIATE;

	STAILQ_INIT(&u->u_deps);
	STAILQ_INIT(&u->u_rdeps);
	STAILQ_INIT(&u->u_rctl);
	STAILQ_INIT(&u->u_rctl_active);
	STAILQ_INIT(&u->u_env);
	TAILQ_INIT(&u->u_sockets);

	us = calloc(1, sizeof(*us));
	ATF_REQUIRE(us != NULL);
	us->us_name = strdup("test_sock");
	us->us_address = strdup("tcp:127.0.0.1:0");
	us->us_fd = -1;
	us->us_type = SOCK_ACT_STREAM;
	us->us_backlog = 1;

	TAILQ_INSERT_TAIL(&u->u_sockets, us, us_entries);
	return (u);
}

ATF_TC_WITHOUT_HEAD(deferred_register_store);
ATF_TC_BODY(deferred_register_store, tc)
{
	struct unit *u;
	struct unit_socket *us;

	u = make_test_unit();
	us = TAILQ_FIRST(&u->u_sockets);

	/* Bind the socket */
	ATF_REQUIRE(sockact_bind(us) == 0);
	ATF_CHECK(us->us_fd >= 0);

	/* Initially the kevent should be zeroed */
	ATF_CHECK(us->us_kev.filter == 0);

	/* Register deferred */
	sockact_register_deferred(u);

	/* Verify the kevent was stored correctly */
	ATF_CHECK_EQ(us->us_kev.ident, (uintptr_t)us->us_fd);
	ATF_CHECK_EQ(us->us_kev.filter, EVFILT_READ);
	ATF_CHECK_EQ(us->us_kev.flags, EV_ADD);
	ATF_CHECK(us->us_kev.udata == u);

	sockact_close(u);
	unit_free(u);
}

ATF_TC_WITHOUT_HEAD(deferred_register_apply);
ATF_TC_BODY(deferred_register_apply, tc)
{
	struct unit *u;
	struct unit_socket *us;
	struct rcd_ctx ctx;
	struct kevent kev;
	struct sockaddr_in sin;
	socklen_t sinlen;
	int kq, client_fd;

	u = make_test_unit();
	us = TAILQ_FIRST(&u->u_sockets);

	/* Bind the socket */
	ATF_REQUIRE(sockact_bind(us) == 0);
	ATF_REQUIRE(us->us_fd >= 0);

	/* Register deferred */
	sockact_register_deferred(u);

	/* Create a kqueue and set up a minimal context */
	kq = kqueue();
	ATF_REQUIRE(kq >= 0);
	memset(&ctx, 0, sizeof(ctx));
	ctx.ctx_kq = kq;

	/* Apply the deferred registration */
	sockact_deferred_register_all(&ctx, u);

	/* Get the bound port so we can connect */
	sinlen = sizeof(sin);
	ATF_REQUIRE(getsockname(us->us_fd,
	    (struct sockaddr *)&sin, &sinlen) == 0);

	/* Connect a client to trigger an incoming connection */
	client_fd = socket(AF_INET, SOCK_STREAM, 0);
	ATF_REQUIRE(client_fd >= 0);
	ATF_REQUIRE(connect(client_fd,
	    (struct sockaddr *)&sin, sizeof(sin)) == 0);

	/* Poll kqueue — should get the event */
	memset(&kev, 0, sizeof(kev));
	ATF_CHECK(kevent(kq, NULL, 0, &kev, 1,
	    &(struct timespec){ .tv_sec = 1, .tv_nsec = 0 }) == 1);

	/* Verify event details */
	ATF_CHECK_EQ(kev.ident, (uintptr_t)us->us_fd);
	ATF_CHECK_EQ(kev.filter, EVFILT_READ);
	ATF_CHECK(kev.udata == u);

	close(client_fd);
	close(kq);
	sockact_close(u);
	unit_free(u);
}

ATF_TC_WITHOUT_HEAD(deferred_unbound_socket);
ATF_TC_BODY(deferred_unbound_socket, tc)
{
	struct unit *u;
	struct unit_socket *us;

	u = make_test_unit();
	us = TAILQ_FIRST(&u->u_sockets);

	/* Socket not bound — us_fd stays -1 */
	ATF_CHECK_EQ(us->us_fd, -1);

	/* Register deferred — should be a no-op */
	sockact_register_deferred(u);

	/* Verify kevent was NOT populated */
	ATF_CHECK(us->us_kev.filter == 0);

	unit_free(u);
}

ATF_TP_ADD_TCS(tp)
{

	ATF_TP_ADD_TC(tp, parse_tcp_wildcard);
	ATF_TP_ADD_TC(tp, parse_tcp_localhost);
	ATF_TP_ADD_TC(tp, parse_tcp6);
	ATF_TP_ADD_TC(tp, parse_udp);
	ATF_TP_ADD_TC(tp, parse_unix);
	ATF_TP_ADD_TC(tp, parse_invalid_proto);
	ATF_TP_ADD_TC(tp, parse_missing_port);
	ATF_TP_ADD_TC(tp, parse_empty_spec);
	ATF_TP_ADD_TC(tp, deferred_register_store);
	ATF_TP_ADD_TC(tp, deferred_register_apply);
	ATF_TP_ADD_TC(tp, deferred_unbound_socket);

	return (atf_no_error());
}
