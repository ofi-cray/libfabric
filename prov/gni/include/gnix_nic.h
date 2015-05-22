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

#ifndef _GNIX_NIC_H_
#define _GNIX_NIC_H_

#ifdef __cplusplus
extern "C" {
#endif

#if HAVE_CONFIG_H
#include <config.h>
#endif /* HAVE_CONFIG_H */

#include "gnix.h"
#include "gnix_bitmap.h"
#include "gnix_mbox_allocator.h"
#include <assert.h>

/*
 * gnix nic struct - to be used for GNI_PostRdma/PostFma,
 *                   GNI_SmsgSend, GNI_CqGetEvent, etc.
 *
 * list: list entry
 * gnix_nic_list: list entry for domain's nic llist
 * lock: lock to be used when making GNI calls using this nic or derived eps.
 * gni_cdm_hndl: GNI cdm hndl for this nic
 * gni_nic_hndl: GNI nic hndl for this nic
 * rx_cq: non-blocking GNI RX CQ for this nic
 * rx_cq_blk: blocking GNI RX CQ for this nic (async progress)
 * tx_cq: non-blocking GNI TX CQ for this nic
 * tx_cq_blk: blocking GNI TX CQ for this nic (async progress)
 * tx_desc_active_list: active GNI TX desc list for this nic
 * tx_desc_free_list: free GNI TX desc list for this nic
 * tx_desc_base: pointer to memory allocation used for GNI TX descs
 * outstanding_fab_reqs_nic: number of outstanding fab_reqs associated
 *                           with this nic including rma ops, etc.
 * nic_wq: nic work queue
 * buf: event
 * ptag: ptag for this nic's GNI cdm_hndl
 * cookie: cookie for this nic's GNI cdm_hndl
 * dvice_id: device id for this nic's GNI nic_hndl (always 0 since 1 aries/node)
 * dvice_addr: L2-level address of the aries the GNI nic_hndl is bound to
 * ref_cnt: ref count for this nic
 */

struct gnix_nic {
	struct list_node list;
	struct list_node gnix_nic_list;
	fastlock_t lock;
	gni_cdm_handle_t gni_cdm_hndl;
	gni_nic_handle_t gni_nic_hndl;
	gni_cq_handle_t rx_cq;
	gni_cq_handle_t rx_cq_blk;
	gni_cq_handle_t tx_cq;
	gni_cq_handle_t tx_cq_blk;
	struct list_head tx_desc_active_list;
	struct list_head tx_desc_free_list;
	struct gnix_tx_descriptor *tx_desc_base;
	atomic_t outstanding_fab_reqs_nic;
	fastlock_t wq_lock;
	struct list_head nic_wq;
	uint8_t ptag;
	uint32_t cookie;
	uint32_t device_id;
	uint32_t device_addr;
	int max_tx_desc_id;
	fastlock_t vc_id_lock;
	struct gnix_vc **vc_id_table;
	int vc_id_table_capacity;
	int vc_id_table_count;
	gnix_bitmap_t vc_id_bitmap;
	uint32_t mem_per_mbox;
	struct gnix_mbox_alloc_handle *mbox_hndl;
	atomic_t ref_cnt;
};


struct gnix_smsg_hdr {
	size_t len;
	uint64_t imm;
};

struct gnix_smsg_descriptor {
	struct gnix_smsg_hdr hdr;
	void    *buf;         /* may point to inject buffer */
	uint8_t  tag;
};

/*
 * what's going on here is we're making sure that
 * gni_tx descriptor ends up being cacheline aligned
 */

union gnix_tx_descriptor0 {
	struct {
		struct list_node          list;
		gni_post_descriptor_t       gni_desc;
		struct gnix_smsg_descriptor gnix_smsg_desc;
		struct gnix_fab_req *req;
		int  (*completer_func)(void *);
		int id;
	};
	char padding[GNIX_CACHELINE_SIZE];
} __attribute__ ((aligned (GNIX_CACHELINE_SIZE)));

struct gnix_tx_descriptor {
	union gnix_tx_descriptor0 desc;
	char inject_buf[GNIX_CACHELINE_SIZE];
} __attribute__ ((aligned (GNIX_CACHELINE_SIZE)));


/*
 * globals
 */

extern uint32_t gnix_def_max_nics_per_ptag;

/*
 * prototypes
 */

int _gnix_nic_tx_freelist_init(struct gnix_nic *nic, int n_descs);
int _gnix_nic_tx_alloc(struct gnix_nic *nic, struct gnix_tx_descriptor **tdesc);
int _gnix_nic_tx_free(struct gnix_nic *nic, struct gnix_tx_descriptor *tdesc);
int _gnix_nic_free(struct gnix_nic *nic);
int gnix_nic_alloc(struct gnix_fid_domain *domain,
			struct gnix_nic **nic_ptr);
int _gnix_nic_progress(struct gnix_nic *nic);

/*
 * inline functions
 */

static inline struct gnix_tx_descriptor *
		gnix_desc_lkup_by_id(struct gnix_nic *nic,
				     int desc_id)
{
	struct gnix_tx_descriptor *tx_desc;

	assert((desc_id >= 0) && (desc_id < nic->max_tx_desc_id));
	tx_desc = &nic->tx_desc_base[desc_id];
	return tx_desc;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _GNIX_NIC_H_ */
