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
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>

#include "infiniswap.h"
#include "is_rdma.h"
#include "is_remote_io_transaction.h"

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
	if (accepting_io && atomic_read(&device->remote_lost))
		accepting_io = false;
	device->state = state;
	device->accepting_opens = accepting_io;
	device->accepting_io = accepting_io;
	spin_unlock_irqrestore(&device->io_lock, flags);
}

void is_device_init(struct is_device *device, const char *name)
{
	mutex_init(&device->configfs_lock);
	mutex_init(&device->lifecycle_lock);
	mutex_init(&device->remote_state_lock);
	spin_lock_init(&device->io_lock);
	spin_lock_init(&device->backing_lock);
	init_waitqueue_head(&device->drain_wait);
	atomic_set(&device->openers, 0);
	atomic_set(&device->inflight, 0);
	atomic_set(&device->connection_state, IS_CONNECTION_NOT_CONNECTED);
	atomic_set(&device->backing_state, IS_BACKING_HEALTHY);
	atomic_set(&device->remote_lost, 0);
	atomic64_set(&device->next_io_generation, 0);
	atomic64_set(&device->io_requests_total, 0);
	atomic64_set(&device->io_completed_total, 0);
	atomic64_set(&device->io_errors_total, 0);
	atomic64_set(&device->authentication_failures_total, 0);
	atomic64_set(&device->admission_rejections_total, 0);
	atomic64_set(&device->backing_failures_total, 0);
	atomic64_set(&device->backing_retries_total, 0);
	atomic64_set(&device->backing_degraded_transitions_total, 0);
	atomic64_set(&device->provider_timeouts_total, 0);
	atomic64_set(&device->late_rdma_completions_total, 0);
	atomic64_set(&device->rejected_writes_total, 0);
	atomic64_set(&device->local_only_writes_total, 0);
	atomic64_set(&device->remote_lost_transitions_total, 0);
	atomic64_set(&device->backing_invalid_sectors, 0);
	device->mode = IS_DEVICE_MODE_UNSET;
	device->transaction_engine.initialized = 0;
	device->acknowledgement_policy = IS_ACKNOWLEDGEMENT_POLICY_STRICT;
	device->provider_failure_deadline_ms =
		IS_PROTOCOL_FAILURE_DEADLINE_DEFAULT_MS;
	device->hot_range_threshold = IS_HOT_RANGE_THRESHOLD_DEFAULT;
	device->hot_range_read_weight = IS_HOT_RANGE_READ_WEIGHT_DEFAULT;
	device->hot_range_write_weight = IS_HOT_RANGE_WRITE_WEIGHT_DEFAULT;
	device->placement_sample_size = IS_PLACEMENT_SAMPLE_DEFAULT;
	device->placement_seed = 0;
	device->provider_count = 0;
	device->provider_bind_index = 0;
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
	switch (device->mode) {
	case IS_DEVICE_MODE_BACKED:
		name = "backed";
		break;
	case IS_DEVICE_MODE_REMOTE_ONLY:
		name = "remote-only";
		break;
	default:
		name = "unset";
		break;
	}
	mutex_unlock(&device->lifecycle_lock);
	return name;
}

int is_device_set_mode(struct is_device *device, const char *buf, size_t count)
{
	enum is_device_mode mode;
	int ret = 0;

	if (sysfs_streq(buf, "backed"))
		mode = IS_DEVICE_MODE_BACKED;
	else if (sysfs_streq(buf, "remote-only"))
		mode = IS_DEVICE_MODE_REMOTE_ONLY;
	else
		return -EINVAL;

	mutex_lock(&device->lifecycle_lock);
	if (!is_device_configurable(device))
		ret = -EBUSY;
	else if (device->mode == IS_DEVICE_MODE_UNSET)
		device->mode = mode;
	else if (device->mode != mode)
		ret = -EINVAL;
	mutex_unlock(&device->lifecycle_lock);
	return ret;
}

int is_device_set_remote_only_eligible(struct is_device *device,
				       const char *buf, size_t count)
{
	bool eligible;
	int ret;

	(void)count;
	ret = kstrtobool(buf, &eligible);
	if (ret)
		return ret;
	mutex_lock(&device->lifecycle_lock);
	if (!is_device_configurable(device))
		ret = -EBUSY;
	else {
		device->remote_only_eligible = eligible;
		ret = 0;
	}
	mutex_unlock(&device->lifecycle_lock);
	return ret;
}

const char *is_device_acknowledgement_policy_name(struct is_device *device)
{
	const char *name;

	mutex_lock(&device->lifecycle_lock);
	if (device->mode == IS_DEVICE_MODE_REMOTE_ONLY) {
		name = "unset";
	} else {
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
	else if (device->mode == IS_DEVICE_MODE_REMOTE_ONLY)
		ret = -EINVAL;
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
	u64 previous;
	int ret;

	(void)count;
	ret = kstrtoull(buf, 0, &threshold);
	if (ret)
		return ret;
	if (!threshold || threshold > S64_MAX)
		return -ERANGE;
	mutex_lock(&device->lifecycle_lock);
	previous = READ_ONCE(device->hot_range_threshold);
	WRITE_ONCE(device->hot_range_threshold, threshold);
	ret = is_rdma_mapping_parameters_changed(device);
	if (ret)
		WRITE_ONCE(device->hot_range_threshold, previous);
	mutex_unlock(&device->lifecycle_lock);
	return ret;
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
	u32 previous;
	int ret;

	(void)count;
	mutex_lock(&device->lifecycle_lock);
	previous = READ_ONCE(device->hot_range_read_weight);
	ret = is_device_set_hot_range_weight(&device->hot_range_read_weight, buf);
	if (!ret)
		ret = is_rdma_mapping_parameters_changed(device);
	if (ret)
		WRITE_ONCE(device->hot_range_read_weight, previous);
	mutex_unlock(&device->lifecycle_lock);
	return ret;
}

int is_device_set_hot_range_write_weight(struct is_device *device,
					 const char *buf, size_t count)
{
	u32 previous;
	int ret;

	(void)count;
	mutex_lock(&device->lifecycle_lock);
	previous = READ_ONCE(device->hot_range_write_weight);
	ret = is_device_set_hot_range_weight(&device->hot_range_write_weight, buf);
	if (!ret)
		ret = is_rdma_mapping_parameters_changed(device);
	if (ret)
		WRITE_ONCE(device->hot_range_write_weight, previous);
	mutex_unlock(&device->lifecycle_lock);
	return ret;
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
	unsigned int count = 0;

	while ((provider = strsep(&cursor, ",")) != NULL) {
		if (!provider[0] || !is_runtime_identifier_valid(provider))
			return false;
		count++;
		if (count > IS_MAX_PROVIDERS)
			return false;
	}
	return count > 0;
}

static void is_sync_bound_provider_mirror(struct is_device *device)
{
	struct is_provider_endpoint *endpoint;

	if (device->provider_count == 0 ||
	    device->provider_bind_index >= device->provider_count)
		return;
	endpoint = &device->provider_endpoints[device->provider_bind_index];
	strscpy(device->provider_address, endpoint->address,
		sizeof(device->provider_address));
	device->provider_port = endpoint->port;
	strscpy(device->rdma_device, endpoint->rdma_device,
		sizeof(device->rdma_device));
	device->rdma_port = endpoint->rdma_port;
	device->rdma_numa_node = endpoint->rdma_numa_node;
	strscpy(device->provider_key_id, endpoint->key_id,
		sizeof(device->provider_key_id));
	memzero_explicit(device->provider_psk, sizeof(device->provider_psk));
	memcpy(device->provider_psk, endpoint->psk, endpoint->psk_size);
	device->provider_psk_size = endpoint->psk_size;
}

static struct is_provider_endpoint *is_bound_provider(struct is_device *device)
{
	if (device->provider_count == 0)
		return NULL;
	if (device->provider_bind_index >= device->provider_count)
		return NULL;
	return &device->provider_endpoints[device->provider_bind_index];
}

int is_device_set_providers(struct is_device *device, const char *buf,
			    size_t count)
{
	char *candidate;
	char *providers;
	char *validation_copy;
	char *parse_copy;
	char *cursor;
	char *provider;
	unsigned int provider_count = 0;
	unsigned int index;
	int ret = 0;

	if (!count || count >= IS_PROVIDER_LIST_SIZE)
		return -ENAMETOOLONG;
	candidate = kstrndup(buf, count, GFP_KERNEL);
	if (!candidate)
		return -ENOMEM;
	providers = strim(candidate);
	validation_copy = kstrdup(providers, GFP_KERNEL);
	if (!validation_copy) {
		ret = -ENOMEM;
		goto out;
	}
	if (!is_provider_list_valid(validation_copy)) {
		ret = -EINVAL;
		goto free_validation;
	}

	parse_copy = kstrdup(providers, GFP_KERNEL);
	if (!parse_copy) {
		ret = -ENOMEM;
		goto free_validation;
	}
	cursor = parse_copy;
	while ((provider = strsep(&cursor, ",")) != NULL) {
		for (index = 0; index < provider_count; index++) {
			if (!strcmp(device->provider_endpoints[index].name,
				    provider)) {
				ret = -EINVAL;
				goto free_parse;
			}
		}
		provider_count++;
	}
	kfree(parse_copy);
	parse_copy = kstrdup(providers, GFP_KERNEL);
	if (!parse_copy) {
		ret = -ENOMEM;
		goto free_validation;
	}

	mutex_lock(&device->lifecycle_lock);
	if (!is_device_configurable(device)) {
		ret = -EBUSY;
		goto unlock;
	}
	memset(device->provider_endpoints, 0, sizeof(device->provider_endpoints));
	device->provider_count = 0;
	device->provider_bind_index = 0;
	cursor = parse_copy;
	while ((provider = strsep(&cursor, ",")) != NULL) {
		struct is_provider_endpoint *endpoint =
			&device->provider_endpoints[device->provider_count];

		strscpy(endpoint->name, provider, sizeof(endpoint->name));
		endpoint->placement_weight = IS_PLACEMENT_WEIGHT_DEFAULT;
		endpoint->rdma_numa_node = NUMA_NO_NODE;
		device->provider_count++;
	}
	strscpy(device->providers, providers, sizeof(device->providers));
	if (device->placement_sample_size > device->provider_count)
		device->placement_sample_size = device->provider_count ?
			device->provider_count : IS_PLACEMENT_SAMPLE_DEFAULT;
	is_sync_bound_provider_mirror(device);
unlock:
	mutex_unlock(&device->lifecycle_lock);

free_parse:
	kfree(parse_copy);
free_validation:
	kfree(validation_copy);
out:
	kfree(candidate);
	return ret;
}

int is_device_set_provider_bind(struct is_device *device, const char *buf,
				size_t count)
{
	char *candidate;
	char *name;
	unsigned int index;
	int ret = 0;

	if (!count || count >= IS_PROVIDER_NAME_SIZE)
		return -ENAMETOOLONG;
	candidate = kstrndup(buf, count, GFP_KERNEL);
	if (!candidate)
		return -ENOMEM;
	name = strim(candidate);
	if (!is_runtime_identifier_valid(name)) {
		ret = -EINVAL;
		goto out;
	}

	mutex_lock(&device->lifecycle_lock);
	if (!is_device_configurable(device)) {
		ret = -EBUSY;
		goto unlock;
	}
	for (index = 0; index < device->provider_count; index++) {
		if (!strcmp(device->provider_endpoints[index].name, name)) {
			device->provider_bind_index = index;
			is_sync_bound_provider_mirror(device);
			goto unlock;
		}
	}
	ret = -ENOENT;
unlock:
	mutex_unlock(&device->lifecycle_lock);
out:
	kfree(candidate);
	return ret;
}

int is_device_set_placement_sample_size(struct is_device *device,
					const char *buf, size_t count)
{
	u32 sample_size;
	int ret;

	(void)count;
	ret = kstrtou32(buf, 0, &sample_size);
	if (ret)
		return ret;
	mutex_lock(&device->lifecycle_lock);
	if (!is_device_configurable(device))
		ret = -EBUSY;
	else if (sample_size == 0 || sample_size > IS_MAX_PROVIDERS ||
		 (device->provider_count && sample_size > device->provider_count))
		ret = -ERANGE;
	else {
		device->placement_sample_size = sample_size;
		ret = 0;
	}
	mutex_unlock(&device->lifecycle_lock);
	return ret;
}

int is_device_set_placement_seed(struct is_device *device, const char *buf,
				 size_t count)
{
	u64 seed;
	int ret;

	(void)count;
	ret = kstrtou64(buf, 0, &seed);
	if (ret)
		return ret;
	mutex_lock(&device->lifecycle_lock);
	if (!is_device_configurable(device))
		ret = -EBUSY;
	else {
		device->placement_seed = seed;
		ret = 0;
	}
	mutex_unlock(&device->lifecycle_lock);
	return ret;
}

int is_device_set_placement_weight(struct is_device *device, const char *buf,
				   size_t count)
{
	struct is_provider_endpoint *endpoint;
	u32 weight;
	int ret;

	(void)count;
	ret = kstrtou32(buf, 0, &weight);
	if (ret)
		return ret;
	if (weight < IS_PLACEMENT_WEIGHT_MIN || weight > IS_PLACEMENT_WEIGHT_MAX)
		return -ERANGE;
	mutex_lock(&device->lifecycle_lock);
	endpoint = is_bound_provider(device);
	if (!is_device_configurable(device))
		ret = -EBUSY;
	else if (!endpoint)
		ret = -EINVAL;
	else {
		endpoint->placement_weight = weight;
		ret = 0;
	}
	mutex_unlock(&device->lifecycle_lock);
	return ret;
}

static int is_device_set_config_string(struct is_device *device,
				       const char *buf, size_t count,
				       char *target, size_t target_size,
				       bool identifier)
{
	char *candidate;
	char *value;
	struct is_provider_endpoint *endpoint;
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
	else {
		strscpy(target, value, target_size);
		endpoint = is_bound_provider(device);
		if (endpoint) {
			if (target == device->provider_address)
				strscpy(endpoint->address, value,
					sizeof(endpoint->address));
			else if (target == device->rdma_device)
				strscpy(endpoint->rdma_device, value,
					sizeof(endpoint->rdma_device));
			else if (target == device->provider_key_id)
				strscpy(endpoint->key_id, value,
					sizeof(endpoint->key_id));
			endpoint->configured =
				endpoint->address[0] && endpoint->port &&
				endpoint->rdma_device[0] && endpoint->rdma_port &&
				endpoint->key_id[0] &&
				endpoint->psk_size >= IS_PSK_MIN_SIZE;
		}
	}
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
	struct is_provider_endpoint *endpoint;
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
		endpoint = is_bound_provider(device);
		if (endpoint) {
			endpoint->port = port;
			endpoint->configured =
				endpoint->address[0] && endpoint->port &&
				endpoint->rdma_device[0] && endpoint->rdma_port &&
				endpoint->key_id[0] &&
				endpoint->psk_size >= IS_PSK_MIN_SIZE;
		}
		ret = 0;
	}
	mutex_unlock(&device->lifecycle_lock);
	return ret;
}

int is_device_set_rdma_port(struct is_device *device, const char *buf,
			    size_t count)
{
	struct is_provider_endpoint *endpoint;
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
		endpoint = is_bound_provider(device);
		if (endpoint) {
			endpoint->rdma_port = port;
			endpoint->configured =
				endpoint->address[0] && endpoint->port &&
				endpoint->rdma_device[0] && endpoint->rdma_port &&
				endpoint->key_id[0] &&
				endpoint->psk_size >= IS_PSK_MIN_SIZE;
		}
		ret = 0;
	}
	mutex_unlock(&device->lifecycle_lock);
	return ret;
}

int is_device_set_rdma_numa_node(struct is_device *device, const char *buf,
				 size_t count)
{
	struct is_provider_endpoint *endpoint;
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
		endpoint = is_bound_provider(device);
		if (endpoint)
			endpoint->rdma_numa_node = numa_node;
		ret = 0;
	}
	mutex_unlock(&device->lifecycle_lock);
	return ret;
}

int is_device_set_provider_psk(struct is_device *device, const char *buf,
			       size_t count)
{
	struct is_provider_endpoint *endpoint;
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
		endpoint = is_bound_provider(device);
		if (endpoint) {
			memzero_explicit(endpoint->psk, sizeof(endpoint->psk));
			memcpy(endpoint->psk, secret, encoded_size / 2U);
			endpoint->psk_size = encoded_size / 2U;
			endpoint->configured =
				endpoint->address[0] && endpoint->port &&
				endpoint->rdma_device[0] && endpoint->rdma_port &&
				endpoint->key_id[0] &&
				endpoint->psk_size >= IS_PSK_MIN_SIZE;
		}
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

const char *is_device_operational_state_name(struct is_device *device)
{
	if (atomic_read(&device->remote_lost))
		return "remote-lost";
	return is_device_backing_state_name(device);
}

bool is_device_mark_remote_connected(struct is_device *device)
{
	unsigned long flags;
	bool connected = false;

	mutex_lock(&device->remote_state_lock);
	spin_lock_irqsave(&device->io_lock, flags);
	if (!atomic_read(&device->remote_lost)) {
		atomic_set(&device->connection_state, IS_CONNECTION_CONNECTED);
		WRITE_ONCE(device->last_error, 0);
		connected = true;
	}
	spin_unlock_irqrestore(&device->io_lock, flags);
	mutex_unlock(&device->remote_state_lock);
	return connected;
}

void is_device_mark_remote_lost_locked(struct is_device *device, int error)
{
	unsigned long flags;
	bool transitioned = false;

	if (device->mode != IS_DEVICE_MODE_REMOTE_ONLY)
		return;
	spin_lock_irqsave(&device->io_lock, flags);
	if (atomic_cmpxchg(&device->remote_lost, 0, 1) == 0) {
		device->accepting_opens = false;
		device->accepting_io = false;
		atomic_set(&device->connection_state, IS_CONNECTION_REMOTE_LOST);
		WRITE_ONCE(device->last_error, error > 0 ? error : EIO);
		transitioned = true;
	}
	spin_unlock_irqrestore(&device->io_lock, flags);
	if (!transitioned)
		return;
	atomic64_inc(&device->remote_lost_transitions_total);
	wake_up_all(&device->drain_wait);
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
	if (device->mode == IS_DEVICE_MODE_REMOTE_ONLY) {
		ret = -EINVAL;
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
	if (atomic_dec_return(&device->inflight) == 0)
		WRITE_ONCE(device->oldest_inflight_started, 0);
	wake_up_all(&device->drain_wait);
}

static void is_complete_request(struct is_device *device,
				struct request *request, blk_status_t status)
{
	atomic64_inc(&device->io_completed_total);
	if (status != BLK_STS_OK)
		atomic64_inc(&device->io_errors_total);
	blk_mq_end_request(request, status);
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
	is_complete_request(device, request, status);
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
	struct is_remote_io_transaction *transaction;
	atomic_t pending_bios;
	atomic_t local_status;
	unsigned int backing_submissions;
	bool prepared;
	struct work_struct local_work;
	struct bio *backing_bio;
	struct is_rdma_io rdma_io;
	struct page *owned_pages[IS_RDMA_MAX_SEGMENTS];
	unsigned int owned_page_count;
};

static struct is_device *is_transaction_engine_device(
	struct is_remote_io_transaction_engine *engine)
{
	return container_of(engine, struct is_device, transaction_engine);
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

	if (generation == remote->rdma_io.generation && !status && !cancelled &&
	    !remote->rdma_io.write)
		status = is_copy_owned_pages_to_request(remote);
	is_remote_io_transaction_rdma_completed(remote->transaction, generation,
		status, cancelled);
}

static void is_remote_transport_release(void *context, u64 generation)
{
	struct is_remote_request *remote = context;

	is_remote_io_transaction_rdma_released(remote->transaction, generation);
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
	is_remote_io_transaction_backing_completed(remote->transaction,
		blk_status_to_errno(status));
}

static void is_remote_backing_end_io(struct bio *bio)
{
	struct is_remote_request *remote = bio->bi_private;
	blk_status_t status = bio->bi_status;

	bio_put(bio);
	is_remote_local_complete(remote, status);
}

static struct bio *is_alloc_owned_bio(struct is_remote_request *remote)
{
	struct bio *bio;

#ifdef INFINISWAP_HAVE_BIO_ALLOC_CLONE
	bio = bio_alloc_bioset(remote->device->backing_bdev,
		remote->owned_page_count, remote->command_flags, GFP_KERNEL,
		&remote->device->bio_set);
#else
	bio = bio_alloc_bioset(GFP_KERNEL, remote->owned_page_count,
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
			alloc_page(GFP_KERNEL) :
			alloc_pages_node(remote->device->rdma_numa_node,
				GFP_KERNEL, 0);
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

static void is_submit_remote_local_work(struct work_struct *work)
{
	struct is_remote_request *remote = container_of(
		work, struct is_remote_request, local_work);
	struct bio *source;
	blk_status_t status = BLK_STS_OK;

	if (remote->rdma_io.write && remote->prepared) {
		if (is_submit_owned_backing_write(remote))
			is_remote_local_complete(remote, BLK_STS_RESOURCE);
		return;
	}
	atomic_set(&remote->local_status, BLK_STS_OK);
	atomic_set(&remote->pending_bios, 1);
	if (!remote->rdma_io.write &&
	    !is_backing_range_valid(remote->device, remote->sector,
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

int is_remote_io_transaction_adapter_allocate(
	struct is_remote_io_transaction_engine *engine,
	const struct is_remote_io_transaction_spec *spec,
	size_t transaction_size, size_t transaction_alignment,
	struct is_remote_io_transaction_allocation *allocation)
{
	struct is_device *device = is_transaction_engine_device(engine);
	struct request *request = spec->payload;
	struct is_remote_request *remote;
	size_t transaction_offset = ALIGN(sizeof(*remote), transaction_alignment);
	size_t allocation_size;

	if (transaction_size > SIZE_MAX - transaction_offset)
		return -EOVERFLOW;
	allocation_size = transaction_offset + transaction_size;
	if (device->rdma_numa_node == NUMA_NO_NODE)
		remote = kzalloc(allocation_size, GFP_KERNEL);
	else
		remote = kzalloc_node(allocation_size, GFP_KERNEL,
			device->rdma_numa_node);
	if (!remote)
		return -ENOMEM;

	remote->device = device;
	remote->request = request;
	remote->sector = (sector_t)spec->sector;
	remote->bytes = spec->bytes;
	remote->command_flags = request->cmd_flags;
	remote->transaction = (struct is_remote_io_transaction *)
		((u8 *)remote + transaction_offset);
	remote->rdma_io.sector = remote->sector;
	remote->rdma_io.bytes = remote->bytes;
	remote->rdma_io.write =
		spec->direction == IS_REMOTE_IO_TRANSACTION_WRITE;
	remote->rdma_io.generation = spec->generation;
	remote->rdma_io.context = remote;
	remote->rdma_io.complete = is_remote_rdma_complete;
	remote->rdma_io.release = is_remote_transport_release;
	INIT_WORK(&remote->local_work, is_submit_remote_local_work);
	allocation->transaction_storage = remote->transaction;
	allocation->adapter_context = remote;
	return 0;
}

int is_remote_io_transaction_adapter_prepare(
	struct is_remote_io_transaction_engine *engine,
	const struct is_remote_io_transaction_spec *spec, bool prepare_backing,
	struct is_remote_io_transaction *transaction, void *adapter_context)
{
	struct is_remote_request *remote = adapter_context;
	int status;

	(void)engine;
	(void)spec;
	(void)transaction;
	status = is_prepare_owned_pages(remote, remote->rdma_io.write);
	if (!status && prepare_backing) {
		remote->backing_bio = is_build_owned_bio(remote);
		if (!remote->backing_bio)
			status = -ENOMEM;
	}
	if (!status)
		remote->prepared = true;
	return status;
}

int is_remote_io_transaction_adapter_submit_backing(
	struct is_remote_io_transaction_engine *engine,
	const struct is_remote_io_transaction_spec *spec,
	struct is_remote_io_transaction *transaction, void *adapter_context)
{
	struct is_remote_request *remote = adapter_context;

	(void)engine;
	(void)spec;
	(void)transaction;
	if (remote->backing_submissions++)
		atomic64_inc(&remote->device->backing_retries_total);
	if (remote->rdma_io.write && remote->prepared) {
		atomic_set(&remote->local_status, BLK_STS_OK);
		atomic_set(&remote->pending_bios, 1);
	}
	if (!remote->rdma_io.write || !remote->prepared ||
	    remote->backing_submissions > 1)
		return queue_work(remote->device->ordered_backing_wq,
			&remote->local_work) ? 0 : -EIO;

	submit_bio_noacct(remote->backing_bio);
	remote->backing_bio = NULL;
	return 0;
}

int is_remote_io_transaction_adapter_submit_rdma(
	struct is_remote_io_transaction_engine *engine,
	const struct is_remote_io_transaction_spec *spec,
	struct is_remote_io_transaction *transaction, void *adapter_context)
{
	struct is_remote_request *remote = adapter_context;

	(void)engine;
	(void)spec;
	(void)transaction;
	return is_rdma_submit(remote->device, &remote->rdma_io);
}

void is_remote_io_transaction_adapter_complete(
	struct is_remote_io_transaction_engine *engine,
	const struct is_remote_io_transaction_spec *spec, void *adapter_context,
	int status)
{
	struct is_device *device = is_transaction_engine_device(engine);
	struct request *request = spec->payload;

	(void)adapter_context;
	is_complete_request(device, request,
		status ? errno_to_blk_status(status) : BLK_STS_OK);
}

void is_remote_io_transaction_adapter_backing_degraded(
	struct is_remote_io_transaction_engine *engine,
	const struct is_remote_io_transaction_spec *spec, void *adapter_context)
{
	(void)adapter_context;
	is_degrade_backing(is_transaction_engine_device(engine),
		(sector_t)spec->sector, spec->bytes, EIO);
}

void is_remote_io_transaction_adapter_mark_local_only(
	struct is_remote_io_transaction_engine *engine,
	const struct is_remote_io_transaction_spec *spec, void *adapter_context)
{
	(void)spec;
	(void)adapter_context;
	atomic64_inc(&is_transaction_engine_device(engine)->local_only_writes_total);
}

void is_remote_io_transaction_adapter_release(
	struct is_remote_io_transaction_engine *engine,
	const struct is_remote_io_transaction_spec *spec, void *adapter_context)
{
	struct is_remote_request *remote = adapter_context;
	unsigned int index;

	(void)engine;
	(void)spec;
	if (remote->backing_bio)
		bio_put(remote->backing_bio);
	for (index = 0; index < remote->owned_page_count; index++)
		__free_page(remote->owned_pages[index]);
	kfree(remote);
}

void is_remote_io_transaction_adapter_settle(
	struct is_remote_io_transaction_engine *engine,
	const struct is_remote_io_transaction_spec *spec)
{
	(void)spec;
	is_finish_inflight(is_transaction_engine_device(engine));
}

void is_remote_io_transaction_adapter_invariant(
	struct is_remote_io_transaction_engine *engine,
	const struct is_remote_io_transaction_spec *spec, void *adapter_context,
	enum is_remote_io_transaction_event event)
{
	struct is_device *device = is_transaction_engine_device(engine);

	(void)adapter_context;
	if (spec) {
		pr_warn(IS_DRIVER_NAME
			": illegal Remote I/O Transaction event %u for generation %llu\n",
			(unsigned int)event, spec->generation);
	} else {
		pr_warn(IS_DRIVER_NAME
			": refused to destroy active Remote I/O Transaction engine for %s\n",
			device->name);
	}
}

static bool is_dispatch_remote(struct is_device *device,
			       struct request *request)
{
	unsigned int bytes = blk_rq_bytes(request);
	sector_t sector = blk_rq_pos(request);
	bool write = req_op(request) == REQ_OP_WRITE;

	if (device->mode == IS_DEVICE_MODE_BACKED)
		is_rdma_note_activity(device, sector, bytes, write);
	if (!device->remote_chunks ||
	    atomic_read(&device->connection_state) != IS_CONNECTION_CONNECTED)
		return false;

	{
		const struct is_remote_io_transaction_spec spec = {
			.direction = write ? IS_REMOTE_IO_TRANSACTION_WRITE :
				IS_REMOTE_IO_TRANSACTION_READ,
			.sector = sector,
			.bytes = bytes,
			.generation = atomic64_inc_return(
				&device->next_io_generation),
			.payload = request,
		};

		is_remote_io_transaction_start(&device->transaction_engine, &spec);
	}
	return true;
}

static void is_complete_accepted_request(struct is_device *device,
					 struct request *request,
					 blk_status_t status)
{
	is_complete_request(device, request, status);
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
	if (accepted && device->mode == IS_DEVICE_MODE_BACKED && write &&
	    !is_backing_healthy(device)) {
		accepted = false;
		rejected_degraded = true;
	}
	if (accepted) {
		if (atomic_inc_return(&device->inflight) == 1)
			WRITE_ONCE(device->oldest_inflight_started, jiffies);
	}
	spin_unlock_irqrestore(&device->io_lock, flags);
	if (rejected_degraded)
		atomic64_inc(&device->rejected_writes_total);
	return accepted;
}

static void is_issue_flush(struct is_device *device, struct request *request)
{
	blk_status_t status;

	if (device->mode == IS_DEVICE_MODE_REMOTE_ONLY) {
		unsigned long flags;

		status = errno_to_blk_status(is_rdma_flush(device));
		spin_lock_irqsave(&device->io_lock, flags);
		if (atomic_read(&device->remote_lost))
			status = BLK_STS_IOERR;
		is_complete_request(device, request, status);
		is_finish_inflight(device);
		spin_unlock_irqrestore(&device->io_lock, flags);
		return;
	}
	status = errno_to_blk_status(blkdev_issue_flush(device->backing_bdev));
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
	} else if (device->mode == IS_DEVICE_MODE_REMOTE_ONLY) {
		is_complete_accepted_request(device, request, BLK_STS_IOERR);
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

	atomic64_inc(&device->io_requests_total);
	blk_mq_start_request(request);
	if (req_op(request) != REQ_OP_READ &&
	    req_op(request) != REQ_OP_WRITE &&
	    req_op(request) != REQ_OP_FLUSH) {
		is_complete_request(device, request, BLK_STS_NOTSUPP);
		return BLK_STS_OK;
	}
	if (!is_accept_request(device, request)) {
		is_complete_request(device, request, BLK_STS_IOERR);
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
	struct request_queue *queue = device->disk->queue;

	if (device->mode == IS_DEVICE_MODE_BACKED) {
		struct request_queue *backing_queue =
			bdev_get_queue(device->backing_bdev);

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
			min_t(unsigned int, queue_max_segment_size(backing_queue),
			      PAGE_SIZE));
		blk_queue_write_cache(queue,
			test_bit(QUEUE_FLAG_WC, &backing_queue->queue_flags),
			test_bit(QUEUE_FLAG_FUA, &backing_queue->queue_flags));
	} else {
		blk_queue_logical_block_size(queue, IS_SECTOR_SIZE);
		blk_queue_physical_block_size(queue, PAGE_SIZE);
		blk_queue_max_hw_sectors(queue,
			IS_RDMA_MAX_SEGMENTS * (PAGE_SIZE >> 9));
		blk_queue_max_segments(queue, IS_RDMA_MAX_SEGMENTS);
		blk_queue_max_segment_size(queue, PAGE_SIZE);
		blk_queue_write_cache(queue, true, false);
	}
	blk_queue_max_discard_sectors(queue, 0);
	blk_queue_max_write_zeroes_sectors(queue, 0);
	blk_queue_chunk_sectors(queue, IS_REMOTE_CHUNK_BYTES / IS_SECTOR_SIZE);
	blk_queue_flag_set(QUEUE_FLAG_NONROT, queue);
	blk_queue_flag_clear(QUEUE_FLAG_ADD_RANDOM, queue);
}

static int is_release_resources(struct is_device *device)
{
	int ret;

	if (device->transaction_engine.initialized) {
		ret = is_remote_io_transaction_engine_destroy(
			&device->transaction_engine);
		if (ret)
			return ret;
	}
	is_rdma_stop(device);
	if (device->remote_chunks) {
		ret = is_remote_chunk_module_destroy(device->remote_chunks);
		if (ret)
			return ret;
		device->remote_chunks = NULL;
	}
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
	return 0;
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
	bool remote_only;
	bool remote_configured;
	bool remote_state_locked = false;
	int ret;

	mutex_lock(&device->lifecycle_lock);
	if (!is_device_configurable(device)) {
		ret = device->state == IS_DEVICE_ACTIVE ? -EALREADY : -EINVAL;
		goto out;
	}
	remote_only = device->mode == IS_DEVICE_MODE_REMOTE_ONLY;
	remote_configured =
		!(device->capacity_bytes % IS_REMOTE_CHUNK_BYTES) &&
		device->capacity_bytes <=
			IS_REMOTE_CHUNK_BYTES * IS_MAX_REMOTE_CHUNKS &&
		device->provider_count > 0 &&
		device->placement_sample_size >= 1 &&
		device->placement_sample_size <= device->provider_count;
	if (remote_configured) {
		unsigned int index;

		for (index = 0; index < device->provider_count; index++) {
			if (!device->provider_endpoints[index].configured) {
				remote_configured = false;
				break;
			}
		}
	}
	if (!remote_configured && device->provider_count == 0) {
		remote_configured =
			!(device->capacity_bytes % IS_REMOTE_CHUNK_BYTES) &&
			device->capacity_bytes <=
				IS_REMOTE_CHUNK_BYTES * IS_MAX_REMOTE_CHUNKS &&
			device->provider_address[0] && device->provider_port &&
			device->rdma_device[0] && device->rdma_port &&
			device->provider_key_id[0] &&
			device->provider_psk_size >= IS_PSK_MIN_SIZE;
	}
	if (atomic_read(&device->remote_lost) ||
	    (device->mode == IS_DEVICE_MODE_BACKED &&
	     !is_backing_healthy(device))) {
		ret = -EUCLEAN;
		goto out;
	}
	if ((device->mode != IS_DEVICE_MODE_BACKED && !remote_only) ||
	    !device->capacity_sectors || !device->consumer_id[0] ||
	    !device->providers[0] || device->swap_priority < 0) {
		ret = -EINVAL;
		goto out;
	}
	if (remote_only) {
		if (!device->remote_only_eligible || device->backing_path[0] ||
		    !remote_configured) {
			ret = -EINVAL;
			goto out;
		}
	} else if (!device->backing_path[0] ||
		   (device->acknowledgement_policy ==
			IS_ACKNOWLEDGEMENT_POLICY_REMOTE_FIRST &&
		    !remote_configured)) {
		ret = -EINVAL;
		goto out;
	}
	previous_state = device->state;

	{
		u64 remote_chunk_count = device->capacity_bytes /
			IS_REMOTE_CHUNK_BYTES +
			!!(device->capacity_bytes % IS_REMOTE_CHUNK_BYTES);
		const struct is_remote_chunk_config config = {
			.mode = remote_only ? IS_REMOTE_CHUNK_MODE_REMOTE_ONLY :
				IS_REMOTE_CHUNK_MODE_BACKED,
			.chunk_count = remote_configured ?
				(unsigned int)remote_chunk_count : 1U,
			.hot_policy = {
				.threshold = device->hot_range_threshold,
				.read_weight = device->hot_range_read_weight,
				.write_weight = device->hot_range_write_weight,
			},
		};

		ret = is_remote_chunk_module_create(&config,
			&device->remote_chunks);
		if (ret)
			goto out;
	}

	if (!remote_only) {
		ret = is_open_backing_store(device);
		if (ret)
			goto release_resources;
		ret = is_validate_open_backing_store(device);
		if (ret)
			goto release_resources;

		ret = bioset_init(&device->bio_set, IS_BIO_POOL_SIZE, 0,
				  BIOSET_NEED_RESCUER | BIOSET_NEED_BVECS);
		if (ret)
			goto release_resources;
		device->bioset_initialized = true;

		device->backing_invalid_bitmap = kvcalloc(
			BITS_TO_LONGS(device->capacity_sectors),
			sizeof(unsigned long), GFP_KERNEL);
		if (!device->backing_invalid_bitmap) {
			ret = -ENOMEM;
			goto release_resources;
		}
		atomic64_set(&device->backing_invalid_sectors, 0);
	}

	device->ordered_backing_wq = alloc_ordered_workqueue("infiniswap-io",
							WQ_MEM_RECLAIM);
	if (!device->ordered_backing_wq) {
		ret = -ENOMEM;
		goto release_resources;
	}

	ret = is_rdma_start(device);
	if (ret) {
		WRITE_ONCE(device->last_error, -ret);
		if (remote_only)
			goto release_resources;
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
	if (remote_only) {
		mutex_lock(&device->remote_state_lock);
		remote_state_locked = true;
	}
	if (remote_only &&
	    (atomic_read(&device->remote_lost) ||
	     atomic_read(&device->connection_state) != IS_CONNECTION_CONNECTED ||
	     is_rdma_remote_capacity_bytes(device) != device->capacity_bytes)) {
		ret = -ENOTCONN;
		goto release_resources;
	}

	ret = is_remote_io_transaction_engine_init(&device->transaction_engine,
		remote_only ? IS_REMOTE_IO_TRANSACTION_REMOTE_ONLY :
		(device->acknowledgement_policy ==
			IS_ACKNOWLEDGEMENT_POLICY_REMOTE_FIRST ?
			IS_REMOTE_IO_TRANSACTION_BACKED_REMOTE_FIRST :
			IS_REMOTE_IO_TRANSACTION_BACKED_STRICT));
	if (ret)
		goto release_resources;

	is_set_io_state(device, IS_DEVICE_ACTIVE, true);
	ret = add_disk(device->disk);
	if (ret) {
		is_set_io_state(device, previous_state, false);
		goto release_resources;
	}
	device->disk_added = true;
	if (remote_state_locked) {
		mutex_unlock(&device->remote_state_lock);
		remote_state_locked = false;
	}
	pr_info(IS_DRIVER_NAME ": activated %s in %s mode (%llu bytes)\n",
		device->name, remote_only ? "remote-only" : "backed",
		device->capacity_bytes);
	goto out;

release_resources:
	if (remote_state_locked)
		mutex_unlock(&device->remote_state_lock);
	is_set_io_state(device, previous_state, false);
	WARN_ON(is_release_resources(device));
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
	if (device->remote_chunks) {
		int ret = is_remote_chunk_module_quiesce(device->remote_chunks);

		if (ret && ret != -EALREADY)
			return ret;
	}
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

	ret = is_release_resources(device);
	if (ret)
		goto out;
	is_set_io_state(device, IS_DEVICE_STOPPED, false);
	pr_info(IS_DRIVER_NAME ": stopped %s\n", device->name);
out:
	mutex_unlock(&device->lifecycle_lock);
	return ret;
}
