/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Baptiste Daroussin <bapt@FreeBSD.org>
 */

/*
 * Stub definition for rcd_oom_env, used by tests that link against
 * code using x* allocators (xmalloc.h).  OOM never occurs in tests.
 */

#include "rcd.h"

jmp_buf rcd_oom_env;
