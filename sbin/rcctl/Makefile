.include <src.opts.mk>

CFLAGS+=	-I${SRCTOP}/contrib/libucl/include
CFLAGS+=	-I${SRCTOP}/sbin/rcd

PACKAGE=	rc

PROG=		rcctl
SRCS=		rcctl.c

LIBADD=		ucl

MAN=		rcctl.8

WARNS?=		6

.include <bsd.prog.mk>
