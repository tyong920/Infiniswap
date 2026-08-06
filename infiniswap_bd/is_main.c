// SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause
/*
 * Copyright 2017 University of Michigan, Ann Arbor
 * Copyright (c) 2013 Mellanox Technologies. All rights reserved.
 */
#include <linux/idr.h>
#include <linux/init.h>
#include <linux/kdev_t.h>
#include <linux/module.h>

#include "infiniswap.h"

#define IS_DRIVER_VERSION "0.1"

MODULE_AUTHOR("Infiniswap contributors");
MODULE_DESCRIPTION("Infiniswap Memory Consumer block device");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_VERSION(IS_DRIVER_VERSION);

int is_major;
static DEFINE_IDA(is_minor_ida);

int is_minor_alloc(void)
{
	return ida_alloc_max(&is_minor_ida, MINORMASK, GFP_KERNEL);
}

void is_minor_free(int minor)
{
	ida_free(&is_minor_ida, minor);
}

static int __init is_module_init(void)
{
	int ret;

	is_major = register_blkdev(0, IS_DRIVER_NAME);
	if (is_major < 0)
		return is_major;

	ret = is_configfs_register();
	if (ret) {
		unregister_blkdev(is_major, IS_DRIVER_NAME);
		is_major = 0;
		return ret;
	}

	pr_info(IS_DRIVER_NAME ": registered block major %d\n", is_major);
	return 0;
}

static void __exit is_module_exit(void)
{
	is_configfs_unregister();
	unregister_blkdev(is_major, IS_DRIVER_NAME);
	ida_destroy(&is_minor_ida);
	pr_info(IS_DRIVER_NAME ": unloaded\n");
}

module_init(is_module_init);
module_exit(is_module_exit);
