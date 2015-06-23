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
 * code for managing VC's
 */

#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "gnix.h"
#include "gnix_vc.h"
#include "gnix_util.h"
#include "gnix_datagram.h"
#include "gnix_cm_nic.h"
#include "gnix_nic.h"
#include "gnix_ep.h"
#include "gnix_mbox_allocator.h"
#include "gnix_hashtable.h"

/*******************************************************************************
 * Helper functions.
 ******************************************************************************/

/*
 * call back prior to posting of datagram to kgni,
 * within the critical region surrounding the call
 * to GNI_EpPostDataWId.
 *
 * Returning 1 in the address post, says to post
 * the datagram to kgni, otherwise not.
 */

static int  __gnix_vc_pre_post_clbk(struct gnix_datagram *dgram,
					int *post)
{
	int ret = FI_SUCCESS;
	int can_we_post = 1;
	struct gnix_vc *vc;

	GNIX_TRACE(FI_LOG_EP_CTRL, "\n");

	vc = (struct gnix_vc *)dgram->cache;
	assert(vc);

	/*
	 * vc should be in the hash table
	 */

	assert(vc->modes & GNIX_VC_MODE_IN_HT);

	/*
	 * the VC may have already completed connecting
	 * if one of the local WC datagrams got matched
	 * with the peer we were trying to connect to.
	 * In this case, don't post the datagram.
	 */
	if ((vc->conn_state == GNIX_VC_CONNECTED)
		|| (vc->conn_state == GNIX_VC_CONNECTING))
		can_we_post = 0;

	*post = can_we_post;
	return ret;
}

/*
 * call back after posting of datagram to kgni,
 * within the critical region surrounding call to
 * GNI_EpPostDataWId.
 *
 * The vc is transitioned to GNIX_VC_CONNECTING if
 * the datagram has been successfully posted to kgni,
 * or there is already a wildcard match to the
 * VC's address in kgni's datagram state engine.
 * Otherwise move the vc to error state and return
 * error code.
 */
static int __gnix_vc_post_post_clbk(struct gnix_datagram *dgram,
				    gni_return_t status)
{
	int ret;
	struct gnix_vc *vc;

	GNIX_TRACE(FI_LOG_EP_CTRL, "\n");

	vc = (struct gnix_vc *)dgram->cache;
	assert(vc);

	switch (status) {
	case GNI_RC_SUCCESS:
		vc->modes |= GNIX_VC_MODE_DG_POSTED;
	case GNI_RC_ERROR_RESOURCE:
		vc->conn_state = GNIX_VC_CONNECTING;
		ret = FI_SUCCESS;
		break;
	default:
		vc->conn_state = GNIX_VC_CONN_ERROR;
		ret = gnixu_to_fi_errno(status);
	}
	return ret;
}

/*
 * special call back for wildcard datagrams.  The
 * call back is invoked after calling GNI_EpPostDataTestById
 * within the critical region surrounding the call.
 *
 * If the post state reported by GNI indicates that
 * the datagram was matched, insert the vc into
 * the hash table and transition to GNIX_VC_CONNECTING.
 *
 * Note there are several intermediate gni post states
 * that may be reported as a datagram exchange is taking
 * place: GNI_POST_COMPLETED, GNI_POST_REMOTE_DATA,
 * and GNI_POST_PENDING, so this routine may be invoked
 * multiple times per completing datagram exchange.
 */
static int __gnix_vc_post_test_clbk(struct gnix_datagram *dgram,
				    struct gnix_address peer_addr,
				    gni_post_state_t post_state)
{
	int ret = FI_SUCCESS;
	gnix_ht_key_t key;
	struct gnix_vc *vc = NULL;
	struct gnix_fid_ep *ep = NULL;

	GNIX_TRACE(FI_LOG_EP_CTRL, "\n");

	vc = (struct gnix_vc *)dgram->cache;
	assert(vc);

	ep = vc->ep;
	assert(ep);
	assert(ep->vc_ht);

	switch (post_state) {
	case GNI_POST_COMPLETED:
	case GNI_POST_REMOTE_DATA:
	case GNI_POST_PENDING:
		if (GNIX_ADDR_UNSPEC(vc->peer_addr)) {
			vc->peer_addr = peer_addr;
			memcpy(&key, &peer_addr,
				sizeof(gnix_ht_key_t));
			ret = _gnix_ht_insert(ep->vc_ht, key,
						vc);
			if (ret == FI_SUCCESS) {
				vc->modes |= GNIX_VC_MODE_IN_HT;
				vc->conn_state =
					GNIX_VC_CONNECTING;
			} else if (ret != -FI_ENOSPC)
				GNIX_WARN(FI_LOG_EP_CTRL,
				"__gnix_vc_post_test_clbk (0x%x,0x%x) %d\n",
				peer_addr.device_addr,
				peer_addr.cdm_id,
				ret);
		}
		break;
	default:
		break;
	}
	return ret;
}

/*
 * My connection request matched either a connection
 * request from my peer or a wildcard.  This means
 * I have already inserted the vc in to the hash table
 * and may potentially have a message backlog.
 */

static int __gnix_vc_hndl_con_match_con(struct gnix_datagram *dgram,
					struct gnix_address peer_address)
{
	int ret = FI_SUCCESS;
	struct gnix_vc *vc = NULL;
	struct gnix_fid_ep *ep;
	struct gnix_nic *nic;
	int local_id, peer_id;
	gni_smsg_attr_t local_smsg_attr;
	gni_smsg_attr_t peer_smsg_attr;
	gni_return_t __attribute__((unused)) status;
	ssize_t __attribute__((unused)) len;

	GNIX_TRACE(FI_LOG_EP_CTRL, "\n");

	/*
	 * get our local vc associated with this datagram
	 * from the cache
	 */

	vc = (struct gnix_vc *)dgram->cache;
	assert(vc);

	/*
	 * at this point vc should be in connecting state
	 */
	assert(vc->conn_state == GNIX_VC_CONNECTING);

	ep = vc->ep;
	assert(ep);

	nic = ep->nic;
	assert(nic);

	/*
	 * get our local id (the vc_id of the vc
	 * that we allocated earlier)
	 */
	len = _gnix_dgram_unpack_buf(dgram,
				     GNIX_DGRAM_IN_BUF,
				     &local_id,
				     sizeof(int));
	assert(len == sizeof(int));

	/*
	 * get the smsg attributes of the mbox we allocated
	 * earlier for this conn req.
	 */
	len = _gnix_dgram_unpack_buf(dgram,
				     GNIX_DGRAM_IN_BUF,
				     &local_smsg_attr,
				     sizeof(gni_smsg_attr_t));
	assert(len == sizeof(gni_smsg_attr_t));

	/*
	 * get the vc id of peer
	 */
	len = _gnix_dgram_unpack_buf(dgram, GNIX_DGRAM_OUT_BUF,
				     &peer_id,
				     sizeof(int));
	assert(len == sizeof(int));

	/*
	 * get the smsg attributes of the mbox our peer allocated
	 * earlier for this conn req.
	 */
	len = _gnix_dgram_unpack_buf(dgram,
				     GNIX_DGRAM_OUT_BUF,
				     &peer_smsg_attr,
				     sizeof(gni_smsg_attr_t));

	assert(len == sizeof(gni_smsg_attr_t));

	/*
	 *  now build the SMSG connection
	 */
	status = GNI_EpCreate(ep->nic->gni_nic_hndl,
			     ep->nic->tx_cq,
			     &vc->gni_ep);
	if (status != GNI_RC_SUCCESS) {
		GNIX_WARN(FI_LOG_EP_CTRL,
			  "GNI_EpCreate returned %s\n", gni_err_str[status]);
		ret = gnixu_to_fi_errno(status);
		goto err;
	}

	status = GNI_EpBind(vc->gni_ep,
			    peer_address.device_addr,
			    peer_address.cdm_id);
	if (status != GNI_RC_SUCCESS) {
		GNIX_WARN(FI_LOG_EP_CTRL,
			  "GNI_EpBind returned %s\n", gni_err_str[status]);
		ret = gnixu_to_fi_errno(status);
		goto err1;
	}

	status = GNI_SmsgInit(vc->gni_ep,
			      &local_smsg_attr,
			      &peer_smsg_attr);
	if (status != GNI_RC_SUCCESS) {
		GNIX_WARN(FI_LOG_EP_CTRL,
			  "GNI_SmsgInit returned %s\n", gni_err_str[status]);
		ret = gnixu_to_fi_errno(status);
		goto err1;
	}

	status = GNI_EpSetEventData(vc->gni_ep,
				    local_id,
				    peer_id);
	if (status != GNI_RC_SUCCESS) {
		GNIX_WARN(FI_LOG_EP_CTRL,
			  "GNI_EpSetEventData returned %s\n",
			  gni_err_str[status]);
		ret = gnixu_to_fi_errno(status);
		goto err1;
	}

	/*
	 * transition the VC to connected
	 * put in to the nic's work queue for
	 * further processing
	 */

	vc->conn_state = GNIX_VC_CONNECTED;
	ret = _gnix_dgram_free(dgram);
	if (ret != FI_SUCCESS)
		GNIX_WARN(FI_LOG_EP_CTRL,
			  "_gnix_dgram_free returned %d\n", ret);
	vc->dgram = NULL;

	ret = _gnix_vc_add_to_wq(vc);
	if (ret == FI_SUCCESS)
		ret = _gnix_nic_progress(nic);

	return ret;
err1:
	GNI_EpDestroy(vc->gni_ep);
err:
	vc->conn_state = GNIX_VC_CONN_ERROR;
	return ret;
}

/*
 * One of my wildcards matched an incoming connection
 * request from peer.  In this case we may have a vc for this
 * peer in the hash table of vc's for this ep.
 */

static int __gnix_vc_hndl_wc_match_con(struct gnix_datagram *dgram,
					struct gnix_address peer_address)
{
	int ret = FI_SUCCESS;
	struct gnix_vc *vc = NULL, *wc_vc = NULL;
	struct gnix_fid_ep *ep;
	struct gnix_nic *nic;
	int local_id, peer_id;
	gni_smsg_attr_t local_smsg_attr;
	gni_smsg_attr_t peer_smsg_attr;
	gni_return_t __attribute__((unused)) status;
	gnix_ht_key_t key;
	ssize_t __attribute__((unused)) len;

	GNIX_TRACE(FI_LOG_EP_CTRL, "\n");

	wc_vc = (struct gnix_vc *)dgram->cache;
	assert(wc_vc);

	ep = wc_vc->ep;
	assert(ep);

	nic = ep->nic;
	assert(nic);

	/*
	 * should be in connecting state at this
	 * point.
	 */
	assert(wc_vc->conn_state == GNIX_VC_CONNECTING);

	/*
	 * if the wc vc is not in the hash table
	 * that means we are hitting the case where
	 * we were trying to connect with a peer
	 * who was also connecting to us, and
	 * so hit the GNI_RC_ERROR_RESOURCE case
	 * when posting a datagram.  We need to
	 * move any pending sends on to the wc_vc,
	 * remove the current vc for this peer from
	 * the hash table, and insert the wc_vc
	 */
	if (!(wc_vc->modes & GNIX_VC_MODE_IN_HT)) {
		memcpy(&key, &peer_address,
			sizeof(gnix_ht_key_t));
		vc = _gnix_ht_lookup(ep->vc_ht, key);
		assert(vc != NULL);
		ret = _gnix_ht_remove(ep->vc_ht, key);
		assert(ret == FI_SUCCESS);
		if (!slist_empty(&vc->tx_queue))
			slist_insert_head(vc->tx_queue.head,
					  &wc_vc->tx_queue);
		fastlock_acquire(&ep->vc_list_lock);
		dlist_remove(&vc->entry);
		fastlock_release(&ep->vc_list_lock);
		ret = _gnix_vc_destroy(vc);
		assert(ret == FI_SUCCESS);
		vc = wc_vc;
		ret = _gnix_ht_insert(ep->vc_ht, key, vc);
		assert(ret == FI_SUCCESS);
		fastlock_acquire(&ep->vc_list_lock);
		dlist_insert_tail(&vc->entry,
				  &ep->wc_vc_list);
		fastlock_release(&ep->vc_list_lock);
		assert(ret == FI_SUCCESS);
	} else
		vc = wc_vc;


	/*
	 * get our local id (the vc_id of the vc
	 * that we allocated earlier)
	 */
	len = _gnix_dgram_unpack_buf(dgram,
				     GNIX_DGRAM_IN_BUF,
				     &local_id,
				     sizeof(int));
	assert(len == sizeof(int));

	/*
	 * get the smsg attributes of the mbox we allocated
	 * earlier for this conn req.
	 */
	len = _gnix_dgram_unpack_buf(dgram,
				     GNIX_DGRAM_IN_BUF,
				     &local_smsg_attr,
				     sizeof(gni_smsg_attr_t));
	assert(len == sizeof(gni_smsg_attr_t));

	/*
	 * get the vc id of peer
	 */
	len = _gnix_dgram_unpack_buf(dgram, GNIX_DGRAM_OUT_BUF,
				     &peer_id,
				     sizeof(int));
	assert(len == sizeof(int));

	/*
	 * get the smsg attributes of the mbox our peer allocated
	 * earlier for this conn req.
	 */
	len = _gnix_dgram_unpack_buf(dgram,
				     GNIX_DGRAM_OUT_BUF,
				     &peer_smsg_attr,
				     sizeof(gni_smsg_attr_t));
	assert(len == sizeof(gni_smsg_attr_t));

	/*
	 *  now build the SMSG connection
	 */

	status = GNI_EpCreate(ep->nic->gni_nic_hndl,
			     ep->nic->tx_cq,
			     &vc->gni_ep);
	if (status != GNI_RC_SUCCESS) {
		GNIX_WARN(FI_LOG_EP_CTRL,
			  "GNI_EpCreate returned %s\n", gni_err_str[status]);
		ret = gnixu_to_fi_errno(status);
		goto err;
	}

	status = GNI_EpBind(vc->gni_ep,
			    peer_address.device_addr,
			    peer_address.cdm_id);
	if (status != GNI_RC_SUCCESS) {
		GNIX_WARN(FI_LOG_EP_CTRL,
			  "GNI_EpBind returned %s\n", gni_err_str[status]);
		ret = gnixu_to_fi_errno(status);
		goto err1;
	}

	status = GNI_SmsgInit(vc->gni_ep,
			      &local_smsg_attr,
			      &peer_smsg_attr);
	if (status != GNI_RC_SUCCESS) {
		GNIX_WARN(FI_LOG_EP_CTRL,
			  "GNI_SmsgInit returned %s\n", gni_err_str[status]);
		ret = gnixu_to_fi_errno(status);
		goto err1;
	}

	status = GNI_EpSetEventData(vc->gni_ep,
				    local_id,
				    peer_id);
	if (status != GNI_RC_SUCCESS) {
		GNIX_WARN(FI_LOG_EP_CTRL,
			  "GNI_EpSetEventData returned %s\n",
			  gni_err_str[status]);
		ret = gnixu_to_fi_errno(status);
		goto err1;
	}

	/*
	 * transition the VC to connected
	 * repost a wildcard, then
	 * put the vc in to the nic's work queue for
	 * further processing
	 */

	vc->conn_state = GNIX_VC_CONNECTED;


	ret = _gnix_dgram_free(dgram);
	if (ret != FI_SUCCESS)
		GNIX_WARN(FI_LOG_EP_CTRL,
			  "_gnix_dgram_free returned %d\n",
			  ret);
	vc->dgram = NULL;


	/*
	 * repost a wildcard datagram
	 */

	ret = _gnix_vc_alloc(ep, FI_ADDR_UNSPEC, &wc_vc);
	if (ret != FI_SUCCESS)
		goto err1;

	ret = _gnix_vc_accept(wc_vc);
	if (ret != FI_SUCCESS)
		GNIX_WARN(FI_LOG_EP_CTRL,
			  "gnix_vc_accept returned %d\n",
			  ret);

	fastlock_acquire(&ep->vc_list_lock);
	dlist_insert_tail(&wc_vc->entry, &ep->wc_vc_list);
	fastlock_release(&ep->vc_list_lock);

	/*
	 * put the connected vc in to the work queue of the gnix_nic
	 */

	ret = _gnix_vc_add_to_wq(vc);
	if (ret == FI_SUCCESS) {
		ret = _gnix_nic_progress(nic);
		goto out;
	}

	return ret;
err1:
	GNI_EpDestroy(vc->gni_ep);
err:
out:
	return ret;
}


static int _gnix_vc_process_datagram_w_error(struct gnix_datagram *dgram,
					     struct gnix_address peer_address,
					     gni_post_state_t state)
{
	GNIX_TRACE(FI_LOG_EP_CTRL, "\n");
	return -FI_ENOSYS;
}

static int __gnix_vc_process_datagram(struct gnix_datagram *dgram,
				     struct gnix_address peer_address,
				     gni_post_state_t state)
{
	int ret = FI_SUCCESS;
	enum gnix_vc_conn_req_type rtype_in, rtype_out;
	ssize_t __attribute__((unused)) len;

	GNIX_TRACE(FI_LOG_EP_CTRL, "\n");

	/*
	 * first get the vc conn req type from the in buf
	 */

	len = _gnix_dgram_unpack_buf(dgram, GNIX_DGRAM_IN_BUF,
				     &rtype_in,
				     sizeof(rtype_in));
	assert(len == sizeof(rtype_in));

	len = _gnix_dgram_unpack_buf(dgram, GNIX_DGRAM_OUT_BUF,
				     &rtype_out,
				     sizeof(rtype_out));
	assert(len == sizeof(rtype_out));

	/*
	 * next check the post state, if its anything but
	 * GNI_POST_COMPLETED process using error handling
	 * function.
	 */

	if (state != GNI_POST_COMPLETED) {
		ret = _gnix_vc_process_datagram_w_error(dgram,
							 peer_address,
							 state);
		goto err;
	}

	/*
	 * handle different cases for datagram matching:
	 * my active matched peer's active conn req
	 * my passive matched peer's active
	 * my active matched peer's passive
	 * passive matching passive is an error
	 */

	switch (rtype_in) {
	case GNIX_VC_CONN_REQ_CONN:
	switch (rtype_out) {
	case GNIX_VC_CONN_REQ_CONN:
	case GNIX_VC_CONN_REQ_LISTEN:

		ret = __gnix_vc_hndl_con_match_con(dgram,peer_address);
		break;
		default:
		assert(0);
	}
	break;

	case GNIX_VC_CONN_REQ_LISTEN:
		switch (rtype_out) {
		case GNIX_VC_CONN_REQ_CONN:

		ret = __gnix_vc_hndl_wc_match_con(dgram,peer_address);
		break;
		case GNIX_VC_CONN_REQ_LISTEN:
		default:
		assert(0);
		}
	}
err:
	return ret;
}

static int __gnix_vc_connect_prog_fn(void *data, int *complete_ptr)
{
	int ret = FI_SUCCESS;
	gni_return_t __attribute__((unused)) status;
	int complete = 0;
	struct gnix_vc *vc = (struct gnix_vc *)data;
	struct gnix_mbox *mbox = NULL;
	gni_smsg_attr_t smsg_mbox_attr;
	struct gnix_fid_ep *ep = NULL;
	struct gnix_cm_nic *cm_nic = NULL;
	struct gnix_datagram *dgram = NULL;
	enum gnix_vc_conn_req_type rtype = GNIX_VC_CONN_REQ_CONN;
	ssize_t __attribute__((unused)) len;

	GNIX_TRACE(FI_LOG_EP_CTRL, "\n");

	ep = vc->ep;
	if (ep == NULL)
		return -FI_EINVAL;

	cm_nic = ep->cm_nic;
	if (cm_nic == NULL)
		return -FI_EINVAL;

	/*
	 * sanity check that the vc is in the hash table
	 */

	if (!(vc->modes & GNIX_VC_MODE_IN_HT))
		return -FI_EINVAL;

	/*
	 * if one of our wild cards is completing or has
	 * already completed setup of the connection just
	 * indicate completion and early return
	 */

	if ((vc->conn_state == GNIX_VC_CONNECTING)
		|| (vc->conn_state == GNIX_VC_CONNECTED)) {
		complete = 1;
		goto exit;
	}

	/*
	 * first see if we still need a mailbox
	 */

	if (vc->smsg_mbox == NULL) {
		ret = _gnix_mbox_alloc(vc->ep->nic->mbox_hndl,
				       &mbox);
		if (ret == FI_SUCCESS)
			vc->smsg_mbox = mbox;
		else
			goto exit;
	}

	mbox = vc->smsg_mbox;

	/*
	 * okay, got a mailbox, now lets try to
	 * get a datagram
	 */

	ret = _gnix_dgram_alloc(cm_nic->dgram_hndl,
				GNIX_DGRAM_BND,
				&dgram);
	if (ret != FI_SUCCESS)
		goto exit;

	/*
	 * set the datagram completion callback function
	 * and target address
	 */

	dgram->target_addr = vc->peer_addr;
	dgram->callback_fn = __gnix_vc_process_datagram;
	dgram->pre_post_clbk_fn = __gnix_vc_pre_post_clbk;
	dgram->post_post_clbk_fn = __gnix_vc_post_post_clbk;
	dgram->post_test_clbk_fn = __gnix_vc_post_test_clbk;
	dgram->cache = vc;

	/*
	 * fill in the mbox smsg info and pack in to the
	 * datagram IN payload
	 */

	smsg_mbox_attr.msg_type = GNI_SMSG_TYPE_MBOX_AUTO_RETRANSMIT;
	smsg_mbox_attr.msg_buffer = mbox->base;
	smsg_mbox_attr.buff_size =  vc->ep->nic->mem_per_mbox;
	smsg_mbox_attr.mem_hndl = *mbox->memory_handle;
	smsg_mbox_attr.mbox_offset = (uint64_t)mbox->offset;
	smsg_mbox_attr.mbox_maxcredit = 64; /* TODO: fix this */
	smsg_mbox_attr.msg_maxsize =  16384;  /* TODO: definite fix */

	/*
	 * pack the things we need into the datagram in_box:
	 * - vc conn request type
	 * - vc_id of the connection
	 * - the smsg_mbox_attr sturr
	 */

	len = _gnix_dgram_pack_buf(dgram, GNIX_DGRAM_IN_BUF,
				   &rtype,
				   sizeof(enum gnix_vc_conn_req_type));
	assert(len == sizeof(enum gnix_vc_conn_req_type));

	len = _gnix_dgram_pack_buf(dgram, GNIX_DGRAM_IN_BUF,
				   &vc->vc_id, sizeof(vc->vc_id));
	assert(len == sizeof(vc->vc_id));

	len = _gnix_dgram_pack_buf(dgram, GNIX_DGRAM_IN_BUF,
				   &smsg_mbox_attr,
				   sizeof(gni_smsg_attr_t));
	assert(len == sizeof(gni_smsg_attr_t));

	ret = _gnix_dgram_bnd_post(dgram);
	if (ret != FI_SUCCESS)
		goto exit;

	/*
	 * We may not have posted a datagram.  This
	 * happens if a previously posted wildcard datagram
	 * already matched with the destination address,
	 * we free the mbox and datagram since these
	 * will be supplied from the values of the in
	 * buffer in the wildcard datagram that matched.
	 */

	if (!(vc->modes & GNIX_VC_MODE_DG_POSTED) &&
		(vc->conn_state != GNIX_VC_CONNECTED)) {
		ret = _gnix_mbox_free(vc->smsg_mbox);
		if (ret != FI_SUCCESS)
			GNIX_WARN(FI_LOG_EP_CTRL,
				  "_gnix_mbox_free returned %d\n",
				  ret);
		vc->smsg_mbox = NULL;

		ret = _gnix_dgram_free(dgram);
		if (ret != FI_SUCCESS)
			GNIX_WARN(FI_LOG_EP_CTRL,
				  "_gnix_dgram_free returned %d\n",
				  ret);
	}

	complete = 1;

exit:
	*complete_ptr = complete;
	return ret;
}

/*
 * connect completer function for work queue element,
 * sort of a NO-OP for now.
 */
static int __gnix_vc_connect_comp_fn(void *data)
{
	return FI_SUCCESS;
}

static int __gnix_vc_prog_fn(void *data, int *complete_ptr)
{
	int ret;
	struct gnix_vc *vc = (struct gnix_vc *)data;
	struct gnix_cm_nic *cm_nic;

	*complete_ptr = 0;

	/*
	 * TODO: this is temporary and will be removed
	 * once the cm_nic functionality goes in to
	 * nic functionality (see issue 218)
	 */
	if (vc->conn_state < GNIX_VC_CONNECTED) {
		cm_nic = vc->ep->cm_nic;
		ret = _gnix_cm_nic_progress(cm_nic);
		if (ret != FI_SUCCESS)
			GNIX_WARN(FI_LOG_EP_CTRL,
				  "_gnix_cm_nic_progress returned %d\n",
				   ret);
		goto err;
	}

	/*
	 * check for pending messages
	 */

	if ((vc->modes & GNIX_VC_MODE_PENDING_MSGS) &&
		(vc->conn_state == GNIX_VC_CONNECTED)) {
		ret = _gnix_ep_vc_dequeue_smsg(vc);
		if (ret != FI_SUCCESS) {
			GNIX_WARN(FI_LOG_EP_CTRL,
				  "_gnix_ep_vc_dqueue returned %d\n",
				   ret);
			goto err;
		}
	}

	ret = _gnix_vc_push_tx_reqs(vc);
	if (ret != FI_SUCCESS) {
		GNIX_WARN(FI_LOG_EP_CTRL,
			  "_gnix_ep_push_vc_sendq returned %d\n",
			   ret);
		goto err;
	}

	if (slist_empty(&vc->tx_queue) &&
	    !(vc->modes & GNIX_VC_MODE_PENDING_MSGS))
		*complete_ptr = 1;
#if 0
		if ((ret == -FI_ENOSPC) || (ret == -FI_EOPBADSTATE))
			ret = FI_SUCCESS;  /* FI_ENOSPC is not an error */
#endif

err:
	return ret;
}

static int __gnix_vc_comp_fn(void *data)
{
	struct gnix_vc *vc = (struct gnix_vc *)data;

	vc->modes &= ~GNIX_VC_MODE_IN_WQ;
	return FI_SUCCESS;
}

/*******************************************************************************
 * Internal API functions
 ******************************************************************************/

int _gnix_vc_alloc(struct gnix_fid_ep *ep_priv, fi_addr_t dest_addr,
		   struct gnix_vc **vc)

{
	int ret = FI_SUCCESS;
	int remote_id;
	struct gnix_vc *vc_ptr = NULL;
	struct gnix_nic *nic = NULL;
#if 0
	struct gnix_fid_av *av = NULL;
#endif

	GNIX_TRACE(FI_LOG_EP_CTRL, "\n");

#if 0
	/*
	 * if ep is of type FI_EP_RDM, need to check the map type
	 */

	if (ep_priv->type == FI_EP_RDM) {
		if (!ep_priv->av)
			return -FI_EINVAL;
		av = ep_priv->av;
		if (ep_priv->av->type == FI_AV_TABLE)
			/* TODO: need something here */
	}
#endif

	nic = ep_priv->nic;
	if (nic == NULL)
		return -FI_EINVAL;

	vc_ptr = calloc(1, sizeof(*vc_ptr));
	if (!vc_ptr)
		return -FI_ENOMEM;

	vc_ptr->conn_state = GNIX_VC_CONN_NONE;
	memcpy(&vc_ptr->peer_addr, &dest_addr, sizeof(dest_addr));
	vc_ptr->ep = ep_priv;
	atomic_inc(&ep_priv->ref_cnt);
	slist_init(&vc_ptr->tx_queue);

	/*
	 * we need an id for the vc to allow for quick lookup
	 * based on GNI_CQ_GET_INST_ID
	 */

	ret = _gnix_nic_get_rem_id(nic, &remote_id, vc_ptr);
	if (ret != FI_SUCCESS)
		goto err;
	vc_ptr->vc_id = remote_id;

	*vc = vc_ptr;

	return ret;

err:
	if (vc_ptr)
		free(vc_ptr);
	return ret;
}

int _gnix_vc_destroy(struct gnix_vc *vc)
{
	int ret = FI_SUCCESS;
	struct gnix_nic *nic = NULL;
	gni_return_t status;

	GNIX_TRACE(FI_LOG_EP_CTRL, "\n");

	if (vc->ep == NULL)
		return -FI_EINVAL;

	nic = vc->ep->nic;
	if (nic == NULL)
		return -FI_EINVAL;

	/*
	 * check the state of the VC, may need to
	 * do something for some cases
	 */

	if ((vc->conn_state != GNIX_VC_CONN_NONE)
		&& (vc->conn_state != GNIX_VC_CONN_TERMINATED)) {
		GNIX_WARN(FI_LOG_EP_CTRL,
			      "vc conn state  %d\n",
			       vc->conn_state);
		return -FI_EBUSY;
	}

	/*
	 * if send_q not empty, return -FI_EBUSY
	 * Note for FI_EP_MSG type eps, this behavior
	 * may not be correct for handling fi_shutdown.
	 */

	if (!slist_empty(&vc->tx_queue)) {
		GNIX_WARN(FI_LOG_EP_CTRL, "vc sendqueue not empty\n");
		return -FI_EBUSY;
	}

	if (vc->gni_ep != NULL) {
		status = GNI_EpDestroy(vc->gni_ep);
		if (status != GNI_RC_SUCCESS) {
			GNIX_WARN(FI_LOG_EP_CTRL, "GNI_EpDestroy returned %s\n",
				  gni_err_str[status]);
			ret = gnixu_to_fi_errno(status);
			return ret;
		}
	}

	if (vc->smsg_mbox != NULL) {
		ret = _gnix_mbox_free(vc->smsg_mbox);
		if (ret != FI_SUCCESS)
			GNIX_WARN(FI_LOG_EP_CTRL,
			      "_gnix_mbox_free returned %d\n", ret);
		vc->smsg_mbox = NULL;
	}

	if (vc->dgram != NULL) {
		ret = _gnix_dgram_free(vc->dgram);
		if (ret != FI_SUCCESS)
			GNIX_WARN(FI_LOG_EP_CTRL,
			      "_gnix_dgram_free returned %d\n", ret);
		vc->dgram = NULL;
	}

	ret = _gnix_nic_free_rem_id(nic, vc->vc_id);
	if (ret != FI_SUCCESS)
		GNIX_WARN(FI_LOG_EP_CTRL,
		      "__gnix_vc_free_id returned %d\n", ret);

	atomic_dec(&vc->ep->ref_cnt);

	free(vc);

	return ret;
}

int _gnix_vc_connect(struct gnix_vc *vc)
{
	int ret = FI_SUCCESS;
	struct gnix_fid_ep *ep = NULL;
	struct gnix_cm_nic *cm_nic = NULL;
	struct gnix_work_req *work_req;

	GNIX_TRACE(FI_LOG_EP_CTRL, "\n");

	/*
	 * can happen that we are already connecting, or
	 * are connected
	 */

	if ((vc->conn_state == GNIX_VC_CONNECTING) ||
		(vc->conn_state == GNIX_VC_CONNECTED))
		return FI_SUCCESS;

	ep = vc->ep;
	if (ep == NULL)
		return -FI_EINVAL;

	cm_nic = ep->cm_nic;
	if (cm_nic == NULL)
		return -FI_EINVAL;

	/*
	 * only endpoints of type FI_EP_RDM use this
	 * connection method
	 */
	if (ep->type != FI_EP_RDM)
		return -FI_EINVAL;

	/*
	 * allocate a work request and try to
	 * run the progress function once.  If it
	 * doesn't succeed, put it on the cm_nic work queue.
	 */

	work_req = calloc(1, sizeof(*work_req));
	if (work_req == NULL)
		return -FI_ENOMEM;

	work_req->progress_fn = __gnix_vc_connect_prog_fn;
	work_req->data = vc;
	work_req->completer_fn = __gnix_vc_connect_comp_fn;
	work_req->completer_data = vc;

	/*
	 * add the work request to the tail of the
	 * cm_nic's work queue, progress the cm_nic.
	 */

	fastlock_acquire(&cm_nic->wq_lock);
	list_add_tail(&cm_nic->cm_nic_wq, &work_req->list);
	fastlock_release(&cm_nic->wq_lock);

	ret = _gnix_cm_nic_progress(cm_nic);

	return ret;
}

int _gnix_vc_accept(struct gnix_vc *vc)
{
	int ret = FI_SUCCESS;
	struct gnix_fid_ep *ep = NULL;
	struct gnix_cm_nic *cm_nic = NULL;
	struct gnix_nic *nic = NULL;
	struct gnix_mbox *mbox = NULL;
	gni_smsg_attr_t smsg_mbox_attr;
	struct gnix_datagram *dgram = NULL;
	enum gnix_vc_conn_req_type rtype = GNIX_VC_CONN_REQ_LISTEN;
	ssize_t __attribute__((unused)) len;

	GNIX_TRACE(FI_LOG_EP_CTRL, "\n");

	ep = vc->ep;
	if (ep == NULL)
		return -FI_EINVAL;

	cm_nic = ep->cm_nic;
	if (cm_nic == NULL)
		return -FI_EINVAL;

	nic = ep->nic;
	if (nic == NULL)
		return -FI_EINVAL;

	/*
	 * only endpoints of type FI_EP_RDM use this
	 * connection method
	 */
	if (ep->type != FI_EP_RDM)
		return -FI_EINVAL;

	/*
	 * the peer_address of the endpoint must be GNIX_ADDR_UNSPEC
	 */

	if (!GNIX_ADDR_UNSPEC(vc->peer_addr))
		return -FI_EINVAL;

	/*
	 * try to allocate a mailbox
	 */
	ret = _gnix_mbox_alloc(nic->mbox_hndl,
			       &mbox);
	if (ret == FI_SUCCESS)
		vc->smsg_mbox = mbox;
	else {
		ret = -FI_EAGAIN;
		goto err;
	}

	/*
	 * try to allocate a datagram
	 */

	ret = _gnix_dgram_alloc(cm_nic->dgram_hndl,
				GNIX_DGRAM_WC,
				&dgram);
	if (ret != FI_SUCCESS)
		goto err1;

	/*
	 * set the datagram completion callback function
	 */

	dgram->callback_fn = __gnix_vc_process_datagram;
	dgram->post_test_clbk_fn = __gnix_vc_post_test_clbk;
	vc->dgram = dgram;
	dgram->cache = vc;

	/*
	 * fill in the mbox smsg info and pack in to the
	 * datagram IN payload
	 */

	smsg_mbox_attr.msg_type = GNI_SMSG_TYPE_MBOX_AUTO_RETRANSMIT;
	smsg_mbox_attr.msg_buffer = mbox->base;
	smsg_mbox_attr.buff_size =  nic->mem_per_mbox;
	smsg_mbox_attr.mem_hndl = *mbox->memory_handle;
	smsg_mbox_attr.mbox_offset = (uint64_t)mbox->offset;
	smsg_mbox_attr.mbox_maxcredit = 64; /* TODO: fix this */
	smsg_mbox_attr.msg_maxsize =  16384;  /* TODO: definite fix */

	/*
	 * pack the things we need into the datagram in_box:
	 * - vc conn request type
	 * - vc_id of the connection
	 * - the smsg_mbox_attr stuff
	 */

	len = _gnix_dgram_pack_buf(dgram, GNIX_DGRAM_IN_BUF,
				   &rtype,
				   sizeof(enum gnix_vc_conn_req_type));
	assert(len == sizeof(enum gnix_vc_conn_req_type));

	len = _gnix_dgram_pack_buf(dgram, GNIX_DGRAM_IN_BUF,
				   &vc->vc_id, sizeof(vc->vc_id));
	assert(len == sizeof(vc->vc_id));

	len = _gnix_dgram_pack_buf(dgram, GNIX_DGRAM_IN_BUF,
				   &smsg_mbox_attr,
				   sizeof(gni_smsg_attr_t));
	assert(len == sizeof(gni_smsg_attr_t));

	ret = _gnix_dgram_wc_post(dgram);
	if (ret != FI_SUCCESS)
		goto err1;

	return ret;

err1:
	ret = _gnix_mbox_free(mbox);
err:
	return ret;
}

/*
 * TODO: this is very simple right now and will need more
 * work to propertly disconnect
 */

int _gnix_vc_disconnect(struct gnix_vc *vc)
{
	GNIX_TRACE(FI_LOG_EP_CTRL, "\n");

	vc->conn_state = GNIX_VC_CONN_TERMINATED;
	return FI_SUCCESS;
}

int _gnix_vc_add_to_wq(struct gnix_vc *vc)
{
	struct gnix_nic *nic = vc->ep->nic;
	struct gnix_work_req *work_req;

	GNIX_TRACE(FI_LOG_EP_CTRL, "\n");

	if (!(vc->modes & GNIX_VC_MODE_IN_WQ)) {

		work_req = calloc(1, sizeof(*work_req));
		if (work_req == NULL)
			return -FI_ENOMEM;

		work_req->progress_fn = __gnix_vc_prog_fn;
		work_req->data = vc;

		work_req->completer_fn = __gnix_vc_comp_fn;
		work_req->completer_data = vc;

		fastlock_acquire(&nic->wq_lock);
		list_add_tail(&nic->nic_wq, &work_req->list);
		fastlock_release(&nic->wq_lock);
		vc->modes |= GNIX_VC_MODE_IN_WQ;
	}

	return FI_SUCCESS;
}

int _gnix_vc_queue_tx_req(struct gnix_fab_req *req)
{
	struct gnix_vc *vc = req->vc;
	int rc;

	if (likely(vc->conn_state == GNIX_VC_CONNECTED) &&
		   slist_empty(&vc->tx_queue)) {
		/* try post */
		rc = req->send_fn(req);
		if (rc) {
			/* queue request, TODO locking */
			slist_insert_tail(&req->slist, &vc->tx_queue);
			GNIX_INFO(FI_LOG_EP_DATA,
				  "Queued request (%p) on full VC\n",
				  req);
		}
	} else {
		/* queue request, TODO locking */
		slist_insert_tail(&req->slist, &vc->tx_queue);
		GNIX_INFO(FI_LOG_EP_DATA,
			  "Queued request (%p) on connecting VC\n",
			  req);
	}

	return FI_SUCCESS;
}

int _gnix_vc_push_tx_reqs(struct gnix_vc *vc)
{
	int ret = FI_SUCCESS;
	struct slist *list;
	struct slist_entry *item;
	struct gnix_fab_req *req;

	/*
	 * if vc is not in connected state can't push sends
	 */

	if (vc->conn_state != GNIX_VC_CONNECTED)
		return -FI_EAGAIN;

	/*
	 * TODO: quick return if no TXDs
	 */

	/*
	 * TODO: need a lock for send queue
	 */

	list = &vc->tx_queue;
	item = list->head;
	while (item != NULL) {
		req = (struct gnix_fab_req *)container_of(item,
							  struct gnix_fab_req,
							  slist);
		ret = req->send_fn(req);
		if (ret == -EAGAIN) {
			GNIX_INFO(FI_LOG_EP_DATA,
				  "TX request queue stalled: %p\n",
				  req);
			break;
		} else if (ret) {
			GNIX_WARN(FI_LOG_EP_DATA,
				  "Failed to push TX request %p: %d\n",
				  req, ret);
		} else {
			GNIX_INFO(FI_LOG_EP_DATA,
				  "TX request processed: %p\n",
				  req);
		}

		slist_remove_head(&vc->tx_queue);
		item = list->head;
	}

	/*
	 * release tx_queue lock
	 */

	return ret;
}

int _gnix_ep_get_vc(struct gnix_fid_ep *ep, fi_addr_t dest_addr,
			struct gnix_vc **vc_ptr)
{
	int ret = FI_SUCCESS;
	struct gnix_vc *vc = NULL;
	struct gnix_fid_av *av;
	gnix_ht_key_t key;

	av = ep->av;
	assert(av != NULL);
	if (av->type == FI_AV_MAP) {
		memcpy(&key, &dest_addr, sizeof(gnix_ht_key_t));
		vc = (struct gnix_vc *)_gnix_ht_lookup(ep->vc_ht,
							key);
		if (vc == NULL) {
			ret = _gnix_vc_alloc(ep,
					    dest_addr,
					    &vc);
			if (ret != FI_SUCCESS) {
				GNIX_WARN(FI_LOG_EP_DATA,
					  "_gnix_ht_alloc returned %d\n",
					  ret);
				goto err;
			}
			ret = _gnix_ht_insert(ep->vc_ht, key,
						vc);
			if (likely(ret == FI_SUCCESS)) {
				vc->modes |= GNIX_VC_MODE_IN_HT;
				fastlock_acquire(&ep->vc_list_lock);
				dlist_insert_tail(&vc->entry,
						  &ep->wc_vc_list);
				fastlock_release(&ep->vc_list_lock);
				ret = _gnix_vc_connect(vc);
				if (ret != FI_SUCCESS) {
					GNIX_WARN(FI_LOG_EP_DATA,
						"_gnix_ht_connect returned %d\n",
						   ret);
					goto err;
				}
			} else if (ret == -FI_ENOSPC) {
				vc = _gnix_ht_lookup(ep->vc_ht, key);
				assert(vc != NULL);
				assert(vc->modes & GNIX_VC_MODE_IN_HT);
			} else {
				GNIX_WARN(FI_LOG_EP_DATA,
					  "_gnix_ht_insert returned %d\n",
					   ret);
				goto err;
			}
		}
		*vc_ptr = vc;
	} else {
		/*
		 * TODO: need thread safety for vc_table
		 */
		vc = ep->vc_table[dest_addr];
		if (vc == NULL) {
			ret = _gnix_vc_alloc(ep,
					    dest_addr, /*TODO: need translate */
					    &vc);
			if (ret == FI_SUCCESS) {
				ep->vc_table[dest_addr] = vc;
				ret = _gnix_vc_connect(vc);
				if (ret != FI_SUCCESS) {
					GNIX_WARN(FI_LOG_EP_DATA,
						  "_gnix_ht_connect returned %d\n",
						   ret);
					goto err;
				}
			} else {
				GNIX_WARN(FI_LOG_EP_DATA,
					  "_gnix_ht_alloc returned %d\n",
					   ret);
				goto err;
			}
		}
		*vc_ptr = vc;
	}

	return ret;
err:
	if (vc != NULL)
		_gnix_vc_destroy(vc);
	return ret;
}

