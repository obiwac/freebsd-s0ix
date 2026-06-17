/*
 * Copyright (c) 2026 Baptiste Daroussin <bapt@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Legacy rc.d script compatibility layer.
 *
 * Scans directories for rc.d scripts, parses their PROVIDE/REQUIRE/BEFORE/
 * KEYWORD headers (same format as rcorder(8)), checks rc.conf to determine
 * if the service is enabled, and wraps each script as a virtual unit of
 * type UNIT_LEGACY in the dependency graph.
 */

#include <sys/param.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <paths.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rcd.h"

/*
 * Parse a single header line of the form:
 *   # PROVIDE: name1 name2 ...
 *   # REQUIRE: name1 name2 ...
 *   # BEFORE:  name1 name2 ...
 *   # KEYWORD: kw1 kw2 ...
 *
 * Also accepts REQUIRES/KEYWORDS (plural) for compatibility with rcorder.
 */
static int
parse_header_line(const char *line, const char *tag, charv_t *outp)
{
	const char *p;
	char *buf, *tok;
	size_t taglen, prev_len;

	taglen = strlen(tag);

	/* Skip leading whitespace and '#' */
	p = line;
	while (*p == ' ' || *p == '\t')
		p++;
	if (*p != '#')
		return (0);
	p++;
	while (*p == ' ' || *p == '\t')
		p++;

	/* Match tag (case insensitive) */
	if (strncasecmp(p, tag, taglen) != 0)
		return (0);
	p += taglen;

	/* Accept optional trailing 'S' (REQUIRES, KEYWORDS) */
	if (*p == 's' || *p == 'S')
		p++;

	/* Must be followed by ':' */
	if (*p != ':')
		return (0);
	p++;

	/* Parse space-separated names */
	buf = xstrdup(p);

	prev_len = outp->len;
	for (tok = strtok(buf, " \t\r\n"); tok != NULL;
	    tok = strtok(NULL, " \t\r\n"))
		vec_push(outp, xstrdup(tok));
	free(buf);

	return (outp->len > prev_len ? 1 : 0);
}

static bool
compat_has_keyword(const struct unit *u, const char *kw)
{

	vec_foreach(u->u_keyword, i) {
		if (strcasecmp(u->u_keyword.d[i], kw) == 0)
			return (true);
	}
	return (false);
}

/*
 * Parse PROVIDE/REQUIRE/BEFORE/KEYWORD headers from an rc.d script.
 */
int
compat_parse_headers(const char *path, struct unit *u)
{
	FILE *fp;
	char *line;
	size_t linecap;
	ssize_t linelen;
	int found_any;
	bool has_command, has_pidfile, has_start_cmd, has_rcvar, has_code;

	fp = fopen(path, "re");
	if (fp == NULL) {
		log_warn("open %s: %s", path, strerror(errno));
		return (-1);
	}

	line = NULL;
	linecap = 0;
	found_any = 0;
	has_command = false;
	has_pidfile = false;
	has_start_cmd = false;
	has_rcvar = false;
	has_code = false;

	while ((linelen = getline(&line, &linecap, fp)) > 0) {
		const char *p = line;

		while (*p == ' ' || *p == '\t')
			p++;

		/* Comment lines: parse PROVIDE/REQUIRE/BEFORE/KEYWORD */
		if (*p == '#') {
			parse_header_line(line, "PROVIDE",
			    &u->u_provide);
			parse_header_line(line, "REQUIRE",
			    &u->u_require);
			parse_header_line(line, "BEFORE",
			    &u->u_before);
			if (parse_header_line(line, "KEYWORD",
			    &u->u_keyword) > 0)
				found_any++;
			if (u->u_provide.len > 0 || u->u_require.len > 0 ||
			    u->u_before.len > 0)
				found_any = 1;
			continue;
		}

		/* Blank lines */
		if (*p == '\n' || *p == '\0')
			continue;

		/* Non-comment, non-blank: executable code */
		has_code = true;

		/* Detect type-determining variables */
		if (strncmp(p, "pidfile=", 8) == 0)
			has_pidfile = true;
		else if (strncmp(p, "command=", 8) == 0)
			has_command = true;
		else if (strncmp(p, "start_cmd=", 10) == 0)
			has_start_cmd = true;
		else if (strncmp(p, "rcvar=", 6) == 0)
			has_rcvar = true;
	}

	free(line);
	fclose(fp);

	/*
	 * Classify the script type:
	 *   pidfile= or command= (without start_cmd=) → daemon
	 *   only comments/blank lines (no code)        → barrier
	 *   otherwise                                  → oneshot
	 */
	if (has_pidfile || (has_command && !has_start_cmd))
		u->u_type = UNIT_LEGACY_FORKING;
	else if (!has_code)
		u->u_type = UNIT_BARRIER;
	u->u_has_rcvar = has_rcvar;

	return (found_any > 0 ? 0 : -1);
}

/*
 * Load all rc.conf variables by sourcing rc.subr in a shell.
 *
 * This is the safe approach: we let /bin/sh handle all the
 * complexity of rc.conf parsing (variable expansion, conditionals,
 * includes, rc.conf.d/ directories, etc.) and just read the result.
 *
 * Runs once at startup; the result is cached in cfg->cfg_rcvars.
 */
int
compat_load_rcvars(struct rcd_config *cfg)
{
	FILE *fp;
	char *line;
	size_t linecap;
	ssize_t linelen;
	int pipefd[2];
	pid_t pid;
	int status, error;
	char *argv[4];
	posix_spawn_file_actions_t fa;

	cfg->cfg_rcvars = hash_new();

	if (pipe(pipefd) != 0)
		return (-1);

	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDOUT_FILENO);
	posix_spawn_file_actions_addclose(&fa, pipefd[0]);
	posix_spawn_file_actions_addclose(&fa, pipefd[1]);

	argv[0] = __DECONST(char *, _PATH_BSHELL);
	argv[1] = __DECONST(char *, "-c");
	argv[2] = __DECONST(char *,
	    ". /etc/rc.subr;"
	    "load_rc_config;"
	    "set");
	argv[3] = NULL;

	error = posix_spawn(&pid, _PATH_BSHELL, &fa, NULL, argv, NULL);
	posix_spawn_file_actions_destroy(&fa);
	close(pipefd[1]);

	if (error != 0) {
		close(pipefd[0]);
		return (-1);
	}

	fp = fdopen(pipefd[0], "r");
	if (fp == NULL) {
		close(pipefd[0]);
		xwaitpid(pid, &status, 0);
		return (-1);
	}

	line = NULL;
	linecap = 0;
	while ((linelen = getline(&line, &linecap, fp)) > 0) {
		char *eq, *val, *end;

		/* Only keep *_enable variables */
		eq = strchr(line, '=');
		if (eq == NULL)
			continue;
		*eq = '\0';

		if (strlen(line) < 8 ||
		    strcmp(line + strlen(line) - 7, "_enable") != 0)
			continue;

		val = eq + 1;
		/* Strip quotes and trailing whitespace */
		if (val[0] == '$' && val[1] == '\'')
			val += 2;
		else
			while (*val == '\'' || *val == '"')
				val++;
		end = val + strlen(val) - 1;
		while (end > val && (*end == '\n' || *end == '\r' ||
		    *end == '\'' || *end == '"'))
			*end-- = '\0';

		hash_add(cfg->cfg_rcvars, line, xstrdup(val), free);
	}

	free(line);
	fclose(fp);

	if (xwaitpid(pid, &status, 0) < 0) {
		log_warn("waitpid rc.subr: %s", strerror(errno));
		return (-1);
	}
	if (WIFSIGNALED(status)) {
		log_warn("rc.subr shell killed by signal %d",
		    WTERMSIG(status));
		return (-1);
	}
	if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
		log_warn("rc.subr shell exited with status %d",
		    WEXITSTATUS(status));
	}

	return (0);
}

/*
 * Check if a legacy service is enabled by looking up ${name}_enable
 * in the cached rc.conf variables.
 */
bool
compat_is_enabled(const char *name, struct rcd_config *cfg)
{
	char *pattern;
	const char *val;

	xasprintf(&pattern, "%s_enable", name);

	val = (const char *)hash_get_value(cfg->cfg_rcvars, pattern);
	free(pattern);
	if (val != NULL) {
		if (strcasecmp(val, "YES") == 0 ||
		    strcasecmp(val, "TRUE") == 0 ||
		    strcasecmp(val, "ON") == 0 ||
		    strcmp(val, "1") == 0)
			return (true);
		return (false);
	}
	return (false);	/* Default: not enabled */
}

/*
 * Scan a directory for rc.d scripts and create legacy unit wrappers.
 */
int
compat_scan(struct rcd_ctx *ctx, const char *dirpath)
{
	DIR *dp;
	struct dirent *de;
	struct unit *u;
	char path[PATH_MAX];

	dp = opendir(dirpath);
	if (dp == NULL) {
		if (errno == ENOENT)
			return (0);
		log_warn("opendir %s: %s", dirpath, strerror(errno));
		return (-1);
	}

	while ((de = readdir(dp)) != NULL) {
		struct stat sb;

		/* Skip hidden files and common non-script files */
		if (de->d_name[0] == '.')
			continue;

		if (snprintf(path, sizeof(path), "%s/%s",
		    dirpath, de->d_name) >= (int)sizeof(path))
			continue;

		/* Skip non-regular files (directories, symlinks, etc.) */
		if (stat(path, &sb) != 0 || !S_ISREG(sb.st_mode))
			continue;

		/*
		 * Check if this provision is already satisfied by a
		 * native unit.  If so, the native unit takes precedence.
		 */

		u = unit_alloc();
		if (u == NULL)
			continue;

		u->u_type = UNIT_LEGACY;
		u->u_path = xstrdup(path);

		/*
		 * For legacy scripts, set stop_command to call the
		 * script with "stop" argument.  This ensures proper
		 * teardown instead of just sending SIGTERM.
		 */
		xasprintf(&u->u_stop_command, "%s %s stop", _PATH_BSHELL, path);

		/* Parse headers from the script */
		if (compat_parse_headers(path, u) != 0) {
			unit_free(u);
			continue;
		}

		/* KEYWORD: NORCD — script explicitly opts out of rcd */
		if (compat_has_keyword(u, "NORCD")) {
			log_debug("skipping %s: NORCD keyword", de->d_name);
			unit_free(u);
			continue;
		}

		/* Use the first provision as the service name */
		if (u->u_provide.len > 0)
			u->u_name = xstrdup(u->u_provide.d[0]);
		else
			u->u_name = xstrdup(de->d_name);

		/* Check if already provided by a native unit */
		if (depgraph_find(&ctx->ctx_graph, u->u_name) != NULL) {
			log_debug("skipping legacy %s: overridden by native unit",
			    u->u_name);
			unit_free(u);
			continue;
		}

		/*
		 * Check rc.conf for enabled state.
		 * Scripts without rcvar= are always enabled (like rc.subr).
		 * Barriers are also always enabled.
		 */
		if (u->u_type == UNIT_BARRIER)
			u->u_enabled = true;
		else if (u->u_has_rcvar)
			u->u_enabled = compat_is_enabled(u->u_name,
			    &ctx->ctx_config);
		else
			u->u_enabled = true;

		/* Check keywords for filtering */
		vec_foreach(u->u_keyword, i) {
			if (strcmp(u->u_keyword.d[i],
			    "nostart") == 0)
				u->u_nostart = true;
			if (strcmp(u->u_keyword.d[i],
			    "firstboot") == 0)
				u->u_boot_only = true;
			if (strcmp(u->u_keyword.d[i],
			    "nojail") == 0)
				u->u_nojail = true;
			if (strcmp(u->u_keyword.d[i],
			    "nojailvnet") == 0)
				u->u_nojailvnet = true;
			if (strcmp(u->u_keyword.d[i],
			    "resume") == 0)
				u->u_resume = true;
		}

		depgraph_add(&ctx->ctx_graph, u);
		log_debug("loaded legacy unit: %s (%s)", u->u_name, path);
	}

	closedir(dp);
	return (0);
}


