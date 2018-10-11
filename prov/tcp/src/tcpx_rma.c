/*
 * Copyright (c) 2018 Intel Corporation. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *	   Redistribution and use in source and binary forms, with or
 *	   without modification, are permitted provided that the following
 *	   conditions are met:
 *
 *		- Redistributions of source code must retain the above
 *		  copyright notice, this list of conditions and the following
 *		  disclaimer.
 *
 *		- Redistributions in binary form must reproduce the above
 *		  copyright notice, this list of conditions and the following
 *		  disclaimer in the documentation and/or other materials
 *		  provided with the distribution.
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

#include <rdma/fi_errno.h>
#include "rdma/fi_eq.h"
#include "ofi_iov.h"
#include <ofi_prov.h>
#include "tcpx.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <ofi_util.h>
#include <unistd.h>
#include <string.h>
#include <poll.h>
#include <arpa/inet.h>
#include <netdb.h>

static void tcpx_rma_read_send_entry_fill(struct tcpx_xfer_entry *send_entry,
					  struct tcpx_ep *tcpx_ep,
					  const struct fi_msg_rma *msg)
{
	memcpy(send_entry->msg_hdr.rma_iov, msg->rma_iov,
	       msg->rma_iov_count * sizeof(msg->rma_iov[0]));
	send_entry->msg_hdr.rma_iov_cnt = msg->rma_iov_count;

	send_entry->msg_data.iov[0].iov_base = (void *) &send_entry->msg_hdr;
	send_entry->msg_data.iov[0].iov_len = sizeof(send_entry->msg_hdr);
	send_entry->msg_data.iov_cnt = 1;
	send_entry->ep = tcpx_ep;
	send_entry->done_len = 0;
	send_entry->flags = 0;
}

static void tcpx_rma_read_recv_entry_fill(struct tcpx_xfer_entry *recv_entry,
					  struct tcpx_ep *tcpx_ep,
					  const struct fi_msg_rma *msg,
					  uint64_t flags)
{
	memcpy(&recv_entry->msg_data.iov[0], &msg->msg_iov[0],
	       msg->iov_count * sizeof(struct iovec));

	recv_entry->msg_data.iov_cnt = msg->iov_count;
	recv_entry->ep = tcpx_ep;
	recv_entry->context = msg->context;
	recv_entry->done_len = 0;
	recv_entry->flags = ((tcpx_ep->util_ep.tx_op_flags & FI_COMPLETION) |
			     flags | FI_RMA | FI_READ);
}

static ssize_t tcpx_rma_readmsg(struct fid_ep *ep, const struct fi_msg_rma *msg,
				uint64_t flags)
{
	struct tcpx_ep *tcpx_ep;
	struct tcpx_cq *tcpx_cq;
	struct tcpx_xfer_entry *send_entry;
	struct tcpx_xfer_entry *recv_entry;

	tcpx_ep = container_of(ep, struct tcpx_ep, util_ep.ep_fid);
	tcpx_cq = container_of(tcpx_ep->util_ep.tx_cq, struct tcpx_cq,
			       util_cq);

	assert(msg->iov_count <= TCPX_IOV_LIMIT);
	assert(msg->rma_iov_count <= TCPX_IOV_LIMIT);

	send_entry = tcpx_xfer_entry_alloc(tcpx_cq, TCPX_OP_READ_REQ);
	if (!send_entry)
		return -FI_EAGAIN;

	recv_entry = tcpx_xfer_entry_alloc(tcpx_cq, TCPX_OP_READ_RSP);
	if (!recv_entry) {
		tcpx_xfer_entry_release(tcpx_cq, send_entry);
		return -FI_EAGAIN;
	}
	tcpx_rma_read_send_entry_fill(send_entry, tcpx_ep, msg);
	tcpx_rma_read_recv_entry_fill(recv_entry, tcpx_ep, msg, flags);

	fastlock_acquire(&tcpx_ep->lock);
	slist_insert_tail(&recv_entry->entry, &tcpx_ep->rma_read_queue);
	tcpx_tx_queue_insert(tcpx_ep, send_entry);
	fastlock_release(&tcpx_ep->lock);
	return FI_SUCCESS;
}

static ssize_t tcpx_rma_read(struct fid_ep *ep, void *buf, size_t len, void *desc,
			     fi_addr_t src_addr, uint64_t addr, uint64_t key, void *context)
{
	struct iovec msg_iov = {
		.iov_base = (void *)buf,
		.iov_len = len,
	};
	struct fi_rma_iov rma_iov = {
		.addr = addr,
		.key = key,
		.len = len,
	};
	struct fi_msg_rma msg = {
		.msg_iov = &msg_iov,
		.desc = &desc,
		.iov_count = 1,
		.rma_iov_count = 1,
		.rma_iov = &rma_iov,
		.addr = src_addr,
		.context = context,
		.data = 0,
	};

	return tcpx_rma_readmsg(ep, &msg, 0);
}

static ssize_t tcpx_rma_readv(struct fid_ep *ep, const struct iovec *iov, void **desc,
			      size_t count, fi_addr_t src_addr, uint64_t addr, uint64_t key,
			      void *context)
{
	struct fi_rma_iov rma_iov = {
		.addr = addr,
		.len = ofi_total_iov_len(iov, count),
		.key = key,
	};
	struct fi_msg_rma msg = {
		.msg_iov = iov,
		.desc = desc,
		.iov_count = count,
		.rma_iov_count = 1,
		.rma_iov = &rma_iov,
		.addr = src_addr,
		.context = context,
		.data = 0,
	};

	return tcpx_rma_readmsg(ep, &msg, 0);
}

static ssize_t tcpx_rma_writemsg(struct fid_ep *ep, const struct fi_msg_rma *msg,
				 uint64_t flags)
{
	struct tcpx_ep *tcpx_ep;
	struct tcpx_cq *tcpx_cq;
	struct tcpx_xfer_entry *send_entry;
	uint64_t data_len;

	tcpx_ep = container_of(ep, struct tcpx_ep, util_ep.ep_fid);
	tcpx_cq = container_of(tcpx_ep->util_ep.tx_cq, struct tcpx_cq,
			       util_cq);

	send_entry = tcpx_xfer_entry_alloc(tcpx_cq, TCPX_OP_WRITE);
	if (!send_entry)
		return -FI_EAGAIN;

	assert(msg->iov_count < TCPX_IOV_LIMIT);
	assert(msg->rma_iov_count < TCPX_IOV_LIMIT);

	data_len = ofi_total_iov_len(msg->msg_iov, msg->iov_count);

	assert(!(flags & FI_INJECT) || (data_len <= TCPX_MAX_INJECT_SZ));

	send_entry->msg_hdr.hdr.version = OFI_CTRL_VERSION;
	send_entry->msg_hdr.hdr.op = ofi_op_write;
	send_entry->msg_hdr.hdr.op_data = TCPX_OP_WRITE;
	send_entry->msg_hdr.hdr.size = htonll(data_len + sizeof(send_entry->msg_hdr));
	send_entry->msg_hdr.hdr.flags = 0;

	memcpy(send_entry->msg_hdr.rma_iov, msg->rma_iov,
	       msg->rma_iov_count * sizeof(msg->rma_iov[0]));
	send_entry->msg_hdr.rma_iov_cnt = msg->rma_iov_count;

	send_entry->msg_data.iov[0].iov_base = (void *) &send_entry->msg_hdr;
	send_entry->msg_data.iov[0].iov_len = sizeof(send_entry->msg_hdr);
	send_entry->msg_data.iov_cnt = msg->iov_count + 1;

	if (flags & FI_INJECT) {
		ofi_copy_iov_buf(msg->msg_iov, msg->iov_count, 0,
				 send_entry->msg_data.inject,
				 data_len,
				 OFI_COPY_IOV_TO_BUF);

		send_entry->msg_data.iov[1].iov_base = (void *)send_entry->msg_data.inject;
		send_entry->msg_data.iov[1].iov_len = data_len;
		send_entry->msg_data.iov_cnt++;
	} else {
		memcpy(&send_entry->msg_data.iov[1], &msg->msg_iov[0],
		       msg->iov_count * sizeof(struct iovec));
	}

	if (flags & FI_REMOTE_CQ_DATA) {
		send_entry->msg_hdr.hdr.flags |= OFI_REMOTE_CQ_DATA;
		send_entry->msg_hdr.hdr.data = htonll(msg->data);
	}

	send_entry->flags = ((tcpx_ep->util_ep.tx_op_flags & FI_COMPLETION) |
			     flags | FI_RMA | FI_WRITE);

	if (flags & (FI_TRANSMIT_COMPLETE | FI_DELIVERY_COMPLETE)) {
		send_entry->flags &= ~FI_COMPLETION;
		send_entry->msg_hdr.hdr.flags |= OFI_DELIVERY_COMPLETE;
	}

	if (flags & FI_COMMIT_COMPLETE) {
		send_entry->flags &= ~FI_COMPLETION;
		send_entry->msg_hdr.hdr.flags |= OFI_COMMIT_COMPLETE;
	}

	send_entry->msg_hdr.hdr.flags = htonl(send_entry->msg_hdr.hdr.flags);
	send_entry->ep = tcpx_ep;
	send_entry->context = msg->context;
	send_entry->done_len = 0;

	fastlock_acquire(&tcpx_ep->lock);
	tcpx_tx_queue_insert(tcpx_ep, send_entry);
	fastlock_release(&tcpx_ep->lock);
	return FI_SUCCESS;
}

static ssize_t tcpx_rma_write(struct fid_ep *ep, const void *buf, size_t len, void *desc,
			      fi_addr_t dest_addr, uint64_t addr, uint64_t key, void *context)
{
	struct iovec msg_iov = {
		.iov_base = (void *)buf,
		.iov_len = len,
	};
	struct fi_rma_iov rma_iov = {
		.addr = addr,
		.key = key,
		.len = len,
	};
	struct fi_msg_rma msg = {
		.msg_iov = &msg_iov,
		.desc = &desc,
		.iov_count = 1,
		.rma_iov_count = 1,
		.rma_iov = &rma_iov,
		.addr = dest_addr,
		.context = context,
		.data = 0,
	};

	return tcpx_rma_writemsg(ep, &msg, 0);
}

static ssize_t tcpx_rma_writev(struct fid_ep *ep, const struct iovec *iov, void **desc,
			       size_t count, fi_addr_t dest_addr, uint64_t addr, uint64_t key,
			       void *context)
{
	struct fi_rma_iov rma_iov = {
		.addr = addr,
		.key = key,
		.len = ofi_total_iov_len(iov, count),
	};
	struct fi_msg_rma msg = {
		.msg_iov = iov,
		.desc = desc,
		.iov_count = count,
		.rma_iov_count = 1,
		.rma_iov = &rma_iov,
		.addr = dest_addr,
		.context = context,
		.data = 0,
	};

	return tcpx_rma_writemsg(ep, &msg, 0);
}


static ssize_t tcpx_rma_writedata(struct fid_ep *ep, const void *buf, size_t len, void *desc,
				  uint64_t data, fi_addr_t dest_addr, uint64_t addr, uint64_t key,
				  void *context)
{
	struct iovec msg_iov = {
		.iov_base = (void *)buf,
		.iov_len = len,
	};
	struct fi_rma_iov rma_iov = {
		.addr = addr,
		.key = key,
		.len = len,
	};
	struct fi_msg_rma msg = {
		.desc = &desc,
		.iov_count = 1,
		.rma_iov_count = 1,
		.rma_iov = &rma_iov,
		.msg_iov = &msg_iov,
		.addr = dest_addr,
		.context = context,
		.data = data,
	};

	return tcpx_rma_writemsg(ep, &msg, FI_REMOTE_CQ_DATA);
}

static ssize_t tcpx_rma_inject_common(struct fid_ep *ep, const void *buf,
				      size_t len, uint64_t data,
				      fi_addr_t dest_addr, uint64_t addr,
				      uint64_t key, uint64_t flags)
{
	struct tcpx_ep *tcpx_ep;
	struct tcpx_cq *tcpx_cq;
	struct tcpx_xfer_entry *send_entry;

	tcpx_ep = container_of(ep, struct tcpx_ep, util_ep.ep_fid);
	tcpx_cq = container_of(tcpx_ep->util_ep.tx_cq, struct tcpx_cq,
			       util_cq);

	send_entry = tcpx_xfer_entry_alloc(tcpx_cq, TCPX_OP_WRITE);
	if (!send_entry)
		return -FI_EAGAIN;

	assert(len <= TCPX_MAX_INJECT_SZ);
	send_entry->msg_hdr.hdr.size = htonll(len + sizeof(send_entry->msg_hdr));
	send_entry->msg_hdr.hdr.flags = 0;

	send_entry->msg_hdr.rma_iov[0].addr = addr;
	send_entry->msg_hdr.rma_iov[0].key = key;
	send_entry->msg_hdr.rma_iov[0].len = len;
	send_entry->msg_hdr.rma_iov_cnt = 1;

	send_entry->msg_data.iov[0].iov_base = (void *) &send_entry->msg_hdr;
	send_entry->msg_data.iov[0].iov_len = sizeof(send_entry->msg_hdr);

	memcpy(send_entry->msg_data.inject, (uint8_t *)buf, len);
	send_entry->msg_data.iov[1].iov_base = (void *)send_entry->msg_data.inject;
	send_entry->msg_data.iov[1].iov_len = len;

	send_entry->msg_data.iov_cnt = 2;

	if (flags & FI_REMOTE_CQ_DATA) {
		send_entry->msg_hdr.hdr.flags |= OFI_REMOTE_CQ_DATA;
		send_entry->msg_hdr.hdr.data = htonll(data);
	}

	send_entry->msg_hdr.hdr.flags = htonl(send_entry->msg_hdr.hdr.flags);
	send_entry->ep = tcpx_ep;
	send_entry->done_len = 0;

	fastlock_acquire(&tcpx_ep->lock);
	tcpx_tx_queue_insert(tcpx_ep, send_entry);
	fastlock_release(&tcpx_ep->lock);
	return FI_SUCCESS;
}

static ssize_t tcpx_rma_inject(struct fid_ep *ep, const void *buf, size_t len,
			       fi_addr_t dest_addr, uint64_t addr, uint64_t key)
{
	return tcpx_rma_inject_common(ep, buf, len, dest_addr,
				      0, addr, key, FI_INJECT);
}

static ssize_t tcpx_rma_injectdata(struct fid_ep *ep, const void *buf, size_t len,
				   uint64_t data, fi_addr_t dest_addr, uint64_t addr, uint64_t key)
{
	return tcpx_rma_inject_common(ep, buf, len, dest_addr,
				      data, addr, key,
				      FI_INJECT | FI_REMOTE_CQ_DATA);
}

struct fi_ops_rma tcpx_rma_ops = {
	.size = sizeof(struct fi_ops_rma),
	.read = tcpx_rma_read,
	.readv = tcpx_rma_readv,
	.readmsg = tcpx_rma_readmsg,
	.write = tcpx_rma_write,
	.writev = tcpx_rma_writev,
	.writemsg = tcpx_rma_writemsg,
	.inject = tcpx_rma_inject,
	.writedata = tcpx_rma_writedata,
	.injectdata = tcpx_rma_injectdata,
};
