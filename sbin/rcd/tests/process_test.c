/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Baptiste Daroussin <bapt@FreeBSD.org>
 */

/*
 * Tests for process.c: preconditions, module loading, hook execution.
 */

#include <sys/param.h>
#include <sys/stat.h>
#include <sys/sysctl.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <atf-c.h>

#include "rcd.h"

/* ── proc_check_preconditions ── */

ATF_TC(precond_dirs_ok);
ATF_TC_HEAD(precond_dirs_ok, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Preconditions pass when required dirs exist");
}
ATF_TC_BODY(precond_dirs_ok, tc)
{
	struct unit *u;

	u = unit_alloc();
	u->u_name = xstrdup("test");
	vec_push(&u->u_required_dirs, xstrdup("/tmp"));
	vec_push(&u->u_required_dirs, xstrdup("/var"));

	ATF_CHECK(proc_check_preconditions(u) == 0);
	unit_free(u);
}

ATF_TC(precond_dirs_missing);
ATF_TC_HEAD(precond_dirs_missing, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Preconditions fail when required dir is missing");
}
ATF_TC_BODY(precond_dirs_missing, tc)
{
	struct unit *u;

	u = unit_alloc();
	u->u_name = xstrdup("test");
	vec_push(&u->u_required_dirs, xstrdup("/nonexistent_dir_12345"));

	ATF_CHECK(proc_check_preconditions(u) != 0);
	unit_free(u);
}

ATF_TC(precond_files_ok);
ATF_TC_HEAD(precond_files_ok, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Preconditions pass when required files are readable");
}
ATF_TC_BODY(precond_files_ok, tc)
{
	struct unit *u;

	u = unit_alloc();
	u->u_name = xstrdup("test");
	vec_push(&u->u_required_files, xstrdup("/etc/rc.conf"));

	ATF_CHECK(proc_check_preconditions(u) == 0);
	unit_free(u);
}

ATF_TC(precond_files_missing);
ATF_TC_HEAD(precond_files_missing, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Preconditions fail when required file is missing");
}
ATF_TC_BODY(precond_files_missing, tc)
{
	struct unit *u;

	u = unit_alloc();
	u->u_name = xstrdup("test");
	vec_push(&u->u_required_files, xstrdup("/no_such_file_99999"));

	ATF_CHECK(proc_check_preconditions(u) != 0);
	unit_free(u);
}

ATF_TC(precond_sysctl_match);
ATF_TC_HEAD(precond_sysctl_match, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Preconditions pass when sysctl matches expected value");
}
ATF_TC_BODY(precond_sysctl_match, tc)
{
	struct unit *u;
	struct kv *sc;
	char buf[256];
	size_t len;

	/* Read the actual hostname to use as expected value */
	len = sizeof(buf);
	if (sysctlbyname("kern.ostype", buf, &len, NULL, 0) != 0)
		atf_tc_skip("cannot read kern.ostype");
	if (len < sizeof(buf))
		buf[len] = '\0';

	u = unit_alloc();
	u->u_name = xstrdup("test");
	sc = xcalloc(1, sizeof(*sc));
	sc->kv_key = xstrdup("kern.ostype");
	sc->kv_val = xstrdup(buf);
	STAILQ_INSERT_TAIL(&u->u_required_sysctl, sc, kv_entries);

	ATF_CHECK(proc_check_preconditions(u) == 0);
	unit_free(u);
}

ATF_TC(precond_sysctl_mismatch);
ATF_TC_HEAD(precond_sysctl_mismatch, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Preconditions fail when sysctl doesn't match");
}
ATF_TC_BODY(precond_sysctl_mismatch, tc)
{
	struct unit *u;
	struct kv *sc;

	u = unit_alloc();
	u->u_name = xstrdup("test");
	sc = xcalloc(1, sizeof(*sc));
	sc->kv_key = xstrdup("kern.ostype");
	sc->kv_val = xstrdup("NOT_AN_OS");
	STAILQ_INSERT_TAIL(&u->u_required_sysctl, sc, kv_entries);

	ATF_CHECK(proc_check_preconditions(u) != 0);
	unit_free(u);
}

ATF_TC(precond_vars_present);
ATF_TC_HEAD(precond_vars_present, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Preconditions pass when required_vars are in override config");
}
ATF_TC_BODY(precond_vars_present, tc)
{
	struct unit *u;

	u = unit_alloc();
	u->u_name = xstrdup("test");
	vec_push(&u->u_required_vars, xstrdup("api_key"));
	u->u_override_conf = xstrdup("api_key = \"secret123\";");

	ATF_CHECK(proc_check_preconditions(u) == 0);
	unit_free(u);
}

ATF_TC(precond_vars_missing);
ATF_TC_HEAD(precond_vars_missing, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Preconditions fail when required_vars missing from config");
}
ATF_TC_BODY(precond_vars_missing, tc)
{
	struct unit *u;

	u = unit_alloc();
	u->u_name = xstrdup("test");
	vec_push(&u->u_required_vars, xstrdup("api_key"));
	u->u_override_conf = xstrdup("other_key = \"value\";");

	ATF_CHECK(proc_check_preconditions(u) != 0);
	unit_free(u);
}

ATF_TC(precond_vars_no_config);
ATF_TC_HEAD(precond_vars_no_config, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Preconditions fail when required_vars set but no override");
}
ATF_TC_BODY(precond_vars_no_config, tc)
{
	struct unit *u;

	u = unit_alloc();
	u->u_name = xstrdup("test");
	vec_push(&u->u_required_vars, xstrdup("api_key"));
	/* u->u_override_conf is NULL */

	ATF_CHECK(proc_check_preconditions(u) != 0);
	unit_free(u);
}

ATF_TC(precond_empty);
ATF_TC_HEAD(precond_empty, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "No preconditions always passes");
}
ATF_TC_BODY(precond_empty, tc)
{
	struct unit *u;

	u = unit_alloc();
	u->u_name = xstrdup("test");

	ATF_CHECK(proc_check_preconditions(u) == 0);
	unit_free(u);
}

/* ── tokenize ── */

ATF_TC(tokenize_simple);
ATF_TC_HEAD(tokenize_simple, tc)
{
	atf_tc_set_md_var(tc, "descr", "Tokenize simple space-separated words");
}
ATF_TC_BODY(tokenize_simple, tc)
{
	charv_t v = vec_init();

	tokenize("hello world foo", &v);
	ATF_CHECK_EQ(v.len, 3);
	ATF_CHECK_STREQ(v.d[0], "hello");
	ATF_CHECK_STREQ(v.d[1], "world");
	ATF_CHECK_STREQ(v.d[2], "foo");
	vec_free_and_free(&v, free);
}

ATF_TC(tokenize_quoted);
ATF_TC_HEAD(tokenize_quoted, tc)
{
	atf_tc_set_md_var(tc, "descr", "Tokenize handles single and double quotes");
}
ATF_TC_BODY(tokenize_quoted, tc)
{
	charv_t v = vec_init();

	tokenize("\"hello world\" 'foo bar' baz", &v);
	ATF_CHECK_EQ(v.len, 3);
	ATF_CHECK_STREQ(v.d[0], "hello world");
	ATF_CHECK_STREQ(v.d[1], "foo bar");
	ATF_CHECK_STREQ(v.d[2], "baz");
	vec_free_and_free(&v, free);
}

ATF_TC(tokenize_empty);
ATF_TC_HEAD(tokenize_empty, tc)
{
	atf_tc_set_md_var(tc, "descr", "Tokenize empty string yields nothing");
}
ATF_TC_BODY(tokenize_empty, tc)
{
	charv_t v = vec_init();

	tokenize("", &v);
	ATF_CHECK_EQ(v.len, 0);
	vec_free_and_free(&v, free);
}

ATF_TC(tokenize_whitespace_only);
ATF_TC_HEAD(tokenize_whitespace_only, tc)
{
	atf_tc_set_md_var(tc, "descr", "Tokenize whitespace-only string yields nothing");
}
ATF_TC_BODY(tokenize_whitespace_only, tc)
{
	charv_t v = vec_init();

	tokenize("   \t  \t  ", &v);
	ATF_CHECK_EQ(v.len, 0);
	vec_free_and_free(&v, free);
}

ATF_TC(tokenize_tabs);
ATF_TC_HEAD(tokenize_tabs, tc)
{
	atf_tc_set_md_var(tc, "descr", "Tokenize splits on tabs too");
}
ATF_TC_BODY(tokenize_tabs, tc)
{
	charv_t v = vec_init();

	tokenize("a\tb\t\tc", &v);
	ATF_CHECK_EQ(v.len, 3);
	ATF_CHECK_STREQ(v.d[0], "a");
	ATF_CHECK_STREQ(v.d[1], "b");
	ATF_CHECK_STREQ(v.d[2], "c");
	vec_free_and_free(&v, free);
}

ATF_TC(tokenize_dq_escape);
ATF_TC_HEAD(tokenize_dq_escape, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Tokenize handles backslash escapes inside double quotes");
}
ATF_TC_BODY(tokenize_dq_escape, tc)
{
	charv_t v = vec_init();

	tokenize("\"hello\\\"world\" 'no\\'escape'", &v);
	ATF_CHECK_EQ(v.len, 2);
	ATF_CHECK_STREQ(v.d[0], "hello\"world");
	ATF_CHECK_STREQ(v.d[1], "no\\'escape");
	vec_free_and_free(&v, free);
}

ATF_TC(tokenize_dq_backslash);
ATF_TC_HEAD(tokenize_dq_backslash, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Tokenize handles \\\\ escape inside double quotes");
}
ATF_TC_BODY(tokenize_dq_backslash, tc)
{
	charv_t v = vec_init();

	tokenize("\"path\\\\to\\\\file\"", &v);
	ATF_CHECK_EQ(v.len, 1);
	ATF_CHECK_STREQ(v.d[0], "path\\to\\file");
	vec_free_and_free(&v, free);
}

/* ── proc_run_hook ── */

ATF_TC(hook_success);
ATF_TC_HEAD(hook_success, tc)
{
	atf_tc_set_md_var(tc, "descr", "Hook returning 0 succeeds");
}
ATF_TC_BODY(hook_success, tc)
{

	ATF_CHECK(proc_run_hook("/usr/bin/true") == 0);
}

ATF_TC(hook_failure);
ATF_TC_HEAD(hook_failure, tc)
{
	atf_tc_set_md_var(tc, "descr", "Hook returning non-zero fails");
}
ATF_TC_BODY(hook_failure, tc)
{

	ATF_CHECK(proc_run_hook("/usr/bin/false") != 0);
}

ATF_TC(hook_null);
ATF_TC_HEAD(hook_null, tc)
{
	atf_tc_set_md_var(tc, "descr", "NULL hook is a no-op success");
}
ATF_TC_BODY(hook_null, tc)
{

	ATF_CHECK(proc_run_hook(NULL) == 0);
}

ATF_TC(hook_empty);
ATF_TC_HEAD(hook_empty, tc)
{
	atf_tc_set_md_var(tc, "descr", "Empty string hook is a no-op success");
}
ATF_TC_BODY(hook_empty, tc)
{

	ATF_CHECK(proc_run_hook("") == 0);
}

ATF_TP_ADD_TCS(tp)
{

	ATF_TP_ADD_TC(tp, precond_dirs_ok);
	ATF_TP_ADD_TC(tp, precond_dirs_missing);
	ATF_TP_ADD_TC(tp, precond_files_ok);
	ATF_TP_ADD_TC(tp, precond_files_missing);
	ATF_TP_ADD_TC(tp, precond_sysctl_match);
	ATF_TP_ADD_TC(tp, precond_sysctl_mismatch);
	ATF_TP_ADD_TC(tp, precond_vars_present);
	ATF_TP_ADD_TC(tp, precond_vars_missing);
	ATF_TP_ADD_TC(tp, precond_vars_no_config);
	ATF_TP_ADD_TC(tp, precond_empty);
	ATF_TP_ADD_TC(tp, hook_success);
	ATF_TP_ADD_TC(tp, hook_failure);
	ATF_TP_ADD_TC(tp, hook_null);
	ATF_TP_ADD_TC(tp, hook_empty);
	ATF_TP_ADD_TC(tp, tokenize_simple);
	ATF_TP_ADD_TC(tp, tokenize_quoted);
	ATF_TP_ADD_TC(tp, tokenize_empty);
	ATF_TP_ADD_TC(tp, tokenize_whitespace_only);
	ATF_TP_ADD_TC(tp, tokenize_tabs);
	ATF_TP_ADD_TC(tp, tokenize_dq_escape);
	ATF_TP_ADD_TC(tp, tokenize_dq_backslash);

	return (atf_no_error());
}
