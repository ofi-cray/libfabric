/*
 * Copyright (c) 2015 Cray Inc. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdio.h>
#include <sys/time.h>
#include <fi_list.h>

#include "gnix_util.h"

#include <criterion/criterion.h>

static void setup(void)
{
	srand(time(NULL));
}

static void teardown(void)
{
}

static void generate_perm(int *perm, int len)
{
	int i;
	/* good 'nuff */
	for (i = 0; i < len; i++) {
		int t = perm[i];
		int j = rand() % len;

		perm[i] = perm[j];
		perm[j] = t;
	}
}

TestSuite(dlist_utils, .init = setup, .fini = teardown);

struct dlist_test {
	char dummy[3];
	struct dlist_entry le;
	int x;
};

Test(dlist_utils, for_each_safe)
{
	int i;
	const int n = 2609;
	struct dlist_entry dl;
	struct dlist_test dt[n];
	struct dlist_test *elem, *next;
	int perm[n];

	for (i = 0; i < n; i++)
		perm[i] = i;

	generate_perm(perm, n);

	dlist_init(&dl);
	for (i = 0; i < n; i++) {
		dt[perm[i]].x = i;
		dlist_insert_tail(&dt[perm[i]].le, &dl);
	}

	dlist_for_each_safe(&dl, elem, next, le) {
		cr_assert(elem->x == perm[i]);
		dlist_remove(&elem->le);
		++i;
	}
}

Test(dlist_utils, for_each_safe_empty)
{
	struct dlist_entry dl;
	struct dlist_test *elem, *next;

	dlist_init(&dl);

	dlist_for_each_safe(&dl, elem, next, le) {
		cr_assert(false);
	}
}
