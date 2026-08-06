"""Command-line application boundary for local Infiniswap administration."""

import argparse
import errno
import json
import sys
from typing import IO, Any, Dict, List, Optional, Tuple

from .config import (
    ConfigError,
    ConsumerConfig,
    load_consumer,
    load_provider,
    load_provider_directory,
    validate_device_name,
)
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

    validate = commands.add_parser("validate")
    validate.add_argument(
        "kind", choices=("consumer", "provider", "provider-directory")
    )
    validate.add_argument("--config", required=True)
    return parser


def _print_create_preflight(config: ConsumerConfig, stdout: IO[str]) -> None:
    providers = ", ".join(provider.name for provider in config.providers)
    print("Preflight create " + config.name, file=stdout)
    print("  mode: " + config.mode, file=stdout)
    print("  acknowledgement policy: " + config.acknowledgement_policy, file=stdout)
    print("  capacity bytes: " + str(config.capacity_bytes), file=stdout)
    print("  Backing Store: " + config.backing_store, file=stdout)
    print("  Providers: " + providers, file=stdout)
    print("  explicit swap priority: " + str(config.swap_priority), file=stdout)
    print(
        "  Provider Failure Deadline: %d ms" % config.provider_failure_deadline_ms,
        file=stdout,
    )


def _create(config: ConsumerConfig, system: Any, stdout: IO[str]) -> None:
    system.ensure_configfs()
    system.ensure_module()
    if system.group_exists(config.name):
        raise ConfigError("Infiniswap Device already exists: " + config.name)

    system.create_group(config.name)
    try:
        attributes = (
            ("mode", config.mode),
            ("acknowledgement_policy", config.acknowledgement_policy),
            ("backing_store", config.backing_store),
            ("capacity_bytes", str(config.capacity_bytes)),
            ("provider_failure_deadline_ms", str(config.provider_failure_deadline_ms)),
            ("consumer_id", config.consumer_id),
            ("providers", ",".join(provider.name for provider in config.providers)),
            ("swap_priority", str(config.swap_priority)),
            ("state", "activate"),
        )
        for attribute, value in attributes:
            system.write_attribute(config.name, attribute, value)
    except OSError:
        try:
            system.remove_group(config.name)
        except OSError:
            pass
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


def _device_status(name: str, system: Any) -> Dict[str, Any]:
    path, lifecycle = _inspect_device(name, system)
    mode = system.read_attribute(name, "mode")
    policy = system.read_attribute(name, "acknowledgement_policy")
    capacity_bytes = int(system.read_attribute(name, "capacity_bytes"))
    deadline_ms = int(system.read_attribute(name, "provider_failure_deadline_ms"))
    connection_state = system.read_attribute(name, "connection_state")
    remote_capacity_bytes = int(system.read_attribute(name, "remote_capacity_bytes"))
    provider_names = sorted(
        filter(None, system.read_attribute(name, "providers").split(","))
    )
    healthy_providers = len(provider_names) if connection_state == "connected" else 0
    swap_enabled = system.is_swap_enabled(path)
    priority = system.swap_priority(path) if swap_enabled else None
    configured_priority = int(system.read_attribute(name, "swap_priority"))
    error_number = int(system.read_attribute(name, "last_error"))
    last_error = None
    if error_number:
        positive_error = abs(error_number)
        last_error = {
            "code": errno.errorcode.get(positive_error, "EUNKNOWN"),
            "errno": positive_error,
        }

    return {
        "schema_version": 1,
        "kind": "infiniswap.device-status",
        "device": {
            "name": name,
            "path": path,
            "lifecycle": lifecycle,
            "mode": mode,
            "acknowledgement_policy": policy,
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
            "providers": [
                {"provider_id": provider, "state": connection_state}
                for provider in provider_names
            ],
        },
        "capacity": {
            "advertised_bytes": capacity_bytes,
            "backing_bytes": capacity_bytes if mode == "backed" else 0,
            "remote_bytes": remote_capacity_bytes,
        },
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
    if device["swap"]["enabled"]:
        print(
            "  swap: enabled at priority %d" % device["swap"]["priority"], file=stdout
        )
    else:
        print("  swap: disabled", file=stdout)
    print(
        "  connection: %s (%d/%d Providers healthy)"
        % (
            connection["state"],
            connection["healthy_providers"],
            connection["total_providers"],
        ),
        file=stdout,
    )
    if status["last_error"] is None:
        print("  last error: none", file=stdout)
    else:
        print("  last error: %(code)s (%(errno)d)" % status["last_error"], file=stdout)


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
            return 0

        if args.command == "status":
            status = _device_status(validate_device_name(args.device), system)
            _print_status(status, args.json, stdout)
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
