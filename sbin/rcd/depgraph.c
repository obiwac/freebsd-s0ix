/*
 * Copyright (c) 2026 Baptiste Daroussin <bapt@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Dependency graph — DAG construction, topological sort, and parallel
 * scheduling.  Units declare provide/require/before relationships;
 * this module resolves them into a graph and provides the ready set
 * for parallel startup.
 */

#include <sys/param.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rcd.h"

int
depgraph_init(struct depgraph *dg)
{

	TAILQ_INIT(&dg->dg_units);
	dg->dg_nunits = 0;
	dg->dg_provisions = hash_new();
	return (0);
}

int
depgraph_add(struct depgraph *dg, struct unit *u)
{

	TAILQ_INSERT_TAIL(&dg->dg_units, u, u_entries);
	dg->dg_nunits++;

	/* Register all provisions */
	vec_foreach(u->u_provide, i)
		hash_add(dg->dg_provisions, u->u_provide.d[i], u, NULL);

	return (0);
}

/*
 * Find a unit by provision name.
 */
struct unit *
depgraph_find(struct depgraph *dg, const char *name)
{

	return ((struct unit *)hash_get_value(dg->dg_provisions, name));
}

/*
 * Resolve all require/before references into dep_link pointers.
 * Returns 0 on success, -1 if there are missing dependencies (non-fatal).
 */
int
depgraph_resolve(struct depgraph *dg)
{
	struct unit *u, *dep;
	struct dep_link *dl, *dl_tmp;
	int errors;

	errors = 0;

	/* Clear existing links to allow re-resolution on reload */
	TAILQ_FOREACH(u, &dg->dg_units, u_entries) {
		STAILQ_FOREACH_SAFE(dl, &u->u_deps, dl_entries, dl_tmp)
			free(dl);
		STAILQ_INIT(&u->u_deps);
		STAILQ_FOREACH_SAFE(dl, &u->u_rdeps, dl_entries, dl_tmp)
			free(dl);
		STAILQ_INIT(&u->u_rdeps);
		u->u_unmet = 0;
	}

	TAILQ_FOREACH(u, &dg->dg_units, u_entries) {
		/* Resolve REQUIRE: u depends on dep */
		vec_foreach(u->u_require, i) {
			dep = (struct unit *)hash_get_value(
			    dg->dg_provisions, u->u_require.d[i]);
			if (dep == NULL) {
				log_warn(
				    "%s: unresolved dependency: %s",
				    u->u_name,
				    u->u_require.d[i]);
				errors++;
				continue;
			}

			/*
			 * Disabled dependencies are considered
			 * already satisfied — they will never start,
			 * so waiting on them would block forever.
			 * This matches rcorder behavior where
			 * filtered scripts are simply absent.
			 */
			/* u depends on dep (dep must start before u) */
			dl = xcalloc(1, sizeof(*dl));
			dl->dl_unit = dep;
			STAILQ_INSERT_TAIL(&u->u_deps, dl,
			    dl_entries);
			u->u_unmet++;

			/* dep is depended on by u */
			dl = xcalloc(1, sizeof(*dl));
			dl->dl_unit = u;
			STAILQ_INSERT_TAIL(&dep->u_rdeps, dl,
			    dl_entries);
		}

		/*
		 * Resolve BEFORE: u must start before dep → dep depends on u.
		 * Skip if u is disabled — it won't run, so adding
		 * "dep depends on u" would block dep forever.
		 */
		if (!u->u_enabled)
			continue; /* u won't run, skip its BEFORE edges */
		vec_foreach(u->u_before, i) {
			dep = (struct unit *)hash_get_value(
			    dg->dg_provisions, u->u_before.d[i]);
			if (dep == NULL)
				continue;
			if (!dep->u_enabled)
				continue;

			/* dep depends on u */
			dl = xcalloc(1, sizeof(*dl));
			dl->dl_unit = u;
			STAILQ_INSERT_TAIL(&dep->u_deps, dl,
			    dl_entries);
			dep->u_unmet++;

			/* u is depended on by dep */
			dl = xcalloc(1, sizeof(*dl));
			dl->dl_unit = dep;
			STAILQ_INSERT_TAIL(&u->u_rdeps, dl,
			    dl_entries);
		}
	}

	return (errors > 0 ? -1 : 0);
}

/*
 * DFS visit states for cycle detection.
 */
enum dfs_state {
	DFS_UNVISITED,
	DFS_IN_PROGRESS,
	DFS_DONE
};

static int
dfs_visit(struct unit *u, enum dfs_state *states, struct unit **ulist,
    int nunits)
{
	struct dep_link *dl;
	int cycles, idx_dep;

	/* Find index of u in ulist */
	int idx;
	for (idx = 0; idx < nunits; idx++) {
		if (ulist[idx] == u)
			break;
	}
	if (idx >= nunits)
		return (0);

	states[idx] = DFS_IN_PROGRESS;
	cycles = 0;

	STAILQ_FOREACH(dl, &u->u_deps, dl_entries) {
		for (idx_dep = 0; idx_dep < nunits; idx_dep++) {
			if (ulist[idx_dep] == dl->dl_unit)
				break;
		}
		if (idx_dep >= nunits)
			continue;

		if (states[idx_dep] == DFS_IN_PROGRESS) {
			log_warn("dependency cycle: %s -> %s",
			    u->u_name, dl->dl_unit->u_name);
			dl->dl_unit->u_state = STATE_FAILED;
			cycles++;
		} else if (states[idx_dep] == DFS_UNVISITED) {
			cycles += dfs_visit(dl->dl_unit, states, ulist,
			    nunits);
		}
	}

	states[idx] = DFS_DONE;
	return (cycles);
}

/*
 * Detect cycles using DFS.  Returns 0 if no cycles, -1 if cycles found.
 */
int
depgraph_check_cycles(struct depgraph *dg)
{
	struct unit **ulist;
	enum dfs_state *states;
	struct unit *u;
	int i, n, cycles;

	n = dg->dg_nunits;
	if (n == 0)
		return (0);

	ulist = xcalloc(n, sizeof(*ulist));
	states = xcalloc(n, sizeof(*states));

	i = 0;
	TAILQ_FOREACH(u, &dg->dg_units, u_entries)
		ulist[i++] = u;

	cycles = 0;
	for (i = 0; i < n; i++) {
		if (states[i] == DFS_UNVISITED)
			cycles += dfs_visit(ulist[i], states, ulist, n);
	}

	free(ulist);
	free(states);
	return (cycles > 0 ? -1 : 0);
}

/*
 * Get the set of units whose dependencies are all satisfied (unmet == 0)
 * and that are in INACTIVE state (not yet started).
 */
void
depgraph_ready_set(struct depgraph *dg, struct unit **out, int *nout)
{
	struct unit *u;
	int n, cap;

	cap = (int)dg->dg_nunits;
	n = 0;
	TAILQ_FOREACH(u, &dg->dg_units, u_entries) {
		if (!u->u_enabled)
			continue;
		if (u->u_template)
			continue;
		if (u->u_state != STATE_INACTIVE)
			continue;
		if (u->u_unmet <= 0) {
			if (n >= cap)
				break;
			out[n++] = u;
		}
	}
	*nout = n;
}

/*
 * Mark a unit as done (started or failed) and decrement the unmet count
 * of all units that depend on it.
 */
void
depgraph_mark_done(struct depgraph *dg __unused, struct unit *u)
{
	struct dep_link *dl;

	STAILQ_FOREACH(dl, &u->u_rdeps, dl_entries) {
		if (dl->dl_unit->u_unmet > 0)
			dl->dl_unit->u_unmet--;
	}
}

/*
 * Compute shutdown order (reverse dependency order).
 * Returns units in the order they should be stopped.
 * 'cap' is the capacity of the 'out' array (must be >= dg_nunits).
 */
void
depgraph_shutdown_order(struct depgraph *dg, struct unit **out, int *nout)
{
	struct unit *u;
	int n;

	/*
	 * Simple approach: collect all running units, then reverse.
	 * A proper implementation would do reverse topological sort.
	 */
	n = 0;
	TAILQ_FOREACH(u, &dg->dg_units, u_entries) {
		if (!u->u_enabled || u->u_template)
			continue;
		if (u->u_state != STATE_RUNNING)
			continue;
		out[n++] = u;
	}

	/* Reverse the array for shutdown order */
	for (int i = 0; i < n / 2; i++) {
		struct unit *tmp = out[i];
		out[i] = out[n - 1 - i];
		out[n - 1 - i] = tmp;
	}

	*nout = n;
}

void
depgraph_free(struct depgraph *dg)
{
	struct unit *u, *u_tmp;

	TAILQ_FOREACH_SAFE(u, &dg->dg_units, u_entries, u_tmp) {
		TAILQ_REMOVE(&dg->dg_units, u, u_entries);
		unit_free(u);
	}
	hash_destroy(dg->dg_provisions);
}
