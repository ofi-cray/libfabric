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

#define GNI_MAJOR_VERSION 0
#define GNI_MINOR_VERSION 5

/*
 * useful macros
 */
#define PFX "libfabric:gni"

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
	(FI_MSG | FI_RMA | FI_TAGGED | FI_ATOMICS | FI_BUFFERED_RECV |         \
	 FI_DIRECTED_RECV | FI_MULTI_RECV | FI_INJECT | FI_SOURCE | FI_READ |  \
	 FI_WRITE | FI_SEND | FI_RECV | FI_REMOTE_READ | FI_REMOTE_WRITE |     \
	 FI_REMOTE_COMPLETE | FI_CANCEL | FI_FENCE)

/*
 * see Operations flags in fi_endpoint.3
 */
#define GNIX_EP_OP_FLAGS                                                       \
	(FI_MULTI_RECV | FI_BUFFERED_RECV | FI_COMPLETION |                    \
	 FI_REMOTE_COMPLETE | FI_READ | FI_WRITE | FI_SEND | FI_RECV |         \
	 FI_REMOTE_READ | FI_REMOTE_WRITE)

/*
 * if this has to be changed, check gnix_getinfo, etc.
 */
#define GNIX_EP_MSG_CAPS GNIX_EP_RDM_CAPS

#define GNIX_MAX_MSG_SIZE ((0x1ULL << 32) - 1)
#define GNIX_INJECT_SIZE 64

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

struct gnix_fabric {
	struct fid_fabric fab_fid;
	struct list_head cdm_list;    /* llist of cdm's opened from fabric */
};

/*
 * a gnix_domain is associated with one cdm and one nic
 * since a single cdm with a given cookie/cdm_id can only
 * be bound once to a given physical aries nic
 */

struct gnix_domain {
	struct fid_domain domain_fid;
	/* used for fabric object llist of domains*/
	struct list_node list;
	/* list nics this domain is attached to, TODO: thread safety */
	struct list_head nic_list;
	/* cm nic bound to this domain */
	struct gnix_cm_nic *cm_nic;
	uint8_t ptag;
	uint32_t cookie;
	/* work queue for domain */
        struct list_head domain_wq;
        int ref_cnt;
};

struct gnix_cdm {
	struct list_node list;
	gni_cdm_handle_t gni_cdm_hndl;
	/* list nics this cdm is attached to, TODO: thread safety */
	struct list_head nic_list;
	uint32_t inst_id;
	uint8_t ptag;
	uint32_t cookie;
	uint32_t modes;
	/* TODO: thread safety */
	int ref_cnt;
};

/*
 * gnix cm nic struct - to be used only for GNI_EpPostData, etc.
 */

struct gnix_cm_nic {
	struct list_node        list;
	gni_cdm_handle_t        gni_cdm_hndl;
	gni_nic_handle_t        gni_nic_hndl;
	/* free list of datagrams   */
	struct list_head        datagram_free_list;
	/* list of active wc datagrams   */
	struct list_head        wc_datagram_active_list;
	/* free list of wc datagrams   */
	struct list_head        wc_datagram_free_list;
	/* pointer to domain this nic is attached to */
	struct gnix_domain      *domain;
	struct gnix_datagram    *datagram_base;
	uint32_t                inst_id;
	uint32_t                device_id;
	uint32_t                device_addr;
	int                     ref_cnt;
};


struct gnix_nic {
	struct list_node list;
	gni_cdm_handle_t        gni_cdm_hndl;
	gni_nic_handle_t gni_nic_hndl;
	/* receive completion queue for hndl */
	gni_cq_handle_t rx_cq;
	/* receive completion queue for hndl (blocking) */
	gni_cq_handle_t rx_cq_blk;
	/* local(tx) completion queue for hndl */
	gni_cq_handle_t tx_cq;
	/* local(tx) completion queue for hndl (blocking) */
	gni_cq_handle_t tx_cq_blk;
	/* list of wqe's */
	struct list_head wqe_active_list;
	/* list for managing wqe's */
	struct gnix_wqe_list *wqe_list;
        struct gnix_domain      *domain;                   /* pointer to domain this nic is attached to */
	struct list_head smsg_active_req_list;
	/* list for managing smsg req's */
	struct gnix_smsg_req_list *smsg_req_list;
	/* list of active datagrams */
	struct list_head datagram_active_list;
	/* free list of datagrams   */
	struct list_head datagram_free_list;
	/* list of active wc datagrams   */
	struct list_head wc_datagram_active_list;
	/* free list of wc datagrams */
	struct list_head wc_datagram_free_list;
	/* pointer to cdm this nic is attached to */
	struct gnix_cdm *cdm;
	struct gnix_datagram *datagram_base;
	uint32_t device_id;
	uint32_t device_addr;
	int ref_cnt;
};

/*
 * CQE struct definitions
 */
struct gnix_cq_entry {
	struct list_node list;
	struct fi_cq_entry the_entry;
};

struct gnix_cq_msg_entry {
	struct list_node list;
	struct fi_cq_msg_entry the_entry;
};

struct gnix_cq_tagged_entry {
	struct list_node list;
	struct fi_cq_tagged_entry the_entry;
};

struct gnix_cq {
	struct fid_cq fid;
	uint64_t flags;
	struct gnix_domain *domain;
	void *free_list_base;
	struct list_head entry;
	struct list_head err_entry;
	struct list_head entry_free_list;
	int (*progress_fn)(struct gnix_cq *);
	enum fi_cq_format format;
};

struct gnix_mem_desc {
	struct fid_mr mr_fid;
	struct gnix_domain *domain;
	gni_mem_handle_t mem_hndl;
};

/*
 *   gnix_rdm_ep - FI_EP_RDM type ep
 */
struct gnix_rdm_ep {
	struct fid_ep ep_fid;
	struct gnix_domain *domain;
	void *vc_cache_hndl;
	int (*progress_fn)(struct gnix_rdm_ep *, enum gnix_progress_type);
	/* RX specific progress fn */
	int (*rx_progress_fn)(struct gnix_rdm_ep *, gni_return_t *rc);
	int enabled;
	/* num. active post descs associated with this ep */
	uint32_t active_post_descs;
};

/*
 * globals
 */
extern const char const gnix_fab_name[];
extern const char const gnix_dom_name[];
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
 * prototypes 
 */
int gnix_domain_open(struct fid_fabric *fabric, struct fi_info *info,
                     struct fid_domain **domain, void *context);
int gnix_av_open(struct fid_domain *domain, struct fi_av_attr *attr,
		 struct fid_av **av, void *context);
int gnix_cq_open(struct fid_domain *domain, struct fi_cq_attr *attr,
		 struct fid_cq **cq, void *context);
int gnix_ep_open(struct fid_domain *domain, struct fi_info *info,
		 struct fid_ep **ep, void *context);

int gnix_mr_reg(struct fid_domain *domain, const void *buf, size_t len,
		uint64_t access, uint64_t offset, uint64_t requested_key,
		uint64_t flags, struct fid_mr **mr, void *context);



#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _GNIX_H_ */
