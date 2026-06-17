/*
 * Copyright (c) 2026 Baptiste Daroussin <bapt@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * rcd-exec — Pre-exec setup helper for rcd(8).
 *
 * Spawned by rcd via posix_spawn(3).  Reads setup instructions from
 * environment variables (set by rcd), applies them, and exec's the
 * real service command.
 *
 * This helper exists to be able to express optioations like setuid,
 * setgid, jail_attach, rlimits, cpuset, or umask.
 * By having rcd posix_spawn this helper (getting a process descriptor),
 * the helper then does the privileged setup and exec's the service.
 * The process descriptor continues to track the process tree.
 *
 * Environment variables read:
 *   RCD_USER       - setuid target
 *   RCD_GROUP      - setgid target
 *   RCD_UMASK      - umask (octal)
 *   RCD_NICE       - nice level
 *   RCD_CPUSET     - cpuset specification
 *   RCD_JAIL       - jail name to attach to
 *   RCD_NOTIFY_FD  - readiness notification fd
 *   RCD_LIMITS     - resource limits (e.g. "openfiles=1024,stacksize=8388608")
 *   RCD_CHDIR      - working directory (backup if not set via file_actions)
 *
 * argv[1..] is the service command and arguments.
 */

#include <sys/param.h>
#include <sys/cpuset.h>
#include <sys/jail.h>
#include <sys/procctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/wait.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <jail.h>
#include <login_cap.h>
#include <pwd.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void
apply_umask(void)
{
	const char *val, *errstr;
	mode_t mask;

	val = getenv("RCD_UMASK");
	if (val == NULL)
		return;

	mask = (mode_t)strtonum(val, 0, 0777, &errstr);
	if (errstr != NULL) {
		warnx("invalid RCD_UMASK: %s (%s)", val, errstr);
		return;
	}
	umask(mask);
}

static void
apply_nice(void)
{
	const char *val, *errstr;
	int prio;

	val = getenv("RCD_NICE");
	if (val == NULL)
		return;

	prio = (int)strtonum(val, -20, 20, &errstr);
	if (errstr != NULL) {
		warnx("invalid RCD_NICE: %s (%s)", val, errstr);
		return;
	}
	if (setpriority(PRIO_PROCESS, 0, prio) != 0)
		warn("setpriority(%d)", prio);
}

/*
 * Parse a cpuset specification string (e.g., "0-3,8,12-15") into a
 * cpuset_t bitmask.  Supports individual CPUs and ranges.
 */
static int
parse_cpuset(const char *spec, cpuset_t *mask)
{
	const char *p, *errstr;
	long lo, hi, i;

	CPU_ZERO(mask);
	p = spec;

	while (*p != '\0') {
		/* Skip whitespace */
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '\0')
			break;

		lo = strtonum(p, 0, CPU_SETSIZE - 1, &errstr);
		if (errstr != NULL)
			return (-1);
		/* Advance past the number */
		while (*p >= '0' && *p <= '9')
			p++;

		if (*p == '-') {
			p++;
			hi = strtonum(p, lo, CPU_SETSIZE - 1, &errstr);
			if (errstr != NULL)
				return (-1);
			/* Advance past the number */
			while (*p >= '0' && *p <= '9')
				p++;
		} else {
			hi = lo;
		}

		for (i = lo; i <= hi; i++)
			CPU_SET(i, mask);

		if (*p == ',')
			p++;
	}

	return (0);
}

static void
apply_cpuset(void)
{
	const char *val;
	cpuset_t mask;

	val = getenv("RCD_CPUSET");
	if (val == NULL)
		return;

	if (parse_cpuset(val, &mask) != 0) {
		warnx("invalid cpuset specification: %s", val);
		return;
	}

	if (cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_PID, -1,
	    sizeof(mask), &mask) != 0)
		warn("cpuset_setaffinity");
}

/*
 * Set the FIB (routing table) for this process.
 * Validates against the kernel's net.fibs sysctl.
 */
static void
apply_fib(void)
{
	const char *val, *errstr;
	int fib, maxfibs;
	size_t len;

	val = getenv("RCD_FIB");
	if (val == NULL)
		return;

	/* Query the number of FIBs supported by the running kernel */
	maxfibs = 1;
	len = sizeof(maxfibs);
	sysctlbyname("net.fibs", &maxfibs, &len, NULL, 0);

	fib = (int)strtonum(val, 0, maxfibs - 1, &errstr);
	if (errstr != NULL) {
		warnx("invalid RCD_FIB: %s (%s, max %d)", val, errstr,
		    maxfibs - 1);
		return;
	}
	if (setfib(fib) != 0)
		warn("setfib(%d)", fib);
}

static rlim_t
parse_rlim(const char *s)
{
	const char *errstr;
	long long val;

	if (strcasecmp(s, "unlimited") == 0 ||
	    strcasecmp(s, "infinity") == 0 ||
	    strcmp(s, "-1") == 0)
		return (RLIM_INFINITY);
	val = strtonum(s, 0, LLONG_MAX, &errstr);
	if (errstr != NULL) {
		warnx("invalid rlimit value: %s (%s)", s, errstr);
		return (RLIM_INFINITY);
	}
	return ((rlim_t)val);
}

/*
 * Apply resource limits from RCD_LIMITS.
 * Format: comma-separated "resource=soft:hard" pairs.
 * Example: "stacksize=8388608:16777216,openfiles=1024:4096"
 * Values of -1 or "unlimited" mean RLIM_INFINITY.
 */
static void
apply_limits(void)
{
	static const struct {
		const char	*name;
		int		 resource;
	} rlmap[] = {
		{ "cputime",		RLIMIT_CPU },
		{ "filesize",		RLIMIT_FSIZE },
		{ "datasize",		RLIMIT_DATA },
		{ "stacksize",		RLIMIT_STACK },
		{ "coredumpsize",	RLIMIT_CORE },
		{ "memoryuse",		RLIMIT_RSS },
		{ "memorylocked",	RLIMIT_MEMLOCK },
		{ "maxprocesses",	RLIMIT_NPROC },
		{ "openfiles",		RLIMIT_NOFILE },
		{ "sbsize",		RLIMIT_SBSIZE },
		{ "vmemoryuse",		RLIMIT_VMEM },
		{ "npts",		RLIMIT_NPTS },
		{ "swapuse",		RLIMIT_SWAP },
		{ "kqueues",		RLIMIT_KQUEUES },
		{ "umtxp",		RLIMIT_UMTXP },
	};
	const char *val;
	char *buf, *pair, *eq, *colon;
	struct rlimit rl;
	size_t i;
	int res;

	val = getenv("RCD_LIMITS");
	if (val == NULL)
		return;

	buf = strdup(val);
	if (buf == NULL)
		return;

	for (pair = strtok(buf, ","); pair != NULL;
	    pair = strtok(NULL, ",")) {
		eq = strchr(pair, '=');
		if (eq == NULL)
			continue;
		*eq = '\0';

		/* Look up the resource name */
		res = -1;
		for (i = 0; i < nitems(rlmap); i++) {
			if (strcmp(pair, rlmap[i].name) == 0) {
				res = rlmap[i].resource;
				break;
			}
		}
		if (res < 0) {
			warnx("unknown rlimit resource: %s", pair);
			continue;
		}

		/* Parse soft:hard — "unlimited" or "-1" mean RLIM_INFINITY */
		colon = strchr(eq + 1, ':');
		if (colon == NULL) {
			rl.rlim_cur = parse_rlim(eq + 1);
			rl.rlim_max = rl.rlim_cur;
		} else {
			*colon = '\0';
			rl.rlim_cur = parse_rlim(eq + 1);
			rl.rlim_max = parse_rlim(colon + 1);
		}

		if (setrlimit(res, &rl) != 0)
			warn("setrlimit(%s)", pair);
	}

	free(buf);
}

/*
 * Apply chroot before credential drop.
 */
static void
apply_chroot(void)
{
	const char *val;

	val = getenv("RCD_CHROOT");
	if (val == NULL)
		return;

	if (chroot(val) != 0)
		err(1, "chroot(%s)", val);
	if (chdir("/") != 0)
		err(1, "chdir(/) after chroot");
}

/*
 * Load environment variables from a file.
 * Format: one VAR=value per line, comments with #, blank lines ignored.
 * Security: only opens files owned by root and not symlinks.
 * Refuses to set LD_* or other dangerous variables.
 */
static const char *dangerous_env_prefixes[] = {
	"LD_", "LIBPATH", "RCD_", "IFS", NULL
};

static bool
is_dangerous_envvar(const char *name)
{
	int i;

	for (i = 0; dangerous_env_prefixes[i] != NULL; i++) {
		if (strncmp(name, dangerous_env_prefixes[i],
		    strlen(dangerous_env_prefixes[i])) == 0)
			return (true);
	}
	return (false);
}

static void
apply_env_file(void)
{
	const char *path;
	FILE *fp;
	struct stat sb;
	char *line, *eq, *name, *val, *end;
	size_t linecap;
	ssize_t linelen;
	int fd;

	path = getenv("RCD_ENV_FILE");
	if (path == NULL)
		return;

	/* Open with O_NOFOLLOW to prevent symlink attacks */
	fd = open(path, O_RDONLY | O_NOFOLLOW);
	if (fd < 0) {
		warn("env_file: %s", path);
		return;
	}

	/* Verify ownership is root */
	if (fstat(fd, &sb) != 0 || sb.st_uid != 0) {
		warnx("env_file: %s: not owned by root, skipping", path);
		close(fd);
		return;
	}

	fp = fdopen(fd, "r");
	if (fp == NULL) {
		close(fd);
		return;
	}

	line = NULL;
	linecap = 0;
	while ((linelen = getline(&line, &linecap, fp)) > 0) {
		/* Strip trailing newline/carriage return */
		while (linelen > 0 &&
		    (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
			line[--linelen] = '\0';

		/* Skip comments and blank lines */
		name = line;
		while (*name == ' ' || *name == '\t')
			name++;
		if (*name == '#' || *name == '\0')
			continue;

		eq = strchr(name, '=');
		if (eq == NULL)
			continue;
		*eq = '\0';
		val = eq + 1;

		/* Reject dangerous environment variables */
		if (is_dangerous_envvar(name)) {
			warnx("env_file: refusing to set %s", name);
			continue;
		}

		/* Strip quotes around value */
		if ((*val == '"' || *val == '\'') && strlen(val) >= 2) {
			char q = *val;
			val++;
			end = val + strlen(val) - 1;
			if (*end == q)
				*end = '\0';
		}

		setenv(name, val, 1);
	}

	free(line);
	fclose(fp);
}

static void
apply_jail(void)
{
	const char *name;
	int jid;

	name = getenv("RCD_JAIL");
	if (name == NULL || name[0] == '\0')
		return;

	jid = jail_getid(name);
	if (jid < 0)
		errx(1, "jail not found: %s", name);

	if (jail_attach(jid) != 0)
		err(1, "jail_attach(%s)", name);
}

static void
apply_credentials(void)
{
	const char *user, *group, *groups_str, *login_class;
	struct passwd *pw;
	struct group *gr;
	gid_t gid, supp_gids[NGROUPS_MAX];
	int nsupp;
	char *buf, *tok;

	group = getenv("RCD_GROUP");
	user = getenv("RCD_USER");
	groups_str = getenv("RCD_GROUPS");
	login_class = getenv("RCD_LOGIN_CLASS");

	/* Primary group */
	if (group != NULL) {
		gr = getgrnam(group);
		if (gr == NULL)
			errx(1, "unknown group: %s", group);
		gid = gr->gr_gid;
		if (setgid(gid) != 0)
			err(1, "setgid(%s)", group);
	}

	if (user != NULL) {
		pw = getpwnam(user);
		if (pw == NULL)
			errx(1, "unknown user: %s", user);

		/* If no explicit group was set, drop to the user's gid */
		if (group == NULL) {
			if (setgid(pw->pw_gid) != 0)
				err(1, "setgid(%d)", pw->pw_gid);
		}

		/*
		 * Apply login class before setuid if specified.
		 * setusercontext sets rlimits, umask, priority from
		 * login.conf.
		 */
		if (login_class != NULL) {
			login_cap_t *lc;

			lc = login_getclass(login_class);
			if (lc == NULL)
				errx(1, "unknown login class: %s",
				    login_class);
			if (setusercontext(lc, pw, pw->pw_uid,
			    LOGIN_SETRESOURCES | LOGIN_SETPRIORITY) != 0)
				err(1, "setusercontext(%s)", login_class);
			login_close(lc);
		}

		/* Supplementary groups */
		if (groups_str != NULL) {
			nsupp = 0;
			/* Start with primary gid */
			supp_gids[nsupp++] = pw->pw_gid;

			buf = strdup(groups_str);
			if (buf == NULL)
				err(1, "strdup");
			for (tok = strtok(buf, ","); tok != NULL;
			    tok = strtok(NULL, ",")) {
				gr = getgrnam(tok);
				if (gr == NULL) {
					warnx("unknown group: %s", tok);
					continue;
				}
				if (nsupp < NGROUPS_MAX)
					supp_gids[nsupp++] = gr->gr_gid;
			}
			free(buf);
			if (setgroups(nsupp, supp_gids) != 0)
				err(1, "setgroups");
		} else {
			if (initgroups(pw->pw_name, pw->pw_gid) != 0)
				err(1, "initgroups(%s)", user);
		}

		if (setuid(pw->pw_uid) != 0)
			err(1, "setuid(%s)", user);
	}
}

static void
apply_chdir(void)
{
	const char *dir;

	dir = getenv("RCD_CHDIR");
	if (dir == NULL)
		return;

	if (chdir(dir) != 0)
		warn("chdir(%s)", dir);
}

/*
 * Sub-reaper mode for forking daemons and legacy scripts.
 *
 * When RCD_REAPER is set, rcd-exec becomes a sub-reaper for the
 * service.  It forks a child that execs the real command, then waits
 * for the initial process to exit.  Any daemon forked by the command
 * is reparented to us (the sub-reaper).  We then monitor the daemon
 * until it exits.
 *
 * This gives rcd reliable supervision of forking daemons and legacy
 * scripts without needing pidfiles.  rcd tracks us via a process
 * descriptor; we track the daemon via the reaper mechanism.
 *
 * Signals:
 *   SIGTERM → kill entire subtree (PROC_REAP_KILL) and exit
 *   Other  → forward to the daemon process
 */
static volatile sig_atomic_t reaper_stop;
static pid_t reaper_daemon_pid = -1;

static void
reaper_sighandler(int sig)
{

	if (sig == SIGTERM || sig == SIGINT)
		reaper_stop = 1;
	else if (reaper_daemon_pid > 0)
		kill(reaper_daemon_pid, sig);
}

/*
 * Find the daemon PID under our reaper.
 * After the initial process (shell or forking parent) exits,
 * the daemon is reparented to us.  We find it via PROC_REAP_STATUS.
 */
static pid_t
find_daemon_pid(void)
{
	struct procctl_reaper_status rs;
	struct procctl_reaper_pids rp;
	struct procctl_reaper_pidinfo *pids;
	pid_t found;
	unsigned int i;

	if (procctl(P_PID, getpid(), PROC_REAP_STATUS, &rs) != 0)
		return (-1);

	if (rs.rs_children == 0)
		return (-1);

	pids = calloc(rs.rs_descendants, sizeof(*pids));
	if (pids == NULL)
		return (-1);

	rp.rp_count = rs.rs_descendants;
	rp.rp_pids = pids;
	found = -1;

	if (procctl(P_PID, getpid(), PROC_REAP_GETPIDS, &rp) == 0) {
		for (i = 0; i < rp.rp_count; i++) {
			if (pids[i].pi_flags & REAPER_PIDINFO_CHILD) {
				found = pids[i].pi_pid;
				break;
			}
		}
	}

	free(pids);
	return (found);
}

static void
reaper_kill_all(void)
{
	struct procctl_reaper_kill rk;

	memset(&rk, 0, sizeof(rk));
	rk.rk_sig = SIGKILL;
	procctl(P_PID, getpid(), PROC_REAP_KILL, &rk);

	/*
	 * Reap all children after killing them, otherwise they become
	 * zombies reparented to the nearest subreaper ancestor (rcd).
	 * Loop until ECHILD or we give up after 500ms total.
	 */
	for (int tries = 0; tries < 10; tries++) {
		while (waitpid(-1, NULL, WNOHANG) > 0)
			;
		usleep(50000);
	}
}

/*
 * Signal readiness by writing to the notification fd (if set).
 * The fd number comes from RCD_NOTIFY_FD in the environment.
 * rcd is waiting on this eventfd before registering sockets.
 */
static void
signal_readiness(void)
{
	const char *fdstr;
	int fd;
	uint64_t val = 1;

	fdstr = getenv("RCD_NOTIFY_FD");
	if (fdstr == NULL)
		return;
	fd = strtonum(fdstr, 0, 255, NULL);
	if (fd <= 0)
		return;
	(void)write(fd, &val, sizeof(val));
}

static void
cleanup_rcd_env(void)
{

	unsetenv("RCD_USER");
	unsetenv("RCD_GROUP");
	unsetenv("RCD_GROUPS");
	unsetenv("RCD_UMASK");
	unsetenv("RCD_NICE");
	unsetenv("RCD_CPUSET");
	unsetenv("RCD_FIB");
	unsetenv("RCD_JAIL");
	unsetenv("RCD_CHROOT");
	unsetenv("RCD_LIMITS");
	unsetenv("RCD_LOGIN_CLASS");
	unsetenv("RCD_ENV_FILE");
	unsetenv("RCD_NOTIFY_FD");
	unsetenv("RCD_CHDIR");
	unsetenv("RCD_REAPER");
	unsetenv("RCD_SERVICE");
	unsetenv("RCD_INSTANCE");
}

static int
reaper_main(int argc __unused, char *argv[])
{
	struct sigaction sa;
	pid_t child, wpid;
	int status;

	/* Become sub-reaper */
	if (procctl(P_PID, getpid(), PROC_REAP_ACQUIRE, NULL) != 0)
		err(1, "PROC_REAP_ACQUIRE");

	/* Fork the actual command */
	child = fork();
	if (child < 0)
		err(1, "fork");

	if (child == 0) {
		/*
		 * Grandchild: apply pre-exec setup and exec.
		 * For legacy scripts (no RCD_USER etc.), the apply
		 * functions are no-ops since the env vars aren't set.
		 */
		apply_env_file();
		apply_umask();
		apply_nice();
		apply_cpuset();
		apply_fib();
		apply_limits();
		apply_jail();
		apply_chroot();
		apply_chdir();
		apply_credentials();
		signal_readiness();
		cleanup_rcd_env();

		execvp(argv[1], &argv[1]);
		err(1, "exec %s", argv[1]);
	}

	/* Parent: sub-reaper supervisor */
	memset(&sa, 0, sizeof(sa));
	sigemptyset(&sa.sa_mask);
	sa.sa_handler = reaper_sighandler;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);

	/* Wait for the initial process (shell or forking parent) to exit */
	while ((wpid = waitpid(child, &status, 0)) < 0) {
		if (errno != EINTR)
			break;
		if (reaper_stop) {
			reaper_kill_all();
			return (0);
		}
	}

	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		/* Command failed — kill any orphans and exit */
		reaper_kill_all();
		/* Final drain — reap anything that died during the loop */
		while (waitpid(-1, NULL, WNOHANG) > 0)
			;
		return (WIFEXITED(status) ? WEXITSTATUS(status) : 1);
	}

	/*
	 * Find the daemon PID (reparented to us after the shell exited).
	 * Give a brief window for the reparenting to complete and for
	 * the daemon to finish its own daemonization (double fork).
	 */
	{
		int tries;

		reaper_daemon_pid = -1;
		for (tries = 0; tries < 20; tries++) {
			reaper_daemon_pid = find_daemon_pid();
			if (reaper_daemon_pid > 0)
				break;
			usleep(50000);	/* 50ms, up to 1s total */
		}
	}

	if (reaper_daemon_pid <= 0) {
		/*
		 * No daemon found.  The command may have forked short-lived
		 * processes that already exited.  Reap any remaining children
		 * so they do not become zombies under rcd.
		 */
		while (waitpid(-1, NULL, WNOHANG) > 0)
			;
		return (0);
	}

	/* Set process title to show what we're supervising */
	{
		const char *svcname;

		svcname = getenv("RCD_SERVICE");
		if (svcname != NULL)
			setproctitle("reaper: %s (pid %d)", svcname,
			    reaper_daemon_pid);
		else
			setproctitle("reaper: pid %d",
			    reaper_daemon_pid);
	}

	/* Monitor the daemon until it exits or we get SIGTERM */
	for (;;) {
		wpid = waitpid(-1, &status, 0);
		if (wpid == reaper_daemon_pid) {
			/*
			 * The tracked PID exited.  This might be a
			 * double-fork daemonization where the first child
			 * exits and the real daemon is a grandchild
			 * (now reparented to us).  Check if there are
			 * remaining children before giving up.
			 */
			pid_t new_daemon;

			new_daemon = find_daemon_pid();
			if (new_daemon > 0) {
				reaper_daemon_pid = new_daemon;
				continue;
			}
			/* No more children — daemon is truly gone */
			return (WIFEXITED(status) ? WEXITSTATUS(status) : 1);
		}
		if (wpid < 0) {
			if (errno == EINTR) {
				if (reaper_stop) {
					reaper_kill_all();
					return (0);
				}
				continue;
			}
			/* No more children */
			return (0);
		}
		/* Some other child exited (e.g. session), keep waiting */
	}
}

int
main(int argc, char *argv[])
{

	if (argc < 2) {
		fprintf(stderr, "usage: rcd-exec command [args...]\n");
		return (1);
	}

	/* Sub-reaper mode for forking daemons and legacy scripts */
	if (getenv("RCD_REAPER") != NULL)
		return (reaper_main(argc, argv));

	/*
	 * Apply setup in the correct order:
	 * 1. env_file (load extra vars before anything else)
	 * 2. umask (before any file creation)
	 * 3. nice
	 * 4. cpuset
	 * 5. fib (routing table)
	 * 6. jail (before credential drop, since jail_attach requires root)
	 * 7. chroot (before credential drop)
	 * 8. chdir
	 * 9. credentials (last, since we lose privileges; includes
	 *    login_class and supplementary groups)
	 */
	apply_env_file();
	apply_umask();
	apply_nice();
	apply_cpuset();
	apply_fib();
	apply_limits();
	apply_jail();
	apply_chroot();
	apply_chdir();
	apply_credentials();

	/* Signal readiness after all setup is done */
	signal_readiness();

	/* Clean up RCD_* variables from the environment */
	unsetenv("RCD_USER");
	unsetenv("RCD_GROUP");
	unsetenv("RCD_GROUPS");
	unsetenv("RCD_UMASK");
	unsetenv("RCD_NICE");
	unsetenv("RCD_CPUSET");
	unsetenv("RCD_FIB");
	unsetenv("RCD_JAIL");
	unsetenv("RCD_CHROOT");
	unsetenv("RCD_LIMITS");
	unsetenv("RCD_LOGIN_CLASS");
	unsetenv("RCD_ENV_FILE");
	unsetenv("RCD_NOTIFY_FD");
	unsetenv("RCD_CHDIR");

	/* exec the real service */
	execvp(argv[1], &argv[1]);
	err(1, "exec %s", argv[1]);
}
