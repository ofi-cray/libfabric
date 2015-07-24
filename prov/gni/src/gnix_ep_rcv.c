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
#include "gnix_vc.h"
#include "gnix_ep.h"
#include "gnix_hashtable.h"
#include "gnix_cntr.h"

/*******************************************************************************
 * Helper functions used for handling receipt of messages on GNIX EPs
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

			GNIX_INFO(FI_LOG_EP_DATA, "Matched request: %p\n", req);

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

		GNIX_INFO(FI_LOG_EP_DATA, "Unmatched request: %p\n", req);
	}

err:
	fastlock_release(&ep->recv_queue_lock);
	return ret;
}

