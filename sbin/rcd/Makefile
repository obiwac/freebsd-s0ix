.include <src.opts.mk>

LUASRC=		${SRCTOP}/contrib/lua/src

CFLAGS+=	-I${SRCTOP}/contrib/libucl/include
CFLAGS+=	-I${SRCTOP}/lib/liblua
CFLAGS+=	-I${LUASRC}
CFLAGS+=	-I${SRCTOP}/libexec/flua/modules

PACKAGE=	rc

PROGS=		rcd rcd-exec

# --- rcd: main daemon ---
SRCS.rcd=	rcd.c unit.c depgraph.c process.c sockact.c \
		compat.c control.c rctl_mgr.c jail_svc.c log.c enable.c \
		luaexec.c hash.c

# Built-in Lua modules from flua
.PATH:		${SRCTOP}/libexec/flua/modules
SRCS.rcd+=	lposix.c

# lua_ucl.c — UCL bindings for Lua (built-in, no /usr needed)
UCLSRC=		${SRCTOP}/contrib/libucl
.PATH:		${UCLSRC}/lua
SRCS.rcd+=	lua_ucl.c
CFLAGS+=	-I${UCLSRC}/src -I${UCLSRC}/uthash -I${SRCTOP}/libexec/flua
CWARNFLAGS.lua_ucl.c=	-Wno-cast-align -Wno-cast-qual
CWARNFLAGS.hash.c=	-Wno-cast-align -Wno-cast-qual

LIBADD.rcd=	ucl jail lua

MAN.rcd=	rcd.8 rcd.conf.5 rcd.d.5 rcd-lua.3

# Embed JSON schemas into the binary as C byte arrays.
CFLAGS+=	-I${.OBJDIR}

conf_schema.h: ${.CURDIR}/schema/rcd.conf.schema.json
	file2c -sx 'static const char conf_schema_json[] = {' ', 0x00};' \
	    < ${.ALLSRC} > ${.TARGET}

unit_schema.h: ${.CURDIR}/schema/unit.schema.json
	file2c -sx 'static const char unit_schema_json[] = {' ', 0x00};' \
	    < ${.ALLSRC} > ${.TARGET}

rcd.pieo: conf_schema.h unit_schema.h

CLEANFILES+=	conf_schema.h unit_schema.h

# --- rcd-exec: pre-exec setup helper ---
SRCS.rcd-exec=	rcd-exec.c

LIBADD.rcd-exec=	jail util

MAN.rcd-exec=	rcd-exec.8

WARNS?=		6

HAS_TESTS=
SUBDIR.${MK_TESTS}+=	tests

.include <bsd.progs.mk>
