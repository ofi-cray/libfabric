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

	atomic_dec(&req->vc->outstanding_tx_reqs);

	/* We could have requests waiting for TXDs or FI_FENCE operations.
	 * Schedule this VC to push any such TXs. */
	_gnix_vc_schedule_tx(req->vc);

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

/*******************************************************************************
 * Generic EP recv handling
 ******************************************************************************/

/* TODO: this function is somewhat of a placeholder till all of the
 * message pathways are coded. */
int _gnix_ep_eager_msg_w_data_match(struct gnix_fid_ep *ep, void *msg,
				   struct gnix_address addr, size_t len,
				   uint64_t imm, uint64_t sflags)
{
	int matched = 0, ret = FI_SUCCESS;
	struct slist_entry *item = NULL;
	struct gnix_fab_req *req;
	struct gnix_fid_cq *cq;
	ssize_t cq_len;
	uint64_t flags;
	struct gnix_address *addr_ptr;

	flags = (sflags & FI_REMOTE_CQ_DATA) ? FI_REMOTE_CQ_DATA : 0;

	/*
	 * we have to keep this lock till either we find a match
	 * or we add the request to the tail of the unexpected queue
	 */

	addr_ptr = (struct gnix_address *)&addr;
	fastlock_acquire(&ep->recv_queue_lock);

	item = slist_remove_first_match(&ep->posted_recv_queue,
					__msg_match_fab_req,
					(const void *)addr_ptr);
	if (item) {
		req = container_of(item, struct gnix_fab_req, slist);
		memcpy((void *)req->loc_addr, msg, MIN(req->len, len));
		req->addr = addr;
		req->imm = imm;
		req->len = MIN(req->len, len);
		req->modes |= (GNIX_FAB_RQ_M_MATCHED |
					GNIX_FAB_RQ_M_COMPLETE);
		matched = 1;
	}

	/*
	 * post to cq and free fab req, otherwise put on unexpected queue
	 */

	if (matched) {

		cq = ep->recv_cq;
		assert(cq != NULL);

		/*
		 * if no previously posted receives that are pending
		 * completion, we can go ahead and post CQ to
		 * libfabric cq for this ep.  This check is required
		 * to make sure the SAS ordering GNI provider promises
		 * actually is obeyed on the receiving end.
		 */

		if (slist_empty(&ep->pending_recv_comp_queue)) {

			/*
			 * TODO: eventually need to deal with
			 * FI_SELECTIVE_COMPLETION
			 */
			cq_len = _gnix_cq_add_event(cq,
						    req->user_context,
						    flags | FI_RECV | FI_MSG,
						    req->len,
						    (void *)req->loc_addr,
						    req->imm,
						    0);
			if (cq_len != FI_SUCCESS)  {
				GNIX_WARN((FI_LOG_CQ | FI_LOG_EP_DATA),
					  "_gnix_cq_add_event returned %d\n",
					   cq_len);
				ret = (int)cq_len; /* ugh */
			}

			if (ep->recv_cntr) {
				ret = _gnix_cntr_inc(ep->recv_cntr);
				if (ret != FI_SUCCESS)
					GNIX_WARN(FI_LOG_CQ,
					  "_gnix_cntr_inc() failed: %d\n", ret);
			}

			_gnix_fr_free(ep, req);

		} else
			gnix_slist_insert_tail(&req->slist,
					       &ep->pending_recv_comp_queue);

	} else {

		req = _gnix_fr_alloc(ep);
		if (req == NULL) {
			ret = -FI_ENOMEM;
			goto err;
		}

		req->loc_addr = (uint64_t)malloc(len);
		if (req->loc_addr == 0UL) {
			_gnix_fr_free(ep, req);
			ret = -FI_ENOMEM;
			goto err;
		}

		memcpy((void *)req->loc_addr, msg, len);
		req->addr = addr;
		req->imm = imm;
		req->len = len;
		req->cq_flags = flags;
		req->modes = GNIX_FAB_RQ_M_UNEXPECTED | GNIX_FAB_RQ_M_COMPLETE;
		req->type = GNIX_FAB_RQ_RECV;

		gnix_slist_insert_tail(&req->slist,
					&ep->unexp_recv_queue);
	}

err:
	fastlock_release(&ep->recv_queue_lock);
	return ret;
}

static struct gnix_fab_req *__gnix_recv_match(struct gnix_fid_ep *ep,
					      struct gnix_address *addr,
					      uint64_t flags, uint64_t tag,
					      uint64_t ignore)
{
	struct gnix_fab_req *req = NULL;
	struct slist_entry *item;

	if (flags & GNIX_MSG_TAGGED) {
		/* TODO */
		assert(0);
	} else if (ep->type == FI_EP_RDM) {
		/* Search the EP's unexpected list for a match. */
		item = slist_remove_first_match(&ep->unexp_recv_queue,
						__msg_match_fab_req,
						addr);
		if (item) {
			req = container_of(item, struct gnix_fab_req, slist);
			req->modes |= GNIX_FAB_RQ_M_MATCHED;
		}
	} else if (ep->type == FI_EP_MSG) {
		/* Take the first element off of the unexpected list. */
		item = slist_remove_head(&ep->unexp_recv_queue);
		if (item) {
			req = container_of(item, struct gnix_fab_req, slist);
			req->modes |= GNIX_FAB_RQ_M_MATCHED;
		}
	}

	return req;
}

ssize_t _gnix_recv(struct gnix_fid_ep *ep, uint64_t buf, size_t len, void *desc,
		   uint64_t src_addr, void *context, uint64_t flags,
		   uint64_t tag, uint64_t ignore)
{
	int ret = FI_SUCCESS;
	int sched_req = 0;
	struct gnix_fid_cq *cq;
	struct gnix_fid_av *av;
	struct gnix_fab_req *req = NULL;
	ssize_t cq_len;
	struct gnix_address *addr_ptr = NULL;
	fi_addr_t real_addr;

	/* TODO make generic address lookup function */
	/* TODO ignore src_addr unless FI_DIRECT_RECV */
	if (ep->type == FI_EP_RDM) {
		av = ep->av;
		assert(av != NULL);
		if (av->type == FI_AV_TABLE) {
			ret = _gnix_table_addr_to_map_addr(av, src_addr, &real_addr);
			if (ret != FI_SUCCESS) {
				GNIX_WARN(FI_LOG_AV,
					"_gnix_table_addr_to_map_addr"
					" returned %d\n",
					ret);
				return ret;
				}
			addr_ptr = (struct gnix_address*)&real_addr;
		} else
			addr_ptr = (struct gnix_address *)&src_addr;
	} else {
		addr_ptr = (struct gnix_address *)&src_addr;
	}
	assert(addr_ptr != NULL);

	fastlock_acquire(&ep->recv_queue_lock);
	req = __gnix_recv_match(ep, addr_ptr, flags, tag, ignore);

	if (req) {
		if (req->modes & GNIX_FAB_RQ_M_COMPLETE) {
			memcpy((void *)buf, (void *)req->loc_addr,
			       MIN(req->len, len));
			free((void *)req->loc_addr);
			req->loc_addr = 0UL;
		}

		cq = ep->recv_cq;
		assert(cq != NULL);

		if (slist_empty(&ep->pending_recv_comp_queue) &&
			(req->modes & GNIX_FAB_RQ_M_COMPLETE)) {

			/*
			 * TODO: eventually need to deal with
			 * FI_SELECTIVE_COMPLETION
			 */
			cq_len = _gnix_cq_add_event(cq,
						    context,
						    req->cq_flags |
							FI_RECV | FI_MSG,
						    MIN(req->len, len),
						    (void *)buf,
						    req->imm,
						    0);
			if (cq_len != FI_SUCCESS)  {
				GNIX_WARN((FI_LOG_CQ | FI_LOG_EP_DATA),
					  "_gnix_cq_add_event returned %d\n",
					   cq_len);
				ret = (int)cq_len; /* ugh */
			}

			if (ep->recv_cntr) {
				ret = _gnix_cntr_inc(ep->recv_cntr);
				if (ret != FI_SUCCESS)
					GNIX_WARN(FI_LOG_CQ,
					  "_gnix_cntr_inc() failed: %d\n", ret);
			}

			_gnix_fr_free(ep, req);
		} else {
			gnix_slist_insert_tail(&req->slist,
					&ep->pending_recv_comp_queue);
			sched_req = 1;
		}

	} else {

		req = _gnix_fr_alloc(ep);
		if (req == NULL) {
			ret = -FI_EAGAIN;
			goto err;
		}

		req->addr = *addr_ptr;
		req->len = len;
		req->loc_addr = (uint64_t)buf;
		req->type = GNIX_FAB_RQ_RECV;
		req->user_context = context;
		gnix_slist_insert_tail(&req->slist,
				       &ep->posted_recv_queue);
	}

err:
	fastlock_release(&ep->recv_queue_lock);
	if (sched_req) {
		/*
		 * TODO: schedule completion of req if not completed
		 */
	}

	return ret;
}

/*******************************************************************************
 * Generic EP send handling
 ******************************************************************************/

static int _gnix_send_req(void *data)
{
	struct gnix_fab_req *req = (struct gnix_fab_req *)data;
	struct gnix_nic *nic;
	struct gnix_fid_ep *ep;
	struct gnix_tx_descriptor *tdesc;
	gni_return_t status;
	int rc;
	int rendezvous = !!(req->flags & GNIX_MSG_RENDEZVOUS);
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

	if (rendezvous) {
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

#define GNIX_MSG_RENDEZVOUS_THRESH (16*1024)

ssize_t _gnix_send(struct gnix_fid_ep *ep, uint64_t loc_addr, size_t len,
		   void *mdesc, uint64_t dest_addr, void *context,
		   uint64_t flags, uint64_t data)
{
	int ret = FI_SUCCESS;
	struct gnix_vc *vc = NULL;
	struct gnix_fab_req *req;
	struct gnix_fid_mem_desc *md = NULL;
	int rendezvous;

	if (!ep) {
		return -FI_EINVAL;
	}

	if ((flags & FI_INJECT) && (len > GNIX_INJECT_SIZE)) {
		GNIX_INFO(FI_LOG_EP_DATA,
			  "Send length %d exceeds inject max size: %d\n",
			  len, GNIX_INJECT_SIZE);
		return -FI_EINVAL;
	}

	rendezvous = len >= GNIX_MSG_RENDEZVOUS_THRESH;

	/* need a memory descriptor for large sends */
	if (rendezvous && !mdesc) {
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

	if (rendezvous) {
		req->flags |= GNIX_MSG_RENDEZVOUS;
	}

	return _gnix_vc_queue_tx_req(req);
}

