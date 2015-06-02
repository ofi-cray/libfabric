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
#include "gnix_nic.h"
#include "gnix_util.h"

uint32_t gnix_def_gni_tx_cq_size = 2048;
/* rx cq bigger to avoid having to deal with rx overruns so much */
uint32_t gnix_def_gni_rx_cq_size = 16384;
/* TODO: should we use physical pages for gni cq rings? This is a question for
 * Zach */
gni_cq_mode_t gnix_def_gni_cq_modes = GNI_CQ_PHYS_PAGES;

/*******************************************************************************
 * Forward declaration for ops structures.
 ******************************************************************************/

static struct fi_ops gnix_domain_fi_ops;
static struct fi_ops_mr gnix_domain_mr_ops;
static struct fi_ops_domain gnix_domain_ops;

/*******************************************************************************
 * API function implementations.
 ******************************************************************************/
static int gnix_domain_close(fid_t fid)
{
	int ret = FI_SUCCESS, v;
	struct gnix_fid_domain *domain;
	struct gnix_nic *p, *next;
	gni_return_t status;

	GNIX_TRACE(FI_LOG_DOMAIN, "\n");

	domain = container_of(fid, struct gnix_fid_domain, domain_fid.fid);
	if (domain->domain_fid.fid.fclass != FI_CLASS_DOMAIN) {
		ret = -FI_EINVAL;
		goto err;
	}

	/* before checking the refcnt, flush the memory registration cache */
	ret = _gnix_mr_cache_flush(&domain->mr_cache);
	if (ret != FI_SUCCESS) {
		GNIX_WARN(FI_LOG_DOMAIN,
			  "failed to flush memory cache on domain close\n");
		goto err;
	}

	/*
	 * if non-zero refcnt, there are eps, mrs, and/or an eq associated
	 * with this domain which have not been closed.
	 */

	if (atomic_get(&domain->ref_cnt) != 0) {
		GNIX_WARN(FI_LOG_DOMAIN, "non zero refcnt %d\n",
			  atomic_get(&domain->ref_cnt));
		ret = -FI_EBUSY;
		goto err;
	}

	ret = _gnix_mr_cache_destroy(&domain->mr_cache);
	if (ret != FI_SUCCESS) {
		GNIX_WARN(FI_LOG_DOMAIN,
			  "failed to destroy memory cache on domain close\n");
		goto err;
	}

	/*
	 *  remove nics from the domain's nic list,
	 *  decrement ref_cnt on each nic.  If ref_cnt
	 *  drops to 0, destroy the cdm, remove from
	 *  the global nic list.
	 */
	list_for_each_safe(&domain->nic_list, p, next, list)
	{
		list_del(&p->list);
		gnix_list_node_init(&p->list);
		v = atomic_dec(&p->ref_cnt);
		assert(v >= 0);
		if (v == 0) {
			list_del(&p->gnix_nic_list);
			gnix_list_node_init(&p->gnix_nic_list);
			status = GNI_CdmDestroy(p->gni_cdm_hndl);
			if (status != GNI_RC_SUCCESS)
				GNIX_ERR(FI_LOG_DOMAIN,
					 "oops, cdm destroy failed\n");
			free(p);
		}
	}

	v = atomic_dec(&domain->fabric->ref_cnt);
	assert(v >= 0);

	/*
	 * remove from the list of cdms attached to fabric
	 */
	gnix_list_del_init(&domain->list);

	memset(domain, 0, sizeof *domain);
	free(domain);

	GNIX_INFO(FI_LOG_DOMAIN, "gnix_domain_close invoked returning %d\n",
		  ret);
err:
	return ret;
}

/*
 * gnix_domain_ops will provide means for an application to
 * better control allocation of underlying aries resources associated
 * with the domain.  Examples will include controlling size of underlying
 * hardware CQ sizes, max size of RX ring buffers, etc.
 *
 * Currently this function is not implemented, so just return -FI_ENOSYS
 */

static int
gnix_domain_ops_open(struct fid *fid, const char *ops_name, uint64_t flags,
			void **ops, void *context)
{
	int ret = -FI_ENOSYS;
	struct gnix_fid_domain *domain;

	GNIX_TRACE(FI_LOG_DOMAIN, "\n");

	domain = container_of(fid, struct gnix_fid_domain, domain_fid.fid);
	if (domain->domain_fid.fid.fclass != FI_CLASS_DOMAIN) {
		ret = -FI_EINVAL;
		goto err;
	}

err:
	return ret;
}

int gnix_domain_open(struct fid_fabric *fabric, struct fi_info *info,
		     struct fid_domain **dom, void *context)
{
	struct gnix_fid_domain *domain = NULL;
	int ret = FI_SUCCESS;
	uint8_t ptag;
	uint32_t cookie;
	struct gnix_fid_fabric *fabric_priv;

	GNIX_TRACE(FI_LOG_DOMAIN, "\n");

	fabric_priv = container_of(fabric, struct gnix_fid_fabric, fab_fid);
	if (!info->domain_attr->name ||
	    strncmp(info->domain_attr->name, gnix_dom_name,
		    strlen(gnix_dom_name))) {
		ret = -FI_EINVAL;
		goto err;
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
			GNIX_ERR(FI_LOG_DOMAIN,
				   "gnixu_get_rdma_credentials returned ptag %u cookie 0x%x\n",
				   ptag, cookie);
			goto err;
		}
	} else {
		ret = gnixu_get_rdma_credentials(NULL, &ptag, &cookie);
	}

	GNIX_INFO(FI_LOG_DOMAIN,
		  "gnix rdma credentials returned ptag %u cookie 0x%x\n",
		  ptag, cookie);
	domain = calloc(1, sizeof *domain);
	if (domain == NULL) {
		ret = -FI_ENOMEM;
		goto err;
	}

	ret = _gnix_mr_cache_init(&domain->mr_cache, NULL);
	if (ret != FI_SUCCESS)
		goto err;

	list_head_init(&domain->nic_list);
	gnix_list_node_init(&domain->list);

	list_add_tail(&fabric_priv->domain_list, &domain->list);

	list_head_init(&domain->domain_wq);

	domain->fabric = fabric_priv;
	atomic_inc(&fabric_priv->ref_cnt);

	domain->ptag = ptag;
	domain->cookie = cookie;
	domain->cdm_id_seed = getpid();  /*TODO: direct syscall better */
	domain->gni_tx_cq_size = gnix_def_gni_tx_cq_size;
	domain->gni_rx_cq_size = gnix_def_gni_rx_cq_size;
	domain->gni_cq_modes = gnix_def_gni_cq_modes;
	atomic_initialize(&domain->ref_cnt, 0);

	domain->domain_fid.fid.fclass = FI_CLASS_DOMAIN;
	domain->domain_fid.fid.context = context;
	domain->domain_fid.fid.ops = &gnix_domain_fi_ops;
	domain->domain_fid.ops = &gnix_domain_ops;
	domain->domain_fid.mr = &gnix_domain_mr_ops;

	domain->control_progress = info->domain_attr->control_progress;
	domain->data_progress = info->domain_attr->data_progress;

	*dom = &domain->domain_fid;
	return FI_SUCCESS;

err:
	if (domain != NULL) {
		free(domain);
	}
	return ret;
}

/*******************************************************************************
 * FI_OPS_* data structures.
 ******************************************************************************/

static struct fi_ops gnix_domain_fi_ops = {
	.size = sizeof(struct fi_ops),
	.close = gnix_domain_close,
	.bind = fi_no_bind,
	.control = fi_no_control,
	.ops_open = gnix_domain_ops_open
};

static struct fi_ops_mr gnix_domain_mr_ops = {
	.size = sizeof(struct fi_ops_mr),
	.reg = gnix_mr_reg
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
