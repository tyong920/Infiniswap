// SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause
/*
 * Copyright 2017 University of Michigan, Ann Arbor
 * Copyright (c) 2013 Mellanox Technologies. All rights reserved.
 */
#include <linux/ctype.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "infiniswap.h"

static struct configfs_subsystem is_subsystem;

static inline struct is_device *to_is_device(struct config_item *item)
{
	return container_of(to_config_group(item), struct is_device, group);
}

static ssize_t is_device_backing_store_show(struct config_item *item,
					    char *page)
{
	struct is_device *device = to_is_device(item);
	ssize_t count;

	mutex_lock(&device->lifecycle_lock);
	count = sysfs_emit(page, "%s\n", device->backing_path);
	mutex_unlock(&device->lifecycle_lock);
	return count;
}

static ssize_t is_device_backing_store_store(struct config_item *item,
					     const char *page,
					     size_t count)
{
	int ret = is_device_set_backing_store(to_is_device(item), page, count);

	return ret ? ret : count;
}

static ssize_t is_device_mode_show(struct config_item *item, char *page)
{
	return sysfs_emit(page, "%s\n",
			  is_device_mode_name(to_is_device(item)));
}

static ssize_t is_device_mode_store(struct config_item *item,
				    const char *page, size_t count)
{
	int ret = is_device_set_mode(to_is_device(item), page, count);

	return ret ? ret : count;
}

static ssize_t is_device_capacity_bytes_show(struct config_item *item,
					     char *page)
{
	struct is_device *device = to_is_device(item);
	ssize_t count;

	mutex_lock(&device->lifecycle_lock);
	count = sysfs_emit(page, "%llu\n", device->capacity_bytes);
	mutex_unlock(&device->lifecycle_lock);
	return count;
}

static ssize_t is_device_capacity_bytes_store(struct config_item *item,
					      const char *page,
					      size_t count)
{
	int ret = is_device_set_capacity(to_is_device(item), page, count);

	return ret ? ret : count;
}

static ssize_t is_device_state_show(struct config_item *item, char *page)
{
	return sysfs_emit(page, "%s\n",
			  is_device_state_name(to_is_device(item)));
}

static ssize_t is_device_state_store(struct config_item *item,
				     const char *page, size_t count)
{
	struct is_device *device = to_is_device(item);
	ssize_t result;
	int ret;

	mutex_lock(&device->configfs_lock);
	if (sysfs_streq(page, "activate")) {
		if (device->configfs_dependent) {
			ret = -EALREADY;
			goto out;
		}

		ret = configfs_depend_item_unlocked(&is_subsystem, item);
		if (ret)
			goto out;
		device->configfs_dependent = true;

		ret = is_device_activate(device);
		if (ret) {
			device->configfs_dependent = false;
			configfs_undepend_item_unlocked(item);
		}
	} else if (sysfs_streq(page, "drain")) {
		ret = is_device_drain(device);
	} else if (sysfs_streq(page, "stop")) {
		ret = is_device_stop(device);
		if (!ret && device->configfs_dependent) {
			device->configfs_dependent = false;
			configfs_undepend_item_unlocked(item);
		}
	} else {
		ret = -EINVAL;
	}

out:
	result = ret ? ret : count;
	mutex_unlock(&device->configfs_lock);
	return result;
}

CONFIGFS_ATTR(is_device_, backing_store);
CONFIGFS_ATTR(is_device_, mode);
CONFIGFS_ATTR(is_device_, capacity_bytes);
CONFIGFS_ATTR(is_device_, state);

static struct configfs_attribute *is_device_attrs[] = {
	&is_device_attr_backing_store,
	&is_device_attr_mode,
	&is_device_attr_capacity_bytes,
	&is_device_attr_state,
	NULL,
};

static void is_device_release(struct config_item *item)
{
	struct is_device *device = to_is_device(item);

	WARN_ON(device->configfs_dependent);
	WARN_ON(device->disk);
	WARN_ON(device->backing_bdev);
	kfree(device);
}

static struct configfs_item_operations is_device_item_ops = {
	.release = is_device_release,
};

static struct config_item_type is_device_type = {
	.ct_item_ops = &is_device_item_ops,
	.ct_attrs = is_device_attrs,
	.ct_owner = THIS_MODULE,
};

static bool is_valid_device_name(const char *name)
{
	const unsigned char *cursor = (const unsigned char *)name;

	if (!name[0] || !isalnum((unsigned char)name[0]) ||
	    strnlen(name, DISK_NAME_LEN) == DISK_NAME_LEN)
		return false;

	for (; *cursor; cursor++) {
		if (!isalnum(*cursor) && *cursor != '-' && *cursor != '_')
			return false;
	}
	return true;
}

static struct config_group *is_make_device(struct config_group *group,
					   const char *name)
{
	struct is_device *device;

	if (!is_valid_device_name(name))
		return ERR_PTR(-EINVAL);

	device = kzalloc(sizeof(*device), GFP_KERNEL);
	if (!device)
		return ERR_PTR(-ENOMEM);

	is_device_init(device, name);
	config_group_init_type_name(&device->group, name, &is_device_type);
	pr_info(IS_DRIVER_NAME ": created device configuration %s\n", name);
	return &device->group;
}

static void is_drop_device(struct config_group *group, struct config_item *item)
{
	struct is_device *device = to_is_device(item);
	int ret;

	mutex_lock(&device->configfs_lock);
	ret = is_device_stop(device);
	if (WARN_ON(ret)) {
		mutex_unlock(&device->configfs_lock);
		return;
	}
	mutex_unlock(&device->configfs_lock);
	pr_info(IS_DRIVER_NAME ": destroyed device configuration %s\n",
		device->name);
	config_item_put(item);
}

static struct configfs_group_operations is_root_group_ops = {
	.make_group = is_make_device,
	.drop_item = is_drop_device,
};

static struct config_item_type is_root_type = {
	.ct_group_ops = &is_root_group_ops,
	.ct_owner = THIS_MODULE,
};

int is_configfs_register(void)
{
	config_group_init_type_name(&is_subsystem.su_group, IS_DRIVER_NAME,
				    &is_root_type);
	mutex_init(&is_subsystem.su_mutex);
	return configfs_register_subsystem(&is_subsystem);
}

void is_configfs_unregister(void)
{
	configfs_unregister_subsystem(&is_subsystem);
}
