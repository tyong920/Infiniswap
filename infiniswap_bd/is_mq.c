// SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause
/*
 * Copyright 2017 University of Michigan, Ann Arbor
 * Copyright 2014 Oren Kishon
 * Copyright (c) 2013 Mellanox Technologies. All rights reserved.
 */
#include <linux/bitmap.h>
#include <linux/ctype.h>
#include <linux/fs.h>
#include <linux/highmem.h>
#include <linux/major.h>
#include <linux/namei.h>
#include <linux/refcount.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>

#include "infiniswap.h"
#include "is_io_policy.h"
#include "is_rdma.h"

#ifdef INFINISWAP_HAVE_BDEV_HANDLE
#define IS_BACKING_MODE (BLK_OPEN_READ | BLK_OPEN_WRITE | BLK_OPEN_EXCL)
#else
#define IS_BACKING_MODE (FMODE_READ | FMODE_WRITE | FMODE_EXCL)
#endif

static bool is_device_configurable(struct is_device *device)
{
	return device->state == IS_DEVICE_CREATED ||
	       device->state == IS_DEVICE_STOPPED;
}

static void is_set_io_state(struct is_device *device,
			    enum is_device_state state, bool accepting_io)
{
	unsigned long flags;

	spin_lock_irqsave(&device->io_lock, flags);
	device->state = state;
	device->accepting_opens = accepting_io;
	device->accepting_io = accepting_io;
	spin_unlock_irqrestore(&device->io_lock, flags);
}

void is_device_init(struct is_device *device, const char *name)
{
	mutex_init(&device->configfs_lock);
	mutex_init(&device->lifecycle_lock);
	spin_lock_init(&device->io_lock);
	spin_lock_init(&device->backing_lock);
	init_waitqueue_head(&device->drain_wait);
	atomic_set(&device->openers, 0);
	atomic_set(&device->inflight, 0);
	atomic_set(&device->connection_state, IS_CONNECTION_NOT_CONNECTED);
	atomic_set(&device->backing_state, IS_BACKING_HEALTHY);
	atomic_set(&device->mapped_hot_ranges, 0);
	atomic64_set(&device->next_io_generation, 0);
	atomic64_set(&device->backing_failures_total, 0);
	atomic64_set(&device->backing_retries_total, 0);
	atomic64_set(&device->backing_degraded_transitions_total, 0);
	atomic64_set(&device->provider_timeouts_total, 0);
	atomic64_set(&device->late_rdma_completions_total, 0);
	atomic64_set(&device->rejected_writes_total, 0);
	atomic64_set(&device->local_only_writes_total, 0);
	atomic64_set(&device->backing_invalid_sectors, 0);
	device->mode = IS_DEVICE_MODE_UNSET;
	device->acknowledgement_policy = IS_ACKNOWLEDGEMENT_POLICY_STRICT;
	device->provider_failure_deadline_ms =
		IS_PROTOCOL_FAILURE_DEADLINE_DEFAULT_MS;
	device->hot_range_threshold = IS_HOT_RANGE_THRESHOLD_DEFAULT;
	device->hot_range_read_weight = IS_HOT_RANGE_READ_WEIGHT_DEFAULT;
	device->hot_range_write_weight = IS_HOT_RANGE_WRITE_WEIGHT_DEFAULT;
	device->rdma_numa_node = NUMA_NO_NODE;
	device->swap_priority = -1;
	device->state = IS_DEVICE_CREATED;
	device->minor = -1;
	strscpy(device->name, name, sizeof(device->name));
}

const char *is_device_mode_name(struct is_device *device)
{
	const char *name;

	mutex_lock(&device->lifecycle_lock);
	name = device->mode == IS_DEVICE_MODE_BACKED ? "backed" : "unset";
	mutex_unlock(&device->lifecycle_lock);
	return name;
}

int is_device_set_mode(struct is_device *device, const char *buf, size_t count)
{
	int ret = 0;

	if (!sysfs_streq(buf, "backed"))
		return -EOPNOTSUPP;

	mutex_lock(&device->lifecycle_lock);
	if (!is_device_configurable(device))
		ret = -EBUSY;
	else if (device->mode == IS_DEVICE_MODE_UNSET)
		device->mode = IS_DEVICE_MODE_BACKED;
	else if (device->mode != IS_DEVICE_MODE_BACKED)
		ret = -EINVAL;
	mutex_unlock(&device->lifecycle_lock);
	return ret;
}

const char *is_device_acknowledgement_policy_name(struct is_device *device)
{
	const char *name;

	mutex_lock(&device->lifecycle_lock);
	switch (device->acknowledgement_policy) {
	case IS_ACKNOWLEDGEMENT_POLICY_STRICT:
		name = "strict";
		break;
	case IS_ACKNOWLEDGEMENT_POLICY_REMOTE_FIRST:
		name = "remote-first";
		break;
	default:
		name = "unset";
		break;
	}
	mutex_unlock(&device->lifecycle_lock);
	return name;
}

int is_device_set_acknowledgement_policy(struct is_device *device,
					 const char *buf, size_t count)
{
	enum is_acknowledgement_policy policy;
	int ret = 0;

	if (sysfs_streq(buf, "strict"))
		policy = IS_ACKNOWLEDGEMENT_POLICY_STRICT;
	else if (sysfs_streq(buf, "remote-first"))
		policy = IS_ACKNOWLEDGEMENT_POLICY_REMOTE_FIRST;
	else
		return -EINVAL;

	mutex_lock(&device->lifecycle_lock);
	if (!is_device_configurable(device))
		ret = -EBUSY;
	else
		device->acknowledgement_policy = policy;
	mutex_unlock(&device->lifecycle_lock);
	return ret;
}

int is_device_set_failure_deadline(struct is_device *device, const char *buf,
				   size_t count)
{
	u32 deadline_ms;
	int ret;

	ret = kstrtou32(buf, 0, &deadline_ms);
	if (ret)
		return ret;
	if (deadline_ms < IS_PROTOCOL_FAILURE_DEADLINE_MIN_MS ||
	    deadline_ms > IS_PROTOCOL_FAILURE_DEADLINE_MAX_MS)
		return -ERANGE;

	mutex_lock(&device->lifecycle_lock);
	if (!is_device_configurable(device))
		ret = -EBUSY;
	else {
		device->provider_failure_deadline_ms = deadline_ms;
		ret = 0;
	}
	mutex_unlock(&device->lifecycle_lock);
	return ret;
}

int is_device_set_hot_range_threshold(struct is_device *device, const char *buf,
				      size_t count)
{
	u64 threshold;
	int ret;

	(void)count;
	ret = kstrtoull(buf, 0, &threshold);
	if (ret)
		return ret;
	if (!threshold || threshold > S64_MAX)
		return -ERANGE;
	WRITE_ONCE(device->hot_range_threshold, threshold);
	is_rdma_mapping_parameters_changed(device);
	return 0;
}

static int is_device_set_hot_range_weight(u32 *target, const char *buf)
{
	u32 weight;
	int ret = kstrtou32(buf, 0, &weight);

	if (ret)
		return ret;
	if (!weight || weight > 1000000U)
		return -ERANGE;
	WRITE_ONCE(*target, weight);
	return 0;
}

int is_device_set_hot_range_read_weight(struct is_device *device,
					const char *buf, size_t count)
{
	(void)count;
	return is_device_set_hot_range_weight(&device->hot_range_read_weight, buf);
}

int is_device_set_hot_range_write_weight(struct is_device *device,
					 const char *buf, size_t count)
{
	(void)count;
	return is_device_set_hot_range_weight(&device->hot_range_write_weight, buf);
}

static bool is_runtime_identifier_valid(const char *value)
{
	const unsigned char *cursor = (const unsigned char *)value;

	if (!value[0] || !isalnum((unsigned char)value[0]) ||
	    strnlen(value, IS_PROTOCOL_CONSUMER_ID_MAX + 1U) >
		    IS_PROTOCOL_CONSUMER_ID_MAX)
		return false;
	for (; *cursor; cursor++) {
		if (!isalnum(*cursor) && *cursor != '.' && *cursor != '_' &&
		    *cursor != '-')
			return false;
	}
	return true;
}

int is_device_set_consumer_id(struct is_device *device, const char *buf,
			      size_t count)
{
	char *candidate;
	char *consumer_id;
	int ret = 0;

	if (!count || count >= IS_CONSUMER_ID_SIZE)
		return -ENAMETOOLONG;
	candidate = kstrndup(buf, count, GFP_KERNEL);
	if (!candidate)
		return -ENOMEM;
	consumer_id = strim(candidate);
	if (!is_runtime_identifier_valid(consumer_id)) {
		ret = -EINVAL;
		goto out;
	}

	mutex_lock(&device->lifecycle_lock);
	if (!is_device_configurable(device))
		ret = -EBUSY;
	else
		strscpy(device->consumer_id, consumer_id,
			sizeof(device->consumer_id));
	mutex_unlock(&device->lifecycle_lock);
out:
	kfree(candidate);
	return ret;
}

static bool is_provider_list_valid(char *providers)
{
	char *cursor = providers;
	char *provider;

	while ((provider = strsep(&cursor, ",")) != NULL) {
		if (!is_runtime_identifier_valid(provider))
			return false;
	}
	return true;
}

int is_device_set_providers(struct is_device *device, const char *buf,
			    size_t count)
{
	char *candidate;
	char *providers;
	char *validation_copy;
	int ret = 0;

	if (!count || count >= IS_PROVIDER_LIST_SIZE)
		return -ENAMETOOLONG;
	candidate = kstrndup(buf, count, GFP_KERNEL);
	if (!candidate)
		return -ENOMEM;
	providers = strim(candidate);
	if (strchr(providers, ',')) {
		ret = -EOPNOTSUPP;
		goto out;
	}
	validation_copy = kstrdup(providers, GFP_KERNEL);
	if (!validation_copy) {
		ret = -ENOMEM;
		goto out;
	}
	if (!is_provider_list_valid(validation_copy)) {
		ret = -EINVAL;
		goto free_validation;
	}

	mutex_lock(&device->lifecycle_lock);
	if (!is_device_configurable(device))
		ret = -EBUSY;
	else
		strscpy(device->providers, providers, sizeof(device->providers));
	mutex_unlock(&device->lifecycle_lock);

free_validation:
	kfree(validation_copy);
out:
	kfree(candidate);
	return ret;
}

static int is_device_set_config_string(struct is_device *device,
				       const char *buf, size_t count,
				       char *target, size_t target_size,
				       bool identifier)
{
	char *candidate;
	char *value;
	int ret = 0;

	if (!count || count > target_size)
		return -ENAMETOOLONG;
	candidate = kstrndup(buf, count, GFP_KERNEL);
	if (!candidate)
		return -ENOMEM;
	value = strim(candidate);
	if (!value[0] || strlen(value) >= target_size ||
	    (identifier && !is_runtime_identifier_valid(value))) {
		ret = -EINVAL;
		goto out;
	}

	mutex_lock(&device->lifecycle_lock);
	if (!is_device_configurable(device))
		ret = -EBUSY;
	else
		strscpy(target, value, target_size);
	mutex_unlock(&device->lifecycle_lock);
out:
	kfree(candidate);
	return ret;
}

int is_device_set_provider_address(struct is_device *device, const char *buf,
				   size_t count)
{
	return is_device_set_config_string(device, buf, count,
		device->provider_address, sizeof(device->provider_address), false);
}

int is_device_set_rdma_device(struct is_device *device, const char *buf,
			      size_t count)
{
	return is_device_set_config_string(device, buf, count,
		device->rdma_device, sizeof(device->rdma_device), true);
}

int is_device_set_provider_key_id(struct is_device *device, const char *buf,
				  size_t count)
{
	return is_device_set_config_string(device, buf, count,
		device->provider_key_id, sizeof(device->provider_key_id), true);
}

int is_device_set_provider_port(struct is_device *device, const char *buf,
				size_t count)
{
	u16 port;
	int ret;

	(void)count;
	ret = kstrtou16(buf, 0, &port);
	if (ret)
		return ret;
	if (!port)
		return -ERANGE;
	mutex_lock(&device->lifecycle_lock);
	if (!is_device_configurable(device))
		ret = -EBUSY;
	else {
		device->provider_port = port;
		ret = 0;
	}
	mutex_unlock(&device->lifecycle_lock);
	return ret;
}

int is_device_set_rdma_port(struct is_device *device, const char *buf,
			    size_t count)
{
	u8 port;
	int ret;

	(void)count;
	ret = kstrtou8(buf, 0, &port);
	if (ret)
		return ret;
	if (!port)
		return -ERANGE;
	mutex_lock(&device->lifecycle_lock);
	if (!is_device_configurable(device))
		ret = -EBUSY;
	else {
		device->rdma_port = port;
		ret = 0;
	}
	mutex_unlock(&device->lifecycle_lock);
	return ret;
}

int is_device_set_rdma_numa_node(struct is_device *device, const char *buf,
				 size_t count)
{
	int numa_node;
	int ret;

	(void)count;
	ret = kstrtoint(buf, 0, &numa_node);
	if (ret)
		return ret;
	if (numa_node < NUMA_NO_NODE || numa_node > 65535)
		return -ERANGE;
	mutex_lock(&device->lifecycle_lock);
	if (!is_device_configurable(device))
		ret = -EBUSY;
	else {
		device->rdma_numa_node = numa_node;
		ret = 0;
	}
	mutex_unlock(&device->lifecycle_lock);
	return ret;
}

int is_device_set_provider_psk(struct is_device *device, const char *buf,
			       size_t count)
{
	u8 secret[IS_PSK_MAX_SIZE];
	char *candidate;
	char *encoded;
	size_t encoded_size;
	int ret = 0;

	if (!count || count > IS_PSK_MAX_SIZE * 2U + 1U)
		return -E2BIG;
	candidate = kstrndup(buf, count, GFP_KERNEL);
	if (!candidate)
		return -ENOMEM;
	encoded = strim(candidate);
	encoded_size = strlen(encoded);
	if (encoded_size < IS_PSK_MIN_SIZE * 2U ||
	    encoded_size > IS_PSK_MAX_SIZE * 2U || (encoded_size & 1U) ||
	    hex2bin(secret, encoded, encoded_size / 2U)) {
		ret = -EINVAL;
		goto out;
	}

	mutex_lock(&device->lifecycle_lock);
	if (!is_device_configurable(device)) {
		ret = -EBUSY;
	} else {
		memzero_explicit(device->provider_psk,
				 sizeof(device->provider_psk));
		memcpy(device->provider_psk, secret, encoded_size / 2U);
		device->provider_psk_size = encoded_size / 2U;
	}
	mutex_unlock(&device->lifecycle_lock);
out:
	memzero_explicit(secret, sizeof(secret));
	memzero_explicit(candidate, count);
	kfree(candidate);
	return ret;
}

int is_device_set_swap_priority(struct is_device *device, const char *buf,
				size_t count)
{
	int priority;
	int ret;

	ret = kstrtoint(buf, 0, &priority);
	if (ret)
		return ret;
	if (priority < 0 || priority > 32767)
		return -ERANGE;

	mutex_lock(&device->lifecycle_lock);
	if (!is_device_configurable(device))
		ret = -EBUSY;
	else {
		device->swap_priority = priority;
		ret = 0;
	}
	mutex_unlock(&device->lifecycle_lock);
	return ret;
}

const char *is_device_state_name(struct is_device *device)
{
	const char *name;

	mutex_lock(&device->lifecycle_lock);
	switch (device->state) {
	case IS_DEVICE_CREATED:
		name = "created";
		break;
	case IS_DEVICE_ACTIVE:
		name = "active";
		break;
	case IS_DEVICE_DRAINING:
		name = "draining";
		break;
	case IS_DEVICE_DRAINED:
		name = "drained";
		break;
	case IS_DEVICE_STOPPED:
		name = "stopped";
		break;
	default:
		name = "unknown";
		break;
	}
	mutex_unlock(&device->lifecycle_lock);
	return name;
}

const char *is_device_backing_state_name(struct is_device *device)
{
	return atomic_read(&device->backing_state) == IS_BACKING_DEGRADED ?
		"backing-degraded" : "healthy";
}

static bool is_backing_range_valid(struct is_device *device, sector_t sector,
				   unsigned int bytes)
{
	unsigned long flags;
	unsigned long sector_count = bytes >> 9;
	bool valid;

	if (!device->backing_invalid_bitmap || !bytes || (bytes & 511U))
		return false;
	spin_lock_irqsave(&device->backing_lock, flags);
	valid = find_next_bit(device->backing_invalid_bitmap,
		sector + sector_count, sector) >= sector + sector_count;
	spin_unlock_irqrestore(&device->backing_lock, flags);
	return valid;
}

static void is_mark_backing_invalid(struct is_device *device, sector_t sector,
				    unsigned int bytes)
{
	unsigned long flags;
	unsigned long sector_count = bytes >> 9;
	unsigned long index;
	u64 newly_invalid = 0;

	if (!device->backing_invalid_bitmap || !bytes || (bytes & 511U))
		return;
	spin_lock_irqsave(&device->backing_lock, flags);
	for (index = sector; index < sector + sector_count; index++) {
		if (!test_bit(index, device->backing_invalid_bitmap)) {
			__set_bit(index, device->backing_invalid_bitmap);
			newly_invalid++;
		}
	}
	spin_unlock_irqrestore(&device->backing_lock, flags);
	atomic64_add(newly_invalid, &device->backing_invalid_sectors);
}

static void is_degrade_backing(struct is_device *device, sector_t sector,
			       unsigned int bytes, int error)
{
	is_mark_backing_invalid(device, sector, bytes);
	atomic64_inc(&device->backing_failures_total);
	if (atomic_cmpxchg(&device->backing_state, IS_BACKING_HEALTHY,
			   IS_BACKING_DEGRADED) == IS_BACKING_HEALTHY)
		atomic64_inc(&device->backing_degraded_transitions_total);
	WRITE_ONCE(device->last_error, error > 0 ? error : EIO);
}

static bool is_backing_healthy(struct is_device *device)
{
	return atomic_read(&device->backing_state) == IS_BACKING_HEALTHY;
}

static bool is_backing_device_allowed(dev_t dev)
{
	return MAJOR(dev) != LOOP_MAJOR && MAJOR(dev) != is_major;
}

static int is_validate_backing_path(const char *path)
{
	struct inode *inode;
	struct path resolved;
	int ret;

	ret = kern_path(path, LOOKUP_FOLLOW, &resolved);
	if (ret)
		return ret;

	inode = d_inode(resolved.dentry);
	if (!S_ISBLK(inode->i_mode)) {
		ret = -ENOTBLK;
	} else if (!is_backing_device_allowed(inode->i_rdev)) {
		ret = -EINVAL;
	} else {
		ret = 0;
	}
	path_put(&resolved);
	return ret;
}

int is_device_set_backing_store(struct is_device *device, const char *buf,
				size_t count)
{
	char *candidate;
	char *path;
	int ret;

	if (!count || count >= PATH_MAX)
		return -ENAMETOOLONG;

	candidate = kstrndup(buf, count, GFP_KERNEL);
	if (!candidate)
		return -ENOMEM;
	path = strim(candidate);
	if (!path[0]) {
		ret = -EINVAL;
		goto out;
	}

	mutex_lock(&device->lifecycle_lock);
	if (!is_device_configurable(device)) {
		ret = -EBUSY;
		goto unlock;
	}

	ret = is_validate_backing_path(path);
	if (!ret)
		strscpy(device->backing_path, path,
			sizeof(device->backing_path));

unlock:
	mutex_unlock(&device->lifecycle_lock);
out:
	kfree(candidate);
	return ret;
}

int is_device_set_capacity(struct is_device *device, const char *buf,
			   size_t count)
{
	sector_t sectors;
	u64 bytes;
	int ret;

	ret = kstrtoull(buf, 0, &bytes);
	if (ret)
		return ret;
	if (!bytes || bytes % IS_SECTOR_SIZE)
		return -EINVAL;

	sectors = (sector_t)(bytes / IS_SECTOR_SIZE);
	if ((u64)sectors != bytes / IS_SECTOR_SIZE)
		return -EOVERFLOW;

	mutex_lock(&device->lifecycle_lock);
	if (!is_device_configurable(device)) {
		ret = -EBUSY;
	} else {
		device->capacity_bytes = bytes;
		device->capacity_sectors = sectors;
		ret = 0;
	}
	mutex_unlock(&device->lifecycle_lock);
	return ret;
}

static int is_open_backing_store(struct is_device *device)
{
#ifdef INFINISWAP_HAVE_BDEV_HANDLE
	device->backing_handle = bdev_open_by_path(device->backing_path,
						   IS_BACKING_MODE, device,
						   NULL);
	if (IS_ERR(device->backing_handle)) {
		int ret = PTR_ERR(device->backing_handle);

		device->backing_handle = NULL;
		return ret;
	}
	device->backing_bdev = device->backing_handle->bdev;
#else
	device->backing_bdev = blkdev_get_by_path(device->backing_path,
						  IS_BACKING_MODE, device);
	if (IS_ERR(device->backing_bdev)) {
		int ret = PTR_ERR(device->backing_bdev);

		device->backing_bdev = NULL;
		return ret;
	}
#endif
	return 0;
}

static void is_close_backing_store(struct is_device *device)
{
	if (!device->backing_bdev)
		return;

#ifdef INFINISWAP_HAVE_BDEV_HANDLE
	bdev_release(device->backing_handle);
	device->backing_handle = NULL;
#else
	blkdev_put(device->backing_bdev, IS_BACKING_MODE);
#endif
	device->backing_bdev = NULL;
}

static void is_finish_inflight(struct is_device *device)
{
	if (atomic_dec_and_test(&device->inflight))
		wake_up_all(&device->drain_wait);
}

static void is_end_request(struct is_request_ctx *ctx, blk_status_t status)
{
	struct is_device *device = ctx->device;
	struct request *request = ctx->request;

	if (status != BLK_STS_OK)
		atomic_cmpxchg(&ctx->status, BLK_STS_OK, (__force int)status);

	if (!atomic_dec_and_test(&ctx->pending_bios))
		return;

	status = (__force blk_status_t)atomic_read(&ctx->status);
	if (atomic_read(&ctx->backing_io_failed))
		is_degrade_backing(device, blk_rq_pos(request),
			blk_rq_bytes(request), EIO);
	else if (req_op(request) == REQ_OP_WRITE && status == BLK_STS_OK)
		atomic64_inc(&device->local_only_writes_total);
	blk_mq_end_request(request, status);
	is_finish_inflight(device);
}

static void is_backing_end_io(struct bio *bio)
{
	struct is_request_ctx *ctx = bio->bi_private;
	blk_status_t status = bio->bi_status;

	if (status != BLK_STS_OK)
		atomic_set(&ctx->backing_io_failed, 1);
	bio_put(bio);
	is_end_request(ctx, status);
}

static struct bio *is_clone_bio(struct is_device *device, struct bio *source)
{
	struct bio *clone;

#ifdef INFINISWAP_HAVE_BIO_ALLOC_CLONE
	clone = bio_alloc_clone(device->backing_bdev, source, GFP_NOIO,
				&device->bio_set);
#else
	clone = bio_clone_fast(source, GFP_NOIO, &device->bio_set);
	if (clone)
		bio_set_dev(clone, device->backing_bdev);
#endif
	return clone;
}

struct is_remote_request {
	struct is_device *device;
	struct request *request;
	sector_t sector;
	unsigned int bytes;
	unsigned int command_flags;
	spinlock_t policy_lock;
	refcount_t references;
	struct is_io_policy policy;
	atomic_t pending_bios;
	atomic_t local_status;
	unsigned int backing_retries;
	struct work_struct local_work;
	struct is_rdma_io rdma_io;
	struct page *owned_pages[IS_RDMA_MAX_SEGMENTS];
	unsigned int owned_page_count;
};

static void is_remote_request_put(struct is_remote_request *remote)
{
	unsigned int index;

	if (!refcount_dec_and_test(&remote->references))
		return;
	WARN_ON_ONCE(!remote->policy.request_complete);
	WARN_ON_ONCE(!is_io_policy_releasable(&remote->policy));
	for (index = 0; index < remote->owned_page_count; index++)
		__free_page(remote->owned_pages[index]);
	is_finish_inflight(remote->device);
	kfree(remote);
}

static void is_remote_local_complete(struct is_remote_request *remote,
				     blk_status_t status);
static void is_remote_path_complete(struct is_remote_request *remote,
				    bool remote_path,
				    blk_status_t status, bool cancelled);

static void is_remote_apply_action(struct is_remote_request *remote,
				   enum is_io_action action,
				   blk_status_t completion_status)
{
	if (action & IS_IO_BACKING_DEGRADED)
		is_degrade_backing(remote->device, remote->sector,
			remote->bytes, EIO);
	if (action & IS_IO_MARK_LOCAL_ONLY)
		atomic64_inc(&remote->device->local_only_writes_total);
	if (action & IS_IO_SUBMIT_LOCAL) {
		refcount_inc(&remote->references);
		if (!queue_work(remote->device->ordered_backing_wq,
				&remote->local_work))
			is_remote_path_complete(remote, false, BLK_STS_IOERR,
				false);
	}
	if (action & IS_IO_COMPLETE_SUCCESS)
		blk_mq_end_request(remote->request, BLK_STS_OK);
	else if (action & IS_IO_COMPLETE_ERROR)
		blk_mq_end_request(remote->request, completion_status);
}

static void is_remote_path_complete(struct is_remote_request *remote,
				    bool remote_path,
				    blk_status_t status, bool cancelled)
{
	enum is_io_action action;
	blk_status_t completion_status = status;
	unsigned long flags;

	spin_lock_irqsave(&remote->policy_lock, flags);
	if (remote_path) {
		if (cancelled)
			action = is_io_policy_cancel_remote(&remote->policy,
				remote->rdma_io.generation, (__force int)status);
		else
			action = is_io_policy_remote_complete(&remote->policy,
				remote->rdma_io.generation, (__force int)status);
	} else {
		action = is_io_policy_local_complete(&remote->policy,
			(__force int)status);
	}
	if (action & IS_IO_COMPLETE_ERROR) {
		if (remote->policy.local_done)
			completion_status = (__force blk_status_t)
				remote->policy.local_status;
		else
			completion_status = (__force blk_status_t)
				remote->policy.remote_status;
	}
	spin_unlock_irqrestore(&remote->policy_lock, flags);

	is_remote_apply_action(remote, action, completion_status);
	is_remote_request_put(remote);
}

static int is_copy_owned_pages_to_request(struct is_remote_request *remote)
{
	struct req_iterator iterator;
	struct bio_vec segment;
	unsigned int index = 0;

	rq_for_each_segment(segment, remote->request, iterator) {
		void *destination;
		void *source;

		if (index >= remote->owned_page_count ||
		    segment.bv_len != remote->rdma_io.segments[index].length)
			return -EIO;
		destination = kmap_local_page(segment.bv_page);
		source = kmap_local_page(remote->owned_pages[index]);
		memcpy((u8 *)destination + segment.bv_offset, source,
			segment.bv_len);
		kunmap_local(source);
		kunmap_local(destination);
		index++;
	}
	return index == remote->owned_page_count ? 0 : -EIO;
}

static void is_remote_rdma_complete(void *context, u64 generation, int status,
				    bool cancelled)
{
	struct is_remote_request *remote = context;

	if (generation != remote->rdma_io.generation) {
		is_remote_request_put(remote);
		return;
	}
	if (!status && !cancelled && !remote->rdma_io.write)
		status = is_copy_owned_pages_to_request(remote);
	is_remote_path_complete(remote, true,
		status ? errno_to_blk_status(status) : BLK_STS_OK, cancelled);
}

static void is_remote_transport_release(void *context)
{
	is_remote_request_put(context);
}

static void is_remote_local_complete(struct is_remote_request *remote,
				     blk_status_t status)
{
	if (status != BLK_STS_OK)
		atomic_cmpxchg(&remote->local_status, BLK_STS_OK,
			       (__force int)status);
	if (!atomic_dec_and_test(&remote->pending_bios))
		return;
	status = (__force blk_status_t)atomic_read(&remote->local_status);
	is_remote_path_complete(remote, false, status, false);
}

static void is_remote_backing_end_io(struct bio *bio)
{
	struct is_remote_request *remote = bio->bi_private;
	blk_status_t status = bio->bi_status;

	bio_put(bio);
	if (status != BLK_STS_OK && remote->rdma_io.write &&
	    remote->policy.kind == IS_IO_POLICY_REMOTE_FIRST_WRITE &&
	    remote->backing_retries < IS_BACKING_RETRY_LIMIT) {
		remote->backing_retries++;
		atomic64_inc(&remote->device->backing_retries_total);
		if (queue_work(remote->device->ordered_backing_wq,
			       &remote->local_work))
			return;
	}
	is_remote_local_complete(remote, status);
}

static struct bio *is_alloc_owned_bio(struct is_remote_request *remote)
{
	struct bio *bio;

#ifdef INFINISWAP_HAVE_BIO_ALLOC_CLONE
	bio = bio_alloc_bioset(remote->device->backing_bdev,
		remote->owned_page_count, remote->command_flags, GFP_NOIO,
		&remote->device->bio_set);
#else
	bio = bio_alloc_bioset(GFP_NOIO, remote->owned_page_count,
		&remote->device->bio_set);
	if (bio) {
		bio_set_dev(bio, remote->device->backing_bdev);
		bio->bi_opf = remote->command_flags;
	}
#endif
	if (bio)
		bio->bi_iter.bi_sector = remote->sector;
	return bio;
}

static struct bio *is_build_owned_bio(struct is_remote_request *remote)
{
	struct bio *bio = is_alloc_owned_bio(remote);
	unsigned int index;

	if (!bio)
		return NULL;
	for (index = 0; index < remote->owned_page_count; index++) {
		unsigned int length = remote->rdma_io.segments[index].length;

		if (bio_add_page(bio, remote->owned_pages[index], length, 0) !=
		    length) {
			bio_put(bio);
			return NULL;
		}
	}
	bio->bi_private = remote;
	bio->bi_end_io = is_remote_backing_end_io;
	return bio;
}

static int is_submit_owned_backing_write(struct is_remote_request *remote)
{
	struct bio *bio = is_build_owned_bio(remote);

	if (!bio)
		return -ENOMEM;
	submit_bio_noacct(bio);
	return 0;
}

static int is_prepare_owned_pages(struct is_remote_request *remote, bool copy)
{
	struct req_iterator iterator;
	struct bio_vec segment;
	unsigned int copied = 0;

	rq_for_each_segment(segment, remote->request, iterator) {
		struct page *page;

		if (remote->owned_page_count == IS_RDMA_MAX_SEGMENTS ||
		    segment.bv_len > PAGE_SIZE)
			return -E2BIG;
		page = remote->device->rdma_numa_node == NUMA_NO_NODE ?
			alloc_page(GFP_NOIO) :
			alloc_pages_node(remote->device->rdma_numa_node,
				GFP_NOIO, 0);
		if (!page)
			return -ENOMEM;
		remote->owned_pages[remote->owned_page_count] = page;
		if (copy) {
			void *destination = kmap_local_page(page);
			void *source = kmap_local_page(segment.bv_page);

			memcpy(destination, (u8 *)source + segment.bv_offset,
				segment.bv_len);
			kunmap_local(source);
			kunmap_local(destination);
		}
		remote->rdma_io.segments[remote->owned_page_count].page = page;
		remote->rdma_io.segments[remote->owned_page_count].offset = 0;
		remote->rdma_io.segments[remote->owned_page_count].length =
			segment.bv_len;
		remote->owned_page_count++;
		copied += segment.bv_len;
	}
	remote->rdma_io.segment_count = remote->owned_page_count;
	return remote->owned_page_count &&
		copied == blk_rq_bytes(remote->request) ? 0 : -EIO;
}

static struct is_remote_request *is_alloc_remote_request(
	struct is_device *device, struct request *request)
{
	struct is_remote_request *remote;

	if (device->rdma_numa_node == NUMA_NO_NODE)
		remote = kzalloc(sizeof(*remote), GFP_NOIO);
	else
		remote = kzalloc_node(sizeof(*remote), GFP_NOIO,
			device->rdma_numa_node);

	if (!remote)
		return NULL;
	remote->device = device;
	remote->request = request;
	remote->sector = blk_rq_pos(request);
	remote->bytes = blk_rq_bytes(request);
	remote->command_flags = request->cmd_flags;
	spin_lock_init(&remote->policy_lock);
	remote->rdma_io.sector = remote->sector;
	remote->rdma_io.bytes = remote->bytes;
	remote->rdma_io.generation = atomic64_inc_return(
		&device->next_io_generation);
	remote->rdma_io.context = remote;
	remote->rdma_io.complete = is_remote_rdma_complete;
	remote->rdma_io.release = is_remote_transport_release;
	return remote;
}

static void is_submit_remote_local_work(struct work_struct *work)
{
	struct is_remote_request *remote = container_of(
		work, struct is_remote_request, local_work);
	struct bio *source;
	blk_status_t status = BLK_STS_OK;

	if (remote->rdma_io.write) {
		if (is_submit_owned_backing_write(remote))
			is_remote_local_complete(remote, BLK_STS_RESOURCE);
		return;
	}
	atomic_set(&remote->local_status, BLK_STS_OK);
	atomic_set(&remote->pending_bios, 1);
	if (!is_backing_range_valid(remote->device, remote->sector,
				    remote->bytes)) {
		is_remote_local_complete(remote, BLK_STS_IOERR);
		return;
	}

	for (source = remote->request->bio; source; source = source->bi_next) {
		struct bio *clone = is_clone_bio(remote->device, source);

		if (!clone) {
			status = BLK_STS_RESOURCE;
			break;
		}
		clone->bi_private = remote;
		clone->bi_end_io = is_remote_backing_end_io;
		atomic_inc(&remote->pending_bios);
		submit_bio_noacct(clone);
	}
	if (!remote->request->bio)
		status = BLK_STS_IOERR;
	is_remote_local_complete(remote, status);
}

static bool is_dispatch_remote_read(struct is_device *device,
				    struct request *request)
{
	struct is_remote_request *remote =
		is_alloc_remote_request(device, request);
	u64 generation;
	int ret;

	if (!remote)
		return false;
	INIT_WORK(&remote->local_work, is_submit_remote_local_work);
	ret = is_prepare_owned_pages(remote, false);
	if (ret)
		goto free_remote;
	remote->rdma_io.write = false;
	generation = remote->rdma_io.generation;
	is_io_policy_init_remote_read(&remote->policy, generation);
	refcount_set(&remote->references, 3);
	ret = is_rdma_submit(device, &remote->rdma_io);
	if (ret) {
		is_remote_transport_release(remote);
		is_remote_rdma_complete(remote, generation, ret, false);
	}
	is_remote_request_put(remote);
	return true;

free_remote:
	while (remote->owned_page_count)
		__free_page(remote->owned_pages[--remote->owned_page_count]);
	kfree(remote);
	return false;
}

static bool is_dispatch_remote_write(struct is_device *device,
				     struct request *request)
{
	struct is_remote_request *remote =
		is_alloc_remote_request(device, request);
	struct bio *bio;
	enum is_io_policy_kind kind;
	u64 generation;
	unsigned int index;
	int ret;

	if (!remote)
		return false;
	INIT_WORK(&remote->local_work, is_submit_remote_local_work);
	ret = is_prepare_owned_pages(remote, true);
	if (ret)
		goto free_remote;
	bio = is_build_owned_bio(remote);
	if (!bio)
		goto free_remote;
	remote->rdma_io.write = true;
	generation = remote->rdma_io.generation;
	kind = device->acknowledgement_policy ==
		IS_ACKNOWLEDGEMENT_POLICY_REMOTE_FIRST ?
		IS_IO_POLICY_REMOTE_FIRST_WRITE : IS_IO_POLICY_STRICT_WRITE;
	is_io_policy_init_write(&remote->policy, kind, generation);
	refcount_set(&remote->references, 4);
	atomic_set(&remote->local_status, BLK_STS_OK);
	atomic_set(&remote->pending_bios, 1);

	/* Both policies require the Backing Store write to be submitted first. */
	submit_bio_noacct(bio);
	ret = is_rdma_submit(device, &remote->rdma_io);
	if (ret) {
		is_remote_transport_release(remote);
		is_remote_rdma_complete(remote, generation, ret, false);
	}
	is_remote_request_put(remote);
	return true;

free_remote:
	for (index = 0; index < remote->owned_page_count; index++)
		__free_page(remote->owned_pages[index]);
	kfree(remote);
	return false;
}

static bool is_dispatch_remote(struct is_device *device,
			       struct request *request)
{
	unsigned int bytes = blk_rq_bytes(request);
	sector_t sector = blk_rq_pos(request);
	bool write = req_op(request) == REQ_OP_WRITE;

	is_rdma_note_activity(device, sector, bytes, write);
	if (write) {
		if (!is_rdma_range_mapped(device, sector, bytes))
			return false;
		return is_dispatch_remote_write(device, request);
	}
	if (!is_rdma_range_valid(device, sector, bytes))
		return false;
	return is_dispatch_remote_read(device, request);
}

static void is_complete_accepted_request(struct is_device *device,
					 struct request *request,
					 blk_status_t status)
{
	blk_mq_end_request(request, status);
	is_finish_inflight(device);
}

static void is_submit_to_backing_store(struct is_device *device,
				       struct request *request)
{
	struct is_request_ctx *ctx = blk_mq_rq_to_pdu(request);
	struct bio *source;
	blk_status_t status = BLK_STS_OK;

	atomic_set(&ctx->status, BLK_STS_OK);
	atomic_set(&ctx->backing_io_failed, 0);
	/* The sentinel keeps the request alive while clones are submitted. */
	atomic_set(&ctx->pending_bios, 1);

	for (source = request->bio; source; source = source->bi_next) {
		struct bio *clone = is_clone_bio(device, source);

		if (!clone) {
			status = BLK_STS_RESOURCE;
			break;
		}
		clone->bi_private = ctx;
		clone->bi_end_io = is_backing_end_io;
		atomic_inc(&ctx->pending_bios);
		submit_bio_noacct(clone);
	}

	if (!request->bio)
		status = BLK_STS_IOERR;
	is_end_request(ctx, status);
}

static bool is_accept_request(struct is_device *device,
			      struct request *request)
{
	unsigned long flags;
	bool write = req_op(request) == REQ_OP_WRITE ||
		req_op(request) == REQ_OP_FLUSH;
	bool rejected_degraded = false;
	bool accepted;

	spin_lock_irqsave(&device->io_lock, flags);
	accepted = device->accepting_io;
	if (accepted && write && !is_backing_healthy(device)) {
		accepted = false;
		rejected_degraded = true;
	}
	if (accepted)
		atomic_inc(&device->inflight);
	spin_unlock_irqrestore(&device->io_lock, flags);
	if (rejected_degraded)
		atomic64_inc(&device->rejected_writes_total);
	return accepted;
}

static void is_issue_flush(struct is_device *device, struct request *request)
{
	blk_status_t status = errno_to_blk_status(
		blkdev_issue_flush(device->backing_bdev));

	if (status != BLK_STS_OK)
		is_degrade_backing(device, 0, 0, EIO);
	is_complete_accepted_request(device, request, status);
}

static void is_dispatch_backing_work(struct work_struct *work)
{
	struct is_request_ctx *ctx = container_of(work, struct is_request_ctx,
						  work);
	struct is_device *device = ctx->device;
	struct request *request = ctx->request;

	if (req_op(request) == REQ_OP_FLUSH) {
		is_issue_flush(device, request);
	} else if (is_dispatch_remote(device, request)) {
		return;
	} else if (req_op(request) == REQ_OP_READ &&
		   !is_backing_range_valid(device, blk_rq_pos(request),
			blk_rq_bytes(request))) {
		is_complete_accepted_request(device, request, BLK_STS_IOERR);
	} else {
		is_submit_to_backing_store(device, request);
	}
}

static blk_status_t is_queue_rq(struct blk_mq_hw_ctx *hctx,
				const struct blk_mq_queue_data *bd)
{
	struct is_device *device = hctx->driver_data;
	struct request *request = bd->rq;
	struct is_request_ctx *ctx = blk_mq_rq_to_pdu(request);

	blk_mq_start_request(request);
	if (req_op(request) != REQ_OP_READ &&
	    req_op(request) != REQ_OP_WRITE &&
	    req_op(request) != REQ_OP_FLUSH) {
		blk_mq_end_request(request, BLK_STS_NOTSUPP);
		return BLK_STS_OK;
	}
	if (!is_accept_request(device, request)) {
		blk_mq_end_request(request, BLK_STS_IOERR);
		return BLK_STS_OK;
	}

	/*
	 * Ordered dispatch keeps backing bios out of the caller's blk_plug and
	 * submits every write before a later flush reaches the Backing Store.
	 */
	ctx->device = device;
	ctx->request = request;
	if (WARN_ON_ONCE(!queue_work(device->ordered_backing_wq, &ctx->work)))
		is_complete_accepted_request(device, request, BLK_STS_IOERR);
	return BLK_STS_OK;
}

static int is_init_request(struct blk_mq_tag_set *set, struct request *request,
			   unsigned int hctx_idx, unsigned int numa_node)
{
	struct is_request_ctx *ctx = blk_mq_rq_to_pdu(request);

	INIT_WORK(&ctx->work, is_dispatch_backing_work);
	return 0;
}

static int is_init_hctx(struct blk_mq_hw_ctx *hctx, void *data,
			unsigned int index)
{
	hctx->driver_data = data;
	return 0;
}

static const struct blk_mq_ops is_mq_ops = {
	.queue_rq = is_queue_rq,
	.init_request = is_init_request,
	.init_hctx = is_init_hctx,
};

#ifdef INFINISWAP_HAVE_BDEV_HANDLE
static int is_open(struct gendisk *disk, blk_mode_t mode)
{
	struct is_device *device = disk->private_data;
#else
static int is_open(struct block_device *bdev, fmode_t mode)
{
	struct is_device *device = bdev->bd_disk->private_data;
#endif
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&device->io_lock, flags);
	if (!device->accepting_opens)
		ret = -ENODEV;
	else
		atomic_inc(&device->openers);
	spin_unlock_irqrestore(&device->io_lock, flags);
	return ret;
}

#ifdef INFINISWAP_HAVE_BDEV_HANDLE
static void is_release(struct gendisk *disk)
#else
static void is_release(struct gendisk *disk, fmode_t mode)
#endif
{
	struct is_device *device = disk->private_data;

	WARN_ON(atomic_dec_return(&device->openers) < 0);
}

#ifdef INFINISWAP_HAVE_BDEV_HANDLE
static int is_ioctl(struct block_device *bdev, blk_mode_t mode,
		    unsigned int command, unsigned long argument)
#else
static int is_ioctl(struct block_device *bdev, fmode_t mode,
		    unsigned int command, unsigned long argument)
#endif
{
	return -ENOTTY;
}

static const struct block_device_operations is_block_ops = {
	.owner = THIS_MODULE,
	.open = is_open,
	.release = is_release,
	.ioctl = is_ioctl,
};

static void is_configure_queue(struct is_device *device)
{
	struct request_queue *backing_queue = bdev_get_queue(device->backing_bdev);
	struct request_queue *queue = device->disk->queue;

	blk_queue_logical_block_size(queue,
				     bdev_logical_block_size(device->backing_bdev));
	blk_queue_physical_block_size(queue,
				      bdev_physical_block_size(device->backing_bdev));
	blk_queue_max_hw_sectors(queue,
		min_t(unsigned int, queue_max_hw_sectors(backing_queue),
		      IS_RDMA_MAX_SEGMENTS * (PAGE_SIZE >> 9)));
	blk_queue_max_segments(queue,
		min_t(unsigned int, queue_max_segments(backing_queue),
		      IS_RDMA_MAX_SEGMENTS));
	blk_queue_max_segment_size(queue,
		min_t(unsigned int, queue_max_segment_size(backing_queue), PAGE_SIZE));
	blk_queue_write_cache(queue,
			      test_bit(QUEUE_FLAG_WC,
				       &backing_queue->queue_flags),
			      test_bit(QUEUE_FLAG_FUA,
				       &backing_queue->queue_flags));
	blk_queue_max_discard_sectors(queue, 0);
	blk_queue_max_write_zeroes_sectors(queue, 0);
	blk_queue_flag_set(QUEUE_FLAG_NONROT, queue);
	blk_queue_flag_clear(QUEUE_FLAG_ADD_RANDOM, queue);
}

static void is_release_resources(struct is_device *device)
{
	is_rdma_stop(device);
	if (device->disk && device->disk_added) {
		del_gendisk(device->disk);
		device->disk_added = false;
	}
	if (device->ordered_backing_wq) {
		destroy_workqueue(device->ordered_backing_wq);
		device->ordered_backing_wq = NULL;
	}
	if (device->disk) {
#ifdef INFINISWAP_HAVE_BLK_CLEANUP_DISK
		blk_cleanup_disk(device->disk);
#else
		put_disk(device->disk);
#endif
		device->disk = NULL;
	}
	if (device->tag_set_allocated) {
		blk_mq_free_tag_set(&device->tag_set);
		device->tag_set_allocated = false;
	}
	if (device->bioset_initialized) {
		bioset_exit(&device->bio_set);
		device->bioset_initialized = false;
	}
	kvfree(device->backing_invalid_bitmap);
	device->backing_invalid_bitmap = NULL;
	atomic64_set(&device->backing_invalid_sectors, 0);
	if (device->minor >= 0) {
		is_minor_free(device->minor);
		device->minor = -1;
	}
	is_close_backing_store(device);
}

static int is_validate_open_backing_store(struct is_device *device)
{
	unsigned int logical_block_size;

	if (!is_backing_device_allowed(device->backing_bdev->bd_dev))
		return -EINVAL;
	if (bdev_read_only(device->backing_bdev))
		return -EROFS;
	if (device->capacity_sectors > bdev_nr_sectors(device->backing_bdev))
		return -ENOSPC;

	logical_block_size = bdev_logical_block_size(device->backing_bdev);
	if (device->capacity_bytes % logical_block_size)
		return -EINVAL;
	return 0;
}

int is_device_activate(struct is_device *device)
{
	enum is_device_state previous_state;
	int ret;

	mutex_lock(&device->lifecycle_lock);
	if (!is_device_configurable(device)) {
		ret = device->state == IS_DEVICE_ACTIVE ? -EALREADY : -EINVAL;
		goto out;
	}
	if (!is_backing_healthy(device)) {
		ret = -EUCLEAN;
		goto out;
	}
	if (device->mode != IS_DEVICE_MODE_BACKED ||
	    !device->backing_path[0] || !device->capacity_sectors ||
	    !device->consumer_id[0] || !device->providers[0] ||
	    device->swap_priority < 0) {
		ret = -EINVAL;
		goto out;
	}
	if (device->acknowledgement_policy ==
		    IS_ACKNOWLEDGEMENT_POLICY_REMOTE_FIRST &&
	    ((device->capacity_bytes % IS_REMOTE_CHUNK_BYTES) ||
	     device->capacity_bytes >
		IS_REMOTE_CHUNK_BYTES * IS_MAX_REMOTE_CHUNKS ||
	     !device->provider_address[0] || !device->provider_port ||
	     !device->rdma_device[0] || !device->rdma_port ||
	     !device->provider_key_id[0] ||
	     device->provider_psk_size < IS_PSK_MIN_SIZE)) {
		ret = -EINVAL;
		goto out;
	}
	previous_state = device->state;

	ret = is_open_backing_store(device);
	if (ret)
		goto out;
	ret = is_validate_open_backing_store(device);
	if (ret)
		goto release_resources;

	ret = bioset_init(&device->bio_set, IS_BIO_POOL_SIZE, 0,
			  BIOSET_NEED_RESCUER | BIOSET_NEED_BVECS);
	if (ret)
		goto release_resources;
	device->bioset_initialized = true;

	device->backing_invalid_bitmap = kvcalloc(
		BITS_TO_LONGS(device->capacity_sectors), sizeof(unsigned long),
		GFP_KERNEL);
	if (!device->backing_invalid_bitmap) {
		ret = -ENOMEM;
		goto release_resources;
	}
	atomic64_set(&device->backing_invalid_sectors, 0);

	device->ordered_backing_wq = alloc_ordered_workqueue("infiniswap-io",
							WQ_MEM_RECLAIM);
	if (!device->ordered_backing_wq) {
		ret = -ENOMEM;
		goto release_resources;
	}

	ret = is_rdma_start(device);
	if (ret) {
		WRITE_ONCE(device->last_error, -ret);
		ret = 0;
	}

	device->minor = is_minor_alloc();
	if (device->minor < 0) {
		ret = device->minor;
		device->minor = -1;
		goto release_resources;
	}

	memset(&device->tag_set, 0, sizeof(device->tag_set));
	device->tag_set.ops = &is_mq_ops;
	device->tag_set.nr_hw_queues = max_t(unsigned int, 1,
					     num_online_cpus());
	device->tag_set.queue_depth = IS_QUEUE_DEPTH;
	device->tag_set.numa_node = device->rdma_numa_node;
	device->tag_set.cmd_size = sizeof(struct is_request_ctx);
	device->tag_set.flags = BLK_MQ_F_SHOULD_MERGE |
				BLK_MQ_F_BLOCKING | BLK_MQ_F_STACKING;
	device->tag_set.driver_data = device;

	ret = blk_mq_alloc_tag_set(&device->tag_set);
	if (ret)
		goto release_resources;
	device->tag_set_allocated = true;

	device->disk = blk_mq_alloc_disk(&device->tag_set, device);
	if (IS_ERR(device->disk)) {
		ret = PTR_ERR(device->disk);
		device->disk = NULL;
		goto release_resources;
	}
	device->disk->major = is_major;
	device->disk->first_minor = device->minor;
	device->disk->minors = 1;
	device->disk->fops = &is_block_ops;
	device->disk->private_data = device;
	strscpy(device->disk->disk_name, device->name, DISK_NAME_LEN);
	set_capacity(device->disk, device->capacity_sectors);
	is_configure_queue(device);

	is_set_io_state(device, IS_DEVICE_ACTIVE, true);
	ret = add_disk(device->disk);
	if (ret) {
		is_set_io_state(device, previous_state, false);
		goto release_resources;
	}
	device->disk_added = true;
	pr_info(IS_DRIVER_NAME ": activated %s on %s (%llu bytes)\n",
		device->name, device->backing_path, device->capacity_bytes);
	goto out;

release_resources:
	is_set_io_state(device, previous_state, false);
	is_release_resources(device);
out:
	mutex_unlock(&device->lifecycle_lock);
	return ret;
}

static int is_device_drain_locked(struct is_device *device)
{
	unsigned long flags;

	if (device->state == IS_DEVICE_DRAINED)
		return 0;
	if (device->state != IS_DEVICE_ACTIVE)
		return -EINVAL;

	spin_lock_irqsave(&device->io_lock, flags);
	if (atomic_read(&device->openers)) {
		spin_unlock_irqrestore(&device->io_lock, flags);
		return -EBUSY;
	}
	device->accepting_opens = false;
	device->state = IS_DEVICE_DRAINING;
	spin_unlock_irqrestore(&device->io_lock, flags);

	/* del_gendisk may submit buffered writeback before it drains the queue. */
	if (device->disk_added) {
		del_gendisk(device->disk);
		device->disk_added = false;
	}
	is_set_io_state(device, IS_DEVICE_DRAINED, false);
	wait_event(device->drain_wait, !atomic_read(&device->inflight));
	pr_info(IS_DRIVER_NAME ": drained %s\n", device->name);
	return 0;
}

int is_device_drain(struct is_device *device)
{
	int ret;

	mutex_lock(&device->lifecycle_lock);
	ret = is_device_drain_locked(device);
	mutex_unlock(&device->lifecycle_lock);
	return ret;
}

int is_device_stop(struct is_device *device)
{
	int ret = 0;

	mutex_lock(&device->lifecycle_lock);
	if (device->state == IS_DEVICE_STOPPED)
		goto out;
	if (device->state == IS_DEVICE_CREATED) {
		is_set_io_state(device, IS_DEVICE_STOPPED, false);
		goto out;
	}
	if (device->state == IS_DEVICE_ACTIVE) {
		ret = is_device_drain_locked(device);
		if (ret)
			goto out;
	}
	if (device->state != IS_DEVICE_DRAINED) {
		ret = -EBUSY;
		goto out;
	}

	is_release_resources(device);
	is_set_io_state(device, IS_DEVICE_STOPPED, false);
	pr_info(IS_DRIVER_NAME ": stopped %s\n", device->name);
out:
	mutex_unlock(&device->lifecycle_lock);
	return ret;
}
