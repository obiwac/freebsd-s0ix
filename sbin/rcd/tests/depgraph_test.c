/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Baptiste Daroussin <bapt@FreeBSD.org>
 */

/*
 * Unit tests for the dependency graph module.
 */

#include <sys/param.h>

#include <stdio.h>
#include <string.h>

#include <atf-c.h>

#include "rcd.h"

/*
 * Helper: create a minimal unit with given provide/require.
 */
static struct unit *
make_unit(const char *name, const char **provide, int nprov,
    const char **require, int nreq)
{
	struct unit *u;
	int i;

	u = unit_alloc();
	u->u_name = xstrdup(name);
	u->u_enabled = true;

	for (i = 0; i < nprov; i++)
		vec_push(&u->u_provide, xstrdup(provide[i]));

	for (i = 0; i < nreq; i++)
		vec_push(&u->u_require, xstrdup(require[i]));

	return (u);
}

ATF_TC_WITHOUT_HEAD(empty_graph);
ATF_TC_BODY(empty_graph, tc)
{
	struct depgraph dg;

	depgraph_init(&dg);
	ATF_CHECK_EQ(dg.dg_nunits, 0);
	ATF_CHECK(depgraph_resolve(&dg) == 0);
	ATF_CHECK(depgraph_check_cycles(&dg) == 0);
	depgraph_free(&dg);
}

ATF_TC_WITHOUT_HEAD(single_unit_no_deps);
ATF_TC_BODY(single_unit_no_deps, tc)
{
	struct depgraph dg;
	struct unit *u, *ready[256];
	int nready;
	const char *prov[] = { "test" };

	depgraph_init(&dg);

	u = make_unit("test", prov, 1, NULL, 0);
	depgraph_add(&dg, u);
	ATF_CHECK_EQ(dg.dg_nunits, 1);

	ATF_CHECK(depgraph_resolve(&dg) == 0);

	depgraph_ready_set(&dg, ready, &nready);
	ATF_CHECK_EQ(nready, 1);
	ATF_CHECK_STREQ(ready[0]->u_name, "test");

	depgraph_free(&dg);
}

ATF_TC_WITHOUT_HEAD(linear_dependency_chain);
ATF_TC_BODY(linear_dependency_chain, tc)
{
	struct depgraph dg;
	struct unit *a, *b, *c, *ready[256];
	int nready;
	const char *prov_a[] = { "A" };
	const char *prov_b[] = { "B" };
	const char *prov_c[] = { "C" };
	const char *req_b[] = { "A" };
	const char *req_c[] = { "B" };

	depgraph_init(&dg);

	a = make_unit("A", prov_a, 1, NULL, 0);
	b = make_unit("B", prov_b, 1, req_b, 1);
	c = make_unit("C", prov_c, 1, req_c, 1);

	depgraph_add(&dg, a);
	depgraph_add(&dg, b);
	depgraph_add(&dg, c);

	ATF_CHECK(depgraph_resolve(&dg) == 0);

	/* Only A should be ready initially */
	depgraph_ready_set(&dg, ready, &nready);
	ATF_CHECK_EQ(nready, 1);
	ATF_CHECK_STREQ(ready[0]->u_name, "A");

	/* Mark A as done -> B should become ready */
	a->u_state = STATE_RUNNING;
	depgraph_mark_done(&dg, a);
	depgraph_ready_set(&dg, ready, &nready);
	ATF_CHECK_EQ(nready, 1);
	ATF_CHECK_STREQ(ready[0]->u_name, "B");

	/* Mark B as done -> C should become ready */
	b->u_state = STATE_RUNNING;
	depgraph_mark_done(&dg, b);
	depgraph_ready_set(&dg, ready, &nready);
	ATF_CHECK_EQ(nready, 1);
	ATF_CHECK_STREQ(ready[0]->u_name, "C");

	depgraph_free(&dg);
}

ATF_TC_WITHOUT_HEAD(parallel_independent);
ATF_TC_BODY(parallel_independent, tc)
{
	struct depgraph dg;
	struct unit *a, *b, *c, *ready[256];
	int nready;
	const char *prov_a[] = { "A" };
	const char *prov_b[] = { "B" };
	const char *prov_c[] = { "C" };

	depgraph_init(&dg);

	a = make_unit("A", prov_a, 1, NULL, 0);
	b = make_unit("B", prov_b, 1, NULL, 0);
	c = make_unit("C", prov_c, 1, NULL, 0);

	depgraph_add(&dg, a);
	depgraph_add(&dg, b);
	depgraph_add(&dg, c);

	ATF_CHECK(depgraph_resolve(&dg) == 0);

	/* All three should be ready in parallel */
	depgraph_ready_set(&dg, ready, &nready);
	ATF_CHECK_EQ(nready, 3);

	depgraph_free(&dg);
}

ATF_TC_WITHOUT_HEAD(diamond_dependency);
ATF_TC_BODY(diamond_dependency, tc)
{
	struct depgraph dg;
	struct unit *a, *b, *c, *d, *ready[256];
	int nready;
	const char *prov_a[] = { "A" };
	const char *prov_b[] = { "B" };
	const char *prov_c[] = { "C" };
	const char *prov_d[] = { "D" };
	const char *req_b[] = { "A" };
	const char *req_c[] = { "A" };
	const char *req_d[] = { "B", "C" };

	depgraph_init(&dg);

	a = make_unit("A", prov_a, 1, NULL, 0);
	b = make_unit("B", prov_b, 1, req_b, 1);
	c = make_unit("C", prov_c, 1, req_c, 1);
	d = make_unit("D", prov_d, 1, req_d, 2);

	depgraph_add(&dg, a);
	depgraph_add(&dg, b);
	depgraph_add(&dg, c);
	depgraph_add(&dg, d);

	ATF_CHECK(depgraph_resolve(&dg) == 0);

	/* Only A should be ready initially */
	depgraph_ready_set(&dg, ready, &nready);
	ATF_CHECK_EQ(nready, 1);
	ATF_CHECK_STREQ(ready[0]->u_name, "A");

	/* Mark A as done -> B and C should be ready in parallel */
	a->u_state = STATE_RUNNING;
	depgraph_mark_done(&dg, a);
	depgraph_ready_set(&dg, ready, &nready);
	ATF_CHECK_EQ(nready, 2);

	/* Mark B done -> D still blocked on C */
	b->u_state = STATE_RUNNING;
	depgraph_mark_done(&dg, b);
	depgraph_ready_set(&dg, ready, &nready);
	/* C is still inactive, D should not yet be ready */
	ATF_CHECK_EQ(nready, 1);
	ATF_CHECK_STREQ(ready[0]->u_name, "C");

	/* Mark C done -> D should be ready */
	c->u_state = STATE_RUNNING;
	depgraph_mark_done(&dg, c);
	depgraph_ready_set(&dg, ready, &nready);
	ATF_CHECK_EQ(nready, 1);
	ATF_CHECK_STREQ(ready[0]->u_name, "D");

	depgraph_free(&dg);
}

ATF_TC(cycle_detection);
ATF_TC_HEAD(cycle_detection, tc)
{
	atf_tc_set_md_var(tc, "descr", "Detect dependency cycles");
}
ATF_TC_BODY(cycle_detection, tc)
{
	const char *srcdir = atf_tc_get_config_var(tc, "srcdir");
	char path_a[PATH_MAX], path_b[PATH_MAX];
	struct depgraph dg;
	struct rcd_config cfg;
	struct unit *a, *b;

	memset(&cfg, 0, sizeof(cfg));

	depgraph_init(&dg);

	snprintf(path_a, sizeof(path_a), "%s/%s", srcdir, "cycle_a.ucl");
	snprintf(path_b, sizeof(path_b), "%s/%s", srcdir, "cycle_b.ucl");

	a = unit_parse(path_a, &cfg);
	b = unit_parse(path_b, &cfg);
	ATF_REQUIRE(a != NULL);
	ATF_REQUIRE(b != NULL);

	depgraph_add(&dg, a);
	depgraph_add(&dg, b);
	depgraph_resolve(&dg);

	/* Cycle detection should find the A -> B -> A cycle */
	ATF_CHECK(depgraph_check_cycles(&dg) == -1);

	depgraph_free(&dg);
}

ATF_TC_WITHOUT_HEAD(unresolved_dependency);
ATF_TC_BODY(unresolved_dependency, tc)
{
	struct depgraph dg;
	struct unit *a;
	const char *prov[] = { "A" };
	const char *req[] = { "NONEXISTENT" };

	depgraph_init(&dg);

	a = make_unit("A", prov, 1, req, 1);
	depgraph_add(&dg, a);

	/* Should return -1 for unresolved deps */
	ATF_CHECK(depgraph_resolve(&dg) == -1);

	depgraph_free(&dg);
}

ATF_TC_WITHOUT_HEAD(disabled_units_skipped);
ATF_TC_BODY(disabled_units_skipped, tc)
{
	struct depgraph dg;
	struct unit *a, *b, *ready[256];
	int nready;
	const char *prov_a[] = { "A" };
	const char *prov_b[] = { "B" };

	depgraph_init(&dg);

	a = make_unit("A", prov_a, 1, NULL, 0);
	b = make_unit("B", prov_b, 1, NULL, 0);
	b->u_enabled = false;

	depgraph_add(&dg, a);
	depgraph_add(&dg, b);
	depgraph_resolve(&dg);

	depgraph_ready_set(&dg, ready, &nready);
	ATF_CHECK_EQ(nready, 1);
	ATF_CHECK_STREQ(ready[0]->u_name, "A");

	depgraph_free(&dg);
}

ATF_TC_WITHOUT_HEAD(shutdown_order);
ATF_TC_BODY(shutdown_order, tc)
{
	struct depgraph dg;
	struct unit *a, *b, *c, *order[256];
	int norder;
	const char *prov_a[] = { "A" };
	const char *prov_b[] = { "B" };
	const char *prov_c[] = { "C" };

	depgraph_init(&dg);

	a = make_unit("A", prov_a, 1, NULL, 0);
	b = make_unit("B", prov_b, 1, NULL, 0);
	c = make_unit("C", prov_c, 1, NULL, 0);

	a->u_state = STATE_RUNNING;
	b->u_state = STATE_RUNNING;
	c->u_state = STATE_RUNNING;

	depgraph_add(&dg, a);
	depgraph_add(&dg, b);
	depgraph_add(&dg, c);

	depgraph_shutdown_order(&dg, order, &norder);
	ATF_CHECK_EQ(norder, 3);

	/* Shutdown order is reverse of insertion */
	ATF_CHECK_STREQ(order[0]->u_name, "C");
	ATF_CHECK_STREQ(order[1]->u_name, "B");
	ATF_CHECK_STREQ(order[2]->u_name, "A");

	depgraph_free(&dg);
}

ATF_TC_WITHOUT_HEAD(find_by_provision);
ATF_TC_BODY(find_by_provision, tc)
{
	struct depgraph dg;
	struct unit *a, *found;
	const char *prov[] = { "myservice", "myalias" };

	depgraph_init(&dg);

	a = make_unit("myservice", prov, 2, NULL, 0);
	depgraph_add(&dg, a);

	found = depgraph_find(&dg, "myservice");
	ATF_CHECK(found == a);

	found = depgraph_find(&dg, "myalias");
	ATF_CHECK(found == a);

	found = depgraph_find(&dg, "nonexistent");
	ATF_CHECK(found == NULL);

	depgraph_free(&dg);
}

ATF_TC_WITHOUT_HEAD(disabled_deps_satisfied);
ATF_TC_BODY(disabled_deps_satisfied, tc)
{
	struct depgraph dg;
	struct unit *a, *b, *c, *ready[256];
	int nready;
	const char *prov_a[] = { "A" };
	const char *prov_b[] = { "B" };
	const char *prov_c[] = { "C" };
	const char *req_c[] = { "B" };

	depgraph_init(&dg);

	a = make_unit("A", prov_a, 1, NULL, 0);
	b = make_unit("B", prov_b, 1, NULL, 0);
	b->u_enabled = false;
	c = make_unit("C", prov_c, 1, req_c, 1);

	depgraph_add(&dg, a);
	depgraph_add(&dg, b);
	depgraph_add(&dg, c);

	ATF_CHECK(depgraph_resolve(&dg) == 0);

	/*
	 * B is disabled, so mark it as done immediately.
	 * C depends on B, so after B is done, C should become ready.
	 */
	b->u_state = STATE_DONE;
	depgraph_mark_done(&dg, b);

	depgraph_ready_set(&dg, ready, &nready);
	/*
	 * A is enabled and has no deps -> ready.
	 * C depended on B; B is now done -> C should also be ready.
	 */
	ATF_CHECK_EQ(nready, 2);

	depgraph_free(&dg);
}

ATF_TC_WITHOUT_HEAD(before_disabled_skipped);
ATF_TC_BODY(before_disabled_skipped, tc)
{
	struct depgraph dg;
	struct unit *a, *b, *ready[256];
	int nready;
	const char *prov_a[] = { "A" };
	const char *prov_b[] = { "B" };

	depgraph_init(&dg);

	/*
	 * A declares BEFORE: B, meaning B depends on A.
	 * But A is disabled, so the edge should not be created
	 * and B should not be blocked.
	 */
	a = make_unit("A", prov_a, 1, NULL, 0);
	vec_push(&a->u_before, xstrdup("B"));
	a->u_enabled = false;

	b = make_unit("B", prov_b, 1, NULL, 0);

	depgraph_add(&dg, a);
	depgraph_add(&dg, b);

	ATF_CHECK(depgraph_resolve(&dg) == 0);

	depgraph_ready_set(&dg, ready, &nready);
	/*
	 * A is disabled so should not appear in the ready set.
	 * B should be ready because the BEFORE edge from disabled
	 * A should not block it.
	 */
	ATF_CHECK_EQ(nready, 1);
	ATF_CHECK_STREQ(ready[0]->u_name, "B");

	depgraph_free(&dg);
}

ATF_TP_ADD_TCS(tp)
{

	ATF_TP_ADD_TC(tp, empty_graph);
	ATF_TP_ADD_TC(tp, single_unit_no_deps);
	ATF_TP_ADD_TC(tp, linear_dependency_chain);
	ATF_TP_ADD_TC(tp, parallel_independent);
	ATF_TP_ADD_TC(tp, diamond_dependency);
	ATF_TP_ADD_TC(tp, cycle_detection);
	ATF_TP_ADD_TC(tp, unresolved_dependency);
	ATF_TP_ADD_TC(tp, disabled_units_skipped);
	ATF_TP_ADD_TC(tp, shutdown_order);
	ATF_TP_ADD_TC(tp, find_by_provision);
	ATF_TP_ADD_TC(tp, disabled_deps_satisfied);
	ATF_TP_ADD_TC(tp, before_disabled_skipped);

	return (atf_no_error());
}
