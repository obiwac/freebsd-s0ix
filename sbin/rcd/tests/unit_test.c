/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Baptiste Daroussin <bapt@FreeBSD.org>
 */

/*
 * Unit tests for the UCL unit file parser.
 */

#include <sys/param.h>

#include <signal.h>
#include <stdio.h>
#include <string.h>

#include <atf-c.h>

#include "rcd.h"

ATF_TC(parse_simple_service);
ATF_TC_HEAD(parse_simple_service, tc)
{
	atf_tc_set_md_var(tc, "descr", "Parse a simple service unit file");
}
ATF_TC_BODY(parse_simple_service, tc)
{
	const char *srcdir = atf_tc_get_config_var(tc, "srcdir");
	char path[PATH_MAX];
	struct rcd_config cfg;
	struct unit *u;

	memset(&cfg, 0, sizeof(cfg));

	snprintf(path, sizeof(path), "%s/%s", srcdir, "sshd.ucl");
	u = unit_parse(path, &cfg);
	ATF_REQUIRE_MSG(u != NULL, "unit_parse returned NULL");
	ATF_CHECK_STREQ(u->u_name, "sshd");
	ATF_CHECK_STREQ(u->u_description, "Secure Shell Daemon");
	ATF_CHECK(u->u_type == UNIT_SIMPLE);
	ATF_CHECK_STREQ(u->u_command, "/usr/sbin/sshd");
	ATF_CHECK_STREQ(u->u_command_args, "-D -f /etc/ssh/sshd_config");
	ATF_CHECK(u->u_state == STATE_INACTIVE);

	unit_free(u);
}

ATF_TC(parse_dependencies);
ATF_TC_HEAD(parse_dependencies, tc)
{
	atf_tc_set_md_var(tc, "descr", "Parse unit dependency declarations");
}
ATF_TC_BODY(parse_dependencies, tc)
{
	const char *srcdir = atf_tc_get_config_var(tc, "srcdir");
	char path[PATH_MAX];
	struct rcd_config cfg;
	struct unit *u;

	memset(&cfg, 0, sizeof(cfg));

	snprintf(path, sizeof(path), "%s/%s", srcdir, "sshd.ucl");
	u = unit_parse(path, &cfg);
	ATF_REQUIRE(u != NULL);

	/* provide: sshd, ssh */
	ATF_CHECK_EQ(u->u_provide.len, 2);
	ATF_CHECK_STREQ(u->u_provide.d[0], "sshd");
	ATF_CHECK_STREQ(u->u_provide.d[1], "ssh");

	/* require: NETWORKING, FILESYSTEMS */
	ATF_CHECK_EQ(u->u_require.len, 2);
	ATF_CHECK_STREQ(u->u_require.d[0], "NETWORKING");
	ATF_CHECK_STREQ(u->u_require.d[1], "FILESYSTEMS");

	/* before: LOGIN */
	ATF_CHECK_EQ(u->u_before.len, 1);
	ATF_CHECK_STREQ(u->u_before.d[0], "LOGIN");

	unit_free(u);
}

ATF_TC(parse_restart_config);
ATF_TC_HEAD(parse_restart_config, tc)
{
	atf_tc_set_md_var(tc, "descr", "Parse restart configuration");
}
ATF_TC_BODY(parse_restart_config, tc)
{
	const char *srcdir = atf_tc_get_config_var(tc, "srcdir");
	char path[PATH_MAX];
	struct rcd_config cfg;
	struct unit *u;

	memset(&cfg, 0, sizeof(cfg));

	snprintf(path, sizeof(path), "%s/%s", srcdir, "sshd.ucl");
	u = unit_parse(path, &cfg);
	ATF_REQUIRE(u != NULL);

	ATF_CHECK(u->u_restart.rc_policy == RESTART_ON_FAILURE);
	ATF_CHECK_EQ(u->u_restart.rc_max_retries, 5);
	ATF_CHECK_EQ(u->u_restart.rc_delay_ms, 5000);
	ATF_CHECK(u->u_restart.rc_backoff == BACKOFF_EXPONENTIAL);
	ATF_CHECK_EQ(u->u_restart.rc_reset_ms, 60000);

	unit_free(u);
}

ATF_TC(parse_restart_always);
ATF_TC_HEAD(parse_restart_always, tc)
{
	atf_tc_set_md_var(tc, "descr", "Parse restart-always policy");
}
ATF_TC_BODY(parse_restart_always, tc)
{
	const char *srcdir = atf_tc_get_config_var(tc, "srcdir");
	char path[PATH_MAX];
	struct rcd_config cfg;
	struct unit *u;

	memset(&cfg, 0, sizeof(cfg));

	snprintf(path, sizeof(path), "%s/%s", srcdir, "nginx.ucl");
	u = unit_parse(path, &cfg);
	ATF_REQUIRE(u != NULL);

	ATF_CHECK(u->u_restart.rc_policy == RESTART_ALWAYS);
	ATF_CHECK_EQ(u->u_restart.rc_max_retries, 10);
	ATF_CHECK(u->u_restart.rc_backoff == BACKOFF_LINEAR);

	unit_free(u);
}

ATF_TC(parse_process_config);
ATF_TC_HEAD(parse_process_config, tc)
{
	atf_tc_set_md_var(tc, "descr", "Parse process configuration");
}
ATF_TC_BODY(parse_process_config, tc)
{
	const char *srcdir = atf_tc_get_config_var(tc, "srcdir");
	char path[PATH_MAX];
	struct rcd_config cfg;
	struct unit *u;

	memset(&cfg, 0, sizeof(cfg));

	snprintf(path, sizeof(path), "%s/%s", srcdir, "sshd.ucl");
	u = unit_parse(path, &cfg);
	ATF_REQUIRE(u != NULL);

	ATF_CHECK_STREQ(u->u_proc.pc_user, "root");
	ATF_CHECK_STREQ(u->u_proc.pc_group, "wheel");
	ATF_CHECK(u->u_proc.pc_oom_protect == true);

	unit_free(u);
}

ATF_TC(parse_process_cpuset);
ATF_TC_HEAD(parse_process_cpuset, tc)
{
	atf_tc_set_md_var(tc, "descr", "Parse process cpuset configuration");
}
ATF_TC_BODY(parse_process_cpuset, tc)
{
	const char *srcdir = atf_tc_get_config_var(tc, "srcdir");
	char path[PATH_MAX];
	struct rcd_config cfg;
	struct unit *u;

	memset(&cfg, 0, sizeof(cfg));

	snprintf(path, sizeof(path), "%s/%s", srcdir, "nginx.ucl");
	u = unit_parse(path, &cfg);
	ATF_REQUIRE(u != NULL);

	ATF_CHECK_STREQ(u->u_proc.pc_user, "www");
	ATF_CHECK_STREQ(u->u_proc.pc_group, "www");
	ATF_CHECK_STREQ(u->u_proc.pc_cpuset, "0-3");

	unit_free(u);
}

ATF_TC(parse_rctl_rules);
ATF_TC_HEAD(parse_rctl_rules, tc)
{
	atf_tc_set_md_var(tc, "descr", "Parse RCTL resource limit rules");
}
ATF_TC_BODY(parse_rctl_rules, tc)
{
	const char *srcdir = atf_tc_get_config_var(tc, "srcdir");
	char path[PATH_MAX];
	struct rcd_config cfg;
	struct unit *u;
	struct rctl_conf *rc;
	int count;

	memset(&cfg, 0, sizeof(cfg));

	snprintf(path, sizeof(path), "%s/%s", srcdir, "sshd.ucl");
	u = unit_parse(path, &cfg);
	ATF_REQUIRE(u != NULL);

	count = 0;
	STAILQ_FOREACH(rc, &u->u_rctl, rc_entries) {
		if (strcmp(rc->rc_resource, "memoryuse") == 0) {
			ATF_CHECK_STREQ(rc->rc_action, "deny");
			ATF_CHECK_STREQ(rc->rc_amount, "1g");
		} else if (strcmp(rc->rc_resource, "openfiles") == 0) {
			ATF_CHECK_STREQ(rc->rc_action, "deny");
			ATF_CHECK_STREQ(rc->rc_amount, "256");
		} else if (strcmp(rc->rc_resource, "maxproc") == 0) {
			ATF_CHECK_STREQ(rc->rc_action, "deny");
			ATF_CHECK_STREQ(rc->rc_amount, "32");
		}
		count++;
	}
	ATF_CHECK_EQ(count, 3);

	unit_free(u);
}

ATF_TC(parse_jail_config);
ATF_TC_HEAD(parse_jail_config, tc)
{
	atf_tc_set_md_var(tc, "descr", "Parse jail configuration");
}
ATF_TC_BODY(parse_jail_config, tc)
{
	const char *srcdir = atf_tc_get_config_var(tc, "srcdir");
	char path[PATH_MAX];
	struct rcd_config cfg;
	struct unit *u;

	memset(&cfg, 0, sizeof(cfg));

	snprintf(path, sizeof(path), "%s/%s", srcdir, "sshd.ucl");
	u = unit_parse(path, &cfg);
	ATF_REQUIRE(u != NULL);

	ATF_CHECK(u->u_jail.jc_enable == true);
	ATF_CHECK_EQ(u->u_jail.jc_options.len, 1);
	ATF_CHECK_STREQ(u->u_jail.jc_options.d[0], "netv4");
	ATF_CHECK_EQ(u->u_jail.jc_ip4addr.len, 1);
	ATF_CHECK_STREQ(u->u_jail.jc_ip4addr.d[0], "192.0.2.1");

	unit_free(u);
}

ATF_TC(parse_socket_activation);
ATF_TC_HEAD(parse_socket_activation, tc)
{
	atf_tc_set_md_var(tc, "descr", "Parse socket activation configuration");
}
ATF_TC_BODY(parse_socket_activation, tc)
{
	const char *srcdir = atf_tc_get_config_var(tc, "srcdir");
	char path[PATH_MAX];
	struct rcd_config cfg;
	struct unit *u;
	struct unit_socket *us;

	memset(&cfg, 0, sizeof(cfg));

	snprintf(path, sizeof(path), "%s/%s", srcdir, "sshd.ucl");
	u = unit_parse(path, &cfg);
	ATF_REQUIRE(u != NULL);

	us = TAILQ_FIRST(&u->u_sockets);
	ATF_REQUIRE(us != NULL);
	ATF_CHECK_STREQ(us->us_name, "ssh");
	ATF_CHECK_STREQ(us->us_address, "tcp:*:22");
	ATF_CHECK(us->us_type == SOCK_ACT_STREAM);
	ATF_CHECK_EQ(us->us_backlog, 128);
	ATF_CHECK_EQ(us->us_fd, -1);

	unit_free(u);
}

ATF_TC(parse_environment);
ATF_TC_HEAD(parse_environment, tc)
{
	atf_tc_set_md_var(tc, "descr", "Parse environment variable settings");
}
ATF_TC_BODY(parse_environment, tc)
{
	const char *srcdir = atf_tc_get_config_var(tc, "srcdir");
	char path[PATH_MAX];
	struct rcd_config cfg;
	struct unit *u;
	struct kv *ue;
	int count;

	memset(&cfg, 0, sizeof(cfg));

	snprintf(path, sizeof(path), "%s/%s", srcdir, "sshd.ucl");
	u = unit_parse(path, &cfg);
	ATF_REQUIRE(u != NULL);

	count = 0;
	STAILQ_FOREACH(ue, &u->u_env, kv_entries) {
		if (strcmp(ue->kv_key, "LC_ALL") == 0)
			ATF_CHECK_STREQ(ue->kv_val, "C");
		else if (strcmp(ue->kv_key, "PATH") == 0)
			ATF_CHECK_STREQ(ue->kv_val,
			    "/sbin:/bin:/usr/sbin:/usr/bin");
		count++;
	}
	ATF_CHECK_EQ(count, 2);

	unit_free(u);
}

ATF_TC(parse_logging);
ATF_TC_HEAD(parse_logging, tc)
{
	atf_tc_set_md_var(tc, "descr", "Parse logging configuration");
}
ATF_TC_BODY(parse_logging, tc)
{
	const char *srcdir = atf_tc_get_config_var(tc, "srcdir");
	char path[PATH_MAX];
	struct rcd_config cfg;
	struct unit *u;

	memset(&cfg, 0, sizeof(cfg));

	snprintf(path, sizeof(path), "%s/%s", srcdir, "sshd.ucl");
	u = unit_parse(path, &cfg);
	ATF_REQUIRE(u != NULL);

	ATF_CHECK_STREQ(u->u_log.lc_stdout, "syslog:daemon.info");
	ATF_CHECK_STREQ(u->u_log.lc_stderr, "syslog:daemon.err");

	unit_free(u);
}

ATF_TC(parse_oneshot);
ATF_TC_HEAD(parse_oneshot, tc)
{
	atf_tc_set_md_var(tc, "descr", "Parse a oneshot unit file");
}
ATF_TC_BODY(parse_oneshot, tc)
{
	const char *srcdir = atf_tc_get_config_var(tc, "srcdir");
	char path[PATH_MAX];
	struct rcd_config cfg;
	struct unit *u;

	memset(&cfg, 0, sizeof(cfg));

	snprintf(path, sizeof(path), "%s/%s", srcdir, "cleartmp.ucl");
	u = unit_parse(path, &cfg);
	ATF_REQUIRE(u != NULL);
	ATF_CHECK_STREQ(u->u_name, "cleartmp");
	ATF_CHECK(u->u_type == UNIT_ONESHOT);
	ATF_CHECK_EQ(u->u_provide.len, 1);
	ATF_CHECK_STREQ(u->u_provide.d[0], "cleartmp");

	unit_free(u);
}

ATF_TC_WITHOUT_HEAD(parse_nonexistent_file);
ATF_TC_BODY(parse_nonexistent_file, tc)
{
	struct rcd_config cfg;
	struct unit *u;

	memset(&cfg, 0, sizeof(cfg));
	u = unit_parse("/nonexistent/path.ucl", &cfg);
	ATF_CHECK(u == NULL);
}

ATF_TC_WITHOUT_HEAD(unit_alloc_defaults);
ATF_TC_BODY(unit_alloc_defaults, tc)
{
	struct unit *u;

	u = unit_alloc();
	ATF_REQUIRE(u != NULL);
	ATF_CHECK(u->u_state == STATE_INACTIVE);
	ATF_CHECK(u->u_type == UNIT_SIMPLE);
	ATF_CHECK(u->u_procdesc_fd == -1);
	ATF_CHECK(u->u_pid == -1);
	ATF_CHECK(u->u_enabled == false);
	ATF_CHECK(u->u_ready_method == READY_IMMEDIATE);
	ATF_CHECK(u->u_restart.rc_policy == RESTART_NEVER);
	ATF_CHECK(u->u_restart.rc_delay_ms == 5000);
	ATF_CHECK(u->u_restart.rc_max_retries == 5);

	unit_free(u);
}

ATF_TC(parse_command_prepend);
ATF_TC_HEAD(parse_command_prepend, tc)
{
	atf_tc_set_md_var(tc, "descr", "Parse command prepend setting");
}
ATF_TC_BODY(parse_command_prepend, tc)
{
	const char *srcdir = atf_tc_get_config_var(tc, "srcdir");
	char path[PATH_MAX];
	struct rcd_config cfg;
	struct unit *u;

	memset(&cfg, 0, sizeof(cfg));

	snprintf(path, sizeof(path), "%s/%s", srcdir, "prepend.ucl");
	u = unit_parse(path, &cfg);
	ATF_REQUIRE_MSG(u != NULL, "unit_parse returned NULL");
	ATF_CHECK_STREQ(u->u_name, "prepend_test");
	ATF_CHECK_STREQ(u->u_command, "/usr/bin/test");
	ATF_CHECK_STREQ(u->u_command_prepend, "/usr/bin/strace");

	unit_free(u);
}

ATF_TC(parse_setup_cmd);
ATF_TC_HEAD(parse_setup_cmd, tc)
{
	atf_tc_set_md_var(tc, "descr", "Parse setup and hook commands");
}
ATF_TC_BODY(parse_setup_cmd, tc)
{
	const char *srcdir = atf_tc_get_config_var(tc, "srcdir");
	char path[PATH_MAX];
	struct rcd_config cfg;
	struct unit *u;

	memset(&cfg, 0, sizeof(cfg));

	snprintf(path, sizeof(path), "%s/%s", srcdir, "hooks.ucl");
	u = unit_parse(path, &cfg);
	ATF_REQUIRE_MSG(u != NULL, "unit_parse returned NULL");
	ATF_CHECK_STREQ(u->u_name, "hooks_test");
	ATF_CHECK_STREQ(u->u_setup_cmd, "/usr/bin/setup");
	ATF_CHECK_STREQ(u->u_start_precmd, "/usr/bin/prestart");
	ATF_CHECK_STREQ(u->u_stop_postcmd, "/usr/bin/poststop");

	unit_free(u);
}

ATF_TC(parse_enable_false);
ATF_TC_HEAD(parse_enable_false, tc)
{
	atf_tc_set_md_var(tc, "descr", "Parse disabled unit file");
}
ATF_TC_BODY(parse_enable_false, tc)
{
	const char *srcdir = atf_tc_get_config_var(tc, "srcdir");
	char path[PATH_MAX];
	struct rcd_config cfg;
	struct unit *u;

	memset(&cfg, 0, sizeof(cfg));

	snprintf(path, sizeof(path), "%s/%s", srcdir, "disabled.ucl");
	u = unit_parse(path, &cfg);
	ATF_REQUIRE_MSG(u != NULL, "unit_parse returned NULL");
	ATF_CHECK_STREQ(u->u_name, "disabled_test");
	ATF_CHECK(u->u_enabled == false);

	unit_free(u);
}

ATF_TC(parse_required_vars);
ATF_TC_HEAD(parse_required_vars, tc)
{
	atf_tc_set_md_var(tc, "descr", "Parse required variables");
}
ATF_TC_BODY(parse_required_vars, tc)
{
	const char *srcdir = atf_tc_get_config_var(tc, "srcdir");
	char path[PATH_MAX];
	struct rcd_config cfg;
	struct unit *u;

	memset(&cfg, 0, sizeof(cfg));

	snprintf(path, sizeof(path), "%s/%s", srcdir, "reqvars.ucl");
	u = unit_parse(path, &cfg);
	ATF_REQUIRE_MSG(u != NULL, "unit_parse returned NULL");
	ATF_CHECK_STREQ(u->u_name, "reqvars_test");
	ATF_CHECK_EQ(u->u_required_vars.len, 2);
	ATF_CHECK_STREQ(u->u_required_vars.d[0], "api_key");
	ATF_CHECK_STREQ(u->u_required_vars.d[1], "db_host");

	unit_free(u);
}

ATF_TC(parse_off_command);
ATF_TC_HEAD(parse_off_command, tc)
{
	atf_tc_set_md_var(tc, "descr", "Parse off command setting");
}
ATF_TC_BODY(parse_off_command, tc)
{
	const char *srcdir = atf_tc_get_config_var(tc, "srcdir");
	char path[PATH_MAX];
	struct rcd_config cfg;
	struct unit *u;

	memset(&cfg, 0, sizeof(cfg));

	snprintf(path, sizeof(path), "%s/%s", srcdir, "offcmd.ucl");
	u = unit_parse(path, &cfg);
	ATF_REQUIRE_MSG(u != NULL, "unit_parse returned NULL");
	ATF_CHECK_STREQ(u->u_name, "offcmd_test");
	ATF_CHECK_STREQ(u->u_off_command, "/usr/bin/cleanup");

	unit_free(u);
}

/* ── Override tests ── */

ATF_TC(override_append_and_remove);
ATF_TC_HEAD(override_append_and_remove, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Override: append arrays, remove entries, replace scalars");
}
ATF_TC_BODY(override_append_and_remove, tc)
{
	const char *srcdir = atf_tc_get_config_var(tc, "srcdir");
	char path[PATH_MAX], confdir[PATH_MAX];
	struct rcd_config cfg;
	struct unit *u;

	memset(&cfg, 0, sizeof(cfg));

	snprintf(path, sizeof(path), "%s/%s", srcdir, "override_base.ucl");
	u = unit_parse(path, &cfg);
	ATF_REQUIRE_MSG(u != NULL, "unit_parse returned NULL");

	/* Pre-override checks */
	ATF_CHECK_EQ(u->u_provide.len, 2);
	ATF_CHECK_EQ(u->u_require.len, 1);
	ATF_CHECK_EQ(u->u_sig_stop, SIGTERM);

	/* Apply override */
	snprintf(confdir, sizeof(confdir), "%s/%s", srcdir, "confdir");
	unit_apply_overrides(u, confdir);

	/* Scalar: command replaced */
	ATF_CHECK_STREQ(u->u_command, "/usr/local/bin/overridden");

	/* Scalar: sig_stop replaced (SIGUSR1 = 30 on FreeBSD) */
	ATF_CHECK_EQ(u->u_sig_stop, SIGUSR1);

	/* Scalar: start_delay set */
	ATF_CHECK_EQ(u->u_start_delay_ms, 500);

	/* Scalar: hook set */
	ATF_CHECK(u->u_start_precmd != NULL);
	ATF_CHECK_STREQ(u->u_start_precmd, "/usr/bin/precheck");

	/* remove{}: base_alias removed from provides */
	ATF_CHECK_EQ(u->u_provide.len, 1);
	ATF_CHECK_STREQ(u->u_provide.d[0], "override_base");

	/* Array append: requires was ["NETWORKING"], now has FILESYSTEMS too */
	/* NETWORKING is a dup, so only FILESYSTEMS is appended */
	ATF_CHECK_EQ(u->u_require.len, 2);
	ATF_CHECK_STREQ(u->u_require.d[0], "NETWORKING");
	ATF_CHECK_STREQ(u->u_require.d[1], "FILESYSTEMS");

	/* Array append: before had ["LOGIN"], now also has DAEMON */
	ATF_CHECK_EQ(u->u_before.len, 2);
	ATF_CHECK_STREQ(u->u_before.d[0], "LOGIN");
	ATF_CHECK_STREQ(u->u_before.d[1], "DAEMON");

	/* Array append: keywords had ["nojail"], now also "resume" */
	ATF_CHECK_EQ(u->u_keyword.len, 2);
	ATF_CHECK(u->u_nojail == true);
	ATF_CHECK(u->u_resume == true);

	/* Object merge: restart.delay changed, policy untouched */
	ATF_CHECK(u->u_restart.rc_policy == RESTART_ON_FAILURE);
	ATF_CHECK_EQ(u->u_restart.rc_delay_ms, 5000);
	ATF_CHECK_EQ(u->u_restart.rc_max_retries, 3);

	unit_free(u);
}

ATF_TC(override_replace);
ATF_TC_HEAD(override_replace, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Override: replace{} replaces arrays entirely");
}
ATF_TC_BODY(override_replace, tc)
{
	const char *srcdir = atf_tc_get_config_var(tc, "srcdir");
	char path[PATH_MAX], confdir[PATH_MAX];
	struct rcd_config cfg;
	struct unit *u;

	memset(&cfg, 0, sizeof(cfg));

	snprintf(path, sizeof(path), "%s/%s", srcdir, "replace_test.ucl");
	u = unit_parse(path, &cfg);
	ATF_REQUIRE_MSG(u != NULL, "unit_parse returned NULL");

	/* Pre-override: requires = [A, B, C], keywords = [nojail, nostart] */
	ATF_CHECK_EQ(u->u_require.len, 3);
	ATF_CHECK(u->u_nojail == true);
	ATF_CHECK(u->u_nostart == true);

	/* Apply override */
	snprintf(confdir, sizeof(confdir), "%s/%s", srcdir, "confdir");
	unit_apply_overrides(u, confdir);

	/* enable=true from override re-enables */
	ATF_CHECK(u->u_enabled == true);

	/* replace{}: requires completely replaced with [X, Y] */
	ATF_CHECK_EQ(u->u_require.len, 2);
	ATF_CHECK_STREQ(u->u_require.d[0], "X");
	ATF_CHECK_STREQ(u->u_require.d[1], "Y");

	/* replace{}: keywords completely replaced with [resume] */
	ATF_CHECK_EQ(u->u_keyword.len, 1);
	ATF_CHECK_STREQ(u->u_keyword.d[0], "resume");

	/* Keyword flags reprocessed: nojail cleared, resume set */
	ATF_CHECK(u->u_nojail == false);
	ATF_CHECK(u->u_resume == true);

	unit_free(u);
}

ATF_TC(override_no_file);
ATF_TC_HEAD(override_no_file, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Override: missing override file is not an error");
}
ATF_TC_BODY(override_no_file, tc)
{
	const char *srcdir = atf_tc_get_config_var(tc, "srcdir");
	char path[PATH_MAX];
	struct rcd_config cfg;
	struct unit *u;
	int ret;

	memset(&cfg, 0, sizeof(cfg));

	snprintf(path, sizeof(path), "%s/%s", srcdir, "sshd.ucl");
	u = unit_parse(path, &cfg);
	ATF_REQUIRE(u != NULL);

	/* Apply override from a non-existent directory */
	ret = unit_apply_overrides(u, "/nonexistent/confdir");
	ATF_CHECK_EQ(ret, 0);

	/* Unit unchanged */
	ATF_CHECK_STREQ(u->u_command, "/usr/sbin/sshd");
	ATF_CHECK_EQ(u->u_require.len, 2);

	unit_free(u);
}

ATF_TP_ADD_TCS(tp)
{

	ATF_TP_ADD_TC(tp, parse_simple_service);
	ATF_TP_ADD_TC(tp, parse_dependencies);
	ATF_TP_ADD_TC(tp, parse_restart_config);
	ATF_TP_ADD_TC(tp, parse_restart_always);
	ATF_TP_ADD_TC(tp, parse_process_config);
	ATF_TP_ADD_TC(tp, parse_process_cpuset);
	ATF_TP_ADD_TC(tp, parse_rctl_rules);
	ATF_TP_ADD_TC(tp, parse_jail_config);
	ATF_TP_ADD_TC(tp, parse_socket_activation);
	ATF_TP_ADD_TC(tp, parse_environment);
	ATF_TP_ADD_TC(tp, parse_logging);
	ATF_TP_ADD_TC(tp, parse_oneshot);
	ATF_TP_ADD_TC(tp, parse_nonexistent_file);
	ATF_TP_ADD_TC(tp, unit_alloc_defaults);
	ATF_TP_ADD_TC(tp, parse_command_prepend);
	ATF_TP_ADD_TC(tp, parse_setup_cmd);
	ATF_TP_ADD_TC(tp, parse_enable_false);
	ATF_TP_ADD_TC(tp, parse_required_vars);
	ATF_TP_ADD_TC(tp, parse_off_command);
	ATF_TP_ADD_TC(tp, override_append_and_remove);
	ATF_TP_ADD_TC(tp, override_replace);
	ATF_TP_ADD_TC(tp, override_no_file);

	return (atf_no_error());
}
