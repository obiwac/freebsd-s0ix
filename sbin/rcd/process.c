/*
 * Copyright (c) 2026 Baptiste Daroussin <bapt@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Process lifecycle management.
 *
 * Uses posix_spawn(3) with posix_spawnattr_setprocdescp_np(3) to launch
 * services, obtaining a process descriptor fd without manual fork+exec.
 * Pre-exec setup (credentials, jail, rlimits) is delegated to rcd-exec(8).
 *
 * rcd becomes a subreaper via procctl(PROC_REAP_ACQUIRE).  Service subtrees
 * are killed via procctl(PROC_REAP_KILL).  Process deaths are detected via
 * kqueue EVFILT_PROCDESC on the process descriptor fd.
 */

#include <sys/param.h>
#include <sys/event.h>
#include <sys/eventfd.h>
#include <sys/linker.h>
#include <sys/module.h>
#include <sys/procdesc.h>
#include <sys/procctl.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/wait.h>

#include <errno.h>
#include <paths.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "rcd.h"

extern char **environ;

#define RCD_EXEC_PATH	"/sbin/rcd-exec"
#define LISTEN_FD_START	3

/*
 * Become a subreaper so all orphaned descendants are reparented to us.
 */
int
proc_reaper_init(void)
{
	int error;

	error = procctl(P_PID, getpid(), PROC_REAP_ACQUIRE, NULL);
	if (error != 0) {
		log_warn("PROC_REAP_ACQUIRE: %s", strerror(errno));
		return (-1);
	}
	return (0);
}

/*
 * Check preconditions before starting a service.
 * Returns 0 if all preconditions are met, -1 otherwise.
 */
int
proc_check_preconditions(struct unit *u)
{
	struct stat sb;
	struct kv *sc;

	vec_foreach(u->u_required_dirs, i) {
		if (stat(u->u_required_dirs.d[i], &sb) != 0 ||
		    !S_ISDIR(sb.st_mode)) {
			log_warn("%s: required directory missing: %s",
			    u->u_name,
			    u->u_required_dirs.d[i]);
			return (-1);
		}
	}

	vec_foreach(u->u_required_files, i) {
		if (access(u->u_required_files.d[i],
		    R_OK) != 0) {
			log_warn("%s: required file missing: %s",
			    u->u_name,
			    u->u_required_files.d[i]);
			return (-1);
		}
	}

	/*
	 * Check required_vars: verify that named keys exist in
	 * the override config.  This ensures the admin has set
	 * mandatory per-site configuration before the service starts.
	 */
	if (u->u_required_vars.len > 0 && u->u_override_conf != NULL) {
		struct ucl_parser *vp;
		ucl_object_t *vtop;

		vp = ucl_parser_new(UCL_PARSER_DEFAULT);
		if (ucl_parser_add_string(vp, u->u_override_conf,
		    strlen(u->u_override_conf))) {
			vtop = ucl_parser_get_object(vp);
			vec_foreach(u->u_required_vars, vi) {
				if (ucl_object_lookup(vtop,
				    u->u_required_vars.d[vi]) == NULL) {
					log_warn("%s: required variable "
					    "not set: %s", u->u_name,
					    u->u_required_vars.d[vi]);
					ucl_object_unref(vtop);
					ucl_parser_free(vp);
					return (-1);
				}
			}
			ucl_object_unref(vtop);
		}
		ucl_parser_free(vp);
	} else if (u->u_required_vars.len > 0) {
		log_warn("%s: required_vars set but no override config",
		    u->u_name);
		return (-1);
	}

	/* Check required sysctl values */
	STAILQ_FOREACH(sc, &u->u_required_sysctl, kv_entries) {
		char buf[256];
		size_t len;

		len = sizeof(buf);
		if (sysctlbyname(sc->kv_key, buf, &len, NULL, 0) != 0) {
			log_warn("%s: required sysctl %s: %s",
			    u->u_name, sc->kv_key, strerror(errno));
			return (-1);
		}
		/* Null-terminate if it's a string */
		if (len < sizeof(buf))
			buf[len] = '\0';

		if (strcmp(buf, sc->kv_val) != 0) {
			log_warn("%s: sysctl %s = \"%s\", want \"%s\"",
			    u->u_name, sc->kv_key, buf, sc->kv_val);
			return (-1);
		}
	}

	return (0);
}

/*
 * Load required kernel modules via kldload(2).
 * Returns 0 on success, -1 if a module could not be loaded.
 */
int
proc_load_modules(struct unit *u)
{

	vec_foreach(u->u_required_modules, i) {
		/* Check if already loaded */
		if (modfind(u->u_required_modules.d[i]) != -1)
			continue;
		if (kldload(u->u_required_modules.d[i]) < 0 &&
		    errno != EEXIST) {
			log_warn("%s: kldload(%s): %s", u->u_name,
			    u->u_required_modules.d[i],
			    strerror(errno));
			return (-1);
		}
		log_info("%s: loaded module %s", u->u_name,
		    u->u_required_modules.d[i]);
	}
	return (0);
}

/*
 * Run a hook command (precmd/postcmd) synchronously.
 *
 * If the command starts with "lua:", the remainder is evaluated as Lua
 * code in rcd's embedded interpreter.  Otherwise, the command is run
 * via posix_spawn("/bin/sh", "-c", cmd).
 *
 * Returns 0 on success, -1 on failure.
 */
int
proc_run_hook_inst(const char *cmd, const struct unit *u)
{
	posix_spawnattr_t attr;
	pid_t pid;
	int status, error;
	char *argv[4];
	const char *instance;

	if (cmd == NULL || cmd[0] == '\0')
		return (0);

	instance = (u != NULL) ? u->u_instance : NULL;

	/* Dispatch to the embedded Lua interpreter */
	if (IS_LUA_HOOK(cmd))
		return (lua_exec(cmd + strlen(LUA_HOOK_PREFIX), "hook", u));

	/* Set RCD_INSTANCE for shell hooks (if template instance) */
	if (instance != NULL)
		setenv("RCD_INSTANCE", instance, 1);

	/* Shell hook */
	argv[0] = __DECONST(char *, _PATH_BSHELL);
	argv[1] = __DECONST(char *, "-c");
	argv[2] = __DECONST(char *, cmd);
	argv[3] = NULL;

	posix_spawnattr_init(&attr);
	error = posix_spawn(&pid, _PATH_BSHELL, NULL, &attr, argv, environ);
	posix_spawnattr_destroy(&attr);

	if (instance != NULL)
		unsetenv("RCD_INSTANCE");

	if (error != 0) {
		log_warn("hook posix_spawn: %s", strerror(error));
		return (-1);
	}

	if (xwaitpid(pid, &status, 0) < 0)
		return (-1);

	return (WIFEXITED(status) ? WEXITSTATUS(status) : -1);
}

int
proc_run_hook(const char *cmd)
{

	return (proc_run_hook_inst(cmd, NULL));
}

/*
 * Free a NULL-terminated string array (environment from build_environ).
 */
static void
free_argv(char **av)
{

	if (av == NULL)
		return;
	for (int i = 0; av[i] != NULL; i++)
		free(av[i]);
	free(av);
}

/*
 * Tokenize a string into a charv_t, splitting on whitespace.
 * Respects single and double quotes.  Inside double quotes,
 * backslash escapes are recognized: \" → ", \\ → \.
 * Single quotes are fully literal (no escapes).
 * The caller owns the resulting strings and must free them
 * with vec_free_and_free.
 */
void
tokenize(const char *str, charv_t *out)
{
	char *buf, *p, *start;
	char quote;

	buf = xstrdup(str);
	p = buf;
	while (*p != '\0') {
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '\0')
			break;

		if (*p == '\'' || *p == '"') {
			quote = *p++;
			start = p;

			if (quote == '"') {
				/*
				 * Double quotes: handle backslash escapes.
				 * Use separate read (r) and write (w) pointers
				 * to compact out escape backslashes in place.
				 */
				char *r, *w;

				r = p;
				w = p;
				while (*r != '\0' && *r != '"') {
					if (*r == '\\' &&
					    (*(r + 1) == '"' ||
					    *(r + 1) == '\\'))
						r++;	/* skip backslash */
					*w++ = *r++;
				}
				*w = '\0';
				p = r;
				if (*p == '"')
					p++;
			} else {
				/* Single quotes: fully literal, no escapes */
				while (*p != '\0' && *p != '\'')
					p++;
				if (*p != '\0')
					p++;
			}

			vec_push(out, xstrdup(start));
		} else {
			start = p;
			while (*p != '\0' && *p != ' ' && *p != '\t')
				p++;

			if (*p != '\0')
				*p++ = '\0';

			vec_push(out, xstrdup(start));
		}
	}

	free(buf);
}

/*
 * Build the environment for rcd-exec.  This includes:
 * - Unit's declared environment variables
 * - RCD_* control variables for rcd-exec's pre-exec setup
 * - LISTEN_FDS / LISTEN_FDNAMES for socket activation
 */
/*
 * Push a formatted environment variable into the env vec.
 */
static void __printflike(2, 3)
env_push(charv_t *ev, const char *fmt, ...)
{
	va_list ap;
	char *s;

	va_start(ap, fmt);
	if (vasprintf(&s, fmt, ap) < 0)
		abort();
	va_end(ap);
	vec_push(ev, s);
}

static char **
build_environ(struct unit *u, int notify_fd, int listen_fds)
{
	struct kv *ue;
	charv_t ev = vec_init();

	/* Unit environment */
	STAILQ_FOREACH(ue, &u->u_env, kv_entries)
		env_push(&ev, "%s=%s", ue->kv_key, ue->kv_val);

	/* Credentials for rcd-exec */
	if (u->u_proc.pc_user != NULL)
		env_push(&ev, "RCD_USER=%s", u->u_proc.pc_user);
	if (u->u_proc.pc_group != NULL)
		env_push(&ev, "RCD_GROUP=%s", u->u_proc.pc_group);
	if (u->u_proc.pc_chdir != NULL)
		env_push(&ev, "RCD_CHDIR=%s", u->u_proc.pc_chdir);

	env_push(&ev, "RCD_UMASK=%o", u->u_proc.pc_umask);

	if (u->u_proc.pc_nice != 0)
		env_push(&ev, "RCD_NICE=%d", u->u_proc.pc_nice);
	if (u->u_proc.pc_cpuset != NULL)
		env_push(&ev, "RCD_CPUSET=%s", u->u_proc.pc_cpuset);
	if (u->u_proc.pc_fib != 0)
		env_push(&ev, "RCD_FIB=%d", u->u_proc.pc_fib);
	if (u->u_proc.pc_chroot != NULL)
		env_push(&ev, "RCD_CHROOT=%s", u->u_proc.pc_chroot);
	if (u->u_proc.pc_login_class != NULL)
		env_push(&ev, "RCD_LOGIN_CLASS=%s", u->u_proc.pc_login_class);
	if (u->u_proc.pc_limits != NULL)
		env_push(&ev, "RCD_LIMITS=%s", u->u_proc.pc_limits);
	if (u->u_proc.pc_env_file != NULL)
		env_push(&ev, "RCD_ENV_FILE=%s", u->u_proc.pc_env_file);

	/* Template instance name */
	if (u->u_instance != NULL)
		env_push(&ev, "RCD_INSTANCE=%s", u->u_instance);

	/* Supplementary groups as comma-separated list */
	if (u->u_proc.pc_groups.len > 0) {
		char *gbuf;
		size_t gbuf_sz;
		int off;

		gbuf_sz = 0;
		vec_foreach(u->u_proc.pc_groups, gi)
			gbuf_sz += strlen(u->u_proc.pc_groups.d[gi]) + 1;
		gbuf = xmalloc(gbuf_sz + 1);
		off = 0;
		vec_foreach(u->u_proc.pc_groups, gi) {
			if (gi > 0)
				gbuf[off++] = ',';
			off += snprintf(gbuf + off, gbuf_sz + 1 - off,
			    "%s", u->u_proc.pc_groups.d[gi]);
		}
		env_push(&ev, "RCD_GROUPS=%s", gbuf);
		free(gbuf);
	}

	/* Jail name for rcd-exec to attach to */
	if (u->u_jail.jc_enable && u->u_jail.jc_jid > 0)
		env_push(&ev, "RCD_JAIL=%s",
		    u->u_jail.jc_name != NULL ? u->u_jail.jc_name : "");

	/* Readiness notification fd */
	if (notify_fd >= 0)
		env_push(&ev, "RCD_NOTIFY_FD=%d", notify_fd);

	/* Socket activation */
	if (listen_fds > 0) {
		env_push(&ev, "LISTEN_FDS=%d", listen_fds);
		env_push(&ev, "LISTEN_PID=0");
	}

	/* Sub-reaper mode for forking daemons */
	if (u->u_type == UNIT_FORKING || u->u_type == UNIT_LEGACY_FORKING) {
		env_push(&ev, "RCD_REAPER=1");
		env_push(&ev, "RCD_SERVICE=%s", u->u_name);
	}

	/* NULL-terminate for execve */
	vec_push(&ev, NULL);
	return (ev.d);
}

/*
 * Spawn a service using posix_spawn(3).
 */
int
proc_spawn(struct rcd_ctx *ctx, struct unit *u)
{
	posix_spawnattr_t attr;
	posix_spawn_file_actions_t fa;
	sigset_t sigmask, sigdefault;
	c_charv_t argv = vec_init();	/* Borrowed pointers, not owned */
	charv_t argtoks = vec_init();	/* Owned: tokenized command_args */
	char **envp;
	pid_t pid;
	int procdesc_fd;
	int notify_fd, notify_efd;
	int listen_fds;
	int error;

	procdesc_fd = -1;
	notify_efd = -1;
	listen_fds = 0;

	/* Barriers are pure synchronisation points — no process to run */
	if (u->u_type == UNIT_BARRIER) {
		boottrace("rcd: barrier %s", u->u_name);
		log_info("barrier %s reached", u->u_name);
		u->u_state = STATE_DONE;
		return (0);
	}

	/* Check preconditions */
	if (proc_check_preconditions(u) != 0)
		return (-1);

	/* Load required kernel modules */
	if (proc_load_modules(u) != 0)
		return (-1);

	/* Start delay — sleep before launching */
	if (u->u_start_delay_ms > 0) {
		struct timespec ts;

		ts.tv_sec = u->u_start_delay_ms / 1000;
		ts.tv_nsec = (u->u_start_delay_ms % 1000) * 1000000L;
		log_info("%s: delaying start by %u ms", u->u_name,
		    u->u_start_delay_ms);
		while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
			;
	}

	/* Run setup_cmd hook (before precmd, like rc's _setup) */
	if (u->u_setup_cmd != NULL) {
		if (proc_run_hook_inst(u->u_setup_cmd, u) != 0) {
			log_warn("%s: setup_cmd failed", u->u_name);
			return (-1);
		}
	}

	/* Run start_precmd hook */
	if (u->u_start_precmd != NULL) {
		if (proc_run_hook_inst(u->u_start_precmd, u) != 0) {
			log_warn("%s: start_precmd failed", u->u_name);
			return (-1);
		}
	}

	/*
	 * Inline exec for oneshots: if the unit has an "exec" field,
	 * run it directly (lua: or shell) instead of spawning a process.
	 * This avoids needing a command binary for script-driven oneshots.
	 */
	if (u->u_exec != NULL) {
		int rc;

		boottrace("rcd: exec %s", u->u_name);
		rc = proc_run_hook_inst(u->u_exec, u);
		if (u->u_start_postcmd != NULL)
			proc_run_hook_inst(u->u_start_postcmd, u);
		u->u_state = (rc == 0) ? STATE_DONE : STATE_FAILED;
		log_info("%s: exec %s", u->u_name,
		    rc == 0 ? "succeeded" : "failed");
		return (rc);
	}

	/* Create jail if needed (before spawn, so rcd-exec can attach) */
	if (u->u_jail.jc_enable) {
		if (jail_svc_create(u) != 0) {
			log_warn("%s: jail creation failed", u->u_name);
			return (-1);
		}
	}

	/* Readiness notification fd (for READY_FD and READY_SOCKET) */
	notify_fd = -1;
	if (u->u_ready_method == READY_FD ||
	    u->u_ready_method == READY_SOCKET) {
		notify_efd = eventfd(0, EFD_NONBLOCK);
		if (notify_efd < 0) {
			log_warn("%s: eventfd: %s", u->u_name,
			    strerror(errno));
			return (-1);
		}
		notify_fd = notify_efd;
	}

	/*
	 * Build argv.  All types go through rcd-exec now:
	 *
	 * simple:  rcd-exec command [args...]
	 * forking: rcd-exec command [args...]  (with RCD_REAPER)
	 * legacy:  rcd-exec /bin/sh script start (with RCD_REAPER)
	 *
	 * The reaper mode makes rcd-exec become a sub-reaper that
	 * tracks the daemon after the initial process (shell or
	 * forking parent) exits.
	 */
	vec_push(&argv, RCD_EXEC_PATH);
	if (u->u_type == UNIT_LEGACY || u->u_type == UNIT_LEGACY_FORKING) {
		vec_push(&argv, _PATH_BSHELL);
		vec_push(&argv, u->u_path);
		vec_push(&argv, "start");
	} else {
		if (u->u_command_prepend != NULL) {
			tokenize(u->u_command_prepend, &argtoks);
			vec_foreach(argtoks, pi)
				vec_push(&argv, argtoks.d[pi]);
		}
		vec_push(&argv, u->u_command);
		if (u->u_command_args != NULL) {
			tokenize(u->u_command_args, &argtoks);
			vec_foreach(argtoks, ti)
				vec_push(&argv, argtoks.d[ti]);
		}
		if (u->u_instance != NULL)
			vec_push(&argv, u->u_instance);
	}
	vec_push(&argv, NULL);

	/* Set up file actions first — sockact_setup_fds sets listen_fds */
	posix_spawn_file_actions_init(&fa);
	log_setup_fds(u, &fa);
	sockact_setup_fds(u, &fa, &listen_fds);
	if (u->u_proc.pc_chdir != NULL)
		posix_spawn_file_actions_addchdir_np(&fa, u->u_proc.pc_chdir);

	/*
	 * Pass notification fd to child if needed.
	 * Place it right after the last listen socket fd so that
	 * closefrom doesn't close it.  The child (rcd-exec) will
	 * write to it after pre-exec setup to signal readiness.
	 */
	if (notify_efd >= 0) {
		int notify_target = LISTEN_FD_START + listen_fds;

		posix_spawn_file_actions_adddup2(&fa, notify_efd,
		    notify_target);
		posix_spawn_file_actions_addclose(&fa, notify_efd);
		notify_fd = notify_target;
		listen_fds++;	/* keep closefrom past the notify fd */
	}

	/*
	 * Close inherited fds (procdesc fds from other services,
	 * kqueue fd, control socket, etc.).  The dup2 actions above
	 * place needed fds at low numbers (0-2 for stdio, 3+ for
	 * sockets + notification fd).  Close everything from
	 * LISTEN_FD_START + listen_fds.
	 */
	posix_spawn_file_actions_addclosefrom_np(&fa,
	    LISTEN_FD_START + listen_fds);

	/* Now build environment with the correct listen_fds count */
	envp = build_environ(u, notify_fd, listen_fds);
	if (envp == NULL) {
		posix_spawn_file_actions_destroy(&fa);
		vec_free(&argv);
		vec_free_and_free(&argtoks, free);
		if (notify_efd >= 0) {
			close(notify_efd);
			notify_efd = -1;
		}
		return (-1);
	}

	/* Initialize posix_spawn attributes */
	posix_spawnattr_init(&attr);
	posix_spawnattr_setprocdescp_np(&attr, &procdesc_fd, PD_CLOEXEC);

	sigemptyset(&sigmask);
	posix_spawnattr_setsigmask(&attr, &sigmask);
	sigfillset(&sigdefault);
	posix_spawnattr_setsigdefault(&attr, &sigdefault);
	posix_spawnattr_setflags(&attr,
	    POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

	/* Spawn — all types go through rcd-exec now */
	error = posix_spawn(&pid, RCD_EXEC_PATH, &fa, &attr,
	    __DECONST(char **, argv.d), envp);

	posix_spawn_file_actions_destroy(&fa);
	posix_spawnattr_destroy(&attr);
	vec_free(&argv);
	vec_free_and_free(&argtoks, free);
	free_argv(envp);

	/* Close syslog pipe write-ends now that the child inherited them */
	if (u->u_log.lc_stdout_wfd >= 0) {
		close(u->u_log.lc_stdout_wfd);
		u->u_log.lc_stdout_wfd = -1;
	}
	if (u->u_log.lc_stderr_wfd >= 0) {
		close(u->u_log.lc_stderr_wfd);
		u->u_log.lc_stderr_wfd = -1;
	}

	if (error != 0) {
		log_warn("%s: posix_spawn: %s", u->u_name, strerror(error));
		if (notify_efd >= 0) {
			close(notify_efd);
			notify_efd = -1;
		}
		u->u_notify_fd = -1;
		return (-1);
	}

	u->u_pid = pid;
	u->u_procdesc_fd = procdesc_fd;
	clock_gettime(CLOCK_MONOTONIC, &u->u_last_start);

	/* Monitor the process descriptor via kqueue */
	{
		struct kevent kev;

		EV_SET(&kev, procdesc_fd, EVFILT_PROCDESC, EV_ADD | EV_ONESHOT,
		    NOTE_EXIT, 0, u);
		if (kevent(ctx->ctx_kq, &kev, 1, NULL, 0, NULL) < 0) {
			log_warn("%s: kevent EVFILT_PROCDESC: %s",
			    u->u_name, strerror(errno));
		}
	}

	/* Monitor readiness notification if using fd method */
	if (notify_efd >= 0) {
		struct kevent kev;

		u->u_notify_fd = notify_efd;
		EV_SET(&kev, notify_efd, EVFILT_READ, EV_ADD | EV_ONESHOT,
		    0, 0, u);
		if (kevent(ctx->ctx_kq, &kev, 1, NULL, 0, NULL) < 0)
			log_warn("%s: kevent readiness fd: %s",
			    u->u_name, strerror(errno));
	}

	/* Register syslog pipe fds in kqueue for draining */
	log_register_pipe_fds(u, ctx->ctx_kq);

	/* Apply rctl rules */
	rctl_apply(u);

	/* OOM protection via procctl(PROC_SPROTECT) */
	if (u->u_proc.pc_oom_protect) {
		int flags = PPROT_SET | PPROT_DESCEND | PPROT_INHERIT;

		if (procctl(P_PID, pid, PROC_SPROTECT, &flags) != 0)
			log_warn("%s: PROC_SPROTECT: %s", u->u_name,
			    strerror(errno));
	}

	boottrace("rcd: started %s", u->u_name);
	log_info("started %s (pid %d, procdesc fd %d)",
	    u->u_name, pid, procdesc_fd);

	/*
	 * simple/forking: mark running immediately (procdesc
	 * tracks the daemon or the sub-reaper).
	 * legacy (oneshot): stay STATE_STARTING until the shell
	 * exits and procdesc event gives the exit status.
	 */
	if (u->u_type == UNIT_SIMPLE || u->u_type == UNIT_FORKING ||
	    u->u_type == UNIT_LEGACY_FORKING)
		u->u_state = STATE_RUNNING;

	/* Run start_postcmd hook */
	if (u->u_start_postcmd != NULL)
		proc_run_hook_inst(u->u_start_postcmd, u);

	return (0);
}

/*
 * Send a signal via process descriptor, retrying on EINTR.
 * Returns 0 on success, -1 on error (ESRCH is not an error).
 */
static int
pdkill_retry(int fd, int sig)
{
	int ret;

	do {
		ret = pdkill(fd, sig);
	} while (ret < 0 && errno == EINTR);

	if (ret < 0 && errno != ESRCH)
		return (-1);
	return (0);
}

/*
 * Stop a running service.
 *
 * In the normal (async) case used during the kqueue event loop, sends
 * SIGTERM and sets a watchdog timer.  If the process doesn't exit in
 * time, the timer fires and proc_kill_subtree SIGKILL's everything.
 *
 * During shutdown, proc_stop_sync() can be used for a blocking wait
 * using pdwait(2) on the process descriptor.
 */
int
proc_stop(struct rcd_ctx *ctx, struct unit *u)
{
	struct kevent kev;

	if (u->u_state != STATE_RUNNING && u->u_state != STATE_STARTING &&
	    u->u_state != STATE_DONE)
		return (0);

	u->u_state = STATE_STOPPING;

	/* Run stop_precmd hook */
	if (u->u_stop_precmd != NULL)
		proc_run_hook_inst(u->u_stop_precmd, u);

	/*
	 * If the unit has an explicit stop_command, run it instead of
	 * sending a signal.  This covers oneshots (apm -e disable) and
	 * services with custom teardown logic.
	 */
	if (u->u_stop_command != NULL) {
		int rc = proc_run_hook_inst(u->u_stop_command, u);

		if (u->u_stop_postcmd != NULL)
			proc_run_hook_inst(u->u_stop_postcmd, u);
		u->u_state = (rc == 0) ? STATE_INACTIVE : STATE_FAILED;
		return (rc);
	}

	/* No running process to signal (e.g., done oneshot without stop_command) */
	if (u->u_procdesc_fd < 0) {
		u->u_state = STATE_INACTIVE;
		return (0);
	}

	/* Send stop signal via pdkill(2) using the process descriptor */
	if (pdkill_retry(u->u_procdesc_fd, u->u_sig_stop) != 0)
		log_warn("%s: pdkill(%d): %s", u->u_name,
		    u->u_sig_stop, strerror(errno));

	/* Set a watchdog timer; if the process doesn't exit in time, SIGKILL */
	EV_SET(&kev, (uintptr_t)u, EVFILT_TIMER, EV_ADD | EV_ONESHOT,
	    NOTE_MSECONDS, ctx->ctx_config.cfg_stop_timeout_ms, u);
	if (kevent(ctx->ctx_kq, &kev, 1, NULL, 0, NULL) < 0)
		log_warn("%s: kevent stop timer: %s",
		    u->u_name, strerror(errno));

	return (0);
}

/*
 * Synchronous stop with timeout using kqueue + pdwait.
 * Waits up to stop_timeout_ms for the process to exit after SIGTERM,
 * then force-kills via PROC_REAP_KILL if it doesn't.
 */
int
proc_stop_sync(struct rcd_ctx *ctx, struct unit *u)
{
	struct kevent kev;
	struct timespec ts;
	int kq, status, nev;

	/*
	 * For units with no running process (e.g., legacy scripts in
	 * STATE_DONE or oneshots with stop_command), run the stop
	 * command directly without trying to signal a process.
	 */
	if (u->u_procdesc_fd < 0) {
		if (u->u_stop_command != NULL) {
			if (u->u_stop_precmd != NULL)
				proc_run_hook_inst(u->u_stop_precmd, u);
			proc_run_hook_inst(u->u_stop_command, u);
			if (u->u_stop_postcmd != NULL)
				proc_run_hook_inst(u->u_stop_postcmd, u);
			u->u_state = STATE_INACTIVE;
		}
		return (0);
	}

	/*
	 * Remove any pending EVFILT_PROCDESC from the main kqueue
	 * before we wait on the temporary one.  This prevents a stale
	 * event from firing in the main loop after we restart the unit.
	 */
	EV_SET(&kev, u->u_procdesc_fd, EVFILT_PROCDESC, EV_DELETE, 0, 0,
	    NULL);
	if (kevent(ctx->ctx_kq, &kev, 1, NULL, 0, NULL) < 0 &&
	    errno != ENOENT)
		log_warn("%s: kevent delete procdesc: %s", u->u_name,
		    strerror(errno));

	/* Send stop signal */
	pdkill_retry(u->u_procdesc_fd, u->u_sig_stop);

	/* Use a temporary kqueue to wait with timeout */
	kq = kqueue();
	if (kq < 0) {
		log_warn("%s: kqueue: %s, falling back to polling wait",
		    u->u_name, strerror(errno));
		/* Fallback: force-kill and poll until dead */
		proc_kill_subtree(u);
		for (int tries = 0; tries < 100; tries++) {
			if (pdwait(u->u_procdesc_fd, &status,
			    WNOHANG, NULL, NULL) > 0)
				break;
			usleep(10000); /* 10ms, up to 1s total */
		}
		goto cleanup;
	}

	EV_SET(&kev, u->u_procdesc_fd, EVFILT_PROCDESC, EV_ADD | EV_ONESHOT,
	    NOTE_EXIT, 0, NULL);
	kevent(kq, &kev, 1, NULL, 0, NULL);

	ts.tv_sec = ctx->ctx_config.cfg_stop_timeout_ms / 1000;
	ts.tv_nsec = (ctx->ctx_config.cfg_stop_timeout_ms % 1000) * 1000000;

	nev = kevent(kq, NULL, 0, &kev, 1, &ts);
	close(kq);

	if (nev > 0) {
		/* Process exited within timeout, reap it */
		pdwait(u->u_procdesc_fd, &status, WNOHANG, NULL, NULL);
		goto cleanup;
	}

	log_warn("%s: stop timeout, killing", u->u_name);
	proc_kill_subtree(u);
	/* Wait synchronously after force-kill */
	while (pdwait(u->u_procdesc_fd, &status, 0, NULL, NULL) < 0 &&
	    errno == EINTR)
		;

cleanup:
	rctl_remove(u);
	if (u->u_jail.jc_enable)
		jail_svc_destroy(u);
	if (u->u_log.lc_stdout_pipefd >= 0) {
		close(u->u_log.lc_stdout_pipefd);
		u->u_log.lc_stdout_pipefd = -1;
	}
	if (u->u_log.lc_stderr_pipefd >= 0) {
		close(u->u_log.lc_stderr_pipefd);
		u->u_log.lc_stderr_pipefd = -1;
	}
	close(u->u_procdesc_fd);
	u->u_procdesc_fd = -1;
	u->u_pid = -1;
	u->u_state = STATE_INACTIVE;

	return (0);
}

/*
 * Send a reload signal (SIGHUP) to a running service.
 */
int
proc_reload(struct rcd_ctx *ctx __unused, struct unit *u)
{

	if (u->u_state != STATE_RUNNING)
		return (-1);

	if (u->u_procdesc_fd >= 0) {
		if (pdkill_retry(u->u_procdesc_fd, u->u_sig_reload) != 0) {
			log_warn("%s: pdkill(%d): %s", u->u_name,
			    u->u_sig_reload, strerror(errno));
			return (-1);
		}
	}
	return (0);
}

/*
 * Kill all processes in a service's reaper subtree using PROC_REAP_KILL.
 */
int
proc_kill_subtree(struct unit *u)
{
	struct procctl_reaper_kill rk;
	int error;

	memset(&rk, 0, sizeof(rk));
	rk.rk_sig = SIGKILL;
	rk.rk_flags = REAPER_KILL_SUBTREE;
	rk.rk_subtree = u->u_pid;

	error = procctl(P_PID, getpid(), PROC_REAP_KILL, &rk);
	if (error != 0 && errno != ESRCH) {
		log_warn("%s: PROC_REAP_KILL: %s (killed %u)",
		    u->u_name, strerror(errno), rk.rk_killed);
		return (-1);
	}

	if (rk.rk_killed > 0)
		log_info("%s: killed %u remaining processes",
		    u->u_name, rk.rk_killed);

	return (0);
}

/*
 * Handle a service process exit.
 */
void
proc_handle_exit(struct rcd_ctx *ctx, struct unit *u, int status)
{
	bool should_restart;

	boottrace("rcd: stopped %s", u->u_name);
	log_info("%s exited (status %d, pid %d)",
	    u->u_name, status, u->u_pid);

	/* Run stop_postcmd hook */
	if (u->u_stop_postcmd != NULL)
		proc_run_hook_inst(u->u_stop_postcmd, u);

	/*
	 * Kill remaining descendants.  For forking/legacy services
	 * in reaper mode, the rcd-exec sub-reaper already killed
	 * its subtree before exiting, so this is a no-op.
	 */
	proc_kill_subtree(u);

	/* Clean up rctl rules */
	rctl_remove(u);

	/* Clean up jail */
	if (u->u_jail.jc_enable)
		jail_svc_destroy(u);

	/* Clean up readiness notification fd */
	if (u->u_notify_fd >= 0) {
		close(u->u_notify_fd);
		u->u_notify_fd = -1;
	}

	/* Flush and close syslog pipe fds */
	log_flush_pipes(u);

	/*
	 * Reap the zombie.  EVFILT_PROCDESC delivers the exit status
	 * but does not consume the wait record.  Without an explicit
	 * pdwait the zombie persists even after the procdesc fd is
	 * closed, because SA_NOCLDWAIT may not have been active when
	 * the process exited (e.g. during boot, before the event loop
	 * sets signal(SIGCHLD, SIG_IGN)).
	 */
	if (u->u_procdesc_fd >= 0)
		xpdwait(u->u_procdesc_fd, NULL, WNOHANG);

	/* Close process descriptor */
	if (u->u_procdesc_fd >= 0) {
		close(u->u_procdesc_fd);
		u->u_procdesc_fd = -1;
	}
	u->u_pid = -1;
	u->u_exit_status = status;

	/*
	 * Drain any children reparented from the subreaper (rcd-exec).
	 * When a legacy/forking service exits, rcd-exec's children
	 * (the daemon and any workers) may have died before or at the
	 * same time as rcd-exec.  If reaper_kill_all() in rcd-exec
	 * didn't fully reap them (race), they become zombies under rcd
	 * and need an explicit drain.
	 */
	while (waitpid(-1, NULL, WNOHANG) > 0)
		;

	/* Evaluate restart policy */
	should_restart = false;
	switch (u->u_restart.rc_policy) {
	case RESTART_ALWAYS:
		should_restart = true;
		break;
	case RESTART_ON_FAILURE:
		should_restart = (status != 0);
		break;
	case RESTART_NEVER:
		break;
	}

	/* During shutdown, never restart */
	if (ctx->ctx_shutting_down)
		should_restart = false;

	/* Oneshot services don't restart */
	if (u->u_type == UNIT_ONESHOT) {
		should_restart = false;
		if (status == 0)
			u->u_state = STATE_DONE;
		else
			u->u_state = STATE_FAILED;
		return;
	}

	/*
	 * Forking and legacy services use the rcd-exec sub-reaper.
	 * The reaper stays alive while the daemon runs, and exits
	 * when the daemon exits (forwarding its exit status).
	 * So this exit event means the daemon is gone.
	 */
	if (u->u_type == UNIT_LEGACY || u->u_type == UNIT_LEGACY_FORKING ||
	    u->u_type == UNIT_FORKING) {
		u->u_state = (status == 0) ? STATE_DONE : STATE_FAILED;
		return;
	}

	if (!should_restart) {
		u->u_state = (status == 0) ? STATE_DONE : STATE_FAILED;
		return;
	}

	/* Check retry limit */
	u->u_retry_count++;
	if (u->u_restart.rc_max_retries > 0 &&
	    u->u_retry_count > u->u_restart.rc_max_retries) {
		log_warn("%s: max retries (%u) exceeded",
		    u->u_name, u->u_restart.rc_max_retries);
		u->u_state = STATE_FAILED;
		return;
	}

	/* Schedule restart with delay */
	{
		struct kevent kev;
		unsigned int delay;

		delay = u->u_restart.rc_delay_ms;

		/* Apply backoff, capped to 1 hour */
		switch (u->u_restart.rc_backoff) {
		case BACKOFF_EXPONENTIAL:
			{
				unsigned int shift;

				shift = u->u_retry_count - 1;
				if (shift > 20)
					shift = 20;
				delay *= (1u << shift);
			}
			break;
		case BACKOFF_LINEAR:
			delay *= u->u_retry_count;
			break;
		case BACKOFF_NONE:
			break;
		}

		/* Cap restart delay to 1 hour */
		if (delay > 3600000)
			delay = 3600000;

		u->u_state = STATE_WAITING;
		log_info("%s: scheduling restart in %u ms (attempt %u)",
		    u->u_name, delay, u->u_retry_count);

		EV_SET(&kev, (uintptr_t)u, EVFILT_TIMER,
		    EV_ADD | EV_ONESHOT, NOTE_MSECONDS, delay, u);
		if (kevent(ctx->ctx_kq, &kev, 1, NULL, 0, NULL) < 0)
			log_warn("%s: kevent restart timer: %s",
			    u->u_name, strerror(errno));
	}
}
