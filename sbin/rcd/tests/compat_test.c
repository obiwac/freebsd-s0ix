/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Baptiste Daroussin <bapt@FreeBSD.org>
 */

/*
 * Unit tests for the rc.d compatibility layer (header parsing).
 */

#include <sys/param.h>

#include <stdio.h>
#include <string.h>

#include <atf-c.h>

#include "rcd.h"

ATF_TC(parse_provide_header);
ATF_TC_HEAD(parse_provide_header, tc)
{
	atf_tc_set_md_var(tc, "descr", "Parse PROVIDE header from legacy script");
}
ATF_TC_BODY(parse_provide_header, tc)
{
	const char *srcdir = atf_tc_get_config_var(tc, "srcdir");
	char path[PATH_MAX];
	struct unit *u;

	u = unit_alloc();
	ATF_REQUIRE(u != NULL);

	snprintf(path, sizeof(path), "%s/%s", srcdir, "legacy_script");
	ATF_CHECK(compat_parse_headers(path, u) == 0);

	ATF_CHECK_EQ(u->u_provide.len, 1);
	ATF_CHECK_STREQ(u->u_provide.d[0], "test_legacy");

	unit_free(u);
}

ATF_TC(parse_require_header);
ATF_TC_HEAD(parse_require_header, tc)
{
	atf_tc_set_md_var(tc, "descr", "Parse REQUIRE header from legacy script");
}
ATF_TC_BODY(parse_require_header, tc)
{
	const char *srcdir = atf_tc_get_config_var(tc, "srcdir");
	char path[PATH_MAX];
	struct unit *u;

	u = unit_alloc();
	ATF_REQUIRE(u != NULL);

	snprintf(path, sizeof(path), "%s/%s", srcdir, "legacy_script");
	ATF_CHECK(compat_parse_headers(path, u) == 0);

	ATF_CHECK_EQ(u->u_require.len, 2);
	ATF_CHECK_STREQ(u->u_require.d[0], "NETWORKING");
	ATF_CHECK_STREQ(u->u_require.d[1], "FILESYSTEMS");

	unit_free(u);
}

ATF_TC(parse_before_header);
ATF_TC_HEAD(parse_before_header, tc)
{
	atf_tc_set_md_var(tc, "descr", "Parse BEFORE header from legacy script");
}
ATF_TC_BODY(parse_before_header, tc)
{
	const char *srcdir = atf_tc_get_config_var(tc, "srcdir");
	char path[PATH_MAX];
	struct unit *u;

	u = unit_alloc();
	ATF_REQUIRE(u != NULL);

	snprintf(path, sizeof(path), "%s/%s", srcdir, "legacy_script");
	ATF_CHECK(compat_parse_headers(path, u) == 0);

	ATF_CHECK_EQ(u->u_before.len, 1);
	ATF_CHECK_STREQ(u->u_before.d[0], "LOGIN");

	unit_free(u);
}

ATF_TC(parse_keyword_header);
ATF_TC_HEAD(parse_keyword_header, tc)
{
	atf_tc_set_md_var(tc, "descr", "Parse KEYWORD header from legacy script");
}
ATF_TC_BODY(parse_keyword_header, tc)
{
	const char *srcdir = atf_tc_get_config_var(tc, "srcdir");
	char path[PATH_MAX];
	struct unit *u;

	u = unit_alloc();
	ATF_REQUIRE(u != NULL);

	snprintf(path, sizeof(path), "%s/%s", srcdir, "legacy_script");
	ATF_CHECK(compat_parse_headers(path, u) == 0);

	ATF_CHECK_EQ(u->u_keyword.len, 1);
	ATF_CHECK_STREQ(u->u_keyword.d[0], "nojail");

	unit_free(u);
}

ATF_TC_WITHOUT_HEAD(parse_nonexistent_script);
ATF_TC_BODY(parse_nonexistent_script, tc)
{
	struct unit *u;

	u = unit_alloc();
	ATF_REQUIRE(u != NULL);

	ATF_CHECK(compat_parse_headers("/nonexistent/script", u) == -1);

	unit_free(u);
}

ATF_TC(detect_barrier_script);
ATF_TC_HEAD(detect_barrier_script, tc)
{
	atf_tc_set_md_var(tc, "descr", "Detect barrier script type");
}
ATF_TC_BODY(detect_barrier_script, tc)
{
	const char *srcdir = atf_tc_get_config_var(tc, "srcdir");
	char path[PATH_MAX];
	struct unit *u;

	u = unit_alloc();
	ATF_REQUIRE(u != NULL);

	u->u_type = UNIT_LEGACY;
	snprintf(path, sizeof(path), "%s/%s", srcdir, "barrier_script");
	ATF_CHECK(compat_parse_headers(path, u) == 0);

	ATF_CHECK(u->u_type == UNIT_BARRIER);
	ATF_CHECK_EQ(u->u_provide.len, 1);
	ATF_CHECK_STREQ(u->u_provide.d[0], "TEST_BARRIER");

	unit_free(u);
}

ATF_TC(detect_legacy_forking);
ATF_TC_HEAD(detect_legacy_forking, tc)
{
	atf_tc_set_md_var(tc, "descr", "Detect legacy forking daemon type");
}
ATF_TC_BODY(detect_legacy_forking, tc)
{
	const char *srcdir = atf_tc_get_config_var(tc, "srcdir");
	char path[PATH_MAX];
	struct unit *u;

	u = unit_alloc();
	ATF_REQUIRE(u != NULL);

	u->u_type = UNIT_LEGACY;
	snprintf(path, sizeof(path), "%s/%s", srcdir, "daemon_script");
	ATF_CHECK(compat_parse_headers(path, u) == 0);

	ATF_CHECK(u->u_type == UNIT_LEGACY_FORKING);
	ATF_CHECK_EQ(u->u_provide.len, 1);
	ATF_CHECK_STREQ(u->u_provide.d[0], "test_daemon");

	unit_free(u);
}

ATF_TC(detect_legacy_forking_by_command);
ATF_TC_HEAD(detect_legacy_forking_by_command, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Detect legacy forking daemon by command variable");
}
ATF_TC_BODY(detect_legacy_forking_by_command, tc)
{
	const char *srcdir = atf_tc_get_config_var(tc, "srcdir");
	char path[PATH_MAX];
	struct unit *u;

	u = unit_alloc();
	ATF_REQUIRE(u != NULL);

	u->u_type = UNIT_LEGACY;
	snprintf(path, sizeof(path), "%s/%s", srcdir, "daemon_script2");
	ATF_CHECK(compat_parse_headers(path, u) == 0);

	/* command= without start_cmd= -> UNIT_LEGACY_FORKING */
	ATF_CHECK(u->u_type == UNIT_LEGACY_FORKING);
	ATF_CHECK_EQ(u->u_provide.len, 1);
	ATF_CHECK_STREQ(u->u_provide.d[0], "test_daemon2");

	unit_free(u);
}

ATF_TC(detect_legacy_oneshot);
ATF_TC_HEAD(detect_legacy_oneshot, tc)
{
	atf_tc_set_md_var(tc, "descr", "Detect legacy oneshot script type");
}
ATF_TC_BODY(detect_legacy_oneshot, tc)
{
	const char *srcdir = atf_tc_get_config_var(tc, "srcdir");
	char path[PATH_MAX];
	struct unit *u;

	u = unit_alloc();
	ATF_REQUIRE(u != NULL);

	u->u_type = UNIT_LEGACY;
	snprintf(path, sizeof(path), "%s/%s", srcdir, "oneshot_script");
	ATF_CHECK(compat_parse_headers(path, u) == 0);

	/* start_cmd= present -> stays UNIT_LEGACY (oneshot) */
	ATF_CHECK(u->u_type == UNIT_LEGACY);
	ATF_CHECK_EQ(u->u_provide.len, 1);
	ATF_CHECK_STREQ(u->u_provide.d[0], "test_oneshot");

	unit_free(u);
}

ATF_TC(detect_rcvar);
ATF_TC_HEAD(detect_rcvar, tc)
{
	atf_tc_set_md_var(tc, "descr", "Detect rcvar presence in scripts");
}
ATF_TC_BODY(detect_rcvar, tc)
{
	const char *srcdir = atf_tc_get_config_var(tc, "srcdir");
	char path[PATH_MAX];
	struct unit *u;

	/* Script with rcvar= should have u_has_rcvar set */
	u = unit_alloc();
	ATF_REQUIRE(u != NULL);
	u->u_type = UNIT_LEGACY;
	snprintf(path, sizeof(path), "%s/%s", srcdir, "daemon_script");
	ATF_CHECK(compat_parse_headers(path, u) == 0);
	ATF_CHECK(u->u_has_rcvar == true);
	unit_free(u);

	/* Script without rcvar= should not have u_has_rcvar set */
	u = unit_alloc();
	ATF_REQUIRE(u != NULL);
	u->u_type = UNIT_LEGACY;
	snprintf(path, sizeof(path), "%s/%s", srcdir, "daemon_script2");
	ATF_CHECK(compat_parse_headers(path, u) == 0);
	ATF_CHECK(u->u_has_rcvar == false);
	unit_free(u);
}

ATF_TC(scan_directory);
ATF_TC_HEAD(scan_directory, tc)
{
	atf_tc_set_md_var(tc, "descr", "Scan a directory for legacy scripts");
}
ATF_TC_BODY(scan_directory, tc)
{
	const char *srcdir = atf_tc_get_config_var(tc, "srcdir");
	struct rcd_ctx ctx;
	struct unit *u;

	memset(&ctx, 0, sizeof(ctx));
	depgraph_init(&ctx.ctx_graph);
	ctx.ctx_config.cfg_rcvars = hash_new();

	/* Scanning a nonexistent directory should succeed quietly */
	ATF_CHECK(compat_scan(&ctx, "/nonexistent/dir") == 0);

	/* Scanning the data directory should find our legacy script */
	ATF_CHECK(compat_scan(&ctx, srcdir) == 0);

	/*
	 * Check if the legacy script was loaded.  Note: compat_scan
	 * only loads files without a .ucl extension, so it should find
	 * legacy_script.
	 */
	TAILQ_FOREACH(u, &ctx.ctx_graph.dg_units, u_entries) {
		if (strcmp(u->u_name, "test_legacy") == 0) {
			ATF_CHECK(u->u_type == UNIT_LEGACY ||
			    u->u_type == UNIT_LEGACY_FORKING);
			ATF_CHECK_EQ(u->u_provide.len, 1);
			break;
		}
	}

	depgraph_free(&ctx.ctx_graph);
}

ATF_TC(norcd_keyword);
ATF_TC_HEAD(norcd_keyword, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Scripts with KEYWORD: NORCD are skipped by compat_scan");
}
ATF_TC_BODY(norcd_keyword, tc)
{
	const char *srcdir = atf_tc_get_config_var(tc, "srcdir");
	struct rcd_ctx ctx;
	struct unit *u;

	memset(&ctx, 0, sizeof(ctx));
	depgraph_init(&ctx.ctx_graph);
	ctx.ctx_config.cfg_rcvars = hash_new();

	ATF_CHECK(compat_scan(&ctx, srcdir) == 0);

	/* norcd_test must NOT appear in the graph */
	TAILQ_FOREACH(u, &ctx.ctx_graph.dg_units, u_entries)
		ATF_CHECK_MSG(strcmp(u->u_name, "norcd_test") != 0,
		    "norcd_test should have been skipped");

	depgraph_free(&ctx.ctx_graph);
}

ATF_TP_ADD_TCS(tp)
{

	ATF_TP_ADD_TC(tp, parse_provide_header);
	ATF_TP_ADD_TC(tp, parse_require_header);
	ATF_TP_ADD_TC(tp, parse_before_header);
	ATF_TP_ADD_TC(tp, parse_keyword_header);
	ATF_TP_ADD_TC(tp, parse_nonexistent_script);
	ATF_TP_ADD_TC(tp, detect_barrier_script);
	ATF_TP_ADD_TC(tp, detect_legacy_forking);
	ATF_TP_ADD_TC(tp, detect_legacy_forking_by_command);
	ATF_TP_ADD_TC(tp, detect_legacy_oneshot);
	ATF_TP_ADD_TC(tp, detect_rcvar);
	ATF_TP_ADD_TC(tp, scan_directory);
	ATF_TP_ADD_TC(tp, norcd_keyword);

	return (atf_no_error());
}
