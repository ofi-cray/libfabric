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

#ifndef _GNIX_EP_H_
#define _GNIX_EP_H_

#include "gnix.h"
#include "gnix_nic.h"

/*
 * enum of tags used for GNI_SmsgSendWTag
 * and callbacks at receive side to process
 * these messages
 */

enum {
	GNIX_SMSG_T_EGR_W_DATA = 10,
	GNIX_SMSG_T_EGR_W_DATA_ACK,
	GNIX_SMSG_T_EGR_GET,
	GNIX_SMSG_T_EGR_GET_ACK,
	GNIX_SMSG_T_RNDZV_RTS,
	GNIX_SMSG_T_RNDZV_RTR,
	GNIX_SMSG_T_RNDZV_COOKIE,
	GNIX_SMSG_T_RNDZV_SDONE,
	GNIX_SMSG_T_RNDZV_RDONE
};

extern smsg_completer_fn_t gnix_ep_smsg_completers[];

/*
 * prototypes for GNI EP helper functions for managing
 * transactions: send/recv, etc.
 */

/**
 * @brief  match an incoming SMSG eager message that includes all
 *         message data with posted receive buffer, or if none present,
 *         add to unexpected receive queue.
 *
 * @param[in] ep        pointer to a previously allocated endpoint
 * @param[in] msg       pointer to msg data in SMSG mailbox buffer
 * @param[in] addr      address of the sender of the message
 * @param[in] len       length of the message
 * @param[in] imm       immediate data
 * @param[in] sflags    flags used on sender side to indicate presence
 *                      of immediate data - FI_REMOTE_CQ_DATA
 * @return              FI_SUCCESS on success, -FI_ENOMEM insufficient
 *                      memory to create unexpected request structure
 */
int _gnix_ep_eager_msg_w_data_match(struct gnix_fid_ep *ep, void *msg,
				    struct gnix_address addr, size_t len,
				    uint64_t imm, uint64_t sflags);

/**
 * @brief  dequeue smsg messages that arrived before vc fully
 *         initialized at receiver
 *
 * @param[in] vc        pointer to a previously allocated vc
 * @return              FI_SUCCESS on success meaning that no errors
 *                      were encountered dequeing SMSG messages, -FI_EINVAL
 *                      if an invalid argument is supplied,
 *                      -FI_EAGAIN if the SMSG channel is in an
 *                      invalid state.
 */
int _gnix_ep_vc_dequeue_smsg(struct gnix_vc *vc);
/*
 * typedefs for function vectors used to steer send/receive/rma/amo requests,
 * i.e. fi_send, fi_recv, etc. to ep type specific methods
 */

typedef ssize_t (*send_func_t)(struct fid_ep *ep, const void *buf,
				size_t len, void *desc,
				fi_addr_t dest_addr, void *context);

typedef ssize_t (*sendv_func_t)(struct fid_ep *ep, const struct iovec *iov,
				void **desc, size_t count,
				fi_addr_t dest_addr, void *context);

typedef ssize_t (*sendmsg_func_t)(struct fid_ep *ep, const struct fi_msg *msg,
				   uint64_t flags);

typedef ssize_t (*msg_inject_func_t)(struct fid_ep *ep, const void *buf,
					size_t len, fi_addr_t dest_addr);

typedef ssize_t (*recv_func_t)(struct fid_ep *ep, const void *buf,
				size_t len, void *desc,
				fi_addr_t dest_addr, void *context);

typedef ssize_t (*recvv_func_t)(struct fid_ep *ep, const struct iovec *iov,
				 void **desc, size_t count,
				 fi_addr_t dest_addr, void *context);

typedef ssize_t (*recvmsg_func_t)(struct fid_ep *ep, const struct fi_msg *msg,
				  uint64_t flags);

typedef ssize_t (*tsend_func_t)(struct fid_ep *ep, const void *buf,
				 size_t len, void *desc,
				 fi_addr_t dest_addr, uint64_t tag,
				 void *context);

typedef ssize_t (*tsendv_func_t)(struct fid_ep *ep, const struct iovec *iov,
				  void **desc, size_t count,
				  fi_addr_t dest_addr, uint64_t tag,
				  void *context);

typedef ssize_t (*tsendmsg_func_t)(struct fid_ep *ep,
				    const struct fi_msg_tagged *msg,
				    uint64_t flags);

typedef ssize_t (*tinject_func_t)(struct fid_ep *ep,
				   const void *buf,
				   size_t len,
				   fi_addr_t dest_addr,
				   uint64_t flags);

typedef ssize_t (*trecv_func_t)(struct fid_ep *ep,
				 void *buf,
				 size_t len,
				 void *desc,
				 fi_addr_t src_addr,
				 uint64_t tag,
				 uint64_t ignore,
				 void *context);

typedef ssize_t (*trecvv_func_t)(struct fid_ep *ep,
				 const struct iovec *iov,
				 void **desc,
				 size_t count,
				 fi_addr_t src_addr,
				 uint64_t tag,
				 uint64_t ignore,
				 void *context);

typedef ssize_t (*trecvmsg_func_t)(struct fid_ep *ep,
				   const struct fi_msg_tagged *msg,
				   uint64_t flags);

/*
 * inline functions
 */

static inline struct gnix_fab_req *
_gnix_fr_alloc(struct gnix_fid_ep *ep)
{
	struct slist_entry *se;
	struct gnix_fab_req *fr = NULL;
	int ret = _gnix_sfe_alloc(&se, &ep->fr_freelist);

	while (ret == -FI_EAGAIN)
		ret = _gnix_sfe_alloc(&se, &ep->fr_freelist);

	if (ret == FI_SUCCESS) {
		fr = container_of(se, struct gnix_fab_req, slist);
		fr->gnix_ep = ep;
	}

	return fr;
}

static inline void
_gnix_fr_free(struct gnix_fid_ep *ep, struct gnix_fab_req *fr)
{
	assert(fr->gnix_ep == ep);
	_gnix_sfe_free(&fr->slist, &ep->fr_freelist);
}

static inline int
__msg_match_fab_req(struct slist_entry *item, const void *arg)
{
	struct gnix_fab_req *req;
	const struct gnix_address *addr_ptr = arg;

	req = container_of(item, struct gnix_fab_req, slist);

	return ((GNIX_ADDR_UNSPEC(*addr_ptr)) ||
				(GNIX_ADDR_EQUAL(req->addr, *addr_ptr)));
}


#endif /* _GNIX_EP_H_ */
