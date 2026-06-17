/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Baptiste Daroussin <bapt@FreeBSD.org>
 */

/*
 * Tests for rctl_mgr.c: rctl availability check, apply/remove.
 */

#include <sys/param.h>

#include <string.h>

#include <atf-c.h>

#include "rcd.h"

ATF_TC(rctl_available_returns);
ATF_TC_HEAD(rctl_available_returns, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "rctl_available returns a boolean without crashing");
}
ATF_TC_BODY(rctl_available_returns, tc)
{
	bool avail;

	/* Just verify it doesn't crash — result depends on kernel config */
	avail = rctl_available();
	(void)avail;
}

ATF_TC(rctl_apply_empty);
ATF_TC_HEAD(rctl_apply_empty, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "rctl_apply with no rules is a no-op");
}
ATF_TC_BODY(rctl_apply_empty, tc)
{
	struct unit *u;

	u = unit_alloc();
	u->u_name = xstrdup("test");

	ATF_CHECK(rctl_apply(u) == 0);
	unit_free(u);
}

ATF_TC(rctl_remove_empty);
ATF_TC_HEAD(rctl_remove_empty, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "rctl_remove with no active rules is a no-op");
}
ATF_TC_BODY(rctl_remove_empty, tc)
{
	struct unit *u;

	u = unit_alloc();
	u->u_name = xstrdup("test");

	/* Should not crash */
	rctl_remove(u);
	unit_free(u);
}

ATF_TC(rctl_get_usage_no_pid);
ATF_TC_HEAD(rctl_get_usage_no_pid, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "rctl_get_usage returns -1 when no pid and no jail");
}
ATF_TC_BODY(rctl_get_usage_no_pid, tc)
{
	struct unit *u;
	char buf[256];

	u = unit_alloc();
	u->u_name = xstrdup("test");

	ATF_CHECK(rctl_get_usage(u, buf, sizeof(buf)) == -1);
	unit_free(u);
}

ATF_TP_ADD_TCS(tp)
{

	ATF_TP_ADD_TC(tp, rctl_available_returns);
	ATF_TP_ADD_TC(tp, rctl_apply_empty);
	ATF_TP_ADD_TC(tp, rctl_remove_empty);
	ATF_TP_ADD_TC(tp, rctl_get_usage_no_pid);

	return (atf_no_error());
}
