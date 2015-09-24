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

#ifndef _GNIX_CM_NIC_H_
#define _GNIX_CM_NIC_H_

#include "gnix.h"

/**
 * @brief GNI provider connection management (cm) nic structure
 *
 * @var lock           spin lock for protecting calls in to GNI using
 *                     gni_nic_hndl
 * @var gni_cdm_hndl   underlying gni cdm handle associated with this nic
 * @var gni_nic_hndl   underlying gni nic handle associated with this nic
 * @var dgram_hndl     handle to dgram allocator associated with this nic
 * @var domain         GNI provider domain associated with this nic
 * @var wq_lock        spin lock for cm nic's work queue
 * @var cm_nic_wq      workqueue associated with this nic
 * @var cdm_id         cdm_id of this nic.  This is unique
 *                     on the local node for a given ptag/cookie.
 * @var ptag           ptag of this nic.
 * @var cookie         cookie of this nic.
 * @var device_id      local Aries device id associated with this nic.
 *                     This will always be zero unless Cray starts
 *                     selling systems with multiple aries/node.
 * @var device_addr    Aries network address associated with this nic.
 */
struct gnix_cm_nic {
	fastlock_t lock;
	gni_cdm_handle_t gni_cdm_hndl;
	gni_nic_handle_t gni_nic_hndl;
	struct gnix_dgram_hndl *dgram_hndl;
	struct gnix_fid_domain *domain;
	fastlock_t wq_lock;
	struct list_head cm_nic_wq;
	enum fi_progress control_progress;
	uint32_t cdm_id;
	uint8_t ptag;
	uint32_t cookie;
	uint32_t device_id;
	uint32_t device_addr;
};

/**
 * @brief Frees a previously allocated cm nic structure
 *
 * @param[in] cm_nic   pointer to previously allocated gnix_cm_nic struct
 * @return             FI_SUCCESS on success, -EINVAL on invalid argument
 */
int _gnix_cm_nic_free(struct gnix_cm_nic *cm_nic);

/**
 * @brief allocates a cm nic structure
 *
 * @param[in]  domain   pointer to a previously allocated gnix_fid_domain struct
 * @param[in]  cdm_id   cdm id to use when creating cm nic's GNI CDM.  If
 *                      set to -1, a cdm_id will be generated for the cm nic.
 * @param[out] cm_nic   pointer to address where address of the allocated
 *                      cm nic structure should be returned
 * @return              FI_SUCCESS on success, -EINVAL on invalid argument,
 *                      -FI_ENOMEM if insufficient memory to allocate
 *                      the cm nic structure
 */
int _gnix_cm_nic_alloc(struct gnix_fid_domain *domain,
		       uint32_t cdm_id,
		       struct gnix_cm_nic **cm_nic);

/**
 * @brief poke the cm nic's progress engine
 *
 * @param[in] cm_nic   pointer to previously allocated gnix_cm_nic struct
 * @return              FI_SUCCESS on success, -EINVAL on invalid argument.
 *                     Other error codes may be returned depending on the
 *                     error codes returned from callback function
 *                     that had been added to the nic's work queue.
 */
int _gnix_cm_nic_progress(struct gnix_cm_nic *cm_nic);

/**
 * @brief function to return a unique 32 bit id for the ptag/cookie associated
 *        with the supplied domain.
 *
 * @param[in]  domain  pointer to a previously allocated gnix_fid_domain struct
 * @param[out] id      Unique id on the local node for the given ptag/cookie
 *                     associated with the supplied domain.
 * @return             FI_SUCCESS on success.  Currently no other error codes
 *                     can be returned.
 */
int _gnix_get_new_cdm_id(struct gnix_fid_domain *domain, uint32_t *id);

#endif /* _GNIX_CM_NIC_H_ */
