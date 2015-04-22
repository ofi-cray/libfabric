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
#define GNIX_FAB_MODES (FI_CONTEXT | FI_LOCAL_MR | FI_PROV_MR_ATTR)

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
	/* number of directed datagrams for domains opened from
	 * this fabric object - used by cm nic*/
	int n_dgrams;
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
	struct list_head nic_list;
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
	atomic_t ref_cnt;
};

struct gnix_fid_mem_desc {
	struct fid_mr mr_fid;
	struct gnix_fid_domain *domain;
	gni_mem_handle_t mem_hndl;
};

/*
 *   gnix endpoint structure
 *
 * A gnix_cm_nic is associated with an EP if it is of type  FID_EP_RDM.
 * The gnix_cm_nic is used for building internal connections between the
 * endpoints at different addresses.
 */
struct gnix_fid_ep {
	struct fid_ep ep_fid;
	enum fi_ep_type type;
	struct gnix_fid_domain *domain;
	struct gnix_fid_cq *send_cq;
	struct gnix_fid_cq *recv_cq;
	struct gnix_fid_av *av;
	/* cm nic bound to this ep (FID_EP_RDM only) */
	struct gnix_cm_nic *cm_nic;
	struct gnix_nic *nic;
	union {
		void *vc_hash_hndl;  /*used for FI_AV_MAP */
		void *vc_table;      /* used for FI_AV_TABLE */
		void *vc;
	};
	/* used for unexpected receives */
	struct slist unexp_recv_queue;
	/* used for posted receives */
	struct slist posted_recv_queue;
	/* pointer to tag matching engine */
	void *tag_matcher;
	int (*progress_fn)(struct gnix_fid_ep *, enum gnix_progress_type);
	/* RX specific progress fn */
	int (*rx_progress_fn)(struct gnix_fid_ep *, gni_return_t *rc);
	int enabled;
	int no_want_cqes;
	/* num. active fab_reqs associated with this ep */
	atomic_t active_fab_reqs;
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
 * work queue struct, used for handling delay ops, etc. in a generic wat
 */
/*
 * gnix cm nic struct - to be used only for GNI_EpPostData, etc.
 */


struct gnix_cm_nic {
	fastlock_t lock;
	gni_cdm_handle_t gni_cdm_hndl;
	gni_nic_handle_t gni_nic_hndl;
	struct gnix_dgram_hndl *dgram_hndl;
	struct gnix_fid_domain *domain;
	/* work queue for cm nic */
	struct list_head cm_nic_wq;
	uint32_t cdm_id;
	uint8_t ptag;
	uint32_t cookie;
	uint32_t device_id;
	uint32_t device_addr;
};

/*
 * Stores events inside of the event queue.
 *
 * type: EQ event type defined in fi_eq.h
 * len: length of the event
 * flags: control flags
 * buf: event
 * entry: list entry, contains next pointer
 */
struct gnix_event {
	uint32_t type;
	size_t len;
	uint64_t flags;
	void *buf;
	struct slist_entry entry;
};

/*
 * EQ structure. Contains error and event queue.
 */
struct gnix_fid_eq {
	struct fid_eq eq_fid;
	struct fi_eq_attr attr;
	struct gnix_fid_fabric *eq_fabric;
	atomic_t ref_cnt;
	fastlock_t lock;

	struct slist err_queue;
	struct slist ev_queue;
};

/*
 * defines for connection state for gnix VC
 */
enum gnix_vc_conn_state {
	GNIX_VC_CONN_NONE = 1,
	GNIX_VC_CONNECTING,
	GNIX_VC_CONNECTED,
	GNIX_VC_CONN_TERMINATING,
	GNIX_VC_CONN_TERMINATED
};

/*
 * gnix vc struct - internal struc for managing send/recv,
 * rma, amo ops between endpoints.  For FI_EP_RDM, many vc's
 * may map to a single RDM depending on how many remote ep's have
 * been targeted for send/recv, rma, or amo ops.  For FI_EP_MSG
 * endpoint types, there is a one-to-one mapping between the ep
 * and a vc.
 *
 * Notes: This structure needs to be as small as possible as the
 *        as the number of gnix_vc's scales linearly with
 *        the number of peers in a domain with which a process sends/
 *        receives messages.
 *        these structs should be allocated out of a slab allocator
 *        using large pages to potentially reduce cpu and i/o mmu
 *        TlB pressure.
 */

struct gnix_vc {
	struct gnix_address peer_addr;
	void *smsg_mbox;
	gni_ep_handle_t gni_ep;
	/* used for send fab_reqs, these must go in order, hence an slist */
	struct slist send_queue;
	/* number of outstanding fab_reqs posted to this vc including rma ops, etc.*/
	atomic_t outstanding_fab_reqs_vc;
	/* connection status of this VC */
	enum gnix_vc_conn_state conn_state;
	atomic_t ref_cnt;
} __attribute__ ((aligned (GNIX_CACHELINE_SIZE)));

/*
 *  enums, defines, for gni provider internal fab requests.
 */

#define GNIX_FAB_RQ_M_IN_ACTIVE_LIST          0x00000001
#define GNIX_FAB_RQ_M_REPLAYABLE              0x00000002
#define GNIX_FAB_RQ_M_UNEXPECTED              0x00000004
#define GNIX_FAB_RQ_M_MATCHED                 0x00000008
#define GNIX_FAB_RQ_M_INJECT_DATA             0x00000010

enum gnix_fab_req_type {
	GNIX_FAB_RQ_SEND,
	GNIX_FAB_RQ_TSEND,
	GNIX_FAB_RQ_RDMA_WRITE,
	GNIX_FAB_RQ_RDMA_WRITE_IMM_DATA,
	GNIX_FAB_RQ_RDMA_READ,
	GNIX_FAB_RQ_RECV,
	GNIX_FAB_RQ_TRECV
};


/*
 * Fabric request layout, there is a one to one
 * correspondence between an application's invocation of fi_send, fi_recv
 * and a gnix fab_req.
 */

struct gnix_fab_req {
	struct list_node          list;
	struct slist_entry        slist;
	enum gnix_fab_req_type    type;
	struct gnix_fid_ep        *gnix_ep;
	void                      *user_context;
	/* matched_rcv_fab_req only applicable to GNIX_FAB_RQ_RECV type */
	struct gnix_fab_req       *matched_rcv_fab_req;
	void     *buf;
	/* current point in the buffer for next transfer chunk -
	   case of long messages or rdma requests greater than 4 GB */
	void     *cur_pos;
	size_t   len;
	uint64_t imm;
	uint64_t tag;
	struct gnix_vc *vc;
	void *completer_data;
	int (*completer_func)(void *,int *);
	int modes;
	int retries;
	uint32_t id;
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
	int (*progress_func)(void *,int *);
	/* data to be passed to the progress function */
	void *data;
	/* function to be invoked if this work element has completed */
	int (*completer_func)(void *);
	/* data for completer function */
	void *completer_data;
};

/*
 * GNI datagram related structs and defines.
 * The GNI_EpPostData, etc. are used to manage
 * connecting VC's for the FI_EP_RDM endpoint
 * type.
 */

struct gnix_dgram_hndl {
	struct gnix_cm_nic *nic;
	/* free list of datagrams   */
	struct list_head datagram_free_list;
	/* list of active datagrams   */
	struct list_head datagram_active_list;
	/* free list of wc datagrams   */
	struct list_head wc_datagram_free_list;
	/* list of active wc datagrams   */
	struct list_head wc_datagram_active_list;
	struct gnix_datagram *datagram_base;
	uint64_t datagram_timeout;
	int n_dgrams;
	int n_wc_dgrams;
};

enum gnix_dgram_type {
	GNIX_DGRAM_WC = 100,
	GNIX_DGRAM_BND
};

enum gnix_dgram_state {
	GNIX_DGRAM_STATE_FREE,
	GNIX_DGRAM_STATE_CONNECTING,
	GNIX_DGRAM_STATE_LISTENING,
	GNIX_DGRAM_STATE_CONNECTED,
	GNIX_DGRAM_STATE_ALREADY_CONNECTING
};

struct gnix_datagram {
	struct list_node        list;
	struct list_head        *free_list_head;
	gni_ep_handle_t         gni_ep;
	struct gnix_cm_nic      *nic;
	struct gnix_address     target_addr;
	enum gnix_dgram_state   state;
	enum gnix_dgram_type    type;
	struct gnix_dgram_hndl  *d_hndl;
	int  (*callback_fn)(struct gnix_datagram *,
			    struct gnix_address,
			    gni_post_state_t);
	char dgram_in_buf[GNI_DATAGRAM_MAXSIZE];
	char dgram_out_buf[GNI_DATAGRAM_MAXSIZE];
};

/*
 * globals
 */
extern const char gnix_fab_name[];
extern const char gnix_dom_name[];
extern uint32_t gnix_cdm_modes;

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

/*
 * prototypes for gni provider internal functions
 */

int gnix_dgram_hndl_alloc(const struct gnix_fid_fabric *fabric,
				struct gnix_cm_nic *cm_nic,
				struct gnix_dgram_hndl **hndl_ptr);
int gnix_dgram_hndl_free(struct gnix_dgram_hndl *hndl);
int gnix_dgram_alloc(struct gnix_dgram_hndl *hndl, enum gnix_dgram_type type,
			struct gnix_datagram **d_ptr);
int gnix_dgram_unbnd_post(struct gnix_datagram *d,
			gni_return_t *status_ptr);
int gnix_dgram_connect_post(struct gnix_datagram *d,
				gni_return_t *status_ptr);
void gnix_dgram_prog_thread_fn(void *the_arg);


#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _GNIX_H_ */
