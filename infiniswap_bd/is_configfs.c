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
#include "is_rdma.h"

static struct configfs_subsystem is_subsystem;

static inline struct is_device *to_is_device(struct config_item *item)
{
	return container_of(to_config_group(item), struct is_device, group);
}

static ssize_t is_store_result(struct is_device *device, int ret, size_t count)
{
	if (ret)
		WRITE_ONCE(device->last_error, -ret);
	return ret ? ret : count;
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
	struct is_device *device = to_is_device(item);
	int ret = is_device_set_backing_store(device, page, count);

	return is_store_result(device, ret, count);
}

static ssize_t is_device_mode_show(struct config_item *item, char *page)
{
	return sysfs_emit(page, "%s\n",
			  is_device_mode_name(to_is_device(item)));
}

static ssize_t is_device_mode_store(struct config_item *item,
				    const char *page, size_t count)
{
	struct is_device *device = to_is_device(item);
	int ret = is_device_set_mode(device, page, count);

	return is_store_result(device, ret, count);
}

static ssize_t is_device_remote_only_eligible_show(struct config_item *item,
						    char *page)
{
	struct is_device *device = to_is_device(item);

	return sysfs_emit(page, "%u\n", device->remote_only_eligible ? 1U : 0U);
}

static ssize_t is_device_remote_only_eligible_store(struct config_item *item,
						     const char *page,
						     size_t count)
{
	struct is_device *device = to_is_device(item);
	int ret = is_device_set_remote_only_eligible(device, page, count);

	return is_store_result(device, ret, count);
}

static ssize_t is_device_acknowledgement_policy_show(struct config_item *item,
						      char *page)
{
	return sysfs_emit(page, "%s\n",
		is_device_acknowledgement_policy_name(to_is_device(item)));
}

static ssize_t is_device_acknowledgement_policy_store(
	struct config_item *item, const char *page, size_t count)
{
	struct is_device *device = to_is_device(item);
	int ret = is_device_set_acknowledgement_policy(device, page, count);

	return is_store_result(device, ret, count);
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
	struct is_device *device = to_is_device(item);
	int ret = is_device_set_capacity(device, page, count);

	return is_store_result(device, ret, count);
}

static ssize_t is_device_provider_failure_deadline_ms_show(
	struct config_item *item, char *page)
{
	struct is_device *device = to_is_device(item);
	ssize_t count;

	mutex_lock(&device->lifecycle_lock);
	count = sysfs_emit(page, "%u\n",
			   device->provider_failure_deadline_ms);
	mutex_unlock(&device->lifecycle_lock);
	return count;
}

static ssize_t is_device_provider_failure_deadline_ms_store(
	struct config_item *item, const char *page, size_t count)
{
	struct is_device *device = to_is_device(item);
	int ret = is_device_set_failure_deadline(device, page, count);

	return is_store_result(device, ret, count);
}

static ssize_t is_device_hot_range_threshold_show(struct config_item *item,
						   char *page)
{
	return sysfs_emit(page, "%llu\n",
		READ_ONCE(to_is_device(item)->hot_range_threshold));
}

static ssize_t is_device_hot_range_threshold_store(struct config_item *item,
						    const char *page,
						    size_t count)
{
	struct is_device *device = to_is_device(item);
	int ret = is_device_set_hot_range_threshold(device, page, count);

	return is_store_result(device, ret, count);
}

static ssize_t is_device_hot_range_read_weight_show(struct config_item *item,
						     char *page)
{
	return sysfs_emit(page, "%u\n",
		READ_ONCE(to_is_device(item)->hot_range_read_weight));
}

static ssize_t is_device_hot_range_read_weight_store(struct config_item *item,
						      const char *page,
						      size_t count)
{
	struct is_device *device = to_is_device(item);
	int ret = is_device_set_hot_range_read_weight(device, page, count);

	return is_store_result(device, ret, count);
}

static ssize_t is_device_hot_range_write_weight_show(struct config_item *item,
						      char *page)
{
	return sysfs_emit(page, "%u\n",
		READ_ONCE(to_is_device(item)->hot_range_write_weight));
}

static ssize_t is_device_hot_range_write_weight_store(struct config_item *item,
						       const char *page,
						       size_t count)
{
	struct is_device *device = to_is_device(item);
	int ret = is_device_set_hot_range_write_weight(device, page, count);

	return is_store_result(device, ret, count);
}

static ssize_t is_device_consumer_id_show(struct config_item *item, char *page)
{
	struct is_device *device = to_is_device(item);
	ssize_t count;

	mutex_lock(&device->lifecycle_lock);
	count = sysfs_emit(page, "%s\n", device->consumer_id);
	mutex_unlock(&device->lifecycle_lock);
	return count;
}

static ssize_t is_device_consumer_id_store(struct config_item *item,
					   const char *page, size_t count)
{
	struct is_device *device = to_is_device(item);
	int ret = is_device_set_consumer_id(device, page, count);

	return is_store_result(device, ret, count);
}

static ssize_t is_device_providers_show(struct config_item *item, char *page)
{
	struct is_device *device = to_is_device(item);
	ssize_t count;

	mutex_lock(&device->lifecycle_lock);
	count = sysfs_emit(page, "%s\n", device->providers);
	mutex_unlock(&device->lifecycle_lock);
	return count;
}

static ssize_t is_device_providers_store(struct config_item *item,
					 const char *page, size_t count)
{
	struct is_device *device = to_is_device(item);
	int ret = is_device_set_providers(device, page, count);

	return is_store_result(device, ret, count);
}

static ssize_t is_device_provider_bind_store(struct config_item *item,
					      const char *page, size_t count)
{
	struct is_device *device = to_is_device(item);
	int ret = is_device_set_provider_bind(device, page, count);

	return is_store_result(device, ret, count);
}

static ssize_t is_device_placement_sample_size_show(struct config_item *item,
						     char *page)
{
	return sysfs_emit(page, "%u\n",
			  to_is_device(item)->placement_sample_size);
}

static ssize_t is_device_placement_sample_size_store(struct config_item *item,
						      const char *page,
						      size_t count)
{
	struct is_device *device = to_is_device(item);
	int ret = is_device_set_placement_sample_size(device, page, count);

	return is_store_result(device, ret, count);
}

static ssize_t is_device_placement_seed_show(struct config_item *item,
					      char *page)
{
	return sysfs_emit(page, "%llu\n", to_is_device(item)->placement_seed);
}

static ssize_t is_device_placement_seed_store(struct config_item *item,
						const char *page, size_t count)
{
	struct is_device *device = to_is_device(item);
	int ret = is_device_set_placement_seed(device, page, count);

	return is_store_result(device, ret, count);
}

static ssize_t is_device_placement_weight_show(struct config_item *item,
						char *page)
{
	struct is_device *device = to_is_device(item);
	u32 weight = IS_PLACEMENT_WEIGHT_DEFAULT;

	mutex_lock(&device->lifecycle_lock);
	if (device->provider_count &&
	    device->provider_bind_index < device->provider_count)
		weight = device->provider_endpoints[device->provider_bind_index]
				 .placement_weight;
	mutex_unlock(&device->lifecycle_lock);
	return sysfs_emit(page, "%u\n", weight);
}

static ssize_t is_device_placement_weight_store(struct config_item *item,
						 const char *page, size_t count)
{
	struct is_device *device = to_is_device(item);
	int ret = is_device_set_placement_weight(device, page, count);

	return is_store_result(device, ret, count);
}

static ssize_t is_device_provider_address_show(struct config_item *item,
						char *page)
{
	struct is_device *device = to_is_device(item);

	return sysfs_emit(page, "%s\n", device->provider_address);
}

static ssize_t is_device_provider_address_store(struct config_item *item,
						 const char *page,
						 size_t count)
{
	struct is_device *device = to_is_device(item);
	int ret = is_device_set_provider_address(device, page, count);

	return is_store_result(device, ret, count);
}

static ssize_t is_device_provider_port_show(struct config_item *item, char *page)
{
	return sysfs_emit(page, "%u\n", to_is_device(item)->provider_port);
}

static ssize_t is_device_provider_port_store(struct config_item *item,
					      const char *page, size_t count)
{
	struct is_device *device = to_is_device(item);
	int ret = is_device_set_provider_port(device, page, count);

	return is_store_result(device, ret, count);
}

static ssize_t is_device_rdma_device_show(struct config_item *item, char *page)
{
	return sysfs_emit(page, "%s\n", to_is_device(item)->rdma_device);
}

static ssize_t is_device_rdma_device_store(struct config_item *item,
					    const char *page, size_t count)
{
	struct is_device *device = to_is_device(item);
	int ret = is_device_set_rdma_device(device, page, count);

	return is_store_result(device, ret, count);
}

static ssize_t is_device_rdma_port_show(struct config_item *item, char *page)
{
	return sysfs_emit(page, "%u\n", to_is_device(item)->rdma_port);
}

static ssize_t is_device_rdma_port_store(struct config_item *item,
					  const char *page, size_t count)
{
	struct is_device *device = to_is_device(item);
	int ret = is_device_set_rdma_port(device, page, count);

	return is_store_result(device, ret, count);
}

static ssize_t is_device_rdma_numa_node_show(struct config_item *item,
					      char *page)
{
	return sysfs_emit(page, "%d\n", to_is_device(item)->rdma_numa_node);
}

static ssize_t is_device_rdma_numa_node_store(struct config_item *item,
					       const char *page, size_t count)
{
	struct is_device *device = to_is_device(item);
	int ret = is_device_set_rdma_numa_node(device, page, count);

	return is_store_result(device, ret, count);
}

static ssize_t is_device_provider_key_id_show(struct config_item *item,
					       char *page)
{
	return sysfs_emit(page, "%s\n", to_is_device(item)->provider_key_id);
}

static ssize_t is_device_provider_key_id_store(struct config_item *item,
						const char *page, size_t count)
{
	struct is_device *device = to_is_device(item);
	int ret = is_device_set_provider_key_id(device, page, count);

	return is_store_result(device, ret, count);
}

static ssize_t is_device_provider_psk_store(struct config_item *item,
					     const char *page, size_t count)
{
	struct is_device *device = to_is_device(item);
	int ret = is_device_set_provider_psk(device, page, count);

	return is_store_result(device, ret, count);
}

static ssize_t is_device_swap_priority_show(struct config_item *item,
					    char *page)
{
	struct is_device *device = to_is_device(item);
	ssize_t count;

	mutex_lock(&device->lifecycle_lock);
	count = sysfs_emit(page, "%d\n", device->swap_priority);
	mutex_unlock(&device->lifecycle_lock);
	return count;
}

static ssize_t is_device_swap_priority_store(struct config_item *item,
					     const char *page, size_t count)
{
	struct is_device *device = to_is_device(item);
	int ret = is_device_set_swap_priority(device, page, count);

	return is_store_result(device, ret, count);
}

static ssize_t is_device_connection_state_show(struct config_item *item,
						char *page)
{
	const char *name;

	switch (atomic_read(&to_is_device(item)->connection_state)) {
	case IS_CONNECTION_CONNECTING:
		name = "connecting";
		break;
	case IS_CONNECTION_CONNECTED:
		name = "connected";
		break;
	case IS_CONNECTION_DEGRADED:
		name = "degraded";
		break;
	case IS_CONNECTION_REMOTE_LOST:
		name = "remote-lost";
		break;
	default:
		name = "not-connected";
		break;
	}
	return sysfs_emit(page, "%s\n", name);
}

static ssize_t is_device_remote_capacity_bytes_show(struct config_item *item,
						     char *page)
{
	struct is_device *device = to_is_device(item);
	ssize_t count;

	mutex_lock(&device->lifecycle_lock);
	count = sysfs_emit(page, "%llu\n", device->remote_capacity_bytes);
	mutex_unlock(&device->lifecycle_lock);
	return count;
}

static ssize_t is_device_mapped_remote_chunks_show(struct config_item *item,
						    char *page)
{
	struct is_device *device = to_is_device(item);

	return sysfs_emit(page, "%u\n", is_fabric_mapped_chunk_count(device));
}

static ssize_t is_device_mapped_hot_ranges_show(struct config_item *item,
						 char *page)
{
	struct is_device *device = to_is_device(item);
	unsigned int mapped = 0;

	if (device->mode == IS_DEVICE_MODE_BACKED)
		mapped = is_fabric_mapped_chunk_count(device);
	return sysfs_emit(page, "%u\n", mapped);
}

static ssize_t is_device_remote_chunk_placements_show(struct config_item *item,
						      char *page)
{
	return is_rdma_remote_chunk_placements_show(to_is_device(item), page);
}

static ssize_t is_device_provider_exclusions_show(struct config_item *item,
						  char *page)
{
	return is_rdma_provider_exclusions_show(to_is_device(item), page);
}

static ssize_t is_device_backing_state_show(struct config_item *item,
					    char *page)
{
	return sysfs_emit(page, "%s\n",
		is_device_backing_state_name(to_is_device(item)));
}

static ssize_t is_device_operational_state_show(struct config_item *item,
						 char *page)
{
	return sysfs_emit(page, "%s\n",
		is_device_operational_state_name(to_is_device(item)));
}

#define IS_DEVICE_METRIC_SHOW(name, field) \
static ssize_t is_device_##name##_show(struct config_item *item, char *page) \
{ \
	return sysfs_emit(page, "%lld\n", \
		(long long)atomic64_read(&to_is_device(item)->field)); \
}

IS_DEVICE_METRIC_SHOW(backing_failures_total, backing_failures_total)
IS_DEVICE_METRIC_SHOW(backing_retries_total, backing_retries_total)
IS_DEVICE_METRIC_SHOW(backing_degraded_transitions_total,
	backing_degraded_transitions_total)
IS_DEVICE_METRIC_SHOW(provider_timeouts_total, provider_timeouts_total)
IS_DEVICE_METRIC_SHOW(late_rdma_completions_total, late_rdma_completions_total)
IS_DEVICE_METRIC_SHOW(rejected_writes_total, rejected_writes_total)
IS_DEVICE_METRIC_SHOW(local_only_writes_total, local_only_writes_total)
IS_DEVICE_METRIC_SHOW(remote_lost_transitions_total,
	remote_lost_transitions_total)
IS_DEVICE_METRIC_SHOW(backing_invalid_sectors, backing_invalid_sectors)

static ssize_t is_device_last_error_show(struct config_item *item, char *page)
{
	return sysfs_emit(page, "%d\n",
			  READ_ONCE(to_is_device(item)->last_error));
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
	result = is_store_result(device, ret, count);
	mutex_unlock(&device->configfs_lock);
	return result;
}

CONFIGFS_ATTR(is_device_, backing_store);
CONFIGFS_ATTR(is_device_, mode);
CONFIGFS_ATTR(is_device_, remote_only_eligible);
CONFIGFS_ATTR(is_device_, acknowledgement_policy);
CONFIGFS_ATTR(is_device_, capacity_bytes);
CONFIGFS_ATTR(is_device_, provider_failure_deadline_ms);
CONFIGFS_ATTR(is_device_, hot_range_threshold);
CONFIGFS_ATTR(is_device_, hot_range_read_weight);
CONFIGFS_ATTR(is_device_, hot_range_write_weight);
CONFIGFS_ATTR(is_device_, consumer_id);
CONFIGFS_ATTR(is_device_, providers);
CONFIGFS_ATTR_WO(is_device_, provider_bind);
CONFIGFS_ATTR(is_device_, placement_sample_size);
CONFIGFS_ATTR(is_device_, placement_seed);
CONFIGFS_ATTR(is_device_, placement_weight);
CONFIGFS_ATTR(is_device_, provider_address);
CONFIGFS_ATTR(is_device_, provider_port);
CONFIGFS_ATTR(is_device_, rdma_device);
CONFIGFS_ATTR(is_device_, rdma_port);
CONFIGFS_ATTR(is_device_, rdma_numa_node);
CONFIGFS_ATTR(is_device_, provider_key_id);
CONFIGFS_ATTR_WO(is_device_, provider_psk);
CONFIGFS_ATTR(is_device_, swap_priority);
CONFIGFS_ATTR_RO(is_device_, connection_state);
CONFIGFS_ATTR_RO(is_device_, remote_capacity_bytes);
CONFIGFS_ATTR_RO(is_device_, mapped_remote_chunks);
CONFIGFS_ATTR_RO(is_device_, mapped_hot_ranges);
CONFIGFS_ATTR_RO(is_device_, remote_chunk_placements);
CONFIGFS_ATTR_RO(is_device_, provider_exclusions);
CONFIGFS_ATTR_RO(is_device_, backing_state);
CONFIGFS_ATTR_RO(is_device_, operational_state);
CONFIGFS_ATTR_RO(is_device_, backing_failures_total);
CONFIGFS_ATTR_RO(is_device_, backing_retries_total);
CONFIGFS_ATTR_RO(is_device_, backing_degraded_transitions_total);
CONFIGFS_ATTR_RO(is_device_, provider_timeouts_total);
CONFIGFS_ATTR_RO(is_device_, late_rdma_completions_total);
CONFIGFS_ATTR_RO(is_device_, rejected_writes_total);
CONFIGFS_ATTR_RO(is_device_, local_only_writes_total);
CONFIGFS_ATTR_RO(is_device_, remote_lost_transitions_total);
CONFIGFS_ATTR_RO(is_device_, backing_invalid_sectors);
CONFIGFS_ATTR_RO(is_device_, last_error);
CONFIGFS_ATTR(is_device_, state);

static struct configfs_attribute *is_device_attrs[] = {
	&is_device_attr_backing_store,
	&is_device_attr_mode,
	&is_device_attr_remote_only_eligible,
	&is_device_attr_acknowledgement_policy,
	&is_device_attr_capacity_bytes,
	&is_device_attr_provider_failure_deadline_ms,
	&is_device_attr_hot_range_threshold,
	&is_device_attr_hot_range_read_weight,
	&is_device_attr_hot_range_write_weight,
	&is_device_attr_consumer_id,
	&is_device_attr_providers,
	&is_device_attr_provider_bind,
	&is_device_attr_placement_sample_size,
	&is_device_attr_placement_seed,
	&is_device_attr_placement_weight,
	&is_device_attr_provider_address,
	&is_device_attr_provider_port,
	&is_device_attr_rdma_device,
	&is_device_attr_rdma_port,
	&is_device_attr_rdma_numa_node,
	&is_device_attr_provider_key_id,
	&is_device_attr_provider_psk,
	&is_device_attr_swap_priority,
	&is_device_attr_connection_state,
	&is_device_attr_remote_capacity_bytes,
	&is_device_attr_mapped_remote_chunks,
	&is_device_attr_mapped_hot_ranges,
	&is_device_attr_remote_chunk_placements,
	&is_device_attr_provider_exclusions,
	&is_device_attr_backing_state,
	&is_device_attr_operational_state,
	&is_device_attr_backing_failures_total,
	&is_device_attr_backing_retries_total,
	&is_device_attr_backing_degraded_transitions_total,
	&is_device_attr_provider_timeouts_total,
	&is_device_attr_late_rdma_completions_total,
	&is_device_attr_rejected_writes_total,
	&is_device_attr_local_only_writes_total,
	&is_device_attr_remote_lost_transitions_total,
	&is_device_attr_backing_invalid_sectors,
	&is_device_attr_last_error,
	&is_device_attr_state,
	NULL,
};

static void is_device_release(struct config_item *item)
{
	struct is_device *device = to_is_device(item);

	WARN_ON(device->configfs_dependent);
	WARN_ON(device->disk);
	WARN_ON(device->backing_bdev);
	memzero_explicit(device->provider_psk, sizeof(device->provider_psk));
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
