/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Baptiste Daroussin <bapt@FreeBSD.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef _RCD_H_
#define _RCD_H_

#include <sys/types.h>
#include <sys/event.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/ucred.h>
#include <spawn.h>
#include <stdbool.h>

#include <ucl.h>

#include <setjmp.h>

/*
 * Allocation error handling.
 * Default is abort(); rcd overrides to longjmp for graceful recovery.
 */
extern jmp_buf	rcd_oom_env;
#define HASH_ALLOC_ERROR	longjmp(rcd_oom_env, 1)

#include "hash.h"
#include "vec.h"

#define XALLOC_ERROR	longjmp(rcd_oom_env, 1)
#include "xmalloc.h"

#include "xio.h"

/*
 * Forward declarations.
 */
struct unit;
struct depgraph;
struct rcd_config;

/*
 * Service types.
 */
enum unit_type {
	UNIT_SIMPLE,		/* Foreground daemon, tracked directly */
	UNIT_FORKING,		/* Daemonizes itself (native) */
	UNIT_ONESHOT,		/* Run-to-completion */
	UNIT_BARRIER,		/* Dependency synchronisation point, no command */
	UNIT_LEGACY,		/* Wrapped rc.d oneshot script */
	UNIT_LEGACY_FORKING	/* Wrapped rc.d daemon (has pidfile/command) */
};

/*
 * Readiness notification methods.
 */
enum ready_method {
	READY_IMMEDIATE,	/* Ready once process is started */
	READY_FD,		/* Service writes to a notification fd */
	READY_EXIT,		/* Parent process exits (forking type) */
	READY_SOCKET		/* Pre-bound socket is serving */
};

/*
 * Restart policies.
 */
enum restart_policy {
	RESTART_NEVER,
	RESTART_ON_FAILURE,
	RESTART_ALWAYS
};

/*
 * Backoff strategies for restart delay.
 */
enum restart_backoff {
	BACKOFF_NONE,
	BACKOFF_LINEAR,
	BACKOFF_EXPONENTIAL
};

/*
 * Service states.
 */
enum unit_state {
	STATE_INACTIVE,		/* Not started */
	STATE_STARTING,		/* Start in progress */
	STATE_RUNNING,		/* Running and ready */
	STATE_STOPPING,		/* Stop in progress */
	STATE_FAILED,		/* Exited with failure */
	STATE_DONE,		/* Oneshot completed successfully */
	STATE_WAITING		/* Waiting for restart delay */
};

/*
 * Socket types for socket activation.
 */
enum sock_type {
	SOCK_ACT_STREAM,
	SOCK_ACT_DGRAM,
	SOCK_ACT_SEQPACKET
};

/*
 * Socket address families.
 */
enum sock_family {
	SOCK_FAM_TCP,
	SOCK_FAM_TCP6,
	SOCK_FAM_UDP,
	SOCK_FAM_UDP6,
	SOCK_FAM_UNIX
};

/*
 * Socket activation definition.
 */
struct unit_socket {
	char			*us_name;
	enum sock_type		 us_type;
	enum sock_family	 us_family;
	char			*us_address;	/* "addr:port" or path */
	int			 us_backlog;
	int			 us_fd;		/* Bound socket fd, -1 if not yet */
	mode_t			 us_permissions;/* For unix sockets */
	char			*us_owner;
	char			*us_group;
	struct kevent		 us_kev;	/* Deferred kevent for READY_SOCKET */
	TAILQ_ENTRY(unit_socket) us_entries;
};

TAILQ_HEAD(unit_socket_list, unit_socket);

/*
 * Restart configuration.
 */
struct restart_conf {
	enum restart_policy	 rc_policy;
	enum restart_backoff	 rc_backoff;
	unsigned int		 rc_max_retries;
	unsigned int		 rc_delay_ms;
	unsigned int		 rc_reset_ms;
};

/*
 * Process configuration (credentials, rlimits, etc.).
 */
struct proc_conf {
	char			*pc_user;
	char			*pc_group;
	charv_t			 pc_groups;	/* Supplementary groups */
	char			*pc_chdir;
	char			*pc_chroot;
	mode_t			 pc_umask;
	int			 pc_nice;
	char			*pc_cpuset;
	int			 pc_fib;	/* Routing table (setfib) */
	char			*pc_login_class; /* Login class for setusercontext */
	char			*pc_limits;	/* Resource limits string */
	char			*pc_env_file;	/* File with extra env vars */
	bool			 pc_oom_protect;
};

/*
 * rctl rule as parsed from unit file.
 */
struct rctl_conf {
	char			*rc_resource;	/* "memoryuse", "pcpu", etc. */
	char			*rc_action;	/* "deny", "log", etc. */
	char			*rc_amount;	/* "2g", "256", etc. */
	STAILQ_ENTRY(rctl_conf)	 rc_entries;
};

STAILQ_HEAD(rctl_conf_list, rctl_conf);

/*
 * Active rctl rule (applied to the system, needs cleanup on stop).
 */
struct rctl_active {
	char			 ra_rule[256];
	STAILQ_ENTRY(rctl_active) ra_entries;
};

STAILQ_HEAD(rctl_active_list, rctl_active);

/*
 * Service jail configuration.
 */
struct jail_conf {
	bool			 jc_enable;
	char			*jc_name;	/* Auto-generated if NULL */
	char			*jc_path;	/* Default: "/" */
	charv_t			 jc_options;
	charv_t			 jc_ip4addr;
	charv_t			 jc_ip6addr;
	bool			 jc_devfs;
	int			 jc_jid;	/* Assigned jail ID, 0 if none */
};

/*
 * Logging configuration.
 */
struct log_conf {
	char			*lc_stdout;	/* "syslog:facility.level" or "file:path" */
	char			*lc_stderr;
	int			 lc_stdout_pipefd;  /* Read-end for syslog pipe, -1 if none */
	int			 lc_stdout_wfd;	    /* Write-end, closed after spawn */
	int			 lc_stderr_pipefd;
	int			 lc_stderr_wfd;
	int			 lc_stdout_priority; /* syslog priority for stdout pipe */
	int			 lc_stderr_priority;
	char			 lc_stdout_resid[4096]; /* Partial line buffer for stdout pipe */
	size_t			 lc_stdout_resid_len;
	char			 lc_stderr_resid[4096]; /* Partial line buffer for stderr pipe */
	size_t			 lc_stderr_resid_len;
};

/*
 * Generic key-value pair.  Used for environment variables,
 * required_sysctl checks, and other name=value mappings.
 */
struct kv {
	char			*kv_key;
	char			*kv_val;
	STAILQ_ENTRY(kv)	 kv_entries;
};

STAILQ_HEAD(kv_list, kv);

/*
 * Dependency link in the graph.
 */
struct dep_link {
	struct unit		*dl_unit;
	STAILQ_ENTRY(dep_link)	 dl_entries;
};

STAILQ_HEAD(dep_list, dep_link);

/*
 * A provision name (a unit may provide multiple names).
 */
struct provision {
	char			*pv_name;
	struct unit		*pv_unit;	/* Back-pointer */
	STAILQ_ENTRY(provision)	 pv_entries;
};

STAILQ_HEAD(provision_list, provision);

/*
 * Per-service access control.
 * Each charv_t contains a list of principals: "username" or "@groupname".
 */
struct unit_access {
	charv_t			 ua_start;
	charv_t			 ua_stop;
	charv_t			 ua_restart;
	charv_t			 ua_reload;
	charv_t			 ua_status;
};

/*
 * Service unit — the central data structure.
 */
struct unit {
	/* Identity */
	char			*u_name;
	char			*u_description;
	char			*u_path;	/* Path to unit file or rc.d script */
	enum unit_type		 u_type;
	enum unit_state		 u_state;

	/* Command */
	char			*u_command;
	char			*u_command_args;
	char			*u_command_prepend; /* Prepended before command in argv */
	char			*u_exec;	/* Inline exec for oneshots (lua: or shell) */
	char			*u_stop_command;
	char			*u_off_command;	/* Run at boot if service is disabled */
	int			 u_sig_stop;	/* Signal for stop (default SIGTERM) */
	int			 u_sig_reload;	/* Signal for reload (default SIGHUP) */
	unsigned int		 u_start_delay_ms; /* Delay before start (0 = none) */

	/* Preconditions — checked before start */
	charv_t			 u_required_dirs;
	charv_t			 u_required_files;
	charv_t			 u_required_modules;
	charv_t			 u_required_vars;  /* Config keys that must be set */
	struct kv_list	 u_required_sysctl; /* sysctl name=value checks */

	/* Hooks — commands run before/after start/stop */
	char			*u_setup_cmd;	/* Run before precmd on start/restart/reload */
	char			*u_start_precmd;
	char			*u_start_postcmd;
	char			*u_stop_precmd;
	char			*u_stop_postcmd;

	/* Extra commands: name → exec code (lua: or shell) */
	struct kv_list		 u_commands;

	/* Dependencies (string lists parsed from unit file) */
	charv_t			 u_provide;
	charv_t			 u_require;
	charv_t			 u_before;
	charv_t			 u_keyword;

	/* Resolved dependency links (populated by depgraph) */
	struct dep_list		 u_deps;	/* Units we depend on */
	struct dep_list		 u_rdeps;	/* Units that depend on us */
	int			 u_unmet;	/* Count of unmet dependencies */

	/* Readiness */
	enum ready_method	 u_ready_method;

	/* Process tracking */
	pid_t			 u_pid;
	int			 u_procdesc_fd;	/* Process descriptor from posix_spawn */
	int			 u_notify_fd;	/* Readiness notification eventfd, -1 if unused */
	int			 u_exit_status;

	/* Restart */
	struct restart_conf	 u_restart;
	unsigned int		 u_retry_count;
	struct timespec		 u_last_start;

	/* Subsystem configs */
	struct proc_conf	 u_proc;
	struct rctl_conf_list	 u_rctl;
	struct rctl_active_list	 u_rctl_active;
	struct jail_conf	 u_jail;
	struct log_conf		 u_log;
	struct kv_list	 u_env;
	struct unit_socket_list	 u_sockets;
	struct unit_access	 u_access;

	/* Flags */
	bool			 u_enabled;
	bool			 u_nostart;	/* nostart keyword: skip at boot, allow manual */
	bool			 u_boot_only;	/* firstboot keyword */
	bool			 u_nojail;	/* nojail keyword */
	bool			 u_nojailvnet;	/* nojailvnet keyword */
	bool			 u_resume;	/* restart on resume from suspend */
	bool			 u_has_rcvar;	/* Legacy script has rcvar= */
	bool			 u_template;	/* This is a template, not a real unit */
	char			*u_instance;	/* Instance name (NULL if not from template) */
	char			*u_instance_conf; /* UCL string of per-instance config */
	char			*u_override_conf; /* UCL string of global override config */
	struct unit		*u_template_ref; /* Back-pointer to template */

	/* List linkage */
	TAILQ_ENTRY(unit)	 u_entries;
};

TAILQ_HEAD(unit_list, unit);

/*
 * Global rcd configuration.
 */
struct rcd_config {
	bool			 cfg_parallel;
	unsigned int		 cfg_max_parallel;
	char			**cfg_unit_paths;
	int			 cfg_nunit_paths;
	char			**cfg_legacy_paths;
	int			 cfg_nlegacy_paths;
	char			**cfg_legacy_rc_conf;
	int			 cfg_nlegacy_rc_conf;
	hash_t			*cfg_rcvars;	/* Cached rc.conf variables */
	char			*cfg_control_socket;
	mode_t			 cfg_control_perms;
	char			*cfg_control_group;
	int			 cfg_log_level;
	unsigned int		 cfg_stop_timeout_ms;
	unsigned int		 cfg_shutdown_timeout_ms;
	char			*cfg_firstboot_sentinel;
	bool			 cfg_precious_machine;	/* Refuse shutdown */
	bool			 cfg_quiet_boot;	/* Suppress info logs during boot */
	bool			 cfg_veriexec;		/* Verify unit file integrity */
	ucl_object_t		*cfg_unit_schema;	/* Loaded once, shared */
};

/*
 * Dependency graph.
 */
struct depgraph {
	struct unit_list	 dg_units;
	unsigned int		 dg_nunits;
	hash_t			*dg_provisions;
};

/*
 * Main daemon context.
 */
struct rcd_ctx {
	struct rcd_config	 ctx_config;
	struct depgraph		 ctx_graph;
	int			 ctx_kq;	/* kqueue fd */
	int			 ctx_ctlsock;	/* Control socket fd */
	int			 ctx_ctlsock_pathfd; /* Vnode watch fd */
	bool			 ctx_booting;	/* Still in boot phase */
	bool			 ctx_shutting_down;
	bool			 ctx_jailed;	/* Running inside a jail */
	bool			 ctx_diskless;	/* Diskless boot detected */
	unsigned int		 ctx_running;	/* Services currently starting */
};

/*
 * Control protocol commands.
 */
enum ctl_command {
	CTL_START,
	CTL_STOP,
	CTL_RESTART,
	CTL_RELOAD,
	CTL_ENABLE,
	CTL_DISABLE,
	CTL_STATUS,
	CTL_RESOURCES,
	CTL_DEPS,
	CTL_LIST,
	CTL_RELOAD_CONFIG
};

/* rcd.c */
int		 rcd_main_loop(struct rcd_ctx *);
void		 rcd_signal_ready(struct rcd_ctx *);

/* unit.c */
struct unit	*unit_parse(const char *, struct rcd_config *);
void		 unit_free(struct unit *);
struct unit	*unit_instantiate(struct unit *, const char *);
int		 ucl_parse_mode(const ucl_object_t *, mode_t *);
struct unit	*unit_alloc(void);
int		 unit_apply_overrides(struct unit *, const char *);
bool		 valid_service_name(const char *);

/* depgraph.c */
int		 depgraph_init(struct depgraph *);
int		 depgraph_add(struct depgraph *, struct unit *);
int		 depgraph_resolve(struct depgraph *);
int		 depgraph_check_cycles(struct depgraph *);
struct unit	*depgraph_find(struct depgraph *, const char *);
void		 depgraph_ready_set(struct depgraph *, struct unit **, int *);
void		 depgraph_mark_done(struct depgraph *, struct unit *);
void		 depgraph_shutdown_order(struct depgraph *, struct unit **, int *);
void		 depgraph_free(struct depgraph *);

/* process.c */
int		 proc_spawn(struct rcd_ctx *, struct unit *);
int		 proc_stop(struct rcd_ctx *, struct unit *);
int		 proc_stop_sync(struct rcd_ctx *, struct unit *);
int		 proc_reload(struct rcd_ctx *, struct unit *);
void		 proc_handle_exit(struct rcd_ctx *, struct unit *, int);
int		 proc_kill_subtree(struct unit *);
int		 proc_reaper_init(void);
int		 proc_check_preconditions(struct unit *);
int		 proc_load_modules(struct unit *);
int		 proc_run_hook(const char *);
int		 proc_run_hook_inst(const char *, const struct unit *);
void		 tokenize(const char *, charv_t *);

/* sockact.c */
int		 parse_listen_addr(const char *, struct sockaddr_storage *,
		    socklen_t *, int *, int *);
int		 sockact_bind(struct unit_socket *);
void		 sockact_register(struct rcd_ctx *, struct unit *);
void		 sockact_register_deferred(struct unit *);
void		 sockact_deferred_register_all(struct rcd_ctx *, struct unit *);
void		 sockact_close(struct unit *);
int		 sockact_setup_fds(struct unit *, posix_spawn_file_actions_t *,
		    int *);

/* compat.c */
int		 compat_scan(struct rcd_ctx *, const char *);
int		 compat_parse_headers(const char *, struct unit *);
int		 compat_load_rcvars(struct rcd_config *);
bool		 compat_is_enabled(const char *, struct rcd_config *);


/* control.c */
int		 control_init(struct rcd_ctx *);
void		 control_reinit(struct rcd_ctx *);
void		 control_handle(struct rcd_ctx *, int);
void		 control_close(struct rcd_ctx *);
bool		 access_check(const struct unit *, const char *,
		    const struct xucred *);

/* rctl_mgr.c */
int		 rctl_apply(struct unit *);
void		 rctl_remove(struct unit *);
int		 rctl_get_usage(struct unit *, char *, size_t);
bool		 rctl_available(void);

/* jail_svc.c */
int		 jail_svc_create(struct unit *);
int		 jail_svc_destroy(struct unit *);

/* log.c */
void		 log_init(int);
void		 log_set_verbose(bool);
void		 log_console_open(void);
void		 log_console_close(void);
void		 log_console(const char *, ...) __printflike(1, 2);
void		 log_console_set_enabled(bool);
void		 log_info(const char *, ...) __printflike(1, 2);
void		 log_warn(const char *, ...) __printflike(1, 2);
void		 log_err(int, const char *, ...) __printflike(2, 3);
void		 log_debug(const char *, ...) __printflike(1, 2);
int		 log_setup_fds(struct unit *, posix_spawn_file_actions_t *);
int		 log_register_pipe_fds(struct unit *, int);
void		 log_handle_pipe_event(struct unit *, int);
void		 log_flush_pipes(struct unit *);
void		 boottrace(const char *, ...) __printflike(1, 2);

/* enable.c */
int		 enable_service(const char *, const char *);
int		 disable_service(const char *, const char *);
int		 delete_override(const char *);

/* luaexec.c */
void		 lua_init(void);
int		 lua_exec(const char *, const char *, const struct unit *);
void		 lua_fini(void);

#define LUA_HOOK_PREFIX	"lua:"
#define IS_LUA_HOOK(s)	((s) != NULL && \
			    strncmp((s), LUA_HOOK_PREFIX, 4) == 0)

#endif /* !_RCD_H_ */
