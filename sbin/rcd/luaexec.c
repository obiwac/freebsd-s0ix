/*
 * Copyright (c) 2026 Baptiste Daroussin <bapt@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Embedded Lua interpreter for rcd(8).
 *
 * Allows unit hooks (start_precmd, stop_postcmd, etc.) and oneshot
 * exec blocks to be written in Lua directly in the UCL unit file.
 *
 * The Lua is configured with posix and ucl module has built in,
 * and package.path/cpath point to /usr/share/flua and /usr/lib/flua
 * for dynamic modules (lfs, jail, hash, etc.).  Dynamic modules
 * require /usr being mounted (i.e., FILESYSTEMS dependency).
 *
 * Hook strings starting with "lua:" are evaluated here.
 * All others are passed to /bin/sh -c as before.
 */

#include <sys/param.h>
#include <sys/linker.h>
#include <sys/module.h>
#include <sys/sysctl.h>
#include <sys/wait.h>

#include <errno.h>
#include <kenv.h>
#include <paths.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
#include "lposix.h"
#include "lua_ucl.h"

#include "rcd.h"

extern char **environ;

#define FLUA_LUA_PATH \
	"/usr/share/flua/?.lua;" \
	"/usr/share/flua/?/init.lua;" \
	"/usr/lib/flua/?.lua;" \
	"/usr/lib/flua/?/init.lua"

#define FLUA_C_PATH \
	"/usr/lib/flua/?.so;" \
	"/usr/lib/flua/loadall.so"

static lua_State *rcd_lua_state;

/*
 * Safe os.execute replacement using posix_spawn instead of system(3).
 * Lua signature: ok, status = os.execute(command)
 * Returns: true/nil, "exit"/"signal", exit_code
 */
static int
safe_os_execute(lua_State *L)
{
	const char *cmd;
	pid_t pid;
	int status, error;
	char *argv[4];

	cmd = luaL_optstring(L, 1, NULL);
	if (cmd == NULL) {
		/* os.execute() with no args: return true (shell available) */
		lua_pushboolean(L, 1);
		return (1);
	}

	argv[0] = __DECONST(char *, _PATH_BSHELL);
	argv[1] = __DECONST(char *, "-c");
	argv[2] = __DECONST(char *, cmd);
	argv[3] = NULL;

	error = posix_spawn(&pid, _PATH_BSHELL, NULL, NULL, argv, environ);
	if (error != 0) {
		lua_pushnil(L);
		lua_pushstring(L, "exit");
		lua_pushinteger(L, 127);
		return (3);
	}

	if (xwaitpid(pid, &status, 0) < 0) {
		lua_pushnil(L);
		lua_pushstring(L, "exit");
		lua_pushinteger(L, 127);
		return (3);
	}

	if (WIFEXITED(status)) {
		int code = WEXITSTATUS(status);

		lua_pushboolean(L, code == 0);
		lua_pushstring(L, "exit");
		lua_pushinteger(L, code);
	} else if (WIFSIGNALED(status)) {
		lua_pushnil(L);
		lua_pushstring(L, "signal");
		lua_pushinteger(L, WTERMSIG(status));
	} else {
		lua_pushnil(L);
		lua_pushstring(L, "exit");
		lua_pushinteger(L, -1);
	}
	return (3);
}

/*
 * rcd.sysctl(name [, value]) — read or write a sysctl.
 *   rcd.sysctl("kern.hostname")         → returns string or nil, err
 *   rcd.sysctl("hw.usb.template", "-1") → sets and returns true or nil, err
 */
static int
lua_rcd_sysctl(lua_State *L)
{
	const char *name;
	char buf[1024];
	size_t len;

	name = luaL_checkstring(L, 1);

	if (lua_gettop(L) >= 2) {
		/* Write mode */
		const char *val = luaL_checkstring(L, 2);

		if (sysctlbyname(name, NULL, NULL, val,
		    strlen(val)) != 0) {
			lua_pushnil(L);
			lua_pushstring(L, strerror(errno));
			return (2);
		}
		lua_pushboolean(L, 1);
		return (1);
	}

	/* Read mode */
	len = sizeof(buf) - 1;
	if (sysctlbyname(name, buf, &len, NULL, 0) != 0) {
		lua_pushnil(L);
		lua_pushstring(L, strerror(errno));
		return (2);
	}
	buf[len] = '\0';
	lua_pushstring(L, buf);
	return (1);
}

/*
 * rcd.kenv(name) — read a kernel environment variable.
 */
static int
lua_rcd_kenv(lua_State *L)
{
	const char *name;
	char buf[1024];

	name = luaL_checkstring(L, 1);
	if (kenv(KENV_GET, name, buf, sizeof(buf)) < 0) {
		lua_pushnil(L);
		lua_pushstring(L, strerror(errno));
		return (2);
	}
	lua_pushstring(L, buf);
	return (1);
}

/*
 * rcd.log(level, msg) — log via syslog.
 * level: "info", "warn", "debug", "err"
 */
static int
lua_rcd_log(lua_State *L)
{
	const char *level, *msg;

	level = luaL_checkstring(L, 1);
	msg = luaL_checkstring(L, 2);

	if (strcmp(level, "info") == 0)
		log_info("%s", msg);
	else if (strcmp(level, "warn") == 0)
		log_warn("%s", msg);
	else if (strcmp(level, "debug") == 0)
		log_debug("%s", msg);
	else if (strcmp(level, "err") == 0)
		log_warn("%s", msg);
	else
		log_info("%s", msg);

	return (0);
}

/*
 * rcd.sleep(seconds) — sleep without forking.
 * Accepts fractional seconds: rcd.sleep(0.5)
 */
static int
lua_rcd_sleep(lua_State *L)
{
	double secs;
	struct timespec ts;

	secs = luaL_checknumber(L, 1);
	ts.tv_sec = (time_t)secs;
	ts.tv_nsec = (long)((secs - ts.tv_sec) * 1000000000L);
	while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
		;
	return (0);
}

/*
 * rcd.exec_output(cmd) — run a command and return its stdout as string.
 * Safe replacement for io.popen using posix_spawn + pipe.
 * Returns: output_string or nil, errmsg
 */
static int
lua_rcd_exec_output(lua_State *L)
{
	const char *cmd;
	pid_t pid;
	int pipefd[2], status, error;
	char *argv[4];
	char buf[4096];
	ssize_t n;
	luaL_Buffer b;

	cmd = luaL_checkstring(L, 1);

	if (pipe(pipefd) != 0) {
		lua_pushnil(L);
		lua_pushstring(L, strerror(errno));
		return (2);
	}

	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDOUT_FILENO);
	posix_spawn_file_actions_addclose(&fa, pipefd[0]);
	posix_spawn_file_actions_addclose(&fa, pipefd[1]);

	argv[0] = __DECONST(char *, _PATH_BSHELL);
	argv[1] = __DECONST(char *, "-c");
	argv[2] = __DECONST(char *, cmd);
	argv[3] = NULL;

	error = posix_spawn(&pid, _PATH_BSHELL, &fa, NULL, argv, environ);
	posix_spawn_file_actions_destroy(&fa);
	close(pipefd[1]);

	if (error != 0) {
		close(pipefd[0]);
		lua_pushnil(L);
		lua_pushstring(L, strerror(error));
		return (2);
	}

	/* Read stdout from the child */
	luaL_buffinit(L, &b);
	for (;;) {
		n = read(pipefd[0], buf, sizeof(buf));
		if (n > 0) {
			luaL_addlstring(&b, buf, n);
		} else if (n < 0 && errno == EINTR) {
			continue;
		} else {
			break;
		}
	}
	close(pipefd[0]);

	xwaitpid(pid, &status, 0);
	luaL_pushresult(&b);

	/* Strip trailing newline */
	{
		size_t len;
		const char *s = lua_tolstring(L, -1, &len);
		if (len > 0 && s[len - 1] == '\n') {
			lua_pushlstring(L, s, len - 1);
			lua_remove(L, -2);
		}
	}

	return (1);
}

/*
 * rcd.symlink(target, path) — create a symlink.
 * Returns true or nil, errmsg.
 */
static int
lua_rcd_symlink(lua_State *L)
{
	const char *target, *path;

	target = luaL_checkstring(L, 1);
	path = luaL_checkstring(L, 2);

	/* Remove existing entry if present */
	unlink(path);

	if (symlink(target, path) != 0) {
		lua_pushnil(L);
		lua_pushstring(L, strerror(errno));
		return (2);
	}
	lua_pushboolean(L, 1);
	return (1);
}

/*
 * rcd.kldload(module) — load a kernel module.
 * Returns true if loaded (or already present), nil + errmsg on failure.
 */
static int
lua_rcd_kldload(lua_State *L)
{
	const char *mod;

	mod = luaL_checkstring(L, 1);

	/* Already loaded? */
	if (modfind(mod) != -1) {
		lua_pushboolean(L, 1);
		return (1);
	}

	if (kldload(mod) < 0 && errno != EEXIST) {
		lua_pushnil(L);
		lua_pushstring(L, strerror(errno));
		return (2);
	}
	lua_pushboolean(L, 1);
	return (1);
}

/*
 * rcd.kldstat(module) — check if a kernel module is loaded.
 * Returns true if loaded, false otherwise.
 */
static int
lua_rcd_kldstat(lua_State *L)
{
	const char *mod;

	mod = luaL_checkstring(L, 1);
	lua_pushboolean(L, modfind(mod) != -1);
	return (1);
}

/*
 * rcd.exec_stdin(cmd, data) — run a command feeding data to its stdin.
 * Returns true/false + exit code, like os.execute.
 */
static int
lua_rcd_exec_stdin(lua_State *L)
{
	const char *cmd, *data;
	size_t datalen;
	pid_t pid;
	int pipefd[2], status, error;
	char *argv[4];
	posix_spawn_file_actions_t fa;

	cmd = luaL_checkstring(L, 1);
	data = luaL_checklstring(L, 2, &datalen);

	if (pipe(pipefd) != 0) {
		lua_pushnil(L);
		lua_pushstring(L, strerror(errno));
		return (2);
	}

	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, pipefd[0], STDIN_FILENO);
	posix_spawn_file_actions_addclose(&fa, pipefd[0]);
	posix_spawn_file_actions_addclose(&fa, pipefd[1]);

	argv[0] = __DECONST(char *, _PATH_BSHELL);
	argv[1] = __DECONST(char *, "-c");
	argv[2] = __DECONST(char *, cmd);
	argv[3] = NULL;

	error = posix_spawn(&pid, _PATH_BSHELL, &fa, NULL, argv, environ);
	posix_spawn_file_actions_destroy(&fa);
	close(pipefd[0]);

	if (error != 0) {
		close(pipefd[1]);
		lua_pushnil(L);
		lua_pushstring(L, strerror(error));
		return (2);
	}

	/* Write data to child's stdin, then close to signal EOF */
	if (xwrite(pipefd[1], data, datalen) < 0) {
		if (errno != EPIPE)  /* EPIPE = child closed stdin */
			log_warn("write to exec_stdin pipe: %s",
			    strerror(errno));
	}
	close(pipefd[1]);

	xwaitpid(pid, &status, 0);

	if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		lua_pushboolean(L, 1);
		return (1);
	}
	lua_pushnil(L);
	lua_pushstring(L, "command failed");
	return (2);
}

static const luaL_Reg rcd_lib[] = {
	{ "sysctl",	lua_rcd_sysctl },
	{ "kenv",	lua_rcd_kenv },
	{ "log",	lua_rcd_log },
	{ "exec_output", lua_rcd_exec_output },
	{ "exec_stdin",	lua_rcd_exec_stdin },
	{ "sleep",	lua_rcd_sleep },
	{ "symlink",	lua_rcd_symlink },
	{ "kldload",	lua_rcd_kldload },
	{ "kldstat",	lua_rcd_kldstat },
	{ NULL, NULL }
};

static int
luaopen_rcd(lua_State *L)
{

	luaL_newlib(L, rcd_lib);
	return (1);
}

/*
 * Initialize the shared Lua state.  Called once at startup.
 */
void
lua_init(void)
{
	lua_State *L;

	L = luaL_newstate();
	if (L == NULL) {
		log_warn("luaL_newstate failed");
		return;
	}

	luaL_openlibs(L);

	/*
	 * Replace os.execute with our safe posix_spawn version.
	 * Remove io.popen entirely — it uses popen(3) which is
	 * unsafe in a signal-modified context.
	 */
	lua_getglobal(L, "os");
	lua_pushcfunction(L, safe_os_execute);
	lua_setfield(L, -2, "execute");
	lua_pop(L, 1);

	lua_getglobal(L, "io");
	lua_pushnil(L);
	lua_setfield(L, -2, "popen");
	lua_pop(L, 1);

	/* Register built-in modules */
	luaL_requiref(L, "posix", luaopen_posix, 1);
	lua_pop(L, 1);
	luaL_requiref(L, "ucl", luaopen_ucl, 1);
	lua_pop(L, 1);
	luaL_requiref(L, "rcd", luaopen_rcd, 1);
	lua_pop(L, 1);

	/* Set module paths matching flua */
	lua_getglobal(L, "package");
	lua_pushstring(L, FLUA_LUA_PATH);
	lua_setfield(L, -2, "path");
	lua_pushstring(L, FLUA_C_PATH);
	lua_setfield(L, -2, "cpath");
	lua_pop(L, 1);

	rcd_lua_state = L;
}

/*
 * Helper: parse a UCL string and set it as a field on the rcd table.
 * If the string is NULL or empty, sets an empty table.
 */
static void
lua_set_ucl_field(lua_State *L, const char *field, const char *ucl_str)
{

	if (ucl_str != NULL && ucl_str[0] != '\0') {
		char buf[256];

		/*
		 * Push the UCL string as a Lua global _rcd_tmp,
		 * then parse it from Lua.  This avoids C-level
		 * escaping of the UCL content.
		 */
		lua_pushstring(L, ucl_str);
		lua_setglobal(L, "_rcd_tmp");

		snprintf(buf, sizeof(buf),
		    "do local p = ucl.parser();"
		    "p:parse_string(_rcd_tmp);"
		    "rcd.%s = p:get_object() or {} end;"
		    "_rcd_tmp = nil", field);
		luaL_dostring(L, buf);
	} else {
		lua_getglobal(L, "rcd");
		lua_newtable(L);
		lua_setfield(L, -2, field);
		lua_pop(L, 1);
	}
}

/*
 * Execute a Lua string in the shared state.
 * Sets:
 *   rcd.instance        — instance name (or nil)
 *   rcd.config          — global service config from override file
 *   rcd.instance_config — per-instance config from instances {} block
 * Returns 0 on success, -1 on error (error is logged).
 */
int
lua_exec(const char *code, const char *source, const struct unit *u)
{
	lua_State *L;
	int error;

	L = rcd_lua_state;
	if (L == NULL) {
		log_warn("lua not initialized");
		return (-1);
	}

	/* Set rcd.instance */
	lua_getglobal(L, "rcd");
	if (u != NULL && u->u_instance != NULL)
		lua_pushstring(L, u->u_instance);
	else
		lua_pushnil(L);
	lua_setfield(L, -2, "instance");
	lua_pop(L, 1);

	/* Set rcd.config — global override config (always, for any unit) */
	lua_set_ucl_field(L, "config",
	    (u != NULL) ? u->u_override_conf : NULL);

	/* Set rcd.instance_config — per-instance config (templates only) */
	lua_set_ucl_field(L, "instance_config",
	    (u != NULL) ? u->u_instance_conf : NULL);

	error = luaL_loadbuffer(L, code, strlen(code), source);
	if (error != 0) {
		log_warn("lua load error: %s", lua_tostring(L, -1));
		lua_pop(L, 1);
		return (-1);
	}

	error = lua_pcall(L, 0, 1, 0);
	if (error != 0) {
		log_warn("lua exec error: %s", lua_tostring(L, -1));
		lua_pop(L, 1);
		return (-1);
	}

	/*
	 * Check return value: if the chunk returns false or nil,
	 * treat it as a precondition failure (return -1).
	 * If it returns true or nothing, return 0 (success).
	 */
	if (lua_isboolean(L, -1) && !lua_toboolean(L, -1)) {
		lua_pop(L, 1);
		return (-1);
	}
	lua_pop(L, 1);
	return (0);
}

/*
 * Clean up the Lua state.
 */
void
lua_fini(void)
{

	if (rcd_lua_state != NULL) {
		lua_close(rcd_lua_state);
		rcd_lua_state = NULL;
	}
}
