// SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause
#include <crypto/hash.h>
#include <linux/bitmap.h>
#include <linux/completion.h>
#include <linux/crypto.h>
#include <linux/dma-direction.h>
#include <linux/inet.h>
#include <linux/jiffies.h>
#include <linux/list.h>
#include <linux/net.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>
#include <net/net_namespace.h>
#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>

#include "infiniswap.h"
#include "infiniswap_placement.h"
#include "is_rdma.h"

#define IS_FABRIC_NO_SESSION IS_MAX_PROVIDERS

#define IS_CHUNK_BYTES IS_REMOTE_CHUNK_BYTES
#define IS_SECTORS_PER_CHUNK (IS_CHUNK_BYTES >> 9)
#define IS_RDMA_CONTROL_TIMEOUT_MS 5000U
#define IS_RDMA_CONTROL_WR_COUNT 16U
#define IS_RDMA_AUTH_DOMAIN "Infiniswap control authentication v1"
#define IS_RDMA_CAPABILITIES \
	(IS_PROTOCOL_CAP_BACKED | IS_PROTOCOL_CAP_REMOTE_ONLY | \
	 IS_PROTOCOL_CAP_OPPORTUNISTIC_POOL | IS_PROTOCOL_CAP_COMMITTED_POOL | \
	 IS_PROTOCOL_CAP_FAILURE_DEADLINE | IS_PROTOCOL_CAP_STATUS | \
	 IS_PROTOCOL_CAP_AUTH_HMAC_SHA256)
#define IS_RDMA_BACKED_REQUIRED_CAPABILITIES \
	(IS_PROTOCOL_CAP_FAILURE_DEADLINE | IS_PROTOCOL_CAP_STATUS | \
	 IS_PROTOCOL_CAP_AUTH_HMAC_SHA256)
#define IS_RDMA_REMOTE_ONLY_REQUIRED_CAPABILITIES \
	(IS_PROTOCOL_CAP_REMOTE_ONLY | IS_PROTOCOL_CAP_COMMITTED_POOL | \
	 IS_PROTOCOL_CAP_FAILURE_DEADLINE | IS_PROTOCOL_CAP_STATUS | \
	 IS_PROTOCOL_CAP_AUTH_HMAC_SHA256)

enum is_rdma_control_state {
	IS_RDMA_CONTROL_CONNECTING = 0,
	IS_RDMA_CONTROL_WAIT_CHALLENGE,
	IS_RDMA_CONTROL_WAIT_ACCEPT,
	IS_RDMA_CONTROL_WAIT_STATUS,
	IS_RDMA_CONTROL_WAIT_RESERVATION,
	IS_RDMA_CONTROL_WAIT_HEARTBEAT,
	IS_RDMA_CONTROL_READY,
	IS_RDMA_CONTROL_FAILED,
	IS_RDMA_CONTROL_STOPPING,
};

enum is_remote_chunk_state {
	IS_REMOTE_CHUNK_UNMAPPED = 0,
	IS_REMOTE_CHUNK_MAPPING,
	IS_REMOTE_CHUNK_MAPPED,
	IS_REMOTE_CHUNK_EVICTING,
};

struct is_fabric_remote_chunk {
	enum is_remote_chunk_state state;
	unsigned int session_index;
	u32 provider_chunk_id;
	u64 remote_address;
	u32 remote_key;
	atomic64_t activity;
	atomic_t inflight;
};

struct is_rdma_fabric {
	struct is_device *device;
	unsigned int session_count;
	struct is_rdma_session *sessions[IS_MAX_PROVIDERS];
	unsigned int chunk_count;
	spinlock_t chunk_lock;
	struct is_fabric_remote_chunk chunks[IS_MAX_REMOTE_CHUNKS];
	unsigned long *valid_sectors;
	unsigned int mapped_per_session[IS_MAX_PROVIDERS];
	struct work_struct mapping_work;
	struct work_struct reservation_work;
	wait_queue_head_t control_wait;
	atomic_t sessions_awaiting_status;
	bool reservation_complete;
	bool reservation_failed;
	bool reservation_started;
	int reservation_error;
	bool stopping;
	u64 placement_rng_state;
	unsigned int reservation_next_logical;
	unsigned int planned_count;
	unsigned int next_resolve_index;
	DECLARE_BITMAP(failed_sessions, IS_MAX_PROVIDERS);
	struct delayed_work start_next_session_work;
};

struct is_rdma_session;

struct is_control_completion {
	struct ib_cqe cqe;
	struct is_rdma_session *session;
};

struct is_rdma_operation {
	struct ib_cqe cqe;
	struct list_head list;
	struct is_rdma_session *session;
	struct is_rdma_io *io;
	struct is_fabric_remote_chunk *chunk;
	struct ib_rdma_wr wr;
	struct ib_sge sges[IS_RDMA_MAX_SEGMENTS];
	u64 dma_addresses[IS_RDMA_MAX_SEGMENTS];
	enum dma_data_direction direction;
	struct delayed_work deadline_work;
	refcount_t references;
	unsigned int mapped_segments;
	unsigned int logical_chunk_id;
	atomic_t callback_complete;
	atomic_t timed_out;
};

struct is_rdma_session {
	struct is_rdma_fabric *fabric;
	unsigned int provider_index;
	struct is_device *device;
	struct rdma_cm_id *cm_id;
	struct ib_pd *pd;
	struct ib_cq *cq;
	struct ib_qp *qp;
	bool qp_has_work;
	bool connect_started;
	struct completion disconnect_complete;

	struct mutex control_lock;
	spinlock_t chunk_lock;
	spinlock_t operation_lock;
	struct list_head operations;
	wait_queue_head_t send_wait;
	wait_queue_head_t chunk_wait;
	wait_queue_head_t control_wait;
	atomic_t send_busy;
	atomic_t rdma_reads_inflight;
	atomic_t operation_objects;
	atomic_t disconnect_started;
	bool failure_started;
	bool disconnect_deferred;
	bool stopping;
	bool ever_connected;
	bool resolve_started;
	bool reservation_complete;
	enum is_rdma_control_state control_state;

	void *send_frame;
	void *recv_frame;
	void *received_frame;
	u64 send_dma;
	u64 recv_dma;
	size_t send_size;
	size_t received_size;
	struct ib_sge send_sge;
	struct ib_sge recv_sge;
	struct ib_send_wr send_wr;
	struct ib_recv_wr recv_wr;
	struct is_control_completion send_completion;
	struct is_control_completion recv_completion;

	struct workqueue_struct *control_wq;
	struct work_struct connect_work;
	struct delayed_work hello_work;
	struct work_struct receive_work;
	struct work_struct mapping_work;
	struct work_struct release_work;
	struct work_struct failure_work;
	struct delayed_work heartbeat_work;
	struct delayed_work control_deadline_work;
	unsigned long control_deadline_expires;
	bool control_deadline_armed;

	u8 hello_frame[IS_PROTOCOL_MAX_FRAME_SIZE];
	size_t hello_size;
	u8 challenge_frame[IS_PROTOCOL_MAX_FRAME_SIZE];
	size_t challenge_size;
	u8 encoded_frame[IS_PROTOCOL_MAX_FRAME_SIZE];
	struct is_protocol_message inbound_message;
	struct is_protocol_message outbound_message;
	u8 consumer_nonce[IS_PROTOCOL_NONCE_SIZE];
	u16 negotiated_minor;
	u64 negotiated_capabilities;
	u64 required_capabilities;
	u64 session_id;
	u64 next_request_id;
	u64 pending_request_id;
	u64 heartbeat_request_id;
	u32 pending_logical_chunk;
	u32 pending_chunk_count;
	u32 release_provider_ids[IS_PROTOCOL_MAX_CHUNKS_PER_FRAME];
	u16 release_count;

	unsigned int remote_chunk_limit;
	bool healthy;
	bool compatible;
	enum is_placement_exclude_reason exclude_reason;
	u32 available_chunks;
	char provider_id[IS_PROVIDER_NAME_SIZE];
	char provider_address[IS_PROVIDER_ADDRESS_SIZE];
	char rdma_device[IS_RDMA_DEVICE_SIZE];
	char provider_key_id[IS_PROVIDER_KEY_ID_SIZE];
	u8 provider_psk[IS_PSK_MAX_SIZE];
	u8 provider_psk_size;
	u16 provider_port;
	u8 rdma_port;
	int rdma_numa_node;
	u32 placement_weight;
};

static void is_rdma_fail(struct is_rdma_session *session, int error);
static int is_post_control_receive(struct is_rdma_session *session);
static void is_mapping_work(struct work_struct *work);
static void is_fabric_mapping_work(struct work_struct *work);
static void is_fabric_reservation_work(struct work_struct *work);
static void is_fabric_maybe_start_reservation(struct is_rdma_fabric *fabric);
static void is_session_status_done(struct is_rdma_session *session);
static void is_fabric_complete_remote_only(struct is_rdma_fabric *fabric,
					   int error);
static void is_arm_control_deadline(struct is_rdma_session *session);
static void is_cancel_control_deadline(struct is_rdma_session *session);
static void is_schedule_heartbeat(struct is_rdma_session *session);
static int is_resolve_provider(struct is_rdma_session *session);
static void is_fabric_refresh_connection_state(struct is_rdma_fabric *fabric);
static void is_fabric_start_next_session(struct is_rdma_fabric *fabric);
static void is_fabric_schedule_next_session(struct is_rdma_fabric *fabric);
static void is_session_copy_endpoint(struct is_rdma_session *session,
				     const struct is_provider_endpoint *endpoint);
static void is_session_copy_legacy(struct is_rdma_session *session,
				   struct is_device *device);
static int is_session_init(struct is_rdma_fabric *fabric,
			   struct is_rdma_session *session,
			   unsigned int provider_index, bool remote_only);
static struct is_rdma_fabric *is_device_fabric(struct is_device *device);
static bool is_remote_only_session(const struct is_rdma_session *session);
static bool is_remote_only_device(const struct is_device *device);
static unsigned int is_session_available_chunks(struct is_rdma_session *session);
static void is_session_set_exclude(struct is_rdma_session *session,
				   enum is_placement_exclude_reason reason);
static void is_unmap_session_chunks(struct is_rdma_session *session);
static void is_fabric_update_remote_capacity(struct is_rdma_fabric *fabric);
static int is_fabric_choose_session(struct is_rdma_fabric *fabric,
				    unsigned int *chosen_index);
static void is_free_control_resources(struct is_rdma_session *session);

static bool is_remote_only_session(const struct is_rdma_session *session)
{
	return session->device->mode == IS_DEVICE_MODE_REMOTE_ONLY;
}

static bool is_remote_only_device(const struct is_device *device)
{
	return device->mode == IS_DEVICE_MODE_REMOTE_ONLY;
}

static struct is_rdma_fabric *is_device_fabric(struct is_device *device)
{
	return READ_ONCE(device->rdma);
}

static bool is_fabric_chunk_mapped_locked(struct is_rdma_fabric *fabric,
					  struct is_fabric_remote_chunk *chunk)
{
	if (chunk->session_index >= fabric->session_count)
		return false;
	if (chunk->state == IS_REMOTE_CHUNK_MAPPED)
		return true;
	if (chunk->state == IS_REMOTE_CHUNK_EVICTING &&
	    chunk->remote_address && chunk->remote_key)
		return true;
	if (chunk->remote_address && chunk->remote_key) {
		chunk->state = IS_REMOTE_CHUNK_MAPPED;
		return true;
	}
	return false;
}

static bool is_fabric_chunk_counted_locked(struct is_fabric_remote_chunk *chunk)
{
	return chunk->state == IS_REMOTE_CHUNK_MAPPED &&
		chunk->session_index != IS_FABRIC_NO_SESSION;
}

unsigned int is_fabric_mapped_chunk_count(struct is_device *device)
{
	struct is_rdma_fabric *fabric = is_device_fabric(device);
	unsigned long flags;
	unsigned int per_session[IS_MAX_PROVIDERS] = { 0 };
	unsigned int index;
	unsigned int mapped = 0;

	if (!fabric)
		return 0;
	spin_lock_irqsave(&fabric->chunk_lock, flags);
	for (index = 0; index < fabric->chunk_count; index++) {
		struct is_fabric_remote_chunk *chunk = &fabric->chunks[index];

		if (!is_fabric_chunk_counted_locked(chunk))
			continue;
		mapped++;
		if (chunk->session_index < fabric->session_count)
			per_session[chunk->session_index]++;
	}
	for (index = 0; index < fabric->session_count; index++)
		fabric->mapped_per_session[index] = per_session[index];
	atomic_set(&device->mapped_remote_chunks, mapped);
	spin_unlock_irqrestore(&fabric->chunk_lock, flags);
	is_fabric_update_remote_capacity(fabric);
	return mapped;
}

static void *is_session_kzalloc(struct is_rdma_session *session, size_t size,
				gfp_t flags)
{
	if (session->rdma_numa_node == NUMA_NO_NODE)
		return kzalloc(size, flags);
	return kzalloc_node(size, flags, session->rdma_numa_node);
}

static is_placement_u32 is_fabric_rand(void *ctx, is_placement_u32 limit)
{
	struct is_rdma_fabric *fabric = ctx;
	struct is_device *device = fabric->device;
	u32 threshold;
	u32 value;

	if (!limit)
		return 0;
	if (device->placement_seed) {
		fabric->placement_rng_state = fabric->placement_rng_state *
			6364136223846793005ULL + 1ULL;
		return (is_placement_u32)((fabric->placement_rng_state >> 33) %
					  limit);
	}
	/* Rejection sampling avoids modulo bias on both supported kernels. */
	threshold = (u32)(-limit) % limit;
	do {
		value = get_random_u32();
	} while (value < threshold);
	return (is_placement_u32)(value % limit);
}

static unsigned int is_session_available_chunks(struct is_rdma_session *session)
{
	struct is_rdma_fabric *fabric = session->fabric;
	unsigned int mapped;

	if (!session->healthy || !session->compatible ||
	    session->control_state != IS_RDMA_CONTROL_READY)
		return 0;
	mapped = fabric->mapped_per_session[session->provider_index];
	if (session->available_chunks <= mapped)
		return 0;
	return session->available_chunks - mapped;
}

static void is_session_set_exclude(struct is_rdma_session *session,
				   enum is_placement_exclude_reason reason)
{
	session->healthy = false;
	session->compatible = false;
	if (session->exclude_reason == IS_PLACEMENT_EXCLUDE_NONE)
		session->exclude_reason = reason;
}

static void is_fabric_update_remote_capacity(struct is_rdma_fabric *fabric)
{
	u64 capacity = (u64)atomic_read(&fabric->device->mapped_remote_chunks) *
		IS_CHUNK_BYTES;

	WRITE_ONCE(fabric->device->remote_capacity_bytes, capacity);
}

static void is_unmap_session_chunks(struct is_rdma_session *session)
{
	struct is_rdma_fabric *fabric = session->fabric;
	unsigned long flags;
	unsigned int index;
	unsigned int unmapped = 0;

	spin_lock_irqsave(&fabric->chunk_lock, flags);
	for (index = 0; index < fabric->chunk_count; index++) {
		struct is_fabric_remote_chunk *chunk = &fabric->chunks[index];

		if (chunk->session_index != session->provider_index ||
		    chunk->state == IS_REMOTE_CHUNK_UNMAPPED)
			continue;
		if (chunk->state == IS_REMOTE_CHUNK_MAPPING) {
			chunk->state = IS_REMOTE_CHUNK_UNMAPPED;
			chunk->session_index = IS_FABRIC_NO_SESSION;
			continue;
		}
		bitmap_clear(fabric->valid_sectors,
			(unsigned long)index * IS_SECTORS_PER_CHUNK,
			IS_SECTORS_PER_CHUNK);
		chunk->state = IS_REMOTE_CHUNK_UNMAPPED;
		chunk->session_index = IS_FABRIC_NO_SESSION;
		chunk->provider_chunk_id = 0;
		chunk->remote_address = 0;
		chunk->remote_key = 0;
		unmapped++;
	}
	if (unmapped) {
		if (fabric->mapped_per_session[session->provider_index] >=
		    unmapped)
			fabric->mapped_per_session[session->provider_index] -=
				unmapped;
		else
			fabric->mapped_per_session[session->provider_index] = 0;
		atomic_sub(unmapped, &fabric->device->mapped_remote_chunks);
	}
	spin_unlock_irqrestore(&fabric->chunk_lock, flags);
	if (unmapped)
		is_fabric_update_remote_capacity(fabric);
}

static bool is_session_holds_mapped_chunks(struct is_rdma_session *session)
{
	struct is_rdma_fabric *fabric = session->fabric;
	unsigned long flags;
	unsigned int index;
	bool holds = false;

	spin_lock_irqsave(&fabric->chunk_lock, flags);
	for (index = 0; index < fabric->chunk_count; index++) {
		struct is_fabric_remote_chunk *chunk = &fabric->chunks[index];

		if (chunk->session_index == session->provider_index &&
		    (chunk->state == IS_REMOTE_CHUNK_MAPPED ||
		     chunk->state == IS_REMOTE_CHUNK_EVICTING)) {
			holds = true;
			break;
		}
	}
	spin_unlock_irqrestore(&fabric->chunk_lock, flags);
	return holds;
}

static int is_fabric_choose_session(struct is_rdma_fabric *fabric,
				    unsigned int *chosen_index)
{
	struct is_placement_candidate *candidates;
	struct is_placement_choice choice;
	unsigned int candidate_count = 0;
	unsigned int index;
	enum is_placement_result result;
	int ret;

	candidates = kcalloc(IS_MAX_PROVIDERS, sizeof(*candidates), GFP_KERNEL);
	if (!candidates)
		return -ENOMEM;

	for (index = 0; index < fabric->session_count; index++) {
		struct is_rdma_session *session = fabric->sessions[index];
		struct is_placement_candidate *candidate =
			&candidates[candidate_count];

		strscpy(candidate->provider_id, session->provider_id,
			sizeof(candidate->provider_id));
		candidate->available_chunks =
			is_session_available_chunks(session);
		candidate->placement_weight = session->placement_weight;
		candidate->healthy = session->healthy ? 1 : 0;
		candidate->compatible = session->compatible ? 1 : 0;
		candidate->exclude_reason = session->exclude_reason;
		candidate_count++;
	}

	result = is_placement_choose(candidates, candidate_count,
		fabric->device->placement_sample_size, is_fabric_rand, fabric,
		&choice);
	kfree(candidates);
	if (result != IS_PLACEMENT_OK) {
		ret = result == IS_PLACEMENT_INSUFFICIENT_CAPACITY ? -ENOSPC :
		      result == IS_PLACEMENT_NO_COMPATIBLE_PROVIDER ? -ENODEV :
		      -EINVAL;
		return ret;
	}
	*chosen_index = choice.chosen_index;
	return 0;
}

static bool is_remote_only(const struct is_rdma_session *session)
{
	return is_remote_only_session(session);
}

static void is_force_qp_error(struct is_rdma_session *session)
{
	struct ib_qp_attr attributes;

	if (!session->qp)
		return;
	memset(&attributes, 0, sizeof(attributes));
	attributes.qp_state = IB_QPS_ERR;
	(void)ib_modify_qp(session->qp, &attributes, IB_QP_STATE);
}

static void is_fail_after_provider_timeout(struct is_rdma_session *session)
{
	/* Send the disconnect during stop, after a transient fault can clear. */
	WRITE_ONCE(session->disconnect_deferred, true);
	is_force_qp_error(session);
	is_rdma_fail(session, -ETIMEDOUT);
}

static void *is_kzalloc_numa(struct is_device *device, size_t size,
			     gfp_t flags)
{
	if (device->rdma_numa_node == NUMA_NO_NODE)
		return kzalloc(size, flags);
	return kzalloc_node(size, flags, device->rdma_numa_node);
}

static void is_set_remote_capacity(struct is_rdma_fabric *fabric)
{
	is_fabric_update_remote_capacity(fabric);
}

static void is_fabric_refresh_connection_state(struct is_rdma_fabric *fabric)
{
	unsigned int index;
	unsigned int healthy = 0;
	unsigned int failed = 0;
	unsigned int pending = 0;
	unsigned int limit;

	if (!fabric || READ_ONCE(fabric->stopping) ||
	    atomic_read(&fabric->device->remote_lost))
		return;
	limit = max(fabric->planned_count, fabric->session_count);
	for (index = 0; index < limit; index++) {
		struct is_rdma_session *session = fabric->sessions[index];

		if (!session) {
			if (test_bit(index, fabric->failed_sessions))
				failed++;
			else
				pending++;
			continue;
		}
		if (READ_ONCE(session->healthy) &&
		    READ_ONCE(session->ever_connected))
			healthy++;
		else if (READ_ONCE(session->failure_started) ||
			 READ_ONCE(session->control_state) ==
				 IS_RDMA_CONTROL_FAILED)
			failed++;
		else
			pending++;
	}
	if (pending)
		atomic_set(&fabric->device->connection_state,
			   IS_CONNECTION_CONNECTING);
	else if (healthy && failed)
		atomic_set(&fabric->device->connection_state,
			   IS_CONNECTION_DEGRADED);
	else if (healthy) {
		WRITE_ONCE(fabric->device->last_error, 0);
		atomic_set(&fabric->device->connection_state,
			   IS_CONNECTION_CONNECTED);
	} else
		atomic_set(&fabric->device->connection_state,
			   IS_CONNECTION_NOT_CONNECTED);
}

static void is_fabric_start_next_session(struct is_rdma_fabric *fabric)
{
	struct is_device *device;
	struct is_rdma_session *session;
	unsigned int index;
	bool remote_only;
	int ret;

	if (!fabric || READ_ONCE(fabric->stopping))
		return;
	device = fabric->device;
	remote_only = device->mode == IS_DEVICE_MODE_REMOTE_ONLY;
	if (fabric->next_resolve_index >= fabric->planned_count)
		return;
	index = fabric->next_resolve_index++;
	if (fabric->sessions[index])
		return;

	session = is_kzalloc_numa(device, sizeof(*session), GFP_KERNEL);
	if (!session) {
		set_bit(index, fabric->failed_sessions);
		is_fabric_refresh_connection_state(fabric);
		queue_work(system_wq, &fabric->mapping_work);
		is_fabric_schedule_next_session(fabric);
		return;
	}
	if (device->provider_count == 0)
		is_session_copy_legacy(session, device);
	else
		is_session_copy_endpoint(session,
			&device->provider_endpoints[index]);
	ret = is_session_init(fabric, session, index, remote_only);
	if (ret) {
		kfree(session);
		set_bit(index, fabric->failed_sessions);
		is_fabric_refresh_connection_state(fabric);
		queue_work(system_wq, &fabric->mapping_work);
		is_fabric_schedule_next_session(fabric);
		return;
	}
	fabric->sessions[index] = session;
	if (fabric->session_count <= index)
		fabric->session_count = index + 1;
	WRITE_ONCE(session->resolve_started, true);
	is_arm_control_deadline(session);
	ret = is_resolve_provider(session);
	if (ret)
		is_rdma_fail(session, ret);
}

static void is_fabric_schedule_next_session(struct is_rdma_fabric *fabric)
{
	if (!fabric || READ_ONCE(fabric->stopping))
		return;
	if (fabric->next_resolve_index >= fabric->planned_count)
		return;
	/*
	 * Soft-RoCE loopback is unstable when two Consumer QPs become ready in
	 * the same jiffy; give the first session a quiet period first.
	 */
	mod_delayed_work(system_wq, &fabric->start_next_session_work,
		msecs_to_jiffies(250));
}

static void is_fabric_start_next_session_work(struct work_struct *work)
{
	struct is_rdma_fabric *fabric = container_of(
		to_delayed_work(work), struct is_rdma_fabric,
		start_next_session_work);

	is_fabric_start_next_session(fabric);
}

static void is_failure_work(struct work_struct *work)
{
	struct is_rdma_session *session = container_of(
		work, struct is_rdma_session, failure_work);

	mutex_lock(&session->control_lock);
	if (session->control_state != IS_RDMA_CONTROL_STOPPING)
		session->control_state = IS_RDMA_CONTROL_FAILED;
	is_cancel_control_deadline(session);
	cancel_delayed_work(&session->heartbeat_work);
	mutex_unlock(&session->control_lock);
	is_unmap_session_chunks(session);
	is_fabric_refresh_connection_state(session->fabric);
	queue_work(system_wq, &session->fabric->mapping_work);
	is_fabric_schedule_next_session(session->fabric);
	wake_up_all(&session->control_wait);
}

static void is_mark_session_remote_lost(struct is_rdma_session *session,
					int error)
{
	unsigned long flags;

	mutex_lock(&session->device->remote_state_lock);
	spin_lock_irqsave(&session->operation_lock, flags);
	is_device_mark_remote_lost_locked(session->device, error);
	spin_unlock_irqrestore(&session->operation_lock, flags);
	mutex_unlock(&session->device->remote_state_lock);
	wake_up_all(&session->chunk_wait);
}

static void is_rdma_fail(struct is_rdma_session *session, int error)
{
	bool terminal;

	if (!session)
		return;
	if (error >= 0)
		error = -EIO;
	mutex_lock(&session->control_lock);
	if (READ_ONCE(session->stopping)) {
		mutex_unlock(&session->control_lock);
		return;
	}
	if (session->exclude_reason == IS_PLACEMENT_EXCLUDE_NONE) {
		if (error == -ENODEV)
			is_session_set_exclude(session,
				IS_PLACEMENT_EXCLUDE_RAIL);
		else if (error == -EPROTO || error == -EACCES)
			is_session_set_exclude(session,
				error == -EACCES ?
				IS_PLACEMENT_EXCLUDE_IDENTITY :
				IS_PLACEMENT_EXCLUDE_VERSION);
		else if (error == -ENOSPC)
			is_session_set_exclude(session,
				IS_PLACEMENT_EXCLUDE_ZERO_CAPACITY);
		else
			is_session_set_exclude(session,
				IS_PLACEMENT_EXCLUDE_UNHEALTHY);
	}
	session->failure_started = true;
	WRITE_ONCE(session->device->last_error, -error);
	terminal = is_remote_only(session) &&
		(session->fabric->reservation_complete ||
		 is_session_holds_mapped_chunks(session));
	mutex_unlock(&session->control_lock);
	is_fabric_refresh_connection_state(session->fabric);
	if (is_remote_only(session) && !session->fabric->reservation_complete) {
		is_session_status_done(session);
		if (session->fabric->reservation_started)
			is_fabric_complete_remote_only(session->fabric, error);
	}
	if (terminal)
		is_mark_session_remote_lost(session, -error);
	queue_work(session->control_wq, &session->failure_work);
	if (session->connect_started &&
	    !READ_ONCE(session->disconnect_deferred) &&
	    atomic_cmpxchg(&session->disconnect_started, 0, 1) == 0)
		rdma_disconnect(session->cm_id);
}

static void is_control_deadline(struct work_struct *work)
{
	struct is_rdma_session *session = container_of(
		to_delayed_work(work), struct is_rdma_session,
		control_deadline_work);
	unsigned long remaining = 0;
	bool timed_out = false;

	mutex_lock(&session->control_lock);
	if (session->control_deadline_armed &&
	    !READ_ONCE(session->stopping)) {
		if (time_before(jiffies, session->control_deadline_expires)) {
			remaining = session->control_deadline_expires - jiffies;
		} else {
			session->control_deadline_armed = false;
			timed_out = true;
		}
	}
	mutex_unlock(&session->control_lock);
	if (remaining) {
		mod_delayed_work(system_wq, &session->control_deadline_work,
			remaining);
		return;
	}
	if (!timed_out)
		return;
	mutex_lock(&session->control_lock);
	if (session->control_state != IS_RDMA_CONTROL_STOPPING)
		session->control_state = IS_RDMA_CONTROL_FAILED;
	mutex_unlock(&session->control_lock);
	atomic64_inc(&session->device->provider_timeouts_total);
	is_fail_after_provider_timeout(session);
}

static void is_arm_control_deadline_ms(struct is_rdma_session *session,
				       u32 timeout_ms)
{
	unsigned long delay = msecs_to_jiffies(timeout_ms);

	session->control_deadline_expires = jiffies + delay;
	session->control_deadline_armed = true;
	mod_delayed_work(system_wq, &session->control_deadline_work, delay);
}

static void is_arm_control_deadline(struct is_rdma_session *session)
{
	is_arm_control_deadline_ms(session,
		session->device->provider_failure_deadline_ms);
}

static void is_cancel_control_deadline(struct is_rdma_session *session)
{
	session->control_deadline_armed = false;
	cancel_delayed_work(&session->control_deadline_work);
}

static void is_control_send_done(struct ib_cq *cq, struct ib_wc *wc)
{
	struct is_control_completion *completion = container_of(
		wc->wr_cqe, struct is_control_completion, cqe);
	struct is_rdma_session *session = completion->session;

	(void)cq;
	atomic_set(&session->send_busy, 0);
	wake_up_all(&session->send_wait);
	if (wc->status != IB_WC_SUCCESS) {
		pr_err(IS_DRIVER_NAME
		       ": control send WC %s vendor=%u on %s\n",
		       ib_wc_status_msg(wc->status), wc->vendor_err,
		       session->provider_id);
		is_rdma_fail(session, -EIO);
	}
}

static void is_control_receive_done(struct ib_cq *cq, struct ib_wc *wc)
{
	struct is_control_completion *completion = container_of(
		wc->wr_cqe, struct is_control_completion, cqe);
	struct is_rdma_session *session = completion->session;

	(void)cq;
	if (READ_ONCE(session->stopping))
		return;
	if (wc->status != IB_WC_SUCCESS) {
		pr_err(IS_DRIVER_NAME
		       ": control recv WC %s vendor=%u on %s\n",
		       ib_wc_status_msg(wc->status), wc->vendor_err,
		       session->provider_id);
		is_rdma_fail(session, -EIO);
		return;
	}
	if (!wc->byte_len || wc->byte_len > IS_PROTOCOL_MAX_FRAME_SIZE) {
		is_rdma_fail(session, -EPROTO);
		return;
	}
	ib_dma_sync_single_for_cpu(session->cm_id->device, session->recv_dma,
		IS_PROTOCOL_MAX_FRAME_SIZE, DMA_FROM_DEVICE);
	memcpy(session->received_frame, session->recv_frame, wc->byte_len);
	session->received_size = wc->byte_len;
	queue_work(session->control_wq, &session->receive_work);
}

static int is_post_control_receive(struct is_rdma_session *session)
{
	const struct ib_recv_wr *bad_wr;

	if (READ_ONCE(session->stopping) || !session->qp)
		return -ESHUTDOWN;
	ib_dma_sync_single_for_device(session->cm_id->device, session->recv_dma,
		IS_PROTOCOL_MAX_FRAME_SIZE, DMA_FROM_DEVICE);
	return ib_post_recv(session->qp, &session->recv_wr, &bad_wr);
}

static int is_send_control_frame_timeout(struct is_rdma_session *session,
					 const u8 *frame, size_t frame_size,
					 u32 wait_ms)
{
	const struct ib_send_wr *bad_wr;
	long waited;
	int ret;

	if (!frame || !frame_size || frame_size > IS_PROTOCOL_MAX_FRAME_SIZE)
		return -EINVAL;
	if (wait_ms) {
		waited = wait_event_timeout(session->send_wait,
			!atomic_read(&session->send_busy) ||
			READ_ONCE(session->stopping),
			msecs_to_jiffies(wait_ms));
		if (!waited || READ_ONCE(session->stopping))
			return -ETIMEDOUT;
	} else if (READ_ONCE(session->stopping)) {
		return -ESHUTDOWN;
	}
	if (atomic_cmpxchg(&session->send_busy, 0, 1) != 0)
		return -EBUSY;

	memcpy(session->send_frame, frame, frame_size);
	session->send_size = frame_size;
	session->send_sge.length = frame_size;
	ib_dma_sync_single_for_device(session->cm_id->device, session->send_dma,
		frame_size, DMA_TO_DEVICE);
	ret = ib_post_send(session->qp, &session->send_wr, &bad_wr);
	if (ret) {
		atomic_set(&session->send_busy, 0);
		wake_up_all(&session->send_wait);
	}
	return ret;
}

static int is_send_control_frame(struct is_rdma_session *session,
				 const u8 *frame, size_t frame_size)
{
	return is_send_control_frame_timeout(session, frame, frame_size,
		session->device->provider_failure_deadline_ms);
}

static void is_init_message(struct is_rdma_session *session,
			    struct is_protocol_message *message,
			    enum is_protocol_message_type type,
			    u64 request_id, bool response)
{
	memset(message, 0, sizeof(*message));
	message->header.major = IS_PROTOCOL_MAJOR;
	message->header.minor = session->negotiated_minor;
	message->header.type = type;
	message->header.flags = response ? IS_PROTOCOL_FLAG_RESPONSE : 0;
	message->header.request_id = request_id;
	message->header.session_id = session->session_id;
	message->header.capabilities = session->negotiated_capabilities;
	message->header.required_capabilities = session->required_capabilities;
}

static int is_encode_and_send_timeout(
	struct is_rdma_session *session,
	const struct is_protocol_message *message, u32 wait_ms)
{
	size_t frame_size;

	if (is_protocol_encode(message, session->encoded_frame,
			       sizeof(session->encoded_frame), &frame_size) !=
	    IS_PROTOCOL_OK)
		return -EPROTO;
	return is_send_control_frame_timeout(session, session->encoded_frame,
		frame_size, wait_ms);
}

static int is_encode_and_send(struct is_rdma_session *session,
			      const struct is_protocol_message *message)
{
	return is_encode_and_send_timeout(session, message,
		session->device->provider_failure_deadline_ms);
}

static u32 is_heartbeat_interval_ms(const struct is_rdma_session *session)
{
	return max_t(u32, 1U,
		session->device->provider_failure_deadline_ms / 3U);
}

static void is_schedule_heartbeat(struct is_rdma_session *session)
{
	mod_delayed_work(system_wq, &session->heartbeat_work,
		msecs_to_jiffies(is_heartbeat_interval_ms(session)));
}

static void is_heartbeat_work(struct work_struct *work)
{
	struct is_rdma_session *session = container_of(
		to_delayed_work(work), struct is_rdma_session, heartbeat_work);
	struct is_protocol_message *request = &session->outbound_message;
	bool remote_only;
	bool timed_out = false;
	int ret = 0;

	mutex_lock(&session->control_lock);
	remote_only = is_remote_only(session);
	if (READ_ONCE(session->stopping))
		goto out;
	if (session->control_state != IS_RDMA_CONTROL_READY ||
	    (!remote_only && (session->pending_request_id ||
			     session->release_count))) {
		if (session->control_state != IS_RDMA_CONTROL_FAILED &&
		    session->control_state != IS_RDMA_CONTROL_STOPPING)
			is_schedule_heartbeat(session);
		goto out;
	}
	if (remote_only && !session->reservation_complete)
		goto out;
	if (!remote_only && session->heartbeat_request_id) {
		timed_out = true;
		goto out;
	}
	is_init_message(session, request, IS_PROTOCOL_MSG_STATUS_REQUEST,
		session->next_request_id++, false);
	if (remote_only) {
		session->pending_request_id = request->header.request_id;
		session->control_state = IS_RDMA_CONTROL_WAIT_HEARTBEAT;
		ret = is_encode_and_send_timeout(session, request, 0);
		if (!ret)
			is_arm_control_deadline_ms(session,
				is_heartbeat_interval_ms(session));
	} else {
		session->heartbeat_request_id = request->header.request_id;
		ret = is_encode_and_send_timeout(session, request, 0);
		if (ret)
			session->heartbeat_request_id = 0;
		else
			is_schedule_heartbeat(session);
	}
out:
	mutex_unlock(&session->control_lock);
	if (timed_out) {
		atomic64_inc(&session->device->provider_timeouts_total);
		is_fail_after_provider_timeout(session);
	} else if (ret) {
		is_rdma_fail(session, ret);
	}
}

static int is_compute_auth_tag(struct is_rdma_session *session,
			       u8 tag[IS_PROTOCOL_AUTH_TAG_SIZE])
{
	static const u8 domain[] = IS_RDMA_AUTH_DOMAIN;
	struct crypto_shash *transform;
	struct shash_desc *description;
	int ret;

	transform = crypto_alloc_shash("hmac(sha256)", 0, 0);
	if (IS_ERR(transform))
		return PTR_ERR(transform);
	ret = crypto_shash_setkey(transform, session->provider_psk,
		session->provider_psk_size);
	if (ret)
		goto free_transform;
	description = kzalloc(sizeof(*description) +
		crypto_shash_descsize(transform), GFP_KERNEL);
	if (!description) {
		ret = -ENOMEM;
		goto free_transform;
	}
	description->tfm = transform;
	ret = crypto_shash_init(description);
	if (!ret)
		ret = crypto_shash_update(description, domain, sizeof(domain) - 1U);
	if (!ret)
		ret = crypto_shash_update(description, session->hello_frame,
			session->hello_size);
	if (!ret)
		ret = crypto_shash_update(description, session->challenge_frame,
			session->challenge_size);
	if (!ret)
		ret = crypto_shash_final(description, tag);
	kfree_sensitive(description);
free_transform:
	crypto_free_shash(transform);
	return ret;
}

static int is_send_hello(struct is_rdma_session *session)
{
	struct is_protocol_message *hello = &session->outbound_message;
	size_t frame_size;
	int ret;

	memset(hello, 0, sizeof(*hello));
	hello->header.major = IS_PROTOCOL_MAJOR;
	hello->header.minor = IS_PROTOCOL_MINOR_CURRENT;
	hello->header.type = IS_PROTOCOL_MSG_HELLO;
	hello->header.request_id = session->next_request_id++;
	hello->header.capabilities = IS_RDMA_CAPABILITIES;
	hello->header.required_capabilities = session->required_capabilities;
	strscpy(hello->payload.hello.consumer_id, session->device->consumer_id,
		sizeof(hello->payload.hello.consumer_id));
	strscpy(hello->payload.hello.key_id, session->provider_key_id,
		sizeof(hello->payload.hello.key_id));
	get_random_bytes(session->consumer_nonce,
		sizeof(session->consumer_nonce));
	memcpy(hello->payload.hello.nonce, session->consumer_nonce,
		sizeof(session->consumer_nonce));
	hello->payload.hello.mode = is_remote_only(session) ?
		IS_PROTOCOL_MODE_REMOTE_ONLY : IS_PROTOCOL_MODE_BACKED;
	hello->payload.hello.pool = is_remote_only(session) ?
		IS_PROTOCOL_POOL_COMMITTED : IS_PROTOCOL_POOL_OPPORTUNISTIC;
	hello->payload.hello.failure_deadline_ms =
		session->device->provider_failure_deadline_ms;
	if (is_protocol_encode(hello, session->hello_frame,
			       sizeof(session->hello_frame), &frame_size) !=
	    IS_PROTOCOL_OK)
		return -EPROTO;
	session->hello_size = frame_size;
	session->pending_request_id = hello->header.request_id;
	session->control_state = IS_RDMA_CONTROL_WAIT_CHALLENGE;
	ret = is_send_control_frame(session, session->hello_frame,
		session->hello_size);
	if (!ret)
		is_arm_control_deadline(session);
	return ret;
}

static void is_hello_work(struct work_struct *work)
{
	struct is_rdma_session *session = container_of(
		to_delayed_work(work), struct is_rdma_session, hello_work);
	int ret;

	mutex_lock(&session->control_lock);
	ret = session->control_state == IS_RDMA_CONTROL_CONNECTING ?
		is_send_hello(session) : -EINVAL;
	mutex_unlock(&session->control_lock);
	if (ret)
		is_rdma_fail(session, ret);
}

static bool is_authenticated_header_valid(
	const struct is_rdma_session *session,
	const struct is_protocol_message *message)
{
	return message->header.major == IS_PROTOCOL_MAJOR &&
		message->header.minor == session->negotiated_minor &&
		message->header.session_id == session->session_id &&
		message->header.capabilities == session->negotiated_capabilities;
}

static int is_handle_challenge(struct is_rdma_session *session,
			       const struct is_protocol_message *challenge)
{
	struct is_protocol_message *auth = &session->outbound_message;
	u8 tag[IS_PROTOCOL_AUTH_TAG_SIZE];
	u16 negotiated_minor;
	u64 negotiated_capabilities;
	int ret;

	if (challenge->header.major != IS_PROTOCOL_MAJOR ||
	    !(challenge->header.flags & IS_PROTOCOL_FLAG_RESPONSE) ||
	    challenge->header.request_id != session->pending_request_id ||
	    challenge->header.session_id != 0 ||
	    is_protocol_negotiate(
		IS_PROTOCOL_MINOR_CURRENT, IS_RDMA_CAPABILITIES,
		session->required_capabilities, challenge->header.minor,
		challenge->header.capabilities,
		challenge->header.required_capabilities, &negotiated_minor,
		&negotiated_capabilities) != IS_PROTOCOL_OK ||
	    challenge->payload.challenge.negotiated_minor != negotiated_minor)
		return -EPROTO;
	session->negotiated_minor = negotiated_minor;
	session->negotiated_capabilities = negotiated_capabilities;
	ret = is_compute_auth_tag(session, tag);
	if (ret)
		return ret;
	is_init_message(session, auth, IS_PROTOCOL_MSG_AUTH,
		session->next_request_id++, false);
	auth->header.session_id = 0;
	memcpy(auth->payload.auth.tag, tag, sizeof(tag));
	memzero_explicit(tag, sizeof(tag));
	session->pending_request_id = auth->header.request_id;
	session->control_state = IS_RDMA_CONTROL_WAIT_ACCEPT;
	ret = is_encode_and_send(session, auth);
	if (!ret)
		is_arm_control_deadline(session);
	return ret;
}

static void is_fabric_maybe_start_reservation(struct is_rdma_fabric *fabric)
{
	if (!is_remote_only_device(fabric->device) ||
	    fabric->reservation_started || fabric->stopping)
		return;
	if (atomic_read(&fabric->sessions_awaiting_status) > 0)
		return;
	fabric->reservation_started = true;
	queue_work(fabric->sessions[0]->control_wq, &fabric->reservation_work);
}

static void is_session_status_done(struct is_rdma_session *session)
{
	struct is_rdma_fabric *fabric = session->fabric;

	if (!is_remote_only_session(session))
		return;
	if (atomic_dec_if_positive(&fabric->sessions_awaiting_status) < 0)
		return;
	is_fabric_maybe_start_reservation(fabric);
}

static void is_fabric_complete_remote_only(struct is_rdma_fabric *fabric,
					   int error)
{
	if (error) {
		fabric->reservation_failed = true;
		fabric->reservation_error = error;
	} else {
		fabric->reservation_complete = true;
	}
	wake_up_all(&fabric->control_wait);
}

static void is_mark_session_ready(struct is_rdma_session *session,
				  unsigned int remote_chunk_limit)
{
	struct is_rdma_fabric *fabric = session->fabric;
	bool remote_only = is_remote_only(session);

	session->remote_chunk_limit = remote_chunk_limit;
	session->healthy = true;
	session->compatible = true;
	session->exclude_reason = IS_PLACEMENT_EXCLUDE_NONE;
	if (remote_only)
		session->reservation_complete = false;
	if (remote_only) {
		if (session->failure_started) {
			is_mark_session_remote_lost(session,
				READ_ONCE(session->device->last_error));
			session->pending_request_id = 0;
			session->pending_chunk_count = 0;
			session->control_state = IS_RDMA_CONTROL_FAILED;
			is_cancel_control_deadline(session);
			is_session_status_done(session);
			wake_up_all(&session->control_wait);
			return;
		}
		session->pending_request_id = 0;
		session->pending_chunk_count = 0;
		session->control_state = IS_RDMA_CONTROL_READY;
		session->ever_connected = true;
		is_cancel_control_deadline(session);
		is_session_status_done(session);
		is_fabric_refresh_connection_state(fabric);
		is_fabric_schedule_next_session(fabric);
		wake_up_all(&session->control_wait);
		if (session->negotiated_capabilities & IS_PROTOCOL_CAP_STATUS)
			is_schedule_heartbeat(session);
		return;
	}
	session->pending_request_id = 0;
	session->pending_chunk_count = 0;
	session->control_state = IS_RDMA_CONTROL_READY;
	session->ever_connected = true;
	is_cancel_control_deadline(session);
	is_fabric_refresh_connection_state(fabric);
	is_fabric_schedule_next_session(fabric);
	wake_up_all(&session->control_wait);
	queue_work(system_wq, &fabric->mapping_work);
	if (session->negotiated_capabilities & IS_PROTOCOL_CAP_STATUS)
		is_schedule_heartbeat(session);
}

static int is_handle_accept(struct is_rdma_session *session,
			    const struct is_protocol_message *accept)
{
	struct is_protocol_message *request = &session->outbound_message;
	int ret;

	if (accept->header.major != IS_PROTOCOL_MAJOR ||
	    accept->header.minor != session->negotiated_minor ||
	    !(accept->header.flags & IS_PROTOCOL_FLAG_RESPONSE) ||
	    accept->header.request_id != session->pending_request_id ||
	    !accept->header.session_id ||
	    accept->payload.accept.negotiated_minor != session->negotiated_minor ||
	    accept->header.capabilities != session->negotiated_capabilities)
		return -EPROTO;
	session->session_id = accept->header.session_id;
	if (!(session->negotiated_capabilities & IS_PROTOCOL_CAP_STATUS)) {
		if (is_remote_only(session))
			return -EPROTO;
		is_mark_session_ready(session, 0);
		return 0;
	}
	is_init_message(session, request, IS_PROTOCOL_MSG_STATUS_REQUEST,
		session->next_request_id++, false);
	session->pending_request_id = request->header.request_id;
	session->control_state = IS_RDMA_CONTROL_WAIT_STATUS;
	ret = is_encode_and_send(session, request);
	if (!ret)
		is_arm_control_deadline(session);
	return ret;
}

static int is_request_remote_only_reservation(
	struct is_rdma_session *session, unsigned int logical_start,
	unsigned int chunk_count)
{
	struct is_protocol_message *request = &session->outbound_message;
	struct is_rdma_fabric *fabric = session->fabric;
	unsigned long flags;
	unsigned int index;
	int ret;

	spin_lock_irqsave(&fabric->chunk_lock, flags);
	for (index = logical_start; index < logical_start + chunk_count; index++) {
		struct is_fabric_remote_chunk *chunk = &fabric->chunks[index];

		if (chunk->state != IS_REMOTE_CHUNK_UNMAPPED ||
		    chunk->session_index != session->provider_index) {
			spin_unlock_irqrestore(&fabric->chunk_lock, flags);
			return -EPROTO;
		}
		chunk->state = IS_REMOTE_CHUNK_MAPPING;
	}
	spin_unlock_irqrestore(&fabric->chunk_lock, flags);

	is_init_message(session, request, IS_PROTOCOL_MSG_CHUNK_REQUEST,
		session->next_request_id++, false);
	request->payload.chunk_request.chunk_count = chunk_count;
	request->payload.chunk_request.logical_start = logical_start;
	request->payload.chunk_request.pool = IS_PROTOCOL_POOL_COMMITTED;
	session->pending_request_id = request->header.request_id;
	session->pending_logical_chunk = logical_start;
	session->pending_chunk_count = chunk_count;
	session->control_state = IS_RDMA_CONTROL_WAIT_RESERVATION;
	ret = is_encode_and_send(session, request);
	if (!ret) {
		is_arm_control_deadline(session);
		return 0;
	}

	spin_lock_irqsave(&fabric->chunk_lock, flags);
	for (index = logical_start; index < logical_start + chunk_count; index++)
		fabric->chunks[index].state = IS_REMOTE_CHUNK_UNMAPPED;
	spin_unlock_irqrestore(&fabric->chunk_lock, flags);
	session->pending_request_id = 0;
	session->pending_chunk_count = 0;
	session->control_state = IS_RDMA_CONTROL_READY;
	return ret;
}

static bool is_status_response_valid(
	const struct is_rdma_session *session,
	const struct is_protocol_message *status, u64 request_id)
{
	return is_authenticated_header_valid(session, status) &&
		(status->header.flags & IS_PROTOCOL_FLAG_RESPONSE) &&
		status->header.request_id == request_id &&
		status->payload.status.provider_failure_deadline_ms ==
			session->device->provider_failure_deadline_ms;
}

static int is_handle_status(struct is_rdma_session *session,
			    const struct is_protocol_message *status)
{
	bool healthy;

	if (!is_status_response_valid(session, status,
		    session->pending_request_id))
		return -EPROTO;
	healthy = (status->payload.status.flags & IS_PROTOCOL_STATUS_HEALTHY) != 0;
	if (is_remote_only(session)) {
		if (!healthy) {
			is_session_set_exclude(session,
				IS_PLACEMENT_EXCLUDE_UNHEALTHY);
			is_session_status_done(session);
			is_mark_session_ready(session, 0);
			return 0;
		}
		session->available_chunks =
			status->payload.status.available_committed_chunks;
		if (session->available_chunks == 0) {
			is_session_set_exclude(session,
				IS_PLACEMENT_EXCLUDE_ZERO_CAPACITY);
			is_session_status_done(session);
			is_mark_session_ready(session, 0);
			return 0;
		}
		is_mark_session_ready(session, session->fabric->chunk_count);
		return 0;
	}

	if (!healthy) {
		is_session_set_exclude(session, IS_PLACEMENT_EXCLUDE_UNHEALTHY);
		is_mark_session_ready(session, 0);
		return 0;
	}
		session->available_chunks =
			status->payload.status.available_opportunistic_chunks;
		if (session->available_chunks == 0) {
			is_session_set_exclude(session,
				IS_PLACEMENT_EXCLUDE_ZERO_CAPACITY);
			session->pending_request_id = 0;
			session->pending_chunk_count = 0;
			session->control_state = IS_RDMA_CONTROL_READY;
			session->ever_connected = true;
			session->remote_chunk_limit = 0;
			is_cancel_control_deadline(session);
			is_fabric_refresh_connection_state(session->fabric);
			is_fabric_schedule_next_session(session->fabric);
			wake_up_all(&session->control_wait);
			if (session->negotiated_capabilities & IS_PROTOCOL_CAP_STATUS)
				is_schedule_heartbeat(session);
			return 0;
		}
		is_mark_session_ready(session, min_t(unsigned int,
			status->payload.status.available_opportunistic_chunks,
			session->fabric->chunk_count));
		return 0;
	}

static int is_handle_heartbeat(struct is_rdma_session *session,
			       const struct is_protocol_message *status)
{
	if (!is_status_response_valid(session, status,
		    session->pending_request_id))
		return -EPROTO;
	if (!(status->payload.status.flags & IS_PROTOCOL_STATUS_HEALTHY))
		return -EIO;
	is_mark_session_ready(session, session->fabric->chunk_count);
	return 0;
}

static int is_handle_backed_heartbeat(
	struct is_rdma_session *session,
	const struct is_protocol_message *status)
{
	if (!session->heartbeat_request_id ||
	    !is_status_response_valid(session, status,
		    session->heartbeat_request_id))
		return -EPROTO;
	session->heartbeat_request_id = 0;
	if (!(status->payload.status.flags & IS_PROTOCOL_STATUS_HEALTHY))
		return -EIO;
	session->available_chunks =
		status->payload.status.available_opportunistic_chunks;
	if (session->available_chunks == 0)
		is_session_set_exclude(session,
			IS_PLACEMENT_EXCLUDE_ZERO_CAPACITY);
	else {
		session->healthy = true;
		session->compatible = true;
		session->exclude_reason = IS_PLACEMENT_EXCLUDE_NONE;
	}
	session->remote_chunk_limit = min_t(unsigned int,
		status->payload.status.available_opportunistic_chunks,
		session->fabric->chunk_count);
	is_schedule_heartbeat(session);
	queue_work(session->control_wq, &session->fabric->mapping_work);
	return 0;
}

static int is_handle_chunk_grant(struct is_rdma_session *session,
				 const struct is_protocol_message *grant)
{
	struct is_rdma_fabric *fabric = session->fabric;
	unsigned int expected = session->pending_chunk_count;
	u8 expected_pool = is_remote_only(session) ?
		IS_PROTOCOL_POOL_COMMITTED : IS_PROTOCOL_POOL_OPPORTUNISTIC;
	unsigned long flags;
	unsigned int index;
	unsigned int other;
	bool remote_only = is_remote_only(session);

	if (!is_authenticated_header_valid(session, grant) ||
	    !(grant->header.flags & IS_PROTOCOL_FLAG_RESPONSE) ||
	    grant->header.request_id != session->pending_request_id ||
	    !expected || grant->payload.chunk_grant.chunk_count != expected)
		return -EPROTO;
	for (index = 0; index < expected; index++) {
		const struct is_protocol_chunk *wire_chunk =
			&grant->payload.chunk_grant.chunks[index];

		if (wire_chunk->logical_chunk_id !=
			    session->pending_logical_chunk + index ||
		    wire_chunk->logical_chunk_id >= fabric->chunk_count ||
		    wire_chunk->provider_chunk_id >=
			    IS_PROTOCOL_MAX_CHUNKS_PER_FRAME ||
		    !wire_chunk->remote_address || !wire_chunk->remote_key ||
		    wire_chunk->pool != expected_pool)
			return -EPROTO;
		for (other = 0; other < index; other++) {
			if (grant->payload.chunk_grant.chunks[other].provider_chunk_id ==
			    wire_chunk->provider_chunk_id)
				return -EPROTO;
		}
	}

	spin_lock_irqsave(&fabric->chunk_lock, flags);
	for (index = 0; index < expected; index++) {
		const struct is_protocol_chunk *wire_chunk =
			&grant->payload.chunk_grant.chunks[index];
		struct is_fabric_remote_chunk *chunk =
			&fabric->chunks[wire_chunk->logical_chunk_id];

		if (chunk->state != IS_REMOTE_CHUNK_MAPPING ||
		    chunk->session_index != session->provider_index) {
			spin_unlock_irqrestore(&fabric->chunk_lock, flags);
			return -EPROTO;
		}
	}
	for (index = 0; index < expected; index++) {
		const struct is_protocol_chunk *wire_chunk =
			&grant->payload.chunk_grant.chunks[index];
		struct is_fabric_remote_chunk *chunk =
			&fabric->chunks[wire_chunk->logical_chunk_id];

		chunk->provider_chunk_id = wire_chunk->provider_chunk_id;
		chunk->remote_address = wire_chunk->remote_address;
		chunk->remote_key = wire_chunk->remote_key;
		chunk->state = IS_REMOTE_CHUNK_MAPPED;
	}
	fabric->mapped_per_session[session->provider_index] += expected;
	atomic_add(expected, &session->device->mapped_remote_chunks);
	spin_unlock_irqrestore(&fabric->chunk_lock, flags);
	is_set_remote_capacity(fabric);
	if (remote_only) {
		fabric->reservation_next_logical =
			session->pending_logical_chunk + expected;
		session->pending_request_id = 0;
		session->pending_chunk_count = 0;
		session->control_state = IS_RDMA_CONTROL_READY;
		queue_work(session->control_wq, &fabric->reservation_work);
	} else {
		session->pending_request_id = 0;
		session->pending_chunk_count = 0;
		queue_work(session->control_wq, &fabric->mapping_work);
	}
	return 0;
}

static struct is_fabric_remote_chunk *is_find_provider_chunk(
	struct is_rdma_session *session, u32 provider_chunk_id,
	unsigned int *logical_chunk_id)
{
	struct is_rdma_fabric *fabric = session->fabric;
	unsigned int index;

	for (index = 0; index < fabric->chunk_count; index++) {
		struct is_fabric_remote_chunk *chunk = &fabric->chunks[index];

		if (chunk->session_index == session->provider_index &&
		    (chunk->state == IS_REMOTE_CHUNK_MAPPED ||
		     chunk->state == IS_REMOTE_CHUNK_EVICTING) &&
		    chunk->provider_chunk_id == provider_chunk_id) {
			if (logical_chunk_id)
				*logical_chunk_id = index;
			return chunk;
		}
	}
	return NULL;
}

static int is_handle_evict(struct is_rdma_session *session,
			   const struct is_protocol_message *evict)
{
	struct is_protocol_message *activity = &session->outbound_message;
	unsigned int index;

	if (is_remote_only(session) ||
	    !is_authenticated_header_valid(session, evict) ||
	    (evict->header.flags & IS_PROTOCOL_FLAG_RESPONSE))
		return -EPROTO;
	is_init_message(session, activity, IS_PROTOCOL_MSG_ACTIVITY,
		evict->header.request_id, true);
	activity->payload.activity.chunk_count =
		evict->payload.chunk_ids.chunk_count;
	for (index = 0; index < evict->payload.chunk_ids.chunk_count; index++) {
		struct is_fabric_remote_chunk *chunk = is_find_provider_chunk(
			session, evict->payload.chunk_ids.chunk_ids[index], NULL);

		if (!chunk)
			return -EPROTO;
		activity->payload.activity.chunks[index].provider_chunk_id =
			chunk->provider_chunk_id;
		activity->payload.activity.chunks[index].activity =
			atomic64_read(&chunk->activity);
	}
	return is_encode_and_send(session, activity);
}

static int is_handle_release(struct is_rdma_session *session,
			     const struct is_protocol_message *release)
{
	unsigned long flags;
	unsigned int index;

	if (is_remote_only(session) ||
	    !is_authenticated_header_valid(session, release) ||
	    (release->header.flags & IS_PROTOCOL_FLAG_RESPONSE) ||
	    !release->payload.chunk_ids.chunk_count)
		return -EPROTO;
	if (session->release_count)
		return -EBUSY;

	spin_lock_irqsave(&session->fabric->chunk_lock, flags);
	for (index = 0; index < release->payload.chunk_ids.chunk_count; index++) {
		struct is_fabric_remote_chunk *chunk = is_find_provider_chunk(
			session, release->payload.chunk_ids.chunk_ids[index], NULL);

		if (!chunk || chunk->state != IS_REMOTE_CHUNK_MAPPED) {
			spin_unlock_irqrestore(&session->fabric->chunk_lock, flags);
			return -EPROTO;
		}
		chunk->state = IS_REMOTE_CHUNK_EVICTING;
		session->release_provider_ids[index] = chunk->provider_chunk_id;
	}
	spin_unlock_irqrestore(&session->fabric->chunk_lock, flags);
	session->release_count = release->payload.chunk_ids.chunk_count;
	session->pending_request_id = release->header.request_id;
	queue_work(session->control_wq, &session->release_work);
	return 1;
}

static void is_release_work(struct work_struct *work)
{
	struct is_rdma_session *session = container_of(
		work, struct is_rdma_session, release_work);
	struct is_protocol_message *response = &session->outbound_message;
	unsigned long flags;
	unsigned int index;
	int ret = 0;

	mutex_lock(&session->control_lock);
	for (index = 0; index < session->release_count; index++) {
		unsigned int logical_chunk_id;
		struct is_fabric_remote_chunk *chunk = is_find_provider_chunk(
			session, session->release_provider_ids[index],
			&logical_chunk_id);
		long waited;

		if (!chunk) {
			ret = -EPROTO;
			break;
		}
		waited = wait_event_timeout(session->chunk_wait,
			!atomic_read(&chunk->inflight) || READ_ONCE(session->stopping),
			msecs_to_jiffies(
				session->device->provider_failure_deadline_ms));
		if (!waited || READ_ONCE(session->stopping)) {
			ret = -ETIMEDOUT;
			break;
		}
		spin_lock_irqsave(&session->fabric->chunk_lock, flags);
		bitmap_clear(session->fabric->valid_sectors,
			(unsigned long)logical_chunk_id * IS_SECTORS_PER_CHUNK,
			IS_SECTORS_PER_CHUNK);
		chunk->state = IS_REMOTE_CHUNK_UNMAPPED;
		chunk->session_index = IS_FABRIC_NO_SESSION;
		chunk->remote_address = 0;
		chunk->remote_key = 0;
		atomic64_set(&chunk->activity, 0);
		if (session->fabric->mapped_per_session[session->provider_index])
			session->fabric->mapped_per_session[session->provider_index]--;
		atomic_dec(&session->device->mapped_remote_chunks);
		spin_unlock_irqrestore(&session->fabric->chunk_lock, flags);
	}
	if (!ret) {
		is_set_remote_capacity(session->fabric);
		is_init_message(session, response, IS_PROTOCOL_MSG_RELEASE,
			session->pending_request_id, true);
		response->payload.chunk_ids.chunk_count = session->release_count;
		for (index = 0; index < session->release_count; index++)
			response->payload.chunk_ids.chunk_ids[index] =
				session->release_provider_ids[index];
		ret = is_encode_and_send(session, response);
	}
	session->release_count = 0;
	session->pending_request_id = 0;
	mutex_unlock(&session->control_lock);
	if (ret)
		is_rdma_fail(session, ret);
	else if (is_post_control_receive(session))
		is_rdma_fail(session, -EIO);
}

static void is_receive_work(struct work_struct *work)
{
	struct is_rdma_session *session = container_of(
		work, struct is_rdma_session, receive_work);
	struct is_protocol_message *message = &session->inbound_message;
	enum is_protocol_result decode_result;
	bool repost = true;
	int ret = 0;

	mutex_lock(&session->control_lock);
	decode_result = is_protocol_decode(session->received_frame,
		session->received_size, message);
	if (decode_result != IS_PROTOCOL_OK) {
		ret = -EPROTO;
		goto out;
	}
	if (message->header.flags & IS_PROTOCOL_FLAG_RESPONSE &&
	    message->header.request_id == session->pending_request_id)
		is_cancel_control_deadline(session);
	if (message->header.type == IS_PROTOCOL_MSG_ERROR) {
		ret = -EREMOTEIO;
		goto out;
	}
	switch (session->control_state) {
	case IS_RDMA_CONTROL_WAIT_CHALLENGE:
		if (message->header.type != IS_PROTOCOL_MSG_CHALLENGE) {
			ret = -EPROTO;
			break;
		}
		memcpy(session->challenge_frame, session->received_frame,
			session->received_size);
		session->challenge_size = session->received_size;
		ret = is_handle_challenge(session, message);
		break;
	case IS_RDMA_CONTROL_WAIT_ACCEPT:
		ret = message->header.type == IS_PROTOCOL_MSG_ACCEPT ?
			is_handle_accept(session, message) : -EPROTO;
		break;
	case IS_RDMA_CONTROL_WAIT_STATUS:
		ret = message->header.type == IS_PROTOCOL_MSG_STATUS_RESPONSE ?
			is_handle_status(session, message) : -EPROTO;
		break;
	case IS_RDMA_CONTROL_WAIT_RESERVATION:
		ret = message->header.type == IS_PROTOCOL_MSG_CHUNK_GRANT ?
			is_handle_chunk_grant(session, message) : -EPROTO;
		break;
	case IS_RDMA_CONTROL_WAIT_HEARTBEAT:
		ret = message->header.type == IS_PROTOCOL_MSG_STATUS_RESPONSE ?
			is_handle_heartbeat(session, message) : -EPROTO;
		break;
	case IS_RDMA_CONTROL_READY:
		switch (message->header.type) {
		case IS_PROTOCOL_MSG_STATUS_RESPONSE:
			ret = is_remote_only(session) ? -EPROTO :
				is_handle_backed_heartbeat(session, message);
			break;
		case IS_PROTOCOL_MSG_CHUNK_GRANT:
			ret = is_handle_chunk_grant(session, message);
			break;
		case IS_PROTOCOL_MSG_EVICT:
			ret = is_handle_evict(session, message);
			break;
		case IS_PROTOCOL_MSG_RELEASE:
			ret = is_handle_release(session, message);
			if (ret == 1) {
				ret = 0;
				repost = false;
			}
			break;
		default:
			ret = -EPROTO;
			break;
		}
		break;
	default:
		ret = -ESHUTDOWN;
		break;
	}
out:
	mutex_unlock(&session->control_lock);
	if (ret)
		is_rdma_fail(session, ret);
	else if (repost && is_post_control_receive(session))
		is_rdma_fail(session, -EIO);
}

static void is_fabric_reservation_work(struct work_struct *work)
{
	struct is_rdma_fabric *fabric = container_of(work, struct is_rdma_fabric,
		reservation_work);
	struct is_device *device = fabric->device;
	unsigned int chosen_index;
	unsigned int total_available = 0;
	unsigned int index;
	struct is_rdma_session *session;
	int ret;

	if (fabric->reservation_complete || fabric->reservation_failed ||
	    READ_ONCE(fabric->stopping))
		return;

	if (fabric->reservation_next_logical == 0) {
		for (index = 0; index < fabric->session_count; index++)
			total_available +=
				is_session_available_chunks(fabric->sessions[index]);
		if (total_available < fabric->chunk_count) {
			is_fabric_complete_remote_only(fabric, -ENOSPC);
			for (index = 0; index < fabric->session_count; index++)
				is_rdma_fail(fabric->sessions[index], -ENOSPC);
			return;
		}
	}

	if (fabric->reservation_next_logical >= fabric->chunk_count) {
		if (!is_device_mark_remote_connected(device)) {
			is_fabric_complete_remote_only(fabric, -EIO);
			return;
		}
		fabric->reservation_complete = true;
		for (index = 0; index < fabric->session_count; index++)
			WRITE_ONCE(fabric->sessions[index]->reservation_complete,
				true);
		wake_up_all(&fabric->control_wait);
		return;
	}

	ret = is_fabric_choose_session(fabric, &chosen_index);
	if (ret) {
		is_fabric_complete_remote_only(fabric, ret);
		for (index = 0; index < fabric->session_count; index++)
			is_rdma_fail(fabric->sessions[index], ret);
		return;
	}

	session = fabric->sessions[chosen_index];
	mutex_lock(&session->control_lock);
	if (session->control_state != IS_RDMA_CONTROL_READY ||
	    session->pending_request_id) {
		mutex_unlock(&session->control_lock);
		return;
	}
	{
		unsigned long flags;
		struct is_fabric_remote_chunk *chunk =
			&fabric->chunks[fabric->reservation_next_logical];

		spin_lock_irqsave(&fabric->chunk_lock, flags);
		chunk->session_index = session->provider_index;
		spin_unlock_irqrestore(&fabric->chunk_lock, flags);
	}
	ret = is_request_remote_only_reservation(session,
		fabric->reservation_next_logical, 1);
	mutex_unlock(&session->control_lock);
	if (ret) {
		is_fabric_complete_remote_only(fabric, ret);
		is_rdma_fail(session, ret);
	}
}

static void is_fabric_mapping_work(struct work_struct *work)
{
	struct is_rdma_fabric *fabric = container_of(work, struct is_rdma_fabric,
		mapping_work);
	struct is_device *device = fabric->device;
	struct is_rdma_session *session = NULL;
	struct is_protocol_message *request;
	struct is_fabric_remote_chunk *chunk = NULL;
	unsigned long flags;
	u64 threshold = READ_ONCE(device->hot_range_threshold);
	unsigned int logical_index;
	unsigned int chosen_index;
	unsigned int mapped_on_session;
	unsigned int index;
	int ret = 0;

	if (is_remote_only_device(device) || READ_ONCE(fabric->stopping) ||
	    atomic_read(&device->connection_state) == IS_CONNECTION_CONNECTING)
		return;

	for (index = 0; index < fabric->session_count; index++) {
		if (fabric->sessions[index]->control_state == IS_RDMA_CONTROL_READY &&
		    fabric->sessions[index]->pending_request_id)
			return;
	}

	ret = is_fabric_choose_session(fabric, &chosen_index);
	if (ret)
		return;

	session = fabric->sessions[chosen_index];
	mutex_lock(&session->control_lock);
	if (session->control_state != IS_RDMA_CONTROL_READY ||
	    session->pending_request_id || session->heartbeat_request_id ||
	    session->release_count)
		goto out;
	mapped_on_session =
		fabric->mapped_per_session[session->provider_index];
	if (mapped_on_session >= session->remote_chunk_limit)
		goto out;

	spin_lock_irqsave(&fabric->chunk_lock, flags);
	for (logical_index = 0; logical_index < fabric->chunk_count;
	     logical_index++) {
		struct is_fabric_remote_chunk *candidate =
			&fabric->chunks[logical_index];

		if (candidate->state == IS_REMOTE_CHUNK_UNMAPPED &&
		    atomic64_read(&candidate->activity) >= threshold) {
			candidate->state = IS_REMOTE_CHUNK_MAPPING;
			candidate->session_index = session->provider_index;
			chunk = candidate;
			break;
		}
	}
	spin_unlock_irqrestore(&fabric->chunk_lock, flags);
	if (!chunk)
		goto out;

	request = &session->outbound_message;
	is_init_message(session, request, IS_PROTOCOL_MSG_CHUNK_REQUEST,
		session->next_request_id++, false);
	request->payload.chunk_request.chunk_count = 1;
	request->payload.chunk_request.logical_start = logical_index;
	request->payload.chunk_request.pool = IS_PROTOCOL_POOL_OPPORTUNISTIC;
	session->pending_request_id = request->header.request_id;
	session->pending_logical_chunk = logical_index;
	session->pending_chunk_count = 1;
	ret = is_encode_and_send(session, request);
	if (ret) {
		spin_lock_irqsave(&fabric->chunk_lock, flags);
		if (chunk->state == IS_REMOTE_CHUNK_MAPPING &&
		    chunk->session_index == session->provider_index) {
			chunk->state = IS_REMOTE_CHUNK_UNMAPPED;
			chunk->session_index = IS_FABRIC_NO_SESSION;
		}
		spin_unlock_irqrestore(&fabric->chunk_lock, flags);
		session->pending_request_id = 0;
		session->pending_chunk_count = 0;
	} else {
		is_arm_control_deadline(session);
	}
out:
	mutex_unlock(&session->control_lock);
	if (ret)
		is_rdma_fail(session, ret);
}

static void is_mapping_work(struct work_struct *work)
{
	struct is_rdma_session *session = container_of(work,
		struct is_rdma_session, mapping_work);

	queue_work(session->control_wq, &session->fabric->mapping_work);
}

static int is_setup_control_buffers(struct is_rdma_session *session)
{
	struct ib_device *device = session->cm_id->device;

	session->send_frame = is_session_kzalloc(session,
		IS_PROTOCOL_MAX_FRAME_SIZE, GFP_KERNEL);
	session->recv_frame = is_session_kzalloc(session,
		IS_PROTOCOL_MAX_FRAME_SIZE, GFP_KERNEL);
	session->received_frame = is_session_kzalloc(session,
		IS_PROTOCOL_MAX_FRAME_SIZE, GFP_KERNEL);
	if (!session->send_frame || !session->recv_frame ||
	    !session->received_frame)
		return -ENOMEM;
	session->send_dma = ib_dma_map_single(device, session->send_frame,
		IS_PROTOCOL_MAX_FRAME_SIZE, DMA_TO_DEVICE);
	if (ib_dma_mapping_error(device, session->send_dma)) {
		session->send_dma = 0;
		return -EIO;
	}
	session->recv_dma = ib_dma_map_single(device, session->recv_frame,
		IS_PROTOCOL_MAX_FRAME_SIZE, DMA_FROM_DEVICE);
	if (ib_dma_mapping_error(device, session->recv_dma)) {
		ib_dma_unmap_single(device, session->send_dma,
			IS_PROTOCOL_MAX_FRAME_SIZE, DMA_TO_DEVICE);
		session->send_dma = 0;
		session->recv_dma = 0;
		return -EIO;
	}

	session->send_completion.session = session;
	session->send_completion.cqe.done = is_control_send_done;
	session->recv_completion.session = session;
	session->recv_completion.cqe.done = is_control_receive_done;
	session->send_sge.addr = session->send_dma;
	session->send_sge.lkey = session->pd->local_dma_lkey;
	session->recv_sge.addr = session->recv_dma;
	session->recv_sge.length = IS_PROTOCOL_MAX_FRAME_SIZE;
	session->recv_sge.lkey = session->pd->local_dma_lkey;
	memset(&session->send_wr, 0, sizeof(session->send_wr));
	session->send_wr.wr_cqe = &session->send_completion.cqe;
	session->send_wr.sg_list = &session->send_sge;
	session->send_wr.num_sge = 1;
	session->send_wr.opcode = IB_WR_SEND;
	session->send_wr.send_flags = IB_SEND_SIGNALED;
	memset(&session->recv_wr, 0, sizeof(session->recv_wr));
	session->recv_wr.wr_cqe = &session->recv_completion.cqe;
	session->recv_wr.sg_list = &session->recv_sge;
	session->recv_wr.num_sge = 1;
	return 0;
}

static int is_setup_qp(struct is_rdma_session *session)
{
	struct ib_qp_init_attr qp_attributes;
	int ret;

	if (strcmp(session->cm_id->device->name, session->rdma_device) ||
	    session->cm_id->port_num != session->rdma_port)
		return -ENODEV;
	session->pd = ib_alloc_pd(session->cm_id->device, 0);
	if (IS_ERR(session->pd)) {
		ret = PTR_ERR(session->pd);
		session->pd = NULL;
		return ret;
	}
	session->cq = ib_alloc_cq(session->cm_id->device, session,
		IS_QUEUE_DEPTH + IS_RDMA_CONTROL_WR_COUNT, 0, IB_POLL_WORKQUEUE);
	if (IS_ERR(session->cq)) {
		ret = PTR_ERR(session->cq);
		session->cq = NULL;
		return ret;
	}
	memset(&qp_attributes, 0, sizeof(qp_attributes));
	qp_attributes.send_cq = session->cq;
	qp_attributes.recv_cq = session->cq;
	qp_attributes.qp_type = IB_QPT_RC;
	qp_attributes.cap.max_send_wr = IS_QUEUE_DEPTH + IS_RDMA_CONTROL_WR_COUNT;
	qp_attributes.cap.max_recv_wr = 4;
	qp_attributes.cap.max_send_sge = IS_RDMA_MAX_SEGMENTS;
	qp_attributes.cap.max_recv_sge = 1;
	ret = rdma_create_qp(session->cm_id, session->pd, &qp_attributes);
	if (ret)
		return ret;
	session->qp = session->cm_id->qp;
	ret = is_setup_control_buffers(session);
	if (ret)
		return ret;
	ret = is_post_control_receive(session);
	if (!ret)
		session->qp_has_work = true;
	return ret;
}

static int is_connect_qp(struct is_rdma_session *session)
{
	struct rdma_conn_param parameters;

	memset(&parameters, 0, sizeof(parameters));
	parameters.initiator_depth = 1;
	parameters.responder_resources = 1;
	parameters.retry_count = 7;
	parameters.rnr_retry_count = 7;
	session->connect_started = true;
	return rdma_connect(session->cm_id, &parameters);
}

static void is_connect_work(struct work_struct *work)
{
	struct is_rdma_session *session = container_of(
		work, struct is_rdma_session, connect_work);
	int ret;

	if (READ_ONCE(session->stopping))
		return;
	ret = is_setup_qp(session);
	if (!ret && !READ_ONCE(session->stopping))
		ret = is_connect_qp(session);
	if (ret)
		is_rdma_fail(session, ret);
}

static int is_rdma_cm_event(struct rdma_cm_id *id,
			    struct rdma_cm_event *event)
{
	struct is_rdma_session *session = id->context;
	int ret = 0;

	if (event->event == RDMA_CM_EVENT_DISCONNECTED)
		complete_all(&session->disconnect_complete);
	if (READ_ONCE(session->stopping) ||
	    READ_ONCE(session->control_state) == IS_RDMA_CONTROL_FAILED)
		return 0;
	switch (event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		if (!id->device || strcmp(id->device->name,
					  session->rdma_device) ||
		    id->port_num != session->rdma_port)
			ret = -ENODEV;
		else
			ret = rdma_resolve_route(id, IS_RDMA_CONTROL_TIMEOUT_MS);
		break;
	case RDMA_CM_EVENT_ROUTE_RESOLVED:
		/* rdma_connect() takes the CM handler lock held by this callback. */
		queue_work(session->control_wq, &session->connect_work);
		break;
	case RDMA_CM_EVENT_ESTABLISHED:
		/*
		 * Soft-RoCE loopback often needs a brief gap after ESTABLISHED
		 * before the peer's receive WR is armed; posting HELLO too early
		 * surfaces as IB_WC_RNR_RETRY_EXC_ERR. Use system_wq so the
		 * delay is not starved behind other ordered control work.
		 */
		queue_delayed_work(system_wq, &session->hello_work,
			msecs_to_jiffies(50));
		break;
	case RDMA_CM_EVENT_DISCONNECTED:
	case RDMA_CM_EVENT_REJECTED:
	case RDMA_CM_EVENT_ADDR_ERROR:
	case RDMA_CM_EVENT_ROUTE_ERROR:
	case RDMA_CM_EVENT_CONNECT_ERROR:
	case RDMA_CM_EVENT_UNREACHABLE:
	case RDMA_CM_EVENT_DEVICE_REMOVAL:
		atomic_set(&session->disconnect_started, 1);
		ret = -ECONNRESET;
		break;
	default:
		break;
	}
	if (ret)
		is_rdma_fail(session, ret);
	return 0;
}

static int is_resolve_provider(struct is_rdma_session *session)
{
	struct sockaddr_storage destination;
	struct sockaddr_in *ipv4 = (struct sockaddr_in *)&destination;
	struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)&destination;
	const char *address = session->provider_address;

	memset(&destination, 0, sizeof(destination));
	if (in4_pton(address, -1, (u8 *)&ipv4->sin_addr.s_addr, -1, NULL)) {
		ipv4->sin_family = AF_INET;
		ipv4->sin_port = htons(session->provider_port);
	} else if (in6_pton(address, -1, (u8 *)&ipv6->sin6_addr.s6_addr, -1,
			   NULL)) {
		ipv6->sin6_family = AF_INET6;
		ipv6->sin6_port = htons(session->provider_port);
	} else {
		return -EINVAL;
	}
	return rdma_resolve_addr(session->cm_id, NULL,
		(struct sockaddr *)&destination, IS_RDMA_CONTROL_TIMEOUT_MS);
}

static void is_session_copy_endpoint(struct is_rdma_session *session,
				     const struct is_provider_endpoint *endpoint)
{
	strscpy(session->provider_id, endpoint->name,
		sizeof(session->provider_id));
	strscpy(session->provider_address, endpoint->address,
		sizeof(session->provider_address));
	strscpy(session->rdma_device, endpoint->rdma_device,
		sizeof(session->rdma_device));
	strscpy(session->provider_key_id, endpoint->key_id,
		sizeof(session->provider_key_id));
	memcpy(session->provider_psk, endpoint->psk, endpoint->psk_size);
	session->provider_psk_size = endpoint->psk_size;
	session->provider_port = endpoint->port;
	session->rdma_port = endpoint->rdma_port;
	session->rdma_numa_node = endpoint->rdma_numa_node;
	session->placement_weight = endpoint->placement_weight;
}

static void is_session_copy_legacy(struct is_rdma_session *session,
				   struct is_device *device)
{
	strscpy(session->provider_id, "legacy", sizeof(session->provider_id));
	strscpy(session->provider_address, device->provider_address,
		sizeof(session->provider_address));
	strscpy(session->rdma_device, device->rdma_device,
		sizeof(session->rdma_device));
	strscpy(session->provider_key_id, device->provider_key_id,
		sizeof(session->provider_key_id));
	memcpy(session->provider_psk, device->provider_psk,
		device->provider_psk_size);
	session->provider_psk_size = device->provider_psk_size;
	session->provider_port = device->provider_port;
	session->rdma_port = device->rdma_port;
	session->rdma_numa_node = device->rdma_numa_node;
	session->placement_weight = IS_PLACEMENT_WEIGHT_DEFAULT;
}

static int is_session_init(struct is_rdma_fabric *fabric,
			   struct is_rdma_session *session,
			   unsigned int provider_index, bool remote_only)
{
	session->fabric = fabric;
	session->provider_index = provider_index;
	session->device = fabric->device;
	session->next_request_id = 1;
	session->negotiated_minor = IS_PROTOCOL_MINOR_CURRENT;
	session->negotiated_capabilities = IS_RDMA_CAPABILITIES;
	session->required_capabilities = remote_only ?
		IS_RDMA_REMOTE_ONLY_REQUIRED_CAPABILITIES :
		IS_RDMA_BACKED_REQUIRED_CAPABILITIES;
	session->control_state = IS_RDMA_CONTROL_CONNECTING;
	session->healthy = false;
	session->compatible = true;
	session->exclude_reason = IS_PLACEMENT_EXCLUDE_NONE;
	init_completion(&session->disconnect_complete);
	mutex_init(&session->control_lock);
	spin_lock_init(&session->chunk_lock);
	spin_lock_init(&session->operation_lock);
	INIT_LIST_HEAD(&session->operations);
	init_waitqueue_head(&session->send_wait);
	init_waitqueue_head(&session->chunk_wait);
	init_waitqueue_head(&session->control_wait);
	atomic_set(&session->send_busy, 0);
	atomic_set(&session->rdma_reads_inflight, 0);
	atomic_set(&session->operation_objects, 0);
	atomic_set(&session->disconnect_started, 0);
	session->failure_started = false;
	session->resolve_started = false;
	session->control_wq = alloc_ordered_workqueue("infiniswap-rdma",
		WQ_MEM_RECLAIM);
	if (!session->control_wq)
		return -ENOMEM;
	INIT_WORK(&session->connect_work, is_connect_work);
	INIT_DELAYED_WORK(&session->hello_work, is_hello_work);
	INIT_WORK(&session->receive_work, is_receive_work);
	INIT_WORK(&session->mapping_work, is_mapping_work);
	INIT_WORK(&session->release_work, is_release_work);
	INIT_WORK(&session->failure_work, is_failure_work);
	INIT_DELAYED_WORK(&session->heartbeat_work, is_heartbeat_work);
	INIT_DELAYED_WORK(&session->control_deadline_work,
		is_control_deadline);
	session->cm_id = rdma_create_id(&init_net, is_rdma_cm_event, session,
		RDMA_PS_TCP, IB_QPT_RC);
	if (IS_ERR(session->cm_id)) {
		int ret = PTR_ERR(session->cm_id);

		session->cm_id = NULL;
		destroy_workqueue(session->control_wq);
		return ret;
	}
	return 0;
}

static void is_session_destroy(struct is_rdma_session *session)
{
	if (!session)
		return;
	WRITE_ONCE(session->stopping, true);
	wake_up_all(&session->send_wait);
	wake_up_all(&session->chunk_wait);
	wake_up_all(&session->control_wait);
	mutex_lock(&session->control_lock);
	session->control_state = IS_RDMA_CONTROL_STOPPING;
	session->control_deadline_armed = false;
	mutex_unlock(&session->control_lock);
	cancel_delayed_work_sync(&session->control_deadline_work);
	cancel_delayed_work_sync(&session->heartbeat_work);
	cancel_work_sync(&session->connect_work);
	if (session->connect_started && session->cm_id) {
		int ret = rdma_disconnect(session->cm_id);

		if (!ret && READ_ONCE(session->ever_connected) &&
		    !READ_ONCE(session->failure_started))
			wait_for_completion_timeout(&session->disconnect_complete,
				msecs_to_jiffies(2 *
					session->device->provider_failure_deadline_ms));
	}
	cancel_delayed_work_sync(&session->hello_work);
	cancel_work_sync(&session->receive_work);
	cancel_work_sync(&session->mapping_work);
	cancel_work_sync(&session->release_work);
	cancel_work_sync(&session->failure_work);
	is_free_control_resources(session);
	if (session->cm_id) {
		rdma_destroy_id(session->cm_id);
		session->cm_id = NULL;
	}
	destroy_workqueue(session->control_wq);
	kfree(session);
}

int is_rdma_start(struct is_device *device)
{
	struct is_rdma_fabric *fabric;
	unsigned long valid_words;
	unsigned int index;
	unsigned int provider_count = device->provider_count;
	bool remote_only = device->mode == IS_DEVICE_MODE_REMOTE_ONLY;
	bool remote_configured = false;
	int ret;

	if (device->mode != IS_DEVICE_MODE_BACKED && !remote_only)
		return 0;
	if (provider_count > 0) {
		remote_configured =
			!(device->capacity_bytes % IS_CHUNK_BYTES) &&
			device->capacity_bytes <=
				IS_CHUNK_BYTES * IS_MAX_REMOTE_CHUNKS;
		for (index = 0; index < provider_count; index++) {
			if (!device->provider_endpoints[index].configured) {
				remote_configured = false;
				break;
			}
		}
	} else {
		remote_configured =
			!(device->capacity_bytes % IS_CHUNK_BYTES) &&
			device->capacity_bytes <=
				IS_CHUNK_BYTES * IS_MAX_REMOTE_CHUNKS &&
			device->provider_address[0] && device->provider_port &&
			device->rdma_device[0] && device->rdma_port &&
			device->provider_key_id[0] &&
			device->provider_psk_size >= IS_PSK_MIN_SIZE;
	}
	if (!remote_configured) {
		if (!remote_only &&
		    device->acknowledgement_policy ==
			IS_ACKNOWLEDGEMENT_POLICY_STRICT)
			return 0;
		if (remote_only)
			return -EINVAL;
		return 0;
	}

	fabric = is_kzalloc_numa(device, sizeof(*fabric), GFP_KERNEL);
	if (!fabric)
		return -ENOMEM;
	fabric->device = device;
	fabric->chunk_count = device->capacity_bytes / IS_CHUNK_BYTES;
	spin_lock_init(&fabric->chunk_lock);
	init_waitqueue_head(&fabric->control_wait);
	INIT_WORK(&fabric->mapping_work, is_fabric_mapping_work);
	INIT_WORK(&fabric->reservation_work, is_fabric_reservation_work);
	INIT_DELAYED_WORK(&fabric->start_next_session_work,
		is_fabric_start_next_session_work);
	if (device->placement_seed)
		fabric->placement_rng_state = device->placement_seed;
	for (index = 0; index < fabric->chunk_count; index++) {
		atomic64_set(&fabric->chunks[index].activity, 0);
		atomic_set(&fabric->chunks[index].inflight, 0);
		fabric->chunks[index].session_index = IS_FABRIC_NO_SESSION;
	}
	valid_words = BITS_TO_LONGS(device->capacity_sectors);
	fabric->valid_sectors = kvcalloc(valid_words, sizeof(unsigned long),
		GFP_KERNEL);
	if (!fabric->valid_sectors) {
		kfree(fabric);
		return -ENOMEM;
	}
	device->rdma = fabric;
	fabric->planned_count = provider_count ? provider_count : 1;

	if (remote_only)
		atomic_set(&fabric->sessions_awaiting_status,
			fabric->planned_count);

	atomic_set(&device->connection_state, IS_CONNECTION_CONNECTING);
	is_fabric_start_next_session(fabric);
	if (!fabric->session_count) {
		ret = remote_only ? -ENODEV : 0;
		if (remote_only)
			goto stop_started;
		is_rdma_stop(device);
		return 0;
	}
	if (!remote_only)
		return 0;

	ret = wait_event_interruptible(fabric->control_wait,
		fabric->reservation_complete || fabric->reservation_failed);
	if (ret)
		goto stop_started;
	if (fabric->reservation_complete)
		return 0;
	ret = fabric->reservation_error;
	return ret ? ret : -EIO;

stop_started:
	is_rdma_stop(device);
	return ret;
}

static void is_free_control_resources(struct is_rdma_session *session)
{
	struct ib_device *device = session->cm_id ? session->cm_id->device : NULL;

	if (session->qp) {
		if (session->qp_has_work)
			ib_drain_qp(session->qp);
		wait_event(session->chunk_wait,
			!atomic_read(&session->operation_objects));
		cancel_delayed_work_sync(&session->hello_work);
		cancel_work_sync(&session->receive_work);
		cancel_work_sync(&session->mapping_work);
		cancel_work_sync(&session->release_work);
		cancel_work_sync(&session->failure_work);
		WARN_ON_ONCE(!list_empty(&session->operations));
		rdma_destroy_qp(session->cm_id);
		session->qp = NULL;
	}
	if (device && session->send_dma)
		ib_dma_unmap_single(device, session->send_dma,
			IS_PROTOCOL_MAX_FRAME_SIZE, DMA_TO_DEVICE);
	if (device && session->recv_dma)
		ib_dma_unmap_single(device, session->recv_dma,
			IS_PROTOCOL_MAX_FRAME_SIZE, DMA_FROM_DEVICE);
	session->send_dma = 0;
	session->recv_dma = 0;
	kfree_sensitive(session->send_frame);
	kfree(session->recv_frame);
	kfree(session->received_frame);
	session->send_frame = NULL;
	session->recv_frame = NULL;
	session->received_frame = NULL;
	if (session->cq) {
		ib_free_cq(session->cq);
		session->cq = NULL;
	}
	if (session->pd) {
		ib_dealloc_pd(session->pd);
		session->pd = NULL;
	}
}

void is_rdma_stop(struct is_device *device)
{
	struct is_rdma_fabric *fabric = device->rdma;
	unsigned int index;

	if (!fabric)
		return;
	device->rdma = NULL;
	WRITE_ONCE(fabric->stopping, true);
	atomic_set(&device->connection_state,
		atomic_read(&device->remote_lost) ? IS_CONNECTION_REMOTE_LOST :
		IS_CONNECTION_NOT_CONNECTED);
	wake_up_all(&fabric->control_wait);
	if (fabric->session_count) {
		cancel_delayed_work_sync(&fabric->start_next_session_work);
		cancel_work_sync(&fabric->mapping_work);
		cancel_work_sync(&fabric->reservation_work);
	}
	for (index = 0; index < IS_MAX_PROVIDERS; index++) {
		if (fabric->sessions[index])
			is_session_destroy(fabric->sessions[index]);
		fabric->sessions[index] = NULL;
	}
	kvfree(fabric->valid_sectors);
	atomic_set(&device->mapped_remote_chunks, 0);
	WRITE_ONCE(device->remote_capacity_bytes, 0);
	kfree(fabric);
}

bool is_rdma_range_mapped(struct is_device *device, sector_t sector,
			  unsigned int bytes)
{
	struct is_rdma_fabric *fabric = is_device_fabric(device);
	struct is_fabric_remote_chunk *chunk;
	unsigned long flags;
	unsigned long sector_count = bytes >> 9;
	unsigned int logical_chunk;
	bool mapped;

	if (!fabric || !bytes || (bytes & 511U) ||
	    atomic_read(&device->connection_state) != IS_CONNECTION_CONNECTED)
		return false;
	is_fabric_mapped_chunk_count(device);
	logical_chunk = div_u64((u64)sector, IS_SECTORS_PER_CHUNK);
	if (logical_chunk >= fabric->chunk_count ||
	    div_u64((u64)sector + sector_count - 1U,
		    IS_SECTORS_PER_CHUNK) != logical_chunk)
		return false;
	chunk = &fabric->chunks[logical_chunk];
	spin_lock_irqsave(&fabric->chunk_lock, flags);
	mapped = is_fabric_chunk_mapped_locked(fabric, chunk);
	spin_unlock_irqrestore(&fabric->chunk_lock, flags);
	return mapped;
}

bool is_rdma_range_valid(struct is_device *device, sector_t sector,
			 unsigned int bytes)
{
	struct is_rdma_fabric *fabric = is_device_fabric(device);
	struct is_fabric_remote_chunk *chunk;
	unsigned long flags;
	unsigned long sector_count = bytes >> 9;
	unsigned int logical_chunk;
	bool valid;

	if (!fabric || !bytes || (bytes & 511U) ||
	    atomic_read(&device->connection_state) != IS_CONNECTION_CONNECTED)
		return false;
	logical_chunk = div_u64((u64)sector, IS_SECTORS_PER_CHUNK);
	if (logical_chunk >= fabric->chunk_count ||
	    div_u64((u64)sector + sector_count - 1U,
		    IS_SECTORS_PER_CHUNK) != logical_chunk)
		return false;
	chunk = &fabric->chunks[logical_chunk];
	spin_lock_irqsave(&fabric->chunk_lock, flags);
	valid = is_fabric_chunk_mapped_locked(fabric, chunk) &&
		find_next_zero_bit(fabric->valid_sectors,
			sector + sector_count, sector) >= sector + sector_count;
	spin_unlock_irqrestore(&fabric->chunk_lock, flags);
	return valid;
}

static void is_operation_put(struct is_rdma_operation *operation)
{
	struct is_rdma_session *session = operation->session;

	if (!refcount_dec_and_test(&operation->references))
		return;
	atomic_dec(&session->operation_objects);
	wake_up_all(&session->chunk_wait);
	kfree(operation);
}

static void is_set_operation_remote_valid(struct is_rdma_operation *operation,
					  bool valid)
{
	struct is_rdma_fabric *fabric = operation->session->fabric;
	unsigned long flags;

	if (!operation->io->write)
		return;
	spin_lock_irqsave(&fabric->chunk_lock, flags);
	if (valid)
		bitmap_set(fabric->valid_sectors, operation->io->sector,
			operation->io->bytes >> 9);
	else
		bitmap_clear(fabric->valid_sectors, operation->io->sector,
			operation->io->bytes >> 9);
	spin_unlock_irqrestore(&fabric->chunk_lock, flags);
}

static void is_operation_deadline(struct work_struct *work)
{
	struct is_rdma_operation *operation = container_of(
		to_delayed_work(work), struct is_rdma_operation, deadline_work);
	struct is_rdma_session *session = operation->session;

	if (atomic_cmpxchg(&operation->callback_complete, 0, 1) == 0) {
		atomic_set(&operation->timed_out, 1);
		is_set_operation_remote_valid(operation, false);
		atomic64_inc(&session->device->provider_timeouts_total);
		is_fail_after_provider_timeout(session);
		operation->io->complete(operation->io->context,
			operation->io->generation, -ETIMEDOUT, true);
	}
	is_operation_put(operation);
}

static void is_complete_operation(struct is_rdma_operation *operation,
				  int status)
{
	struct is_rdma_session *session = operation->session;
	bool callback_ready;
	bool remote_valid;
	unsigned long flags;
	unsigned int index;

	spin_lock_irqsave(&session->operation_lock, flags);
	callback_ready = atomic_cmpxchg(&operation->callback_complete, 0, 1) == 0;
	spin_unlock_irqrestore(&session->operation_lock, flags);
	if (cancel_delayed_work(&operation->deadline_work))
		is_operation_put(operation);
	for (index = 0; index < operation->mapped_segments; index++)
		ib_dma_unmap_page(session->cm_id->device,
			operation->dma_addresses[index],
			operation->io->segments[index].length,
			operation->direction);
	if (!operation->io->write)
		atomic_set(&session->rdma_reads_inflight, 0);
	spin_lock_irqsave(&session->operation_lock, flags);
	list_del(&operation->list);
	spin_unlock_irqrestore(&session->operation_lock, flags);
	atomic_dec(&operation->chunk->inflight);
	wake_up_all(&session->chunk_wait);
	if (callback_ready) {
		spin_lock_irqsave(&session->operation_lock, flags);
		if (!status && is_remote_only(session) &&
		    atomic_read(&session->device->remote_lost))
			status = -EIO;
		remote_valid = !status &&
			atomic_read(&session->device->connection_state) ==
				IS_CONNECTION_CONNECTED;
		is_set_operation_remote_valid(operation, remote_valid);
		operation->io->complete(operation->io->context,
			operation->io->generation, status, false);
		spin_unlock_irqrestore(&session->operation_lock, flags);
	} else {
		is_set_operation_remote_valid(operation, false);
		if (atomic_read(&operation->timed_out))
			atomic64_inc(&session->device->late_rdma_completions_total);
	}
	operation->io->release(operation->io->context);
	is_operation_put(operation);
}

static void is_data_completion(struct ib_cq *cq, struct ib_wc *wc)
{
	struct is_rdma_operation *operation = container_of(
		wc->wr_cqe, struct is_rdma_operation, cqe);
	int status = wc->status == IB_WC_SUCCESS ? 0 : -EIO;

	(void)cq;
	if (status)
		is_rdma_fail(operation->session, status);
	is_complete_operation(operation, status);
}

int is_rdma_submit(struct is_device *device, struct is_rdma_io *io)
{
	struct is_rdma_fabric *fabric = is_device_fabric(device);
	struct is_rdma_session *session;
	struct is_rdma_operation *operation;
	struct is_fabric_remote_chunk *chunk;
	const struct ib_send_wr *bad_wr;
	unsigned long flags;
	u64 remote_offset;
	unsigned int logical_chunk;
	unsigned int index;
	bool read_claimed = false;
	int ret;

	if (!fabric || !io || !io->complete || !io->release ||
	    !io->generation || !io->segment_count ||
	    io->segment_count > IS_RDMA_MAX_SEGMENTS || !io->bytes ||
	    (io->bytes & 511U) ||
	    atomic_read(&device->connection_state) != IS_CONNECTION_CONNECTED)
		return -ENOTCONN;
	logical_chunk = div_u64((u64)io->sector, IS_SECTORS_PER_CHUNK);
	if (logical_chunk >= fabric->chunk_count ||
	    div_u64((u64)io->sector + (io->bytes >> 9) - 1U,
		    IS_SECTORS_PER_CHUNK) != logical_chunk)
		return -ERANGE;
	chunk = &fabric->chunks[logical_chunk];
	spin_lock_irqsave(&fabric->chunk_lock, flags);
	if (!is_fabric_chunk_mapped_locked(fabric, chunk)) {
		spin_unlock_irqrestore(&fabric->chunk_lock, flags);
		return -ENXIO;
	}
	session = fabric->sessions[chunk->session_index];
	spin_unlock_irqrestore(&fabric->chunk_lock, flags);
	if (!session || READ_ONCE(session->stopping))
		return -ENOTCONN;
	if (!io->write && device->mode != IS_DEVICE_MODE_REMOTE_ONLY) {
		if (atomic_cmpxchg(&session->rdma_reads_inflight, 0, 1) != 0)
			return -EAGAIN;
		read_claimed = true;
	}
	spin_lock_irqsave(&fabric->chunk_lock, flags);
	if (!is_fabric_chunk_mapped_locked(fabric, chunk)) {
		spin_unlock_irqrestore(&fabric->chunk_lock, flags);
		ret = -ENXIO;
		goto release_read;
	}
	atomic_inc(&chunk->inflight);
	if (io->write)
		bitmap_clear(fabric->valid_sectors, io->sector, io->bytes >> 9);
	remote_offset = ((u64)io->sector % IS_SECTORS_PER_CHUNK) << 9;
	spin_unlock_irqrestore(&fabric->chunk_lock, flags);

	operation = kzalloc(sizeof(*operation), GFP_KERNEL);
	if (!operation) {
		ret = -ENOMEM;
		goto release_chunk;
	}
	operation->session = session;
	operation->io = io;
	operation->chunk = chunk;
	operation->logical_chunk_id = logical_chunk;
	operation->direction = io->write ? DMA_TO_DEVICE : DMA_FROM_DEVICE;
	operation->cqe.done = is_data_completion;
	atomic_set(&operation->callback_complete, 0);
	atomic_set(&operation->timed_out, 0);
	INIT_DELAYED_WORK(&operation->deadline_work, is_operation_deadline);
	INIT_LIST_HEAD(&operation->list);
	for (index = 0; index < io->segment_count; index++) {
		u64 dma_address = ib_dma_map_page(session->cm_id->device,
			io->segments[index].page, io->segments[index].offset,
			io->segments[index].length, operation->direction);

		if (ib_dma_mapping_error(session->cm_id->device, dma_address)) {
			ret = -EIO;
			goto unmap_segments;
		}
		operation->dma_addresses[index] = dma_address;
		operation->sges[index].addr = dma_address;
		operation->sges[index].length = io->segments[index].length;
		operation->sges[index].lkey = session->pd->local_dma_lkey;
		operation->mapped_segments++;
	}
	memset(&operation->wr, 0, sizeof(operation->wr));
	operation->wr.wr.wr_cqe = &operation->cqe;
	operation->wr.wr.sg_list = operation->sges;
	operation->wr.wr.num_sge = operation->mapped_segments;
	operation->wr.wr.opcode = io->write ? IB_WR_RDMA_WRITE : IB_WR_RDMA_READ;
	operation->wr.wr.send_flags = IB_SEND_SIGNALED;
	operation->wr.remote_addr = chunk->remote_address + remote_offset;
	operation->wr.rkey = chunk->remote_key;
	refcount_set(&operation->references, 3);
	atomic_inc(&session->operation_objects);
	spin_lock_irqsave(&session->operation_lock, flags);
	list_add_tail(&operation->list, &session->operations);
	spin_unlock_irqrestore(&session->operation_lock, flags);
	ret = ib_post_send(session->qp, &operation->wr.wr, &bad_wr);
	if (!ret) {
		if (atomic_read(&operation->callback_complete)) {
			is_operation_put(operation);
		} else {
			schedule_delayed_work(&operation->deadline_work,
				msecs_to_jiffies(
					device->provider_failure_deadline_ms));
		}
		is_operation_put(operation);
		return 0;
	}
	spin_lock_irqsave(&session->operation_lock, flags);
	list_del(&operation->list);
	spin_unlock_irqrestore(&session->operation_lock, flags);
unmap_segments:
	while (operation->mapped_segments) {
		index = --operation->mapped_segments;
		ib_dma_unmap_page(session->cm_id->device,
			operation->dma_addresses[index],
			io->segments[index].length, operation->direction);
	}
	if (refcount_read(&operation->references)) {
		is_operation_put(operation);
		is_operation_put(operation);
		is_operation_put(operation);
	} else {
		kfree(operation);
	}
release_chunk:
	atomic_dec(&chunk->inflight);
	wake_up_all(&session->chunk_wait);
release_read:
	if (read_claimed)
		atomic_set(&session->rdma_reads_inflight, 0);
	return ret;
}

int is_rdma_flush(struct is_device *device)
{
	struct is_rdma_fabric *fabric = is_device_fabric(device);
	unsigned int index;
	bool pending = false;

	if (!fabric || atomic_read(&device->connection_state) !=
			IS_CONNECTION_CONNECTED)
		return -ENOTCONN;
	for (index = 0; index < fabric->session_count; index++) {
		struct is_rdma_session *session = fabric->sessions[index];

		if (!session)
			continue;
		wait_event(session->chunk_wait,
			!atomic_read(&session->operation_objects) ||
			READ_ONCE(session->stopping) ||
			atomic_read(&device->remote_lost));
		if (atomic_read(&session->operation_objects))
			pending = true;
	}
	if (READ_ONCE(fabric->stopping) || atomic_read(&device->remote_lost) ||
	    atomic_read(&device->connection_state) != IS_CONNECTION_CONNECTED ||
	    pending)
		return -EIO;
	return 0;
}

void is_rdma_mapping_parameters_changed(struct is_device *device)
{
	struct is_rdma_fabric *fabric = is_device_fabric(device);

	if (fabric && !READ_ONCE(fabric->stopping) && fabric->session_count)
		queue_work(fabric->sessions[0]->control_wq,
			&fabric->mapping_work);
}

void is_rdma_note_activity(struct is_device *device, sector_t sector,
			   unsigned int bytes, bool write)
{
	struct is_rdma_fabric *fabric = is_device_fabric(device);
	unsigned int first_chunk;
	unsigned int last_chunk;
	u32 weight;
	unsigned int index;

	if (!fabric || !bytes || !fabric->session_count)
		return;
	first_chunk = div_u64((u64)sector, IS_SECTORS_PER_CHUNK);
	last_chunk = div_u64((u64)sector + (bytes >> 9) - 1U,
		IS_SECTORS_PER_CHUNK);
	if (first_chunk >= fabric->chunk_count)
		return;
	if (last_chunk >= fabric->chunk_count)
		last_chunk = fabric->chunk_count - 1U;
	weight = write ? READ_ONCE(device->hot_range_write_weight) :
		READ_ONCE(device->hot_range_read_weight);
	for (index = first_chunk; index <= last_chunk; index++) {
		s64 previous;
		s64 updated;

		do {
			previous = atomic64_read(&fabric->chunks[index].activity);
			updated = previous > S64_MAX - weight ? S64_MAX :
				previous + weight;
		} while (atomic64_cmpxchg(&fabric->chunks[index].activity,
			 previous, updated) != previous);
	}
	queue_work(fabric->sessions[0]->control_wq, &fabric->mapping_work);
}

ssize_t is_rdma_remote_chunk_placements_show(struct is_device *device,
					     char *page)
{
	struct is_rdma_fabric *fabric = is_device_fabric(device);
	ssize_t written = 0;
	unsigned long flags;
	unsigned int index;

	if (!fabric)
		return sysfs_emit(page, "\n");
	spin_lock_irqsave(&fabric->chunk_lock, flags);
	for (index = 0; index < fabric->chunk_count; index++) {
		struct is_fabric_remote_chunk *chunk = &fabric->chunks[index];
		const char *provider_id = "unmapped";
		unsigned int session_index = chunk->session_index;

		if (is_fabric_chunk_mapped_locked(fabric, chunk) &&
		    session_index < fabric->session_count)
			provider_id = fabric->sessions[session_index]->provider_id;
		written += sysfs_emit_at(page, written, "%u:%s%s", index,
			provider_id,
			index + 1U < fabric->chunk_count ? " " : "\n");
	}
	spin_unlock_irqrestore(&fabric->chunk_lock, flags);
	if (!fabric->chunk_count)
		written += sysfs_emit(page, "\n");
	return written;
}

ssize_t is_rdma_provider_exclusions_show(struct is_device *device, char *page)
{
	struct is_rdma_fabric *fabric = is_device_fabric(device);
	ssize_t written = 0;
	unsigned int index;
	unsigned int emitted = 0;

	if (!fabric)
		return sysfs_emit(page, "\n");
	for (index = 0; index < fabric->session_count; index++) {
		struct is_rdma_session *session = fabric->sessions[index];

		if (session->exclude_reason == IS_PLACEMENT_EXCLUDE_NONE)
			continue;
		written += sysfs_emit_at(page, written, "%s%s:%s",
			emitted ? " " : "", session->provider_id,
			is_placement_exclude_reason_name(
				session->exclude_reason));
		emitted++;
	}
	if (!emitted)
		written += sysfs_emit(page, "\n");
	return written;
}
