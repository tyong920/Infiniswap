"""Command-line application boundary for local Infiniswap administration."""

import argparse
import errno
import json
import sys
from typing import IO, Any, Dict, List, Optional, Tuple

from .config import (
    ConfigError,
    ConsumerConfig,
    GIB,
    load_consumer,
    load_provider,
    load_provider_directory,
    validate_device_name,
)
from .observability import evaluate_consumer_alerts, render_consumer_metrics
from .system import LocalSystem


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="infiniswapctl")
    commands = parser.add_subparsers(dest="command", required=True)

    create = commands.add_parser("create")
    create.add_argument("--config", required=True)
    create.add_argument("--dry-run", action="store_true")

    format_command = commands.add_parser("format")
    format_command.add_argument("device")
    format_command.add_argument("--yes", action="store_true")
    format_command.add_argument("--dry-run", action="store_true")

    enable = commands.add_parser("enable")
    enable.add_argument("device")
    enable.add_argument("--priority", required=True, type=int)
    enable.add_argument("--dry-run", action="store_true")

    for name in ("disable", "drain", "destroy"):
        command = commands.add_parser(name)
        command.add_argument("device")
        command.add_argument("--dry-run", action="store_true")

    status = commands.add_parser("status")
    status.add_argument("device")
    status.add_argument("--json", action="store_true")

    metrics = commands.add_parser("metrics")
    metrics.add_argument("device")

    alerts = commands.add_parser("alerts")
    alerts.add_argument("device")
    alerts.add_argument("--json", action="store_true")

    audit = commands.add_parser("audit")
    audit.add_argument("event", choices=("key-rotated", "key-revoked", "cutover"))
    audit.add_argument("--subject", required=True)
    audit.add_argument("--change-id", required=True)
    audit.add_argument(
        "--outcome",
        required=True,
        choices=("started", "completed", "rolled-back", "failed"),
    )
    audit.add_argument("--dry-run", action="store_true")

    provider_status = commands.add_parser("provider-status")
    provider_status.add_argument(
        "--endpoint", default="http://127.0.0.1:9401"
    )
    provider_status.add_argument("--json", action="store_true")

    provider_metrics = commands.add_parser("provider-metrics")
    provider_metrics.add_argument(
        "--endpoint", default="http://127.0.0.1:9401"
    )

    validate = commands.add_parser("validate")
    validate.add_argument(
        "kind", choices=("consumer", "provider", "provider-directory")
    )
    validate.add_argument("--config", required=True)
    return parser


def _validate_audit_identifier(value: str, field: str) -> str:
    if not value or len(value) > 63 or any(
        not (character.isalnum() or character in "._-") for character in value
    ):
        raise ConfigError(
            "%s must be 1-63 letters, digits, periods, underscores, or hyphens"
            % field
        )
    return value


def _print_create_preflight(config: ConsumerConfig, stdout: IO[str]) -> None:
    providers = ", ".join(entry.name for entry in config.providers)
    print("Preflight create " + config.name, file=stdout)
    print("  mode: " + config.mode, file=stdout)
    if config.mode == "remote-only":
        print("  acknowledgement policy: not applicable", file=stdout)
        print("  Backing Store: none", file=stdout)
        print("  Remote-Only-Eligible Host: declared", file=stdout)
    else:
        print(
            "  acknowledgement policy: " + config.acknowledgement_policy,
            file=stdout,
        )
        print("  Backing Store: " + config.backing_store, file=stdout)
    print("  capacity bytes: " + str(config.capacity_bytes), file=stdout)
    print("  Providers: " + providers, file=stdout)
    print(
        "  Power-of-d sample size: %d" % config.placement_sample_size,
        file=stdout,
    )
    for provider in config.providers:
        print(
            "  Provider %s endpoint: %s:%d"
            % (provider.name, provider.address, provider.port),
            file=stdout,
        )
        print(
            "  Provider %s RDMA Rail: %s port %d, NUMA node %d, weight %d"
            % (
                provider.name,
                provider.rail_device,
                provider.rail_port,
                provider.numa_node,
                provider.placement_weight,
            ),
            file=stdout,
        )
    print("  explicit swap priority: " + str(config.swap_priority), file=stdout)
    print(
        "  Provider Failure Deadline: %d ms" % config.provider_failure_deadline_ms,
        file=stdout,
    )
    print(
        "  Hot Range scoring: threshold %d, read weight %d, write weight %d"
        % (
            config.hot_range_threshold,
            config.hot_range_read_weight,
            config.hot_range_write_weight,
        ),
        file=stdout,
    )


def _create(config: ConsumerConfig, system: Any, stdout: IO[str]) -> None:
    system.ensure_configfs()
    system.ensure_module()
    if system.group_exists(config.name):
        raise ConfigError("Infiniswap Device already exists: " + config.name)

    system.create_group(config.name)
    try:
        attributes = [("mode", config.mode)]
        if config.mode == "remote-only":
            attributes.append(("remote_only_eligible", "1"))
        else:
            attributes.extend(
                (
                    ("acknowledgement_policy", config.acknowledgement_policy),
                    ("backing_store", config.backing_store),
                )
            )
        attributes.extend(
            (
                ("capacity_bytes", str(config.capacity_bytes)),
                (
                    "provider_failure_deadline_ms",
                    str(config.provider_failure_deadline_ms),
                ),
            )
        )
        if config.mode == "backed":
            attributes.extend(
                (
                    ("hot_range_threshold", str(config.hot_range_threshold)),
                    ("hot_range_read_weight", str(config.hot_range_read_weight)),
                    ("hot_range_write_weight", str(config.hot_range_write_weight)),
                )
            )
        attributes.extend(
            (
                ("consumer_id", config.consumer_id),
                (
                    "providers",
                    ",".join(provider.name for provider in config.providers),
                ),
                ("placement_sample_size", str(config.placement_sample_size)),
            )
        )
        for provider in config.providers:
            attributes.extend(
                (
                    ("provider_bind", provider.name),
                    ("provider_address", provider.address),
                    ("provider_port", str(provider.port)),
                    ("rdma_device", provider.rail_device),
                    ("rdma_port", str(provider.rail_port)),
                    ("rdma_numa_node", str(provider.numa_node)),
                    ("provider_key_id", provider.key_id),
                    ("provider_psk", provider.psk.hex()),
                    ("placement_weight", str(provider.placement_weight)),
                )
            )
        attributes.extend(
            (
                ("swap_priority", str(config.swap_priority)),
                ("state", "activate"),
            )
        )
        for attribute, value in attributes:
            system.write_attribute(config.name, attribute, value)
    except OSError as original_error:
        try:
            system.remove_group(config.name)
        except OSError as rollback_error:
            raise OSError(
                "%s; rollback failed: %s" % (original_error, rollback_error)
            ) from original_error
        raise
    print("Created active Infiniswap Device " + config.name, file=stdout)


def _inspect_device(name: str, system: Any) -> Tuple[str, str]:
    name = validate_device_name(name)
    if not system.group_exists(name):
        raise ConfigError("Infiniswap Device does not exist: " + name)
    return system.device_path(name), system.read_attribute(name, "state")


def _validate_lifecycle_command(
    args: argparse.Namespace, system: Any
) -> Tuple[str, str]:
    path, state = _inspect_device(args.device, system)
    command = args.command

    if command in ("format", "enable", "disable"):
        if state != "active":
            raise ConfigError("%s requires an active Infiniswap Device" % command)
        if not system.is_block_device(path):
            raise ConfigError("Infiniswap block device is unavailable: " + path)

    if command == "format":
        if system.is_swap_enabled(path):
            raise ConfigError("disable %s before formatting" % args.device)
        if system.is_mounted(path):
            raise ConfigError("refusing to format a mounted Infiniswap Device")
        if not args.dry_run and not args.yes:
            raise ConfigError("format requires --yes after reviewing preflight")
    elif command == "enable":
        if args.priority < 0 or args.priority > 32767:
            raise ConfigError("swap priority must be between 0 and 32767")
        configured_priority = int(system.read_attribute(args.device, "swap_priority"))
        if args.priority != configured_priority:
            raise ConfigError(
                "enable must use configured swap priority %d" % configured_priority
            )
        if system.is_swap_enabled(path):
            raise ConfigError("Infiniswap Device is already enabled as swap")
        if not system.has_swap_signature(path):
            raise ConfigError("format %s before enabling swap" % args.device)
    elif command == "disable":
        if not system.is_swap_enabled(path):
            raise ConfigError("Infiniswap Device is not enabled as swap")
    elif command == "drain":
        if state != "active":
            raise ConfigError("drain requires an active Infiniswap Device")
        if system.is_swap_enabled(path):
            raise ConfigError("disable %s before draining" % args.device)
    elif command == "destroy":
        if system.is_swap_enabled(path):
            raise ConfigError("disable %s before destroying" % args.device)
        if state not in ("created", "drained", "stopped"):
            raise ConfigError("drain %s before destroying" % args.device)
    return path, state


def _print_lifecycle_preflight(
    args: argparse.Namespace, path: str, state: str, stdout: IO[str]
) -> None:
    print("Preflight %s %s" % (args.command, args.device), file=stdout)
    print("  device path: " + path, file=stdout)
    print("  lifecycle: " + state, file=stdout)
    if args.command == "enable":
        print("  explicit swap priority: " + str(args.priority), file=stdout)
    if args.command == "format":
        print("  destructive action: write a swap signature", file=stdout)


def _run_lifecycle_command(
    args: argparse.Namespace, path: str, system: Any, stdout: IO[str]
) -> None:
    if args.command == "format":
        system.run_host_command(["mkswap", path])
    elif args.command == "enable":
        system.run_host_command(["swapon", "--priority", str(args.priority), path])
    elif args.command == "disable":
        system.run_host_command(["swapoff", path])
    elif args.command == "drain":
        system.write_attribute(args.device, "state", "drain")
    elif args.command == "destroy":
        system.write_attribute(args.device, "state", "stop")
        system.remove_group(args.device)
    print("Completed %s for %s" % (args.command, args.device), file=stdout)


def _parse_chunk_placements(raw: str) -> List[Dict[str, Any]]:
    placements: List[Dict[str, Any]] = []
    for token in raw.split():
        if ":" not in token:
            continue
        logical, provider_id = token.split(":", 1)
        if not logical.isdigit() or not provider_id:
            continue
        placements.append(
            {"logical_chunk": int(logical), "provider_id": provider_id}
        )
    return placements


def _parse_excluded_providers(raw: str) -> List[Dict[str, str]]:
    excluded: List[Dict[str, str]] = []
    for token in raw.split():
        if ":" not in token:
            continue
        provider_id, reason = token.split(":", 1)
        if not provider_id or not reason:
            continue
        excluded.append({"provider_id": provider_id, "reason": reason})
    return excluded


def _error_status(error_number: int) -> Optional[Dict[str, Any]]:
    if not error_number:
        return None
    positive_error = abs(error_number)
    return {
        "code": errno.errorcode.get(positive_error, "EUNKNOWN"),
        "errno": positive_error,
    }


def _optional_int_attribute(
    system: Any, name: str, attribute: str, default: int = 0
) -> int:
    try:
        return int(system.read_attribute(name, attribute))
    except (KeyError, OSError, ValueError):
        return default


def _parse_provider_runtime_status(raw: str) -> Dict[str, Dict[str, Any]]:
    providers: Dict[str, Dict[str, Any]] = {}
    valid_states = {
        "not-connected",
        "connecting",
        "connected",
        "degraded",
        "remote-lost",
    }
    for line in raw.splitlines():
        fields = line.split()
        if len(fields) != 5 or fields[1] not in valid_states:
            continue
        try:
            available_chunks = int(fields[2])
            mapped_chunks = int(fields[3])
            error_number = int(fields[4])
        except ValueError:
            continue
        if available_chunks < 0 or mapped_chunks < 0 or not fields[0]:
            continue
        providers[fields[0]] = {
            "provider_id": fields[0],
            "state": fields[1],
            "available_chunks": available_chunks,
            "mapped_chunks": mapped_chunks,
            "last_error": _error_status(error_number),
        }
    return providers


def _device_status(name: str, system: Any) -> Dict[str, Any]:
    path, lifecycle = _inspect_device(name, system)
    mode = system.read_attribute(name, "mode")
    policy = system.read_attribute(name, "acknowledgement_policy")
    capacity_bytes = int(system.read_attribute(name, "capacity_bytes"))
    deadline_ms = int(system.read_attribute(name, "provider_failure_deadline_ms"))
    mapping_threshold = int(system.read_attribute(name, "hot_range_threshold"))
    read_weight = int(system.read_attribute(name, "hot_range_read_weight"))
    write_weight = int(system.read_attribute(name, "hot_range_write_weight"))
    mapped_remote_chunks = int(system.read_attribute(name, "mapped_remote_chunks"))
    mapped_hot_ranges = int(system.read_attribute(name, "mapped_hot_ranges"))
    connection_state = system.read_attribute(name, "connection_state")
    operational_state = system.read_attribute(name, "operational_state")
    remote_capacity_bytes = int(system.read_attribute(name, "remote_capacity_bytes"))
    provider_names = list(
        filter(None, system.read_attribute(name, "providers").split(","))
    )
    try:
        placement_sample_size = int(
            system.read_attribute(name, "placement_sample_size")
        )
    except (KeyError, OSError, ValueError):
        placement_sample_size = min(2, max(1, len(provider_names) or 1))
    try:
        placements = _parse_chunk_placements(
            system.read_attribute(name, "remote_chunk_placements")
        )
    except (KeyError, OSError):
        placements = []
    try:
        excluded_providers = _parse_excluded_providers(
            system.read_attribute(name, "provider_exclusions")
        )
    except (KeyError, OSError):
        excluded_providers = []
    excluded_ids = {entry["provider_id"] for entry in excluded_providers}
    swap_enabled = system.is_swap_enabled(path)
    priority = system.swap_priority(path) if swap_enabled else None
    configured_priority = int(system.read_attribute(name, "swap_priority"))
    last_error = _error_status(int(system.read_attribute(name, "last_error")))

    metric_names = (
        "admission_rejections_total",
        "authentication_failures_total",
        "backing_degraded_transitions_total",
        "backing_failures_total",
        "backing_invalid_sectors",
        "backing_retries_total",
        "late_rdma_completions_total",
        "local_only_writes_total",
        "provider_timeouts_total",
        "remote_lost_transitions_total",
        "rejected_writes_total",
    )
    metrics = {
        metric: _optional_int_attribute(system, name, metric) for metric in metric_names
    }
    io_status = {
        "in_flight": _optional_int_attribute(
            system,
            name,
            "inflight_io",
            _optional_int_attribute(system, name, "inflight", 0),
        ),
        "oldest_in_flight_ms": _optional_int_attribute(
            system, name, "oldest_inflight_ms"
        ),
        "requests_total": _optional_int_attribute(
            system, name, "io_requests_total"
        ),
        "completed_total": _optional_int_attribute(
            system, name, "io_completed_total"
        ),
        "errors_total": _optional_int_attribute(system, name, "io_errors_total"),
    }

    try:
        runtime_status = _parse_provider_runtime_status(
            system.read_attribute(name, "provider_runtime_status")
        )
    except (KeyError, OSError):
        runtime_status = {}
    mapped_by_provider: Dict[str, int] = {}
    for placement in placements:
        provider_id = placement["provider_id"]
        mapped_by_provider[provider_id] = mapped_by_provider.get(provider_id, 0) + 1

    provider_states = []
    for provider_index, provider in enumerate(provider_names):
        runtime = runtime_status.get(provider)
        if runtime is None:
            runtime = runtime_status.get(str(provider_index))
        if runtime is not None:
            runtime = dict(runtime)
            runtime["provider_id"] = provider
            provider_states.append(runtime)
            continue
        if provider in excluded_ids:
            state = "not-connected"
        else:
            state = connection_state
        provider_states.append(
            {
                "provider_id": provider,
                "state": state,
                "available_chunks": 0,
                "mapped_chunks": mapped_by_provider.get(provider, 0),
                "last_error": None,
            }
        )
    healthy_providers = sum(
        1 for provider in provider_states if provider["state"] == "connected"
    )

    return {
        "schema_version": 6,
        "kind": "infiniswap.device-status",
        "device": {
            "name": name,
            "path": path,
            "lifecycle": lifecycle,
            "mode": mode,
            "acknowledgement_policy": policy,
            "operational_state": operational_state,
            "provider_failure_deadline_ms": deadline_ms,
            "swap": {
                "configured_priority": configured_priority,
                "enabled": swap_enabled,
                "priority": priority,
            },
        },
        "connection": {
            "state": connection_state,
            "total_providers": len(provider_names),
            "healthy_providers": healthy_providers,
            "providers": provider_states,
        },
        "capacity": {
            "advertised_bytes": capacity_bytes,
            "backing_bytes": capacity_bytes if mode == "backed" else 0,
            "remote_bytes": remote_capacity_bytes,
        },
        "mapping": {
            "chunk_size_bytes": GIB,
            "threshold": mapping_threshold,
            "read_weight": read_weight,
            "write_weight": write_weight,
            "mapped_remote_chunks": mapped_remote_chunks,
            "mapped_hot_ranges": mapped_hot_ranges,
            "placement_sample_size": placement_sample_size,
            "placements": placements,
            "excluded_providers": excluded_providers,
        },
        "io": io_status,
        "metrics": metrics,
        "last_error": last_error,
    }


def _print_status(status: Dict[str, Any], json_output: bool, stdout: IO[str]) -> None:
    if json_output:
        json.dump(status, stdout, indent=2, sort_keys=True)
        stdout.write("\n")
        return
    device = status["device"]
    connection = status["connection"]
    capacity = status["capacity"]
    print(
        "%s: %s (%s, %s)"
        % (
            device["name"],
            device["lifecycle"],
            device["mode"],
            device["acknowledgement_policy"],
        ),
        file=stdout,
    )
    print(
        "  capacity: %d bytes advertised, %d bytes remote"
        % (capacity["advertised_bytes"], capacity["remote_bytes"]),
        file=stdout,
    )
    print(
        "  Provider Failure Deadline: %d ms"
        % device["provider_failure_deadline_ms"],
        file=stdout,
    )
    io_status = status["io"]
    print(
        "  I/O: %d in flight, %d completed, %d errors"
        % (
            io_status["in_flight"],
            io_status["completed_total"],
            io_status["errors_total"],
        ),
        file=stdout,
    )
    if io_status["in_flight"]:
        print(
            "  oldest in-flight I/O: %d ms"
            % io_status["oldest_in_flight_ms"],
            file=stdout,
        )
    if device["mode"] == "remote-only":
        print(
            "  Remote Chunks: %d mapped" % status["mapping"]["mapped_remote_chunks"],
            file=stdout,
        )
    else:
        print(
            "  Hot Ranges: %d mapped (threshold %d, read/write weights %d/%d)"
            % (
                status["mapping"]["mapped_hot_ranges"],
                status["mapping"]["threshold"],
                status["mapping"]["read_weight"],
                status["mapping"]["write_weight"],
            ),
            file=stdout,
        )
    if device["swap"]["enabled"]:
        print(
            "  swap: enabled at priority %d" % device["swap"]["priority"], file=stdout
        )
    else:
        print("  swap: disabled", file=stdout)
    if device["mode"] == "remote-only":
        remote_suffix = (
            " (all I/O rejected)"
            if device["operational_state"] == "remote-lost"
            else ""
        )
        print(
            "  remote: %s%s" % (device["operational_state"], remote_suffix),
            file=stdout,
        )
    else:
        backing_suffix = (
            " (new writes rejected)"
            if device["operational_state"] == "backing-degraded"
            else ""
        )
        print(
            "  backing: %s%s" % (device["operational_state"], backing_suffix),
            file=stdout,
        )
    print(
        "  backing failures/retries: %d/%d"
        % (
            status["metrics"]["backing_failures_total"],
            status["metrics"]["backing_retries_total"],
        ),
        file=stdout,
    )
    print(
        "  connection: %s (%d/%d Providers healthy)"
        % (
            connection["state"],
            connection["healthy_providers"],
            connection["total_providers"],
        ),
        file=stdout,
    )
    for provider in connection["providers"]:
        provider_error = provider["last_error"]
        error_suffix = (
            ", last error %(code)s (%(errno)d)" % provider_error
            if provider_error
            else ""
        )
        print(
            "  Provider %s: %s, %d chunks available, %d mapped%s"
            % (
                provider["provider_id"],
                provider["state"],
                provider["available_chunks"],
                provider["mapped_chunks"],
                error_suffix,
            ),
            file=stdout,
        )
    mapping = status["mapping"]
    if mapping["placements"]:
        print(
            "  placements: "
            + ", ".join(
                "%d→%s" % (entry["logical_chunk"], entry["provider_id"])
                for entry in mapping["placements"]
            ),
            file=stdout,
        )
    if mapping["excluded_providers"]:
        print(
            "  excluded Providers: "
            + ", ".join(
                "%s (%s)" % (entry["provider_id"], entry["reason"])
                for entry in mapping["excluded_providers"]
            ),
            file=stdout,
        )
    if status["last_error"] is None:
        print("  last error: none", file=stdout)
    else:
        print("  last error: %(code)s (%(errno)d)" % status["last_error"], file=stdout)


def _contains_secret_field(value: Any) -> bool:
    if isinstance(value, dict):
        for key, nested in value.items():
            normalized = key.lower().replace("-", "_")
            if "psk" in normalized or normalized in {
                "key_id",
                "authentication_tag",
                "secret",
            }:
                return True
            if _contains_secret_field(nested):
                return True
    elif isinstance(value, list):
        return any(_contains_secret_field(item) for item in value)
    return False


def _provider_status(endpoint: str, system: Any) -> Dict[str, Any]:
    try:
        status = json.loads(system.fetch_observability(endpoint, "/status"))
        if (
            not isinstance(status, dict)
            or status.get("schema_version") != 1
            or status.get("kind") != "infiniswap.provider-status"
            or not isinstance(status["provider"], dict)
            or not isinstance(status["pools"], list)
            or len(status["pools"]) != 2
            or not isinstance(status["consumers"], list)
            or len(status["consumers"]) > 64
            or _contains_secret_field(status)
        ):
            raise ValueError("invalid Provider status contract")
    except (KeyError, TypeError, ValueError, json.JSONDecodeError) as exc:
        raise ConfigError("Provider returned invalid status JSON") from exc
    return status


def _print_provider_status(status: Dict[str, Any], stdout: IO[str]) -> None:
    provider = status["provider"]
    connection_count = provider["active_connections"]
    print(
        "%s: %s (%d active Consumer connection%s)"
        % (
            provider["provider_id"],
            provider["state"],
            connection_count,
            "" if connection_count == 1 else "s",
        ),
        file=stdout,
    )
    for pool in status["pools"]:
        print(
            "  %s Pool: %d assigned, %d available, %d maximum Remote Chunks"
            % (
                pool["name"].capitalize(),
                pool["assigned_chunks"],
                pool["available_chunks"],
                pool["max_chunks"],
            ),
            file=stdout,
        )
    chunks = status["chunks"]
    print(
        "  chunks: %d allocated, %d assigned, %d quarantined; Host Reserve %d"
        % (
            chunks["allocated_chunks"],
            chunks["assigned_chunks"],
            chunks["quarantined_chunks"],
            chunks["host_reserve_chunks"],
        ),
        file=stdout,
    )
    for consumer in status["consumers"]:
        consumer_error = (
            ", last control error code %d" % consumer["last_error_code"]
            if consumer["last_error_code"]
            else ""
        )
        print(
            "  Consumer %s: %s, %s, %s, deadline %d ms%s"
            % (
                consumer["consumer_id"],
                consumer["state"],
                consumer["mode"],
                consumer["pool"],
                consumer["failure_deadline_ms"],
                consumer_error,
            ),
            file=stdout,
        )
        print(
            "    Remote Chunks: %d opportunistic, %d committed; "
            "in-flight control requests: %d"
            % (
                consumer["assigned_opportunistic_chunks"],
                consumer["assigned_committed_chunks"],
                consumer["inflight_control_requests"],
            ),
            file=stdout,
        )
    if status["last_error"] is None:
        print("  last error: none", file=stdout)
    else:
        print("  last error: " + status["last_error"]["code"], file=stdout)


def run(
    argv: Optional[List[str]] = None,
    *,
    system: Any = None,
    stdout: IO[str] = sys.stdout,
    stderr: IO[str] = sys.stderr,
) -> int:
    args = _parser().parse_args(argv)
    system = system or LocalSystem()
    try:
        if args.command == "create":
            config = load_consumer(args.config, system)
            if not args.dry_run and not system.is_root():
                raise ConfigError("Consumer mutations must run as root")
            _print_create_preflight(config, stdout)
            if args.dry_run:
                print("Dry run: no changes made", file=stdout)
                return 0
            _create(config, system, stdout)
            system.record_audit_event(
                "device.created",
                config.name,
                {
                    "acknowledgement_policy": config.acknowledgement_policy,
                    "mode": config.mode,
                    "provider_count": len(config.providers),
                    "remote_only_eligible": config.remote_only_eligible,
                },
            )
            return 0

        if args.command in ("format", "enable", "disable", "drain", "destroy"):
            path, state = _validate_lifecycle_command(args, system)
            if not args.dry_run and not system.is_root():
                raise ConfigError("Consumer mutations must run as root")
            _print_lifecycle_preflight(args, path, state, stdout)
            if args.dry_run:
                print("Dry run: no changes made", file=stdout)
                return 0
            _run_lifecycle_command(args, path, system, stdout)
            if args.command == "enable":
                event = "swap.activated"
                details = {"priority": args.priority, "scope": "host-wide"}
            elif args.command == "disable":
                event = "swap.deactivated"
                details = {"scope": "host-wide"}
            else:
                event = "device.%s" % (
                    "formatted" if args.command == "format" else args.command
                )
                details = {}
            system.record_audit_event(event, args.device, details)
            return 0

        if args.command == "status":
            status = _device_status(validate_device_name(args.device), system)
            _print_status(status, args.json, stdout)
            return 0

        if args.command == "metrics":
            status = _device_status(validate_device_name(args.device), system)
            stdout.write(render_consumer_metrics(status))
            return 0

        if args.command == "alerts":
            status = _device_status(validate_device_name(args.device), system)
            alerts = evaluate_consumer_alerts(status)
            if args.json:
                json.dump(
                    {
                        "schema_version": 1,
                        "kind": "infiniswap.alert-state",
                        "device": status["device"]["name"],
                        "alerts": alerts,
                    },
                    stdout,
                    indent=2,
                    sort_keys=True,
                )
                stdout.write("\n")
            elif not alerts:
                print("No active Infiniswap alerts", file=stdout)
            else:
                for alert in alerts:
                    print(
                        "%s [%s]: %s (%s)"
                        % (
                            alert["name"],
                            alert["severity"],
                            alert["summary"],
                            alert["runbook"],
                        ),
                        file=stdout,
                    )
            return 0

        if args.command == "audit":
            subject = _validate_audit_identifier(args.subject, "subject")
            change_id = _validate_audit_identifier(args.change_id, "change-id")
            if not args.dry_run and not system.is_root():
                raise ConfigError("audit recording must run as root")
            print(
                "Preflight audit %s for %s (%s)"
                % (args.event, subject, args.outcome),
                file=stdout,
            )
            if args.dry_run:
                print("Dry run: no changes made", file=stdout)
                return 0
            event = args.event.replace("-", ".")
            system.record_audit_event(
                event,
                subject,
                {"change_id": change_id, "outcome": args.outcome},
            )
            print("Recorded %s audit event" % event, file=stdout)
            return 0

        if args.command == "provider-status":
            status = _provider_status(args.endpoint, system)
            if args.json:
                json.dump(status, stdout, indent=2, sort_keys=True)
                stdout.write("\n")
            else:
                _print_provider_status(status, stdout)
            return 0

        if args.command == "provider-metrics":
            metrics = system.fetch_observability(args.endpoint, "/metrics")
            if (
                not metrics.startswith("# infiniswap_metrics_schema_version 1\n")
                or not metrics.endswith("# EOF\n")
                or "psk" in metrics.lower()
                or "key_id" in metrics.lower()
            ):
                raise ConfigError("Provider returned invalid OpenMetrics data")
            stdout.write(metrics)
            return 0

        if args.command == "validate":
            if args.kind == "consumer":
                config = load_consumer(args.config, system)
                print(
                    "Consumer configuration is valid: " + config.consumer_id,
                    file=stdout,
                )
            elif args.kind == "provider":
                config = load_provider(args.config, system)
                print(
                    "Provider configuration is valid: " + config.provider_id,
                    file=stdout,
                )
            else:
                providers = load_provider_directory(args.config, system)
                print(
                    "Provider Directory is valid: %d Providers" % len(providers),
                    file=stdout,
                )
            return 0
    except ConfigError as exc:
        print("infiniswapctl: " + str(exc), file=stderr)
        return 2
    except OSError as exc:
        print("infiniswapctl: host operation failed: " + str(exc), file=stderr)
        return 1
    return 2


def main() -> int:
    return run()


if __name__ == "__main__":
    raise SystemExit(main())
