/*
 * Copyright (c) 2026 Baptiste Daroussin <bapt@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef XIO_H
#define XIO_H

#include <sys/procdesc.h>
#include <sys/wait.h>
#include <errno.h>
#include <unistd.h>

/*
 * Read exactly n bytes from fd, handling EINTR and partial reads.
 * Returns n on success, -1 on error (errno set).
 */
static inline ssize_t
xread(int fd, void *buf, size_t n)
{
	size_t done = 0;
	ssize_t ret;

	while (done < n) {
		ret = read(fd, (char *)buf + done, n - done);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			return (-1);
		}
		if (ret == 0) {
			errno = EPIPE;
			return (-1);
		}
		done += ret;
	}
	return (done);
}

/*
 * Write exactly n bytes to fd, handling EINTR and partial writes.
 * Returns n on success, -1 on error (errno set).
 */
static inline ssize_t
xwrite(int fd, const void *buf, size_t n)
{
	size_t done = 0;
	ssize_t ret;

	while (done < n) {
		ret = write(fd, (const char *)buf + done, n - done);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			return (-1);
		}
		if (ret == 0) {
			/* blocking write returning 0 means no progress possible */
			errno = EIO;
			return (-1);
		}
		done += ret;
	}
	return (done);
}

/*
 * Wait for a child process, retrying on EINTR.
 * Returns the child PID on success, -1 on error (errno set), 0 if WNOHANG
 * was set and no child was available.
 */
static inline pid_t
xwaitpid(pid_t pid, int *status, int opts)
{
	pid_t ret;

	do {
		ret = waitpid(pid, status, opts);
	} while (ret < 0 && errno == EINTR);

	return (ret);
}

/*
 * Wait on a process descriptor, retrying on EINTR.
 * Returns the child PID on success, -1 on error (errno set), 0 if WNOHANG
 * was set and no child was available.
 */
static inline pid_t
xpdwait(int fd, int *status, int opts)
{
	pid_t ret;

	do {
		ret = pdwait(fd, status, opts, NULL, NULL);
	} while (ret < 0 && errno == EINTR);

	return (ret);
}

#endif /* !XIO_H */
