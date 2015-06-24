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

/*
 * Endpoint common code
 */
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "gnix.h"
#include "gnix_cm_nic.h"
#include "gnix_nic.h"
#include "gnix_util.h"
#include "gnix_ep.h"
#include "gnix_ep_rdm.h"
#include "gnix_ep_msg.h"
#include "gnix_hashtable.h"
#include "gnix_vc.h"
#include "gnix_rma.h"


/*******************************************************************************
 * gnix_fab_req freelist functions
 *
 * These are wrappers around the gnix_s_freelist
 *
 ******************************************************************************/

#define GNIX_FAB_REQ_FL_MIN_SIZE 100
#define GNIX_FAB_REQ_FL_REFILL_SIZE 10

static int __fr_freelist_init(struct gnix_fid_ep *ep)
{
	assert(ep);
	return _gnix_sfl_init(sizeof(struct gnix_fab_req),
			      offsetof(struct gnix_fab_req, slist),
			      GNIX_FAB_REQ_FL_MIN_SIZE,
			      GNIX_FAB_REQ_FL_REFILL_SIZE,
			      0, 0, &ep->fr_freelist);
}

static void __fr_freelist_destroy(struct gnix_fid_ep *ep)
{
	assert(ep);
	_gnix_sfl_destroy(&ep->fr_freelist);
}

/*******************************************************************************
 * GNI SMSG callbacks invoked upon receipt of an SMSG message.
 * These callback functions are invoked with the lock for the nic
 * associated with the vc already held.
 ******************************************************************************/

static int __smsg_eager_msg_w_data(void *data, void *msg)
{
	int ret = FI_SUCCESS;
	gni_return_t status;
	struct gnix_vc *vc = (struct gnix_vc *)data;
	struct gnix_smsg_hdr *hdr = (struct gnix_smsg_hdr *)msg;
	struct gnix_fid_ep *ep;

	ep = vc->ep;
	assert(ep);

	/*
	 * the msg data must be consumed either by matching
	 * a message or allocating a buffer and putting
	 * on unexpected queue.
	 */

	ret = _gnix_ep_eager_msg_w_data_match(ep,
					(void *)((char *)msg + sizeof(*hdr)),
					vc->peer_addr,
					hdr->len,
					hdr->imm,
					hdr->flags);

	/*
	 * we keep on going even if we got an error back because
	 * we need to release the pending message in the SMSG buffer
	 * and unlock the nic lock
	 */
	if (ret != FI_SUCCESS)
		GNIX_WARN(FI_LOG_EP_DATA,
				"_gnix_ep_eager_msg_rcv returned %d\n",
				ret);

	status = GNI_SmsgRelease(vc->gni_ep);
	if (unlikely(status != GNI_RC_SUCCESS)) {
		GNIX_WARN(FI_LOG_EP_DATA,
				"GNI_SmsgRelease returned %s\n",
				gni_err_str[status]);
		ret = gnixu_to_fi_errno(status);
		goto err;
	}

err:
	return ret;
}

/*
 * this function will probably not be used unless we need
 * some kind of explicit flow control to handle unexpected
 * receives
 */

static int __smsg_eager_msg_w_data_ack(void *data, void *msg)
{
	return -FI_ENOSYS;
}

static int __smsg_eager_msg_data_at_src(void *data, void *msg)
{
	return -FI_ENOSYS;
}

static int  __smsg_eager_msg_data_at_src_ack(void *data, void *msg)
{
	return -FI_ENOSYS;
}

static int __smsg_rndzv_msg_rts(void *data, void *msg)
{
	return -FI_ENOSYS;
}

static int __smsg_rndzv_msg_rtr(void *data, void *msg)
{
	return -FI_ENOSYS;
}

static int __smsg_rndzv_msg_cookie(void *data, void *msg)
{
	return -FI_ENOSYS;
}

static int __smsg_rndzv_msg_send_done(void *data, void *msg)
{
	return -FI_ENOSYS;
}

static int __smsg_rndzv_msg_recv_done(void *data, void *msg)
{
	return -FI_ENOSYS;
}

static smsg_callback_fn_t gnix_ep_smsg_callbacks[] = {
	[GNIX_SMSG_T_EGR_W_DATA] = __smsg_eager_msg_w_data,
	[GNIX_SMSG_T_EGR_W_DATA_ACK] = __smsg_eager_msg_w_data_ack,
	[GNIX_SMSG_T_EGR_GET] = __smsg_eager_msg_data_at_src,
	[GNIX_SMSG_T_EGR_GET_ACK] = __smsg_eager_msg_data_at_src_ack,
	[GNIX_SMSG_T_RNDZV_RTS] = __smsg_rndzv_msg_rts,
	[GNIX_SMSG_T_RNDZV_RTR] = __smsg_rndzv_msg_rtr,
	[GNIX_SMSG_T_RNDZV_COOKIE] = __smsg_rndzv_msg_cookie,
	[GNIX_SMSG_T_RNDZV_SDONE] = __smsg_rndzv_msg_send_done,
	[GNIX_SMSG_T_RNDZV_RDONE] = __smsg_rndzv_msg_recv_done
};

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

	tdesc = (struct gnix_tx_descriptor *)data;

	ep = tdesc->desc.ep;
	assert(ep != NULL);

	cq = ep->send_cq;
	assert(cq != NULL);

	if (tdesc->desc.req != NULL)
		user_context = tdesc->desc.req->user_context;
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

	ret = _gnix_nic_tx_free(ep->nic, tdesc);

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
 * Forward declaration for ops structures.
 ******************************************************************************/

static struct fi_ops gnix_ep_fi_ops;
static struct fi_ops_ep gnix_ep_ops;
static struct fi_ops_msg gnix_ep_msg_ops;
static struct fi_ops_rma gnix_ep_rma_ops;
struct fi_ops_tagged gnix_ep_tagged_ops;

/*******************************************************************************
 * EP messaging API function implementations.
 ******************************************************************************/

static ssize_t gnix_ep_recv(struct fid_ep *ep, void *buf, size_t len,
			    void *desc, fi_addr_t src_addr, void *context)
{
	int ret = FI_SUCCESS;
	int sched_req = 0;
	struct gnix_fid_ep *ep_priv;
	struct gnix_fid_cq *cq;
	struct gnix_fid_av *av;
	struct slist_entry *item;
	struct slist *list;
	struct gnix_fab_req *req = NULL;
	int matched = 0;
	ssize_t cq_len;
	struct gnix_address *addr_ptr = NULL;

	ep_priv = container_of(ep, struct gnix_fid_ep, ep_fid);
	if (unlikely((ep_priv->type != FI_EP_RDM) &&
				(ep_priv->type != FI_EP_MSG)))
		return -FI_EINVAL;

	if (ep_priv->type == FI_EP_RDM) {
		av = ep_priv->av;
		assert(av != NULL);
		if (av->type == FI_AV_TABLE) {
			/*
			 * TODO: look up gni address -
			 *        just return no support for now
			 */
			return -FI_ENOSYS;
		} else
			addr_ptr = (struct gnix_address *)&src_addr;
	} else {
		addr_ptr = (struct gnix_address *)&src_addr;
	}

	assert(addr_ptr != NULL);

	/*
	 * if type FI_EP_RDM, search unexpected on this ep to see if we
	 * already have a match. For FI_EP_MSG, just take first element
	 * of the unexpected list.
	 */

	list = &ep_priv->unexp_recv_queue;
	fastlock_acquire(&ep_priv->recv_queue_lock);

	if (ep_priv->type == FI_EP_RDM) {

		item = slist_remove_first_match(&ep_priv->unexp_recv_queue,
						__msg_match_fab_req,
						(const void *)addr_ptr);
		if (item != NULL) {
			req = container_of(item, struct gnix_fab_req, slist);
			req->modes |= GNIX_FAB_RQ_M_MATCHED;
			matched = 1;
		}

	} else if (ep_priv->type == FI_EP_MSG) {

		item = slist_remove_head(list);
		if (item) {
			req = container_of(item, struct gnix_fab_req, slist);
			req->modes |= GNIX_FAB_RQ_M_MATCHED;
			matched = 1;
		}
	}

	if (matched) {

		if (req->modes & GNIX_FAB_RQ_M_COMPLETE) {
			memcpy(buf, (void *)req->loc_addr, MIN(req->len, len));
			free((void *)req->loc_addr);
			req->loc_addr = 0UL;
		}

		cq = ep_priv->recv_cq;
		assert(cq != NULL);

		if (slist_empty(&ep_priv->pending_recv_comp_queue) &&
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
						    buf,
						    req->imm,
						    0);
			if (cq_len != FI_SUCCESS)  {
				GNIX_WARN((FI_LOG_CQ | FI_LOG_EP_DATA),
					  "_gnix_cq_add_event returned %d\n",
					   cq_len);
				ret = (int)cq_len; /* ugh */
			}

			_gnix_fr_free(ep_priv, req);
		} else {
			slist_insert_tail(&req->slist,
					&ep_priv->pending_recv_comp_queue);
			sched_req = 1;
		}

	} else {

		req = _gnix_fr_alloc(ep_priv);
		if (req == NULL) {
			ret = -FI_EAGAIN;
			goto err;
		}

		req->addr = *addr_ptr;
		req->len = len;
		req->loc_addr = (uint64_t)buf;
		req->type = GNIX_FAB_RQ_RECV;
		req->user_context = context;
		slist_insert_tail(&req->slist,
				  &ep_priv->posted_recv_queue);
	}

err:
	fastlock_release(&ep_priv->recv_queue_lock);
	if (sched_req) {
		/*
		 * TODO: schedule completion of req if not completed
		 */
	}

	return ret;
}

static const recvv_func_t const recvv_method[] = {
					[FI_EP_MSG] = gnix_ep_recvv_msg,
					[FI_EP_RDM] = gnix_ep_recvv_rdm,
					};

static ssize_t gnix_ep_recvv(struct fid_ep *ep, const struct iovec *iov,
			     void **desc, size_t count, fi_addr_t src_addr,
			     void *context)
{
	struct gnix_fid_ep *ep_priv;

	ep_priv = container_of(ep, struct gnix_fid_ep, ep_fid);
	assert((ep_priv->type == FI_EP_RDM) || (ep_priv->type == FI_EP_MSG));

	return recvv_method[ep_priv->type](ep,
					   iov,
					   desc,
					   count,
					   src_addr,
					   context);
}

static const recvmsg_func_t const recvmsg_method[] = {
					[FI_EP_MSG] = gnix_ep_recvmsg_msg,
					[FI_EP_RDM] = gnix_ep_recvmsg_rdm,
					};

static ssize_t gnix_ep_recvmsg(struct fid_ep *ep, const struct fi_msg *msg,
			uint64_t flags)
{
	struct gnix_fid_ep *ep_priv;

	ep_priv = container_of(ep, struct gnix_fid_ep, ep_fid);
	assert((ep_priv->type == FI_EP_RDM) || (ep_priv->type == FI_EP_MSG));

	return recvmsg_method[ep_priv->type](ep,
					     msg,
					     flags);

}

int _gnix_send_req(void *data)
{
	struct gnix_fab_req *req = (struct gnix_fab_req *)data;
	struct gnix_nic *nic;
	struct gnix_fid_ep *ep;
	struct gnix_tx_descriptor *tdesc;
	gni_return_t status;
	int ret;

	ep = req->gnix_ep;
	assert(ep != NULL);

	nic = ep->nic;
	assert(nic != NULL);

	if (req->len < (16384 - sizeof(struct gnix_smsg_hdr))) {
		/*
		 * pure smsg path
		 */
		ret = _gnix_nic_tx_alloc(nic, &tdesc);
		if (ret == -FI_ENOSPC)
			return ret;
		assert(ret == FI_SUCCESS);

		tdesc->desc.smsg_desc.hdr.len = req->len;
		tdesc->desc.smsg_desc.hdr.flags = 0;
		tdesc->desc.smsg_desc.buf = (void *)req->loc_addr;
		tdesc->desc.req = req;
		tdesc->desc.ep = ep;
		tdesc->desc.completer_fn =
				gnix_ep_smsg_completers[GNIX_SMSG_T_EGR_W_DATA];
		fastlock_acquire(&nic->lock);
		status = GNI_SmsgSendWTag(req->vc->gni_ep,
					  &tdesc->desc.smsg_desc.hdr,
					  sizeof(struct gnix_smsg_hdr),
					  (void *)req->loc_addr,
					  req->len,
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
		ret = gnixu_to_fi_errno(status);
	} else {
		ret = -FI_ENOSYS;  /* only smsg path for now */
	}

	return ret;
}

static ssize_t gnix_ep_send(struct fid_ep *ep, const void *buf, size_t len,
			    void *desc, fi_addr_t dest_addr, void *context)
{
	int ret = FI_SUCCESS;
	struct gnix_vc *vc = NULL;
	struct gnix_nic *nic = NULL;
	struct gnix_fid_ep *ep_priv = NULL;
	struct gnix_fab_req *req;
	struct gnix_address *addr_ptr;

	ep_priv = container_of(ep, struct gnix_fid_ep, ep_fid);

	nic = ep_priv->nic;
	if (nic == NULL)
		return -FI_EINVAL;

	ret = _gnix_ep_get_vc(ep_priv, dest_addr, &vc);
	if (ret) {
		return ret;
	}

	req = _gnix_fr_alloc(ep_priv);
	if (req == NULL)
		return -FI_EAGAIN;

	req->type = GNIX_FAB_RQ_SEND;
	req->send_fn = _gnix_send_req;
	addr_ptr = (struct gnix_address *)&dest_addr;
	req->addr = *addr_ptr;
	req->loc_addr = (uint64_t)buf;
	req->modes = 0;
	req->gnix_ep = ep_priv;
	if (desc != NULL)
		req->rma.loc_md = desc;
	req->len = len;
	req->user_context = context;
	req->vc = vc;

	return _gnix_vc_queue_tx_req(req);
}

static const sendv_func_t const sendv_method[] = {
					[FI_EP_MSG] = gnix_ep_sendv_msg,
					[FI_EP_RDM] = gnix_ep_sendv_rdm,
					};

static ssize_t gnix_ep_sendv(struct fid_ep *ep, const struct iovec *iov,
			     void **desc, size_t count, fi_addr_t dest_addr,
			     void *context)
{
	struct gnix_fid_ep *ep_priv;

	ep_priv = container_of(ep, struct gnix_fid_ep, ep_fid);
	assert((ep_priv->type == FI_EP_RDM) || (ep_priv->type == FI_EP_MSG));

	return sendv_method[ep_priv->type](ep,
					   iov,
					   desc,
					   count,
					   dest_addr,
					   context);
}

static const sendmsg_func_t const sendmsg_method[] = {
					[FI_EP_MSG] = gnix_ep_sendmsg_msg,
					[FI_EP_RDM] = gnix_ep_sendmsg_rdm,
					};

static ssize_t gnix_ep_sendmsg(struct fid_ep *ep, const struct fi_msg *msg,
			uint64_t flags)
{
	struct gnix_fid_ep *ep_priv;

	ep_priv = container_of(ep, struct gnix_fid_ep, ep_fid);
	assert((ep_priv->type == FI_EP_RDM) || (ep_priv->type == FI_EP_MSG));

	return sendmsg_method[ep_priv->type](ep,
					     msg,
					     flags);
}

static const msg_inject_func_t const msg_inject_method[] = {
					  [FI_EP_MSG] = gnix_ep_msg_inject_msg,
					  [FI_EP_RDM] = gnix_ep_msg_inject_rdm,
					};

static ssize_t gnix_ep_msg_inject(struct fid_ep *ep, const void *buf,
				  size_t len, fi_addr_t dest_addr)
{
	struct gnix_fid_ep *ep_priv;

	ep_priv = container_of(ep, struct gnix_fid_ep, ep_fid);
	assert((ep_priv->type == FI_EP_RDM) || (ep_priv->type == FI_EP_MSG));

	return msg_inject_method[ep_priv->type](ep,
						buf,
						len,
						dest_addr);
}

ssize_t gnix_ep_senddata(struct fid_ep *ep, const void *buf, size_t len,
				uint64_t data, fi_addr_t dest_addr)
{
	return -FI_ENOSYS;
}

/*******************************************************************************
 * EP RMA API function implementations.
 ******************************************************************************/

static ssize_t gnix_ep_read(struct fid_ep *ep, void *buf, size_t len,
			    void *desc, fi_addr_t src_addr, uint64_t addr,
			    uint64_t key, void *context)
{
	struct gnix_fid_ep *gnix_ep;

	if (!ep) {
		return -FI_EINVAL;
	}

	gnix_ep = container_of(ep, struct gnix_fid_ep, ep_fid);
	assert((gnix_ep->type == FI_EP_RDM) || (gnix_ep->type == FI_EP_MSG));

	return _gnix_rma(gnix_ep, GNIX_FAB_RQ_RDMA_READ,
			 (uint64_t)buf, len, desc,
			 src_addr, addr, key,
			 context, gnix_ep->op_flags, 0);
}

static ssize_t gnix_ep_readv(struct fid_ep *ep, const struct iovec *iov,
				void **desc, size_t count, fi_addr_t src_addr,
				uint64_t addr, uint64_t key, void *context)
{
	struct gnix_fid_ep *gnix_ep;

	if (!ep || !iov || !desc || count != 1) {
		return -FI_EINVAL;
	}

	gnix_ep = container_of(ep, struct gnix_fid_ep, ep_fid);
	assert((gnix_ep->type == FI_EP_RDM) || (gnix_ep->type == FI_EP_MSG));

	return _gnix_rma(gnix_ep, GNIX_FAB_RQ_RDMA_READ,
			 (uint64_t)iov[0].iov_base, iov[0].iov_len, desc[0],
			 src_addr, addr, key,
			 context, gnix_ep->op_flags, 0);
}

static ssize_t gnix_ep_readmsg(struct fid_ep *ep, const struct fi_msg_rma *msg,
				uint64_t flags)
{
	struct gnix_fid_ep *gnix_ep;

	if (!ep || !msg || !msg->msg_iov || !msg->rma_iov || !msg->desc ||
	    msg->iov_count != 1 || msg->rma_iov_count != 1 ||
	    msg->rma_iov[0].len > msg->msg_iov[0].iov_len) {
		return -FI_EINVAL;
	}

	gnix_ep = container_of(ep, struct gnix_fid_ep, ep_fid);
	assert((gnix_ep->type == FI_EP_RDM) || (gnix_ep->type == FI_EP_MSG));

	return _gnix_rma(gnix_ep, GNIX_FAB_RQ_RDMA_READ,
			 (uint64_t)msg->msg_iov[0].iov_base,
			 msg->msg_iov[0].iov_len, msg->desc[0],
			 msg->addr, msg->rma_iov[0].addr, msg->rma_iov[0].key,
			 msg->context, flags, msg->data);
}

static ssize_t gnix_ep_write(struct fid_ep *ep, const void *buf, size_t len,
				void *desc, fi_addr_t dest_addr, uint64_t addr,
				uint64_t key, void *context)
{
	struct gnix_fid_ep *gnix_ep;

	if (!ep) {
		return -FI_EINVAL;
	}

	gnix_ep = container_of(ep, struct gnix_fid_ep, ep_fid);
	assert((gnix_ep->type == FI_EP_RDM) || (gnix_ep->type == FI_EP_MSG));

	return _gnix_rma(gnix_ep, GNIX_FAB_RQ_RDMA_WRITE,
			 (uint64_t)buf, len, desc,
			 dest_addr, addr, key,
			 context, gnix_ep->op_flags, 0);
}

static ssize_t gnix_ep_writev(struct fid_ep *ep, const struct iovec *iov,
				void **desc, size_t count, fi_addr_t dest_addr,
				uint64_t addr, uint64_t key, void *context)
{
	struct gnix_fid_ep *gnix_ep;

	if (!ep || !iov || !desc || count != 1) {
		return -FI_EINVAL;
	}

	gnix_ep = container_of(ep, struct gnix_fid_ep, ep_fid);
	assert((gnix_ep->type == FI_EP_RDM) || (gnix_ep->type == FI_EP_MSG));

	return _gnix_rma(gnix_ep, GNIX_FAB_RQ_RDMA_WRITE,
			 (uint64_t)iov[0].iov_base, iov[0].iov_len, desc[0],
			 dest_addr, addr, key,
			 context, gnix_ep->op_flags, 0);
}

static ssize_t gnix_ep_writemsg(struct fid_ep *ep, const struct fi_msg_rma *msg,
				uint64_t flags)
{
	struct gnix_fid_ep *gnix_ep;

	if (!ep || !msg || !msg->msg_iov || !msg->rma_iov || !msg->desc ||
	    msg->iov_count != 1 || msg->rma_iov_count != 1 ||
	    msg->rma_iov[0].len > msg->msg_iov[0].iov_len) {
		return -FI_EINVAL;
	}

	gnix_ep = container_of(ep, struct gnix_fid_ep, ep_fid);
	assert((gnix_ep->type == FI_EP_RDM) || (gnix_ep->type == FI_EP_MSG));

	return _gnix_rma(gnix_ep, GNIX_FAB_RQ_RDMA_WRITE,
			 (uint64_t)msg->msg_iov[0].iov_base,
			 msg->msg_iov[0].iov_len, msg->desc[0],
			 msg->addr, msg->rma_iov[0].addr, msg->rma_iov[0].key,
			 msg->context, flags, msg->data);
}

static ssize_t gnix_ep_rma_inject(struct fid_ep *ep, const void *buf,
				  size_t len, fi_addr_t dest_addr,
				  uint64_t addr, uint64_t key)
{
	struct gnix_fid_ep *gnix_ep;
	uint64_t flags;

	if (!ep) {
		return -FI_EINVAL;
	}

	gnix_ep = container_of(ep, struct gnix_fid_ep, ep_fid);
	assert((gnix_ep->type == FI_EP_RDM) || (gnix_ep->type == FI_EP_MSG));

	flags = gnix_ep->op_flags | FI_INJECT;

	return _gnix_rma(gnix_ep, GNIX_FAB_RQ_RDMA_WRITE,
			 (uint64_t)buf, len, NULL,
			 dest_addr, addr, key,
			 NULL, flags, 0);
}

static ssize_t gnix_ep_writedata(struct fid_ep *ep, const void *buf,
				 size_t len, void *desc, uint64_t data,
				 fi_addr_t dest_addr, uint64_t addr,
				 uint64_t key, void *context)
{
	struct gnix_fid_ep *gnix_ep;
	uint64_t flags;

	if (!ep) {
		return -FI_EINVAL;
	}

	gnix_ep = container_of(ep, struct gnix_fid_ep, ep_fid);
	assert((gnix_ep->type == FI_EP_RDM) || (gnix_ep->type == FI_EP_MSG));

	flags = gnix_ep->op_flags | FI_REMOTE_CQ_DATA;

	return _gnix_rma(gnix_ep, GNIX_FAB_RQ_RDMA_WRITE,
			 (uint64_t)buf, len, desc,
			 dest_addr, addr, key,
			 context, flags, data);
}

static ssize_t gnix_ep_rma_injectdata(struct fid_ep *ep, const void *buf,
				      size_t len, uint64_t data,
				      fi_addr_t dest_addr, uint64_t addr,
				      uint64_t key)
{
	struct gnix_fid_ep *gnix_ep;
	uint64_t flags;

	if (!ep) {
		return -FI_EINVAL;
	}

	gnix_ep = container_of(ep, struct gnix_fid_ep, ep_fid);
	assert((gnix_ep->type == FI_EP_RDM) || (gnix_ep->type == FI_EP_MSG));

	flags = gnix_ep->op_flags | FI_INJECT | FI_REMOTE_CQ_DATA;

	return _gnix_rma(gnix_ep, GNIX_FAB_RQ_RDMA_WRITE,
			 (uint64_t)buf, len, NULL,
			 dest_addr, addr, key,
			 NULL, flags, data);
}




/*******************************************************************************
 * EP Tag matching API function implementations.
 ******************************************************************************/

static const trecv_func_t const trecv_method[] = {
					[FI_EP_MSG] = gnix_ep_trecv_msg,
					[FI_EP_RDM] = gnix_ep_trecv_rdm,
					};

static ssize_t gnix_ep_trecv(struct fid_ep *ep, void *buf, size_t len,
			     void *desc, fi_addr_t src_addr, uint64_t tag,
			     uint64_t ignore, void *context)
{
	struct gnix_fid_ep *ep_priv;

	ep_priv = container_of(ep, struct gnix_fid_ep, ep_fid);
	assert((ep_priv->type == FI_EP_RDM) || (ep_priv->type == FI_EP_MSG));

	return trecv_method[ep_priv->type](ep,
					  buf,
					  len,
					  desc,
					  src_addr,
					  tag,
					  ignore,
					  context);
}

static const trecvv_func_t const trecvv_method[] = {
					[FI_EP_MSG] = gnix_ep_trecvv_msg,
					[FI_EP_RDM] = gnix_ep_trecvv_rdm,
					};

static ssize_t gnix_ep_trecvv(struct fid_ep *ep, const struct iovec *iov,
			      void **desc, size_t count, fi_addr_t src_addr,
			      uint64_t tag, uint64_t ignore, void *context)
{
	struct gnix_fid_ep *ep_priv;

	ep_priv = container_of(ep, struct gnix_fid_ep, ep_fid);
	assert((ep_priv->type == FI_EP_RDM) || (ep_priv->type == FI_EP_MSG));

	return trecvv_method[ep_priv->type](ep,
					    iov,
					    desc,
					    count,
					    src_addr,
					    tag,
					    ignore,
					    context);
}

static const trecvmsg_func_t const trecvmsg_method[] = {
					[FI_EP_MSG] = gnix_ep_trecvmsg_msg,
					[FI_EP_RDM] = gnix_ep_trecvmsg_rdm,
					};

static ssize_t gnix_ep_trecvmsg(struct fid_ep *ep,
				const struct fi_msg_tagged *msg, uint64_t flags)
{
	struct gnix_fid_ep *ep_priv;

	ep_priv = container_of(ep, struct gnix_fid_ep, ep_fid);
	assert((ep_priv->type == FI_EP_RDM) || (ep_priv->type == FI_EP_MSG));

	return trecvmsg_method[ep_priv->type](ep,
					      msg,
					      flags);
}

static const tsend_func_t const tsend_method[] = {
					[FI_EP_MSG] = gnix_ep_tsend_msg,
					[FI_EP_RDM] = gnix_ep_tsend_rdm,
					};

static ssize_t gnix_ep_tsend(struct fid_ep *ep, const void *buf, size_t len, void *desc,
			fi_addr_t dest_addr, uint64_t tag, void *context)
{
	struct gnix_fid_ep *ep_priv;

	ep_priv = container_of(ep, struct gnix_fid_ep, ep_fid);
	assert((ep_priv->type == FI_EP_RDM) || (ep_priv->type == FI_EP_MSG));

	return tsend_method[ep_priv->type](ep,
					   buf,
					   len,
					   desc,
					   dest_addr,
					   tag,
					   context);
}

static const tsendv_func_t const tsendv_method[] = {
					[FI_EP_MSG] = gnix_ep_tsendv_msg,
					[FI_EP_RDM] = gnix_ep_tsendv_rdm,
					};

static ssize_t gnix_ep_tsendv(struct fid_ep *ep, const struct iovec *iov,
			      void **desc, size_t count, fi_addr_t dest_addr,
			      uint64_t tag, void *context)
{
	struct gnix_fid_ep *ep_priv;

	ep_priv = container_of(ep, struct gnix_fid_ep, ep_fid);
	assert((ep_priv->type == FI_EP_RDM) || (ep_priv->type == FI_EP_MSG));

	return tsendv_method[ep_priv->type](ep,
					    iov,
					    desc,
					    count,
					    dest_addr,
					    tag,
					    context);
}

static const tsendmsg_func_t const tsendmsg_method[] = {
					[FI_EP_MSG] = gnix_ep_tsendmsg_msg,
					[FI_EP_RDM] = gnix_ep_tsendmsg_rdm,
					};

static ssize_t gnix_ep_tsendmsg(struct fid_ep *ep,
				const struct fi_msg_tagged *msg, uint64_t flags)
{
	struct gnix_fid_ep *ep_priv;

	ep_priv = container_of(ep, struct gnix_fid_ep, ep_fid);
	assert((ep_priv->type == FI_EP_RDM) || (ep_priv->type == FI_EP_MSG));

	return tsendmsg_method[ep_priv->type](ep,
					      msg,
					      flags);
}

static const tinject_func_t const tinject_method[] = {
					[FI_EP_MSG] = gnix_ep_tinject_msg,
					[FI_EP_RDM] = gnix_ep_tinject_rdm,
					};

static ssize_t gnix_ep_tinject(struct fid_ep *ep, const void *buf, size_t len,
				fi_addr_t dest_addr, uint64_t tag)
{
	struct gnix_fid_ep *ep_priv;

	ep_priv = container_of(ep, struct gnix_fid_ep, ep_fid);
	assert((ep_priv->type == FI_EP_RDM) || (ep_priv->type == FI_EP_MSG));

	return tinject_method[ep_priv->type](ep,
					     buf,
					     len,
					     dest_addr,
					     tag);
}

ssize_t gnix_ep_tsenddata(struct fid_ep *ep, const void *buf, size_t len,
			void *desc, uint64_t data, fi_addr_t dest_addr,
			uint64_t tag, void *context)
{
	return -FI_ENOSYS;
}

/*******************************************************************************
 * Base EP API function implementations.
 ******************************************************************************/


static int gnix_ep_control(fid_t fid, int command, void *arg)
{
	int i, ret = FI_SUCCESS;
	struct gnix_fid_ep *ep;
	struct gnix_fid_domain *dom;
	struct gnix_vc *vc;

	ep = container_of(fid, struct gnix_fid_ep, ep_fid);

	switch (command) {
	/*
	 * for FI_EP_RDM, post wc datagrams now
	 */
	case FI_ENABLE:
		if (ep->type == FI_EP_RDM) {
			dom = ep->domain;
			for (i = 0; i < dom->fabric->n_wc_dgrams; i++) {
				assert(ep->recv_cq != NULL);
				ret = _gnix_vc_alloc(ep, FI_ADDR_UNSPEC, &vc);
				if (ret != FI_SUCCESS) {
					GNIX_WARN(FI_LOG_EP_CTRL,
				     "_gnix_vc_alloc call returned %d\n", ret);
					goto err;
				}
				ret = _gnix_vc_accept(vc);
				if (ret != FI_SUCCESS) {
					GNIX_WARN(FI_LOG_EP_CTRL,
						"_gnix_vc_accept returned %d\n",
						ret);
					_gnix_vc_destroy(vc);
					goto err;
				} else {
					fastlock_acquire(&ep->vc_list_lock);
					dlist_insert_tail(&vc->entry,
						       &ep->wc_vc_list);
					fastlock_release(&ep->vc_list_lock);
				}
			}
		}
		break;

	case FI_GETFIDFLAG:
	case FI_SETFIDFLAG:
	case FI_ALIAS:
	default:
		return -FI_ENOSYS;
	}

err:
	return ret;
}

static int gnix_ep_close(fid_t fid)
{
	int ret = FI_SUCCESS;
	struct gnix_fid_ep *ep;
	struct gnix_fid_domain *domain;
	struct gnix_nic *nic;
	struct gnix_vc *vc;
	struct gnix_fid_av *av;
	struct gnix_cm_nic *cm_nic;
	struct dlist_entry *p, *head;


	GNIX_TRACE(FI_LOG_EP_CTRL, "\n");

	ep = container_of(fid, struct gnix_fid_ep, ep_fid.fid);
	/* TODO: lots more stuff to do here */

	if (ep->send_cq) {
		_gnix_cq_poll_nic_rem(ep->send_cq, ep->nic);
		atomic_dec(&ep->send_cq->ref_cnt);
	}

	if (ep->recv_cq) {
		_gnix_cq_poll_nic_rem(ep->recv_cq, ep->nic);
		atomic_dec(&ep->recv_cq->ref_cnt);
	}

	domain = ep->domain;
	assert(domain != NULL);
	atomic_dec(&domain->ref_cnt);

	cm_nic = ep->cm_nic;
	assert(cm_nic != NULL);
	_gnix_cm_nic_free(cm_nic);

	nic = ep->nic;
	assert(nic != NULL);

	av = ep->av;
	if (av != NULL)
		atomic_dec(&av->ref_cnt);

	/*
	 * destroy any vc's being used by this EP.
	 */

	head = &ep->wc_vc_list;
	for (p = head->next; p != head; p = p->next) {
		vc = container_of(p, struct gnix_vc, entry);
		dlist_remove(&vc->entry);
		if (vc->conn_state == GNIX_VC_CONNECTED) {
			ret = _gnix_vc_disconnect(vc);
			if (ret != FI_SUCCESS) {
				GNIX_WARN(FI_LOG_EP_CTRL,
				    "_gnix_vc_disconnect returned %d\n",
				     ret);
				goto err;
			}
		}
		ret = _gnix_vc_destroy(vc);
		if (ret != FI_SUCCESS) {
			GNIX_WARN(FI_LOG_EP_CTRL,
			    "_gnix_vc_destroy returned %d\n",
			     ret);
			goto err;
		}
	}

	/*
	 * clean up any vc hash table or vector
	 */

	if (ep->type == FI_EP_RDM) {
		if (ep->vc_ht != NULL) {
			_gnix_ht_destroy(ep->vc_ht);
			free(ep->vc_ht);
			ep->vc_ht = NULL;
		}
	}

	ret = _gnix_nic_free(nic);
	if (ret != FI_SUCCESS) {
		GNIX_WARN(FI_LOG_EP_CTRL,
		    "_gnix_vc_destroy call returned %d\n",
		     ret);
		goto err;
	}

	ep->nic = NULL;

	/*
	 * Free fab_reqs
	 */
	if (atomic_get(&ep->active_fab_reqs) != 0) {
		/* Should we just assert here? */
		GNIX_WARN(FI_LOG_EP_CTRL,
			  "Active requests while closing an endpoint.");
	}
	__fr_freelist_destroy(ep);

	free(ep);

err:
	return ret;
}

static int gnix_ep_bind(fid_t fid, struct fid *bfid, uint64_t flags)
{
	int ret = FI_SUCCESS;
	struct gnix_fid_ep  *ep;
	struct gnix_fid_av  *av;
	struct gnix_fid_cq  *cq;

	GNIX_TRACE(FI_LOG_EP_CTRL, "\n");

	ep = container_of(fid, struct gnix_fid_ep, ep_fid.fid);

	if (!bfid)
		return -FI_EINVAL;

	switch (bfid->fclass) {
	case FI_CLASS_EQ:
		ret = -FI_ENOSYS;
		goto err;
		break;
	case FI_CLASS_CQ:
		cq = container_of(bfid, struct gnix_fid_cq, cq_fid.fid);
		if (ep->domain != cq->domain) {
			ret = -FI_EINVAL;
			break;
		}
		if (flags & FI_SEND) {
			/* don't allow rebinding */
			if (ep->send_cq) {
				ret = -FI_EINVAL;
				break;
			}

			ep->send_cq = cq;
			if (flags & FI_SELECTIVE_COMPLETION) {
				ep->send_selective_completion = 1;
			}

			_gnix_cq_poll_nic_add(cq, ep->nic);
			atomic_inc(&cq->ref_cnt);
		}
		if (flags & FI_RECV) {
			/* don't allow rebinding */
			if (ep->recv_cq) {
				ret = -FI_EINVAL;
				break;
			}

			ep->recv_cq = cq;
			if (flags & FI_SELECTIVE_COMPLETION) {
				ep->recv_selective_completion = 1;
			}

			_gnix_cq_poll_nic_add(cq, ep->nic);
			atomic_inc(&cq->ref_cnt);
		}
		break;
	case FI_CLASS_AV:
		av = container_of(bfid, struct gnix_fid_av, av_fid.fid);
		if (ep->domain != av->domain) {
			ret = -FI_EINVAL;
			break;
		}
		ep->av = av;
		atomic_inc(&av->ref_cnt);
		break;
	case FI_CLASS_MR:/*TODO: got to figure this one out */
	case FI_CLASS_CNTR: /* TODO: need to support cntrs someday */
	default:
		ret = -FI_ENOSYS;
		break;
	}

err:
	return ret;
}

int gnix_ep_open(struct fid_domain *domain, struct fi_info *info,
		 struct fid_ep **ep, void *context)
{
	int ret = FI_SUCCESS;
	struct gnix_fid_domain *domain_priv;
	struct gnix_fid_ep *ep_priv;
	gnix_hashtable_attr_t gnix_ht_attr;

	GNIX_TRACE(FI_LOG_EP_CTRL, "\n");

	if ((domain == NULL) || (info == NULL) || (ep == NULL))
		return -FI_EINVAL;

	if (info->ep_attr->type != FI_EP_RDM)
		return -FI_ENOSYS;

	domain_priv = container_of(domain, struct gnix_fid_domain, domain_fid);

	ep_priv = calloc(1, sizeof *ep_priv);
	if (!ep_priv)
		return -FI_ENOMEM;

	ep_priv->ep_fid.fid.fclass = FI_CLASS_EP;
	ep_priv->ep_fid.fid.context = context;

	ep_priv->ep_fid.fid.ops = &gnix_ep_fi_ops;
	ep_priv->ep_fid.ops = &gnix_ep_ops;
	ep_priv->domain = domain_priv;
	ep_priv->type = info->ep_attr->type;

	fastlock_init(&ep_priv->vc_list_lock);
	dlist_init(&ep_priv->wc_vc_list);
	atomic_initialize(&ep_priv->active_fab_reqs, 0);
	atomic_initialize(&ep_priv->ref_cnt, 0);

	fastlock_init(&ep_priv->recv_queue_lock);
	slist_init(&ep_priv->unexp_recv_queue);
	slist_init(&ep_priv->posted_recv_queue);
	slist_init(&ep_priv->pending_recv_comp_queue);

	if (info->tx_attr)
		ep_priv->op_flags = info->tx_attr->op_flags;
	if (info->rx_attr)
		ep_priv->op_flags |= info->rx_attr->op_flags;

	ret = __fr_freelist_init(ep_priv);
	if (ret != FI_SUCCESS) {
		GNIX_ERR(FI_LOG_EP_CTRL,
			 "Error allocating gnix_fab_req freelist (%s)",
			 fi_strerror(-ret));
		goto err1;
	}

	ep_priv->ep_fid.msg = &gnix_ep_msg_ops;
	ep_priv->ep_fid.rma = &gnix_ep_rma_ops;
	ep_priv->ep_fid.tagged = &gnix_ep_tagged_ops;
	ep_priv->ep_fid.atomic = NULL;
	fastlock_init(&ep_priv->lock);

	ep_priv->ep_fid.cm = &gnix_cm_ops;

	/*
	 * TODO, initialize vc hash table
	 */
	if (ep_priv->type == FI_EP_RDM) {
		ret = _gnix_cm_nic_alloc(domain_priv,
					 &ep_priv->cm_nic);
		if (ret != FI_SUCCESS)
			goto err;

		gnix_ht_attr.ht_initial_size = 64;     /* TODO: get from domain */
		gnix_ht_attr.ht_maximum_size = 16384;  /* TODO: from domain */
		gnix_ht_attr.ht_increase_step = 2;
		gnix_ht_attr.ht_increase_type = GNIX_HT_INCREASE_MULT;
		gnix_ht_attr.ht_collision_thresh = 500;
		gnix_ht_attr.ht_hash_seed = 0xdeadbeefbeefdead;
		gnix_ht_attr.ht_internal_locking = 1;

		ep_priv->vc_ht = calloc(1, sizeof(struct gnix_hashtable));
		if (ep_priv->vc_ht == NULL)
			goto err;
		ret = _gnix_ht_init(ep_priv->vc_ht, &gnix_ht_attr);
		if (ret != FI_SUCCESS) {
			GNIX_WARN(FI_LOG_EP_CTRL,
				    "gnix_ht_init call returned %d\n",
				     ret);
			goto err;
		}

	} else {
		ep_priv->cm_nic = NULL;
		ep_priv->vc = NULL;
	}

	ep_priv->progress_fn = NULL;
	ep_priv->rx_progress_fn = NULL;

	ret = gnix_nic_alloc(domain_priv, &ep_priv->nic);
	if (ret != FI_SUCCESS) {
		GNIX_WARN(FI_LOG_EP_CTRL,
			    "_gnix_nic_alloc call returned %d\n",
			     ret);
		goto err;
	}

	/*
	 * if smsg callbacks not present hook them up now
	 */

	if (ep_priv->nic->smsg_callbacks == NULL)
		ep_priv->nic->smsg_callbacks = gnix_ep_smsg_callbacks;

	atomic_inc(&domain_priv->ref_cnt);
	*ep = &ep_priv->ep_fid;
	return ret;

err1:
	__fr_freelist_destroy(ep_priv);
err:
	if (ep_priv->vc_ht != NULL) {
		_gnix_ht_destroy(ep_priv->vc_ht); /* may not be initialized but
						     okay */
		free(ep_priv->vc_ht);
		ep_priv->vc_ht = NULL;
	}
	if (ep_priv->cm_nic != NULL)
		ret = _gnix_cm_nic_free(ep_priv->cm_nic);
	free(ep_priv);
	return ret;

}

/*******************************************************************************
 * FI_OPS_* data structures.
 ******************************************************************************/

static struct fi_ops gnix_ep_fi_ops = {
	.size = sizeof(struct fi_ops),
	.close = gnix_ep_close,
	.bind = gnix_ep_bind,
	.control = gnix_ep_control,
};

static struct fi_ops_ep gnix_ep_ops = {
	.size = sizeof(struct fi_ops_ep),
	.cancel = fi_no_cancel,
	.getopt = fi_no_getopt,
	.setopt = fi_no_setopt,
	.tx_ctx = fi_no_tx_ctx,
	.rx_ctx = fi_no_rx_ctx,
	.rx_size_left = fi_no_rx_size_left,
	.tx_size_left = fi_no_tx_size_left,
};

static struct fi_ops_msg gnix_ep_msg_ops = {
	.size = sizeof(struct fi_ops_msg),
	.recv = gnix_ep_recv,
	.recvv = gnix_ep_recvv,
	.recvmsg = gnix_ep_recvmsg,
	.send = gnix_ep_send,
	.sendv = gnix_ep_sendv,
	.sendmsg = gnix_ep_sendmsg,
	.inject = gnix_ep_msg_inject,
	.senddata = fi_no_msg_senddata,
	.injectdata = fi_no_msg_injectdata,
};

static struct fi_ops_rma gnix_ep_rma_ops = {
	.size = sizeof(struct fi_ops_rma),
	.read = gnix_ep_read,
	.readv = gnix_ep_readv,
	.readmsg = gnix_ep_readmsg,
	.write = gnix_ep_write,
	.writev = gnix_ep_writev,
	.writemsg = gnix_ep_writemsg,
	.inject = gnix_ep_rma_inject,
	.writedata = gnix_ep_writedata,
	.injectdata = gnix_ep_rma_injectdata,
};

struct fi_ops_tagged gnix_ep_tagged_ops = {
	.size = sizeof(struct fi_ops_tagged),
	.recv = gnix_ep_trecv,
	.recvv = gnix_ep_trecvv,
	.recvmsg = gnix_ep_trecvmsg,
	.send = gnix_ep_tsend,
	.sendv = gnix_ep_tsendv,
	.sendmsg = gnix_ep_tsendmsg,
	.inject = gnix_ep_tinject,
	.senddata = fi_no_tagged_senddata,
	.senddata = gnix_ep_tsenddata,
	.injectdata = fi_no_tagged_injectdata,
};
