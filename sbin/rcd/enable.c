/*
 * Copyright (c) 2026 Baptiste Daroussin <bapt@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Persistent enable/disable of services.  Writes UCL fragments to
 * /etc/rcd.conf.d/<service> to persist the enabled state across reboots.
 */

#include <sys/param.h>
#include <sys/stat.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ucl.h>

#include "rcd.h"

#define RCD_CONFD_DIR	"/etc/rcd.conf.d"

/*
 * Ensure the override directory exists.
 */
static int
ensure_confdir(void)
{

	if (mkdir(RCD_CONFD_DIR, 0755) != 0 && errno != EEXIST) {
		log_warn("mkdir %s: %s", RCD_CONFD_DIR, strerror(errno));
		return (-1);
	}
	return (0);
}

/*
 * Write an enable/disable override for a service.
 * Creates or updates /etc/rcd.conf.d/<service> with:
 *   enable = true;   (or false)
 *
 * Atomic write: uses mkstemp() for safe temp file creation
 * (prevents symlink races) then rename() for atomic replacement.
 */
static int
write_enable_state(const char *name, bool enable)
{
	struct ucl_parser *parser;
	ucl_object_t *top;
	char path[PATH_MAX];
	unsigned char *buf;
	int fd;

	/* Validate service name to prevent path traversal */
	if (!valid_service_name(name)) {
		log_warn("invalid service name: '%s'", name);
		return (-1);
	}

	if (ensure_confdir() != 0)
		return (-1);

	if ((size_t)snprintf(path, sizeof(path), "%s/%s",
	    RCD_CONFD_DIR, name) >= sizeof(path)) {
		log_warn("%s: service name too long", name);
		return (-1);
	}

	/* Load existing file if present, to preserve other overrides */
	top = NULL;
	parser = ucl_parser_new(UCL_PARSER_DEFAULT);
	if (ucl_parser_add_file(parser, path))
		top = ucl_parser_get_object(parser);
	ucl_parser_free(parser);

	if (top == NULL)
		top = ucl_object_typed_new(UCL_OBJECT);

	/* Set or replace the enable key */
	ucl_object_replace_key(top,
	    ucl_object_frombool(enable), "enable", 0, false);

	buf = ucl_object_emit(top, UCL_EMIT_CONFIG);
	ucl_object_unref(top);

	if (buf == NULL)
		return (-1);

	/*
	 * Atomic write using mkstemp() + rename().
	 * mkstemp() creates the file with O_EXCL | O_CREAT,
	 * preventing symlink races.  The file is created with
	 * mode 0600 and umask restrictions apply.
	 */
	{
		char tmpl[PATH_MAX];

		if ((size_t)snprintf(tmpl, sizeof(tmpl),
		    "%s.XXXXXXXX", path) >= sizeof(tmpl)) {
			free(buf);
			return (-1);
		}
		fd = mkstemp(tmpl);
		if (fd < 0) {
			log_warn("mkstemp %s: %s", tmpl, strerror(errno));
			free(buf);
			return (-1);
		}

		/* Write the content, handling partial writes and EINTR */
		if (xwrite(fd, buf, strlen((char *)buf)) < 0) {
			log_warn("write %s: %s", tmpl,
			    strerror(errno));
			close(fd);
			unlink(tmpl);
			free(buf);
			return (-1);
		}

		if (close(fd) != 0) {
			log_warn("close %s: %s", tmpl, strerror(errno));
			unlink(tmpl);
			free(buf);
			return (-1);
		}

		if (rename(tmpl, path) != 0) {
			log_warn("rename %s -> %s: %s",
			    tmpl, path, strerror(errno));
			unlink(tmpl);
			free(buf);
			return (-1);
		}
	}

	free(buf);

	log_info("%s: %s", name, enable ? "enabled" : "disabled");
	return (0);
}

int
enable_service(const char *name, const char *confdir __unused)
{

	return (write_enable_state(name, true));
}

int
disable_service(const char *name, const char *confdir __unused)
{

	return (write_enable_state(name, false));
}

int
delete_override(const char *name)
{
	char path[PATH_MAX];

	/* Validate service name to prevent path traversal */
	if (!valid_service_name(name)) {
		log_warn("invalid service name: '%s'", name);
		return (-1);
	}

	if ((size_t)snprintf(path, sizeof(path), "%s/%s",
	    RCD_CONFD_DIR, name) >= sizeof(path))
		return (-1);
	if (unlink(path) != 0 && errno != ENOENT) {
		log_warn("unlink %s: %s", path, strerror(errno));
		return (-1);
	}
	log_info("%s: override deleted", name);
	return (0);
}


