/*
 * Copyright (c) 2026 Baptiste Daroussin <bapt@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * rcd — FreeBSD Service Manager
 *
 * Main daemon.  Called by init(8) during boot.  Reads unit files, builds
 * a dependency graph, starts services in parallel, then enters a kqueue
 * supervision loop.  Forks to background when boot completes,
 * signaling init(8) via the parent's exit.
 */

#include <sys/param.h>
#include <sys/event.h>
#include <sys/eventfd.h>
#include <sys/procctl.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/wait.h>

#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <paths.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include <ucl.h>

#include "rcd.h"

#define RCD_CONF_PATH		"/etc/rcd.conf"
#define RCD_UNIT_DIR		"/etc/rcd.d"
#define RCD_LEGACY_DIR		"/etc/rc.d"
#define RCD_CONTROL_SOCK	"/var/run/rcd.sock"
#define RCD_STATE_FILE		"/var/run/rcd.state"

/* Forward declarations for static functions used before definition */
static int	config_load(struct rcd_config *);
static int	load_units_from_dir(struct rcd_ctx *, const char *);
static int	load_legacy_from_dir(struct rcd_ctx *, const char *);
static void	rescan_localbase(struct rcd_ctx *);
static void	filter_units_by_context(struct rcd_ctx *);
static void	remove_firstboot(const char *);

static char localbase[PATH_MAX];

#include "conf_schema.h"
#include "unit_schema.h"

/*
 * Determine localbase from sysctl user.localbase (default: /usr/local).
 */
static void
get_localbase(void)
{
	size_t len;

	len = sizeof(localbase);
	if (sysctlbyname("user.localbase", localbase, &len, NULL, 0) != 0)
		strlcpy(localbase, "/usr/local", sizeof(localbase));
}

/*
 * Parse an embedded JSON schema string into a UCL object.
 */
static ucl_object_t *
load_embedded_schema(const char *data, size_t len)
{
	struct ucl_parser *parser;
	ucl_object_t *schema;

	parser = ucl_parser_new(UCL_PARSER_DEFAULT);
	if (parser == NULL)
		return (NULL);
	if (!ucl_parser_add_string(parser, data, len)) {
		ucl_parser_free(parser);
		return (NULL);
	}
	schema = ucl_parser_get_object(parser);
	ucl_parser_free(parser);
	return (schema);
}

/*
 * Check if we are running inside a jail.
 */
static bool
check_jailed(void)
{
	int jailed;
	size_t len;

	len = sizeof(jailed);
	if (sysctlbyname("security.jail.jailed", &jailed, &len,
	    NULL, 0) != 0)
		return (false);
	return (jailed != 0);
}

/*
 * Check if this is a diskless boot via vfs.nfs.diskless_valid sysctl.
 */
static bool
check_diskless(void)
{
	int val;
	size_t len;

	val = 0;
	len = sizeof(val);
	sysctlbyname("vfs.nfs.diskless_valid", &val, &len, NULL, 0);
	return (val != 0);
}

/*
 * Detect if this is a re-exec upgrade.
 * The state file must exist AND at least one saved procdesc fd
 * must be valid (inherited from the old process).
 */
static bool
check_upgrade_state(void)
{
	struct ucl_parser *sp;
	ucl_object_t *st;
	const ucl_object_t *arr, *sobj, *fobj;
	ucl_object_iter_t sit;
	bool found;

	if (access(RCD_STATE_FILE, F_OK) != 0)
		return (false);

	found = false;
	sp = ucl_parser_new(UCL_PARSER_DEFAULT);
	if (ucl_parser_add_file(sp, RCD_STATE_FILE)) {
		st = ucl_parser_get_object(sp);
		if (st != NULL) {
			arr = ucl_object_lookup(st, "services");
			if (arr != NULL) {
				sit = ucl_object_iterate_new(arr);
				while ((sobj = ucl_object_iterate_safe(
				    sit, true)) != NULL) {
					fobj = ucl_object_lookup(sobj, "fd");
					if (fobj != NULL &&
					    fcntl(ucl_object_toint(fobj),
					    F_GETFD) >= 0) {
						found = true;
						break;
					}
				}
				ucl_object_iterate_free(sit);
			}
			ucl_object_unref(st);
		}
	}
	ucl_parser_free(sp);

	if (!found) {
		log_info("stale state file, ignoring");
		unlink(RCD_STATE_FILE);
	}
	return (found);
}

/*
 * Reload configuration: rescan unit directories and rebuild the DAG.
 * Existing running services are not affected; new/changed units are
 * picked up and stopped/removed units are marked for cleanup.
 */
static void
do_reload_config(struct rcd_ctx *ctx)
{
	char local_unit_dir[PATH_MAX];
	char local_legacy_dir[PATH_MAX];

	log_info("reloading configuration");

	snprintf(local_unit_dir, sizeof(local_unit_dir),
	    "%s/etc/rcd.d", localbase);
	snprintf(local_legacy_dir, sizeof(local_legacy_dir),
	    "%s/etc/rc.d", localbase);

	/*
	 * Re-read global config.  This does not affect already-running
	 * services, but will be used for new starts.
	 */
	config_load(&ctx->ctx_config);

	/*
	 * Scan for new unit files.  depgraph_add skips duplicates
	 * (units with provisions already registered).
	 */
	load_units_from_dir(ctx, RCD_UNIT_DIR);
	load_units_from_dir(ctx, local_unit_dir);
	load_legacy_from_dir(ctx, RCD_LEGACY_DIR);
	load_legacy_from_dir(ctx, local_legacy_dir);

	/* Re-resolve to pick up new dependencies */
	depgraph_resolve(&ctx->ctx_graph);
}

/*
 * Load global configuration from /etc/rcd.conf.
 */
static int
config_load(struct rcd_config *cfg)
{
	struct ucl_parser *parser;
	ucl_object_t *top;
	const ucl_object_t *val;

	/* Free previous allocations (safe on first call — all NULL) */
	free(cfg->cfg_control_socket);
	free(cfg->cfg_control_group);
	free(cfg->cfg_firstboot_sentinel);
	for (int ci = 0; ci < cfg->cfg_nlegacy_rc_conf; ci++)
		free(cfg->cfg_legacy_rc_conf[ci]);
	free(cfg->cfg_legacy_rc_conf);

	/* Set defaults */
	cfg->cfg_parallel = true;
	cfg->cfg_max_parallel = 0;
	cfg->cfg_control_socket = xstrdup(RCD_CONTROL_SOCK);
	cfg->cfg_control_perms = 0660;
	cfg->cfg_control_group = xstrdup("wheel");
	cfg->cfg_log_level = LOG_INFO;
	cfg->cfg_stop_timeout_ms = 10000;
	cfg->cfg_shutdown_timeout_ms = 90000;
	cfg->cfg_firstboot_sentinel = xstrdup("/firstboot");

	/* Default legacy rc.conf paths */
	cfg->cfg_legacy_rc_conf = xcalloc(3, sizeof(char *));
	cfg->cfg_legacy_rc_conf[0] = xstrdup("/etc/defaults/rc.conf");
	cfg->cfg_legacy_rc_conf[1] = xstrdup("/etc/rc.conf");
	cfg->cfg_legacy_rc_conf[2] = xstrdup("/etc/rc.conf.local");
	cfg->cfg_nlegacy_rc_conf = 3;

	/* Parse /etc/rcd.conf with libucl */
	parser = ucl_parser_new(UCL_PARSER_DEFAULT);
	if (parser == NULL)
		return (0);

	if (!ucl_parser_add_file(parser, RCD_CONF_PATH)) {
		/* Config file is optional */
		ucl_parser_free(parser);
		return (0);
	}

	top = ucl_parser_get_object(parser);
	ucl_parser_free(parser);
	if (top == NULL)
		return (0);

	val = ucl_object_lookup(top, "parallel");
	if (val != NULL)
		cfg->cfg_parallel = ucl_object_toboolean(val);

	val = ucl_object_lookup(top, "max_parallel");
	if (val != NULL)
		cfg->cfg_max_parallel = ucl_object_toint(val);

	val = ucl_object_lookup(top, "control_socket");
	if (val != NULL) {
		free(cfg->cfg_control_socket);
		cfg->cfg_control_socket =
		    xstrdup(ucl_object_tostring(val));
	}

	val = ucl_object_lookup(top, "control_permissions");
	if (val != NULL) {
		mode_t m;

		if (ucl_parse_mode(val, &m) == 0)
			cfg->cfg_control_perms = m;
		else
			log_warn("invalid control_permissions");
	}

	val = ucl_object_lookup(top, "control_group");
	if (val != NULL) {
		free(cfg->cfg_control_group);
		cfg->cfg_control_group =
		    xstrdup(ucl_object_tostring(val));
	}

	val = ucl_object_lookup(top, "log_level");
	if (val != NULL) {
		const char *s = ucl_object_tostring(val);
		if (strcmp(s, "debug") == 0)
			cfg->cfg_log_level = LOG_DEBUG;
		else if (strcmp(s, "warning") == 0)
			cfg->cfg_log_level = LOG_WARNING;
		else if (strcmp(s, "error") == 0)
			cfg->cfg_log_level = LOG_ERR;
		else
			cfg->cfg_log_level = LOG_INFO;
	}

	val = ucl_object_lookup(top, "stop_timeout_ms");
	if (val != NULL)
		cfg->cfg_stop_timeout_ms = ucl_object_toint(val);

	val = ucl_object_lookup(top, "shutdown_timeout_ms");
	if (val != NULL)
		cfg->cfg_shutdown_timeout_ms = ucl_object_toint(val);

	val = ucl_object_lookup(top, "precious_machine");
	if (val != NULL)
		cfg->cfg_precious_machine = ucl_object_toboolean(val);

	val = ucl_object_lookup(top, "quiet_boot");
	if (val != NULL)
		cfg->cfg_quiet_boot = ucl_object_toboolean(val);

	val = ucl_object_lookup(top, "veriexec");
	if (val != NULL)
		cfg->cfg_veriexec = ucl_object_toboolean(val);

	ucl_object_unref(top);
	return (0);
}

/*
 * Scan a directory for .ucl unit files and add them to the graph.
 */
static int
load_units_from_dir(struct rcd_ctx *ctx, const char *dirpath)
{
	DIR *dp;
	struct dirent *de;
	struct unit *u;
	char path[PATH_MAX];
	size_t len;

	dp = opendir(dirpath);
	if (dp == NULL) {
		if (errno == ENOENT)
			return (0);
		log_warn("opendir %s: %s", dirpath, strerror(errno));
		return (-1);
	}

	while ((de = readdir(dp)) != NULL) {
		len = strlen(de->d_name);
		if (len < 5 || strcmp(de->d_name + len - 4, ".ucl") != 0)
			continue;

		if (snprintf(path, sizeof(path), "%s/%s",
		    dirpath, de->d_name) >= (int)sizeof(path))
			continue;

		/*
		 * VERIEXEC: if enabled, verify unit file ownership and
		 * permissions before trusting it.  Unit files must be
		 * owned by root and not world-writable.
		 */
		if (ctx->ctx_config.cfg_veriexec) {
			struct stat sb;

			if (stat(path, &sb) != 0)
				continue;
			if (sb.st_uid != 0) {
				log_warn("veriexec: %s not owned by root, "
				    "skipping", path);
				continue;
			}
			if (sb.st_mode & S_IWOTH) {
				log_warn("veriexec: %s is world-writable, "
				    "skipping", path);
				continue;
			}
		}

		u = unit_parse(path, &ctx->ctx_config);
		if (u == NULL) {
			log_warn("failed to parse unit: %s", path);
			continue;
		}

		/* Skip if this provision is already registered (rescan) */
		if (u->u_provide.len > 0 &&
		    depgraph_find(&ctx->ctx_graph, u->u_provide.d[0]) != NULL) {
			unit_free(u);
			continue;
		}

		depgraph_add(&ctx->ctx_graph, u);
	}

	closedir(dp);
	return (0);
}

/*
 * Scan a directory for legacy rc.d scripts and wrap them as units.
 */
static int
load_legacy_from_dir(struct rcd_ctx *ctx, const char *dirpath)
{

	return (compat_scan(ctx, dirpath));
}

/*
 * Re-scan localbase directories for units and legacy scripts.
 *
 * Called when the FILESYSTEMS barrier is reached, meaning /usr (and
 * localbase) is now mounted.
 * This is equivalent of rc(8)'s two-phase boot where rcorder is called
 * a second time after FILESYSTEMS to discover scripts from
 * <localbase>/etc/rc.d/.
 *
 */
static void
rescan_localbase(struct rcd_ctx *ctx)
{
	char local_unit_dir[PATH_MAX];
	char local_legacy_dir[PATH_MAX];
	unsigned int before;
	struct unit *u;

	snprintf(local_unit_dir, sizeof(local_unit_dir),
	    "%s/etc/rcd.d", localbase);
	snprintf(local_legacy_dir, sizeof(local_legacy_dir),
	    "%s/etc/rc.d", localbase);

	before = ctx->ctx_graph.dg_nunits;

	load_units_from_dir(ctx, local_unit_dir);
	load_legacy_from_dir(ctx, local_legacy_dir);

	if (ctx->ctx_graph.dg_nunits > before) {
		log_info("FILESYSTEMS: loaded %u new units from %s",
		    ctx->ctx_graph.dg_nunits - before, localbase);

		/* Re-resolve dependencies with the new units */
		depgraph_resolve(&ctx->ctx_graph);

		/* Apply keyword filtering to new units */
		filter_units_by_context(ctx);

		/* Apply overrides to new units */
		TAILQ_FOREACH(u, &ctx->ctx_graph.dg_units, u_entries)
			unit_apply_overrides(u, "/etc/rcd.conf.d");
	}
}

/*
 * Start all services that are ready (dependencies satisfied).
 */
static int
schedule_ready(struct rcd_ctx *ctx)
{
	struct unit **ready;
	int nready, i, cap;

	cap = ctx->ctx_graph.dg_nunits;
	if (cap == 0)
		return (0);
	ready = xcalloc(cap, sizeof(*ready));
	if (ready == NULL)
		return (-1);

retry:
	depgraph_ready_set(&ctx->ctx_graph, ready, &nready);

	for (i = 0; i < nready; i++) {
		if (ctx->ctx_config.cfg_max_parallel > 0 &&
		    ctx->ctx_running >= ctx->ctx_config.cfg_max_parallel)
			break;

		if (ready[i]->u_state != STATE_INACTIVE)
			continue;

		ready[i]->u_state = STATE_STARTING;
		ctx->ctx_running++;

		if (proc_spawn(ctx, ready[i]) != 0) {
			log_warn("failed to start %s", ready[i]->u_name);
			log_console("WARNING: failed to start %s",
			    ready[i]->u_name);
			ready[i]->u_state = STATE_FAILED;
			ctx->ctx_running--;
			depgraph_mark_done(&ctx->ctx_graph, ready[i]);
			/* Re-evaluate without recursion */
			goto retry;
		}

		if (ready[i]->u_type != UNIT_BARRIER)
			log_console("Starting %s.", ready[i]->u_name);

		/*
		 * Barriers and inline-exec oneshots complete
		 * synchronously (STATE_DONE).  Daemons under
		 * sub-reaper are immediately STATE_RUNNING.
		 * Both satisfy dependencies — mark done so
		 * dependants can be unblocked.
		 */
		if (ready[i]->u_state == STATE_DONE ||
		    ready[i]->u_state == STATE_RUNNING) {
			if (ready[i]->u_state == STATE_DONE)
				ctx->ctx_running--;
			depgraph_mark_done(&ctx->ctx_graph, ready[i]);

			/*
			 * When FILESYSTEMS is reached, /usr may now be
			 * mounted.  Re-scan localbase directories to
			 * pick up ports/packages units and legacy
			 * scripts, like rc(8) does with its two-phase
			 * rcorder approach.
			 */
			vec_foreach(ready[i]->u_provide, pi) {
				if (strcmp(ready[i]->u_provide.d[pi],
				    "FILESYSTEMS") == 0) {
					rescan_localbase(ctx);
					break;
				}
			}

			goto retry;
		}
	}

	free(ready);
	return (0);
}

/*
 * Handle a process exit event from kqueue.
 */
static void
handle_procdesc_event(struct rcd_ctx *ctx, struct kevent *kev)
{
	struct unit *u;
	int status;

	u = (struct unit *)kev->udata;
	status = (int)kev->data;

	ctx->ctx_running--;
	proc_handle_exit(ctx, u, status);

	/*
	 * Always resolve deps and schedule, not just during boot.
	 * Legacy oneshots (cleanvar, var_run) exit quickly; their
	 * procdesc events unblock dependants (syslogd, FILESYSTEMS).
	 */
	depgraph_mark_done(&ctx->ctx_graph, u);
	schedule_ready(ctx);
}

/*
 * Handle a socket activation event (incoming connection on pre-bound socket).
 */
static void
handle_sockact_event(struct rcd_ctx *ctx, struct kevent *kev)
{
	struct unit *u;

	u = (struct unit *)kev->udata;

	if (u->u_state == STATE_INACTIVE || u->u_state == STATE_DONE) {
		log_info("socket activation: starting %s", u->u_name);
		u->u_state = STATE_STARTING;
		ctx->ctx_running++;
		if (proc_spawn(ctx, u) != 0) {
			log_warn("socket activation failed for %s",
			    u->u_name);
			u->u_state = STATE_FAILED;
			ctx->ctx_running--;
		}
	}
}

/*
 * Handle a readiness notification (service has signalled it is ready).
 * For READY_SOCKET units, register deferred sockets in kqueue.
 * For READY_FD units, mark the service as running.
 */
static void
handle_readiness_event(struct rcd_ctx *ctx, struct kevent *kev)
{
	struct unit *u;

	u = (struct unit *)kev->udata;

	if (u->u_state != STATE_STARTING)
		return;

	switch (u->u_ready_method) {
	case READY_SOCKET:
		sockact_deferred_register_all(ctx, u);
		u->u_state = STATE_RUNNING;
		log_info("%s: socket activation ready", u->u_name);
		break;
	case READY_FD:
		u->u_state = STATE_RUNNING;
		log_info("%s: readiness signalled", u->u_name);
		break;
	default:
		break;
	}

	if (u->u_notify_fd >= 0) {
		close(u->u_notify_fd);
		u->u_notify_fd = -1;
	}
}

/*
 * Finalize the boot phase.  Called when boot_complete() returns true
 * or when the boot timeout fires.
 */
static void
finish_boot(struct rcd_ctx *ctx)
{
	struct unit *u;
	struct kevent kev;
	struct stat sb;

	ctx->ctx_booting = false;

	/* Cancel boot timeout if still pending */
	EV_SET(&kev, 0xB007, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
	kevent(ctx->ctx_kq, &kev, 1, NULL, 0, NULL);

	/* Log stuck services */
	TAILQ_FOREACH(u, &ctx->ctx_graph.dg_units, u_entries) {
		if (u->u_enabled && u->u_state == STATE_INACTIVE &&
		    !u->u_template)
			log_warn("boot: %s stuck (unmet dependencies)",
			    u->u_name);
	}

	/*
	 * After boot, verify the control socket is accessible.
	 * During early boot, /var/run (tmpfs) may not yet be mounted,
	 * or a tmpfs may have been mounted over the directory where
	 * the socket was originally created on the root filesystem,
	 * hiding the socket file.  Recreate the socket on the now-
	 * visible filesystem so rcctl(8) can reach it.
	 */
	if (ctx->ctx_ctlsock < 0) {
		/* Never succeeded during early init — retry now */
		log_info("creating control socket after boot");
		control_init(ctx);
	} else if (ctx->ctx_config.cfg_control_socket != NULL) {
		if (stat(ctx->ctx_config.cfg_control_socket, &sb) != 0 ||
		    !S_ISSOCK(sb.st_mode)) {
			log_info("control socket not accessible, "
			    "recreating after boot");
			control_reinit(ctx);
		}
	}

	if (ctx->ctx_config.cfg_quiet_boot) {
		log_init(ctx->ctx_config.cfg_log_level);
		log_console_set_enabled(true);
	}
	boottrace("rcd: boot complete");
	log_info("boot complete");
	log_console("Boot complete.");
	log_console_close();
	rcd_signal_ready(ctx);
	remove_firstboot(ctx->ctx_config.cfg_firstboot_sentinel);
}

/*
 * Handle a restart timer event.
 */
static void
handle_timer_event(struct rcd_ctx *ctx, struct kevent *kev)
{
	struct unit *u;

	/* Boot timeout timer */
	if (kev->ident == 0xB007) {
		if (ctx->ctx_booting) {
			log_warn("boot timeout, forcing completion");
			finish_boot(ctx);
		}
		return;
	}

	u = (struct unit *)kev->udata;

	log_info("restart timer: starting %s", u->u_name);
	u->u_state = STATE_STARTING;
	ctx->ctx_running++;
	if (proc_spawn(ctx, u) != 0) {
		log_warn("restart failed for %s", u->u_name);
		u->u_state = STATE_FAILED;
		ctx->ctx_running--;
	}
}

/*
 * Check if boot is complete.
 *
 * Boot is complete when no more progress can be made: either all
 * enabled services have reached a terminal state (running/done/failed),
 * or no services are currently starting AND no new services can be
 * scheduled (blocked by unsatisfied dependencies).
 */
static bool
boot_complete(struct rcd_ctx *ctx)
{
	struct unit *u;

	TAILQ_FOREACH(u, &ctx->ctx_graph.dg_units, u_entries) {
		if (!u->u_enabled)
			continue;
		if (u->u_template)
			continue;
		if (!TAILQ_EMPTY(&u->u_sockets))
			continue;
		/*
		 * Boot is not complete if any enabled service is still
		 * STARTING (shell running) or INACTIVE (waiting for deps).
		 * RUNNING, DONE, FAILED are all terminal for boot purposes.
		 */
		if (u->u_state == STATE_STARTING ||
		    u->u_state == STATE_INACTIVE)
			return (false);
	}



	return (true);
}

static int ready_efd = -1;

/*
 * Daemonize early: fork before starting services so the child
 * (which becomes the subreaper) owns all service processes.
 * The parent blocks on an eventfd until the child signals boot
 * complete, then exits to tell init(8) to proceed to multi_user.
 */
static void
rcd_daemonize(void)
{
	int efd;
	pid_t pid;

	efd = eventfd(0, EFD_CLOEXEC);
	if (efd < 0) {
		log_warn("eventfd: %s", strerror(errno));
		return;
	}

	pid = fork();
	if (pid < 0) {
		log_warn("fork: %s", strerror(errno));
		close(efd);
		return;
	}

	if (pid > 0) {
		/* Parent: block until child signals boot complete */
		uint64_t val;

		xread(efd, &val, sizeof(val));
		close(efd);
		_exit(0);
	}

	/* Child: becomes the supervisor daemon */
	ready_efd = efd;
	setsid();
	log_info("rcd: daemonized (pid %d)", getpid());
}

/*
 * Signal init that boot is complete.
 * Write to the eventfd; the parent reads it and exits.
 */
void
rcd_signal_ready(struct rcd_ctx *ctx __unused)
{
	uint64_t val;

	if (ready_efd < 0)
		return;
	val = 1;
	xwrite(ready_efd, &val, sizeof(val));
	close(ready_efd);
	ready_efd = -1;
}

/*
 * Perform ordered shutdown of all services with a global watchdog.
 * If shutdown takes longer than cfg_shutdown_timeout_ms, force-kill
 * all remaining services.
 */
static void
do_shutdown(struct rcd_ctx *ctx)
{
	struct unit **order;
	struct kevent kev;
	int norder, i, cap;

	cap = ctx->ctx_graph.dg_nunits;
	if (cap == 0)
		cap = 1;
	order = xcalloc(cap, sizeof(*order));
	if (order == NULL) {
		log_warn("shutdown: out of memory");
		return;
	}

	boottrace("rcd: shutdown started");
	log_info("shutting down services");
	ctx->ctx_shutting_down = true;

	/* Set a global shutdown watchdog timer */
	EV_SET(&kev, 0xDEAD, EVFILT_TIMER, EV_ADD | EV_ONESHOT,
	    NOTE_MSECONDS, ctx->ctx_config.cfg_shutdown_timeout_ms, NULL);
	if (kevent(ctx->ctx_kq, &kev, 1, NULL, 0, NULL) < 0)
		log_warn("kevent shutdown timer: %s", strerror(errno));

	depgraph_shutdown_order(&ctx->ctx_graph, order, &norder);

	for (i = 0; i < norder; i++) {
		if (order[i]->u_state != STATE_RUNNING)
			continue;
		log_info("stopping %s", order[i]->u_name);
		proc_stop_sync(ctx, order[i]);
	}

	/* Cancel the watchdog if we finished in time */
	EV_SET(&kev, 0xDEAD, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
	kevent(ctx->ctx_kq, &kev, 1, NULL, 0, NULL);

	free(order);
	boottrace("rcd: shutdown complete");
}

/*
 * Check if /firstboot sentinel exists (first boot provisioning).
 */
static bool
check_firstboot(const char *sentinel)
{

	if (sentinel == NULL)
		return (false);
	return (access(sentinel, F_OK) == 0);
}

/*
 * Remove the firstboot sentinel after boot completes.
 */
static void
remove_firstboot(const char *sentinel)
{

	if (sentinel != NULL)
		unlink(sentinel);
}

/*
 * Filter units based on keyword flags and runtime context.
 * Disables units that should not run in the current environment.
 */
static void
filter_units_by_context(struct rcd_ctx *ctx)
{
	struct unit *u;
	bool is_firstboot;

	is_firstboot = check_firstboot(
	    ctx->ctx_config.cfg_firstboot_sentinel);

	TAILQ_FOREACH(u, &ctx->ctx_graph.dg_units, u_entries) {
		/* Skip firstboot-only services if not first boot */
		if (u->u_boot_only && !is_firstboot) {
			u->u_enabled = false;
			continue;
		}

		/* Skip nojail services when running inside a jail */
		if (ctx->ctx_jailed && u->u_nojail) {
			u->u_enabled = false;
			continue;
		}

		/*
		 * nojailvnet: skip in jails without VNET.
		 * For simplicity, treat all jails as non-vnet unless
		 * we detect otherwise.
		 */
		if (ctx->ctx_jailed && u->u_nojailvnet) {
			u->u_enabled = false;
			continue;
		}
	}
}

/*
 * Save running state to a file for re-exec upgrade.
 * Records procdesc fds, PIDs, and service states so the new
 * binary can resume supervision without restarting services.
 */
static void
save_state(struct rcd_ctx *ctx)
{
	ucl_object_t *top, *arr, *sobj;
	struct unit *u;
	unsigned char *buf;
	FILE *fp;

	top = ucl_object_typed_new(UCL_OBJECT);

	arr = ucl_object_typed_new(UCL_ARRAY);
	TAILQ_FOREACH(u, &ctx->ctx_graph.dg_units, u_entries) {
		if (u->u_state != STATE_RUNNING)
			continue;
		if (u->u_procdesc_fd < 0)
			continue;

		sobj = ucl_object_typed_new(UCL_OBJECT);
		ucl_object_insert_key(sobj,
		    ucl_object_fromstring(u->u_name), "name", 0, false);
		ucl_object_insert_key(sobj,
		    ucl_object_fromint(u->u_pid), "pid", 0, false);
		ucl_object_insert_key(sobj,
		    ucl_object_fromint(u->u_procdesc_fd), "fd", 0, false);
		ucl_array_append(arr, sobj);

		/* Clear CLOEXEC so the fd survives exec */
		fcntl(u->u_procdesc_fd, F_SETFD, 0);
	}
	ucl_object_insert_key(top, arr, "services", 0, false);

	buf = ucl_object_emit(top, UCL_EMIT_JSON_COMPACT);
	ucl_object_unref(top);

	if (buf != NULL) {
		fp = fopen(RCD_STATE_FILE, "w");
		if (fp != NULL) {
			if (fprintf(fp, "%s\n", buf) < 0)
				log_warn("write %s: %s", RCD_STATE_FILE,
				    strerror(errno));
			if (fclose(fp) != 0)
				log_warn("close %s: %s", RCD_STATE_FILE,
				    strerror(errno));
		}
		free(buf);
	}
}

/*
 * Restore state from a previous rcd instance after re-exec.
 * Matches saved procdesc fds to loaded units and re-registers
 * them in kqueue for continued supervision.
 */
static void
restore_state(struct rcd_ctx *ctx)
{
	struct ucl_parser *parser;
	ucl_object_t *top;
	const ucl_object_t *arr, *sobj;
	ucl_object_iter_t it;

	parser = ucl_parser_new(UCL_PARSER_DEFAULT);
	if (!ucl_parser_add_file(parser, RCD_STATE_FILE)) {
		ucl_parser_free(parser);
		return;
	}
	top = ucl_parser_get_object(parser);
	ucl_parser_free(parser);
	if (top == NULL)
		return;

	arr = ucl_object_lookup(top, "services");
	if (arr == NULL) {
		ucl_object_unref(top);
		unlink(RCD_STATE_FILE);
		return;
	}

	it = ucl_object_iterate_new(arr);
	while ((sobj = ucl_object_iterate_safe(it, true)) != NULL) {
		const ucl_object_t *nobj, *pobj, *fobj;
		const char *name;
		pid_t pid;
		int fd;
		struct unit *u;
		struct kevent kev;

		nobj = ucl_object_lookup(sobj, "name");
		pobj = ucl_object_lookup(sobj, "pid");
		fobj = ucl_object_lookup(sobj, "fd");
		if (nobj == NULL || pobj == NULL || fobj == NULL)
			continue;

		name = ucl_object_tostring(nobj);
		pid = (pid_t)ucl_object_toint(pobj);
		fd = (int)ucl_object_toint(fobj);

		/* Verify the procdesc fd survived exec */
		if (fcntl(fd, F_GETFD) < 0) {
			log_warn("upgrade: %s fd %d invalid", name, fd);
			continue;
		}

		/* Match to loaded unit */
		u = depgraph_find(&ctx->ctx_graph, name);
		if (u == NULL) {
			log_warn("upgrade: %s no longer exists", name);
			close(fd);
			continue;
		}

		u->u_pid = pid;
		u->u_procdesc_fd = fd;
		u->u_state = STATE_RUNNING;

		/* Re-set CLOEXEC for normal operation */
		fcntl(fd, F_SETFD, FD_CLOEXEC);

		EV_SET(&kev, fd, EVFILT_PROCDESC, EV_ADD | EV_ONESHOT,
		    NOTE_EXIT, 0, u);
		if (kevent(ctx->ctx_kq, &kev, 1, NULL, 0, NULL) < 0)
			log_warn("upgrade: kevent for %s: %s",
			    name, strerror(errno));
		else
			log_info("upgrade: restored %s (pid %d fd %d)",
			    name, pid, fd);
	}
	ucl_object_iterate_free(it);

	ucl_object_unref(top);
	unlink(RCD_STATE_FILE);
}

/*
 * Perform a safe upgrade by re-execing ourselves.
 * The running services are preserved via their procdesc fds.
 */
static void
do_upgrade(struct rcd_ctx *ctx, char *argv0)
{

	log_info("upgrade: saving state and re-execing %s", argv0);
	save_state(ctx);

	/* Re-exec ourselves — no extra args, just the binary */
	execl(argv0, argv0, (char *)NULL);

	/* If exec failed, clean up and continue */
	log_warn("upgrade: execl failed: %s", strerror(errno));
	unlink(RCD_STATE_FILE);
}

jmp_buf rcd_oom_env;
static char *rcd_argv0;

/*
 * Main kqueue event loop.
 */
int
rcd_main_loop(struct rcd_ctx *ctx)
{
	struct kevent events[64];
	struct kevent sigkev[3];
	int nevents, i;

	/* Register signal handlers via kqueue */
	EV_SET(&sigkev[0], SIGTERM, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
	EV_SET(&sigkev[1], SIGINT, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
	EV_SET(&sigkev[2], SIGHUP, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
	{
		struct kevent extrakev[2];
		EV_SET(&extrakev[0], SIGALRM, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
		EV_SET(&extrakev[1], SIGUSR1, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
		if (kevent(ctx->ctx_kq, extrakev, 2, NULL, 0, NULL) < 0)
			log_warn("kevent signal registration: %s",
			    strerror(errno));
	}
	if (kevent(ctx->ctx_kq, sigkev, 3, NULL, 0, NULL) < 0)
		log_warn("kevent signal registration: %s",
		    strerror(errno));

	signal(SIGTERM, SIG_IGN);
	signal(SIGINT, SIG_IGN);
	signal(SIGALRM, SIG_IGN);
	signal(SIGHUP, SIG_IGN);
	signal(SIGCHLD, SIG_IGN);
	signal(SIGUSR1, SIG_IGN);
	signal(SIGPIPE, SIG_IGN);

	/*
	 * Reap zombies left over from the boot phase.  Services and
	 * hooks spawned by schedule_ready() may have created orphaned
	 * descendants that exited before SA_NOCLDWAIT was active.
	 * SA_NOCLDWAIT does not retroactively reap existing zombies.
	 */
	while (waitpid(-1, NULL, WNOHANG) > 0)
		;

	for (;;) {
		if (setjmp(rcd_oom_env) != 0) {
			log_warn("OOM: restarting event loop");
			/*
			 * After longjmp from an allocation failure, some
			 * per-event heap state may be leaked, but the
			 * reaper and running services are unaffected.
			 */
		}

		nevents = kevent(ctx->ctx_kq, NULL, 0, events, 64, NULL);
		if (nevents < 0) {
			if (errno == EINTR)
				continue;
			log_err(1, "kevent");
		}

		for (i = 0; i < nevents; i++) {
			struct kevent *kev = &events[i];

			switch (kev->filter) {
			case EVFILT_PROCDESC:
				handle_procdesc_event(ctx, kev);
				break;
			case EVFILT_READ:
				if ((int)kev->ident == ctx->ctx_ctlsock)
					control_handle(ctx, (int)kev->ident);
				else if (kev->udata != NULL) {
					struct unit *u = kev->udata;

					/* Readiness notification (eventfd) */
					if ((int)kev->ident == u->u_notify_fd)
						handle_readiness_event(ctx, kev);
					/* Syslog pipe event */
					else if ((int)kev->ident == u->u_log.lc_stdout_pipefd ||
					    (int)kev->ident == u->u_log.lc_stderr_pipefd)
						log_handle_pipe_event(u, (int)kev->ident);
					else
						handle_sockact_event(ctx, kev);
				}
				break;
			case EVFILT_TIMER:
				handle_timer_event(ctx, kev);
				break;
			case EVFILT_VNODE:
				/* Control socket deleted — recreate it */
				control_reinit(ctx);
				break;
			case EVFILT_SIGNAL:
				if (kev->ident == SIGTERM ||
				    kev->ident == SIGINT) {
					if (ctx->ctx_config.cfg_precious_machine) {
						log_warn("shutdown refused: "
						    "precious_machine is set");
						break;
					}
					do_shutdown(ctx);
					return (0);
				}
				if (kev->ident == SIGHUP) {
					do_reload_config(ctx);
				}
				if (kev->ident == SIGUSR1) {
					do_upgrade(ctx, rcd_argv0);
					/* If exec failed, we're still here */
				}
				/*
				 * SIGALRM: re-read override files during
				 * boot.  Allows enable/disable of services
				 * while boot is in progress (like rc's
				 * SIGALRM rc.conf reload mechanism).
				 */
				if (kev->ident == SIGALRM &&
				    ctx->ctx_booting) {
					struct unit *su;

					log_info("SIGALRM: reloading "
					    "overrides during boot");
					TAILQ_FOREACH(su,
					    &ctx->ctx_graph.dg_units,
					    u_entries)
						unit_apply_overrides(su,
						    "/etc/rcd.conf.d");
				}
				break;
			}
		}

		/* Check if boot phase is complete */
		if (ctx->ctx_booting && boot_complete(ctx))
			finish_boot(ctx);
	}
}

/*
 * Load all unit files and legacy scripts from system and localbase dirs.
 */
static void
load_all_units(struct rcd_ctx *ctx)
{
	char local_unit_dir[PATH_MAX];
	char local_legacy_dir[PATH_MAX];

	snprintf(local_unit_dir, sizeof(local_unit_dir),
	    "%s/etc/rcd.d", localbase);
	snprintf(local_legacy_dir, sizeof(local_legacy_dir),
	    "%s/etc/rc.d", localbase);

	load_units_from_dir(ctx, RCD_UNIT_DIR);
	load_units_from_dir(ctx, local_unit_dir);

	compat_load_rcvars(&ctx->ctx_config);
	load_legacy_from_dir(ctx, RCD_LEGACY_DIR);
	load_legacy_from_dir(ctx, local_legacy_dir);
}

/*
 * Filter units, apply overrides, resolve the DAG, and mark disabled
 * units as done so their dependants are unblocked.
 */
static void
prepare_graph(struct rcd_ctx *ctx)
{
	struct unit *u;
	bool progress;

	filter_units_by_context(ctx);

	if (ctx->ctx_config.cfg_quiet_boot) {
		log_init(LOG_WARNING);
		log_console_set_enabled(false);
	}

	TAILQ_FOREACH(u, &ctx->ctx_graph.dg_units, u_entries)
		unit_apply_overrides(u, "/etc/rcd.conf.d");

	if (depgraph_resolve(&ctx->ctx_graph) != 0)
		log_warn("dependency resolution had errors");
	if (depgraph_check_cycles(&ctx->ctx_graph) != 0)
		log_warn("dependency cycles detected");

	do {
		progress = false;
		TAILQ_FOREACH(u, &ctx->ctx_graph.dg_units, u_entries) {
			if (u->u_state != STATE_INACTIVE)
				continue;
			if (!u->u_enabled || u->u_nostart) {
				u->u_state = STATE_DONE;
				depgraph_mark_done(&ctx->ctx_graph, u);
				progress = true;
			}
		}
	} while (progress);
}

/*
 * Instantiate templates from /etc/rcd.conf.d/<template> and apply
 * per-instance overrides.
 */
static void
instantiate_templates(struct rcd_ctx *ctx)
{
	struct unit *u;

	TAILQ_FOREACH(u, &ctx->ctx_graph.dg_units, u_entries) {
		struct ucl_parser *ip;
		ucl_object_t *itop;
		const ucl_object_t *inst_obj, *cur;
		ucl_object_iter_t iit;
		char ipath[PATH_MAX];

		if (!u->u_template)
			continue;

		if (snprintf(ipath, sizeof(ipath),
		    "/etc/rcd.conf.d/%s", u->u_name) >= (int)sizeof(ipath))
			continue;
		ip = ucl_parser_new(UCL_PARSER_DEFAULT);
		if (!ucl_parser_add_file(ip, ipath)) {
			ucl_parser_free(ip);
			continue;
		}
		itop = ucl_parser_get_object(ip);
		ucl_parser_free(ip);
		if (itop == NULL)
			continue;

		inst_obj = ucl_object_lookup(itop, "instances");
		if (inst_obj == NULL) {
			ucl_object_unref(itop);
			continue;
		}

		iit = ucl_object_iterate_new(inst_obj);
		while ((cur = ucl_object_iterate_safe(iit, true)) != NULL) {
			const char *iname;
			char fullname[256];
			struct unit *inst;

			if (ucl_object_type(cur) == UCL_STRING)
				iname = ucl_object_tostring(cur);
			else
				iname = ucl_object_key(cur);
			if (iname == NULL)
				continue;

			if (snprintf(fullname, sizeof(fullname),
			    "%s@%s", u->u_name, iname) >= (int)sizeof(fullname))
				continue;
			if (depgraph_find(&ctx->ctx_graph, fullname) != NULL)
				continue;

			inst = unit_instantiate(u, iname);
			if (inst == NULL)
				continue;

			if (ucl_object_type(cur) == UCL_OBJECT) {
				unsigned char *s;

				s = ucl_object_emit(cur, UCL_EMIT_CONFIG);
				if (s != NULL)
					inst->u_instance_conf = (char *)s;
			}
			depgraph_add(&ctx->ctx_graph, inst);
		}
		ucl_object_iterate_free(iit);
		ucl_object_unref(itop);
	}

	/* Apply overrides to instances created above */
	TAILQ_FOREACH(u, &ctx->ctx_graph.dg_units, u_entries) {
		if (u->u_instance != NULL)
			unit_apply_overrides(u, "/etc/rcd.conf.d");
	}
}

/*
 * Run off_command for disabled services.
 */
static void
run_off_commands(struct rcd_ctx *ctx)
{
	struct unit *u;

	TAILQ_FOREACH(u, &ctx->ctx_graph.dg_units, u_entries) {
		if (!u->u_enabled && u->u_off_command != NULL) {
			log_info("%s: running off_command", u->u_name);
			proc_run_hook_inst(u->u_off_command, u);
		}
	}
}

/*
 * Bind sockets for socket-activated services and register them
 * in kqueue.
 */
static void
bind_sockets(struct rcd_ctx *ctx)
{
	struct unit *u;
	struct unit_socket *us;

	TAILQ_FOREACH(u, &ctx->ctx_graph.dg_units, u_entries) {
		TAILQ_FOREACH(us, &u->u_sockets, us_entries) {
			if (sockact_bind(us) != 0)
				log_warn("socket bind failed for %s",
				    u->u_name);
		}
		if (TAILQ_EMPTY(&u->u_sockets))
			continue;
		if (u->u_ready_method == READY_SOCKET)
			sockact_register_deferred(u);
		else
			sockact_register(ctx, u);
	}
}

int
main(int argc, char *argv[])
{
	struct rcd_ctx ctx;
	int ch;

	bool upgrading;

	memset(&ctx, 0, sizeof(ctx));
	ctx.ctx_booting = true;
	ctx.ctx_ctlsock = -1;
	ctx.ctx_ctlsock_pathfd = -1;
	depgraph_init(&ctx.ctx_graph);
	rcd_argv0 = argv[0];

	upgrading = check_upgrade_state();

	while ((ch = getopt(argc, argv, "v")) != -1) {
		switch (ch) {
		case 'v':
			log_set_verbose(true);
			break;
		default:
			fprintf(stderr, "usage: rcd [-v]\n");
			return (1);
		}
	}

	log_init(LOG_INFO);

	lua_init();
	get_localbase();

	ctx.ctx_jailed = check_jailed();
	ctx.ctx_diskless = check_diskless();

	if (ctx.ctx_jailed)
		log_info("rcd starting (jailed)");
	else if (ctx.ctx_diskless)
		log_info("rcd starting (diskless)");
	else
		log_info("rcd starting");

	if (ctx.ctx_diskless && access("/etc/rc.initdiskless", X_OK) == 0) {
		log_info("running rc.initdiskless");
		proc_run_hook(_PATH_BSHELL " /etc/rc.initdiskless");
	}

	/*
	 * Fork early so the child (which will be the supervisor) is
	 * the one that acquires the reaper role and owns all service
	 * processes.  The parent waits on a pipe until boot completes.
	 *
	 * Save a copy of stderr (connected to the console by init)
	 * before daemonization so the child can write boot progress
	 * messages to the console.
	 */
	log_console_open();
	if (!upgrading)
		rcd_daemonize();

	/*
	 * Become the subreaper for all service processes.
	 * This is now in the child process, so all services spawned
	 * below will be under our reaper subtree.
	 */
	if (proc_reaper_init() != 0 && !upgrading)
		log_err(1, "proc_reaper_init");

	/*
	 * Ignore SIGCHLD so terminated children are auto-reaped.
	 * Without this, orphaned descendants (e.g., background
	 * processes forked by hook commands or legacy scripts)
	 * become zombies until the main event loop starts.
	 */
	signal(SIGCHLD, SIG_IGN);

	ctx.ctx_kq = kqueue();
	if (ctx.ctx_kq < 0)
		log_err(1, "kqueue");

	if (config_load(&ctx.ctx_config) != 0)
		log_err(1, "config_load");

	ctx.ctx_config.cfg_unit_schema = load_embedded_schema(
	    unit_schema_json, sizeof(unit_schema_json));
	if (ctx.ctx_config.cfg_unit_schema == NULL)
		log_warn("failed to load unit schema");

	load_all_units(&ctx);
	prepare_graph(&ctx);
	instantiate_templates(&ctx);
	run_off_commands(&ctx);

	boottrace("rcd: boot started");
	bind_sockets(&ctx);

	if (upgrading) {
		/*
		 * Re-exec upgrade: restore running services from the
		 * state file.  Skip normal boot — services are already
		 * running.  Create a fresh control socket (the old one
		 * was closed by exec).
		 */
		restore_state(&ctx);
		ctx.ctx_booting = false;
		if (control_init(&ctx) != 0)
			log_warn("control socket init failed");
		log_info("upgrade: state restored, entering supervision");
	} else {
		struct kevent kev;
		/* Initialize control socket */
		if (control_init(&ctx) != 0)
			log_warn("control socket init failed");

		/* Start the parallel boot sequence */
		schedule_ready(&ctx);

		/*
		 * Set a boot timeout timer.  If boot hasn't completed
		 * when it fires, declare it complete anyway.
		 */
		EV_SET(&kev, 0xB007, EVFILT_TIMER,
		    EV_ADD | EV_ONESHOT, NOTE_SECONDS, 30, NULL);
		kevent(ctx.ctx_kq, &kev, 1, NULL, 0, NULL);
	}

	/* Enter supervision loop */
	return (rcd_main_loop(&ctx));
}
