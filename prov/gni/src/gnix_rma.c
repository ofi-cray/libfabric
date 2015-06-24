/*
 * Copyright (c) 2015 Cray Inc. All rights reserved.
 * Copyright (c) 2015 Los Alamos National Security, LLC. All rights reserved.
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

#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "gnix.h"
#include "gnix_nic.h"
#include "gnix_vc.h"
#include "gnix_ep.h"
#include "gnix_mr.h"
#include "gnix_cm_nic.h"
#include "gnix_mbox_allocator.h"
#include <gni_pub.h>

static int __gnix_rma_fab_req_complete(void *arg)
{
	struct gnix_fab_req *req = (struct gnix_fab_req *)arg;
	struct gnix_fid_ep *ep = req->gnix_ep;
	int rc;

	/* more transaction needed for request? */

	if (req->flags & FI_COMPLETION) {
		rc = _gnix_cq_add_event(ep->send_cq, req->user_context,
					req->flags, req->len,
					(void *)req->loc_addr,
					req->imm, req->msg.tag);
		if (rc) {
			GNIX_WARN(FI_LOG_CQ,
				  "_gnix_cq_add_event() failed: %d\n", rc);
		}
	}

	_gnix_fr_free(ep, req);

	return FI_SUCCESS;
}

static int __gnix_rma_txd_complete(void *arg)
{
	struct gnix_tx_descriptor *txd = (struct gnix_tx_descriptor *)arg;

	/* Progress fabric operation in the fab_req completer.  Can we call
	 * fab_req->completer_fn directly from __gnix_tx_progress? */
	return txd->desc.req->completer_fn(txd->desc.req->completer_data);
}

static gni_post_type_t __gnix_fr_post_type(int fr_type)
{
	switch (fr_type) {
	case GNIX_FAB_RQ_RDMA_WRITE:
		return GNI_POST_RDMA_PUT;
	case GNIX_FAB_RQ_RDMA_READ:
		return GNI_POST_RDMA_GET;
	default:
		break;
	}

	GNIX_WARN(FI_LOG_EP_DATA, "Unsupported post type: %d", fr_type);
	return -FI_ENOSYS;
}

int _gnix_rma_post_req(void *data)
{
	struct gnix_fab_req *fab_req = (struct gnix_fab_req *)data;
	struct gnix_fid_ep *ep = fab_req->gnix_ep;
	struct gnix_nic *nic = ep->nic;
	struct gnix_fid_mem_desc *md;
	struct gnix_tx_descriptor *txd;
	gni_mem_handle_t mdh;
	gni_return_t status;
	int rc;

	fastlock_acquire(&nic->lock);

	rc = _gnix_nic_tx_alloc(nic, &txd);
	if (rc) {
		GNIX_INFO(FI_LOG_EP_DATA, "_gnix_nic_tx_alloc() failed: %d\n",
			 rc);
		fastlock_release(&nic->lock);
		return -FI_EAGAIN;
	}

	txd->desc.completer_fn = __gnix_rma_txd_complete;
	txd->desc.req = fab_req;

	_gnix_convert_key_to_mhdl((gnix_mr_key_t *)&fab_req->rma.rem_mr_key,
				  &mdh);
	md = (struct gnix_fid_mem_desc *)fab_req->rma.loc_md;

	//txd->desc.gni_desc.post_id = (uint64_t)fab_req; /* unused */
	txd->desc.gni_desc.type = __gnix_fr_post_type(fab_req->type);
	txd->desc.gni_desc.cq_mode = GNI_CQMODE_GLOBAL_EVENT; /* check flags */
	txd->desc.gni_desc.dlvr_mode = GNI_DLVMODE_PERFORMANCE; /* check flags */
	txd->desc.gni_desc.local_addr = (uint64_t)fab_req->loc_addr;
	txd->desc.gni_desc.local_mem_hndl = md->mem_hndl;
	txd->desc.gni_desc.remote_addr = (uint64_t)fab_req->rma.rem_addr;
	txd->desc.gni_desc.remote_mem_hndl = mdh;
	txd->desc.gni_desc.length = fab_req->len;
	txd->desc.gni_desc.rdma_mode = 0; /* check flags */
	txd->desc.gni_desc.src_cq_hndl = nic->tx_cq; /* check flags */

	{
		gni_mem_handle_t *tl_mdh = &txd->desc.gni_desc.local_mem_hndl;
		gni_mem_handle_t *tr_mdh = &txd->desc.gni_desc.remote_mem_hndl;
		GNIX_INFO(FI_LOG_EP_DATA, "la: %llx ra: %llx len: %d\n",
			  txd->desc.gni_desc.local_addr, txd->desc.gni_desc.remote_addr,
			  txd->desc.gni_desc.length);
		GNIX_INFO(FI_LOG_EP_DATA, "lmdh: %llx:%llx rmdh: %llx:%llx key: %llx\n",
			  *(uint64_t *)tl_mdh, *(((uint64_t *)tl_mdh) + 1),
			  *(uint64_t *)tr_mdh, *(((uint64_t *)tr_mdh) + 1),
			  fab_req->rma.rem_mr_key);
	}

	status = GNI_PostRdma(fab_req->vc->gni_ep, &txd->desc.gni_desc);
	if (status != GNI_RC_SUCCESS) {
		GNIX_INFO(FI_LOG_EP_DATA, "GNI_PostRdma() failed: %d\n", status);
	}

	fastlock_release(&nic->lock);

	return gnixu_to_fi_errno(status);
}

ssize_t _gnix_rma(struct gnix_fid_ep *ep, enum gnix_fab_req_type fr_type,
		  uint64_t loc_addr, size_t len, void *mdesc,
		  uint64_t dest_addr, uint64_t rem_addr, uint64_t mkey,
		  void *context, uint64_t flags, uint64_t data)
{
	struct gnix_vc *vc;
	struct gnix_fab_req *req;
	struct gnix_fid_mem_desc *md;
	int rc;

	/* TODO better validation of mdesc */
	if (!ep || !mdesc) {
		return -FI_EINVAL;
	}

	/* find VC for target */
	rc = _gnix_ep_get_vc(ep, dest_addr, &vc);
	if (rc) {
		GNIX_INFO(FI_LOG_EP_DATA,
			  "_gnix_ep_get_vc() failed, addr: %lx, rc:\n",
			  dest_addr, rc);
		return rc;
	}

	/* setup fabric request */
	req = _gnix_fr_alloc(ep);
	if (!req) {
		GNIX_INFO(FI_LOG_EP_DATA, "_gnix_fr_alloc() failed\n");
		return -FI_ENOSPC;
	}

	req->type = fr_type;
	req->gnix_ep = ep;
	req->vc = vc;
	req->completer_fn = __gnix_rma_fab_req_complete;
	req->completer_data = req;
	req->user_context = context;
	req->send_fn = _gnix_rma_post_req;

	md = container_of(mdesc, struct gnix_fid_mem_desc, mr_fid);
	req->loc_addr = loc_addr;
	req->rma.loc_md = (void *)md;
	req->rma.rem_addr = rem_addr;
	req->rma.rem_mr_key = mkey;
	req->len = len;
	req->flags = flags;

	/* Inject interfaces always suppress completions.  If
	 * SELECTIVE_COMPLETION is set, honor any setting.  Otherwise, always
	 * deliver a completion. */
	if ((flags & GNIX_SUPPRESS_COMPLETION) ||
	    (ep->send_selective_completion && !(flags & FI_COMPLETION))) {
		req->flags &= ~FI_COMPLETION;
	} else {
		req->flags |= FI_COMPLETION;
	}

	return _gnix_vc_queue_tx_req(req);
}

