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
#include "gnix_cm_nic.h"
#include "gnix_nic.h"
#include "gnix_util.h"
#include "gnix_ep.h"
#include "gnix_hashtable.h"
#include "gnix_vc.h"
#include "gnix_cntr.h"

/*******************************************************************************
 * GNI SMSG callbacks invoked upon completion of an SMSG message at the sender.
 ******************************************************************************/

static int __comp_eager_msg_w_data(void *data)
{
	int ret = FI_SUCCESS;
	struct gnix_tx_descriptor *tdesc;
	struct gnix_fid_ep *ep;
	struct gnix_fid_cq *cq;
	struct fi_context *user_context;
	ssize_t cq_len;
	struct gnix_fab_req *req;

	tdesc = (struct gnix_tx_descriptor *)data;
	req = tdesc->desc.req;

	ep = tdesc->desc.ep;
	assert(ep != NULL);

	cq = ep->send_cq;
	assert(cq != NULL);

	if (req != NULL)
		user_context = req->user_context;
	else
		user_context = tdesc->desc.context;

	cq_len = _gnix_cq_add_event(cq,
				    user_context,
				    FI_SEND | FI_MSG,
				    0,
				    0,
				    0,
				    0);
	if (cq_len != FI_SUCCESS)  {
		GNIX_WARN((FI_LOG_CQ | FI_LOG_EP_DATA),
			  "_gnix_cq_add_event returned %d\n",
			   cq_len);
		ret = (int)cq_len; /* ugh */
	}

	if (ep->send_cntr) {
		ret = _gnix_cntr_inc(ep->send_cntr);
		if (ret != FI_SUCCESS)
			GNIX_WARN(FI_LOG_CQ,
			  "_gnix_cntr_inc returned %d\n",
			   ret);
	}

	/* We could have requests waiting for TXDs or FI_FENCE operations.  Try
	 * to push the queue now. */
	atomic_dec(&req->vc->outstanding_tx_reqs);
	_gnix_vc_push_tx_reqs(req->vc);

	_gnix_fr_free(ep, req);

	return ret;

}

static int __comp_eager_msg_w_data_ack(void *data)
{
	return -FI_ENOSYS;
}

static int __comp_eager_msg_data_at_src(void *data)
{
	return -FI_ENOSYS;
}

static int __comp_eager_msg_data_at_src_ack(void *data)
{
	return -FI_ENOSYS;
}

static int __comp_rndzv_msg_rts(void *data)
{
	return -FI_ENOSYS;
}

static int __comp_rndzv_msg_rtr(void *data)
{
	return -FI_ENOSYS;
}

static int __comp_rndzv_msg_cookie(void *data)
{
	return -FI_ENOSYS;
}

static int __comp_rndzv_msg_send_done(void *data)
{
	return -FI_ENOSYS;
}

static int __comp_rndzv_msg_recv_done(void *data)
{
	return -FI_ENOSYS;
}

smsg_completer_fn_t gnix_ep_smsg_completers[] = {
	[GNIX_SMSG_T_EGR_W_DATA] = __comp_eager_msg_w_data,
	[GNIX_SMSG_T_EGR_W_DATA_ACK] = __comp_eager_msg_w_data_ack,
	[GNIX_SMSG_T_EGR_GET] = __comp_eager_msg_data_at_src,
	[GNIX_SMSG_T_EGR_GET_ACK] = __comp_eager_msg_data_at_src_ack,
	[GNIX_SMSG_T_RNDZV_RTS] = __comp_rndzv_msg_rts,
	[GNIX_SMSG_T_RNDZV_RTR] = __comp_rndzv_msg_rtr,
	[GNIX_SMSG_T_RNDZV_COOKIE] = __comp_rndzv_msg_cookie,
	[GNIX_SMSG_T_RNDZV_SDONE] = __comp_rndzv_msg_send_done,
	[GNIX_SMSG_T_RNDZV_RDONE] = __comp_rndzv_msg_recv_done
};

static int _gnix_send_req(void *data)
{
	struct gnix_fab_req *req = (struct gnix_fab_req *)data;
	struct gnix_nic *nic;
	struct gnix_fid_ep *ep;
	struct gnix_tx_descriptor *tdesc;
	gni_return_t status;
	int rc;
	int rendevous = !!(req->flags & GNIX_MSG_RENDEVOUS);
	int len;

	ep = req->gnix_ep;
	assert(ep != NULL);

	nic = ep->nic;
	assert(nic != NULL);

	rc = _gnix_nic_tx_alloc(nic, &tdesc);
	if (rc != FI_SUCCESS) {
		GNIX_INFO(FI_LOG_EP_DATA, "_gnix_nic_tx_alloc() failed: %d\n",
			  rc);
		return -FI_EAGAIN;
	}
	assert(rc == FI_SUCCESS);

	tdesc->desc.req = req;
	tdesc->desc.ep = ep;
	tdesc->desc.completer_fn =
			gnix_ep_smsg_completers[GNIX_SMSG_T_EGR_W_DATA];

	if (rendevous) {
		/* Fill SMSG header with local buffer info. */

		len = 0;
		assert(0);
	} else {
		tdesc->desc.smsg_desc.hdr.len = req->len;
		tdesc->desc.smsg_desc.hdr.flags = 0;
		tdesc->desc.smsg_desc.buf = (void *)req->loc_addr;
		len = req->len;
	}

	fastlock_acquire(&nic->lock);

	status = GNI_SmsgSendWTag(req->vc->gni_ep,
			&tdesc->desc.smsg_desc.hdr,
			sizeof(struct gnix_smsg_hdr),
			(void *)req->loc_addr, len,
			tdesc->desc.id,
			GNIX_SMSG_T_EGR_W_DATA);

	fastlock_release(&nic->lock);

	if (status == GNI_RC_NOT_DONE) {
		_gnix_nic_tx_free(nic, tdesc);
		GNIX_INFO(FI_LOG_EP_DATA,
			  "GNI_SmsgSendWTag returned %s\n",
			  gni_err_str[status]);
	} else if (status != GNI_RC_SUCCESS) {
		_gnix_nic_tx_free(nic, tdesc);
		GNIX_WARN(FI_LOG_EP_DATA,
			  "GNI_SmsgSendWTag returned %s\n",
			  gni_err_str[status]);
	}

	return gnixu_to_fi_errno(status);
}

#define GNIX_MSG_RENDEVOUS_THRESH (16*1024)

ssize_t _gnix_send(struct gnix_fid_ep *ep, uint64_t loc_addr, size_t len,
		   void *mdesc, uint64_t dest_addr, void *context,
		   uint64_t flags, uint64_t data)
{
	int ret = FI_SUCCESS;
	struct gnix_vc *vc = NULL;
	struct gnix_fab_req *req;
	struct gnix_fid_mem_desc *md = NULL;
	int rendevous;

	if (!ep) {
		return -FI_EINVAL;
	}

	if ((flags & FI_INJECT) && (len > GNIX_INJECT_SIZE)) {
		GNIX_INFO(FI_LOG_EP_DATA,
			  "Send length %d exceeds inject max size: %d\n",
			  len, GNIX_INJECT_SIZE);
		return -FI_EINVAL;
	}

	rendevous = len >= GNIX_MSG_RENDEVOUS_THRESH;

	/* need a memory descriptor for large sends */
	if (rendevous && !mdesc) {
		GNIX_INFO(FI_LOG_EP_DATA,
			  "Send of length %d requires memory descriptor\n",
			  len);
		return -FI_EINVAL;
	}

	ret = _gnix_ep_get_vc(ep, dest_addr, &vc);
	if (ret) {
		return ret;
	}

	req = _gnix_fr_alloc(ep);
	if (req == NULL)
		return -FI_EAGAIN;

	req->type = GNIX_FAB_RQ_SEND;
	req->gnix_ep = ep;
	req->vc = vc;
	req->user_context = context;
	req->send_fn = _gnix_send_req;

	if (mdesc) {
		md = container_of(mdesc, struct gnix_fid_mem_desc, mr_fid);
	}
	req->loc_md = (void *)md;

	req->len = len;
	req->flags = flags;

	if (req->flags & FI_INJECT) {
		memcpy(req->inject_buf, (void *)loc_addr, len);
		req->loc_addr = (uint64_t)req->inject_buf;
	} else {
		req->loc_addr = loc_addr;
	}

	if (rendevous) {
		req->flags |= GNIX_MSG_RENDEVOUS;
	}

	return _gnix_vc_queue_tx_req(req);
}

