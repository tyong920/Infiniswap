/*
 * Infiniswap, remote memory paging over RDMA
 * Copyright 2017 University of Michigan, Ann Arbor
 *  
 * Stackbd
 * Copyright 2014 Oren Kishon
 * https://github.com/OrenKishon/stackbd
 *
 * Copyright (c) 2013 Mellanox Technologies��. All rights reserved.
 *
 * This software is available to you under a choice of one of two licenses.
 * You may choose to be licensed under the terms of the GNU General Public
 * License (GPL) Version 2, available from the file COPYING in the main
 * directory of this source tree, or the Mellanox Technologies�� BSD license
 * below:
 *
 *      - Redistribution and use in source and binary forms, with or without
 *        modification, are permitted provided that the following conditions
 *        are met:
 *
 *      - Redistributions of source code must retain the above copyright
 *        notice, this list of conditions and the following disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 *      - Neither the name of the Mellanox Technologies�� nor the names of its
 *        contributors may be used to endorse or promote products derived from
 *        this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "infiniswap.h"

struct stackbd_t stackbd;

static int major_num;
module_param(major_num, int, 0);
static int logical_block_size = 512;
module_param(logical_block_size, int, 0);

static DECLARE_WAIT_QUEUE_HEAD(req_event);

struct bio *IS_bio_clone(struct bio *source, gfp_t gfp)
{
#ifdef INFINISWAP_HAVE_BIO_ALLOC_CLONE
	return bio_alloc_clone(stackbd.bdev_raw, source, gfp, NULL);
#else
	return bio_clone_fast(source, gfp, NULL);
#endif
}


static void IS_stackbd_end_io(struct bio *bio)
{
	struct stackbd_request *stack_req = bio->bi_private;
	blk_status_t status = bio->bi_status;

	bio_put(bio);
	if (status != BLK_STS_OK)
		atomic_cmpxchg(&stack_req->status, BLK_STS_OK, status);
	if (atomic_dec_and_test(&stack_req->pending))
		blk_mq_end_request(stack_req->req,
				   atomic_read(&stack_req->status));
}

static void IS_stackbd_end_io3(struct bio *bio)
{
    struct rdma_ctx *ctx = ptr_from_uint64((uint64_t)bio->bi_private);

    IS_insert_ctx(ctx);
    bio_put(bio);
}


static void stackbd_io_fn(struct bio *bio)
{
	if (bio == NULL)
        printk("bio is NULL\n");

	bio_set_dev(bio, stackbd.bdev_raw);
	submit_bio_noacct(bio);
}
static int stackbd_threadfn(void *data)
{
    struct bio *bio;

    set_user_nice(current, -20);
    while (!kthread_should_stop())
    {
        wait_event_interruptible(req_event, kthread_should_stop() ||
                !bio_list_empty(&stackbd.bio_list));
        spin_lock_irq(&stackbd.lock);
        if (bio_list_empty(&stackbd.bio_list))
        {
            spin_unlock_irq(&stackbd.lock);
            continue;
        }
        bio = bio_list_pop(&stackbd.bio_list);
        spin_unlock_irq(&stackbd.lock);
        stackbd_io_fn(bio);
    }
    return 0;
}
void stackbd_make_request5(struct bio *bio)
{
    spin_lock_irq(&stackbd.lock);
    if (!stackbd.bdev_raw)
    {
        printk("stackbd: Request before bdev_raw is ready, aborting\n");
        goto abort;
    }
    if (!stackbd.is_active)
    {
        printk("stackbd: Device not active yet, aborting\n");
        goto abort;
    }
    bio->bi_end_io = IS_stackbd_end_io3;
    bio_list_add(&stackbd.bio_list, bio);

    wake_up(&req_event);
    spin_unlock_irq(&stackbd.lock);
    return;
abort:
    spin_unlock_irq(&stackbd.lock);
    printk("<%p> Abort request\n\n", bio);
    bio_io_error(bio);
}

void IS_submit_to_backing_store(struct request *req)
{
	struct IS_request_ctx *request_ctx;
	struct stackbd_request *stack_req;
	struct bio_list cloned_bios;
	struct bio *source;
	struct bio *clone;

	request_ctx = blk_mq_rq_to_pdu(req);
	stack_req = &request_ctx->backing;
	stack_req->req = req;
	atomic_set(&stack_req->pending, 0);
	atomic_set(&stack_req->status, BLK_STS_OK);
	bio_list_init(&cloned_bios);

	spin_lock_irq(&stackbd.lock);
	if (!stackbd.bdev_raw || !stackbd.is_active)
		goto abort;

	for (source = req->bio; source; source = source->bi_next) {
		clone = IS_bio_clone(source, GFP_ATOMIC);
		if (!clone)
			goto abort;
		clone->bi_end_io = IS_stackbd_end_io;
		clone->bi_private = stack_req;
		bio_list_add(&cloned_bios, clone);
		atomic_inc(&stack_req->pending);
	}
	if (!atomic_read(&stack_req->pending))
		goto abort;

	while ((clone = bio_list_pop(&cloned_bios)))
		bio_list_add(&stackbd.bio_list, clone);

	wake_up(&req_event);
	spin_unlock_irq(&stackbd.lock);
	return;

abort:
	spin_unlock_irq(&stackbd.lock);
	while ((clone = bio_list_pop(&cloned_bios)))
		bio_put(clone);
	blk_mq_end_request(req, BLK_STS_IOERR);
}

// from original stackbd
#ifdef INFINISWAP_HAVE_BDEV_HANDLE
void stackbd_make_request(struct bio *bio)
#else
blk_qc_t stackbd_make_request(struct bio *bio)
#endif
{
    spin_lock_irq(&stackbd.lock);
    if (!stackbd.bdev_raw)
    {
        printk("stackbd: Request before bdev_raw is ready, aborting\n");
        goto abort;
    }
    if (!stackbd.is_active)
    {
        printk("stackbd: Device not active yet, aborting\n");
        goto abort;
    }
    bio_list_add(&stackbd.bio_list, bio);
    wake_up(&req_event);
    spin_unlock_irq(&stackbd.lock);

#ifndef INFINISWAP_HAVE_BDEV_HANDLE
    return BLK_QC_T_NONE;
#endif
abort:
    spin_unlock_irq(&stackbd.lock);
    printk("<%p> Abort request\n\n", bio);
    bio_io_error(bio);
#ifndef INFINISWAP_HAVE_BDEV_HANDLE
    return BLK_QC_T_NONE;
#endif
}

static struct block_device *stackbd_bdev_open(const char *dev_path)
{
#ifdef INFINISWAP_HAVE_BDEV_HANDLE
    stackbd.bdev_handle = bdev_open_by_path(dev_path, STACKBD_BDEV_MODE,
                                            &stackbd, NULL);
    if (IS_ERR(stackbd.bdev_handle)) {
        printk("stackbd: error opening %s: %ld\n", dev_path,
               PTR_ERR(stackbd.bdev_handle));
        stackbd.bdev_handle = NULL;
        return NULL;
    }
    return stackbd.bdev_handle->bdev;
#else
    struct block_device *bdev_raw;

    bdev_raw = blkdev_get_by_path(dev_path, STACKBD_BDEV_MODE, &stackbd);
    if (IS_ERR(bdev_raw)) {
        printk("stackbd: error opening %s: %ld\n", dev_path,
               PTR_ERR(bdev_raw));
        return NULL;
    }
    return bdev_raw;
#endif
}

static void stackbd_bdev_close(void)
{
#ifdef INFINISWAP_HAVE_BDEV_HANDLE
    bdev_release(stackbd.bdev_handle);
    stackbd.bdev_handle = NULL;
#else
    blkdev_put(stackbd.bdev_raw, STACKBD_BDEV_MODE);
#endif
    stackbd.bdev_raw = NULL;
}

static int stackbd_start(char dev_path[])
{
    unsigned max_sectors;
    unsigned int page_sec = IS_PAGE_SIZE;

    if (!(stackbd.bdev_raw = stackbd_bdev_open(dev_path)))
        return -EFAULT;
    /* Set up our internal device */
    stackbd.capacity = get_capacity(stackbd.bdev_raw->bd_disk);
    printk("stackbd: Device real capacity: %llu\n", (long long unsigned int) stackbd.capacity);
    set_capacity(stackbd.gd, stackbd.capacity);

    sector_div(page_sec, KERNEL_SECTOR_SIZE);
    max_sectors = page_sec * MAX_SGL_LEN;
    blk_queue_max_hw_sectors(stackbd.queue, max_sectors);
    printk("stackbd: Max sectors: %u\n", max_sectors);
    stackbd.thread = kthread_create(stackbd_threadfn, NULL,
           stackbd.gd->disk_name);
    if (IS_ERR(stackbd.thread))
    {
        printk("stackbd: error kthread_create <%lu>\n",
               PTR_ERR(stackbd.thread));
        goto error_after_bdev;
    }
    printk("stackbd: done initializing successfully\n");
    stackbd.is_active = 1;
    atomic_set(&stackbd.redirect_done, STACKBD_REDIRECT_OFF);
    wake_up_process(stackbd.thread);
    return 0;
error_after_bdev:
    stackbd_bdev_close();
    return -EFAULT;
}

static int stackbd_getgeo(struct block_device * block_device, struct hd_geometry * geo)
{
        long size;
        /* We have no real geometry, of course, so make something up. */
        size = stackbd.capacity *
               (logical_block_size / KERNEL_SECTOR_SIZE);
        geo->cylinders = (size & ~0x3f) >> 6;
        geo->heads = 4;
        geo->sectors = 16;
        geo->start = 0;
        return 0;
}

static bool IS_bitmap_group_test(int *bitmap, unsigned long offset,
				 unsigned long len)
{
	unsigned long first_page = offset / IS_PAGE_SIZE;
	unsigned long last_page = (offset + len - 1) / IS_PAGE_SIZE;
	unsigned long page;

	for (page = first_page; page <= last_page; page++) {
		if (!IS_bitmap_test(bitmap, page))
			return false;
	}
	return true;
}

static int IS_request(struct request *req, struct IS_queue *xq)
{
	struct IS_file *xdev = xq->xdev;
	int write = rq_data_dir(req) == WRITE;
	unsigned long start = blk_rq_pos(req) << IS_SECT_SHIFT;
	unsigned long len  = blk_rq_bytes(req);
    unsigned long len1 = 0;
	unsigned long len2 = 0;
	int err = 0;
	struct IS_session *IS_sess = xq->IS_conn->IS_sess;
	int gb_index, end_index;
	unsigned long chunk_offset, chunk2_offset;	
	struct kernel_cb *cb;
	struct kernel_cb *cb2;
	int cb_index, cb2_index;
	int chunk_index, chunk2_index;
	struct remote_chunk_g *chunk;
	struct remote_chunk_g *chunk2;

	if (!len || !IS_ALIGNED(start, IS_PAGE_SIZE) ||
	    !IS_ALIGNED(len, IS_PAGE_SIZE)) {
		IS_submit_to_backing_store(req);
		return 0;
	}

	// pr_info("%s called and req=%p, start=0x%lx, len=%lu\n", __func__, req, start, len);
	gb_index = start >> ONE_GB_SHIFT;
	end_index = (start + len - 1) >> ONE_GB_SHIFT;

	//count
	if (write) {
		spin_lock_irq(&IS_sess->write_ops_lock[gb_index]);
		IS_sess->write_ops[gb_index] += 1;
		spin_unlock_irq(&IS_sess->write_ops_lock[gb_index]);
	} else {
		spin_lock_irq(&IS_sess->read_ops_lock[gb_index]);
		IS_sess->read_ops[gb_index] += 1;
		spin_unlock_irq(&IS_sess->read_ops_lock[gb_index]);
	}

	if (gb_index == end_index) { // it's in the same CHUNK
		cb_index = atomic_read(IS_sess->cb_index_map + gb_index);	
		if (cb_index == NO_CB_MAPPED){
			//go to disk	
			IS_submit_to_backing_store(req);
			return err;
		}
		//find cb and chunk
		chunk_offset = start & ONE_GB_MASK;	
		cb = IS_sess->cb_list[cb_index];
		chunk_index = IS_sess->chunk_map_cb_chunk[gb_index];
		if (chunk_index == -1){
			IS_submit_to_backing_store(req);
			return err;
		}
		chunk = cb->remote_chunk.chunk_list[chunk_index];
	}else {	//two chunks
		cb_index = atomic_read(IS_sess->cb_index_map + gb_index);	
		cb2_index = atomic_read(IS_sess->cb_index_map + end_index);	

        chunk_offset = start & ONE_GB_MASK;             
		if (cb_index != NO_CB_MAPPED){
			cb = IS_sess->cb_list[cb_index];
		 	chunk_index = IS_sess->chunk_map_cb_chunk[gb_index];
			if (chunk_index != -1) {
				chunk = cb->remote_chunk.chunk_list[chunk_index];
				len1 = ONE_GB - chunk_offset;
				pr_err("%s, clear chunk1[%d], start=0x%lx, len1=%lu\n",
				       __func__, gb_index, chunk_offset, len1);
				if (write)
					IS_bitmap_group_clear(chunk->bitmap_g,
							      chunk_offset, len1);
			}
		}
		if (cb2_index != NO_CB_MAPPED){
			chunk2_offset = 0;
			cb2 = IS_sess->cb_list[cb2_index];	
			chunk2_index = IS_sess->chunk_map_cb_chunk[end_index];
			if (chunk2_index != -1) {
				chunk2 = cb2->remote_chunk.chunk_list[chunk2_index];
				len2 = chunk_offset + len - ONE_GB;
				pr_err("%s, clear chunk2[%d], start=0x%lx, len2=%lu\n",
				       __func__, end_index, chunk2_offset, len2);
				if (write)
					IS_bitmap_group_clear(chunk2->bitmap_g,
							      chunk2_offset, len2);
			}
		}
        
		IS_submit_to_backing_store(req);
		return err;
	}

	if (write && len > IS_PAGE_SIZE) {
		/* The RDMA write mirror still handles one page at a time. */
		IS_bitmap_group_clear(chunk->bitmap_g, chunk_offset, len);
		IS_submit_to_backing_store(req);
		return 0;
	}

	if (write){
		if (atomic_read(&IS_sess->rdma_on) == DEV_RDMA_ON){
			err = IS_transfer_chunk(xdev, cb, cb_index, chunk_index,
						chunk, chunk_offset, len, write, req,
						xq);
		}else{
			IS_submit_to_backing_store(req);
		}
	}else{
		if (atomic_read(&IS_sess->rdma_on) == DEV_RDMA_ON){
			if (IS_bitmap_group_test(chunk->bitmap_g, chunk_offset,
						 len)) {
				err = IS_transfer_chunk(xdev, cb, cb_index, chunk_index,
							chunk, chunk_offset, len, write,
							req, xq);
			}else {
				IS_submit_to_backing_store(req);
			}
		}else{
			IS_submit_to_backing_store(req);
		}
	}
	if (unlikely(err))
		pr_err("transfer failed for req %p\n", req);

	return err;
}

static blk_status_t IS_queue_rq(struct blk_mq_hw_ctx *hctx,
				const struct blk_mq_queue_data *bd)
{
	struct IS_queue *IS_q;
	struct request *rq = bd->rq;
	int err;

	IS_q = hctx->driver_data;
	blk_mq_start_request(rq);
	err = IS_request(rq, IS_q);
	if (unlikely(err))
		blk_mq_end_request(rq, BLK_STS_IOERR);

	return BLK_STS_OK;
}

// connect hctx with IS-file, IS-conn, and queue
static int IS_init_hctx(struct blk_mq_hw_ctx *hctx, void *data,
			  unsigned int index)
{
	struct IS_file *xdev = data;
	struct IS_queue *xq;

	xq = &xdev->queues[index];
	pr_info("%s called index=%u xq=%p\n", __func__, index, xq);
	
	xq->IS_conn = xdev->IS_conns[index];
	xq->xdev = xdev;
	xq->queue_depth = xdev->queue_depth;
	hctx->driver_data = xq;

	return 0;
}

static struct blk_mq_ops IS_mq_ops = {
	.queue_rq       = IS_queue_rq,
	.init_hctx      = IS_init_hctx,
};

int IS_setup_queues(struct IS_file *xdev)
{
	pr_debug("%s called\n", __func__);
	xdev->queues = kzalloc(submit_queues * sizeof(*xdev->queues),
			GFP_KERNEL);
	if (!xdev->queues)
		return -ENOMEM;

	return 0;
}

#ifdef INFINISWAP_HAVE_BDEV_HANDLE
static int IS_open(struct gendisk *gd, blk_mode_t mode)
{
	pr_debug("%s called\n", __func__);
	return 0;
}

static void IS_release(struct gendisk *gd)
{
	pr_debug("%s called\n", __func__);
}

static int IS_ioctl(struct block_device *bd, blk_mode_t mode,
		    unsigned cmd, unsigned long arg)
#else
static int IS_open(struct block_device *bd, fmode_t mode)
{
	pr_debug("%s called\n", __func__);
	return 0;
}

static void IS_release(struct gendisk *gd, fmode_t mode)
{
	pr_debug("%s called\n", __func__);
}

static int IS_ioctl(struct block_device *bd, fmode_t mode,
		    unsigned cmd, unsigned long arg)
#endif
{
	pr_debug("%s called\n", __func__);
	return -ENOTTY;
}

// bind to IS_file in IS_register_block_device
static struct block_device_operations IS_ops = {
	.owner           = THIS_MODULE,
	.open            = IS_open,
	.release         = IS_release,
	.ioctl           = IS_ioctl,
};

static struct block_device_operations stackbd_ops = {
	.owner           = THIS_MODULE,
	.submit_bio      = stackbd_make_request,
	.getgeo          = stackbd_getgeo,
};

void IS_destroy_queues(struct IS_file *xdev)
{
	pr_info("%s\n", __func__);
	kfree(xdev->queues);
}

int IS_register_block_device(struct IS_file *IS_file)
{
	sector_t sectors = IS_file->stbuf.st_size / IS_SECT_SIZE;
	unsigned int max_sectors = (PAGE_SIZE / IS_SECT_SIZE) * MAX_SGL_LEN;
	int err;

	pr_info("%s\n", __func__);
	IS_file->major = IS_major;
	IS_file->tag_set.ops = &IS_mq_ops;
	IS_file->tag_set.nr_hw_queues = submit_queues;
	IS_file->tag_set.queue_depth = IS_QUEUE_DEPTH;
	IS_file->tag_set.numa_node = NUMA_NO_NODE;
	IS_file->tag_set.cmd_size = sizeof(struct IS_request_ctx);
	IS_file->tag_set.flags = BLK_MQ_F_SHOULD_MERGE;
	IS_file->tag_set.driver_data = IS_file;

	err = blk_mq_alloc_tag_set(&IS_file->tag_set);
	if (err)
		return err;

	IS_file->disk = blk_mq_alloc_disk(&IS_file->tag_set, IS_file);
	if (IS_ERR(IS_file->disk)) {
		err = PTR_ERR(IS_file->disk);
		IS_file->disk = NULL;
		goto free_tag_set;
	}
	IS_file->queue = IS_file->disk->queue;
	blk_queue_flag_set(QUEUE_FLAG_NONROT, IS_file->queue);
	blk_queue_flag_clear(QUEUE_FLAG_ADD_RANDOM, IS_file->queue);

	IS_file->disk->major = IS_file->major;
	IS_file->disk->first_minor = IS_file->index;
	IS_file->disk->minors = 1;
	IS_file->disk->fops = &IS_ops;
	IS_file->disk->private_data = IS_file;
	blk_queue_logical_block_size(IS_file->queue, IS_SECT_SIZE);
	blk_queue_physical_block_size(IS_file->queue, IS_SECT_SIZE);
	blk_queue_max_hw_sectors(IS_file->queue, max_sectors);
	set_capacity(IS_file->disk, sectors);
	strscpy(IS_file->disk->disk_name, IS_file->dev_name, DISK_NAME_LEN);

	spin_lock_init(&stackbd.lock);
	bio_list_init(&stackbd.bio_list);
	stackbd.is_active = 0;
	stackbd.bdev_raw = NULL;
#ifdef INFINISWAP_HAVE_BDEV_HANDLE
	stackbd.bdev_handle = NULL;
#endif
	stackbd.gd = blk_alloc_disk(NUMA_NO_NODE);
	if (IS_ERR(stackbd.gd)) {
		err = PTR_ERR(stackbd.gd);
		stackbd.gd = NULL;
		goto put_is_disk;
	}
	stackbd.queue = stackbd.gd->queue;
	stackbd.gd->fops = &stackbd_ops;
	stackbd.gd->private_data = &stackbd;
	strscpy(stackbd.gd->disk_name, STACKBD_NAME_0, DISK_NAME_LEN);
	blk_queue_logical_block_size(stackbd.queue, logical_block_size);

	major_num = register_blkdev(major_num, STACKBD_NAME);
	if (major_num < 0) {
		err = major_num;
		goto put_stackbd_disk;
	}
	stackbd.gd->major = major_num;
	stackbd.gd->first_minor = 0;
	stackbd.gd->minors = 1;

	err = add_disk(stackbd.gd);
	if (err)
		goto unregister_stackbd;
	err = stackbd_start(BACKUP_DISK);
	if (err)
		goto delete_stackbd;

	err = add_disk(IS_file->disk);
	if (err)
		goto stop_stackbd;

	return 0;

stop_stackbd:
	stackbd.is_active = 0;
	kthread_stop(stackbd.thread);
	stackbd_bdev_close();
delete_stackbd:
	del_gendisk(stackbd.gd);
unregister_stackbd:
	unregister_blkdev(major_num, STACKBD_NAME);
	major_num = 0;
put_stackbd_disk:
	put_disk(stackbd.gd);
	stackbd.gd = NULL;
put_is_disk:
	put_disk(IS_file->disk);
	IS_file->disk = NULL;
free_tag_set:
	blk_mq_free_tag_set(&IS_file->tag_set);
	return err;
}

void IS_unregister_block_device(struct IS_file *IS_file)
{
	del_gendisk(IS_file->disk);

	if (stackbd.is_active) {
		stackbd.is_active = 0;
		kthread_stop(stackbd.thread);
		stackbd_bdev_close();
	}
	if (stackbd.gd) {
		del_gendisk(stackbd.gd);
		put_disk(stackbd.gd);
		stackbd.gd = NULL;
	}
	if (major_num > 0) {
		unregister_blkdev(major_num, STACKBD_NAME);
		major_num = 0;
	}

	put_disk(IS_file->disk);
	IS_file->disk = NULL;
	blk_mq_free_tag_set(&IS_file->tag_set);
}

void IS_single_chunk_init(struct kernel_cb *cb)
{
	int i = 0;
	int select_chunk = cb->recv_buf.size_gb;
	struct IS_session *IS_session = cb->IS_sess;

	for (i = 0; i < MAX_MR_SIZE_GB; i++) {
		if (cb->recv_buf.rkey[i]) { //from server, this chunk is allocated and given to you
			pr_info("Received rkey %x addr %llx from peer\n", ntohl(cb->recv_buf.rkey[i]), (unsigned long long)ntohll(cb->recv_buf.buf[i]));	
			cb->remote_chunk.chunk_list[i]->remote_rkey = ntohl(cb->recv_buf.rkey[i]);
			cb->remote_chunk.chunk_list[i]->remote_addr = ntohll(cb->recv_buf.buf[i]);
			cb->remote_chunk.chunk_list[i]->bitmap_g = (int *)kzalloc(sizeof(int) * BITMAP_INT_SIZE, GFP_KERNEL);
			IS_bitmap_init(cb->remote_chunk.chunk_list[i]->bitmap_g);
			IS_session->free_chunk_index -= 1;
			IS_session->chunk_map_cb_chunk[select_chunk] = i;
			cb->remote_chunk.chunk_map[i] = select_chunk;

			cb->remote_chunk.chunk_size_g += 1;
			cb->remote_chunk.c_state = C_READY;
			atomic_set(cb->remote_chunk.remote_mapped + i, CHUNK_MAPPED);
			atomic_set(IS_session->cb_index_map + (select_chunk), cb->cb_index);
			break;
		}
	}
}

void IS_chunk_list_init(struct kernel_cb *cb)
{
	int i = 0;
	int size_g = cb->recv_buf.size_gb;
	struct IS_session *IS_session = cb->IS_sess;
	int sess_free_chunk;
	int j = 0;

	for (i = 0; i < MAX_MR_SIZE_GB; i++) {
		if (cb->recv_buf.rkey[i]) { 
			pr_info("Received rkey %x addr %llx from peer\n", ntohl(cb->recv_buf.rkey[i]), (unsigned long long)ntohll(cb->recv_buf.buf[i]));	
			cb->remote_chunk.chunk_list[i]->remote_rkey = ntohl(cb->recv_buf.rkey[i]);
			cb->remote_chunk.chunk_list[i]->remote_addr = ntohll(cb->recv_buf.buf[i]);
			cb->remote_chunk.chunk_list[i]->bitmap_g = (int *)kzalloc(sizeof(int) * BITMAP_INT_SIZE, GFP_KERNEL);
			IS_bitmap_init(cb->remote_chunk.chunk_list[i]->bitmap_g);
			atomic_set(cb->remote_chunk.remote_mapped + i, CHUNK_MAPPED);
			sess_free_chunk = IS_session->unmapped_chunk_list[IS_session->free_chunk_index];
			IS_session->free_chunk_index -= 1;
			atomic_set(IS_session->cb_index_map + (sess_free_chunk), cb->cb_index);
			IS_session->chunk_map_cb_chunk[sess_free_chunk] = i;
			cb->remote_chunk.chunk_map[i] = sess_free_chunk;
			j += 1;
		}
	}
	if (j != size_g){
		pr_err("%s, j%d != size_g%d\n", __func__, j, size_g);
	}
	cb->remote_chunk.chunk_size_g += size_g;
	cb->remote_chunk.c_state = C_READY;
}


void IS_bitmap_set(int *bitmap, int i)
{
	bitmap[i >> BITMAP_SHIFT] |= 1 << (i & BITMAP_MASK);
}

void IS_bitmap_group_set(int *bitmap, unsigned long offset, unsigned long len)
{
	int start_page = (int)(offset/IS_PAGE_SIZE);	
	int len_page = (int)(len/IS_PAGE_SIZE);
	int i;
	for (i=0; i<len_page; i++){
		IS_bitmap_set(bitmap, start_page + i);
	}
}
void IS_bitmap_group_clear(int *bitmap, unsigned long offset, unsigned long len)
{
	int start_page = (int)(offset/IS_PAGE_SIZE);	
	int len_page = (int)(len/IS_PAGE_SIZE);
	int i;
	for (i=0; i<len_page; i++){
		IS_bitmap_clear(bitmap, start_page + i);
	}
}
bool IS_bitmap_test(int *bitmap, int i)
{
	if ((bitmap[i >> BITMAP_SHIFT] & (1 << (i & BITMAP_MASK))) != 0){
		return true;
	}else{
		return false;
	}
}


void IS_bitmap_clear(int *bitmap, int i)
{
	bitmap[i >> BITMAP_SHIFT] &= ~(1 << (i & BITMAP_MASK));
}
void IS_bitmap_init(int *bitmap)
{
	memset(bitmap, 0x00, ONE_GB/(4096*8));
}
