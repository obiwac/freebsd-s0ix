/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Baptiste Daroussin <bapt@FreeBSD.org>
 */

/*
 * Tests for luaexec.c: embedded Lua execution.
 */

#include <sys/param.h>

#include <string.h>

#include <atf-c.h>

#include "rcd.h"

ATF_TC(lua_valid_code);
ATF_TC_HEAD(lua_valid_code, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "lua_exec with valid code returns 0");
}
ATF_TC_BODY(lua_valid_code, tc)
{

	lua_init();
	ATF_CHECK(lua_exec("return true", "test", NULL) == 0);
}

ATF_TC(lua_syntax_error);
ATF_TC_HEAD(lua_syntax_error, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "lua_exec with syntax error returns non-zero");
}
ATF_TC_BODY(lua_syntax_error, tc)
{

	lua_init();
	ATF_CHECK(lua_exec("this is not valid lua!!!", "test", NULL) != 0);
}

ATF_TC(lua_runtime_error);
ATF_TC_HEAD(lua_runtime_error, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "lua_exec with runtime error returns non-zero");
}
ATF_TC_BODY(lua_runtime_error, tc)
{

	lua_init();
	ATF_CHECK(lua_exec("error('boom')", "test", NULL) != 0);
}

ATF_TC(lua_return_false);
ATF_TC_HEAD(lua_return_false, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "lua_exec returning false returns non-zero");
}
ATF_TC_BODY(lua_return_false, tc)
{

	lua_init();
	ATF_CHECK(lua_exec("return false", "test", NULL) != 0);
}

ATF_TC(lua_no_return);
ATF_TC_HEAD(lua_no_return, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "lua_exec with no return value succeeds");
}
ATF_TC_BODY(lua_no_return, tc)
{

	lua_init();
	ATF_CHECK(lua_exec("local x = 1 + 1", "test", NULL) == 0);
}

ATF_TP_ADD_TCS(tp)
{

	ATF_TP_ADD_TC(tp, lua_valid_code);
	ATF_TP_ADD_TC(tp, lua_syntax_error);
	ATF_TP_ADD_TC(tp, lua_runtime_error);
	ATF_TP_ADD_TC(tp, lua_return_false);
	ATF_TP_ADD_TC(tp, lua_no_return);

	return (atf_no_error());
}
