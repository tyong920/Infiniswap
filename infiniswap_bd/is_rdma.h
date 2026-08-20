/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */
#ifndef INFINISWAP_RDMA_H
#define INFINISWAP_RDMA_H

#include <linux/blk_types.h>
#include <linux/mm_types.h>
#include <linux/types.h>

struct is_device;

#define IS_RDMA_MAX_SEGMENTS 32U

struct is_rdma_io_segment {
	struct page *page;
	unsigned int offset;
	unsigned int length;
};

struct is_rdma_io {
	sector_t sector;
	unsigned int bytes;
	unsigned int segment_count;
	bool write;
	u64 generation;
	struct is_rdma_io_segment segments[IS_RDMA_MAX_SEGMENTS];
	void *context;
	void (*complete)(void *context, u64 generation, int status,
			 bool cancelled);
	void (*release)(void *context, u64 generation);
};

int is_rdma_start(struct is_device *device);
void is_rdma_stop(struct is_device *device);
bool is_rdma_range_mapped(struct is_device *device, sector_t sector,
			  unsigned int bytes);
bool is_rdma_range_valid(struct is_device *device, sector_t sector,
			 unsigned int bytes);
int is_rdma_submit(struct is_device *device, struct is_rdma_io *io);
int is_rdma_flush(struct is_device *device);
void is_rdma_note_activity(struct is_device *device, sector_t sector,
			   unsigned int bytes, bool write);
void is_rdma_mapping_parameters_changed(struct is_device *device);

ssize_t is_rdma_remote_chunk_placements_show(struct is_device *device,
					     char *page);
ssize_t is_rdma_provider_exclusions_show(struct is_device *device,
					 char *page);
ssize_t is_rdma_provider_runtime_status_show(struct is_device *device,
					     char *page);
unsigned int is_fabric_mapped_chunk_count(struct is_device *device);

#endif /* INFINISWAP_RDMA_H */
