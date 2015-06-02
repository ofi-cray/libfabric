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
#include "gnix_nic.h"
#include "gnix_cm_nic.h"
#include "gnix_mbox_allocator.h"

static int gnix_nics_per_ptag[GNI_PTAG_USER_END];
static LIST_HEAD(gnix_nic_list);
static pthread_mutex_t gnix_nic_list_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * globals
 */

/* TODO: this will need to be adjustable - probably set in GNI_INI*/
uint32_t gnix_def_max_nics_per_ptag = 4;

/*
 * function to process GNI CQ TX CQES to progress a gnix_nic
 */

static int __nic_tx_progress(struct gnix_nic *nic)
{
	int ret = FI_SUCCESS;
	int msg_id=0;
	gni_return_t  status = GNI_RC_NOT_DONE, status_gc;
	gni_post_descriptor_t  *gni_desc=NULL;
	gni_cq_entry_t cqe;
	struct gnix_tx_descriptor *gnix_tdesc = NULL;
	unsigned int recov;

	/*
	 * first see if there are any CQE's on the
	 * tx cq. Calling GNI_CqTestEvent does not require
	 * getting the nic lock
	 */

	status = GNI_CqTestEvent(nic->tx_cq);
	if (status == GNI_RC_NOT_DONE)
		return FI_SUCCESS;

try_again:
	fastlock_acquire(&nic->lock);
        status = GNI_CqGetEvent(nic->tx_cq, &cqe);
        if (status  == GNI_RC_NOT_DONE) {
		fastlock_release(&nic->lock);
		return FI_SUCCESS;
	}

	switch (status) {
	case GNI_RC_SUCCESS:
		assert(GNI_CQ_STATUS_OK(cqe));
		/*
		 * check whether CQE from SMSG or a Post
		 * transaction
		 */
		if (GNI_CQ_GET_TYPE(cqe)
				== GNI_CQ_EVENT_TYPE_POST) {
			status_gc = GNI_GetCompleted(nic->tx_cq, cqe,
						     &gni_desc);
			if ((status_gc != GNI_RC_SUCCESS) &&
				(status_gc != GNI_RC_TRANSACTION_ERROR)) {
				GNIX_WARN(FI_LOG_EP_DATA,
					  "GNI_GetCompleted returned %s\n",
					  gni_err_str[status_gc]);
				ret = gnixu_to_fi_errno(status_gc);
				goto err1;
			}
			gnix_tdesc = container_of(gni_desc,
						struct gnix_tx_descriptor,
						desc.gni_desc);
		}  else if (GNI_CQ_GET_TYPE(cqe)
			    == GNI_CQ_EVENT_TYPE_SMSG) {
			msg_id = GNI_CQ_GET_MSG_ID(cqe);
			gnix_tdesc = gnix_desc_lkup_by_id(nic,msg_id);
			if (gnix_tdesc == NULL) {
				ret = -FI_ENOENT;
				goto err1;
			}
		} else {
			assert(0);   /* TODO: something better -unexpected event type */
		}

		fastlock_release(&nic->lock);
		if (ret == FI_SUCCESS) {
			if (gnix_tdesc->desc.completer_func) {
				ret =
				   gnix_tdesc->desc.completer_func(gnix_tdesc);
				if (ret)
					goto err;
                        }
                        ret = _gnix_nic_tx_free(nic, gnix_tdesc);
			if (ret)
				goto err;
		}
		break;

	case GNI_RC_TRANSACTION_ERROR: /* uh oh, a hiccup in the network,
					stupid user, etc. */
		/* this shouldn't happen SMSG */
		if (GNI_CQ_GET_TYPE(cqe) == GNI_CQ_EVENT_TYPE_SMSG) {
			char ebuf[512];
			GNI_CqErrorStr(cqe, ebuf, sizeof(ebuf));
			GNIX_WARN(FI_LOG_EP_DATA,
				  "CQ error statusfor GNI_SmsgSend - %s\n",
				  ebuf);
			goto err1;
		}
		status_gc = GNI_GetCompleted(nic->tx_cq, cqe, &gni_desc);
		fastlock_release(&nic->lock);
		gnix_tdesc = container_of(gni_desc,
					struct gnix_tx_descriptor,
					desc.gni_desc);
		if ((status_gc != GNI_RC_SUCCESS) &&
			(status_gc != GNI_RC_TRANSACTION_ERROR)) {
			ret = gnixu_to_fi_errno(status_gc);
			goto err;
		}
		/*
 		 * TODO: need to allow for recover of failed transactions
 		 */
		status = GNI_CqErrorRecoverable(cqe,&recov);
		if ((status == GNI_RC_SUCCESS) && !recov) {
			char ebuf[512];
			GNI_CqErrorStr(cqe, ebuf, sizeof(ebuf));
			GNIX_WARN(FI_LOG_EP_DATA,
				  "CQ error statusfor GNI_Post - %s\n",
				  ebuf);
			goto err;
		}

		break;

	default:
		assert(0);  /* TODO: better error later */
	}

	/*
	 * keep on dequeuing until we get GNI_RC_NOT_DONE
	 */

	goto try_again;
err1:
	fastlock_release(&nic->lock);
err:
	return ret;

}

/*
 * process an RX CQEs for this nic
 */
static int __nic_rx_progress(struct gnix_nic *nic)
{
	return -FI_ENOSYS;
}

int _gnix_nic_progress(struct gnix_nic *nic)
{
	int ret = FI_SUCCESS;
	int complete;
	struct gnix_work_req *p = NULL;

	ret =  __nic_tx_progress(nic);
	if (ret != FI_SUCCESS)
		return ret;

	ret = __nic_rx_progress(nic);
	if (ret != FI_SUCCESS)
		return ret;

	/*
	 * see what's going in the work queue
	 */

check_again:
	if (list_empty(&nic->nic_wq))
		return ret;

	fastlock_acquire(&nic->wq_lock);
	p = list_top(&nic->nic_wq, struct gnix_work_req, list);
	if (p == NULL) {
		fastlock_release(&nic->wq_lock);
		return ret;
	}

	gnix_list_del_init(&p->list);
	fastlock_release(&nic->wq_lock);

	assert(p->progress_func);

	ret = p->progress_func(p->data, &complete);
	if (ret != FI_SUCCESS) {
		GNIX_WARN(FI_LOG_EP_DATA,
			  "progress_func returned with erro - %d\n",
			  ret );
		goto err;
	}

	if (complete == 1) {
		if (p->completer_func) {
			ret = p->completer_func(p->completer_data);
			free(p);
			if (ret != FI_SUCCESS)
				goto err;
		}
		goto check_again;  /* we're getting stuff done, try again */
	} else {
		fastlock_acquire(&nic->wq_lock);
		list_add_tail(&nic->nic_wq, &p->list);
		fastlock_release(&nic->wq_lock);
	}

err:
	return ret;
}

/*
 * allocate a tx desc for this nic
 */

int _gnix_nic_tx_alloc(struct gnix_nic *nic,
		       struct gnix_tx_descriptor **desc)
{
	return -FI_ENOSYS;
}

/*
 * free a tx desc for this nic - the nic is not embedded in the
 * descriptor to help keep it small
 */

int _gnix_nic_tx_free(struct gnix_nic *nic,
			struct gnix_tx_descriptor *desc)
{
	return -FI_ENOSYS;
}

/*
 * allocate a free list of tx descs for a gnix_nic struct.
 */

static int __gnix_nic_tx_freelist_init(struct gnix_nic *nic, int n_descs)
{
	int i, ret = FI_SUCCESS;
	struct gnix_tx_descriptor *desc_base, *desc_ptr;

	GNIX_TRACE(FI_LOG_EP_CTRL, "\n");

	/*
	 * set up free list of tx descriptors.
	 */

	desc_base = calloc(n_descs, sizeof(struct gnix_tx_descriptor));
	if (desc_base == NULL) {
		ret = -FI_ENOMEM;
		goto err;
	}

	list_head_init(&nic->tx_desc_free_list);
	list_head_init(&nic->tx_desc_active_list);

	for (i = 0, desc_ptr = desc_base; i < n_descs; i++) {
		desc_ptr->desc.id = i;
		gnix_list_node_init(&desc_ptr->desc.list);
		list_add_tail(&nic->tx_desc_free_list,
				&desc_ptr->desc.list);
	}

	nic->max_tx_desc_id = n_descs - 1;
	nic->tx_desc_base = desc_base;

err:
	return ret;

}

/*
 * clean up the tx descs free list
 */
static void __gnix_nic_tx_freelist_destroy(struct gnix_nic *nic)
{
	free(nic->tx_desc_base);
}

/*
 * free a gnix nic and associated resources if refcnt drops to 0
 */

int _gnix_nic_free(struct gnix_nic *nic)
{
	int ret = FI_SUCCESS, v;
	gni_return_t status = GNI_RC_SUCCESS;

	GNIX_TRACE(FI_LOG_EP_CTRL, "\n");

	if (nic == NULL)
		return -FI_EINVAL;

	v = atomic_dec(&nic->ref_cnt);
	assert(v >= 0);

	if ((nic->gni_cdm_hndl != NULL) && (v == 0))  {
		if (nic->rx_cq_blk != NULL)
			status = GNI_CqDestroy(nic->rx_cq_blk);
			if (status != GNI_RC_SUCCESS) {
				GNIX_WARN(FI_LOG_EP_CTRL,
					  "GNI_CqDestroy returned %s\n",
					 gni_err_str[status]);
				ret = gnixu_to_fi_errno(status);
				goto err;
			}
		if (nic->rx_cq != NULL)
			status = GNI_CqDestroy(nic->rx_cq);
			if (status != GNI_RC_SUCCESS) {
				GNIX_WARN(FI_LOG_EP_CTRL,
					  "GNI_CqDestroy returned %s\n",
					 gni_err_str[status]);
				ret = gnixu_to_fi_errno(status);
				goto err;
			}
		if (nic->tx_cq_blk != NULL)
			status = GNI_CqDestroy(nic->tx_cq_blk);
			if (status != GNI_RC_SUCCESS) {
				GNIX_WARN(FI_LOG_EP_CTRL,
					  "GNI_CqDestroy returned %s\n",
					 gni_err_str[status]);
				ret = gnixu_to_fi_errno(status);
				goto err;
			}
		if (nic->tx_cq != NULL)
			status = GNI_CqDestroy(nic->tx_cq);
			if (status != GNI_RC_SUCCESS) {
				GNIX_WARN(FI_LOG_EP_CTRL,
					  "GNI_CqDestroy returned %s\n",
					 gni_err_str[status]);
				ret = gnixu_to_fi_errno(status);
				goto err;
			}
		status = GNI_CdmDestroy(nic->gni_cdm_hndl);
		if (status != GNI_RC_SUCCESS) {
			GNIX_WARN(FI_LOG_EP_CTRL,
				  "GNI_CdmDestroy returned %s\n",
				  gni_err_str[status]);
			ret = gnixu_to_fi_errno(status);
			goto err;
		}

		ret = _gnix_mbox_allocator_destroy(nic->mbox_hndl);
		if (ret != FI_SUCCESS)
			GNIX_WARN(FI_LOG_EP_CTRL,
				  "_gnix_mbox_allocator_destroy returned %d\n",
				  ret);

		/*
		 * remove the nic from the linked lists
		 * for the domain and the global nic list
		 */

err:
		pthread_mutex_lock(&gnix_nic_list_lock);

		gnix_list_del_init(&nic->gnix_nic_list);
		--gnix_nics_per_ptag[nic->ptag];
		gnix_list_del_init(&nic->list);

		pthread_mutex_unlock(&gnix_nic_list_lock);

		__gnix_nic_tx_freelist_destroy(nic);
		free(nic);
	}

	return ret;
}

/*
 * allocate a gnix_nic struct using attributes of the domain
 */

int gnix_nic_alloc(struct gnix_fid_domain *domain,
				struct gnix_nic **nic_ptr)
{
	int ret = FI_SUCCESS;
	struct gnix_nic *nic = NULL, *elem;
	uint32_t device_addr;
	gni_return_t status;
	uint32_t fake_cdm_id;
	gni_smsg_attr_t smsg_mbox_attr;

	GNIX_TRACE(FI_LOG_EP_CTRL, "\n");

	*nic_ptr = NULL;

	/*
	 * If we've maxed out the number of nics for this domain/ptag,
	 * search the list of existing nics.  Take the gnix_nic_list_lock
	 * here since the gnix_nic_list will be manipulated whether or
	 * not we attach to an existing nic or create a new one.
	 *
	 * Should not matter much that this is a pretty fat critical section
	 * since endpoint setup for RDM type will typically occur near
	 * app startup, likely in a single threaded region, and for the
	 * case of MSG, where there will likely be many 100s of EPs, after
	 * a few initial slow times through this section when nics are created,
	 * max nic count for the ptag will be reached and only the first part
	 * of the critical section - iteration over existing nics - will be
	 * happening.
	 */

	pthread_mutex_lock(&gnix_nic_list_lock);

	if (gnix_nics_per_ptag[domain->ptag] ==
					gnix_def_max_nics_per_ptag) {
		list_for_each(&gnix_nic_list, elem, list) {
			if ((elem->ptag == domain->ptag) &&
				(elem->cookie == domain->cookie)) {
				nic = elem;
				break;
			}
		}

		/*
		 * nic found, balance use by removing from head and
		 * positioning at end
		 */

		if (nic) {
			gnix_list_del_init(&nic->gnix_nic_list);
			list_add_tail(&gnix_nic_list, &nic->gnix_nic_list);
		}
	}

	/*
	 * no nic found create a cdm and attach
	 */

	if (!nic) {

		nic = calloc(1, sizeof(struct gnix_nic));
		if (nic == NULL) {
			ret = -FI_ENOMEM;
			goto err;
		}

		ret = _gnix_get_new_cdm_id(domain, &fake_cdm_id);
		if (ret != FI_SUCCESS)
			goto err;

		status = GNI_CdmCreate(fake_cdm_id,
					domain->ptag,
					domain->cookie,
					gnix_cdm_modes,
					&nic->gni_cdm_hndl);
		if (status != GNI_RC_SUCCESS) {
			GNIX_WARN(FI_LOG_EP_CTRL, "GNI_CdmCreate returned %s\n",
				 gni_err_str[status]);
			ret = gnixu_to_fi_errno(status);
			goto err1;
		}

		/*
		 * Okay, now go for the attach
		*/
		status = GNI_CdmAttach(nic->gni_cdm_hndl,
					0,
					&device_addr,
					&nic->gni_nic_hndl);
		if (status != GNI_RC_SUCCESS) {
			GNIX_WARN(FI_LOG_EP_CTRL, "GNI_CdmAttach returned %s\n",
				 gni_err_str[status]);
			ret = gnixu_to_fi_errno(status);
			goto err1;
		}

		/*
		 * create TX CQs - first polling, then blocking
		 */

		status = GNI_CqCreate(nic->gni_nic_hndl,
					domain->gni_tx_cq_size,
					0,                  /* no delay count */
					GNI_CQ_NOBLOCK |
					domain->gni_cq_modes,
					NULL,              /* useless handler */
					NULL,               /* useless handler
								context */
					&nic->tx_cq);
		if (status != GNI_RC_SUCCESS) {
			GNIX_WARN(FI_LOG_EP_CTRL,
				  "GNI_CqCreate returned %s\n",
				  gni_err_str[status]);
			ret = gnixu_to_fi_errno(status);
			goto err1;
		}

		status = GNI_CqCreate(nic->gni_nic_hndl,
					domain->gni_tx_cq_size,
					0,
					GNI_CQ_BLOCKING |
						domain->gni_cq_modes,
					NULL,
					NULL,
					&nic->tx_cq_blk);
		if (status != GNI_RC_SUCCESS) {
			GNIX_WARN(FI_LOG_EP_CTRL,
				  "GNI_CqCreate returned %s\n",
				  gni_err_str[status]);
			ret = gnixu_to_fi_errno(status);
			goto err1;
		}

		/*
		 * create RX CQs - first polling, then blocking
		 */

		status = GNI_CqCreate(nic->gni_nic_hndl,
					domain->gni_rx_cq_size,
					0,
					GNI_CQ_NOBLOCK |
						domain->gni_cq_modes,
					NULL,
					NULL,
					&nic->rx_cq);
		if (status != GNI_RC_SUCCESS) {
			GNIX_WARN(FI_LOG_EP_CTRL,
				  "GNI_CqCreate returned %s\n",
				  gni_err_str[status]);
			ret = gnixu_to_fi_errno(status);
			goto err1;
		}

		status = GNI_CqCreate(nic->gni_nic_hndl,
					domain->gni_rx_cq_size,
					0,
					GNI_CQ_BLOCKING |
					domain->gni_cq_modes,
					NULL,
					NULL,
					&nic->rx_cq_blk);
		if (status != GNI_RC_SUCCESS) {
			GNIX_WARN(FI_LOG_EP_CTRL,
				  "GNI_CqCreate returned %s\n",
				  gni_err_str[status]);
			ret = gnixu_to_fi_errno(status);
			goto err1;
		}

		nic->device_addr = device_addr;
		nic->ptag = domain->ptag;
		nic->cookie = domain->cookie;
		fastlock_init(&nic->lock);

		ret = __gnix_nic_tx_freelist_init(nic, domain->gni_tx_cq_size);
		if (ret != FI_SUCCESS)
			goto err1;

		fastlock_init(&nic->wq_lock);
		list_head_init(&nic->nic_wq);

		atomic_initialize(&nic->ref_cnt, 1);
		atomic_initialize(&nic->outstanding_fab_reqs_nic, 0);
		ret = _gnix_alloc_bitmap(&nic->vc_id_bitmap, 1000);
		if (ret != FI_SUCCESS) {
			GNIX_WARN(FI_LOG_EP_CTRL,
				  "alloc_bitmap returned %d\n", ret);
			goto err1;
		}
		fastlock_init(&nic->vc_id_lock);

		/*
		 * TODOs: need a way to specify mboxes/slab via
		 * some domain param.
		 */

		smsg_mbox_attr.msg_type = GNI_SMSG_TYPE_MBOX_AUTO_RETRANSMIT;
		smsg_mbox_attr.mbox_maxcredit = 64; /* TODO: fix this */
		smsg_mbox_attr.msg_maxsize =  16384;  /* TODO: definite fix */

		status = GNI_SmsgBufferSizeNeeded(&smsg_mbox_attr,
						  &nic->mem_per_mbox);
		if (status != GNI_RC_SUCCESS) {
			GNIX_WARN(FI_LOG_EP_CTRL,
				  "GNI_SmsgBufferSizeNeeded returned %s\n",
				  gni_err_str[status]);
			ret = gnixu_to_fi_errno(status);
			goto err1;
		}

		ret = _gnix_mbox_allocator_create(nic,
						  nic->rx_cq,
						  GNIX_PAGE_2MB,
						  (size_t)nic->mem_per_mbox,
						  2048,
						  &nic->mbox_hndl);
		if (ret != FI_SUCCESS) {
			GNIX_WARN(FI_LOG_EP_CTRL,
				  "_gnix_mbox_alloc returned %d\n", ret);
			goto err1;
		}


		list_add_tail(&gnix_nic_list, &nic->gnix_nic_list);
		++gnix_nics_per_ptag[domain->ptag];

		list_add_tail(&domain->nic_list, &nic->list);
	}

	*nic_ptr = nic;
	goto out;

err1:
	atomic_dec(&gnix_id_counter);
err:
	if (nic != NULL) {
		if (nic->rx_cq_blk != NULL)
			GNI_CqDestroy(nic->rx_cq_blk);
		if (nic->rx_cq != NULL)
			GNI_CqDestroy(nic->rx_cq);
		if (nic->tx_cq_blk != NULL)
			GNI_CqDestroy(nic->tx_cq_blk);
		if (nic->tx_cq != NULL)
			GNI_CqDestroy(nic->tx_cq);
		if (nic->gni_cdm_hndl != NULL)
			GNI_CdmDestroy(nic->gni_cdm_hndl);
		free(nic);
	}

out:
	pthread_mutex_unlock(&gnix_nic_list_lock);
	return ret;
}
