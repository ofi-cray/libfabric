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

//
// memory registration common code
//
#include <stdlib.h>
#include <string.h>

#include "gnix.h"
#include "gnix_nic.h"
#include "gnix_util.h"
#include "gnix_mr.h"
#include "gnix_priv.h"
#include "common/atomics.h"

#define PAGE_SHIFT 12

/* forward declarations */
static int __mr_cache_register(
		gnix_mr_cache_t          *cache,
		struct gnix_fid_mem_desc *mr,
		struct gnix_fid_domain   *domain,
		uint64_t                 address,
		uint64_t                 length,
		gni_cq_handle_t          dst_cq_hndl,
		uint32_t                 flags,
		uint32_t                 vmdh_index,
		gni_mem_handle_t         *mem_hndl);

static int __mr_cache_deregister(
		gnix_mr_cache_t          *cache,
		struct gnix_fid_mem_desc *mr);

static int fi_gnix_mr_close(fid_t fid);


/**
 * @brief gnix memory registration cache entry
 *
 * @var   mem_hndl   gni memory handle for the memory registration
 * @var   key        gnix memory registration cache key
 * @var   domain     gnix domain associated with the memory registration
 * @var   nic        gnix nic associated with the memory registration
 * @var   ref_cnt    reference counting for the cache
 */
typedef struct gnix_mr_cache_entry {
	gni_mem_handle_t mem_hndl;
	gnix_mr_cache_key_t key;
	struct gnix_fid_domain *domain;
	struct gnix_nic *nic;
	atomic_t ref_cnt;
} gnix_mr_cache_entry_t;

static struct fi_ops fi_gnix_mr_ops = {
	.size = sizeof(struct fi_ops),
	.close = fi_gnix_mr_close,
	.bind = fi_no_bind,
	.control = fi_no_control,
	.ops_open = fi_no_ops_open,
};

/* default attributes for new caches */
static gnix_mr_cache_attr_t __default_mr_cache_attr = {
		.soft_reg_limit      = 4096,
		.hard_reg_limit      = -1,
		.hard_stale_limit    = 128,
		.lazy_deregistration = 1
};

/**
 * Sign extends the value passed into up to length parameter
 *
 * @param[in]  val  value to be sign extended
 * @param[in]  len  length to sign extend the value
 * @return          sign extended value to length, len
 */
static inline int64_t __sign_extend(
		uint64_t val,
		int len)
{
	int64_t m = 1UL << (len - 1);
	int64_t r = (val ^ m) - m;

	return r;
}

/**
 * Key comparison function for gnix memory registration caches
 *
 * @param[in] x key to be inserted or found
 * @param[in] y key to be compared against
 *
 * @return    -1 if it should be positioned at the left, 0 if the same,
 *             1 otherwise
 */
static inline int __mr_cache_key_comp(
		void *x,
		void *y)
{
	gnix_mr_cache_key_t *to_insert  = (gnix_mr_cache_key_t *) x;
	gnix_mr_cache_key_t *to_compare = (gnix_mr_cache_key_t *) y;
	uint64_t insert_end = to_insert->address + to_insert->length;
	uint64_t compare_end = to_compare->address + to_compare->length;

	/* if to_compare covers the range of to_insert, we'll call it a
	 *   duplicate
	 */
	if (to_compare->address <= to_insert->address &&
			insert_end <= compare_end)
		return 0;

	/* to the left */
	if (to_insert->address < to_compare->address)
		return -1;

	/* to the right */
	return 1;
}

/**
 * Destroys the memory registration cache entry and deregisters the memory
 *   region with uGNI
 *
 * @param[in] entry  a memory registration cache entry
 *
 * @return           grc from GNI_MemDeregister
 */
static inline int __mr_cache_entry_destroy(
		gnix_mr_cache_entry_t *entry)
{
	gni_return_t ret;

	ret = GNI_MemDeregister(entry->nic->gni_nic_hndl, &entry->mem_hndl);
	if (ret == GNI_RC_SUCCESS) {
		atomic_dec(&entry->domain->ref_cnt);
		atomic_dec(&entry->nic->ref_cnt);

		free(entry);
	} else {
		GNIX_WARN(FI_LOG_MR, "failed to deregister memory"
				" region, cache_entry=%p ret=%i\n", entry, ret);
	}

	return ret;
}

/**
 * Increments the reference count on a memory registration cache entry
 *
 * @param[in] cache  gnix memory registration cache
 * @param[in] entry  a memory registration cache entry
 *
 * @return           reference count for the registration
 */
static inline int __mr_cache_entry_get(
		gnix_mr_cache_t       *cache,
		gnix_mr_cache_entry_t *entry)
{
	return atomic_inc(&entry->ref_cnt);
}

/**
 * Decrements the reference count on a memory registration cache entry
 *
 * @param[in] cache  gnix memory registration cache
 * @param[in] entry  a memory registration cache entry
 * @param[in] iter   red-black tree iterator pointing to the entry
 *
 * @return           grc from GNI_MemDeregister
 */
static inline int __mr_cache_entry_put(
		gnix_mr_cache_t       *cache,
		gnix_mr_cache_entry_t *entry,
		RbtIterator           iter)
{
	RbtStatus rc;
	gni_return_t grc = GNI_RC_SUCCESS;

	if (atomic_dec(&entry->ref_cnt) == 0) {
		rbtErase(cache->inuse, iter);
		atomic_dec(&cache->inuse_elements);

		if (cache->attr.lazy_deregistration) {
			GNIX_INFO(FI_LOG_MR, "moving key %i:%i to stale\n",
					entry->key.address, entry->key.length);
			rc = rbtInsert(cache->stale, &entry->key, entry);
			if (rc != RBT_STATUS_OK) {
				grc = __mr_cache_entry_destroy(entry);
			} else {
				atomic_inc(&cache->stale_elements);
			}

		} else {
			grc = __mr_cache_entry_destroy(entry);
		}
	}

	if (grc != GNI_RC_SUCCESS) {
		GNIX_WARN(FI_LOG_MR, "GNI_MemDeregister returned '%s'\n",
				gni_err_str[grc]);
	}

	return grc;
}

void _gnix_convert_key_to_mhdl(
		gnix_mr_key_t *key,
		gni_mem_handle_t *mhdl)
{
	uint64_t va = key->pfn;
	uint8_t flags = 0;

	va = (uint64_t) __sign_extend(va << PAGE_SHIFT, GNIX_MR_VA_BITS);

	if (key->flags & GNIX_MR_FLAG_READONLY)
		flags |= GNI_MEMHNDL_ATTR_READONLY;

	GNI_MEMHNDL_INIT((*mhdl));
	if (key->format)
		GNI_MEMHNDL_SET_FLAGS((*mhdl), GNI_MEMHNDL_FLAG_NEW_FRMT);
	GNI_MEMHNDL_SET_VA((*mhdl), va);
	GNI_MEMHNDL_SET_MDH((*mhdl), key->mdd);
	GNI_MEMHNDL_SET_NPAGES((*mhdl), GNI_MEMHNDL_NPGS_MASK);
	GNI_MEMHNDL_SET_FLAGS((*mhdl), flags);
	GNI_MEMHNDL_SET_PAGESIZE((*mhdl), GNIX_MR_PAGE_SHIFT);
	GNI_MEMHNDL_SET_CRC((*mhdl));
}

void _gnix_convert_mhdl_to_key(
		gni_mem_handle_t *mhdl,
		gnix_mr_key_t *key)
{
	key->pfn = GNI_MEMHNDL_GET_VA((*mhdl)) >> GNIX_MR_PAGE_SHIFT;
	key->mdd = GNI_MEMHNDL_GET_MDH((*mhdl));
	key->format = GNI_MEMHNDL_NEW_FRMT((*mhdl));
	key->flags = 0;

	if (GNI_MEMHNDL_GET_FLAGS((*mhdl)) & GNI_MEMHNDL_FLAG_READONLY)
		key->flags |= GNIX_MR_FLAG_READONLY;
}

int gnix_mr_reg(struct fid *fid, const void *buf, size_t len,
		uint64_t access, uint64_t offset, uint64_t requested_key,
		uint64_t flags, struct fid_mr **mr_o, void *context)
{
	struct gnix_fid_mem_desc *mr;
	int fi_gnix_access = 0;
	struct gnix_fid_domain *domain;
	struct gnix_nic *nic;
	int rc;

	/* Flags are reserved for future use and must be 0. */
	if (flags)
		return -FI_EBADFLAGS;

	/* The offset parameter is reserved for future use and must be 0.
	 *   Additionally, check for invalid pointers, bad access flags and the
	 *   correct fclass on associated fid
	 */
	if (offset || !buf || !mr_o || !access ||
			(access & ~(FI_READ | FI_WRITE | FI_RECV | FI_SEND |
						FI_REMOTE_READ |
						FI_REMOTE_WRITE)) ||
			(fid->fclass != FI_CLASS_DOMAIN))

		return -FI_EINVAL;

	/* requested key is not permitted at this point */
	if (requested_key)
		return -FI_EKEYREJECTED;

	domain = container_of(fid, struct gnix_fid_domain, domain_fid.fid);

	mr = calloc(1, sizeof(*mr));
	if (!mr)
		return -FI_ENOMEM;

	/* If network would be able to write to this buffer, use read-write */
	if (access & (FI_RECV | FI_READ | FI_REMOTE_WRITE))
		fi_gnix_access |= GNI_MEM_READWRITE;
	else
		fi_gnix_access |= GNI_MEM_READ_ONLY;

	/* If the nic list is empty, create a nic */
	if (list_empty(&domain->nic_list)) {
		rc = gnix_nic_alloc(domain, &nic);
		if (rc) {
			GNIX_WARN(FI_LOG_MR, "could not allocate nic to do mr_reg,"
					" ret=%i\n", rc);

			return rc;
		}
	}

	/* call cache register op to retrieve the right entry */
	rc = __mr_cache_register(&domain->mr_cache, mr, domain, (uint64_t) buf,
			len, NULL, fi_gnix_access, -1, &mr->mem_hndl);
	if (rc != FI_SUCCESS)
		goto err;

	/* md.domain */
	mr->domain = domain;
	atomic_inc(&domain->ref_cnt); /* take reference on domain */

	/* md.mr_fid */
	mr->mr_fid.fid.fclass = FI_CLASS_MR;
	mr->mr_fid.fid.context = context;
	mr->mr_fid.fid.ops = &fi_gnix_mr_ops;

	/* nic */
	atomic_inc(&mr->nic->ref_cnt); /* take reference on nic */

	/* setup internal key structure */
	_gnix_convert_mhdl_to_key(&mr->mem_hndl,
			(gnix_mr_key_t *) &mr->mr_fid.key);

	/* set up mr_o out pointer */
	*mr_o = &mr->mr_fid;
	return FI_SUCCESS;

err:
	free(mr);
	return rc;
}

/**
 * Closes and deallocates a libfabric memory registration
 *
 * @param[in]  fid  libfabric memory registration fid
 *
 * @return     FI_SUCCESS on success
 *             -FI_EINVAL on invalid fid
 *             -FI_NOENT when there isn't a matching registration for the
 *               provided fid
 *             Otherwise, GNI_RC_* ret codes converted to FI_* err codes
 */
static int fi_gnix_mr_close(fid_t fid)
{
	struct gnix_fid_mem_desc *mr;
	gni_return_t ret;

	if (fid->fclass != FI_CLASS_MR)
		return -FI_EINVAL;

	mr = container_of(fid, struct gnix_fid_mem_desc, mr_fid.fid);

	/* call cache deregister op */
	ret = __mr_cache_deregister(&mr->domain->mr_cache, mr);
	if (ret == FI_SUCCESS) {
		/* release references to the domain and nic */
		atomic_dec(&mr->domain->ref_cnt);
		atomic_dec(&mr->nic->ref_cnt);

		free(mr);
	} else {
		GNIX_WARN(FI_LOG_MR, "failed to deregister memory, "
				"ret=%i\n", ret);
	}

	return ret;
}

/**
 * Checks the sanity of cache attributes
 *
 * @param[in]   attr  attributes structure to be checked
 * @return      FI_SUCCESS if the attributes are valid
 *              -FI_EINVAL if the attributes are invalid
 */
static inline int __check_mr_cache_attr_sanity(gnix_mr_cache_attr_t *attr)
{
	/* 0 < attr->hard_reg_limit < attr->soft_reg_limit */
	if (attr->hard_reg_limit > 0 &&
			attr->hard_reg_limit < attr->soft_reg_limit)
		return -FI_EINVAL;

	/* valid otherwise */
	return FI_SUCCESS;
}

int _gnix_mr_cache_init(
		gnix_mr_cache_t      *cache,
		gnix_mr_cache_attr_t *attr)
{
	gnix_mr_cache_attr_t *cache_attr = &__default_mr_cache_attr;

	/* ensure we have a relatively clean pointer */
	if (!cache || cache->state == GNIX_MRC_STATE_READY ||
			cache->state > GNIX_MRC_STATE_DEAD)
		return -FI_EINVAL;

	/* if the provider asks us to use their attributes, are they sane? */
	if (attr) {
		if (__check_mr_cache_attr_sanity(attr) != FI_SUCCESS)
			return -FI_EINVAL;

		cache_attr = attr;
	}

	/* save the attribute values */
	memcpy(&cache->attr, cache_attr, sizeof(*cache_attr));

	/* set up inuse tree */
	cache->inuse = rbtNew(__mr_cache_key_comp);
	if (!cache->inuse)
		return -FI_ENOMEM;

	/* if using lazy deregistration, set up stale tree */
	if (cache->attr.lazy_deregistration) {
		cache->stale = rbtNew(__mr_cache_key_comp);
		if (!cache->stale) {
			rbtDelete(cache->inuse);
			cache->inuse = NULL;

			return -FI_ENOMEM;
		}
	}

	/* initialize the element counts. If we are reinitializing a dead cache,
	 *   destroy will have already set the element counts
	 */
	if (cache->state == GNIX_MRC_STATE_UNINITIALIZED) {
		atomic_initialize(&cache->inuse_elements, 0);
		atomic_initialize(&cache->stale_elements, 0);
	}

	cache->state = GNIX_MRC_STATE_READY;

	return FI_SUCCESS;
}

int _gnix_mr_cache_destroy(gnix_mr_cache_t *cache)
{
	if (cache->state != GNIX_MRC_STATE_READY)
		return -FI_EINVAL;

	/*
	 * Remove all of the stale entries from the cache
	 */
	_gnix_mr_cache_flush(cache);

	/*
	 * if there are still elements in the cache after the flush,
	 *   then someone forgot to deregister memory. We probably shouldn't
	 *   destroy the cache at this point.
	 */
	if (atomic_get(&cache->inuse_elements) != 0) {
		return -FI_EAGAIN;
	}

	/* destroy the tree */
	rbtDelete(cache->inuse);
	cache->inuse = NULL;

	/* stale will been flushed already, so just destroy the tree */
	if (cache->attr.lazy_deregistration) {
		rbtDelete(cache->stale);
		cache->stale = NULL;
	}

	cache->state = GNIX_MRC_STATE_DEAD;

	return FI_SUCCESS;
}

int __mr_cache_flush(gnix_mr_cache_t *cache, int flush_count) {
	RbtIterator iter, next;
	gnix_mr_cache_key_t *key;
	gnix_mr_cache_entry_t *entry;
	int destroyed = 0;

	GNIX_INFO(FI_LOG_MR, "starting flush on memory registration cache\n");

	/* flushes are unnecessary for caches without lazy deregistration */
	if (!cache->attr.lazy_deregistration)
		return FI_SUCCESS;

	for (iter = rbtBegin(cache->stale);
			iter != rbtEnd(cache->stale);
			iter = next) {

		if (flush_count >= 0 && flush_count == destroyed)
			break;

		rbtKeyValue(cache->stale, iter, (void **) &key,
				(void **) &entry);

		next = rbtNext(cache->stale, iter);
		rbtErase(cache->stale, iter);

		__mr_cache_entry_destroy(entry);
		entry = NULL;
		++destroyed;
	}

	GNIX_INFO(FI_LOG_MR, "flushed %i of %i entries from memory "
				"registration cache\n", destroyed,
				atomic_get(&cache->stale_elements));

	if (destroyed > 0) {
		atomic_sub(&cache->stale_elements, destroyed);
	}

	return FI_SUCCESS;
}

int _gnix_mr_cache_flush(gnix_mr_cache_t *cache)
{

	if (cache->state != GNIX_MRC_STATE_READY)
		return -FI_EINVAL;

	__mr_cache_flush(cache, cache->attr.hard_reg_limit);

	return FI_SUCCESS;
}

/**
 * Function to register memory with the cache
 *
 * @param[in] cache        gnix memory registration cache pointer
 * @param[in] mr           gnix memory region descriptor pointer
 * @param[in] domain       gnix domain pointer
 * @param[in] address      base address of the memory region to be registered
 * @param[in] length       length of the memory region to be registered
 * @param[in] dst_cq_hndl  destination gni cq handle for cq event delivery
 * @param[in] flags        gni memory registration flags
 * @param[in] vmdh_index   desired index for the new vmdh
 * @param[in,out] mem_hndl gni memory handle pointer to written to and returned
 */
static int __mr_cache_register(
		gnix_mr_cache_t          *cache,
		struct gnix_fid_mem_desc *mr,
		struct gnix_fid_domain   *domain,
		uint64_t                 address,
		uint64_t                 length,
		gni_cq_handle_t          dst_cq_hndl,
		uint32_t                 flags,
		uint32_t                 vmdh_index,
		gni_mem_handle_t         *mem_hndl)
{
	RbtStatus rc;
	RbtIterator iter;
	gnix_mr_cache_key_t key, *e_key;
	gnix_mr_cache_entry_t *entry;
	struct gnix_nic *nic;
	gni_return_t grc = GNI_RC_SUCCESS;

	/* if we shouldn't introduce any new elements, return -FI_ENOSPC */
	if (cache->attr.hard_reg_limit > 0 &&
			atomic_get(&cache->inuse_elements) >= cache->attr.hard_reg_limit)
		return FI_ENOSPC;

	/* build key for searching */
	key.address = address;
	key.length = length;

	/* Is the key in the inuse tree? */
	iter = rbtFind(cache->inuse, &key);
	if (iter) {
		/* Let's increment the ref count of the entry */
		rbtKeyValue(cache->inuse, iter, (void **) &e_key,
				(void **) &entry);

		__mr_cache_entry_get(cache, entry);
		goto success;
	} else if (cache->attr.lazy_deregistration) {
		/* if lazy deregistration is in use, we can check the
		 *   stale tree
		 */
		iter = rbtFind(cache->stale, &key);
		if (iter) {
			rbtKeyValue(cache->stale, iter, (void **) &e_key,
					(void **) &entry);

			/* reset the reference count as it should be zero from
			 *   being in the stale tree anyway
			 */
			atomic_set(&entry->ref_cnt, 1);

			/* clear the element from the stale cache */
			rbtErase(cache->stale, iter);
			atomic_dec(&cache->stale_elements);

			GNIX_INFO(FI_LOG_MR, "moving key %i:%i from stale into inuse\n",
					entry->key.address, entry->key.length);
			rc = rbtInsert(cache->inuse, (void *) &entry->key,
					(void *) entry);
			if (rc == RBT_STATUS_MEM_EXHAUSTED) {
				__mr_cache_entry_destroy(entry);
				return -FI_ENOMEM;
			} else if (rc != RBT_STATUS_OK) {
				GNIX_WARN(FI_LOG_MR, "unexpected error condition "
						"during cache insert, ret=%i\n", rc);
			}

			atomic_inc(&cache->inuse_elements);

			goto success;
		}
	}

	/* If the cache is full, then flush one of the stale entries to make
	 *   room for the new entry. This works because we check above to see if
	 *   the number of inuse entries exceeds the hard reg limit
	 */
	if ((atomic_get(&cache->inuse_elements) + 
			atomic_get(&cache->stale_elements)) == cache->attr.hard_reg_limit)
		__mr_cache_flush(cache, 1);

	/* if we made it here, we didn't find the entry at all */
	entry = calloc(1, sizeof(*entry));
	if (!entry)
		return -FI_ENOMEM;

	/* TODO: should we just try the first nic we find? */
	list_for_each(&domain->nic_list, nic, list)
	{
		grc = GNI_MemRegister(nic->gni_nic_hndl, address, length,
					dst_cq_hndl, flags,
					vmdh_index, &entry->mem_hndl);
		if (grc == GNI_RC_SUCCESS)
			break;
	}

	if (grc != GNI_RC_SUCCESS) {
		free(entry);
		GNIX_INFO(FI_LOG_MR, "failed to register memory with uGNI, "
				"ret=%s", gni_err_str[grc]);
		return -gnixu_to_fi_errno(grc);
	}

	/* set up the entry's key */
	entry->key.address = address;
	entry->key.length = length;

	GNIX_INFO(FI_LOG_MR, "inserting key %i:%i into inuse\n",
			entry->key.address, entry->key.length);
	rc = rbtInsert(cache->inuse, &entry->key, entry);
	if (rc != RBT_STATUS_OK) {
		GNIX_INFO(FI_LOG_MR, "failed to insert registration "
				"into cache, ret=%i", rc);

		grc = GNI_MemDeregister(nic->gni_nic_hndl, &entry->mem_hndl);
		if (grc != GNI_RC_SUCCESS) {
			GNIX_INFO(FI_LOG_MR, "failed to deregister memory with "
					"uGNI, ret=%s", gni_err_str[grc]);
		}

		free(entry);
		return -FI_ENOMEM;
	}

	atomic_inc(&cache->inuse_elements);
	atomic_initialize(&entry->ref_cnt, 1);
	entry->domain = domain;
	entry->nic = nic;

	atomic_inc(&entry->domain->ref_cnt);
	atomic_inc(&entry->nic->ref_cnt);

success:
	mr->nic = entry->nic;
	mr->key.address = entry->key.address;
	mr->key.length = entry->key.length;
	*mem_hndl = entry->mem_hndl;
	return FI_SUCCESS;
}

/**
 * Function to deregister memory in the cache
 *
 * @param[in]  cache  gnix memory registration cache pointer
 * @param[in]  mr     gnix memory registration descriptor pointer
 *
 * @return     FI_SUCCESS on success
 *             -FI_ENOENT if there isn't an active memory registration
 *               associated with the mr
 *             GNI_RC_* return codes for potential calls to GNI_MemDeregister
 */
static int __mr_cache_deregister(
		gnix_mr_cache_t          *cache,
		struct gnix_fid_mem_desc *mr)
{
	RbtIterator iter;
	gnix_mr_cache_key_t *e_key;
	gnix_mr_cache_entry_t *entry;
	gni_return_t grc;

	/* check to see if we can find the entry so that we can drop the
	 *   held reference
	 */
	GNIX_INFO(FI_LOG_MR, "searching for key %i:%i\n",
			mr->key.address, mr->key.length);
	iter = rbtFind(cache->inuse, &mr->key);
	if (!iter) {
		GNIX_WARN(FI_LOG_MR, "failed to find entry in the inuse cache\n");
		return -FI_ENOENT;
	}

	rbtKeyValue(cache->inuse, iter, (void **) &e_key, (void **) &entry);

	grc = __mr_cache_entry_put(cache, entry, iter);

	/* Since we check this on each deregistration, the amount of elements
	 * over the limit should always be 1
	 */
	if (atomic_get(&cache->stale_elements) > cache->attr.hard_stale_limit)
		__mr_cache_flush(cache, 1);

	return gnixu_to_fi_errno(grc);
}
