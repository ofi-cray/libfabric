/*
 * Copyright (c) 2015 Los Alamos National Security, LLC. All rights reserved.
 * Copyright (c) 2015 Cray Inc.  All rights reserved.
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
#include <stddef.h>
#include <stdlib.h>

#include "fi.h"
#include "rdma/fi_domain.h"
#include "rdma/fi_prov.h"

#include "gnix.h"

#include <criterion/criterion.h>

static struct fid_fabric *fab;
static struct fid_domain *dom;
static struct fi_info *hints;
static struct fi_info *fi;
struct gnix_ep_name *fake_names;

static void av_setup(void)
{
	int ret = 0;

	hints = fi_allocinfo();
	cr_assert(hints, "fi_allocinfo");

	hints->domain_attr->cq_data_size = 4;
	hints->mode = ~0;

	hints->fabric_attr->name = strdup("gni");

	ret = fi_getinfo(FI_VERSION(1, 0), NULL, 0, 0, hints, &fi);
	cr_assert_eq(ret, FI_SUCCESS, "fi_getinfo");

	ret = fi_fabric(fi->fabric_attr, &fab, NULL);
	cr_assert_eq(ret, FI_SUCCESS, "fi_fabric");

	ret = fi_domain(fab, fi, &dom, NULL);
	cr_assert_eq(ret, FI_SUCCESS, "fi_domain");

}

static void av_teardown(void)
{
	int ret = 0;

	ret = fi_close(&dom->fid);
	cr_assert_eq(ret, FI_SUCCESS, "failure in closing domain.");
	ret = fi_close(&fab->fid);
	cr_assert_eq(ret, FI_SUCCESS, "failure in closing fabric.");
	fi_freeinfo(fi);
	fi_freeinfo(hints);
}

TestSuite(av, .init = av_setup, .fini = av_teardown);

Test(av, invalid_addrlen_pointer)
{
	int ret;
	struct fid_av *av;
	fi_addr_t address = 0xdeadbeef;
	void *addr = (void *) 0xb00fbabe;
	struct fi_av_attr av_table_attr = {
		.type = FI_AV_MAP,
		.count = 16,
	};

	ret = fi_av_open(dom, &av_table_attr, &av, NULL);
	cr_assert_eq(ret, FI_SUCCESS, "failed to open av");

	/* while the pointers to address and addr aren't valid, they are
	 * acceptable as stated by the manpage. This will only test for a
	 * proper return code from fi_av_lookup()
	 */
	ret = fi_av_lookup(av, address, addr, NULL);
	cr_assert_eq(ret, -FI_EINVAL);

	ret = fi_close(&av->fid);
	cr_assert_eq(ret, FI_SUCCESS, "failed to close av");

}

Test(av, invalid_addrlen_pointer_table)
{
	int ret;
	struct fid_av *av;
	fi_addr_t address = 0xdeadbeef;
	void *addr = (void *) 0xb00fbabe;
	struct fi_av_attr av_table_attr = {
		.type = FI_AV_TABLE,
		.count = 16,
	};

	ret = fi_av_open(dom, &av_table_attr, &av, NULL);
	cr_assert_eq(ret, FI_SUCCESS, "failed to open av");

	/* while the pointers to address and addr aren't valid, they are
	 * acceptable as stated by the manpage. This will only test for a
	 * proper return code from fi_av_lookup()
	 */
	ret = fi_av_lookup(av, address, addr, NULL);
	cr_assert_eq(ret, -FI_EINVAL);

}

#define TABLE_SIZE_INIT  16
#define TABLE_SIZE_FINAL 1024

Test(av, test_capacity)
{
	int ret, i;
	struct fid_av *av;
	fi_addr_t addresses[TABLE_SIZE_FINAL];
	struct fi_av_attr av_table_attr = {
		.type = FI_AV_TABLE,
		.count = TABLE_SIZE_INIT,
	};

	ret = fi_av_open(dom, &av_table_attr, &av, NULL);
	cr_assert_eq(ret, FI_SUCCESS, "failed to open av");

	fake_names = (struct gnix_ep_name *)calloc(TABLE_SIZE_FINAL,
						   sizeof(*fake_names));
	cr_assert_neq(fake_names, NULL);

	for (i = 0; i < TABLE_SIZE_INIT; i++) {
		fake_names[i].gnix_addr.device_addr = i + 100;
		fake_names[i].gnix_addr.cdm_id = i;
		fake_names[i].cm_nic_cdm_id = 0xbeef;
		fake_names[i].cookie = 0xdeadbeef;
	}

	ret = fi_av_insert(av, fake_names, TABLE_SIZE_INIT,
			   addresses, 0, NULL);
	cr_assert_eq(ret, TABLE_SIZE_INIT, "av insert failed");

	/*
	 * now add some more
	 */

	for (i = TABLE_SIZE_INIT; i < TABLE_SIZE_FINAL; i++) {
		fake_names[i].gnix_addr.device_addr = i + 100;
		fake_names[i].gnix_addr.cdm_id = i;
		fake_names[i].cm_nic_cdm_id = 0xbeef;
		fake_names[i].cookie = 0xdeadbeef;
	}

	ret = fi_av_insert(av, &fake_names[TABLE_SIZE_INIT],
			   TABLE_SIZE_FINAL - TABLE_SIZE_INIT,
			   &addresses[TABLE_SIZE_INIT], 0, NULL);
	cr_assert_eq(ret, TABLE_SIZE_FINAL - TABLE_SIZE_INIT,
		     "av insert failed");

}
