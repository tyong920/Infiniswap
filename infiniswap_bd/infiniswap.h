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
#include <linux/workqueue.h>

#include "infiniswap_protocol.h"

#ifdef INFINISWAP_HAVE_LINUX_GENHD_H
#include <linux/genhd.h>
#endif

#define IS_DRIVER_NAME "infiniswap"
#define IS_QUEUE_DEPTH 128
#define IS_BIO_POOL_SIZE 256
#define IS_SECTOR_SIZE 512U
#define IS_CONSUMER_ID_SIZE (IS_PROTOCOL_CONSUMER_ID_MAX + 1U)
#define IS_PROVIDER_LIST_SIZE 4096U
#define IS_PROVIDER_ADDRESS_SIZE 64U
#define IS_RDMA_DEVICE_SIZE 64U
#define IS_PROVIDER_KEY_ID_SIZE (IS_PROTOCOL_KEY_ID_MAX + 1U)
#define IS_PSK_MIN_SIZE 32U
#define IS_PSK_MAX_SIZE 64U
#define IS_HOT_RANGE_THRESHOLD_DEFAULT 8U
#define IS_HOT_RANGE_READ_WEIGHT_DEFAULT 1U
#define IS_HOT_RANGE_WRITE_WEIGHT_DEFAULT 4U
#define IS_BACKING_RETRY_LIMIT 1U
#define IS_REMOTE_CHUNK_BYTES (1ULL << 30)
#define IS_MAX_REMOTE_CHUNKS IS_PROTOCOL_MAX_CHUNKS_PER_FRAME

struct is_rdma_session;

enum is_device_mode {
	IS_DEVICE_MODE_UNSET = 0,
	IS_DEVICE_MODE_BACKED,
};

enum is_acknowledgement_policy {
	IS_ACKNOWLEDGEMENT_POLICY_UNSET = 0,
	IS_ACKNOWLEDGEMENT_POLICY_STRICT,
	IS_ACKNOWLEDGEMENT_POLICY_REMOTE_FIRST,
};

enum is_device_state {
	IS_DEVICE_CREATED = 0,
	IS_DEVICE_ACTIVE,
	IS_DEVICE_DRAINING,
	IS_DEVICE_DRAINED,
	IS_DEVICE_STOPPED,
};

enum is_connection_state {
	IS_CONNECTION_NOT_CONNECTED = 0,
	IS_CONNECTION_CONNECTING,
	IS_CONNECTION_CONNECTED,
	IS_CONNECTION_DEGRADED,
};

enum is_backing_state {
	IS_BACKING_HEALTHY = 0,
	IS_BACKING_DEGRADED,
};

struct is_device {
	struct config_group group;
	struct mutex configfs_lock;
	struct mutex lifecycle_lock;
	spinlock_t io_lock;
	spinlock_t backing_lock;
	enum is_device_mode mode;
	enum is_acknowledgement_policy acknowledgement_policy;
	enum is_device_state state;
	bool accepting_opens;
	bool accepting_io;
	bool configfs_dependent;
	bool disk_added;
	bool tag_set_allocated;
	bool bioset_initialized;
	char name[DISK_NAME_LEN];
	char backing_path[PATH_MAX];
	char consumer_id[IS_CONSUMER_ID_SIZE];
	char providers[IS_PROVIDER_LIST_SIZE];
	char provider_address[IS_PROVIDER_ADDRESS_SIZE];
	char rdma_device[IS_RDMA_DEVICE_SIZE];
	char provider_key_id[IS_PROVIDER_KEY_ID_SIZE];
	u8 provider_psk[IS_PSK_MAX_SIZE];
	u8 provider_psk_size;
	u16 provider_port;
	u8 rdma_port;
	int rdma_numa_node;
	u32 provider_failure_deadline_ms;
	u64 hot_range_threshold;
	u32 hot_range_read_weight;
	u32 hot_range_write_weight;
	int swap_priority;
	int last_error;
	u64 capacity_bytes;
	u64 remote_capacity_bytes;
	sector_t capacity_sectors;
	int minor;
	atomic_t openers;
	atomic_t inflight;
	atomic_t connection_state;
	atomic_t backing_state;
	atomic_t mapped_hot_ranges;
	atomic64_t next_io_generation;
	atomic64_t backing_failures_total;
	atomic64_t backing_retries_total;
	atomic64_t backing_degraded_transitions_total;
	atomic64_t provider_timeouts_total;
	atomic64_t late_rdma_completions_total;
	atomic64_t rejected_writes_total;
	atomic64_t local_only_writes_total;
	atomic64_t backing_invalid_sectors;
	unsigned long *backing_invalid_bitmap;
	wait_queue_head_t drain_wait;
	struct block_device *backing_bdev;
#ifdef INFINISWAP_HAVE_BDEV_HANDLE
	struct bdev_handle *backing_handle;
#endif
	struct bio_set bio_set;
	struct workqueue_struct *ordered_backing_wq;
	struct blk_mq_tag_set tag_set;
	struct gendisk *disk;
	struct is_rdma_session *rdma;
};

struct is_request_ctx {
	struct is_device *device;
	struct request *request;
	atomic_t pending_bios;
	atomic_t status;
	atomic_t backing_io_failed;
	struct work_struct work;
};

extern int is_major;

int is_minor_alloc(void);
void is_minor_free(int minor);

int is_configfs_register(void);
void is_configfs_unregister(void);

void is_device_init(struct is_device *device, const char *name);
const char *is_device_mode_name(struct is_device *device);
int is_device_set_mode(struct is_device *device, const char *buf, size_t count);
const char *is_device_acknowledgement_policy_name(struct is_device *device);
int is_device_set_acknowledgement_policy(struct is_device *device,
					 const char *buf, size_t count);
int is_device_set_failure_deadline(struct is_device *device, const char *buf,
				   size_t count);
int is_device_set_hot_range_threshold(struct is_device *device, const char *buf,
				      size_t count);
int is_device_set_hot_range_read_weight(struct is_device *device,
					const char *buf, size_t count);
int is_device_set_hot_range_write_weight(struct is_device *device,
					 const char *buf, size_t count);
int is_device_set_consumer_id(struct is_device *device, const char *buf,
			      size_t count);
int is_device_set_providers(struct is_device *device, const char *buf,
			    size_t count);
int is_device_set_provider_address(struct is_device *device, const char *buf,
				   size_t count);
int is_device_set_provider_port(struct is_device *device, const char *buf,
				size_t count);
int is_device_set_rdma_device(struct is_device *device, const char *buf,
			      size_t count);
int is_device_set_rdma_port(struct is_device *device, const char *buf,
			    size_t count);
int is_device_set_rdma_numa_node(struct is_device *device, const char *buf,
				 size_t count);
int is_device_set_provider_key_id(struct is_device *device, const char *buf,
				  size_t count);
int is_device_set_provider_psk(struct is_device *device, const char *buf,
			       size_t count);
int is_device_set_swap_priority(struct is_device *device, const char *buf,
				size_t count);
const char *is_device_state_name(struct is_device *device);
const char *is_device_backing_state_name(struct is_device *device);
int is_device_set_backing_store(struct is_device *device, const char *buf,
				size_t count);
int is_device_set_capacity(struct is_device *device, const char *buf,
			   size_t count);
int is_device_activate(struct is_device *device);
int is_device_drain(struct is_device *device);
int is_device_stop(struct is_device *device);

#endif /* INFINISWAP_H */
