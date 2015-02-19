/*
 * Copyright (c) 2014 Intel Corporation, Inc.  All rights reserved.
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

#if HAVE_CONFIG_H
#include <config.h>
#endif /* HAVE_CONFIG_H */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <rdma/fabric.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_rma.h>
#include <rdma/fi_errno.h>
#include "prov.h"

#include "gnix.h"
#include "gnix_util.h"
#include "gnix_nameserver.h"

const char gnix_fab_name[] = "gni";
const char gnix_dom_name[] = "/sys/class/gni/kgni0";
uint32_t gnix_cdm_modes = (GNI_CDM_MODE_FAST_DATAGRAM_POLL | \
			   GNI_CDM_MODE_FMA_SHARED | \
			   GNI_CDM_MODE_FMA_SMALL_WINDOW | \
			   GNI_CDM_MODE_FORK_PARTCOPY | \
			   GNI_CDM_MODE_ERR_NO_KILL);

const struct fi_fabric_attr gnix_fabric_attr = {
	.fabric = NULL,
	.name = NULL,
	.prov_name = NULL,
	.prov_version = FI_VERSION(GNI_MAJOR_VERSION, GNI_MINOR_VERSION),
};

static struct fi_ops_fabric gnix_fab_ops = {
	.size = sizeof(struct fi_ops_fabric),
	.domain = gnix_domain_open,
	/* TODO: need to define for FI_EP_MSG */
	.passive_ep = NULL,
	/* TODO: need to define for FI_EP_MSG */
	.eq_open = NULL,
	/* TODO: what's this about */
	.wait_open = NULL,
};

static int gnix_fabric_close(fid_t fid)
{
	struct gnix_fabric *fab;
	fab = container_of(fid, struct gnix_fabric, fab_fid);

	if(!list_empty(&fab->cdm_list)) {
		return -FI_EBUSY;
	}

	free(fab);
	return FI_SUCCESS;
}

static struct fi_ops gnix_fab_fi_ops = {
	.size = sizeof(struct fi_ops),
	.close = gnix_fabric_close,
	.bind = fi_no_bind,
	.control = fi_no_control,
	.ops_open = fi_no_ops_open,
};

/*
 * define methods needed for the GNI fabric provider
 */
static int gnix_fabric(struct fi_fabric_attr *attr, struct fid_fabric **fabric,
		       void *context)
{
	struct gnix_fabric *fab;

	if (strcmp(attr->name, gnix_fab_name)) {
		return -FI_ENODATA;
	}

	fab = calloc(1, sizeof(*fab));
	if (!fab) {
		return -FI_ENOMEM;
	}

	fab->fab_fid.fid.fclass = FI_CLASS_FABRIC;
	fab->fab_fid.fid.context = context;
	fab->fab_fid.fid.ops = &gnix_fab_fi_ops;
	fab->fab_fid.ops = &gnix_fab_ops;
        list_head_init(&fab->cdm_list);
	*fabric = &fab->fab_fid;

	return FI_SUCCESS;
}

static int gnix_getinfo(uint32_t version, const char *node, const char *service,
			uint64_t flags, struct fi_info *hints,
			struct fi_info **info)
{
	int ret = 0;
	int mode = GNIX_FAB_MODES;
	struct fi_info *gnix_info;
	struct gnix_ep_name *dest_addr = NULL, *src_addr = NULL, *addr = NULL;

	/*
	 * the code below for resolving a node/service to what
	 * will be a gnix_ep_name address is not fully implemented,
	 * but put a place holder in place
	 */
	if (node) {
		addr = malloc(sizeof(*addr));
		if (!addr) {
			ret = -FI_ENOMEM;
			goto err;
		}

		ret = gnix_resolve_name(node, service, addr);
		if (ret) {
			goto err;
		}

		if (flags & FI_SOURCE) {
			src_addr = addr;
		} else {
			dest_addr = addr;
		}
	}

	if (hints) {
		/*
		 * check for endpoint type, only support FI_EP_RDM for now
		 */
		switch (hints->ep_type) {
		case FI_EP_UNSPEC:
		case FI_EP_RDM:
			break;
		default:
			ret = -FI_ENODATA;
			goto err;
		}

		/*
		 * check the mode field
		 */
		if (hints->mode) {
			if ((hints->mode & GNIX_FAB_MODES) != GNIX_FAB_MODES) {
				ret = -FI_ENODATA;
				goto err;
			}
			mode = hints->mode & ~GNIX_FAB_MODES_CLEAR;
		}

		if ((hints->caps & GNIX_EP_RDM_CAPS) != hints->caps) {
			goto err;
		}

		if (hints->ep_attr) {
			switch (hints->ep_attr->protocol) {
			case FI_PROTO_UNSPEC:
			case FI_PROTO_GNI:
				break;
			default:
				ret = -FI_ENODATA;
				goto err;
			}

			if (hints->ep_attr->tx_ctx_cnt > 1) {
				ret = -FI_ENODATA;
				goto err;
			}

			if (hints->ep_attr->rx_ctx_cnt > 1) {
				ret = -FI_ENODATA;
				goto err;
			}
		}

		if (hints->tx_attr &&
		    (hints->tx_attr->op_flags & GNIX_EP_OP_FLAGS) !=
			hints->tx_attr->op_flags) {
			ret = -FI_ENODATA;
			goto err;
		}

		if (hints->rx_attr &&
		    (hints->rx_attr->op_flags & GNIX_EP_OP_FLAGS) !=
			hints->rx_attr->op_flags) {
			ret = -FI_ENODATA;
			goto err;
		}

		if (hints->fabric_attr && hints->fabric_attr->name &&
		    strncmp(hints->fabric_attr->name, gnix_fab_name,
			    strlen(gnix_fab_name))) {
			ret = -FI_ENODATA;
			goto err;
		}

		/* TODO: use hardwared kgni const string */
		if (hints->domain_attr && hints->domain_attr->name &&
		    strncmp(hints->domain_attr->name, gnix_dom_name,
			    strlen(gnix_dom_name))) {
			ret = -FI_ENODATA;
			goto err;
		}

		if (hints->ep_attr) {
			if (hints->ep_attr->max_msg_size > GNIX_MAX_MSG_SIZE) {
				ret = -FI_ENODATA;
				goto err;
			}
			if (hints->ep_attr->inject_size > GNIX_INJECT_SIZE) {
				ret = -FI_ENODATA;
				goto err;
			}
			/*
			 * TODO: tag matching
			 * max_tag_value =
			 * fi_tag_bits(hints->ep_attr->mem_tag_format);
			 */
		}
	}

	/*
	 * fill in the gnix_info struct
	 */
	gnix_info = fi_allocinfo_internal();
	if (gnix_info == NULL) {
		ret = -FI_ENOMEM;
		goto err;
	}

	gnix_info->ep_attr->protocol = FI_PROTO_GNI;
	gnix_info->ep_attr->max_msg_size = GNIX_MAX_MSG_SIZE;
	gnix_info->ep_attr->inject_size = GNIX_INJECT_SIZE;
	/* TODO: need to work on this */
	gnix_info->ep_attr->total_buffered_recv = ~(0ULL);
	/* TODO: need to work on this */
	gnix_info->ep_attr->mem_tag_format = 0x0;
	/* TODO: remember this when implementing sends */
	gnix_info->ep_attr->msg_order = FI_ORDER_SAS;
	gnix_info->ep_attr->comp_order = FI_ORDER_NONE;
	gnix_info->ep_attr->tx_ctx_cnt = 1;
	gnix_info->ep_attr->rx_ctx_cnt = 1;

	gnix_info->domain_attr->threading = FI_THREAD_COMPLETION;
	gnix_info->domain_attr->control_progress = FI_PROGRESS_AUTO;
	gnix_info->domain_attr->data_progress = FI_PROGRESS_AUTO;
	/* only one aries per node */
	gnix_info->domain_attr->name = strdup(gnix_dom_name);

	gnix_info->next = NULL;
	gnix_info->ep_type = FI_EP_RDM;
	gnix_info->caps = GNIX_EP_RDM_CAPS;
	gnix_info->mode = mode;
	gnix_info->addr_format = FI_ADDR_GNI;
	gnix_info->src_addrlen = sizeof(struct gnix_ep_name);
	gnix_info->dest_addrlen = sizeof(struct gnix_ep_name);
	gnix_info->src_addr = src_addr;
	gnix_info->dest_addr = dest_addr;
	gnix_info->fabric_attr->name = strdup(gnix_fab_name);
	/* let's consider gni copyrighted :) */
	gnix_info->fabric_attr->prov_name = strdup(gnix_fab_name);

	gnix_info->tx_attr->caps = gnix_info->caps;
	gnix_info->tx_attr->mode = gnix_info->mode;

	if(hints && hints->tx_attr && hints->tx_attr->op_flags) {
		gnix_info->tx_attr->op_flags = hints->tx_attr->op_flags;
	} else {
		gnix_info->tx_attr->op_flags = GNIX_EP_OP_FLAGS;
	}

	gnix_info->tx_attr->msg_order = gnix_info->ep_attr->msg_order;
	gnix_info->tx_attr->comp_order = gnix_info->ep_attr->comp_order;
	gnix_info->tx_attr->inject_size = gnix_info->ep_attr->inject_size;
	/* TODO: probably something else here */
	gnix_info->tx_attr->size = UINT64_MAX;
	gnix_info->tx_attr->iov_limit = 1;

	gnix_info->rx_attr->caps = gnix_info->caps;
	gnix_info->rx_attr->mode = gnix_info->mode;

	if(hints && hints->rx_attr && hints->rx_attr->op_flags) {
		gnix_info->rx_attr->op_flags = hints->rx_attr->op_flags;
	} else {
		gnix_info->rx_attr->op_flags = GNIX_EP_OP_FLAGS;
	}

	gnix_info->rx_attr->msg_order = gnix_info->ep_attr->msg_order;
	gnix_info->rx_attr->comp_order = gnix_info->ep_attr->comp_order;
	gnix_info->rx_attr->total_buffered_recv =
	    gnix_info->ep_attr->total_buffered_recv;
	/* TODO: probably something else here */
	gnix_info->rx_attr->size = UINT64_MAX;
	gnix_info->rx_attr->iov_limit = 1;

	*info = gnix_info;
	return 0;
err:
	return ret;
}

static void gnix_fini(void)
{
}

struct fi_provider gnix_prov = {
	.name = "gni",
	.version = FI_VERSION(GNI_MAJOR_VERSION, GNI_MINOR_VERSION),
	.fi_version = FI_VERSION(FI_MAJOR_VERSION, FI_MINOR_VERSION),
	.getinfo = gnix_getinfo,
	.fabric = gnix_fabric,
	.cleanup = gnix_fini
};

GNI_INI
{
	struct fi_provider *provider = NULL;
	gni_return_t status;
	gni_version_info_t lib_version;
	int num_devices;

	/*
	 * if no GNI devices available, don't register as provider
	 */
	status = GNI_GetNumLocalDevices(&num_devices);
	if ((status != GNI_RC_SUCCESS) || (num_devices == 0)) {
		return NULL;
	}

	/* sanity check that the 1 aries/node holds */
	assert(num_devices == 1);

	/*
	 * don't register if available ugni is older than one libfabric was
	 * built against
	 */
	status = GNI_GetVersionInformation(&lib_version);
	if ((GNI_GET_MAJOR(lib_version.ugni_version) > GNI_MAJOR_REV) ||
	    ((GNI_GET_MAJOR(lib_version.ugni_version) == GNI_MAJOR_REV) &&
	     GNI_GET_MINOR(lib_version.ugni_version) >= GNI_MINOR_REV)) {
		provider = &gnix_prov;
	}

	return (provider);
}
