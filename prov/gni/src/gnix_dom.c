/*
 * Copyright (c) 2015 Los Alamos National Security, LLC. Allrights reserved.
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

#include "gnix.h"
#include "gnix_util.h"

static int gnix_domain_close(fid_t fid)
{
	int ret = FI_SUCCESS;
	struct gnix_domain *domain;
	struct gnix_cm_nic *cm_nic;
	struct gnix_nic *p, *next;
	gni_return_t status;

	domain = container_of(fid, struct gnix_domain, domain_fid.fid);
	if (domain->ref_cnt) {
		--domain->ref_cnt;
	}

	GNIX_LOG_INFO("gnix_domain_close invoked.\n");

	if (domain->ref_cnt == 0) {
		if (domain->cm_nic) {
			cm_nic = domain->cm_nic;
			if (cm_nic->gni_cdm_hndl) {
				status = GNI_CdmDestroy(cm_nic->gni_cdm_hndl);
				if (status != GNI_RC_SUCCESS) {
					GNIX_LOG_ERROR("oops, cdm destroy"
						       "failed\n");
				}
			}
			free(domain->cm_nic);
		}

		list_for_each_safe(&domain->nic_list, p, next, list)
		{
			list_del(&p->list);
			gnix_list_node_init(&p->list);
			/* TODO: free nic here */
		}

		/*
		 * remove from the list of cdms attached to fabric
		 */
		gnix_list_del_init(&domain->list);

		memset(domain, 0, sizeof *domain);
		free(domain);
	}

	GNIX_LOG_INFO("gnix_domain_close invoked returning %d\n", ret);
	return ret;
}

static struct fi_ops gnix_fi_ops = {
	.size = sizeof(struct fi_ops),
	.close = gnix_domain_close,
	.bind = fi_no_bind,
	.control = fi_no_control
};

static struct fi_ops_domain gnix_domain_ops = {
	.size = sizeof(struct fi_ops_domain),
	.av_open = gnix_av_open,
	.cq_open = gnix_cq_open,
	.endpoint = gnix_ep_open,
	/* TODO: no cntrs for now in gnix */
	.cntr_open = fi_no_cntr_open,
	.poll_open = fi_no_poll_open,
	.stx_ctx = fi_no_stx_context,
	.srx_ctx = fi_no_srx_context
};

static struct fi_ops_mr gnix_domain_mr_ops = {
	.size = sizeof(struct fi_ops_mr),
	.reg = gnix_mr_reg
};

int gnix_domain_open(struct fid_fabric *fabric, struct fi_info *info,
		     struct fid_domain **dom, void *context)
{
	struct gnix_domain *domain = NULL;
	int ret = FI_SUCCESS;
	uint8_t ptag;
	uint32_t cookie, device_addr;
	struct gnix_cm_nic *cm_nic = NULL;
	struct gnix_fabric *fabric_priv;
	gni_return_t status;

	GNIX_LOG_INFO("%s\n", __func__);

	fabric_priv = container_of(fabric, struct gnix_fabric, fab_fid);
	if (!info->domain_attr->name ||
	    strncmp(info->domain_attr->name, gnix_dom_name,
		    strlen(gnix_dom_name))) {
		ret = -FI_EINVAL;
		goto fn_err;
	}

	/*
	 * check cookie/ptag credentials - for FI_EP_MSG we may be creating a
	 * domain
	 * using a cookie supplied being used by the server.  Otherwise, we use
	 * use the cookie/ptag supplied by the job launch system.
	 */
	if (info->dest_addr) {
		ret =
		    gnixu_get_rdma_credentials(info->dest_addr, &ptag, &cookie);
		if (ret) {
			GNIX_LOG_ERROR("gnixu_get_rdma_credentials returned"
				       "ptag %d cookie 0x%x\n", ptag, cookie);
			goto fn_err;
		}
	} else {
		ret = gnixu_get_rdma_credentials(NULL, &ptag, &cookie);
	}

	GNIX_LOG_INFO("gnix rdma credentials returned ptag %d cookie 0x%x\n",
		      ptag, cookie);
	domain = calloc(1, sizeof *domain);
	if (domain == NULL) {
		ret = -FI_ENOMEM;
		goto fn_err;
	}

	list_head_init(&domain->nic_list);
	gnix_list_node_init(&domain->list);

	list_add_tail(&fabric_priv->cdm_list, &domain->list);

	list_head_init(&domain->domain_wq);

	/*
	 * Set up the connection management nic for this domain
	 */
	cm_nic = (struct gnix_cm_nic *)calloc(1, sizeof *cm_nic);
	if (cm_nic == NULL) {
		ret = -FI_ENOMEM;
		goto fn_err;
	}

	gnix_list_node_init(&cm_nic->list);
	list_head_init(&cm_nic->datagram_free_list);
	list_head_init(&cm_nic->wc_datagram_active_list);
	list_head_init(&cm_nic->wc_datagram_free_list);

	status = GNI_CdmCreate(getpid(), ptag, cookie, gnix_cdm_modes,
			       &cm_nic->gni_cdm_hndl);
	if (status != GNI_RC_SUCCESS) {
		GNIX_LOG_ERROR("GNI_CdmCreate returned %s\n",
			       gni_err_str[status]);
		/* TODO: need a translater from gni to fi errors */
		ret = -FI_EACCES;
		goto fn_err;
	}

	/*
	 * Okay, now go for the attach
	 */
	status = GNI_CdmAttach(cm_nic->gni_cdm_hndl, 0, &device_addr,
			       &cm_nic->gni_nic_hndl);
	if (status != GNI_RC_SUCCESS) {
		GNIX_LOG_ERROR("GNI_CdmAttach returned %s\n",
			       gni_err_str[status]);
		ret = -FI_EACCES;
		goto fn_err;
	}

	domain->cm_nic = cm_nic;
	domain->ptag = ptag;
	domain->cookie = cookie;
	domain->ref_cnt = 1;
	cm_nic->domain = domain;

	domain->domain_fid.fid.fclass = FI_CLASS_DOMAIN;
	domain->domain_fid.fid.context = context;
	domain->domain_fid.fid.ops = &gnix_fi_ops;
	domain->domain_fid.ops = &gnix_domain_ops;
	domain->domain_fid.mr = &gnix_domain_mr_ops;

	*dom = &domain->domain_fid;
	return FI_SUCCESS;

fn_err:
	if (cm_nic && cm_nic->gni_cdm_hndl) {
		GNI_CdmDestroy(cm_nic->gni_cdm_hndl);
	}
	if (domain != NULL) {
		free(domain);
	}
	if (cm_nic != NULL) {
		free(cm_nic);
	}
	return ret;
}
