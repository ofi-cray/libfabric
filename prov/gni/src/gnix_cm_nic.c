/*
 * Copyright (c) 2015 Los Alamos National Security, LLC. All rights reserved.
 * Copyright (c) 2015 Cray Inc. All rights reserved.
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

#if HAVE_CONFIG_H
#include <config.h>
#endif /* HAVE_CONFIG_H */

#include <stdlib.h>
#include <assert.h>

#include "gnix.h"
#include "gnix_datagram.h"
#include "gnix_cm_nic.h"

/*
 * generate a cdm_id, use the 16 LSB of base_id from domain
 * with 16 MSBs being obtained from atomic increment of
 * a local variable.
 */

int _gnix_get_new_cdm_id(struct gnix_fid_domain *domain, uint32_t *id)
{
	uint32_t cdm_id;
	int v;

	v = atomic_inc(&gnix_id_counter);
	cdm_id = (domain->cdm_id_seed & 0x0000FFFF) |
			((uint32_t)v << 16);
	*id = cdm_id;
	return FI_SUCCESS;
}

int _gnix_cm_nic_progress(struct gnix_cm_nic *cm_nic)
{
	int ret = FI_SUCCESS;
	int complete;
	struct gnix_work_req *p = NULL;

	/*
	 * if we're doing FI_PROGRESS_MANUAL,
	 * see what's going on inside kgni's datagram
	 * box...
	 */

	if (cm_nic->control_progress == FI_PROGRESS_MANUAL) {
		ret = _gnix_dgram_poll(cm_nic->dgram_hndl,
					  GNIX_DGRAM_NOBLOCK);
		if (ret != FI_SUCCESS)
			goto err;
	}

	/*
	 * do a quick check if queue doesn't have anything yet,
	 * don't need this to be atomic
	 */

check_again:
	if (list_empty(&cm_nic->cm_nic_wq))
		return ret;

	/*
	 * okay, stuff to do, lock work queue,
	 * dequeue head, unlock, process work element,
	 * if it doesn't compete, put back at the tail
	 * of the queue.
	 */

	fastlock_acquire(&cm_nic->wq_lock);
	p = list_top(&cm_nic->cm_nic_wq, struct gnix_work_req, list);
	if (p == NULL) {
		fastlock_release(&cm_nic->wq_lock);
		return ret;
	}

	gnix_list_del_init(&p->list);
	fastlock_release(&cm_nic->wq_lock);

	assert(p->progress_fn);

	ret = p->progress_fn(p->data, &complete);
	if (ret != FI_SUCCESS) {
		/*
		 * TODO: fix this
		 */
	}

	if (complete == 1) {
		if (p->completer_fn) {
			ret = p->completer_fn(p->completer_data);
			free(p);
			if (ret != FI_SUCCESS)
				goto err;
		}
		goto check_again;
	} else {
		fastlock_acquire(&cm_nic->wq_lock);
		list_add_tail(&cm_nic->cm_nic_wq, &p->list);
		fastlock_release(&cm_nic->wq_lock);
	}

err:
	return ret;
}

int _gnix_cm_nic_free(struct gnix_cm_nic *cm_nic)
{
	int ret = FI_SUCCESS;
	gni_return_t status;

	GNIX_TRACE(FI_LOG_EP_CTRL, "\n");

	if (cm_nic == NULL)
		return -FI_EINVAL;

	if (cm_nic->dgram_hndl != NULL) {
		ret = _gnix_dgram_hndl_free(cm_nic->dgram_hndl);
		if (ret != FI_SUCCESS)
			GNIX_WARN(FI_LOG_EP_CTRL,
				  "gnix_dgram_hndl_free returned %d\n",
				  ret);
	}

	if (cm_nic->gni_cdm_hndl != NULL) {
		status = GNI_CdmDestroy(cm_nic->gni_cdm_hndl);
		if (status != GNI_RC_SUCCESS) {
			GNIX_WARN(FI_LOG_EP_CTRL,
				  "cdm destroy failed - %s\n",
				  gni_err_str[status]);
			ret = gnixu_to_fi_errno(status);
		}
	}

	free(cm_nic);
	return ret;
}

int _gnix_cm_nic_alloc(struct gnix_fid_domain *domain,
		       uint32_t cdm_id_arg,
		       struct gnix_cm_nic **cm_nic_ptr)
{
	int ret = FI_SUCCESS;
	struct gnix_cm_nic *cm_nic = NULL;
	uint32_t device_addr, cdm_id;
	gni_return_t status;

	GNIX_TRACE(FI_LOG_EP_CTRL, "\n");

	*cm_nic_ptr = NULL;

	cm_nic = (struct gnix_cm_nic *)calloc(1, sizeof(*cm_nic));
	if (cm_nic == NULL) {
		ret = -FI_ENOMEM;
		goto err;
	}

	if (cdm_id_arg == -1U) {
		ret = _gnix_get_new_cdm_id(domain, &cdm_id);
		if (ret != FI_SUCCESS)
			goto err;
	} else
		cdm_id = cdm_id_arg;

	GNIX_INFO(FI_LOG_EP_CTRL, "creating cm_nic for %u/0x%x/%u\n",
		      domain->ptag, domain->cookie, cdm_id);

	status = GNI_CdmCreate(cdm_id,
			       domain->ptag,
			       domain->cookie,
			       gnix_cdm_modes,
			       &cm_nic->gni_cdm_hndl);
	if (status != GNI_RC_SUCCESS) {
		GNIX_ERR(FI_LOG_EP_CTRL, "GNI_CdmCreate returned %s\n",
			       gni_err_str[status]);
		ret = gnixu_to_fi_errno(status);
		goto err;
	}

	/*
	 * Okay, now go for the attach
	 */
	status = GNI_CdmAttach(cm_nic->gni_cdm_hndl, 0, &device_addr,
			       &cm_nic->gni_nic_hndl);
	if (status != GNI_RC_SUCCESS) {
		GNIX_ERR(FI_LOG_EP_CTRL, "GNI_CdmAttach returned %s\n",
		       gni_err_str[status]);
		ret = gnixu_to_fi_errno(status);
		goto err;
	}

	cm_nic->cdm_id = cdm_id;
	cm_nic->ptag = domain->ptag;
	cm_nic->cookie = domain->cookie;
	cm_nic->device_addr = device_addr;
	cm_nic->control_progress = domain->control_progress;
	fastlock_init(&cm_nic->lock);
	fastlock_init(&cm_nic->wq_lock);
	list_head_init(&cm_nic->cm_nic_wq);

	/*
	 * prep the cm nic's dgram component
	 */
	ret = _gnix_dgram_hndl_alloc(domain->fabric,
				     cm_nic,
				     domain->control_progress,
				     &cm_nic->dgram_hndl);
	if (ret != FI_SUCCESS)
		goto err;

	*cm_nic_ptr = cm_nic;
	return ret;

err:
	if (cm_nic->dgram_hndl)
		_gnix_dgram_hndl_free(cm_nic->dgram_hndl);

	if (cm_nic->gni_cdm_hndl)
		GNI_CdmDestroy(cm_nic->gni_cdm_hndl);

	if (cm_nic != NULL)
		free(cm_nic);

	return ret;
}
