// SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause
/*
 * Copyright 2017 University of Michigan, Ann Arbor
 * Copyright 2014 Oren Kishon
 * Copyright (c) 2013 Mellanox Technologies. All rights reserved.
 */
#include <linux/fs.h>
#include <linux/major.h>
#include <linux/namei.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "infiniswap.h"

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
	init_waitqueue_head(&device->drain_wait);
	atomic_set(&device->openers, 0);
	atomic_set(&device->inflight, 0);
	device->mode = IS_DEVICE_MODE_UNSET;
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
	blk_mq_end_request(request, status);
	is_finish_inflight(device);
}

static void is_backing_end_io(struct bio *bio)
{
	struct is_request_ctx *ctx = bio->bi_private;
	blk_status_t status = bio->bi_status;

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

static bool is_accept_request(struct is_device *device)
{
	unsigned long flags;
	bool accepted;

	spin_lock_irqsave(&device->io_lock, flags);
	accepted = device->accepting_io;
	if (accepted)
		atomic_inc(&device->inflight);
	spin_unlock_irqrestore(&device->io_lock, flags);
	return accepted;
}

static void is_issue_flush(struct is_device *device, struct request *request)
{
	blk_status_t status = errno_to_blk_status(
		blkdev_issue_flush(device->backing_bdev));

	is_complete_accepted_request(device, request, status);
}

static void is_dispatch_backing_work(struct work_struct *work)
{
	struct is_request_ctx *ctx = container_of(work, struct is_request_ctx,
						  work);
	struct is_device *device = ctx->device;
	struct request *request = ctx->request;

	if (req_op(request) == REQ_OP_FLUSH)
		is_issue_flush(device, request);
	else
		is_submit_to_backing_store(device, request);
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
	if (!is_accept_request(device)) {
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
	blk_queue_max_hw_sectors(queue, queue_max_hw_sectors(backing_queue));
	blk_queue_max_segments(queue, queue_max_segments(backing_queue));
	blk_queue_max_segment_size(queue,
				   queue_max_segment_size(backing_queue));
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
	if (device->mode != IS_DEVICE_MODE_BACKED ||
	    !device->backing_path[0] || !device->capacity_sectors) {
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
			  BIOSET_NEED_RESCUER);
	if (ret)
		goto release_resources;
	device->bioset_initialized = true;

	device->ordered_backing_wq = alloc_ordered_workqueue("infiniswap-io",
							WQ_MEM_RECLAIM);
	if (!device->ordered_backing_wq) {
		ret = -ENOMEM;
		goto release_resources;
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
	device->tag_set.numa_node = NUMA_NO_NODE;
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
