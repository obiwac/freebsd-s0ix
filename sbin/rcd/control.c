/*
 * Copyright (c) 2026 Baptiste Daroussin <bapt@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Control socket.  Listens on a UNIX domain socket and handles commands
 * from rcctl(8).  Client credentials are verified via LOCAL_PEERCRED.
 * Messages are UCL-encoded with a 4-byte length prefix.
 */

#include <sys/param.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/ucred.h>
#include <sys/un.h>

#include <arpa/inet.h>

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <libgen.h>
#include <paths.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include <ucl.h>

#include "rcd.h"

#define CTL_MAX_MSG	(64 * 1024)

static const char *state_names[] = {
	[STATE_INACTIVE] = "inactive",
	[STATE_STARTING] = "starting",
	[STATE_RUNNING]  = "running",
	[STATE_STOPPING] = "stopping",
	[STATE_FAILED]   = "failed",
	[STATE_DONE]     = "done",
	[STATE_WAITING]  = "waiting",
};

static const char *
unit_type_str(enum unit_type t)
{

	switch (t) {
	case UNIT_SIMPLE:		return ("simple");
	case UNIT_FORKING:		return ("forking");
	case UNIT_ONESHOT:		return ("oneshot");
	case UNIT_BARRIER:		return ("barrier");
	case UNIT_LEGACY:		return ("legacy");
	case UNIT_LEGACY_FORKING:	return ("legacy-forking");
	}
	return ("unknown");
}

/*
 * Verify that the connecting peer is authorized (root or in control group).
 */
static bool
check_credentials(int fd, const char *groupname)
{
	struct xucred cred;
	socklen_t len;
	struct group *grp;
	int i;

	len = sizeof(cred);
	if (getsockopt(fd, SOL_LOCAL, LOCAL_PEERCRED, &cred, &len) != 0)
		return (false);

	/* Verify credential structure version */
	if (cred.cr_version != XUCRED_VERSION)
		return (false);

	/* Root is always allowed */
	if (cred.cr_uid == 0)
		return (true);

	/* Check group membership */
	if (groupname == NULL) {
		log_warn("control: no control_group configured, "
		    "non-root access denied");
		return (false);
	}
	grp = getgrnam(groupname);
	if (grp == NULL) {
		log_warn("control: group '%s' does not exist",
		    groupname);
		return (false);
	}

	for (i = 0; i < cred.cr_ngroups; i++) {
		if (cred.cr_groups[i] == grp->gr_gid)
			return (true);
	}

	return (false);
}

/*
 * Check if a credential matches a principal list.
 * Principals are: "username" for user match, "@groupname" for group match.
 * Returns true if any principal matches the credential.
 */
static bool
match_principals(const charv_t *principals, const struct xucred *cred)
{
	struct passwd *pw;
	struct group *gr;

	vec_foreach(*principals, i) {
		const char *p = principals->d[i];

		if (p[0] == '@') {
			/* Group match */
			gr = getgrnam(p + 1);
			if (gr == NULL)
				continue;
			for (int j = 0; j < cred->cr_ngroups; j++) {
				if (cred->cr_groups[j] == gr->gr_gid)
					return (true);
			}
		} else {
			/* User match */
			pw = getpwnam(p);
			if (pw != NULL && pw->pw_uid == cred->cr_uid)
				return (true);
		}
	}
	return (false);
}

/*
 * Check if a credential is authorized to perform a command on a unit.
 * Root is always authorized.  The global control_group bypasses ACLs.
 * Per-service ACLs are checked from the unit's access block.
 *
 * cmd is one of: "start", "stop", "restart", "reload", "status",
 * "describe", "show", "resources".
 * Returns true if authorized.
 */
bool
access_check(const struct unit *u, const char *cmd,
    const struct xucred *cred)
{
	const charv_t *acl;

	/* Root always passes */
	if (cred->cr_uid == 0)
		return (true);

	/* Pick the ACL for the requested command */
	if (strcmp(cmd, "start") == 0)
		acl = &u->u_access.ua_start;
	else if (strcmp(cmd, "stop") == 0)
		acl = &u->u_access.ua_stop;
	else if (strcmp(cmd, "restart") == 0)
		acl = &u->u_access.ua_restart;
	else if (strcmp(cmd, "reload") == 0)
		acl = &u->u_access.ua_reload;
	else if (strcmp(cmd, "status") == 0 || strcmp(cmd, "describe") == 0 ||
	    strcmp(cmd, "show") == 0 || strcmp(cmd, "resources") == 0) {
		/*
		 * Read-only commands: implicitly allow all control-group
		 * members when no per-service ACL is defined.
		 */
		acl = &u->u_access.ua_status;
		if (acl->len == 0)
			return (true);
		return (match_principals(acl, cred));
	} else
		return (false);	/* Unknown command — deny */

	/* Mutating commands (start/stop/restart/reload) -- deny if no ACL */
	if (acl->len == 0)
		return (false);

	return (match_principals(acl, cred));
}

/*
 * Send a UCL response on a connected socket.
 */
static int
send_response(int fd, const ucl_object_t *obj)
{
	unsigned char *buf;
	size_t len;
	uint32_t nlen;

	buf = ucl_object_emit(obj, UCL_EMIT_JSON_COMPACT);
	if (buf == NULL)
		return (-1);

	len = strlen((char *)buf);
	nlen = htonl((uint32_t)len);

	if (xwrite(fd, &nlen, sizeof(nlen)) != sizeof(nlen) ||
	    xwrite(fd, buf, len) != (ssize_t)len) {
		free(buf);
		return (-1);
	}

	free(buf);
	return (0);
}

/*
 * Shortcut: send an error response (status: "error", message: msg).
 */
static void
send_error(int fd, const char *msg)
{
	ucl_object_t *resp;

	resp = ucl_object_typed_new(UCL_OBJECT);
	ucl_object_insert_key(resp,
	    ucl_object_fromstring("error"), "status", 0, false);
	ucl_object_insert_key(resp,
	    ucl_object_fromstring(msg), "message", 0, false);
	send_response(fd, resp);
	ucl_object_unref(resp);
}

/*
 * Shortcut: send an OK response (status: "ok"), optionally with a message.
 */
static void
send_ok(int fd, const char *msg)
{
	ucl_object_t *resp;

	resp = ucl_object_typed_new(UCL_OBJECT);
	ucl_object_insert_key(resp,
	    ucl_object_fromstring("ok"), "status", 0, false);
	if (msg != NULL)
		ucl_object_insert_key(resp,
		    ucl_object_fromstring(msg), "message", 0, false);
	send_response(fd, resp);
	ucl_object_unref(resp);
}

/*
 * Build status response for one or all services.
 */
static ucl_object_t *
build_status_response(struct rcd_ctx *ctx, const char *svcname)
{
	ucl_object_t *top, *arr, *sobj;
	struct unit *u;

	top = ucl_object_typed_new(UCL_OBJECT);
	ucl_object_insert_key(top,
	    ucl_object_fromstring("ok"), "status", 0, false);

	arr = ucl_object_typed_new(UCL_ARRAY);

	TAILQ_FOREACH(u, &ctx->ctx_graph.dg_units, u_entries) {
		if (svcname != NULL && strcmp(u->u_name, svcname) != 0)
			continue;

		sobj = ucl_object_typed_new(UCL_OBJECT);
		ucl_object_insert_key(sobj,
		    ucl_object_fromstring(u->u_name), "name", 0, false);
		ucl_object_insert_key(sobj,
		    ucl_object_frombool(u->u_enabled), "enabled", 0, false);
		if (ctx->ctx_jailed && u->u_nojail)
			ucl_object_insert_key(sobj,
			    ucl_object_frombool(true), "nojail", 0, false);
		if (u->u_nostart)
			ucl_object_insert_key(sobj,
			    ucl_object_frombool(true), "nostart", 0, false);
		ucl_object_insert_key(sobj,
		    ucl_object_fromstring(state_names[u->u_state]),
		    "state", 0, false);
		ucl_object_insert_key(sobj,
		    ucl_object_fromint(u->u_pid), "pid", 0, false);
		ucl_object_insert_key(sobj,
		    ucl_object_fromstring(unit_type_str(u->u_type)),
		    "type", 0, false);

		ucl_array_append(arr, sobj);
	}

	ucl_object_insert_key(top, arr, "services", 0, false);
	return (top);
}

/*
 * Process a single control command.
 */
static void
process_command(struct rcd_ctx *ctx, int clientfd,
    const ucl_object_t *req, const struct xucred *cred)
{
	const ucl_object_t *cmd_obj, *svc_obj, *flag_obj;
	const char *cmd, *svc, *instance;
	ucl_object_t *resp;
	struct unit *u;
	bool force_flag, one_flag;

	cmd_obj = ucl_object_lookup(req, "command");
	if (cmd_obj == NULL)
		return;
	cmd = ucl_object_tostring(cmd_obj);

	svc_obj = ucl_object_lookup(req, "service");
	svc = (svc_obj != NULL) ? ucl_object_tostring(svc_obj) : NULL;

	/*
	 * Command modifier flags (like rc's fast/force/one/quiet prefixes).
	 *   force: bypass enable check, always run
	 *   one:   temporarily treat as enabled for this invocation
	 */
	force_flag = false;
	one_flag = false;
	flag_obj = ucl_object_lookup(req, "force");
	if (flag_obj != NULL)
		force_flag = ucl_object_toboolean(flag_obj);
	flag_obj = ucl_object_lookup(req, "one");
	if (flag_obj != NULL)
		one_flag = ucl_object_toboolean(flag_obj);

	if (strcmp(cmd, "status") == 0) {
		resp = build_status_response(ctx, svc);
		send_response(clientfd, resp);
		ucl_object_unref(resp);
		return;
	}

	/*
	 * Power management: suspend stops all services with the
	 * "resume" keyword, resume restarts them.  These are global
	 * commands that don't take a service name.
	 */
	if (strcmp(cmd, "suspend") == 0) {
		struct unit *su;
		int count;

		log_info("suspend: stopping resume-flagged services");
		count = 0;
		TAILQ_FOREACH(su, &ctx->ctx_graph.dg_units, u_entries) {
			if (!su->u_resume)
				continue;
			if (su->u_state != STATE_RUNNING)
				continue;
			log_info("suspend: stopping %s", su->u_name);
			proc_stop_sync(ctx, su);
			count++;
		}
		resp = ucl_object_typed_new(UCL_OBJECT);
		ucl_object_insert_key(resp,
		    ucl_object_fromstring("ok"), "status", 0, false);
		ucl_object_insert_key(resp,
		    ucl_object_fromint(count), "stopped", 0, false);
		send_response(clientfd, resp);
		ucl_object_unref(resp);
		return;
	}

	if (strcmp(cmd, "resume") == 0) {
		struct unit *su;
		int count;

		log_info("resume: restarting resume-flagged services");
		count = 0;
		TAILQ_FOREACH(su, &ctx->ctx_graph.dg_units, u_entries) {
			if (!su->u_resume)
				continue;
			if (su->u_state == STATE_RUNNING)
				continue;
			if (!su->u_enabled)
				continue;
			log_info("resume: starting %s", su->u_name);
			su->u_state = STATE_STARTING;
			su->u_retry_count = 0;
			proc_spawn(ctx, su);
			count++;
		}
		resp = ucl_object_typed_new(UCL_OBJECT);
		ucl_object_insert_key(resp,
		    ucl_object_fromstring("ok"), "status", 0, false);
		ucl_object_insert_key(resp,
		    ucl_object_fromint(count), "started", 0, false);
		send_response(clientfd, resp);
		ucl_object_unref(resp);
		return;
	}

	/* Commands that require a service name */
	if (svc == NULL) {
		send_error(clientfd, "service name required");
		return;
	}

	instance = NULL;
	u = depgraph_find(&ctx->ctx_graph, svc);

	/*
	 * Handle "service@instance" names.
	 * If the full name isn't found, split on '@', look up the
	 * base unit, and either instantiate a template or pass through
	 * to a legacy script with the instance as an argument.
	 */
	if (u == NULL) {
		const char *at = strchr(svc, '@');

		if (at != NULL) {
			char tmpl_name[256];
			struct unit *tmpl;
			size_t tlen;

			tlen = (size_t)(at - svc);
			if (tlen >= sizeof(tmpl_name))
				tlen = sizeof(tmpl_name) - 1;
			memcpy(tmpl_name, svc, tlen);
			tmpl_name[tlen] = '\0';

			tmpl = depgraph_find(&ctx->ctx_graph, tmpl_name);
			if (tmpl != NULL && tmpl->u_template) {
				u = unit_instantiate(tmpl, at + 1);
				if (u != NULL)
					depgraph_add(&ctx->ctx_graph, u);
			} else if (tmpl != NULL &&
			    (tmpl->u_type == UNIT_LEGACY ||
			    tmpl->u_type == UNIT_LEGACY_FORKING)) {
				/* Validate instance name: only safe chars */
				if (!valid_service_name(at + 1)) {
					log_warn("control: invalid "
					    "instance name '%s'", at + 1);
					send_error(clientfd,
					    "invalid instance name");
					return;
				}
				u = tmpl;
				instance = at + 1;
			}
		}
	}

	if (u == NULL) {
		send_error(clientfd, "service not found");
		return;
	}

	/*
	 * Per-service access control.
	 * check_credentials already verified the client is root or in
	 * the control_group.  For non-root clients that passed via
	 * control_group, also check per-service ACLs if defined.
	 */
	if (cred->cr_uid != 0 && !access_check(u, cmd, cred)) {
		log_warn("control: uid %d denied %s on %s",
		    cred->cr_uid, cmd, u->u_name);
		send_error(clientfd, "permission denied");
		return;
	}

	/*
	 * Legacy rc.d scripts: pass all commands directly to the
	 * script.  The script knows how to handle start, stop,
	 * restart, reload, status, and any extra_commands.
	 * Only enable/disable/show/describe are handled by rcd.
	 */
	if ((u->u_type == UNIT_LEGACY || u->u_type == UNIT_LEGACY_FORKING) &&
	    strcmp(cmd, "enable") != 0 &&
	    strcmp(cmd, "disable") != 0 &&
	    strcmp(cmd, "delete") != 0 &&
	    strcmp(cmd, "show") != 0 &&
	    strcmp(cmd, "describe") != 0 &&
	    strcmp(cmd, "rcvar") != 0 &&
	    strcmp(cmd, "resources") != 0) {
		char hook[PATH_MAX];
		const char *script_path;
		int rc, off;

		script_path = (u->u_template_ref != NULL) ?
		    u->u_template_ref->u_path : u->u_path;

		off = snprintf(hook, sizeof(hook), "%s %s %s",
		    _PATH_BSHELL, script_path, cmd);
		if (off >= (int)sizeof(hook))
			off = (int)sizeof(hook) - 1;

		/*
		 * Append instance names.  If the request has an
		 * "instances" array, pass them all as arguments
		 * so the script gets: sh script cmd inst1 inst2
		 * Otherwise, if this is a single instance, pass it.
		 */
		{
			const ucl_object_t *insts, *ival;
			ucl_object_iter_t iit;
			int n;

			insts = ucl_object_lookup(req, "instances");
			if (insts != NULL &&
			    ucl_object_type(insts) == UCL_ARRAY) {
				iit = ucl_object_iterate_new(insts);
				while ((ival = ucl_object_iterate_safe(iit,
				    true)) != NULL &&
				    off < (int)sizeof(hook) - 1) {
					n = snprintf(hook + off,
					    sizeof(hook) - off, " %s",
					    ucl_object_tostring(ival));
					if (n >= (int)(sizeof(hook) - off))
						break;
					off += n;
				}
				ucl_object_iterate_free(iit);
			} else if (u->u_instance != NULL &&
			    off < (int)sizeof(hook) - 1) {
				n = snprintf(hook + off,
				    sizeof(hook) - off, " %s",
				    u->u_instance);
				if (n < (int)(sizeof(hook) - off))
					off += n;
			} else if (instance != NULL &&
			    off < (int)sizeof(hook) - 1) {
				n = snprintf(hook + off,
				    sizeof(hook) - off, " %s",
				    instance);
				if (n < (int)(sizeof(hook) - off))
					off += n;
			}
		}

		log_info("%s: legacy %s", u->u_name, cmd);
		rc = proc_run_hook(hook);

		/*
		 * Update state based on command and result.
		 */
		if (strcmp(cmd, "start") == 0)
			u->u_state = (rc == 0) ? STATE_DONE : STATE_FAILED;
		else if (strcmp(cmd, "stop") == 0 && rc == 0)
			u->u_state = STATE_INACTIVE;

		resp = ucl_object_typed_new(UCL_OBJECT);
		ucl_object_insert_key(resp,
		    ucl_object_fromstring(rc == 0 ? "ok" : "error"),
		    "status", 0, false);
		if (rc != 0)
			ucl_object_insert_key(resp,
			    ucl_object_fromstring("command failed"),
			    "message", 0, false);
		send_response(clientfd, resp);
		ucl_object_unref(resp);
		return;
	}

	if (strcmp(cmd, "start") == 0) {
		/*
		 * force/one: bypass disabled check.  "force" also
		 * skips precondition checks (like rc's forcestart).
		 * "one" just enables temporarily (like onestart).
		 */
		if (!u->u_enabled && !force_flag && !one_flag) {
			send_error(clientfd, "service is disabled");
			return;
		}
		if (u->u_state == STATE_RUNNING) {
			send_ok(clientfd, "already running");
			return;
		}
		u->u_state = STATE_STARTING;
		proc_spawn(ctx, u);
	} else if (strcmp(cmd, "stop") == 0) {
		proc_stop(ctx, u);
	} else if (strcmp(cmd, "restart") == 0) {
		/*
		 * Synchronous restart with timeout via proc_stop_sync,
		 * then immediate re-spawn.  proc_stop_sync uses a
		 * temporary kqueue and does not block indefinitely.
		 */
		proc_stop_sync(ctx, u);
		u->u_state = STATE_STARTING;
		u->u_retry_count = 0;
		proc_spawn(ctx, u);
	} else if (strcmp(cmd, "reload") == 0) {
		proc_reload(ctx, u);
	} else if (strcmp(cmd, "enable") == 0) {
		enable_service(u->u_name, NULL);
		u->u_enabled = true;
	} else if (strcmp(cmd, "disable") == 0) {
		disable_service(u->u_name, NULL);
		u->u_enabled = false;
	} else if (strcmp(cmd, "delete") == 0) {
		delete_override(u->u_name);
		/* Reset to unit file defaults */
		u->u_enabled = true;
		free(u->u_override_conf);
		u->u_override_conf = NULL;
	} else if (strcmp(cmd, "rcvar") == 0) {
		char path[PATH_MAX];

		resp = ucl_object_typed_new(UCL_OBJECT);
		ucl_object_insert_key(resp,
		    ucl_object_fromstring("ok"), "status", 0, false);
		ucl_object_insert_key(resp,
		    ucl_object_fromstring(u->u_name), "name", 0, false);
		ucl_object_insert_key(resp,
		    ucl_object_frombool(u->u_enabled), "enabled", 0, false);
		ucl_object_insert_key(resp,
		    ucl_object_fromstring(unit_type_str(u->u_type)),
		    "type", 0, false);
		if (u->u_path != NULL)
			ucl_object_insert_key(resp,
			    ucl_object_fromstring(u->u_path),
			    "unit_file", 0, false);
		snprintf(path, sizeof(path), "/etc/rcd.conf.d/%s", u->u_name);
		if (access(path, F_OK) == 0)
			ucl_object_insert_key(resp,
			    ucl_object_fromstring(path),
			    "override_file", 0, false);
		send_response(clientfd, resp);
		ucl_object_unref(resp);
		return;
	} else if (strcmp(cmd, "describe") == 0) {
		resp = ucl_object_typed_new(UCL_OBJECT);
		ucl_object_insert_key(resp,
		    ucl_object_fromstring("ok"), "status", 0, false);
		ucl_object_insert_key(resp,
		    ucl_object_fromstring(
			u->u_description ? u->u_description : u->u_name),
		    "description", 0, false);
		ucl_object_insert_key(resp,
		    ucl_object_fromstring(unit_type_str(u->u_type)),
		    "type", 0, false);
		ucl_object_insert_key(resp,
		    ucl_object_frombool(u->u_enabled),
		    "enabled", 0, false);
		if (u->u_command != NULL)
			ucl_object_insert_key(resp,
			    ucl_object_fromstring(u->u_command),
			    "command", 0, false);
		/* Include custom commands */
		if (!STAILQ_EMPTY(&u->u_commands)) {
			ucl_object_t *earr;
			struct kv *cmd_kv;

			earr = ucl_object_typed_new(UCL_ARRAY);
			STAILQ_FOREACH(cmd_kv, &u->u_commands, kv_entries)
				ucl_array_append(earr,
				    ucl_object_fromstring(cmd_kv->kv_key));
			ucl_object_insert_key(resp, earr,
			    "commands", 0, false);
		}
		send_response(clientfd, resp);
		ucl_object_unref(resp);
		return;
	} else if (strcmp(cmd, "show") == 0) {
		/*
		 * Dump the full effective unit configuration as UCL.
		 * This reflects the unit file + any overrides applied.
		 */
		ucl_object_t *cfg;
		struct kv *kv_p;

		resp = ucl_object_typed_new(UCL_OBJECT);
		ucl_object_insert_key(resp,
		    ucl_object_fromstring("ok"), "status", 0, false);

		cfg = ucl_object_typed_new(UCL_OBJECT);

		ucl_object_insert_key(cfg,
		    ucl_object_fromstring(u->u_name), "name", 0, false);
		if (u->u_description != NULL)
			ucl_object_insert_key(cfg,
			    ucl_object_fromstring(u->u_description),
			    "description", 0, false);

		ucl_object_insert_key(cfg,
		    ucl_object_fromstring(unit_type_str(u->u_type)),
		    "type", 0, false);

		if (u->u_command != NULL)
			ucl_object_insert_key(cfg,
			    ucl_object_fromstring(u->u_command),
			    "command", 0, false);
		if (u->u_command_args != NULL)
			ucl_object_insert_key(cfg,
			    ucl_object_fromstring(u->u_command_args),
			    "command_args", 0, false);
		if (u->u_exec != NULL)
			ucl_object_insert_key(cfg,
			    ucl_object_fromstring(u->u_exec),
			    "exec", 0, false);

		ucl_object_insert_key(cfg,
		    ucl_object_frombool(u->u_enabled), "enabled", 0, false);

		if (u->u_path != NULL)
			ucl_object_insert_key(cfg,
			    ucl_object_fromstring(u->u_path),
			    "path", 0, false);

		/* Provide/require/before */
		if (u->u_provide.len > 0) {
			ucl_object_t *arr = ucl_object_typed_new(UCL_ARRAY);
			vec_foreach(u->u_provide, pi)
				ucl_array_append(arr,
				    ucl_object_fromstring(u->u_provide.d[pi]));
			ucl_object_insert_key(cfg, arr, "provides", 0, false);
		}
		if (u->u_require.len > 0) {
			ucl_object_t *arr = ucl_object_typed_new(UCL_ARRAY);
			vec_foreach(u->u_require, ri)
				ucl_array_append(arr,
				    ucl_object_fromstring(u->u_require.d[ri]));
			ucl_object_insert_key(cfg, arr, "requires", 0, false);
		}
		if (u->u_before.len > 0) {
			ucl_object_t *arr = ucl_object_typed_new(UCL_ARRAY);
			vec_foreach(u->u_before, bi)
				ucl_array_append(arr,
				    ucl_object_fromstring(u->u_before.d[bi]));
			ucl_object_insert_key(cfg, arr, "before", 0, false);
		}

		/* Process config */
		if (u->u_proc.pc_user != NULL || u->u_proc.pc_group != NULL ||
		    u->u_proc.pc_nice != 0 || u->u_proc.pc_oom_protect) {
			ucl_object_t *proc = ucl_object_typed_new(UCL_OBJECT);
			if (u->u_proc.pc_user != NULL)
				ucl_object_insert_key(proc,
				    ucl_object_fromstring(u->u_proc.pc_user),
				    "user", 0, false);
			if (u->u_proc.pc_group != NULL)
				ucl_object_insert_key(proc,
				    ucl_object_fromstring(u->u_proc.pc_group),
				    "group", 0, false);
			if (u->u_proc.pc_nice != 0)
				ucl_object_insert_key(proc,
				    ucl_object_fromint(u->u_proc.pc_nice),
				    "nice", 0, false);
			if (u->u_proc.pc_cpuset != NULL)
				ucl_object_insert_key(proc,
				    ucl_object_fromstring(u->u_proc.pc_cpuset),
				    "cpuset", 0, false);
			if (u->u_proc.pc_fib != 0)
				ucl_object_insert_key(proc,
				    ucl_object_fromint(u->u_proc.pc_fib),
				    "fib", 0, false);
			ucl_object_insert_key(proc,
			    ucl_object_frombool(u->u_proc.pc_oom_protect),
			    "oom_protect", 0, false);
			ucl_object_insert_key(cfg, proc, "process", 0, false);
		}

		/* Restart config */
		if (u->u_restart.rc_policy != RESTART_NEVER) {
			ucl_object_t *rst = ucl_object_typed_new(UCL_OBJECT);
			const char *pol = u->u_restart.rc_policy == RESTART_ALWAYS ?
			    "always" : "on-failure";
			ucl_object_insert_key(rst,
			    ucl_object_fromstring(pol), "policy", 0, false);
			ucl_object_insert_key(rst,
			    ucl_object_fromint(u->u_restart.rc_max_retries),
			    "max_retries", 0, false);
			ucl_object_insert_key(rst,
			    ucl_object_fromint(u->u_restart.rc_delay_ms),
			    "delay", 0, false);
			ucl_object_insert_key(cfg, rst, "restart", 0, false);
		}

		/* Signals */
		if (u->u_sig_stop != SIGTERM)
			ucl_object_insert_key(cfg,
			    ucl_object_fromint(u->u_sig_stop),
			    "sig_stop", 0, false);
		if (u->u_sig_reload != SIGHUP)
			ucl_object_insert_key(cfg,
			    ucl_object_fromint(u->u_sig_reload),
			    "sig_reload", 0, false);

		/* Start delay */
		if (u->u_start_delay_ms > 0)
			ucl_object_insert_key(cfg,
			    ucl_object_fromint(u->u_start_delay_ms),
			    "start_delay", 0, false);

		/* Hooks */
		if (u->u_start_precmd != NULL)
			ucl_object_insert_key(cfg,
			    ucl_object_fromstring(u->u_start_precmd),
			    "start_precmd", 0, false);
		if (u->u_stop_command != NULL)
			ucl_object_insert_key(cfg,
			    ucl_object_fromstring(u->u_stop_command),
			    "stop_command", 0, false);

		/* Custom commands */
		if (!STAILQ_EMPTY(&u->u_commands)) {
			ucl_object_t *cmds = ucl_object_typed_new(UCL_OBJECT);
			STAILQ_FOREACH(kv_p, &u->u_commands, kv_entries)
				ucl_object_insert_key(cmds,
				    ucl_object_fromstring(kv_p->kv_val),
				    kv_p->kv_key, 0, false);
			ucl_object_insert_key(cfg, cmds,
			    "commands", 0, false);
		}

		/* Runtime state */
		{
			ucl_object_t *state = ucl_object_typed_new(UCL_OBJECT);

			ucl_object_insert_key(state,
			    ucl_object_fromstring(state_names[u->u_state]),
			    "state", 0, false);
			ucl_object_insert_key(state,
			    ucl_object_fromint(u->u_pid),
			    "pid", 0, false);
			ucl_object_insert_key(state,
			    ucl_object_fromint(u->u_retry_count),
			    "retry_count", 0, false);
			ucl_object_insert_key(cfg, state,
			    "runtime", 0, false);
		}

		ucl_object_insert_key(resp, cfg, "config", 0, false);
		send_response(clientfd, resp);
		ucl_object_unref(resp);
		return;
	} else if (strcmp(cmd, "resources") == 0) {
		char usage[4096];
		if (rctl_get_usage(u, usage, sizeof(usage)) == 0) {
			resp = ucl_object_typed_new(UCL_OBJECT);
			ucl_object_insert_key(resp,
			    ucl_object_fromstring("ok"), "status", 0, false);
			ucl_object_insert_key(resp,
			    ucl_object_fromstring(usage),
			    "resources", 0, false);
			send_response(clientfd, resp);
			ucl_object_unref(resp);
			return;
		}
		/*
		 * rctl_get_usage writes a detailed reason into usage
		 * on failure (e.g., RACCT not in kernel, service not
		 * running).  Send it back to the client.
		 */
		send_error(clientfd, usage);
		return;
	} else {
		/*
		 * Not a built-in command — look it up in the unit's
		 * custom commands map.
		 */
		struct kv *cmd_kv;

		STAILQ_FOREACH(cmd_kv, &u->u_commands, kv_entries) {
			if (strcmp(cmd_kv->kv_key, cmd) == 0) {
				int rc;

				log_info("%s: running command %s",
				    u->u_name, cmd);
				rc = proc_run_hook(cmd_kv->kv_val);
				resp = ucl_object_typed_new(UCL_OBJECT);
				ucl_object_insert_key(resp,
				    ucl_object_fromstring(
					rc == 0 ? "ok" : "error"),
				    "status", 0, false);
				if (rc != 0)
					ucl_object_insert_key(resp,
					    ucl_object_fromstring(
						"command failed"),
					    "message", 0, false);
				send_response(clientfd, resp);
				ucl_object_unref(resp);
				return;
			}
		}

		/* Unknown command */
		send_error(clientfd, "unknown command");
		return;
	}

	/* Generic OK response */
	send_ok(clientfd, NULL);
}

/*
 * Initialize the control socket.
 */
int
control_init(struct rcd_ctx *ctx)
{
	struct sockaddr_un sun;
	struct kevent kev;
	int fd, dirfd, sockfd;
	mode_t old_umask;
	char *dir, *base, *pathcopy, *p;

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		log_warn("control socket: %s", strerror(errno));
		return (-1);
	}

	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_UNIX;
	strlcpy(sun.sun_path, ctx->ctx_config.cfg_control_socket,
	    sizeof(sun.sun_path));

	/*
	 * Use dirfd-relative operations (funlinkat, bindat, fchmodat)
	 * to avoid TOCTOU races between lstat/unlink/bind/chmod.
	 */

	/*
	 * Ensure the parent directory exists.
	 * During early boot /var/run (tmpfs) may not exist yet.
	 * Walk the path and mkdir each component (mkdir -p
	 * semantics) so the socket can be created regardless.
	 */
	pathcopy = xstrdup(sun.sun_path);
	dir = dirname(pathcopy);
	for (p = dir; *p != '\0'; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		(void)mkdir(dir, 0755);
		*p = '/';
	}
	(void)mkdir(dir, 0755);
	free(pathcopy);

	pathcopy = xstrdup(sun.sun_path);
	dir = dirname(pathcopy);
	dirfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	free(pathcopy);
	if (dirfd < 0) {
		log_warn("control: open %s: %s", dir,
		    strerror(errno));
		close(fd);
		return (-1);
	}

	pathcopy = xstrdup(sun.sun_path);
	base = basename(pathcopy);

	/* Remove stale socket if present */
	sockfd = openat(dirfd, base, O_PATH | O_CLOEXEC);
	if (sockfd >= 0) {
		funlinkat(dirfd, base, sockfd, 0);
		close(sockfd);
	}

	old_umask = umask(0177);
	if (bindat(dirfd, fd, (struct sockaddr *)&sun,
	    SUN_LEN(&sun)) != 0) {
		log_warn("control bindat: %s", strerror(errno));
		umask(old_umask);
		free(pathcopy);
		close(dirfd);
		close(fd);
		return (-1);
	}
	umask(old_umask);

	if (fchmodat(dirfd, base,
	    ctx->ctx_config.cfg_control_perms, 0) != 0)
		log_warn("control fchmodat: %s", strerror(errno));

	ctx->ctx_ctlsock_pathfd = openat(dirfd, base,
	    O_RDONLY | O_CLOEXEC);
	if (ctx->ctx_ctlsock_pathfd < 0)
		log_warn("control openat: %s", strerror(errno));

	free(pathcopy);
	close(dirfd);

	if (listen(fd, 16) != 0) {
		log_warn("control listen: %s", strerror(errno));
		close(fd);
		return (-1);
	}

	ctx->ctx_ctlsock = fd;

	/* Register in kqueue for incoming connections */
	EV_SET(&kev, fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
	if (kevent(ctx->ctx_kq, &kev, 1, NULL, 0, NULL) != 0) {
		log_warn("control kevent: %s", strerror(errno));
		close(fd);
		ctx->ctx_ctlsock = -1;
		return (-1);
	}

	/*
	 * Watch the socket file for deletion (e.g., cleanvar).
	 * EVFILT_VNODE with NOTE_DELETE fires when the socket is unlinked.
	 * We use EV_CLEAR (not EV_ONESHOT) so the watch remains active
	 * after each event — control_reinit() will close the old fd and
	 * register a new one on the recreated socket.
	 *
	 * The pathfd was opened immediately after bindat() above, while
	 * we still held the parent directory fd, eliminating the race
	 * where cleanvar deletes the socket between bindat() and openat().
	 */
	if (ctx->ctx_ctlsock_pathfd >= 0) {
		EV_SET(&kev, ctx->ctx_ctlsock_pathfd, EVFILT_VNODE,
		    EV_ADD | EV_CLEAR, NOTE_DELETE, 0, NULL);
		if (kevent(ctx->ctx_kq, &kev, 1, NULL, 0, NULL) != 0) {
			log_warn("control vnode kevent: %s",
			    strerror(errno));
			close(ctx->ctx_ctlsock_pathfd);
			ctx->ctx_ctlsock_pathfd = -1;
		}
	}

	log_info("control socket: %s", sun.sun_path);
	return (0);
}

/*
 * Recreate the control socket after it was deleted (e.g., by cleanvar).
 */
void
control_reinit(struct rcd_ctx *ctx)
{

	log_info("control socket deleted, recreating");
	if (ctx->ctx_ctlsock_pathfd >= 0) {
		close(ctx->ctx_ctlsock_pathfd);
		ctx->ctx_ctlsock_pathfd = -1;
	}
	control_close(ctx);
	control_init(ctx);
}

/*
 * Handle activity on the control socket (accept + read + process).
 */
void
control_handle(struct rcd_ctx *ctx, int listenfd)
{
	struct ucl_parser *parser;
	ucl_object_t *req;
	struct xucred cred;
	struct timeval tv;
	socklen_t credlen;
	int clientfd;
	char buf[CTL_MAX_MSG];
	uint32_t msglen;
	ssize_t n;

	clientfd = accept4(listenfd, NULL, NULL,
	    SOCK_CLOEXEC | SOCK_NONBLOCK);
	if (clientfd < 0) {
		if (errno != EINTR && errno != ECONNABORTED)
			log_warn("accept: %s", strerror(errno));
		return;
	}

	/*
	 * Set a receive timeout to prevent a malicious client from
	 * blocking the event loop by connecting and never sending data.
	 * Switch to blocking mode with a 5-second timeout.
	 */
	{
		int flags = fcntl(clientfd, F_GETFL, 0);
		if (flags < 0 ||
		    fcntl(clientfd, F_SETFL, flags & ~O_NONBLOCK) < 0) {
			log_warn("control: fcntl: %s", strerror(errno));
			close(clientfd);
			return;
		}
	}
	tv.tv_sec = 5;
	tv.tv_usec = 0;
	if (setsockopt(clientfd, SOL_SOCKET, SO_RCVTIMEO,
	    &tv, sizeof(tv)) != 0)
		log_warn("control: SO_RCVTIMEO: %s", strerror(errno));

	/*
	 * Get peer credentials.  We need these both for the global
	 * authorization check and for per-service ACLs.
	 */
	credlen = sizeof(cred);
	if (getsockopt(clientfd, SOL_LOCAL, LOCAL_PEERCRED,
	    &cred, &credlen) != 0 || cred.cr_version != XUCRED_VERSION) {
		close(clientfd);
		return;
	}

	if (!check_credentials(clientfd, ctx->ctx_config.cfg_control_group)) {
		log_warn("control: unauthorized client (uid %d)",
		    cred.cr_uid);
		close(clientfd);
		return;
	}

	/* Read length-prefixed message (handles EINTR and partial reads) */
	n = xread(clientfd, &msglen, sizeof(msglen));
	if (n != (ssize_t)sizeof(msglen)) {
		close(clientfd);
		return;
	}
	msglen = ntohl(msglen);
	if (msglen == 0 || msglen >= CTL_MAX_MSG) {
		close(clientfd);
		return;
	}

	n = xread(clientfd, buf, msglen);
	if (n != (ssize_t)msglen) {
		close(clientfd);
		return;
	}
	buf[msglen] = '\0';

	/* Parse UCL request */
	parser = ucl_parser_new(UCL_PARSER_DEFAULT);
	if (!ucl_parser_add_string(parser, buf, msglen)) {
		ucl_parser_free(parser);
		close(clientfd);
		return;
	}
	req = ucl_parser_get_object(parser);
	ucl_parser_free(parser);

	if (req != NULL) {
		process_command(ctx, clientfd, req, &cred);
		ucl_object_unref(req);
	}

	close(clientfd);
}

void
control_close(struct rcd_ctx *ctx)
{

	if (ctx->ctx_ctlsock >= 0) {
		close(ctx->ctx_ctlsock);
		unlink(ctx->ctx_config.cfg_control_socket);
		ctx->ctx_ctlsock = -1;
	}
}
