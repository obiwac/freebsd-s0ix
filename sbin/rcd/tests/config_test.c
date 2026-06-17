/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Baptiste Daroussin <bapt@FreeBSD.org>
 */

/*
 * Unit tests for configuration parsing helpers, in particular
 * ucl_parse_mode() which accepts integer, octal string, and
 * symbolic permission formats.
 */

#include <sys/param.h>
#include <sys/stat.h>

#include <stdio.h>
#include <string.h>

#include <atf-c.h>
#include <ucl.h>

#include "rcd.h"

/*
 * Helper: create a UCL integer object.
 */
static ucl_object_t *
ucl_int(int64_t v)
{

	return (ucl_object_fromint(v));
}

/*
 * Helper: create a UCL string object.
 */
static ucl_object_t *
ucl_str(const char *s)
{

	return (ucl_object_fromstring(s));
}

/* ---- Integer mode tests ---- */

ATF_TC_WITHOUT_HEAD(mode_int_0660);
ATF_TC_BODY(mode_int_0660, tc)
{
	ucl_object_t *obj;
	mode_t m;

	obj = ucl_int(0660);
	ATF_REQUIRE(ucl_parse_mode(obj, &m) == 0);
	ATF_CHECK_EQ(m, 0660);
	ucl_object_unref(obj);
}

ATF_TC_WITHOUT_HEAD(mode_int_0755);
ATF_TC_BODY(mode_int_0755, tc)
{
	ucl_object_t *obj;
	mode_t m;

	obj = ucl_int(0755);
	ATF_REQUIRE(ucl_parse_mode(obj, &m) == 0);
	ATF_CHECK_EQ(m, 0755);
	ucl_object_unref(obj);
}

ATF_TC_WITHOUT_HEAD(mode_int_0);
ATF_TC_BODY(mode_int_0, tc)
{
	ucl_object_t *obj;
	mode_t m;

	obj = ucl_int(0);
	ATF_REQUIRE(ucl_parse_mode(obj, &m) == 0);
	ATF_CHECK_EQ(m, 0);
	ucl_object_unref(obj);
}

ATF_TC_WITHOUT_HEAD(mode_int_0777);
ATF_TC_BODY(mode_int_0777, tc)
{
	ucl_object_t *obj;
	mode_t m;

	obj = ucl_int(0777);
	ATF_REQUIRE(ucl_parse_mode(obj, &m) == 0);
	ATF_CHECK_EQ(m, 0777);
	ucl_object_unref(obj);
}

ATF_TC_WITHOUT_HEAD(mode_int_negative);
ATF_TC_BODY(mode_int_negative, tc)
{
	ucl_object_t *obj;
	mode_t m;

	obj = ucl_int(-1);
	ATF_CHECK(ucl_parse_mode(obj, &m) == -1);
	ucl_object_unref(obj);
}

ATF_TC_WITHOUT_HEAD(mode_int_too_large);
ATF_TC_BODY(mode_int_too_large, tc)
{
	ucl_object_t *obj;
	mode_t m;

	/* 01000 = setuid bit — should be rejected */
	obj = ucl_int(01000);
	ATF_CHECK(ucl_parse_mode(obj, &m) == -1);
	ucl_object_unref(obj);
}

/* ---- Octal string tests ---- */

ATF_TC_WITHOUT_HEAD(mode_str_octal_0660);
ATF_TC_BODY(mode_str_octal_0660, tc)
{
	ucl_object_t *obj;
	mode_t m;

	obj = ucl_str("0660");
	ATF_REQUIRE(ucl_parse_mode(obj, &m) == 0);
	ATF_CHECK_EQ(m, 0660);
	ucl_object_unref(obj);
}

ATF_TC_WITHOUT_HEAD(mode_str_octal_0755);
ATF_TC_BODY(mode_str_octal_0755, tc)
{
	ucl_object_t *obj;
	mode_t m;

	obj = ucl_str("0755");
	ATF_REQUIRE(ucl_parse_mode(obj, &m) == 0);
	ATF_CHECK_EQ(m, 0755);
	ucl_object_unref(obj);
}

ATF_TC_WITHOUT_HEAD(mode_str_octal_0600);
ATF_TC_BODY(mode_str_octal_0600, tc)
{
	ucl_object_t *obj;
	mode_t m;

	obj = ucl_str("0600");
	ATF_REQUIRE(ucl_parse_mode(obj, &m) == 0);
	ATF_CHECK_EQ(m, 0600);
	ucl_object_unref(obj);
}

/* ---- Symbolic string tests (chmod-style: "u=rw,g=rw") ---- */

ATF_TC_WITHOUT_HEAD(mode_str_chmod_u_rw_g_rw);
ATF_TC_BODY(mode_str_chmod_u_rw_g_rw, tc)
{
	ucl_object_t *obj;
	mode_t m;

	/* "u=rw,g=rw" = 0660 */
	obj = ucl_str("u=rw,g=rw");
	ATF_REQUIRE(ucl_parse_mode(obj, &m) == 0);
	ATF_CHECK_EQ(m, 0660);
	ucl_object_unref(obj);
}

ATF_TC_WITHOUT_HEAD(mode_str_chmod_u_rwx_g_rx);
ATF_TC_BODY(mode_str_chmod_u_rwx_g_rx, tc)
{
	ucl_object_t *obj;
	mode_t m;

	/* "u=rwx,g=rx" = 0750 */
	obj = ucl_str("u=rwx,g=rx");
	ATF_REQUIRE(ucl_parse_mode(obj, &m) == 0);
	ATF_CHECK_EQ(m, 0750);
	ucl_object_unref(obj);
}

ATF_TC_WITHOUT_HEAD(mode_str_chmod_a_rwx);
ATF_TC_BODY(mode_str_chmod_a_rwx, tc)
{
	ucl_object_t *obj;
	mode_t m;

	/* "a=rwx" = 0777 */
	obj = ucl_str("a=rwx");
	ATF_REQUIRE(ucl_parse_mode(obj, &m) == 0);
	ATF_CHECK_EQ(m, 0777);
	ucl_object_unref(obj);
}

ATF_TC_WITHOUT_HEAD(mode_str_chmod_u_rw);
ATF_TC_BODY(mode_str_chmod_u_rw, tc)
{
	ucl_object_t *obj;
	mode_t m;

	/* "u=rw" = 0600 */
	obj = ucl_str("u=rw");
	ATF_REQUIRE(ucl_parse_mode(obj, &m) == 0);
	ATF_CHECK_EQ(m, 0600);
	ucl_object_unref(obj);
}

/* ---- Error cases ---- */

ATF_TC_WITHOUT_HEAD(mode_str_invalid);
ATF_TC_BODY(mode_str_invalid, tc)
{
	ucl_object_t *obj;
	mode_t m;

	obj = ucl_str("not_a_mode");
	ATF_CHECK(ucl_parse_mode(obj, &m) == -1);
	ucl_object_unref(obj);
}

ATF_TC_WITHOUT_HEAD(mode_str_empty);
ATF_TC_BODY(mode_str_empty, tc)
{
	ucl_object_t *obj;
	mode_t m;

	obj = ucl_str("");
	/* Empty string: setmode returns NULL on some implementations */
	ucl_parse_mode(obj, &m);
	/* Just verify it doesn't crash */
	ucl_object_unref(obj);
}

ATF_TC_WITHOUT_HEAD(mode_wrong_type);
ATF_TC_BODY(mode_wrong_type, tc)
{
	ucl_object_t *obj;
	mode_t m;

	/* Boolean is neither INT nor STRING — should fail */
	obj = ucl_object_frombool(true);
	ATF_CHECK(ucl_parse_mode(obj, &m) == -1);
	ucl_object_unref(obj);
}

/* ---- UCL integration: parse from config string ---- */

ATF_TC_WITHOUT_HEAD(mode_ucl_string_octal);
ATF_TC_BODY(mode_ucl_string_octal, tc)
{
	struct ucl_parser *parser;
	ucl_object_t *top;
	const ucl_object_t *val;
	mode_t m;

	parser = ucl_parser_new(UCL_PARSER_DEFAULT);
	ATF_REQUIRE(ucl_parser_add_string(parser,
	    "control_permissions = \"0750\";", 0));
	top = ucl_parser_get_object(parser);
	ucl_parser_free(parser);
	ATF_REQUIRE(top != NULL);

	val = ucl_object_lookup(top, "control_permissions");
	ATF_REQUIRE(val != NULL);
	ATF_REQUIRE(ucl_parse_mode(val, &m) == 0);
	ATF_CHECK_EQ(m, 0750);

	ucl_object_unref(top);
}

ATF_TP_ADD_TCS(tp)
{

	/* Integer modes */
	ATF_TP_ADD_TC(tp, mode_int_0660);
	ATF_TP_ADD_TC(tp, mode_int_0755);
	ATF_TP_ADD_TC(tp, mode_int_0);
	ATF_TP_ADD_TC(tp, mode_int_0777);
	ATF_TP_ADD_TC(tp, mode_int_negative);
	ATF_TP_ADD_TC(tp, mode_int_too_large);

	/* Octal string modes */
	ATF_TP_ADD_TC(tp, mode_str_octal_0660);
	ATF_TP_ADD_TC(tp, mode_str_octal_0755);
	ATF_TP_ADD_TC(tp, mode_str_octal_0600);



	/* Symbolic string modes (chmod-style) */
	ATF_TP_ADD_TC(tp, mode_str_chmod_u_rw_g_rw);
	ATF_TP_ADD_TC(tp, mode_str_chmod_u_rwx_g_rx);
	ATF_TP_ADD_TC(tp, mode_str_chmod_a_rwx);
	ATF_TP_ADD_TC(tp, mode_str_chmod_u_rw);

	/* Error cases */
	ATF_TP_ADD_TC(tp, mode_str_invalid);
	ATF_TP_ADD_TC(tp, mode_str_empty);
	ATF_TP_ADD_TC(tp, mode_wrong_type);

	/* UCL integration */
	ATF_TP_ADD_TC(tp, mode_ucl_string_octal);

	return (atf_no_error());
}
