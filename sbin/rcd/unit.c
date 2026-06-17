/*
 * Copyright (c) 2026 Baptiste Daroussin <bapt@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Unit file parser.  Reads UCL-format service definitions and populates
 * the unit structure.
 */

#include <sys/param.h>

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ucl.h>

#include "rcd.h"

/*
 * Validate that a service name contains only safe characters.
 * Prevents path traversal in enable.c and injection in rctl rules.
 */
/*
 * Convert a signal name (with or without "SIG" prefix) to a signal number.
 * Returns -1 if the name is not recognized.
 */
static int
parse_signal(const ucl_object_t *val)
{
	const char *s;
	int i;

	if (ucl_object_type(val) == UCL_INT)
		return ((int)ucl_object_toint(val));

	s = ucl_object_tostring(val);
	if (s == NULL)
		return (-1);

	/* Skip optional "SIG" prefix */
	if (strncasecmp(s, "SIG", 3) == 0)
		s += 3;

	for (i = 1; i < NSIG; i++) {
		if (strcasecmp(s, sys_signame[i]) == 0)
			return (i);
	}
	return (-1);
}

bool
valid_service_name(const char *name)
{
	const char *p;

	if (name == NULL || name[0] == '\0')
		return (false);
	/* Must start with alphanumeric or underscore */
	if (!isalnum((unsigned char)name[0]) && name[0] != '_')
		return (false);
	for (p = name; *p != '\0'; p++) {
		if (!isalnum((unsigned char)*p) && *p != '_' &&
		    *p != '-' && *p != '.')
			return (false);
	}
	return (true);
}

struct unit *
unit_alloc(void)
{
	struct unit *u;

	u = xcalloc(1, sizeof(*u));

	u->u_procdesc_fd = -1;
	u->u_notify_fd = -1;
	u->u_pid = -1;
	u->u_state = STATE_INACTIVE;
	u->u_ready_method = READY_IMMEDIATE;
	u->u_sig_stop = SIGTERM;
	u->u_sig_reload = SIGHUP;
	u->u_log.lc_stdout_pipefd = -1;
	u->u_log.lc_stdout_wfd = -1;
	u->u_log.lc_stderr_pipefd = -1;
	u->u_log.lc_stderr_wfd = -1;
	u->u_proc.pc_umask = 022;
	u->u_restart.rc_policy = RESTART_NEVER;
	u->u_restart.rc_backoff = BACKOFF_NONE;
	u->u_restart.rc_delay_ms = 5000;
	u->u_restart.rc_max_retries = 5;
	u->u_restart.rc_reset_ms = 60000;
	u->u_enabled = false;

	STAILQ_INIT(&u->u_deps);
	STAILQ_INIT(&u->u_rdeps);
	STAILQ_INIT(&u->u_rctl);
	STAILQ_INIT(&u->u_rctl_active);
	STAILQ_INIT(&u->u_env);
	STAILQ_INIT(&u->u_required_sysctl);
	STAILQ_INIT(&u->u_commands);
	TAILQ_INIT(&u->u_sockets);

	return (u);
}

void
unit_free(struct unit *u)
{
	struct rctl_conf *rc, *rc_tmp;
	struct kv *ue, *ue_tmp;
	struct unit_socket *us, *us_tmp;

	if (u == NULL)
		return;

	free(u->u_name);
	free(u->u_description);
	free(u->u_path);
	free(u->u_command);
	free(u->u_command_args);
	free(u->u_command_prepend);
	free(u->u_exec);
	free(u->u_stop_command);
	free(u->u_off_command);
	free(u->u_instance);
	free(u->u_instance_conf);
	free(u->u_override_conf);

	vec_free_and_free(&u->u_provide, free);
	vec_free_and_free(&u->u_require, free);
	vec_free_and_free(&u->u_before, free);
	vec_free_and_free(&u->u_keyword, free);

	/* Free dependency links (allocated by depgraph_resolve) */
	{
		struct dep_link *dl, *dl_tmp;

		STAILQ_FOREACH_SAFE(dl, &u->u_deps, dl_entries, dl_tmp)
			free(dl);
		STAILQ_INIT(&u->u_deps);
		STAILQ_FOREACH_SAFE(dl, &u->u_rdeps, dl_entries, dl_tmp)
			free(dl);
		STAILQ_INIT(&u->u_rdeps);
	}

	free(u->u_setup_cmd);
	free(u->u_start_precmd);
	free(u->u_start_postcmd);
	free(u->u_stop_precmd);
	free(u->u_stop_postcmd);

	vec_free_and_free(&u->u_required_dirs, free);
	vec_free_and_free(&u->u_required_files, free);
	vec_free_and_free(&u->u_required_modules, free);
	vec_free_and_free(&u->u_required_vars, free);
	{
		struct kv *cmd, *cmd_tmp;
		STAILQ_FOREACH_SAFE(cmd, &u->u_commands, kv_entries, cmd_tmp) {
			free(cmd->kv_key);
			free(cmd->kv_val);
			free(cmd);
		}
	}

	free(u->u_proc.pc_user);
	free(u->u_proc.pc_group);
	vec_free_and_free(&u->u_proc.pc_groups, free);
	free(u->u_proc.pc_chdir);
	free(u->u_proc.pc_chroot);
	free(u->u_proc.pc_cpuset);
	free(u->u_proc.pc_login_class);
	free(u->u_proc.pc_limits);
	free(u->u_proc.pc_env_file);

	free(u->u_log.lc_stdout);
	free(u->u_log.lc_stderr);
	if (u->u_log.lc_stdout_pipefd >= 0)
		close(u->u_log.lc_stdout_pipefd);
	if (u->u_log.lc_stderr_pipefd >= 0)
		close(u->u_log.lc_stderr_pipefd);

	free(u->u_jail.jc_name);
	free(u->u_jail.jc_path);
	vec_free_and_free(&u->u_jail.jc_options, free);
	vec_free_and_free(&u->u_jail.jc_ip4addr, free);
	vec_free_and_free(&u->u_jail.jc_ip6addr, free);

	vec_free_and_free(&u->u_access.ua_start, free);
	vec_free_and_free(&u->u_access.ua_stop, free);
	vec_free_and_free(&u->u_access.ua_restart, free);
	vec_free_and_free(&u->u_access.ua_reload, free);
	vec_free_and_free(&u->u_access.ua_status, free);

	STAILQ_FOREACH_SAFE(rc, &u->u_rctl, rc_entries, rc_tmp) {
		free(rc->rc_resource);
		free(rc->rc_action);
		free(rc->rc_amount);
		free(rc);
	}

	STAILQ_FOREACH_SAFE(ue, &u->u_env, kv_entries, ue_tmp) {
		free(ue->kv_key);
		free(ue->kv_val);
		free(ue);
	}

	STAILQ_FOREACH_SAFE(ue, &u->u_required_sysctl, kv_entries, ue_tmp) {
		free(ue->kv_key);
		free(ue->kv_val);
		free(ue);
	}

	TAILQ_FOREACH_SAFE(us, &u->u_sockets, us_entries, us_tmp) {
		free(us->us_name);
		free(us->us_address);
		free(us->us_owner);
		free(us->us_group);
		free(us);
	}

	if (u->u_procdesc_fd >= 0)
		close(u->u_procdesc_fd);

	free(u);
}

/*
 * Parse a UCL value as a file mode.  Supports:
 *   - UCL_INT:    0660 (UCL parses octal natively)
 *   - UCL_STRING: "0660" (octal string), "rw-rw----" (ls-style),
 *                 "u=rw,g=rw" (chmod-style symbolic)
 * Returns 0 on success, -1 on invalid input.
 */
int
ucl_parse_mode(const ucl_object_t *obj, mode_t *modep)
{

	if (ucl_object_type(obj) == UCL_INT) {
		int64_t v = ucl_object_toint(obj);

		if (v < 0 || v > 0777)
			return (-1);
		*modep = (mode_t)v;
		return (0);
	}

	if (ucl_object_type(obj) == UCL_STRING) {
		void *set;

		set = setmode(ucl_object_tostring(obj));
		if (set == NULL)
			return (-1);
		*modep = getmode(set, 0);
		free(set);
		return (0);
	}

	return (-1);
}

/*
 * Parse a string list from a UCL array into a charv_t.
 */
static void
parse_string_list(const ucl_object_t *obj, charv_t *out)
{
	const ucl_object_t *elem;
	ucl_object_iter_t it;

	if (ucl_array_size(obj) <= 0)
		return;

	it = ucl_object_iterate_new(obj);
	while ((elem = ucl_object_iterate_safe(it, true)) != NULL)
		vec_push(out, xstrdup(ucl_object_tostring(elem)));
	ucl_object_iterate_free(it);
}

/*
 * Parse a "socket" block from the unit file.
 */
static int
parse_socket(const ucl_object_t *obj, const char *name, struct unit *u)
{
	struct unit_socket *us;
	const ucl_object_t *val;

	us = xcalloc(1, sizeof(*us));
	us->us_name = xstrdup(name);
	us->us_fd = -1;
	us->us_backlog = 128;
	us->us_permissions = 0666;

	val = ucl_object_lookup(obj, "listen");
	if (val != NULL)
		us->us_address = xstrdup(ucl_object_tostring(val));

	val = ucl_object_lookup(obj, "type");
	if (val != NULL) {
		const char *s = ucl_object_tostring(val);
		if (strcmp(s, "dgram") == 0)
			us->us_type = SOCK_ACT_DGRAM;
		else if (strcmp(s, "seqpacket") == 0)
			us->us_type = SOCK_ACT_SEQPACKET;
		else
			us->us_type = SOCK_ACT_STREAM;
	}

	val = ucl_object_lookup(obj, "backlog");
	if (val != NULL)
		us->us_backlog = ucl_object_toint(val);

	TAILQ_INSERT_TAIL(&u->u_sockets, us, us_entries);
	return (0);
}

/*
 * Parse a "restart" block.
 */
static void
parse_restart(const ucl_object_t *obj, struct restart_conf *rc)
{
	const ucl_object_t *val;

	val = ucl_object_lookup(obj, "policy");
	if (val != NULL) {
		const char *s = ucl_object_tostring(val);
		if (strcmp(s, "always") == 0)
			rc->rc_policy = RESTART_ALWAYS;
		else if (strcmp(s, "on-failure") == 0)
			rc->rc_policy = RESTART_ON_FAILURE;
		else
			rc->rc_policy = RESTART_NEVER;
	}

	val = ucl_object_lookup(obj, "max_retries");
	if (val != NULL)
		rc->rc_max_retries = ucl_object_toint(val);

	val = ucl_object_lookup(obj, "delay");
	if (val != NULL)
		rc->rc_delay_ms = ucl_object_toint(val);

	val = ucl_object_lookup(obj, "reset");
	if (val != NULL)
		rc->rc_reset_ms = ucl_object_toint(val);

	val = ucl_object_lookup(obj, "backoff");
	if (val != NULL) {
		const char *s = ucl_object_tostring(val);
		if (strcmp(s, "exponential") == 0)
			rc->rc_backoff = BACKOFF_EXPONENTIAL;
		else if (strcmp(s, "linear") == 0)
			rc->rc_backoff = BACKOFF_LINEAR;
		else
			rc->rc_backoff = BACKOFF_NONE;
	}
}

/*
 * Parse a "process" block.
 */
static void
parse_process(const ucl_object_t *obj, struct proc_conf *pc)
{
	const ucl_object_t *val;

	val = ucl_object_lookup(obj, "user");
	if (val != NULL)
		pc->pc_user = xstrdup(ucl_object_tostring(val));

	val = ucl_object_lookup(obj, "group");
	if (val != NULL)
		pc->pc_group = xstrdup(ucl_object_tostring(val));

	val = ucl_object_lookup(obj, "chdir");
	if (val != NULL)
		pc->pc_chdir = xstrdup(ucl_object_tostring(val));

	val = ucl_object_lookup(obj, "chroot");
	if (val != NULL)
		pc->pc_chroot = xstrdup(ucl_object_tostring(val));

	val = ucl_object_lookup(obj, "umask");
	if (val != NULL)
		pc->pc_umask = (mode_t)ucl_object_toint(val);

	val = ucl_object_lookup(obj, "nice");
	if (val != NULL)
		pc->pc_nice = ucl_object_toint(val);

	val = ucl_object_lookup(obj, "cpuset");
	if (val != NULL)
		pc->pc_cpuset = xstrdup(ucl_object_tostring(val));

	val = ucl_object_lookup(obj, "oom_protect");
	if (val != NULL)
		pc->pc_oom_protect = ucl_object_toboolean(val);

	val = ucl_object_lookup(obj, "fib");
	if (val != NULL)
		pc->pc_fib = ucl_object_toint(val);

	val = ucl_object_lookup(obj, "login_class");
	if (val != NULL)
		pc->pc_login_class = xstrdup(ucl_object_tostring(val));

	val = ucl_object_lookup(obj, "limits");
	if (val != NULL)
		pc->pc_limits = xstrdup(ucl_object_tostring(val));

	val = ucl_object_lookup(obj, "env_file");
	if (val != NULL)
		pc->pc_env_file = xstrdup(ucl_object_tostring(val));

	/* Supplementary groups */
	val = ucl_object_lookup(obj, "groups");
	if (val != NULL && ucl_object_type(val) == UCL_ARRAY) {
		const ucl_object_t *elem;
		ucl_object_iter_t git;

		git = ucl_object_iterate_new(val);
		while ((elem = ucl_object_iterate_safe(git,
		    true)) != NULL)
			vec_push(&pc->pc_groups,
			    xstrdup(ucl_object_tostring(elem)));
		ucl_object_iterate_free(git);
	}
}

/*
 * Validate that an rctl field contains only safe characters:
 * alphanumeric, underscore, dot, hyphen, and (for amounts) digits.
 * Rejects colons, equals signs, spaces, shell metacharacters.
 */
static bool
valid_rctl_field(const char *s)
{
	const char *p;

	if (s == NULL || *s == '\0')
		return (false);
	for (p = s; *p != '\0'; p++) {
		if (!isalnum((unsigned char)*p) && *p != '_' &&
		    *p != '.' && *p != '-' && *p != '/')
			return (false);
	}
	return (true);
}

/*
 * Parse an "rctl" block.
 */
static void
parse_rctl(const ucl_object_t *obj, struct rctl_conf_list *list)
{
	const ucl_object_t *cur;
	ucl_object_iter_t it;

	it = ucl_object_iterate_new(obj);
	while ((cur = ucl_object_iterate_safe(it, true)) != NULL) {
		struct rctl_conf *rc;
		const ucl_object_t *act, *amt;
		const char *key;

		key = ucl_object_key(cur);
		if (!valid_rctl_field(key)) {
			log_warn("rctl: invalid resource name '%s', "
			    "skipping", key);
			continue;
		}

		rc = xcalloc(1, sizeof(*rc));
		if (rc == NULL)
			continue;

		rc->rc_resource = xstrdup(key);

		act = ucl_object_lookup(cur, "action");
		if (act != NULL) {
			const char *s = ucl_object_tostring(act);
			if (valid_rctl_field(s))
				rc->rc_action = xstrdup(s);
			else
				log_warn("rctl: invalid action '%s' "
				    "for resource '%s'", s, key);
		}

		amt = ucl_object_lookup(cur, "amount");
		if (amt != NULL) {
			const char *s = ucl_object_tostring(amt);
			/* Amounts can contain digits, unit suffixes */
			rc->rc_amount = xstrdup(s);
		}

		STAILQ_INSERT_TAIL(list, rc, rc_entries);
	}
	ucl_object_iterate_free(it);
}

/*
 * Parse a "jail" block.
 */
static void
parse_jail(const ucl_object_t *obj, struct jail_conf *jc)
{
	const ucl_object_t *val;

	val = ucl_object_lookup(obj, "enable");
	if (val != NULL)
		jc->jc_enable = ucl_object_toboolean(val);

	val = ucl_object_lookup(obj, "name");
	if (val != NULL)
		jc->jc_name = xstrdup(ucl_object_tostring(val));

	val = ucl_object_lookup(obj, "path");
	if (val != NULL)
		jc->jc_path = xstrdup(ucl_object_tostring(val));

	val = ucl_object_lookup(obj, "devfs");
	if (val != NULL)
		jc->jc_devfs = ucl_object_toboolean(val);

	/* Parse options array */
	val = ucl_object_lookup(obj, "options");
	if (val != NULL && ucl_object_type(val) == UCL_ARRAY) {
		const ucl_object_t *elem;
		ucl_object_iter_t oit;

		oit = ucl_object_iterate_new(val);
		while ((elem = ucl_object_iterate_safe(oit,
		    true)) != NULL)
			vec_push(&jc->jc_options,
			    xstrdup(ucl_object_tostring(elem)));
		ucl_object_iterate_free(oit);
	}

	/* Parse ip4addr array */
	val = ucl_object_lookup(obj, "ip4addr");
	if (val != NULL && ucl_object_type(val) == UCL_ARRAY) {
		const ucl_object_t *elem;
		ucl_object_iter_t oit;

		oit = ucl_object_iterate_new(val);
		while ((elem = ucl_object_iterate_safe(oit,
		    true)) != NULL)
			vec_push(&jc->jc_ip4addr,
			    xstrdup(ucl_object_tostring(elem)));
		ucl_object_iterate_free(oit);
	}

	/* Parse ip6addr array */
	val = ucl_object_lookup(obj, "ip6addr");
	if (val != NULL && ucl_object_type(val) == UCL_ARRAY) {
		const ucl_object_t *elem;
		ucl_object_iter_t oit;

		oit = ucl_object_iterate_new(val);
		while ((elem = ucl_object_iterate_safe(oit,
		    true)) != NULL)
			vec_push(&jc->jc_ip6addr,
			    xstrdup(ucl_object_tostring(elem)));
		ucl_object_iterate_free(oit);
	}
}

/*
 * Parse an "environment" block.
 */
static void
parse_environment(const ucl_object_t *obj, struct kv_list *list)
{
	const ucl_object_t *cur;
	ucl_object_iter_t it;

	it = ucl_object_iterate_new(obj);
	while ((cur = ucl_object_iterate_safe(it, true)) != NULL) {
		struct kv *ue;

		ue = xcalloc(1, sizeof(*ue));
		if (ue == NULL)
			continue;

		ue->kv_key = xstrdup(ucl_object_key(cur));
		ue->kv_val = xstrdup(ucl_object_tostring(cur));
		STAILQ_INSERT_TAIL(list, ue, kv_entries);
	}
	ucl_object_iterate_free(it);
}

/*
 * Parse a "logging" block.
 */
static void
parse_logging(const ucl_object_t *obj, struct log_conf *lc)
{
	const ucl_object_t *val;

	val = ucl_object_lookup(obj, "stdout");
	if (val != NULL)
		lc->lc_stdout = xstrdup(ucl_object_tostring(val));

	val = ucl_object_lookup(obj, "stderr");
	if (val != NULL)
		lc->lc_stderr = xstrdup(ucl_object_tostring(val));
}

/*
 * Create an instance from a template unit.
 * The instance gets its own name ("template@instance"), its own state,
 * and shares the template's command/exec/hooks by copying them.
 * The instance variable is available to Lua hooks via rcd.instance
 * and to shell hooks via $RCD_INSTANCE.
 */
struct unit *
unit_instantiate(struct unit *tmpl, const char *instance)
{
	struct unit *u;

	if (!tmpl->u_template)
		return (NULL);

	u = unit_alloc();

	/* Identity — name is "template@instance" */
	xasprintf(&u->u_name, "%s@%s", tmpl->u_name, instance);
	u->u_instance = xstrdup(instance);
	u->u_template_ref = tmpl;

	if (tmpl->u_description != NULL)
		xasprintf(&u->u_description, "%s (%s)",
		    tmpl->u_description, instance);
	if (tmpl->u_path != NULL)
		u->u_path = xstrdup(tmpl->u_path);

	/* Copy type and config from template */
	u->u_type = tmpl->u_type;
	u->u_sig_stop = tmpl->u_sig_stop;
	u->u_sig_reload = tmpl->u_sig_reload;
	u->u_start_delay_ms = tmpl->u_start_delay_ms;
	u->u_ready_method = tmpl->u_ready_method;
	u->u_restart = tmpl->u_restart;
	u->u_proc = (struct proc_conf){ 0 };  /* reset, copy fields */
	if (tmpl->u_proc.pc_user != NULL)
		u->u_proc.pc_user = xstrdup(tmpl->u_proc.pc_user);
	if (tmpl->u_proc.pc_group != NULL)
		u->u_proc.pc_group = xstrdup(tmpl->u_proc.pc_group);
	u->u_proc.pc_umask = tmpl->u_proc.pc_umask;
	u->u_proc.pc_nice = tmpl->u_proc.pc_nice;
	u->u_proc.pc_oom_protect = tmpl->u_proc.pc_oom_protect;
	if (tmpl->u_proc.pc_cpuset != NULL)
		u->u_proc.pc_cpuset = xstrdup(tmpl->u_proc.pc_cpuset);
	u->u_proc.pc_fib = tmpl->u_proc.pc_fib;

	/* Copy command/exec/hooks — these reference the instance */
	if (tmpl->u_command != NULL)
		u->u_command = xstrdup(tmpl->u_command);
	if (tmpl->u_command_args != NULL)
		u->u_command_args = xstrdup(tmpl->u_command_args);
	if (tmpl->u_exec != NULL)
		u->u_exec = xstrdup(tmpl->u_exec);
	if (tmpl->u_stop_command != NULL)
		u->u_stop_command = xstrdup(tmpl->u_stop_command);
	if (tmpl->u_start_precmd != NULL)
		u->u_start_precmd = xstrdup(tmpl->u_start_precmd);
	if (tmpl->u_start_postcmd != NULL)
		u->u_start_postcmd = xstrdup(tmpl->u_start_postcmd);
	if (tmpl->u_stop_precmd != NULL)
		u->u_stop_precmd = xstrdup(tmpl->u_stop_precmd);
	if (tmpl->u_stop_postcmd != NULL)
		u->u_stop_postcmd = xstrdup(tmpl->u_stop_postcmd);

	/* Flags */
	u->u_enabled = true;
	u->u_nojail = tmpl->u_nojail;
	u->u_nojailvnet = tmpl->u_nojailvnet;
	u->u_resume = tmpl->u_resume;

	/* Jail config */
	u->u_jail.jc_enable = tmpl->u_jail.jc_enable;

	log_info("instantiated %s from template %s",
	    u->u_name, tmpl->u_name);

	return (u);
}

/*
 * Parse a unit file and return a populated unit structure.
 */
struct unit *
unit_parse(const char *path, struct rcd_config *cfg __unused)
{
	struct ucl_parser *parser;
	ucl_object_t *top;
	const ucl_object_t *val, *sub;
	ucl_object_iter_t it;
	struct unit *u;

	parser = ucl_parser_new(UCL_PARSER_DEFAULT);
	if (parser == NULL)
		return (NULL);

	if (!ucl_parser_add_file(parser, path)) {
		log_warn("ucl parse error: %s: %s", path,
		    ucl_parser_get_error(parser));
		ucl_parser_free(parser);
		return (NULL);
	}

	top = ucl_parser_get_object(parser);
	ucl_parser_free(parser);

	if (top == NULL)
		return (NULL);

	/* Validate against schema if available */
	if (cfg->cfg_unit_schema != NULL) {
		struct ucl_schema_error serr;

		if (!ucl_object_validate_root(cfg->cfg_unit_schema, top,
		    cfg->cfg_unit_schema, &serr)) {
			log_warn("%s: schema validation failed: %s",
			    path, serr.msg);
			ucl_object_unref(top);
			return (NULL);
		}
	}

	u = unit_alloc();
	u->u_path = xstrdup(path);

	/* Service name — required */
	val = ucl_object_lookup(top, "name");
	if (val == NULL) {
		log_warn("%s: missing 'name' field", path);
		ucl_object_unref(top);
		unit_free(u);
		return (NULL);
	}
	u->u_name = xstrdup(ucl_object_tostring(val));

	if (!valid_service_name(u->u_name)) {
		log_warn("invalid service name: %s in %s",
		    u->u_name, path);
		ucl_object_unref(top);
		unit_free(u);
		return (NULL);
	}

	/* All fields are at top level */
	val = ucl_object_lookup(top, "description");
	if (val != NULL)
		u->u_description = xstrdup(ucl_object_tostring(val));

	val = ucl_object_lookup(top, "type");
	if (val != NULL) {
		const char *s = ucl_object_tostring(val);
		if (strcmp(s, "forking") == 0)
			u->u_type = UNIT_FORKING;
		else if (strcmp(s, "oneshot") == 0)
			u->u_type = UNIT_ONESHOT;
		else if (strcmp(s, "barrier") == 0)
			u->u_type = UNIT_BARRIER;
		else
			u->u_type = UNIT_SIMPLE;
	}

	val = ucl_object_lookup(top, "enable");
	if (val != NULL)
		u->u_enabled = ucl_object_toboolean(val);

	val = ucl_object_lookup(top, "template");
	if (val != NULL)
		u->u_template = ucl_object_toboolean(val);

	val = ucl_object_lookup(top, "command");
	if (val != NULL)
		u->u_command = xstrdup(ucl_object_tostring(val));

	/*
	 * Barriers need nothing.  Oneshots can use either command or exec.
	 * Daemons (simple/forking) always need command.
	 */
	if (u->u_command == NULL && u->u_exec == NULL &&
	    u->u_type != UNIT_BARRIER) {
		log_warn("%s: missing 'command' or 'exec' field", path);
		ucl_object_unref(top);
		unit_free(u);
		return (NULL);
	}

	val = ucl_object_lookup(top, "command_args");
	if (val != NULL)
		u->u_command_args = xstrdup(ucl_object_tostring(val));

	val = ucl_object_lookup(top, "exec");
	if (val != NULL)
		u->u_exec = xstrdup(ucl_object_tostring(val));

	val = ucl_object_lookup(top, "command_prepend");
	if (val != NULL)
		u->u_command_prepend = xstrdup(ucl_object_tostring(val));

	val = ucl_object_lookup(top, "stop_command");
	if (val != NULL)
		u->u_stop_command = xstrdup(ucl_object_tostring(val));

	val = ucl_object_lookup(top, "off_command");
	if (val != NULL)
		u->u_off_command = xstrdup(ucl_object_tostring(val));

	/* Signals */
	val = ucl_object_lookup(top, "sig_stop");
	if (val != NULL) {
		int sig = parse_signal(val);
		if (sig > 0)
			u->u_sig_stop = sig;
	}

	val = ucl_object_lookup(top, "sig_reload");
	if (val != NULL) {
		int sig = parse_signal(val);
		if (sig > 0)
			u->u_sig_reload = sig;
	}

	val = ucl_object_lookup(top, "start_delay");
	if (val != NULL)
		u->u_start_delay_ms = ucl_object_toint(val);

	/* Preconditions */
	val = ucl_object_lookup(top, "required_dirs");
	if (val != NULL)
		parse_string_list(val, &u->u_required_dirs);

	val = ucl_object_lookup(top, "required_files");
	if (val != NULL)
		parse_string_list(val, &u->u_required_files);

	val = ucl_object_lookup(top, "required_modules");
	if (val != NULL)
		parse_string_list(val, &u->u_required_modules);

	val = ucl_object_lookup(top, "required_vars");
	if (val != NULL)
		parse_string_list(val, &u->u_required_vars);

	/* Required sysctl values — key=value pairs */
	sub = ucl_object_lookup(top, "required_sysctl");
	if (sub != NULL) {
		const ucl_object_t *cur;
		ucl_object_iter_t sit;

		sit = ucl_object_iterate_new(sub);
		while ((cur = ucl_object_iterate_safe(sit, true)) != NULL) {
			struct kv *sc;

			sc = xcalloc(1, sizeof(*sc));
			sc->kv_key = xstrdup(ucl_object_key(cur));
			sc->kv_val = xstrdup(ucl_object_tostring(cur));
			STAILQ_INSERT_TAIL(&u->u_required_sysctl, sc,
			    kv_entries);
		}
		ucl_object_iterate_free(sit);
	}

	/* Hooks */
	val = ucl_object_lookup(top, "setup_cmd");
	if (val != NULL)
		u->u_setup_cmd = xstrdup(ucl_object_tostring(val));

	val = ucl_object_lookup(top, "start_precmd");
	if (val != NULL)
		u->u_start_precmd = xstrdup(ucl_object_tostring(val));

	val = ucl_object_lookup(top, "start_postcmd");
	if (val != NULL)
		u->u_start_postcmd = xstrdup(ucl_object_tostring(val));

	val = ucl_object_lookup(top, "stop_precmd");
	if (val != NULL)
		u->u_stop_precmd = xstrdup(ucl_object_tostring(val));

	val = ucl_object_lookup(top, "stop_postcmd");
	if (val != NULL)
		u->u_stop_postcmd = xstrdup(ucl_object_tostring(val));

	/* Extra commands */
	/* Custom commands: name → exec code */
	sub = ucl_object_lookup(top, "commands");
	if (sub != NULL) {
		const ucl_object_t *cur;
		ucl_object_iter_t cit;

		cit = ucl_object_iterate_new(sub);
		while ((cur = ucl_object_iterate_safe(cit, true)) != NULL) {
			struct kv *cmd;

			cmd = xcalloc(1, sizeof(*cmd));
			cmd->kv_key = xstrdup(ucl_object_key(cur));
			cmd->kv_val = xstrdup(ucl_object_tostring(cur));
			STAILQ_INSERT_TAIL(&u->u_commands, cmd, kv_entries);
		}
		ucl_object_iterate_free(cit);
	}

	/* Dependency lists */
	val = ucl_object_lookup(top, "provides");
	if (val != NULL)
		parse_string_list(val, &u->u_provide);

	val = ucl_object_lookup(top, "requires");
	if (val != NULL)
		parse_string_list(val, &u->u_require);

	val = ucl_object_lookup(top, "before");
	if (val != NULL)
		parse_string_list(val, &u->u_before);

	val = ucl_object_lookup(top, "keywords");
	if (val != NULL) {
		parse_string_list(val, &u->u_keyword);
		/* Process keyword flags */
		vec_foreach(u->u_keyword, ki) {
			if (strcmp(u->u_keyword.d[ki],
			    "nojail") == 0)
				u->u_nojail = true;
			else if (strcmp(u->u_keyword.d[ki],
			    "nojailvnet") == 0)
				u->u_nojailvnet = true;
			else if (strcmp(u->u_keyword.d[ki],
			    "firstboot") == 0)
				u->u_boot_only = true;
			else if (strcmp(u->u_keyword.d[ki],
			    "nostart") == 0)
				u->u_nostart = true;
			else if (strcmp(u->u_keyword.d[ki],
			    "resume") == 0)
				u->u_resume = true;
		}
	}

	/* Readiness method */
	val = ucl_object_lookup(top, "ready");
	if (val != NULL) {
		sub = ucl_object_lookup(val, "method");
		if (sub != NULL) {
			const char *s = ucl_object_tostring(sub);
			if (strcmp(s, "fd") == 0)
				u->u_ready_method = READY_FD;
			else if (strcmp(s, "exit") == 0)
				u->u_ready_method = READY_EXIT;
			else if (strcmp(s, "socket") == 0)
				u->u_ready_method = READY_SOCKET;
		}
	}

	/* Socket blocks */
	sub = ucl_object_lookup(top, "socket");
	if (sub != NULL) {
		it = ucl_object_iterate_new(sub);
		while ((val = ucl_object_iterate_safe(it, true)) != NULL) {
			if (ucl_object_type(val) == UCL_OBJECT)
				parse_socket(val, ucl_object_key(val), u);
		}
		ucl_object_iterate_free(it);
	}

	/* Subsystem blocks */
	sub = ucl_object_lookup(top, "process");
	if (sub != NULL)
		parse_process(sub, &u->u_proc);

	sub = ucl_object_lookup(top, "restart");
	if (sub != NULL)
		parse_restart(sub, &u->u_restart);

	sub = ucl_object_lookup(top, "rctl");
	if (sub != NULL)
		parse_rctl(sub, &u->u_rctl);

	sub = ucl_object_lookup(top, "jail");
	if (sub != NULL)
		parse_jail(sub, &u->u_jail);

	sub = ucl_object_lookup(top, "environment");
	if (sub != NULL)
		parse_environment(sub, &u->u_env);

	sub = ucl_object_lookup(top, "logging");
	if (sub != NULL)
		parse_logging(sub, &u->u_log);

	/* Access control */
	sub = ucl_object_lookup(top, "access");
	if (sub != NULL) {
		val = ucl_object_lookup(sub, "start");
		if (val != NULL)
			parse_string_list(val, &u->u_access.ua_start);
		val = ucl_object_lookup(sub, "stop");
		if (val != NULL)
			parse_string_list(val, &u->u_access.ua_stop);
		val = ucl_object_lookup(sub, "restart");
		if (val != NULL)
			parse_string_list(val, &u->u_access.ua_restart);
		val = ucl_object_lookup(sub, "reload");
		if (val != NULL)
			parse_string_list(val, &u->u_access.ua_reload);
		val = ucl_object_lookup(sub, "status");
		if (val != NULL)
			parse_string_list(val, &u->u_access.ua_status);
	}

	ucl_object_unref(top);
	return (u);
}

/*
 * Override helpers for unit_apply_overrides().
 */

/* Replace a char * field if the key exists in the UCL object. */
static void
override_string(const ucl_object_t *top, const char *key, char **field)
{
	const ucl_object_t *val;

	val = ucl_object_lookup(top, key);
	if (val != NULL) {
		free(*field);
		*field = xstrdup(ucl_object_tostring(val));
	}
}

/* Append UCL array elements to a charv_t, skipping duplicates. */
static void
override_append_array(const ucl_object_t *top, const char *key, charv_t *vec)
{
	const ucl_object_t *val, *elem;
	ucl_object_iter_t it;
	size_t i;
	bool dup;

	val = ucl_object_lookup(top, key);
	if (val == NULL || ucl_object_type(val) != UCL_ARRAY)
		return;

	it = ucl_object_iterate_new(val);
	while ((elem = ucl_object_iterate_safe(it, true)) != NULL) {
		const char *s = ucl_object_tostring(elem);

		dup = false;
		for (i = 0; i < vec->len; i++) {
			if (strcmp(vec->d[i], s) == 0) {
				dup = true;
				break;
			}
		}
		if (!dup)
			vec_push(vec, xstrdup(s));
	}
	ucl_object_iterate_free(it);
}

/* Remove named entries from a charv_t. */
static void
override_remove_array(const ucl_object_t *block, const char *key, charv_t *vec)
{
	const ucl_object_t *val, *elem;
	ucl_object_iter_t it;

	val = ucl_object_lookup(block, key);
	if (val == NULL || ucl_object_type(val) != UCL_ARRAY)
		return;

	it = ucl_object_iterate_new(val);
	while ((elem = ucl_object_iterate_safe(it, true)) != NULL) {
		const char *s = ucl_object_tostring(elem);
		size_t i;

		for (i = 0; i < vec->len; i++) {
			if (strcmp(vec->d[i], s) == 0) {
				vec_remove_and_free(vec, i, free);
				break;
			}
		}
	}
	ucl_object_iterate_free(it);
}

/* Replace a charv_t entirely from a UCL array. */
static void
override_replace_array(const ucl_object_t *block, const char *key, charv_t *vec)
{
	const ucl_object_t *val;

	val = ucl_object_lookup(block, key);
	if (val == NULL || ucl_object_type(val) != UCL_ARRAY)
		return;

	vec_free_and_free(vec, free);
	parse_string_list(val, vec);
}

/*
 * Reprocess keyword flags from the keyword array.
 * Must be called after any modification to u_keyword.
 */
static void
reprocess_keyword_flags(struct unit *u)
{

	u->u_nojail = false;
	u->u_nojailvnet = false;
	u->u_nostart = false;
	u->u_boot_only = false;
	u->u_resume = false;

	vec_foreach(u->u_keyword, ki) {
		if (strcmp(u->u_keyword.d[ki], "nojail") == 0)
			u->u_nojail = true;
		else if (strcmp(u->u_keyword.d[ki], "nojailvnet") == 0)
			u->u_nojailvnet = true;
		else if (strcmp(u->u_keyword.d[ki], "firstboot") == 0)
			u->u_boot_only = true;
		else if (strcmp(u->u_keyword.d[ki], "nostart") == 0)
			u->u_nostart = true;
		else if (strcmp(u->u_keyword.d[ki], "resume") == 0)
			u->u_resume = true;
	}
}

/*
 * Apply per-service overrides from /etc/rcd.conf.d/<name>.
 *
 * Override files are UCL fragments merged on top of the unit's
 * configuration.  Merge semantics:
 *   - Scalar fields: replaced.
 *   - Array fields (provides, requires, before, keywords, required_*):
 *     appended (duplicates skipped).
 *   - Object fields (restart, process, environment, jail, logging, access):
 *     merged key-by-key.
 *   - remove {} block: entries listed are removed from arrays.
 *   - replace {} block: arrays listed are replaced entirely.
 *
 * Application order: remove first, then merge/append, then replace.
 */
int
unit_apply_overrides(struct unit *u, const char *confdir)
{
	struct ucl_parser *parser;
	ucl_object_t *top;
	const ucl_object_t *val, *sub;
	char path[PATH_MAX];
	bool keywords_changed;

	if (snprintf(path, sizeof(path), "%s/%s",
	    confdir, u->u_name) >= (int)sizeof(path))
		return (0);

	parser = ucl_parser_new(UCL_PARSER_DEFAULT);
	if (parser == NULL)
		return (0);

	if (!ucl_parser_add_file(parser, path)) {
		ucl_parser_free(parser);
		return (0);	/* File doesn't exist — not an error */
	}

	top = ucl_parser_get_object(parser);
	ucl_parser_free(parser);
	if (top == NULL)
		return (0);

	/* Store the raw UCL for rcd.config in Lua hooks */
	{
		unsigned char *raw;

		raw = ucl_object_emit(top, UCL_EMIT_CONFIG);
		if (raw != NULL) {
			free(u->u_override_conf);
			u->u_override_conf = (char *)raw;
		}
	}

	keywords_changed = false;

	sub = ucl_object_lookup(top, "remove");
	if (sub != NULL) {
		override_remove_array(sub, "provides", &u->u_provide);
		override_remove_array(sub, "requires", &u->u_require);
		override_remove_array(sub, "before", &u->u_before);
		if (ucl_object_lookup(sub, "keywords") != NULL) {
			override_remove_array(sub, "keywords", &u->u_keyword);
			keywords_changed = true;
		}
		override_remove_array(sub, "required_dirs",
		    &u->u_required_dirs);
		override_remove_array(sub, "required_files",
		    &u->u_required_files);
		override_remove_array(sub, "required_modules",
		    &u->u_required_modules);
		override_remove_array(sub, "required_vars",
		    &u->u_required_vars);
	}

	/* Replace scalars */
	val = ucl_object_lookup(top, "enable");
	if (val != NULL)
		u->u_enabled = ucl_object_toboolean(val);

	override_string(top, "command", &u->u_command);
	override_string(top, "command_args", &u->u_command_args);
	override_string(top, "command_prepend", &u->u_command_prepend);
	override_string(top, "exec", &u->u_exec);
	override_string(top, "stop_command", &u->u_stop_command);
	override_string(top, "off_command", &u->u_off_command);
	override_string(top, "description", &u->u_description);

	/* Hooks */
	override_string(top, "setup_cmd", &u->u_setup_cmd);
	override_string(top, "start_precmd", &u->u_start_precmd);
	override_string(top, "start_postcmd", &u->u_start_postcmd);
	override_string(top, "stop_precmd", &u->u_stop_precmd);
	override_string(top, "stop_postcmd", &u->u_stop_postcmd);

	/* Signals */
	val = ucl_object_lookup(top, "sig_stop");
	if (val != NULL) {
		int sig = parse_signal(val);
		if (sig > 0)
			u->u_sig_stop = sig;
	}
	val = ucl_object_lookup(top, "sig_reload");
	if (val != NULL) {
		int sig = parse_signal(val);
		if (sig > 0)
			u->u_sig_reload = sig;
	}

	/* Start delay */
	val = ucl_object_lookup(top, "start_delay");
	if (val != NULL)
		u->u_start_delay_ms = ucl_object_toint(val);

	/* arrays (append, deduplicate) */
	override_append_array(top, "provides", &u->u_provide);
	override_append_array(top, "requires", &u->u_require);
	override_append_array(top, "before", &u->u_before);
	if (ucl_object_lookup(top, "keywords") != NULL) {
		override_append_array(top, "keywords", &u->u_keyword);
		keywords_changed = true;
	}
	override_append_array(top, "required_dirs", &u->u_required_dirs);
	override_append_array(top, "required_files", &u->u_required_files);
	override_append_array(top, "required_modules",
	    &u->u_required_modules);
	override_append_array(top, "required_vars", &u->u_required_vars);

	/* objects (merge) */
	sub = ucl_object_lookup(top, "restart");
	if (sub != NULL)
		parse_restart(sub, &u->u_restart);

	sub = ucl_object_lookup(top, "process");
	if (sub != NULL)
		parse_process(sub, &u->u_proc);

	sub = ucl_object_lookup(top, "environment");
	if (sub != NULL)
		parse_environment(sub, &u->u_env);

	sub = ucl_object_lookup(top, "jail");
	if (sub != NULL)
		parse_jail(sub, &u->u_jail);

	sub = ucl_object_lookup(top, "logging");
	if (sub != NULL)
		parse_logging(sub, &u->u_log);

	/* Access control — arrays within access are replaced, not appended */
	sub = ucl_object_lookup(top, "access");
	if (sub != NULL) {
		val = ucl_object_lookup(sub, "start");
		if (val != NULL) {
			vec_free_and_free(&u->u_access.ua_start, free);
			parse_string_list(val, &u->u_access.ua_start);
		}
		val = ucl_object_lookup(sub, "stop");
		if (val != NULL) {
			vec_free_and_free(&u->u_access.ua_stop, free);
			parse_string_list(val, &u->u_access.ua_stop);
		}
		val = ucl_object_lookup(sub, "restart");
		if (val != NULL) {
			vec_free_and_free(&u->u_access.ua_restart, free);
			parse_string_list(val, &u->u_access.ua_restart);
		}
		val = ucl_object_lookup(sub, "reload");
		if (val != NULL) {
			vec_free_and_free(&u->u_access.ua_reload, free);
			parse_string_list(val, &u->u_access.ua_reload);
		}
		val = ucl_object_lookup(sub, "status");
		if (val != NULL) {
			vec_free_and_free(&u->u_access.ua_status, free);
			parse_string_list(val, &u->u_access.ua_status);
		}
	}

	sub = ucl_object_lookup(top, "replace");
	if (sub != NULL) {
		override_replace_array(sub, "provides", &u->u_provide);
		override_replace_array(sub, "requires", &u->u_require);
		override_replace_array(sub, "before", &u->u_before);
		if (ucl_object_lookup(sub, "keywords") != NULL) {
			override_replace_array(sub, "keywords",
			    &u->u_keyword);
			keywords_changed = true;
		}
		override_replace_array(sub, "required_dirs",
		    &u->u_required_dirs);
		override_replace_array(sub, "required_files",
		    &u->u_required_files);
		override_replace_array(sub, "required_modules",
		    &u->u_required_modules);
		override_replace_array(sub, "required_vars",
		    &u->u_required_vars);
	}

	/* Recompute keyword-derived flags if keywords were touched */
	if (keywords_changed)
		reprocess_keyword_flags(u);

	ucl_object_unref(top);
	return (0);
}
