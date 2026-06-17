/*
 * Copyright (c) 2026 Baptiste Daroussin <bapt@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Logging subsystem.  Outputs to syslog and optionally stderr.
 * Also sets up stdout/stderr redirects for service processes via
 * posix_spawn file actions.
 */

#include <sys/param.h>
#include <sys/event.h>
#include <sys/sysctl.h>

#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "rcd.h"

static int log_level = LOG_INFO;
static bool log_verbose;
static bool log_console_enabled = true;
static int log_console_fd = -1;

void
log_init(int level)
{

	log_level = level;
	openlog("rcd", LOG_PID | LOG_NDELAY, LOG_DAEMON);
}

void
log_set_verbose(bool v)
{

	log_verbose = v;
}

/*
 * Duplicate stderr to a private fd for console output.
 * Must be called before daemonization, while stderr is still
 * connected to the console by init(8).
 */
void
log_console_open(void)
{

	if (log_console_fd >= 0)
		close(log_console_fd);
	log_console_fd = dup(STDERR_FILENO);
}

/*
 * Close the console fd.  Called when boot is complete.
 */
void
log_console_close(void)
{

	if (log_console_fd >= 0) {
		close(log_console_fd);
		log_console_fd = -1;
	}
}

/*
 * Write a message to the console for boot-time progress feedback.
 * Uses the fd saved by log_console_open(), which survives
 * daemonization (fork + setsid).
 *
 * The message is written only if console output is enabled
 * (controlled by quiet_boot in rcd.conf).
 */
void
log_console(const char *fmt, ...)
{
	va_list ap;
	char buf[256];
	ssize_t len;

	if (!log_console_enabled || log_console_fd < 0)
		return;

	va_start(ap, fmt);
	len = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	if (len > 0) {
		if ((size_t)len >= sizeof(buf))
			len = sizeof(buf) - 1;
		buf[len] = '\n';
		xwrite(log_console_fd, buf, (size_t)(len + 1));
	}
}

void
log_console_set_enabled(bool enabled)
{

	log_console_enabled = enabled;
}

void
log_info(const char *fmt, ...)
{
	va_list ap;

	if (log_level < LOG_INFO)
		return;
	va_start(ap, fmt);
	vsyslog(LOG_INFO, fmt, ap);
	va_end(ap);
	if (log_verbose) {
		va_start(ap, fmt);
		vfprintf(stderr, fmt, ap);
		fputc('\n', stderr);
		va_end(ap);
	}
}

void
log_warn(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsyslog(LOG_WARNING, fmt, ap);
	va_end(ap);
	if (log_verbose) {
		va_start(ap, fmt);
		fprintf(stderr, "WARNING: ");
		vfprintf(stderr, fmt, ap);
		fputc('\n', stderr);
		va_end(ap);
	}
}

void
log_err(int eval, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsyslog(LOG_ERR, fmt, ap);
	va_end(ap);
	if (log_verbose) {
		va_start(ap, fmt);
		fprintf(stderr, "ERROR: ");
		vfprintf(stderr, fmt, ap);
		fputc('\n', stderr);
		va_end(ap);
	}
	exit(eval);
}

void
log_debug(const char *fmt, ...)
{
	va_list ap;

	if (log_level < LOG_DEBUG)
		return;
	va_start(ap, fmt);
	vsyslog(LOG_DEBUG, fmt, ap);
	va_end(ap);
	if (log_verbose) {
		va_start(ap, fmt);
		fprintf(stderr, "DEBUG: ");
		vfprintf(stderr, fmt, ap);
		fputc('\n', stderr);
		va_end(ap);
	}
}

/*
 * Write a boottrace message to kern.boottrace.log.
 * This allows measuring boot performance with boottrace(4).
 */
void
boottrace(const char *fmt, ...)
{
	va_list ap;
	char msg[256];

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);

	sysctlbyname("kern.boottrace.log", NULL, NULL, msg,
	    strlen(msg) + 1);
}

/*
 * Parse a "syslog:facility.level" spec into syslog priority.
 */
static int
parse_syslog_priority(const char *spec)
{
	const char *dot;
	int facility, level;

	/* Skip "syslog:" prefix */
	if (strncmp(spec, "syslog:", 7) == 0)
		spec += 7;

	facility = LOG_DAEMON;
	level = LOG_INFO;

	dot = strchr(spec, '.');
	if (dot != NULL) {
		char fac[32];
		size_t len = (size_t)(dot - spec);

		if (len >= sizeof(fac))
			len = sizeof(fac) - 1;
		memcpy(fac, spec, len);
		fac[len] = '\0';

		if (strcmp(fac, "daemon") == 0)
			facility = LOG_DAEMON;
		else if (strcmp(fac, "local0") == 0)
			facility = LOG_LOCAL0;
		else if (strcmp(fac, "local1") == 0)
			facility = LOG_LOCAL1;
		else if (strcmp(fac, "user") == 0)
			facility = LOG_USER;

		spec = dot + 1;
	}

	if (strcmp(spec, "err") == 0 || strcmp(spec, "error") == 0)
		level = LOG_ERR;
	else if (strcmp(spec, "warning") == 0 || strcmp(spec, "warn") == 0)
		level = LOG_WARNING;
	else if (strcmp(spec, "debug") == 0)
		level = LOG_DEBUG;
	else if (strcmp(spec, "notice") == 0)
		level = LOG_NOTICE;

	return (facility | level);
}

/*
 * Create a pipe for syslog-forwarded output.  The write end is dup2'd
 * into the child's stdout or stderr via posix_spawn file actions.
 * Returns the read end fd.  The write end fd is returned via *wfdp
 * and MUST be closed by the caller AFTER posix_spawn (the child needs
 * it in its fd table at fork time).
 */
static int
create_syslog_pipe(posix_spawn_file_actions_t *fa, int target_fd,
    int *wfdp)
{
	int pipefd[2];

	if (pipe(pipefd) != 0)
		return (-1);

	/* In the child: dup2 write end to target, close both originals */
	posix_spawn_file_actions_adddup2(fa, pipefd[1], target_fd);
	posix_spawn_file_actions_addclose(fa, pipefd[1]);
	posix_spawn_file_actions_addclose(fa, pipefd[0]);

	/*
	 * Return read end to caller; write end must stay open until
	 * after posix_spawn so the child inherits it.
	 */
	*wfdp = pipefd[1];
	return (pipefd[0]);
}

/*
 * Set up posix_spawn file actions to redirect the child's stdout and stderr
 * based on the unit's logging configuration.
 *
 * Supported targets:
 *   "syslog:facility.level"  - pipe to syslog via rcd's kqueue
 *   "file:/path"             - redirect to file
 *   "null"                   - redirect to /dev/null
 *
 * For syslog targets, we create a pipe.  The write end is dup2'd into the
 * child's fd.  The read end stays open in rcd and should be registered in
 * kqueue for line-buffered forwarding to syslog.  The pipe fd is stored
 * in the unit's log_conf for the caller to retrieve.
 */
int
log_setup_fds(struct unit *u, posix_spawn_file_actions_t *fa)
{
	const char *out, *err;

	out = u->u_log.lc_stdout;
	err = u->u_log.lc_stderr;

	/* Default: redirect to /dev/null if no logging configured */
	if (out == NULL && err == NULL) {
		posix_spawn_file_actions_addopen(fa, STDOUT_FILENO,
		    "/dev/null", O_WRONLY, 0);
		posix_spawn_file_actions_addopen(fa, STDERR_FILENO,
		    "/dev/null", O_WRONLY, 0);
		return (0);
	}

	/* stdout */
	if (out != NULL) {
		if (strncmp(out, "file:", 5) == 0) {
			posix_spawn_file_actions_addopen(fa, STDOUT_FILENO,
			    out + 5, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW, 0644);
		} else if (strcmp(out, "null") == 0) {
			posix_spawn_file_actions_addopen(fa, STDOUT_FILENO,
			    "/dev/null", O_WRONLY, 0);
		} else if (strncmp(out, "syslog:", 7) == 0) {
			int wfd;
			int rfd = create_syslog_pipe(fa, STDOUT_FILENO,
			    &wfd);
			if (rfd >= 0) {
				u->u_log.lc_stdout_pipefd = rfd;
				u->u_log.lc_stdout_wfd = wfd;
				u->u_log.lc_stdout_priority =
				    parse_syslog_priority(out);
			}
		}
	}

	/* stderr */
	if (err != NULL) {
		if (strncmp(err, "file:", 5) == 0) {
			posix_spawn_file_actions_addopen(fa, STDERR_FILENO,
			    err + 5, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW, 0644);
		} else if (strcmp(err, "null") == 0) {
			posix_spawn_file_actions_addopen(fa, STDERR_FILENO,
			    "/dev/null", O_WRONLY, 0);
		} else if (strncmp(err, "syslog:", 7) == 0) {
			int wfd;
			int rfd = create_syslog_pipe(fa, STDERR_FILENO,
			    &wfd);
			if (rfd >= 0) {
				u->u_log.lc_stderr_pipefd = rfd;
				u->u_log.lc_stderr_wfd = wfd;
				u->u_log.lc_stderr_priority =
				    parse_syslog_priority(err);
			}
		}
	} else {
		/* Default: stderr follows stdout */
		posix_spawn_file_actions_adddup2(fa, STDOUT_FILENO,
		    STDERR_FILENO);
	}

	return (0);
}

/*
 * Register syslog pipe read-ends in kqueue with EVFILT_READ.
 * Called after posix_spawn, once lc_stdout_wfd/lc_stderr_wfd
 * are closed in the parent and only the service side holds
 * the write end.
 */
int
log_register_pipe_fds(struct unit *u, int kq)
{
	struct kevent kev;

	if (u->u_log.lc_stdout_pipefd >= 0) {
		EV_SET(&kev, u->u_log.lc_stdout_pipefd, EVFILT_READ,
		    EV_ADD, 0, 0, u);
		if (kevent(kq, &kev, 1, NULL, 0, NULL) < 0) {
			log_warn("%s: kevent stdout pipe: %s",
			    u->u_name, strerror(errno));
			return (-1);
		}
	}

	if (u->u_log.lc_stderr_pipefd >= 0) {
		EV_SET(&kev, u->u_log.lc_stderr_pipefd, EVFILT_READ,
		    EV_ADD, 0, 0, u);
		if (kevent(kq, &kev, 1, NULL, 0, NULL) < 0) {
			log_warn("%s: kevent stderr pipe: %s",
			    u->u_name, strerror(errno));
			return (-1);
		}
	}

	return (0);
}

/*
 * Forward data from a syslog pipe to syslog(3).
 *
 * Reads whatever is available and processes complete lines
 * (delimited by '\n').  Partial data at the end is buffered
 * in the unit's residual buffer and prepended to the next
 * read.  On EOF (read returns 0), any remaining residual is
 * flushed.
 */
void
log_handle_pipe_event(struct unit *u, int fd)
{
	int priority;
	char *resid;
	size_t *resid_len;
	char buf[4096];
	ssize_t nread;
	bool is_eof;

	/* Determine which fd triggered and pick the right state */
	if (fd == u->u_log.lc_stdout_pipefd) {
		priority = u->u_log.lc_stdout_priority;
		resid = u->u_log.lc_stdout_resid;
		resid_len = &u->u_log.lc_stdout_resid_len;
	} else if (fd == u->u_log.lc_stderr_pipefd) {
		priority = u->u_log.lc_stderr_priority;
		resid = u->u_log.lc_stderr_resid;
		resid_len = &u->u_log.lc_stderr_resid_len;
	} else {
		return;
	}

	nread = read(fd, buf, sizeof(buf));
	if (nread < 0) {
		if (errno == EINTR)
			return;
		/* EOF or error — flush residual and close */
		is_eof = true;
		nread = 0;
	} else if (nread == 0) {
		is_eof = true;
	} else {
		is_eof = false;
	}

	/*
	 * Process data: prepend any residual from the previous read,
	 * then syslog each complete line.
	 */
	{
		char *line_start, *nl;
		size_t total, consumed;

		/* Build a contiguous buffer: residual + new data */
		total = *resid_len + (size_t)nread;
		char combined[total + 1];

		memcpy(combined, resid, *resid_len);
		if (nread > 0)
			memcpy(combined + *resid_len, buf, (size_t)nread);
		combined[total] = '\0';

		line_start = combined;
		consumed = 0;

		while ((nl = memchr(line_start, '\n',
		    total - consumed)) != NULL) {
			*nl = '\0';
			if (line_start != nl)
				syslog(priority, "%s", line_start);
			consumed += (size_t)(nl - line_start + 1);
			line_start = nl + 1;
		}

		/* Keep remaining data (no newline) as residual */
		*resid_len = total - consumed;
		if (*resid_len > 0)
			memcpy(resid, line_start, *resid_len);
		else
			*resid_len = 0;
	}

	/*
	 * On EOF or error, flush any remaining residual data
	 * and close the pipe fd.
	 */
	if (is_eof) {
		if (*resid_len > 0) {
			syslog(priority, "%s", resid);
			*resid_len = 0;
		}
		close(fd);
		if (fd == u->u_log.lc_stdout_pipefd)
			u->u_log.lc_stdout_pipefd = -1;
		else
			u->u_log.lc_stderr_pipefd = -1;
	}
}

/*
 * Flush any remaining pipe data and close pipe fds.
 * Called on service exit (in proc_handle_exit) to ensure
 * all output is forwarded before the unit is cleaned up.
 */
void
log_flush_pipes(struct unit *u)
{

	if (u->u_log.lc_stdout_pipefd >= 0) {
		log_handle_pipe_event(u, u->u_log.lc_stdout_pipefd);
		/* fd is closed by log_handle_pipe_event on EOF */
	}
	if (u->u_log.lc_stderr_pipefd >= 0) {
		log_handle_pipe_event(u, u->u_log.lc_stderr_pipefd);
		/* fd is closed by log_handle_pipe_event on EOF */
	}
}
