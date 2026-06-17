/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Baptiste Daroussin <bapt@FreeBSD.org>
 */

/*
 * Tests for enable.c: enable/disable/delete override persistence.
 * Requires root (writes to /etc/rcd.conf.d/).
 */

#include <sys/param.h>
#include <sys/stat.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <atf-c.h>
#include <ucl.h>

#include "rcd.h"

#define CONFD	"/etc/rcd.conf.d"
#define SVC	"_rcd_test_enable"

/* Read back and verify the enable state from the override file. */
static bool
read_enable_state(const char *name)
{
	struct ucl_parser *parser;
	ucl_object_t *top;
	const ucl_object_t *val;
	char path[PATH_MAX];
	bool enabled;

	snprintf(path, sizeof(path), "%s/%s", CONFD, name);
	parser = ucl_parser_new(UCL_PARSER_DEFAULT);
	if (!ucl_parser_add_file(parser, path)) {
		ucl_parser_free(parser);
		return (false);
	}
	top = ucl_parser_get_object(parser);
	ucl_parser_free(parser);
	if (top == NULL)
		return (false);

	val = ucl_object_lookup(top, "enable");
	enabled = (val != NULL && ucl_object_toboolean(val));
	ucl_object_unref(top);
	return (enabled);
}

ATF_TC_WITH_CLEANUP(enable_creates_file);
ATF_TC_HEAD(enable_creates_file, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "enable_service creates override file with enable=true");
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(enable_creates_file, tc)
{
	char path[PATH_MAX];

	/* Clean up any leftover */
	snprintf(path, sizeof(path), "%s/%s", CONFD, SVC);
	unlink(path);

	ATF_REQUIRE(enable_service(SVC, NULL) == 0);
	ATF_CHECK(read_enable_state(SVC) == true);
}
ATF_TC_CLEANUP(enable_creates_file, tc)
{
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/%s", CONFD, SVC);
	unlink(path);
}

ATF_TC_WITH_CLEANUP(disable_sets_false);
ATF_TC_HEAD(disable_sets_false, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "disable_service sets enable=false");
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(disable_sets_false, tc)
{
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "%s/%s", CONFD, SVC);
	unlink(path);

	ATF_REQUIRE(enable_service(SVC, NULL) == 0);
	ATF_CHECK(read_enable_state(SVC) == true);

	ATF_REQUIRE(disable_service(SVC, NULL) == 0);
	ATF_CHECK(read_enable_state(SVC) == false);
}
ATF_TC_CLEANUP(disable_sets_false, tc)
{
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/%s", CONFD, SVC);
	unlink(path);
}

ATF_TC_WITH_CLEANUP(enable_preserves_overrides);
ATF_TC_HEAD(enable_preserves_overrides, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "enable_service preserves existing override keys");
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(enable_preserves_overrides, tc)
{
	struct ucl_parser *parser;
	ucl_object_t *top;
	const ucl_object_t *val;
	char path[PATH_MAX];
	FILE *fp;

	snprintf(path, sizeof(path), "%s/%s", CONFD, SVC);
	unlink(path);

	/* Create an override file with extra keys */
	mkdir(CONFD, 0755);
	fp = fopen(path, "w");
	ATF_REQUIRE(fp != NULL);
	fprintf(fp, "enable = false;\ncommand = \"/usr/local/bin/test\";\n");
	fclose(fp);

	/* Enable should preserve 'command' */
	ATF_REQUIRE(enable_service(SVC, NULL) == 0);

	parser = ucl_parser_new(UCL_PARSER_DEFAULT);
	ATF_REQUIRE(ucl_parser_add_file(parser, path));
	top = ucl_parser_get_object(parser);
	ucl_parser_free(parser);
	ATF_REQUIRE(top != NULL);

	val = ucl_object_lookup(top, "enable");
	ATF_CHECK(val != NULL);
	ATF_CHECK(ucl_object_toboolean(val) == true);

	val = ucl_object_lookup(top, "command");
	ATF_CHECK(val != NULL);
	ATF_CHECK_STREQ(ucl_object_tostring(val), "/usr/local/bin/test");

	ucl_object_unref(top);
}
ATF_TC_CLEANUP(enable_preserves_overrides, tc)
{
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/%s", CONFD, SVC);
	unlink(path);
}

ATF_TC_WITH_CLEANUP(delete_removes_file);
ATF_TC_HEAD(delete_removes_file, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "delete_override removes the file");
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(delete_removes_file, tc)
{
	struct stat sb;
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "%s/%s", CONFD, SVC);
	unlink(path);

	ATF_REQUIRE(enable_service(SVC, NULL) == 0);
	ATF_CHECK(stat(path, &sb) == 0);

	ATF_REQUIRE(delete_override(SVC) == 0);
	ATF_CHECK(stat(path, &sb) != 0);
	ATF_CHECK(errno == ENOENT);
}
ATF_TC_CLEANUP(delete_removes_file, tc)
{
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/%s", CONFD, SVC);
	unlink(path);
}

ATF_TC(delete_nonexistent);
ATF_TC_HEAD(delete_nonexistent, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "delete_override on nonexistent file succeeds");
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(delete_nonexistent, tc)
{

	ATF_CHECK(delete_override("_rcd_no_such_service") == 0);
}

ATF_TC(reject_path_traversal);
ATF_TC_HEAD(reject_path_traversal, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Service names with path traversal are rejected");
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(reject_path_traversal, tc)
{

	ATF_CHECK(enable_service("../etc/passwd", NULL) != 0);
	ATF_CHECK(enable_service("foo/bar", NULL) != 0);
	ATF_CHECK(delete_override("../etc/passwd") != 0);
}

ATF_TP_ADD_TCS(tp)
{

	ATF_TP_ADD_TC(tp, enable_creates_file);
	ATF_TP_ADD_TC(tp, disable_sets_false);
	ATF_TP_ADD_TC(tp, enable_preserves_overrides);
	ATF_TP_ADD_TC(tp, delete_removes_file);
	ATF_TP_ADD_TC(tp, delete_nonexistent);
	ATF_TP_ADD_TC(tp, reject_path_traversal);

	return (atf_no_error());
}
