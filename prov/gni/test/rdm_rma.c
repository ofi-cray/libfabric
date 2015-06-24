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
#include <stdlib.h>
#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <time.h>
#include <string.h>
#include <pthread.h>

#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_errno.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_cm.h>

#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

#include "gnix_vc.h"
#include "gnix_cm_nic.h"
#include "gnix_hashtable.h"
#include "gnix_rma.h"

#include <criterion/criterion.h>

#define dbg_printf(...)

static struct fid_fabric *fab;
static struct fid_domain *dom;
static struct fid_ep *ep[2];
static struct fid_av *av;
static struct fi_info *hints;
static struct fi_info *fi;
void *ep_name[2];
size_t gni_addr[2];
static struct fid_cq *send_cq;
static struct fi_cq_attr cq_attr;

#define BUF_SZ 4096
uint64_t target[BUF_SZ];
uint64_t source[BUF_SZ];
struct fid_mr *rem_mr, *loc_mr;
uint64_t mr_key;

void rdm_rma_setup(void)
{
	int ret = 0;
	struct fi_av_attr attr;
	size_t addrlen = 0;

	hints = fi_allocinfo();
	cr_assert(hints, "fi_allocinfo");

	hints->domain_attr->cq_data_size = 4;
	hints->mode = ~0;

	hints->fabric_attr->name = strdup("gni");

	ret = fi_getinfo(FI_VERSION(1, 0), NULL, 0, 0, hints, &fi);
	cr_assert(!ret, "fi_getinfo");

	ret = fi_fabric(fi->fabric_attr, &fab, NULL);
	cr_assert(!ret, "fi_fabric");

	ret = fi_domain(fab, fi, &dom, NULL);
	cr_assert(!ret, "fi_domain");

	attr.type = FI_AV_MAP;
	attr.count = 16;

	ret = fi_av_open(dom, &attr, &av, NULL);
	cr_assert(!ret, "fi_av_open");

	ret = fi_endpoint(dom, fi, &ep[0], NULL);
	cr_assert(!ret, "fi_endpoint");

	cq_attr.format = FI_CQ_FORMAT_CONTEXT;
	cq_attr.size = 1024;
	cq_attr.wait_obj = 0;

	ret = fi_cq_open(dom, &cq_attr, &send_cq, 0);
	cr_assert(!ret, "fi_cq_open");

	ret = fi_ep_bind(ep[0], &send_cq->fid, FI_SEND);
	cr_assert(!ret, "fi_ep_bind");

	ret = fi_getname(&ep[0]->fid, NULL, &addrlen);
	cr_assert(addrlen > 0);

	ep_name[0] = malloc(addrlen);
	cr_assert(ep_name[0] != NULL);

	ep_name[1] = malloc(addrlen);
	cr_assert(ep_name[1] != NULL);

	ret = fi_getname(&ep[0]->fid, ep_name[0], &addrlen);
	cr_assert(ret == FI_SUCCESS);

	ret = fi_endpoint(dom, fi, &ep[1], NULL);
	cr_assert(!ret, "fi_endpoint");

	ret = fi_getname(&ep[1]->fid, ep_name[1], &addrlen);
	cr_assert(ret == FI_SUCCESS);

	ret = fi_av_insert(av, ep_name[0], 1, &gni_addr[0], 0,
				NULL);
	cr_assert(ret == 1);

	ret = fi_av_insert(av, ep_name[1], 1, &gni_addr[1], 0,
				NULL);
	cr_assert(ret == 1);

	ret = fi_ep_bind(ep[0], &av->fid, 0);
	cr_assert(!ret, "fi_ep_bind");

	ret = fi_ep_bind(ep[1], &av->fid, 0);
	cr_assert(!ret, "fi_ep_bind");

	ret = fi_enable(ep[0]);
	cr_assert(!ret, "fi_ep_enable");

	ret = fi_enable(ep[1]);
	cr_assert(!ret, "fi_ep_enable");

	ret = fi_mr_reg(dom, &target, sizeof(target),
			FI_REMOTE_WRITE, 0, 0, 0, &rem_mr, &target);
	cr_assert_eq(ret, 0);

	ret = fi_mr_reg(dom, &source, sizeof(source),
			FI_REMOTE_WRITE, 0, 0, 0, &loc_mr, &source);
	cr_assert_eq(ret, 0);

	mr_key = fi_mr_key(rem_mr);
}

void rdm_rma_teardown(void)
{
	int ret = 0;

	fi_close(&loc_mr->fid);
	fi_close(&rem_mr->fid);

	ret = fi_close(&ep[0]->fid);
	cr_assert(!ret, "failure in closing ep.");

	ret = fi_close(&ep[1]->fid);
	cr_assert(!ret, "failure in closing ep.");

	ret = fi_close(&send_cq->fid);
	cr_assert(!ret, "failure in send cq.");

	ret = fi_close(&av->fid);
	cr_assert(!ret, "failure in closing av.");

	ret = fi_close(&dom->fid);
	cr_assert(!ret, "failure in closing domain.");

	ret = fi_close(&fab->fid);
	cr_assert(!ret, "failure in closing fabric.");

	fi_freeinfo(fi);
	fi_freeinfo(hints);
	free(ep_name[0]);
	free(ep_name[1]);
}

void init_data(uint64_t *buf, int len, uint64_t seed)
{
	int i;

	for (i = 0; i < len; i++) {
		buf[i] = seed++;
	}
}

int check_data(uint64_t *buf1, uint64_t *buf2, int len)
{
	int i;

	for (i = 0; i < len; i++) {
		if (buf1[i] != buf2[i]) {
			printf("data mismatch, elem: %d, exp: %lx, act: %lx\n",
			       i, buf1[i], buf2[i]);
			fflush(stdout);
			return 0;
		}
	}

	return 1;
}

/*******************************************************************************
 * Test vc functions.
 ******************************************************************************/

TestSuite(rdm_rma, .init = rdm_rma_setup, .fini = rdm_rma_teardown,
	  .disabled = false);

Test(rdm_rma, write)
{
	int ret;
	ssize_t sz;
	struct fi_cq_entry cqe;

	init_data(source, BUF_SZ, 0xdeadbeefbeef4123);
	init_data(target, BUF_SZ, 0);
	sz = fi_write(ep[0], source, sizeof(target),
			 loc_mr, gni_addr[1], (uint64_t)&target, mr_key,
			 &target);
	cr_assert_eq(sz, 0);

	while ((ret = fi_cq_read(send_cq, &cqe, 1)) == -FI_EAGAIN) {
		pthread_yield();
	}

	cr_assert_eq(ret, 1);
	cr_assert_eq((uint64_t)cqe.op_context, (uint64_t)&target);

	dbg_printf("got write context event!\n");

	cr_assert(check_data(source, target, BUF_SZ), "Data mismatch");
}

Test(rdm_rma, writev)
{
	int ret;
	ssize_t sz;
	struct fi_cq_entry cqe;
	struct iovec iov;

	iov.iov_base = source;
	iov.iov_len = sizeof(source);

	init_data(source, BUF_SZ, 0xdeadbeefbeef4125);
	init_data(target, BUF_SZ, 0);
	sz = fi_writev(ep[0], &iov, (void **)&loc_mr, 1,
		       gni_addr[1], (uint64_t)&target, mr_key,
		       &target);
	cr_assert_eq(sz, 0);

	while ((ret = fi_cq_read(send_cq, &cqe, 1)) == -FI_EAGAIN) {
		pthread_yield();
	}

	cr_assert_eq(ret, 1);
	cr_assert_eq((uint64_t)cqe.op_context, (uint64_t)&target);

	dbg_printf("got write context event!\n");

	cr_assert(check_data(source, target, BUF_SZ), "Data mismatch");
}

Test(rdm_rma, writemsg)
{
	int ret;
	ssize_t sz;
	struct fi_cq_entry cqe;
	struct iovec iov;
	struct fi_msg_rma msg;
	struct fi_rma_iov rma_iov;

	iov.iov_base = source;
	iov.iov_len = sizeof(source);

	rma_iov.addr = (uint64_t)target;
	rma_iov.len = sizeof(target);
	rma_iov.key = mr_key;

	msg.msg_iov = &iov;
	msg.desc = (void **)&loc_mr;
	msg.iov_count = 1;
	msg.addr = gni_addr[1];
	msg.rma_iov = &rma_iov;
	msg.rma_iov_count = 1;
	msg.context = target;
	msg.data = (uint64_t)target;

	init_data(source, BUF_SZ, 0xdeadbeefbeef);
	init_data(target, BUF_SZ, 0);
	sz = fi_writemsg(ep[0], &msg, 0);
	cr_assert_eq(sz, 0);

	while ((ret = fi_cq_read(send_cq, &cqe, 1)) == -FI_EAGAIN) {
		pthread_yield();
	}

	cr_assert_eq(ret, 1);
	cr_assert_eq((uint64_t)cqe.op_context, (uint64_t)&target);

	dbg_printf("got write context event!\n");

	cr_assert(check_data(source, target, BUF_SZ), "Data mismatch");
}

Test(rdm_rma, inject_write)
{
	ssize_t sz;

	init_data(source, BUF_SZ, 0xdeadbeefbeef4123);
	init_data(target, BUF_SZ, 0);
	sz = fi_inject_write(ep[0], source, sizeof(target),
			     gni_addr[1], (uint64_t)&target, mr_key);
	cr_assert_eq(sz, 0);

	/* TODO sync */

	cr_assert(check_data(source, target, BUF_SZ), "Data mismatch");
}

Test(rdm_rma, writedata)
{
	int ret;
	ssize_t sz;
	struct fi_cq_entry cqe;

#define WRITE_DATA 0x5123da1a145
	init_data(source, BUF_SZ, 0xdeadbeefbeef4123);
	init_data(target, BUF_SZ, 0);
	sz = fi_writedata(ep[0], source, sizeof(target), loc_mr, WRITE_DATA,
			 gni_addr[1], (uint64_t)&target, mr_key,
			 &target);
	cr_assert_eq(sz, 0);

	while ((ret = fi_cq_read(send_cq, &cqe, 1)) == -FI_EAGAIN) {
		pthread_yield();
	}

	cr_assert_eq(ret, 1);
	cr_assert_eq((uint64_t)cqe.op_context, (uint64_t)&target);

	dbg_printf("got write context event!\n");

	cr_assert(check_data(source, target, BUF_SZ), "Data mismatch");
}

Test(rdm_rma, inject_writedata)
{
	ssize_t sz;

#define INJECT_WRITE_DATA 0x5123da1a
	init_data(source, BUF_SZ, 0xdeadbeefbeef4123);
	init_data(target, BUF_SZ, 0);
	sz = fi_inject_writedata(ep[0], source, sizeof(target),
				 INJECT_WRITE_DATA,
				 gni_addr[1], (uint64_t)&target, mr_key);
	cr_assert_eq(sz, 0);

	/* TODO sync */

	cr_assert(check_data(source, target, BUF_SZ), "Data mismatch");
}



Test(rdm_rma, read)
{
	int ret;
	ssize_t sz;
	struct fi_cq_entry cqe;

#define READ_CTX 0x4e3dda1aULL
	init_data(source, BUF_SZ, 0);
	init_data(target, BUF_SZ, 0xdeadbeefbeef43ad);
	sz = fi_read(ep[0], source, sizeof(target),
			loc_mr, gni_addr[1], (uint64_t)&target, mr_key,
			(void *)READ_CTX);
	cr_assert_eq(sz, 0);

	while ((ret = fi_cq_read(send_cq, &cqe, 1)) == -FI_EAGAIN) {
		pthread_yield();
	}

	cr_assert_eq(ret, 1);
	cr_assert_eq((uint64_t)cqe.op_context, READ_CTX);

	dbg_printf("got read context event!\n");

	cr_assert(check_data(source, target, BUF_SZ), "Data mismatch");
}
