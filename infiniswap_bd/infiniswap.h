// SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause
/*
 * Copyright 2017 University of Michigan, Ann Arbor
 * Copyright (c) 2013 Mellanox Technologies. All rights reserved.
 */
#ifndef INFINISWAP_H
#define INFINISWAP_H

#include <linux/atomic.h>
#include <linux/bio.h>
#include <linux/blk-mq.h>
#include <linux/blkdev.h>
#include <linux/configfs.h>
#include <linux/limits.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/wait.h>

#ifdef INFINISWAP_HAVE_LINUX_GENHD_H
#include <linux/genhd.h>
#endif

#define IS_DRIVER_NAME "infiniswap"
#define IS_QUEUE_DEPTH 128
#define IS_BIO_POOL_SIZE 256
#define IS_SECTOR_SIZE 512U

enum is_device_mode {
	IS_DEVICE_MODE_UNSET = 0,
	IS_DEVICE_MODE_BACKED,
};

enum is_device_state {
	IS_DEVICE_CREATED = 0,
	IS_DEVICE_ACTIVE,
	IS_DEVICE_DRAINING,
	IS_DEVICE_DRAINED,
	IS_DEVICE_STOPPED,
};

struct is_device {
	struct config_group group;
	struct mutex configfs_lock;
	struct mutex lifecycle_lock;
	spinlock_t io_lock;
	enum is_device_mode mode;
	enum is_device_state state;
	bool accepting_opens;
	bool accepting_io;
	bool configfs_dependent;
	bool disk_added;
	bool tag_set_allocated;
	bool bioset_initialized;
	char name[DISK_NAME_LEN];
	char backing_path[PATH_MAX];
	u64 capacity_bytes;
	sector_t capacity_sectors;
	int minor;
	atomic_t openers;
	atomic_t inflight;
	wait_queue_head_t drain_wait;
	struct block_device *backing_bdev;
#ifdef INFINISWAP_HAVE_BDEV_HANDLE
	struct bdev_handle *backing_handle;
#endif
	struct bio_set bio_set;
	struct blk_mq_tag_set tag_set;
	struct gendisk *disk;
};

struct is_request_ctx {
	struct is_device *device;
	struct request *request;
	atomic_t pending_bios;
	atomic_t status;
};

extern int is_major;

int is_minor_alloc(void);
void is_minor_free(int minor);

int is_configfs_register(void);
void is_configfs_unregister(void);

void is_device_init(struct is_device *device, const char *name);
const char *is_device_mode_name(struct is_device *device);
int is_device_set_mode(struct is_device *device, const char *buf, size_t count);
const char *is_device_state_name(struct is_device *device);
int is_device_set_backing_store(struct is_device *device, const char *buf,
				size_t count);
int is_device_set_capacity(struct is_device *device, const char *buf,
			   size_t count);
int is_device_activate(struct is_device *device);
int is_device_drain(struct is_device *device);
int is_device_stop(struct is_device *device);

#endif /* INFINISWAP_H */
