/*
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

#ifndef _GNIX_EQ_H_
#define _GNIX_EQ_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <rdma/fi_eq.h>

#include "gnix_queue.h"

#define GNIX_EQ_DEFAULT_SIZE 256

/*
 * Stores events inside of the event queue.
 *
 * type: EQ event type defined in fi_eq.h
 * len: length of the event
 * flags: control flags
 * buf: event
 * item: list entry, contains next pointer
 */
struct gnix_eq_entry {
	uint64_t flags;
	uint32_t type;
	size_t len;
	void *the_entry;

	struct slist_entry item;
};

/*
 * EQ structure. Contains error and event queue.
 */
struct gnix_fid_eq {
	struct fid_eq eq_fid;
	struct gnix_fid_fabric *eq_fabric;

	struct gnix_queue *events;
	struct gnix_queue *errors;

	struct fi_eq_attr attr;

	fastlock_t lock;
	atomic_t ref_cnt;
};

#ifdef __cplusplus
}
#endif

#endif /* _GNIX_EQ_H_ */
