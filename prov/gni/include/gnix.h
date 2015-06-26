/*
 * Copyright (c) 2015 Cray Inc.  All rights reserved.
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

#ifndef _GNIX_H_
#define _GNIX_H_

#ifdef __cplusplus
extern "C" {
#endif

#if HAVE_CONFIG_H
#include <config.h>
#endif /* HAVE_CONFIG_H */

#include <stdbool.h>

#include <rdma/fabric.h>
#include <rdma/fi_atomic.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_eq.h>
#include <rdma/fi_errno.h>
#include <rdma/fi_prov.h>
#include <rdma/fi_rma.h>
#include <rdma/fi_tagged.h>
#include <rdma/fi_trigger.h>

#include <fi.h>
#include <fi_enosys.h>
#include <fi_indexer.h>
#include <fi_rbuf.h>
#include <fi_list.h>
#include "gni_pub.h"
#include "ccan/list.h"
#include "gnix_util.h"
#include "gnix_freelist.h"
#include "gnix_mr.h"

#include "gnix_cq.h"

#define GNI_MAJOR_VERSION 0
#define GNI_MINOR_VERSION 5

/*
 * useful macros
 */
#ifndef likely
#define likely(x) __builtin_expect((x), 1)
#endif
#ifndef unlikely
#define unlikely(x) __builtin_expect((x), 0)
#endif

#ifndef FLOOR
#define FLOOR(a, b) ((long long)(a) - (((long long)(a)) % (b)))
#endif

#ifndef CEILING
#define CEILING(a, b) ((long long)(a) <= 0LL ? 0 : (FLOOR((a)-1, b) + (b)))
#endif

#ifndef IN
#define IN
#endif

#ifndef OUT
#define OUT
#endif

#ifndef INOUT
#define INOUT
#endif

/*
 * Flags
 * The 64-bit flag field is used as follows:
 * 1-grow up    common (usable with multiple operations)
 * 59-grow down operation specific (used for single call/class)
 * 60 - 63      provider specific
 */

#define GNIX_SUPPRESS_COMPLETION	(1ULL << 60)	/* TX only flag */
#define GNIX_RMA_RDMA			(1ULL << 61)	/* RMA only flag */

/*
 * Cray gni provider supported flags for fi_getinfo argument for now, needs
 * refining (see fi_getinfo.3 man page)
 */
#define GNIX_SUPPORTED_FLAGS (FI_NUMERICHOST | FI_SOURCE)

#define GNIX_DEFAULT_FLAGS (0)

/*
 * Cray gni provider will try to support the fabric interface capabilities (see
 * fi_getinfo.3 man page)
 * for RDM and MSG (future) endpoint types.
 */

/*
 * see capabilities section in fi_getinfo.3
 */
#define GNIX_EP_RDM_CAPS                                                       \
	(FI_MSG | FI_RMA | FI_TAGGED | FI_ATOMICS |                            \
	 FI_DIRECTED_RECV | FI_MULTI_RECV | FI_INJECT | FI_SOURCE | FI_READ |  \
	 FI_WRITE | FI_SEND | FI_RECV | FI_REMOTE_READ | FI_REMOTE_WRITE |     \
	 FI_TRANSMIT_COMPLETE | FI_FENCE)

/*
 * see Operations flags in fi_endpoint.3
 */
#define GNIX_EP_OP_FLAGS                                                       \
	(FI_MULTI_RECV | FI_COMPLETION |                                       \
	 FI_TRANSMIT_COMPLETE | FI_READ | FI_WRITE | FI_SEND | FI_RECV |       \
	 FI_REMOTE_READ | FI_REMOTE_WRITE)

/*
 * if this has to be changed, check gnix_getinfo, etc.
 */
#define GNIX_EP_MSG_CAPS GNIX_EP_RDM_CAPS

#define GNIX_MAX_MSG_SIZE ((0x1ULL << 32) - 1)
#define GNIX_CACHELINE_SIZE (64)
#define GNIX_INJECT_SIZE GNIX_CACHELINE_SIZE

/*
 * Cray gni provider will require the following fabric interface modes (see
 * fi_getinfo.3 man page)
 */
#define GNIX_FAB_MODES (FI_CONTEXT | FI_LOCAL_MR)

/*
 * fabric modes that GNI provider doesn't need
 */
#define GNIX_FAB_MODES_CLEAR (FI_MSG_PREFIX | FI_ASYNC_IOV)

/*
 * gnix address format - used for fi_send/fi_recv, etc.
 */
struct gnix_address {
	uint32_t device_addr;
	uint32_t cdm_id;
};

/*
 * macro for testing whether a gnix_address value is FI_ADDR_UNSPEC
 */

#define GNIX_ADDR_UNSPEC(var) (((var).device_addr == -1) && \
				((var).cdm_id == -1))
/*
 * macro testing for gnix_address equality
 */

#define GNIX_ADDR_EQUAL(a, b) (((a).device_addr == (b).device_addr) && \
				((a).cdm_id == (b).cdm_id))


/*
 * info returned by fi_getname/fi_getpeer - has enough
 * side band info for RDM ep's to be able to connect, etc.
 */
struct gnix_ep_name {
	struct gnix_address gnix_addr;
	struct {
		uint32_t name_type : 8;
		uint32_t unused : 24;
		uint32_t cookie;
	};
	uint64_t reserved[4];
};

/*
 * enum for blocking/non-blocking progress
 */
enum gnix_progress_type {
	GNIX_PRG_BLOCKING,
	GNIX_PRG_NON_BLOCKING
};

/*
 * simple struct for gnix fabric, may add more stuff here later
 */
struct gnix_fid_fabric {
	struct fid_fabric fab_fid;
	/* llist of domains's opened from fabric */
	struct list_head domain_list;
	/* number of bound datagrams for domains opened from
	 * this fabric object - used by cm nic*/
	int n_bnd_dgrams;
	/* number of wildcard datagrams for domains opened from
	 * this fabric object - used by cm nic*/
	int n_wc_dgrams;
	/* timeout datagram completion - see
	 * GNI_PostdataProbeWaitById in gni_pub.h */
	uint64_t datagram_timeout;
	atomic_t ref_cnt;
};

extern struct fi_ops_cm gnix_cm_ops;

/*
 * a gnix_fid_domain is associated with one or more gnix_nic's.
 * the gni_nics are in turn associated with ep's opened off of the
 * domain.  The gni_nic's are use for data motion - sending/receivng
 * messages, rma ops, etc.
 */
struct gnix_fid_domain {
	struct fid_domain domain_fid;
	/* used for fabric object llist of domains*/
	struct list_node list;
	/* list nics this domain is attached to, TODO: thread safety */
	struct dlist_entry nic_list;
	struct gnix_fid_fabric *fabric;
	uint8_t ptag;
	uint32_t cookie;
	uint32_t cdm_id_seed;
	/* work queue for domain */
	struct list_head domain_wq;
	/* size of gni tx cqs for this domain */
	uint32_t gni_tx_cq_size;
	/* size of gni rx cqs for this domain */
	uint32_t gni_rx_cq_size;
	/* additional gni cq modes to use for this domain */
	gni_cq_mode_t gni_cq_modes;
	/* additional gni cq modes to use for this domain */
	enum fi_progress control_progress;
	enum fi_progress data_progress;
	atomic_t ref_cnt;
	gnix_mr_cache_t mr_cache;
};

#define GNIX_CQS_PER_EP		8

/*
 *   gnix endpoint structure
 *
 * A gnix_cm_nic is associated with an EP if it is of type  FI_EP_RDM.
 * The gnix_cm_nic is used for building internal connections between the
 * endpoints at different addresses.
 */
struct gnix_fid_ep {
	struct fid_ep ep_fid;
	enum fi_ep_type type;
	struct gnix_fid_domain *domain;
	uint64_t op_flags;
	struct gnix_fid_cq *send_cq;
	struct gnix_fid_cq *recv_cq;
	struct gnix_fid_av *av;
	/* cm nic bound to this ep (FID_EP_RDM only) */
	struct gnix_cm_nic *cm_nic;
	struct gnix_nic *nic;
	union {
		struct gnix_hashtable *vc_ht;
		struct gnix_vc **vc_table;      /* used for FI_AV_TABLE */
		struct gnix_vc *vc;
	};
	fastlock_t vc_list_lock;
	struct dlist_entry wc_vc_list;
	/* used for unexpected receives */
	struct slist unexp_recv_queue;
	/* used for posted receives */
	struct slist posted_recv_queue;
	/* lock for unexp and posted recv queue */
	fastlock_t recv_queue_lock;
	struct slist pending_recv_comp_queue;
	/* pointer to tag matching engine */
	void *tag_matcher;
	int (*progress_fn)(struct gnix_fid_ep *, enum gnix_progress_type);
	/* RX specific progress fn */
	int (*rx_progress_fn)(struct gnix_fid_ep *, gni_return_t *rc);
	int enabled;
	int send_selective_completion;
	int recv_selective_completion;
	/* num. active read and write fab_reqs associated with this ep */
	atomic_t active_fab_reqs;
	struct gnix_s_freelist fr_freelist;
	atomic_t ref_cnt;
	fastlock_t lock;
};

struct gnix_addr_entry {
	struct gnix_address* addr;
	bool valid;
};

/*
 * TODO: Support shared named AVs
 */
struct gnix_fid_av {
	struct fid_av av_fid;
	struct gnix_fid_domain *domain;
	enum fi_av_type type;
	struct gnix_addr_entry* table;
	size_t addrlen;
	/* How many addresses AV can hold before it needs to be resized */
	size_t capacity;
	/* How many address are currently stored in AV */
	size_t count;
	atomic_t ref_cnt;
};

/*
 *  enums, defines, for gni provider internal fab requests.
 */

#define GNIX_FAB_RQ_M_IN_ACTIVE_LIST          0x00000001
#define GNIX_FAB_RQ_M_REPLAYABLE              0x00000002
#define GNIX_FAB_RQ_M_UNEXPECTED              0x00000004
#define GNIX_FAB_RQ_M_MATCHED                 0x00000008
#define GNIX_FAB_RQ_M_COMPLETE                0x00000010
#define GNIX_FAB_RQ_M_INJECT_DATA             0x00000020

enum gnix_fab_req_type {
	GNIX_FAB_RQ_SEND,
	GNIX_FAB_RQ_TSEND,
	GNIX_FAB_RQ_RDMA_WRITE,
	GNIX_FAB_RQ_RDMA_READ,
	GNIX_FAB_RQ_RECV,
	GNIX_FAB_RQ_TRECV
};

struct gnix_fab_req_rma {
	void *loc_md;
	uint64_t rem_addr;
	uint64_t rem_mr_key;
};

struct gnix_fab_req_msg {
	uint64_t tag;
	uint64_t mask_bits;
};

/*
 * Fabric request layout, there is a one to one
 * correspondence between an application's invocation of fi_send, fi_recv
 * and a gnix fab_req.
 */

struct gnix_fab_req {
	struct slist_entry        slist;
	uint64_t tag;
	uint64_t mask_bits;
	struct gnix_address addr;
	enum gnix_fab_req_type    type;
	struct gnix_fid_ep        *gnix_ep;
	void                      *user_context;
	/* matched_rcv_fab_req only applicable to GNIX_FAB_RQ_RECV type */
	struct gnix_fab_req       *matched_rcv_fab_req;
	/* current point in the buffer for next transfer chunk -
	   case of long messages or rdma requests greater than 4 GB */
	void     *cur_pos;
	struct gnix_vc *vc;
	int (*send_fn)(void *);
	void *completer_data;
	int (*completer_fn)(void *);
	uint64_t cq_flags;
	int modes;
	int retries;
	/* common to rma/amo/msg */
	uint64_t loc_addr;
	uint64_t imm;
	size_t len;
	uint64_t flags;
	union {
		struct gnix_fab_req_rma rma;
		struct gnix_fab_req_msg msg;
	};
	char inject_buf[GNIX_INJECT_SIZE];
};

/*
 * work queue struct, used for handling delay ops, etc. in a generic wat
 */

struct gnix_work_req {
	struct list_node list;
	/* function to be invoked to progress this work queue req.
	   first element is pointer to data needec by the func, second
	   is a pointer to an int which will be set to 1 if progress
	   function is complete */
	int (*progress_fn)(void *, int *);
	/* data to be passed to the progress function */
	void *data;
	/* function to be invoked if this work element has completed */
	int (*completer_fn)(void *);
	/* data for completer function */
	void *completer_data;
};

/*
 * globals
 */
extern const char gnix_fab_name[];
extern const char gnix_dom_name[];
extern uint32_t gnix_cdm_modes;
extern atomic_t gnix_id_counter;


/*
 * linked list helpers
 */

static inline void gnix_list_node_init(struct list_node *node)
{
	node->prev = node->next = NULL;
}

static inline void gnix_list_del_init(struct list_node *node)
{
	list_del(node);
	node->prev = node->next = node;
}

/*
 * prototypes for fi ops methods
 */
int gnix_domain_open(struct fid_fabric *fabric, struct fi_info *info,
		     struct fid_domain **domain, void *context);
int gnix_av_open(struct fid_domain *domain, struct fi_av_attr *attr,
		 struct fid_av **av, void *context);
int gnix_cq_open(struct fid_domain *domain, struct fi_cq_attr *attr,
		 struct fid_cq **cq, void *context);
int gnix_ep_open(struct fid_domain *domain, struct fi_info *info,
		 struct fid_ep **ep, void *context);
int gnix_eq_open(struct fid_fabric *fabric, struct fi_eq_attr *attr,
		 struct fid_eq **eq, void *context);

int gnix_mr_reg(struct fid *fid, const void *buf, size_t len,
		uint64_t access, uint64_t offset, uint64_t requested_key,
		uint64_t flags, struct fid_mr **mr_o, void *context);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _GNIX_H_ */
