"""Operating-system adapter used by the administration application."""

import os
import socket
import stat
import subprocess
from pathlib import Path
from typing import Any, List

CONFIGFS_ROOT = Path("/sys/kernel/config/infiniswap")


class LocalSystem:
    """Perform host inspection and mutation through narrow, auditable methods."""

    def is_root(self) -> bool:
        return os.geteuid() == 0

    def resolve_path(self, path: str) -> str:
        return os.path.realpath(path)

    def is_block_device(self, path: str) -> bool:
        try:
            return stat.S_ISBLK(os.stat(path).st_mode)
        except OSError:
            return False

    def is_loop_device(self, path: str) -> bool:
        details = os.stat(path)
        if stat.S_ISBLK(details.st_mode) and os.major(details.st_rdev) == 7:
            return True
        result = subprocess.run(
            ["lsblk", "--inverse", "--noheadings", "--raw", "--output", "TYPE", path],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if result.returncode != 0:
            detail = result.stderr.strip() or "could not inspect block dependencies"
            raise OSError("lsblk %s: %s" % (path, detail))
        return "loop" in result.stdout.split()

    def stat_file(self, path: str) -> Any:
        return os.stat(path, follow_symlinks=False)

    def read_secret(self, path: str) -> bytes:
        return Path(path).read_bytes()

    def rdma_rail_numa_node(self, device: str, port: int) -> int:
        rail = Path("/sys/class/infiniband") / device
        if not (rail / "ports" / str(port)).is_dir():
            raise OSError("RDMA port does not exist")
        return int((rail / "device" / "numa_node").read_text(encoding="ascii").strip())

    def resolve_network_address(self, address: str, port: int) -> str:
        results = socket.getaddrinfo(address, port, type=socket.SOCK_STREAM)
        addresses = sorted({result[4][0] for result in results})
        if len(addresses) != 1:
            raise OSError("Provider address did not resolve uniquely")
        return addresses[0]

    def _run(self, command: List[str]) -> None:
        try:
            subprocess.run(
                command,
                check=True,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
                text=True,
            )
        except subprocess.CalledProcessError as exc:
            detail = exc.stderr.strip() or "command failed"
            raise OSError("%s: %s" % (" ".join(command), detail)) from exc

    def ensure_configfs(self) -> None:
        self._run(["modprobe", "configfs"])
        mounted = subprocess.run(
            ["mountpoint", "-q", "/sys/kernel/config"],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        if mounted.returncode != 0:
            self._run(["mount", "-t", "configfs", "none", "/sys/kernel/config"])

    def ensure_module(self) -> None:
        self._run(["modprobe", "infiniswap"])

    def group_exists(self, name: str) -> bool:
        return (CONFIGFS_ROOT / name).is_dir()

    def device_path(self, name: str) -> str:
        return "/dev/" + name

    def read_attribute(self, name: str, attribute: str) -> str:
        return (CONFIGFS_ROOT / name / attribute).read_text(encoding="ascii").strip()

    def create_group(self, name: str) -> None:
        (CONFIGFS_ROOT / name).mkdir()

    def write_attribute(self, name: str, attribute: str, value: str) -> None:
        (CONFIGFS_ROOT / name / attribute).write_text(value + "\n", encoding="ascii")

    def remove_group(self, name: str) -> None:
        (CONFIGFS_ROOT / name).rmdir()

    def _swaps(self) -> dict:
        swaps = {}
        lines = Path("/proc/swaps").read_text(encoding="utf-8").splitlines()
        for line in lines[1:]:
            fields = line.split()
            if len(fields) >= 5:
                try:
                    swaps[os.path.realpath(fields[0])] = int(fields[4])
                except ValueError:
                    continue
        return swaps

    def is_swap_enabled(self, path: str) -> bool:
        return os.path.realpath(path) in self._swaps()

    def swap_priority(self, path: str) -> Any:
        return self._swaps().get(os.path.realpath(path))

    def has_swap_signature(self, path: str) -> bool:
        result = subprocess.run(
            ["blkid", "--probe", "--match-tag", "TYPE", "--output", "value", path],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
        )
        return result.returncode == 0 and result.stdout.strip() == "swap"

    def is_mounted(self, path: str) -> bool:
        result = subprocess.run(
            ["findmnt", "--noheadings", "--source", path],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        return result.returncode == 0

    def run_host_command(self, command: List[str]) -> None:
        self._run(command)
