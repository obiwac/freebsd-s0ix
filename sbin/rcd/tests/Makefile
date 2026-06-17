.include <src.opts.mk>

PACKAGE=	tests

LUASRC=		${SRCTOP}/contrib/lua/src
UCLSRC=		${SRCTOP}/contrib/libucl

CFLAGS+=	-I${SRCTOP}/contrib/libucl/include
CFLAGS+=	-I${.CURDIR:H}
CFLAGS+=	-I${SRCTOP}/lib/liblua
CFLAGS+=	-I${LUASRC}
CFLAGS+=	-I${SRCTOP}/libexec/flua/modules
CFLAGS+=	-I${UCLSRC}/src -I${UCLSRC}/uthash
CFLAGS+=	-I${SRCTOP}/libexec/flua

.PATH:		${.CURDIR:H}
.PATH:		${SRCTOP}/libexec/flua/modules
.PATH:		${UCLSRC}/lua

ATF_TESTS_C+=	depgraph_test
ATF_TESTS_C+=	unit_test
ATF_TESTS_C+=	compat_test
ATF_TESTS_C+=	config_test
ATF_TESTS_C+=	sockact_test
ATF_TESTS_C+=	enable_test
ATF_TESTS_C+=	process_test
ATF_TESTS_C+=	rctl_test
ATF_TESTS_C+=	luaexec_test
ATF_TESTS_C+=	jail_svc_test

ATF_TESTS_SH+=	rcd_jail_test

${PACKAGE}FILES+=	rcd_utils.subr

SRCS.depgraph_test=	depgraph_test.c depgraph.c unit.c log.c hash.c \
			oom_stub.c
SRCS.unit_test=		unit_test.c unit.c log.c oom_stub.c
SRCS.compat_test=	compat_test.c compat.c unit.c depgraph.c log.c hash.c \
			oom_stub.c
SRCS.config_test=	config_test.c unit.c log.c oom_stub.c
SRCS.sockact_test=	sockact_test.c sockact.c unit.c log.c oom_stub.c
SRCS.enable_test=	enable_test.c enable.c unit.c log.c oom_stub.c
SRCS.process_test=	process_test.c process.c unit.c log.c \
			luaexec.c lposix.c lua_ucl.c \
			sockact.c rctl_mgr.c jail_svc.c hash.c oom_stub.c
SRCS.rctl_test=		rctl_test.c rctl_mgr.c unit.c log.c oom_stub.c
SRCS.luaexec_test=	luaexec_test.c luaexec.c lposix.c lua_ucl.c \
			unit.c log.c oom_stub.c
SRCS.jail_svc_test=	jail_svc_test.c jail_svc.c unit.c log.c oom_stub.c

CWARNFLAGS.hash.c=	-Wno-cast-align -Wno-cast-qual
CWARNFLAGS.lua_ucl.c=	-Wno-cast-align -Wno-cast-qual

LIBADD=			ucl
LIBADD.process_test=	ucl jail lua
LIBADD.luaexec_test=	ucl lua
LIBADD.jail_svc_test=	ucl jail

.PATH:		${.CURDIR}/data

${PACKAGE}FILES+=	sshd.ucl
${PACKAGE}FILES+=	cleartmp.ucl
${PACKAGE}FILES+=	nginx.ucl
${PACKAGE}FILES+=	cycle_a.ucl
${PACKAGE}FILES+=	cycle_b.ucl
${PACKAGE}FILES+=	legacy_script
${PACKAGE}FILES+=	barrier_script
${PACKAGE}FILES+=	daemon_script
${PACKAGE}FILES+=	daemon_script2
${PACKAGE}FILES+=	oneshot_script
${PACKAGE}FILES+=	norcd_script
${PACKAGE}FILES+=	prepend.ucl
${PACKAGE}FILES+=	hooks.ucl
${PACKAGE}FILES+=	disabled.ucl
${PACKAGE}FILES+=	reqvars.ucl
${PACKAGE}FILES+=	offcmd.ucl
${PACKAGE}FILES+=	override_base.ucl
${PACKAGE}FILES+=	replace_test.ucl

# Override test data: installed to ${TESTSDIR}/confdir/
CONFDIR=	${TESTSDIR}/confdir
CONFDIRFILES=	override_base replace_test
CONFDIRFILESDIR=	${CONFDIR}
.PATH:		${.CURDIR}/data/confdir
FILESGROUPS+=	CONFDIRFILES

WARNS?=		6

.include <bsd.test.mk>
