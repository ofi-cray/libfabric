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
#include <stdlib.h>
#include <stddef.h>
#include <sys/time.h>

#include "gnix.h"

#ifdef assert
#undef assert
#endif

#include <criterion/criterion.h>

#include "fab_req.h"

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

static void init_freelist(struct gnix_fid_ep *ep)
{
	int ret = _CT__fab_req_freelist_init(ep,
					 GNIX_FAB_RQ_FL_INIT_SIZE, true);
	assert_eq(ret, FI_SUCCESS, "Failed to allocate gnix_fab_req free list");
}

static void destroy_freelist(struct gnix_fid_ep *ep)
{
	_CT__fab_req_freelist_destroy(ep);
}

TestSuite(gnix_fab_req, .init = setup, .fini = teardown);

Test(gnix_fab_req, freelist_init_destroy)
{
	const int num_eps = 13;
	struct gnix_fid_ep eps[num_eps];
	int i;

	for (i = 0; i < num_eps; i++)
		init_freelist(&eps[i]);

	for (i = num_eps-1; i >= 0; i--)
		destroy_freelist(&eps[i]);
}

Test(gnix_fab_req, freelist_refill_test)
{
	struct gnix_fid_ep ep;
	int i, ret;
	struct gnix_fab_req *reqs[GNIX_FAB_RQ_FL_INIT_SIZE];
	struct gnix_fab_req *refill_reqs[GNIX_FAB_RQ_FL_INIT_SIZE];

	init_freelist(&ep);

	for (i = 0; i < GNIX_FAB_RQ_FL_INIT_SIZE; i++) {
		ret = _CT__fab_req_alloc(&ep, &reqs[i]);
		assert_eq(ret, FI_SUCCESS,
			  "Failed to obtain valid gnix_fab_req");
	}
	assert(_CT__fab_req_freelist_empty(&ep));

	for (i = 0; i < GNIX_FAB_RQ_FL_INIT_REFILL_SIZE; i++) {
		ret = _CT__fab_req_alloc(&ep, &refill_reqs[i]);
		assert_eq(ret, FI_SUCCESS,
			  "Failed to obtain valid gnix_fab_req");
		if (i != GNIX_FAB_RQ_FL_INIT_REFILL_SIZE-1) {
			/* Not the last one, so must be empty */
			assert(!_CT__fab_req_freelist_empty(&ep));
		}
	}
	assert(_CT__fab_req_freelist_empty(&ep));

	for (i = GNIX_FAB_RQ_FL_INIT_SIZE-1; i >= 0 ; i--) {
		_CT__fab_req_free(reqs[i]);
		assert(!_CT__fab_req_freelist_empty(&ep));
	}

	for (i = GNIX_FAB_RQ_FL_INIT_REFILL_SIZE-1; i >= 0 ; i--) {
		_CT__fab_req_free(refill_reqs[i]);
		assert(!_CT__fab_req_freelist_empty(&ep));
	}

	destroy_freelist(&ep);
}

Test(gnix_fab_req, freelist_random_alloc_free)
{
	struct gnix_fid_ep ep;
	int i, ret;
	const int num_reqs = 777;
	int perm[num_reqs];
	struct gnix_fab_req *reqs[num_reqs];

	for (i = 0; i < num_reqs; i++)
		perm[i] = i;

	generate_perm(perm, num_reqs);

	init_freelist(&ep);

	for (i = 0; i < num_reqs; i++) {
		ret = _CT__fab_req_alloc(&ep, &reqs[i]);
		assert_eq(ret, FI_SUCCESS,
			  "Failed to obtain valid gnix_fab_req");
	}

	for (i = 0; i < num_reqs; i++) {
		int j = perm[i];

		_CT__fab_req_free(reqs[j]);
		reqs[j] = NULL;
		assert(!_CT__fab_req_freelist_empty(&ep));
	}

	destroy_freelist(&ep);
}

