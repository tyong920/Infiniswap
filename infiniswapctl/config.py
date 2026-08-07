"""Versioned configuration loading and semantic validation."""

import json
import re
import stat
from dataclasses import dataclass, field, replace
from pathlib import Path
from typing import Any, Dict, Iterable, List, Mapping, Optional, Set, Tuple

GIB = 1024 * 1024 * 1024
CONFIGFS_VALUE_SIZE = 4096
FAILURE_DEADLINE_MIN_MS = 500
FAILURE_DEADLINE_MAX_MS = 30000
HOT_RANGE_SCORE_MAX = (1 << 63) - 1
HOT_RANGE_WEIGHT_MAX = 1000000
HOT_RANGE_THRESHOLD_DEFAULT = 8
HOT_RANGE_READ_WEIGHT_DEFAULT = 1
HOT_RANGE_WRITE_WEIGHT_DEFAULT = 4
PSK_MIN_BYTES = 32
PSK_MAX_BYTES = 64
IDENTIFIER = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,62}$")
KEY_IDENTIFIER = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,30}$")
DEVICE_NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_-]{0,30}$")
CAPABILITIES = {
    "backed",
    "remote_only",
    "opportunistic_pool",
    "committed_pool",
    "provider_failure_deadline",
    "status",
    "auth_hmac_sha256",
}


class ConfigError(ValueError):
    """A configuration document is unsafe or incompatible."""


@dataclass(frozen=True)
class ProviderDirectoryEntry:
    name: str
    address: str
    port: int
    rail_device: str
    rail_port: int
    numa_node: int
    consumer_id: str
    key_id: str
    psk_file: str
    capabilities: Tuple[str, ...]
    placement_weight: int
    psk: bytes = field(default=b"", repr=False, compare=False)


@dataclass(frozen=True)
class ConsumerConfig:
    path: str
    consumer_id: str
    name: str
    mode: str
    acknowledgement_policy: str
    backing_store: str
    capacity_bytes: int
    provider_failure_deadline_ms: int
    hot_range_threshold: int
    hot_range_read_weight: int
    hot_range_write_weight: int
    provider_directory: str
    providers: Tuple[ProviderDirectoryEntry, ...]
    swap_priority: int


@dataclass(frozen=True)
class ProviderConfig:
    path: str
    provider_id: str
    address: str
    port: int
    rail_device: str
    rail_port: int
    numa_node: int
    host_reserve_gib: int
    max_opportunistic_gib: int
    max_committed_gib: int
    consumer_count: int


def _reject_duplicate_keys(pairs: Iterable[Tuple[str, Any]]) -> Dict[str, Any]:
    result: Dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ConfigError("duplicate configuration key: " + key)
        result[key] = value
    return result


def load_json(path: str) -> Dict[str, Any]:
    try:
        with Path(path).open("r", encoding="utf-8") as stream:
            document = json.load(stream, object_pairs_hook=_reject_duplicate_keys)
    except ConfigError:
        raise
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ConfigError("could not read configuration: " + str(exc)) from exc
    if not isinstance(document, dict):
        raise ConfigError("configuration must be a JSON object")
    return document


def _object(value: Any, field: str) -> Mapping[str, Any]:
    if not isinstance(value, dict):
        raise ConfigError(field + " is required")
    return value


def _strict_fields(
    value: Mapping[str, Any],
    field: str,
    required: Set[str],
    optional: Optional[Set[str]] = None,
) -> None:
    optional = optional or set()
    missing = sorted(required - set(value))
    if missing:
        raise ConfigError(field + "." + missing[0] + " is required")
    unknown = sorted(set(value) - required - optional)
    if unknown:
        raise ConfigError(field + " contains unknown field " + unknown[0])


def _integer(value: Any, field: str, minimum: int, maximum: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ConfigError(field + " must be an integer")
    if value < minimum or value > maximum:
        raise ConfigError("%s must be between %d and %d" % (field, minimum, maximum))
    return value


def _identifier(value: Any, field: str) -> str:
    if not isinstance(value, str) or not IDENTIFIER.fullmatch(value):
        raise ConfigError(field + " must be a 1-63 character identifier")
    return value


def _key_identifier(value: Any, field: str) -> str:
    if not isinstance(value, str) or not KEY_IDENTIFIER.fullmatch(value):
        raise ConfigError(field + " must be a 1-31 character key identifier")
    return value


def _absolute_path(value: Any, field: str) -> str:
    if not isinstance(value, str) or not value.startswith("/") or "\x00" in value:
        raise ConfigError(field + " must be an absolute path")
    return value


def _secure_psk_file(path: str, system: Any, field: str) -> None:
    try:
        details = system.stat_file(path)
    except (KeyError, OSError) as exc:
        raise ConfigError(field + " is not readable: " + path) from exc
    if not stat.S_ISREG(details.st_mode):
        raise ConfigError(field + " must be a regular file")
    if details.st_uid != 0 or stat.S_IMODE(details.st_mode) != 0o600:
        raise ConfigError(field + " must be owned by root with mode 0600")


def _read_psk(path: str, system: Any, field: str) -> bytes:
    try:
        value = system.read_secret(path)
    except (KeyError, OSError) as exc:
        raise ConfigError(field + " is not readable: " + path) from exc
    if not isinstance(value, bytes):
        raise ConfigError(field + " must contain a binary or hexadecimal PSK")

    stripped = value.strip()
    try:
        encoded = stripped.decode("ascii")
    except UnicodeDecodeError:
        encoded = ""
    if (
        PSK_MIN_BYTES * 2 <= len(encoded) <= PSK_MAX_BYTES * 2
        and len(encoded) % 2 == 0
        and re.fullmatch(r"[0-9A-Fa-f]+", encoded)
    ):
        value = bytes.fromhex(encoded)
    if not PSK_MIN_BYTES <= len(value) <= PSK_MAX_BYTES:
        raise ConfigError(field + " must contain a 32-64 byte PSK")
    return value


def load_provider_directory(
    path: str, system: Any
) -> Dict[str, ProviderDirectoryEntry]:
    document = load_json(path)
    _strict_fields(document, "provider_directory", {"schema_version", "providers"})
    schema_version = document.get("schema_version")
    if schema_version not in (1, 2):
        raise ConfigError("provider_directory.schema_version must be 1 or 2")
    providers = _object(document["providers"], "provider_directory.providers")
    if not providers or len(providers) > 64:
        raise ConfigError("provider_directory.providers must contain 1-64 Providers")

    result: Dict[str, ProviderDirectoryEntry] = {}
    for name in sorted(providers):
        _identifier(name, "provider_directory.providers key")
        field = "provider_directory.providers." + name
        provider = _object(providers[name], field)
        _strict_fields(
            provider,
            field,
            {
                "address",
                "port",
                "rdma_rail",
                "authentication",
                "expected_capabilities",
                "placement_weight",
            },
        )
        address = provider["address"]
        if not isinstance(address, str) or not address or len(address) > 253:
            raise ConfigError(field + ".address must be a non-empty network address")
        port = _integer(provider["port"], field + ".port", 1, 65535)

        rail = _object(provider["rdma_rail"], field + ".rdma_rail")
        _strict_fields(rail, field + ".rdma_rail", {"device", "port", "numa_node"})
        rail_device = _identifier(rail["device"], field + ".rdma_rail.device")
        rail_port = _integer(rail["port"], field + ".rdma_rail.port", 1, 255)
        numa_node = _integer(
            rail["numa_node"],
            field + ".rdma_rail.numa_node",
            -1 if schema_version >= 2 else 0,
            65535,
        )

        authentication = _object(provider["authentication"], field + ".authentication")
        _strict_fields(
            authentication,
            field + ".authentication",
            {"consumer_id", "key_id", "psk_file"},
        )
        consumer_id = _identifier(
            authentication["consumer_id"], field + ".authentication.consumer_id"
        )
        key_id = _key_identifier(
            authentication["key_id"], field + ".authentication.key_id"
        )
        psk_file = _absolute_path(
            authentication["psk_file"], field + ".authentication.psk_file"
        )
        _secure_psk_file(psk_file, system, field + ".authentication.psk_file")

        capabilities_value = provider["expected_capabilities"]
        if (
            not isinstance(capabilities_value, list)
            or not capabilities_value
            or any(not isinstance(item, str) for item in capabilities_value)
        ):
            raise ConfigError(field + ".expected_capabilities must be a non-empty list")
        if len(set(capabilities_value)) != len(capabilities_value):
            raise ConfigError(field + ".expected_capabilities must be unique")
        unknown_capabilities = sorted(set(capabilities_value) - CAPABILITIES)
        if unknown_capabilities:
            raise ConfigError(
                field
                + ".expected_capabilities contains unknown capability "
                + unknown_capabilities[0]
            )
        placement_weight = _integer(
            provider["placement_weight"], field + ".placement_weight", 1, 1000
        )
        result[name] = ProviderDirectoryEntry(
            name=name,
            address=address,
            port=port,
            rail_device=rail_device,
            rail_port=rail_port,
            numa_node=numa_node,
            consumer_id=consumer_id,
            key_id=key_id,
            psk_file=psk_file,
            capabilities=tuple(capabilities_value),
            placement_weight=placement_weight,
        )
    return result


def _provider_authentication(value: Any, field: str, system: Any) -> None:
    authentication = _object(value, field)
    _strict_fields(authentication, field, {"current"}, {"next"})
    current = _object(authentication["current"], field + ".current")
    _strict_fields(current, field + ".current", {"key_id", "psk_file"})
    current_key_id = _key_identifier(current["key_id"], field + ".current.key_id")
    current_psk = _absolute_path(current["psk_file"], field + ".current.psk_file")
    _secure_psk_file(current_psk, system, field + ".current.psk_file")

    if "next" not in authentication:
        return
    next_key = _object(authentication["next"], field + ".next")
    _strict_fields(
        next_key, field + ".next", {"key_id", "psk_file", "valid_until_unix"}
    )
    next_key_id = _key_identifier(next_key["key_id"], field + ".next.key_id")
    if next_key_id == current_key_id:
        raise ConfigError(field + ".next.key_id must differ from the current key")
    next_psk = _absolute_path(next_key["psk_file"], field + ".next.psk_file")
    _secure_psk_file(next_psk, system, field + ".next.psk_file")
    _integer(
        next_key["valid_until_unix"], field + ".next.valid_until_unix", 1, (1 << 63) - 1
    )


def load_provider(path: str, system: Any) -> ProviderConfig:
    document = load_json(path)
    _strict_fields(
        document,
        "provider",
        {
            "schema_version",
            "identity",
            "listen",
            "rdma_rail",
            "pools",
            "consumers",
        },
    )
    schema_version = document.get("schema_version")
    if schema_version not in (1, 2):
        raise ConfigError("schema_version must be 1 or 2")

    identity = _object(document["identity"], "identity")
    _strict_fields(identity, "identity", {"provider_id"})
    provider_id = _identifier(identity["provider_id"], "identity.provider_id")

    listen = _object(document["listen"], "listen")
    _strict_fields(listen, "listen", {"address", "port"})
    address = listen["address"]
    if not isinstance(address, str) or not address or len(address) > 253:
        raise ConfigError("listen.address must be a non-empty network address")
    port = _integer(listen["port"], "listen.port", 1, 65535)

    rail = _object(document["rdma_rail"], "rdma_rail")
    _strict_fields(rail, "rdma_rail", {"device", "port", "numa_node"})
    rail_device = _identifier(rail["device"], "rdma_rail.device")
    rail_port = _integer(rail["port"], "rdma_rail.port", 1, 255)
    numa_node = _integer(
        rail["numa_node"],
        "rdma_rail.numa_node",
        -1 if schema_version >= 2 else 0,
        65535,
    )
    try:
        actual_numa_node = system.rdma_rail_numa_node(rail_device, rail_port)
    except (KeyError, OSError) as exc:
        raise ConfigError("rdma_rail does not identify an available RDMA Rail") from exc
    if actual_numa_node != numa_node:
        raise ConfigError(
            "rdma_rail.numa_node does not match %s port %d" % (rail_device, rail_port)
        )

    pools = _object(document["pools"], "pools")
    _strict_fields(
        pools,
        "pools",
        {
            "host_reserve_gib",
            "max_opportunistic_gib",
            "max_committed_gib",
        },
    )
    host_reserve_gib = _integer(
        pools["host_reserve_gib"], "pools.host_reserve_gib", 1, (1 << 32) - 1
    )
    max_opportunistic_gib = _integer(
        pools["max_opportunistic_gib"], "pools.max_opportunistic_gib", 0, 128
    )
    max_committed_gib = _integer(
        pools["max_committed_gib"], "pools.max_committed_gib", 0, 128
    )
    if max_opportunistic_gib + max_committed_gib == 0:
        raise ConfigError("at least one Provider pool maximum must be non-zero")
    if max_opportunistic_gib + max_committed_gib > 128:
        raise ConfigError("pool maxima may total at most 128 GiB")

    consumers = _object(document["consumers"], "consumers")
    if not consumers or len(consumers) > 64:
        raise ConfigError("consumers must contain 1-64 allowlisted identities")
    for consumer_id in sorted(consumers):
        _identifier(consumer_id, "consumers key")
        field = "consumers." + consumer_id
        consumer = _object(consumers[consumer_id], field)
        _strict_fields(consumer, field, {"quota", "authentication"})
        quota = _object(consumer["quota"], field + ".quota")
        _strict_fields(
            quota,
            field + ".quota",
            {
                "max_connections",
                "max_opportunistic_gib",
                "max_committed_gib",
            },
        )
        _integer(quota["max_connections"], field + ".quota.max_connections", 1, 64)
        opportunistic_quota = _integer(
            quota["max_opportunistic_gib"],
            field + ".quota.max_opportunistic_gib",
            0,
            128,
        )
        committed_quota = _integer(
            quota["max_committed_gib"], field + ".quota.max_committed_gib", 0, 128
        )
        if opportunistic_quota > max_opportunistic_gib:
            raise ConfigError(
                field + ".quota.max_opportunistic_gib exceeds the Opportunistic Pool"
            )
        if committed_quota > max_committed_gib:
            raise ConfigError(
                field + ".quota.max_committed_gib exceeds the Committed Pool"
            )
        if opportunistic_quota + committed_quota == 0:
            raise ConfigError(field + ".quota must allow at least one Remote Chunk")
        _provider_authentication(
            consumer["authentication"], field + ".authentication", system
        )

    return ProviderConfig(
        path=path,
        provider_id=provider_id,
        address=address,
        port=port,
        rail_device=rail_device,
        rail_port=rail_port,
        numa_node=numa_node,
        host_reserve_gib=host_reserve_gib,
        max_opportunistic_gib=max_opportunistic_gib,
        max_committed_gib=max_committed_gib,
        consumer_count=len(consumers),
    )


def validate_device_name(value: Any) -> str:
    if not isinstance(value, str) or not DEVICE_NAME.fullmatch(value):
        raise ConfigError("device name must be a 1-31 character device identifier")
    return value


def load_consumer(path: str, system: Any) -> ConsumerConfig:
    document = load_json(path)
    _strict_fields(document, "consumer", {"schema_version", "identity", "device"})
    schema_version = document.get("schema_version")
    if schema_version not in (1, 2):
        raise ConfigError("schema_version must be 1 or 2")

    identity = _object(document["identity"], "identity")
    _strict_fields(identity, "identity", {"consumer_id"})
    consumer_id = _identifier(identity["consumer_id"], "identity.consumer_id")

    device = _object(document["device"], "device")
    required = {
        "name",
        "mode",
        "capacity_bytes",
        "provider_failure_deadline_ms",
        "provider_directory",
        "providers",
        "swap_priority",
    }
    if schema_version >= 2:
        required.add("hot_range")
    _strict_fields(
        device, "device", required, {"acknowledgement_policy", "backing_store"}
    )
    name = validate_device_name(device["name"])
    mode = device["mode"]
    if mode != "backed":
        if mode == "remote-only":
            raise ConfigError(
                "device.mode remote-only is not available in this milestone"
            )
        raise ConfigError("device.mode must be backed")
    if "backing_store" not in device:
        raise ConfigError("device.backing_store is required")
    policy = device.get("acknowledgement_policy", "strict")
    if policy not in ("strict", "remote-first"):
        raise ConfigError(
            "device.acknowledgement_policy must be strict or remote-first"
        )

    backing_store = _absolute_path(device["backing_store"], "device.backing_store")
    resolved_backing_store = system.resolve_path(backing_store)
    if not resolved_backing_store.startswith("/dev/"):
        raise ConfigError("device.backing_store must resolve below /dev")
    if (
        resolved_backing_store.startswith("/dev/infiniswap")
        or not system.is_block_device(resolved_backing_store)
        or system.is_loop_device(resolved_backing_store)
    ):
        raise ConfigError("device.backing_store must be a non-loop block device")

    capacity_bytes = _integer(
        device["capacity_bytes"], "device.capacity_bytes", GIB, 128 * GIB
    )
    if capacity_bytes % GIB:
        raise ConfigError("device.capacity_bytes must be aligned to 1 GiB")
    deadline = _integer(
        device["provider_failure_deadline_ms"],
        "device.provider_failure_deadline_ms",
        FAILURE_DEADLINE_MIN_MS,
        FAILURE_DEADLINE_MAX_MS,
    )
    if schema_version >= 2:
        hot_range = _object(device["hot_range"], "device.hot_range")
        _strict_fields(
            hot_range,
            "device.hot_range",
            {"mapping_threshold", "read_weight", "write_weight"},
        )
        hot_range_threshold = _integer(
            hot_range["mapping_threshold"],
            "device.hot_range.mapping_threshold",
            1,
            HOT_RANGE_SCORE_MAX,
        )
        hot_range_read_weight = _integer(
            hot_range["read_weight"],
            "device.hot_range.read_weight",
            1,
            HOT_RANGE_WEIGHT_MAX,
        )
        hot_range_write_weight = _integer(
            hot_range["write_weight"],
            "device.hot_range.write_weight",
            1,
            HOT_RANGE_WEIGHT_MAX,
        )
    else:
        hot_range_threshold = HOT_RANGE_THRESHOLD_DEFAULT
        hot_range_read_weight = HOT_RANGE_READ_WEIGHT_DEFAULT
        hot_range_write_weight = HOT_RANGE_WRITE_WEIGHT_DEFAULT
    directory_path = _absolute_path(
        device["provider_directory"], "device.provider_directory"
    )
    directory = load_provider_directory(directory_path, system)

    selected_value = device["providers"]
    if (
        not isinstance(selected_value, list)
        or not selected_value
        or any(not isinstance(item, str) for item in selected_value)
    ):
        raise ConfigError("device.providers must be a non-empty list")
    if len(set(selected_value)) != len(selected_value):
        raise ConfigError("device.providers must be unique")
    if len(selected_value) != 1:
        raise ConfigError("this milestone requires exactly one Provider")
    selected: List[ProviderDirectoryEntry] = []
    required_capabilities = {
        "backed",
        "opportunistic_pool",
        "provider_failure_deadline",
        "status",
        "auth_hmac_sha256",
    }
    for provider_name in selected_value:
        if provider_name not in directory:
            raise ConfigError(
                "device.providers contains untrusted Provider " + provider_name
            )
        provider = directory[provider_name]
        if provider.consumer_id != consumer_id:
            raise ConfigError(
                "Provider %s does not trust identity %s" % (provider_name, consumer_id)
            )
        try:
            actual_numa_node = system.rdma_rail_numa_node(
                provider.rail_device, provider.rail_port
            )
        except (KeyError, OSError) as exc:
            raise ConfigError(
                "Provider %s names an unavailable RDMA Rail" % provider_name
            ) from exc
        if actual_numa_node != provider.numa_node:
            raise ConfigError(
                "Provider %s RDMA Rail NUMA identity does not match" % provider_name
            )
        missing_capabilities = sorted(
            required_capabilities - set(provider.capabilities)
        )
        if missing_capabilities:
            raise ConfigError(
                "Provider %s lacks required capability %s"
                % (provider_name, missing_capabilities[0])
            )
        try:
            resolved_address = system.resolve_network_address(
                provider.address, provider.port
            )
        except (KeyError, OSError) as exc:
            raise ConfigError(
                "Provider %s address must resolve to exactly one network address"
                % provider_name
            ) from exc
        psk = _read_psk(
            provider.psk_file,
            system,
            "Provider %s authentication.psk_file" % provider_name,
        )
        selected.append(replace(provider, address=resolved_address, psk=psk))

    serialized_providers = ",".join(provider.name for provider in selected)
    if len(serialized_providers) + 1 >= CONFIGFS_VALUE_SIZE:
        raise ConfigError("Provider selection is too large for configfs")

    swap_priority = _integer(device["swap_priority"], "device.swap_priority", 0, 32767)
    return ConsumerConfig(
        path=path,
        consumer_id=consumer_id,
        name=name,
        mode=mode,
        acknowledgement_policy=policy,
        backing_store=resolved_backing_store,
        capacity_bytes=capacity_bytes,
        provider_failure_deadline_ms=deadline,
        hot_range_threshold=hot_range_threshold,
        hot_range_read_weight=hot_range_read_weight,
        hot_range_write_weight=hot_range_write_weight,
        provider_directory=directory_path,
        providers=tuple(selected),
        swap_priority=swap_priority,
    )
