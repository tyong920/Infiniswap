#!/usr/bin/env python3
"""QEMU and SSH system-boundary adapter for the VM validation CLI."""

from __future__ import annotations

import hashlib
import json
import os
import secrets
import shlex
import shutil
import signal
import socket
import subprocess
import tarfile
import tempfile
import time
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

from infiniswap_vm import GIB, ScenarioResult

IMAGES = {
    "5.15": {
        "release": "jammy",
        "filename": "jammy-server-cloudimg-amd64.img",
        "base_url": "https://cloud-images.ubuntu.com/jammy/current",
    },
    "6.8": {
        "release": "noble",
        "filename": "noble-server-cloudimg-amd64.img",
        "base_url": "https://cloud-images.ubuntu.com/noble/current",
    },
}

GUEST_PACKAGES = (
    "build-essential",
    "cmake",
    "dmsetup",
    "fio",
    "iptables",
    "libibverbs-dev",
    "librdmacm-dev",
    "libssl-dev",
    "ninja-build",
    "pkg-config",
    "python3",
    "rdma-core",
)


class BoundaryError(RuntimeError):
    pass


@dataclass
class Guest:
    name: str
    role: str
    resources: Dict[str, object]
    work: Path
    ssh_port: int
    mgmt_mac: str
    links: List[Dict[str, object]] = field(default_factory=list)
    process: Optional[subprocess.Popen] = None
    qmp_socket: Optional[Path] = None
    root_image: Optional[Path] = None
    seed_image: Optional[Path] = None
    backing_image: Optional[Path] = None


@dataclass
class CaseHandle:
    entry: Dict[str, object]
    case_dir: Path
    work: Path
    qmp_dir: Path
    key_path: Path
    source_archive: Path
    image_path: Path
    image_sha256: str
    guests: List[Guest]
    consumer: Guest
    providers: List[Guest]
    psks: List[str]
    data_ports: List[int]
    command_index: int = 0


class QemuBackend:
    """Create disposable guests and execute the state/fault matrix."""

    def __init__(self, args, repo_root: Path):
        self.args = args
        self.repo_root = repo_root.resolve()
        self.cache_dir = args.cache_dir.expanduser().resolve()

    def start(self, entry, artifacts):
        case_dir = artifacts.resolve()
        work = case_dir / "work"
        case_dir.mkdir(parents=True, exist_ok=True)
        work.mkdir(parents=True, exist_ok=True)
        image_path, image_sha256 = self._ensure_image(entry["kernel"])
        key_path = work / "id_ed25519"
        self._create_ssh_key(key_path)
        source_archive = work / "source.tar.gz"
        self._create_source_archive(source_archive)
        guests, data_ports = self._guest_layout(entry, work)
        # Keep QMP socket paths below Linux sockaddr_un's 108-byte limit.
        qmp_dir = Path(tempfile.mkdtemp(prefix="infiniswap-qmp-", dir="/tmp"))
        handle = CaseHandle(
            entry=entry,
            case_dir=case_dir,
            work=work,
            qmp_dir=qmp_dir,
            key_path=key_path,
            source_archive=source_archive,
            image_path=image_path,
            image_sha256=image_sha256,
            guests=guests,
            consumer=guests[0],
            providers=guests[1:],
            psks=[secrets.token_hex(32) for _ in guests[1:]],
            data_ports=data_ports,
        )
        try:
            public_key = key_path.with_suffix(".pub").read_text(
                encoding="ascii"
            ).strip()
            for guest in guests:
                self._prepare_guest_images(handle, guest, public_key)
            self._start_qemu(handle, handle.consumer)
            time.sleep(1)
            for provider in handle.providers:
                self._start_qemu(handle, provider)
            for guest in guests:
                self._wait_for_ssh(handle, guest, timeout=600)
                self._provision_guest(handle, guest)
            for guest in guests:
                self._helper(
                    handle, guest, "kernel-baseline-%s" % guest.name, "mark-kernel"
                )
            self._configure_links(handle)
            self._install_secrets(handle)
            for index in range(len(handle.providers)):
                self._start_provider(handle, index)
            return handle
        except Exception:
            self._force_cleanup(handle)
            raise

    def run_scenario(self, handle, scenario_id, artifacts):
        scenarios = {
            "build-deploy": self._scenario_build_deploy,
            "fio-verification": self._scenario_fio,
            "swap-pressure": self._scenario_swap_pressure,
            "normal-shutdown": self._scenario_normal_shutdown,
            "remote-chunk-certification": self._scenario_remote_chunk_certification,
            "provider-process-kill": self._scenario_process_kill,
            "network-interruption": self._scenario_network_interruption,
            "backing-store-error": self._scenario_backing_error,
            "guest-reboot": self._scenario_guest_reboot,
            "safe-module-reload": self._scenario_module_reload,
            "soak": self._scenario_soak,
        }
        action = scenarios.get(scenario_id)
        if action is None:
            raise BoundaryError("unsupported scenario: %s" % scenario_id)
        return action(handle)

    def collect(self, handle, artifacts):
        collected = []
        errors = []
        for guest in handle.guests:
            if guest.process is None or guest.process.poll() is not None:
                errors.append("%s QEMU is not running" % guest.name)
                continue
            try:
                self._helper(handle, guest, "collect-%s" % guest.name, "collect")
                destination = handle.case_dir / ("guest-%s.tar.gz" % guest.name)
                self._download_guest_artifacts(handle, guest, destination)
                collected.append(str(destination))
            except Exception as error:
                marker = handle.case_dir / ("collect-%s.error.txt" % guest.name)
                marker.write_text(str(error) + "\n", encoding="utf-8")
                collected.append(str(marker))
                errors.append("%s: %s" % (guest.name, error))
        for guest in handle.guests:
            serial = handle.case_dir / ("serial-%s.log" % guest.name)
            qemu_log = handle.case_dir / ("qemu-%s.log" % guest.name)
            if serial.exists():
                collected.append(str(serial))
            if qemu_log.exists():
                collected.append(str(qemu_log))
        if errors:
            raise BoundaryError("guest artifact collection failed: " + "; ".join(errors))
        return tuple(collected)

    def shutdown(self, handle):
        addresses = [str(link["provider_ip"]) for link in handle.consumer.links]
        self._helper(
            handle,
            handle.consumer,
            "guest-cleanup-consumer",
            "cleanup",
            *addresses,
            timeout=180,
        )
        for provider in handle.providers:
            self._helper(
                handle,
                provider,
                "guest-cleanup-%s" % provider.name,
                "cleanup",
                timeout=120,
            )
        self.collect(handle, handle.case_dir)

        for guest in reversed(handle.guests):
            if guest.process is None or guest.process.poll() is not None:
                continue
            self._ssh_shell(
                handle,
                guest,
                "poweroff-%s" % guest.name,
                "shutdown -h now",
                sudo=True,
                timeout=15,
                check=False,
            )
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            if all(
                guest.process is None or guest.process.poll() is not None
                for guest in handle.guests
            ):
                return
            time.sleep(1)
        self._force_processes(handle)
        raise BoundaryError("normal guest shutdown timed out; QEMU processes were killed")

    def cleanup(self, handle, retain):
        if retain:
            retained = handle.case_dir / "RETAINED"
            retained.write_text(
                "VMs retained after failure. QMP sockets are under %s; "
                "disks are under work/.\n" % handle.qmp_dir,
                encoding="utf-8",
            )
            return
        self._force_cleanup(handle)

    def leaks(self, handle):
        leaks = []
        for guest in handle.guests:
            if guest.process is not None and guest.process.poll() is None:
                leaks.append("qemu-process:%s:%d" % (guest.name, guest.process.pid))
            if guest.qmp_socket is not None and guest.qmp_socket.exists():
                leaks.append("qmp-socket:%s" % guest.qmp_socket)
            for image in (guest.root_image, guest.seed_image, guest.backing_image):
                if image is not None and image.exists():
                    leaks.append("image:%s" % image)
        return tuple(leaks)

    def _ensure_image(self, kernel: str) -> Tuple[Path, str]:
        image = IMAGES[kernel]
        release_dir = self.cache_dir / image["release"]
        release_dir.mkdir(parents=True, exist_ok=True)
        checksum_url = image["base_url"] + "/SHA256SUMS"
        with urllib.request.urlopen(checksum_url, timeout=60) as response:
            checksum_text = response.read().decode("utf-8")
        expected = None
        for line in checksum_text.splitlines():
            fields = line.split()
            if len(fields) == 2 and fields[1].lstrip("*") == image["filename"]:
                expected = fields[0]
                break
        if expected is None:
            raise BoundaryError("Ubuntu checksum does not list %s" % image["filename"])
        destination = release_dir / image["filename"]
        if destination.exists() and self._sha256(destination) != expected:
            destination.unlink()
        if not destination.exists():
            temporary = destination.with_suffix(destination.suffix + ".part")
            with urllib.request.urlopen(
                image["base_url"] + "/" + image["filename"], timeout=300
            ) as response, temporary.open("wb") as target:
                shutil.copyfileobj(response, target, length=1024 * 1024)
            actual = self._sha256(temporary)
            if actual != expected:
                temporary.unlink(missing_ok=True)
                raise BoundaryError(
                    "cloud image checksum mismatch: expected %s, got %s"
                    % (expected, actual)
                )
            temporary.replace(destination)
        (release_dir / "SHA256SUMS").write_text(checksum_text, encoding="utf-8")
        return destination, expected

    @staticmethod
    def _sha256(path: Path) -> str:
        digest = hashlib.sha256()
        with path.open("rb") as source:
            for block in iter(lambda: source.read(1024 * 1024), b""):
                digest.update(block)
        return digest.hexdigest()

    def _create_ssh_key(self, path: Path) -> None:
        self._run(
            ["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", str(path)],
            timeout=30,
        )

    def _create_source_archive(self, destination: Path) -> None:
        result = self._run(
            [
                "git",
                "ls-files",
                "--cached",
                "--others",
                "--exclude-standard",
                "-z",
            ],
            cwd=self.repo_root,
            timeout=30,
            text=False,
        )
        paths = [
            item.decode("utf-8")
            for item in result.stdout.split(b"\0")
            if item
        ]
        with tarfile.open(destination, "w:gz") as archive:
            for relative in paths:
                source = self.repo_root / relative
                if source.is_file() or source.is_symlink():
                    archive.add(source, arcname=str(Path("infiniswap") / relative))

    def _guest_layout(self, entry, work: Path):
        data_ports = [self._free_port() for _ in range(entry["topology"] - 1)]
        guests = []
        for index, resources in enumerate(entry["guests"]):
            guest = Guest(
                name=resources["name"],
                role=resources["role"],
                resources=resources,
                work=work / resources["name"],
                ssh_port=self._free_port(),
                mgmt_mac="52:54:00:00:00:%02x" % (index + 1),
            )
            guest.work.mkdir(parents=True, exist_ok=True)
            guests.append(guest)
        consumer = guests[0]
        for index, provider in enumerate(guests[1:]):
            third_octet = 50 + index
            link = {
                "index": index,
                "port": data_ports[index],
                "consumer_mac": "52:54:00:10:00:%02x" % (index + 1),
                "provider_mac": "52:54:00:20:00:%02x" % (index + 1),
                "consumer_ip": "192.168.%d.1" % third_octet,
                "provider_ip": "192.168.%d.2" % third_octet,
                "prefix": 30,
                "consumer_rail": "rxe%d" % index,
                "provider_rail": "rxe0",
            }
            consumer.links.append(link)
            provider.links.append(link)
        return guests, data_ports

    def _prepare_guest_images(self, handle, guest: Guest, public_key: str) -> None:
        guest.root_image = guest.work / "root.qcow2"
        self._run(
            [
                "qemu-img",
                "create",
                "-q",
                "-f",
                "qcow2",
                "-F",
                "qcow2",
                "-b",
                str(handle.image_path),
                str(guest.root_image),
                "%dG" % guest.resources["root_disk_gib"],
            ],
            timeout=60,
        )
        if guest.resources["backing_disk_gib"]:
            guest.backing_image = guest.work / "backing.qcow2"
            self._run(
                [
                    "qemu-img",
                    "create",
                    "-q",
                    "-f",
                    "qcow2",
                    str(guest.backing_image),
                    "%dG" % guest.resources["backing_disk_gib"],
                ],
                timeout=60,
            )
        user_data = guest.work / "user-data"
        meta_data = guest.work / "meta-data"
        user_data.write_text(
            "#cloud-config\n"
            "users:\n"
            "  - default\n"
            "  - name: infiniswap\n"
            "    groups: [sudo]\n"
            "    shell: /bin/bash\n"
            "    sudo: ALL=(ALL) NOPASSWD:ALL\n"
            "    ssh_authorized_keys:\n"
            "      - %s\n"
            "ssh_pwauth: false\n"
            "disable_root: true\n"
            "growpart:\n"
            "  mode: auto\n"
            "  devices: ['/']\n"
            "resize_rootfs: true\n" % public_key,
            encoding="utf-8",
        )
        meta_data.write_text(
            "instance-id: infiniswap-%s-%s\nlocal-hostname: %s\n"
            % (handle.entry["kernel"].replace(".", ""), guest.name, guest.name),
            encoding="utf-8",
        )
        guest.seed_image = guest.work / "seed.img"
        self._run(
            [
                "cloud-localds",
                str(guest.seed_image),
                str(user_data),
                str(meta_data),
            ],
            timeout=30,
        )

    def _qemu_command(self, handle, guest: Guest):
        guest.qmp_socket = handle.qmp_dir / (guest.name + ".sock")
        command = [
            "ionice",
            "-c",
            "3",
            "nice",
            "-n",
            "10",
            "qemu-system-x86_64",
            "-name",
            "infiniswap-%s-%s" % (handle.entry["kernel"], guest.name),
            "-machine",
            "q35,accel=kvm",
            "-cpu",
            "host",
            "-smp",
            str(guest.resources["vcpus"]),
            "-m",
            str(int(guest.resources["memory_gib"]) * 1024),
            "-display",
            "none",
            "-monitor",
            "none",
            "-qmp",
            "unix:%s,server=on,wait=off" % guest.qmp_socket,
            "-serial",
            "file:%s" % (handle.case_dir / ("serial-%s.log" % guest.name)),
            "-drive",
            "if=virtio,format=qcow2,file=%s,cache=none,discard=unmap"
            % guest.root_image,
            "-drive",
            "if=ide,media=cdrom,format=raw,file=%s,readonly=on"
            % guest.seed_image,
            "-netdev",
            "user,id=mgmt,hostfwd=tcp:127.0.0.1:%d-:22" % guest.ssh_port,
            "-device",
            "virtio-net-pci,netdev=mgmt,mac=%s" % guest.mgmt_mac,
        ]
        if guest.backing_image is not None:
            command.extend(
                [
                    "-drive",
                    "if=virtio,format=qcow2,file=%s,cache=none,discard=unmap"
                    % guest.backing_image,
                ]
            )
        for link in guest.links:
            netdev_id = "data%d" % link["index"]
            if guest.role == "consumer":
                socket_option = "listen=127.0.0.1:%d" % link["port"]
                mac = link["consumer_mac"]
            else:
                socket_option = "connect=127.0.0.1:%d" % link["port"]
                mac = link["provider_mac"]
            command.extend(
                [
                    "-netdev",
                    "socket,id=%s,%s" % (netdev_id, socket_option),
                    "-device",
                    "virtio-net-pci,netdev=%s,mac=%s" % (netdev_id, mac),
                ]
            )
        return command

    def _start_qemu(self, handle, guest: Guest) -> None:
        log_path = handle.case_dir / ("qemu-%s.log" % guest.name)
        log = log_path.open("wb")
        try:
            guest.process = subprocess.Popen(
                self._qemu_command(handle, guest),
                stdin=subprocess.DEVNULL,
                stdout=log,
                stderr=subprocess.STDOUT,
                start_new_session=True,
            )
        finally:
            log.close()
        time.sleep(0.5)
        if guest.process.poll() is not None:
            detail = log_path.read_text(encoding="utf-8", errors="replace")[-4000:]
            raise BoundaryError("QEMU %s exited during startup: %s" % (guest.name, detail))

    def _wait_for_ssh(self, handle, guest: Guest, timeout: int) -> None:
        deadline = time.monotonic() + timeout
        last_error = "SSH not attempted"
        while time.monotonic() < deadline:
            if guest.process is not None and guest.process.poll() is not None:
                raise BoundaryError("QEMU %s exited before SSH became ready" % guest.name)
            result = self._ssh_shell(
                handle,
                guest,
                "ssh-wait-%s" % guest.name,
                "true",
                timeout=10,
                check=False,
            )
            if result.returncode == 0:
                return
            last_error = result.stderr[-500:]
            time.sleep(2)
        raise BoundaryError("SSH did not become ready for %s: %s" % (guest.name, last_error))

    def _provision_guest(self, handle, guest: Guest) -> None:
        self._ssh_shell(
            handle,
            guest,
            "cloud-init-%s" % guest.name,
            "cloud-init status --wait",
            sudo=True,
            timeout=600,
        )
        packages = " ".join(shlex.quote(package) for package in GUEST_PACKAGES)
        self._ssh_shell(
            handle,
            guest,
            "packages-%s" % guest.name,
            "export DEBIAN_FRONTEND=noninteractive; "
            "apt-get update; apt-get install -y --no-install-recommends %s "
            "linux-headers-$(uname -r) linux-modules-extra-$(uname -r)" % packages,
            sudo=True,
            timeout=1800,
        )
        self._scp_to_guest(handle, guest, handle.source_archive, "/tmp/source.tar.gz")
        self._ssh_shell(
            handle,
            guest,
            "source-%s" % guest.name,
            "rm -rf /opt/infiniswap; tar -xzf /tmp/source.tar.gz -C /opt; "
            "chown -R infiniswap:infiniswap /opt/infiniswap; "
            "mkdir -p /var/tmp/infiniswap-vm-artifacts; "
            "chmod 0700 /var/tmp/infiniswap-vm-artifacts",
            sudo=True,
            timeout=120,
        )
        kernel = self._helper(handle, guest, "kernel-%s" % guest.name, "kernel").stdout.strip()
        if not kernel.startswith(handle.entry["kernel"] + "."):
            raise BoundaryError(
                "%s booted kernel %s, expected Linux %s GA"
                % (guest.name, kernel, handle.entry["kernel"])
            )
        if guest.role == "consumer":
            self._helper(
                handle, guest, "build-consumer", "build-consumer", timeout=1800
            )
        else:
            self._helper(
                handle, guest, "build-provider-%s" % guest.name, "build-provider", timeout=1800
            )

    def _configure_links(self, handle) -> None:
        for link in handle.consumer.links:
            self._helper(
                handle,
                handle.consumer,
                "rxe-consumer-%d" % link["index"],
                "setup-rxe",
                str(link["consumer_rail"]),
                str(link["consumer_mac"]),
                "%s/%s" % (link["consumer_ip"], link["prefix"]),
            )
        for provider in handle.providers:
            link = provider.links[0]
            self._helper(
                handle,
                provider,
                "rxe-%s" % provider.name,
                "setup-rxe",
                str(link["provider_rail"]),
                str(link["provider_mac"]),
                "%s/%s" % (link["provider_ip"], link["prefix"]),
            )

    def _install_secrets(self, handle) -> None:
        for index, provider in enumerate(handle.providers):
            provider_path = "/etc/infiniswap-vm.psk"
            consumer_path = "/etc/infiniswap-vm-provider-%d.psk" % index
            self._write_secret(handle, provider, provider_path, handle.psks[index])
            self._write_secret(handle, handle.consumer, consumer_path, handle.psks[index])

    def _write_secret(self, handle, guest, path: str, value: str) -> None:
        self._ssh_shell(
            handle,
            guest,
            "secret-%s" % guest.name,
            "umask 077; cat >%s" % shlex.quote(path),
            sudo=True,
            timeout=30,
            input_text=value + "\n",
        )

    def _start_provider(self, handle, index: int) -> None:
        provider = handle.providers[index]
        self._helper(
            handle,
            provider,
            "provider-start-%d" % index,
            "provider-start",
            "19420",
            "/etc/infiniswap-vm.psk",
            "provider-%d" % index,
        )

    def _provider_specs(self, handle, indices=None):
        if indices is None:
            indices = range(len(handle.providers))
        specs = []
        for index in indices:
            link = handle.consumer.links[index]
            specs.append(
                "provider-%d|%s|19420|%s|/etc/infiniswap-vm-provider-%d.psk|%d"
                % (
                    index,
                    link["provider_ip"],
                    link["consumer_rail"],
                    index,
                    100 + index * 50,
                )
            )
        return specs

    def _create_device(
        self,
        handle,
        mode: str,
        provider_indices=None,
        capacity_gib: Optional[int] = None,
        policy: str = "strict",
        backing: str = "/dev/vdb",
        wait_for_connection: bool = True,
        failure_deadline_ms: int = 2000,
    ) -> None:
        specs = self._provider_specs(handle, provider_indices)
        if not wait_for_connection:
            specs.insert(0, "--allow-not-connected")
        if capacity_gib is None:
            capacity_gib = len(specs) if mode == "remote-only" else 2
        self._helper(
            handle,
            handle.consumer,
            "device-create-%s" % mode,
            "device-create",
            mode,
            policy,
            backing,
            str(capacity_gib * GIB),
            str(failure_deadline_ms),
            *specs,
            timeout=180,
        )

    def _stop_device(self, handle) -> None:
        self._helper(handle, handle.consumer, "device-stop", "device-stop", timeout=120)

    def _check_kernel(self, handle) -> None:
        for guest in handle.guests:
            self._helper(
                handle,
                guest,
                "kernel-check-%s" % guest.name,
                "check-kernel",
            )

    def _scenario_build_deploy(self, handle):
        command = (
            "test -f /opt/infiniswap/infiniswap_bd/infiniswap.ko; "
            "test -x /opt/infiniswap/bin/infiniswapctl; rdma link show"
        )
        self._ssh_shell(
            handle,
            handle.consumer,
            "verify-build-consumer",
            command,
            sudo=True,
            timeout=60,
        )
        for provider in handle.providers:
            self._ssh_shell(
                handle,
                provider,
                "verify-build-%s" % provider.name,
                "test -x /opt/infiniswap/build/daemon/infiniswap-daemon; "
                "systemctl is-active --quiet infiniswap-vm-provider.service; "
                "rdma link show",
                sudo=True,
                timeout=60,
            )
        kernel_releases = []
        for index, guest in enumerate(handle.guests):
            result = self._ssh_shell(
                handle,
                guest,
                "kernel-release-%d" % index,
                "uname -r",
                timeout=30,
            )
            kernel_releases.append(result.stdout.strip())
        if (
            not kernel_releases[0]
            or any(value != kernel_releases[0] for value in kernel_releases)
            or (
                hasattr(handle, "entry")
                and not kernel_releases[0].startswith(handle.entry["kernel"] + ".")
            )
        ):
            raise BoundaryError("guest kernel ABI does not match the matrix entry")
        psk_path = "/etc/infiniswap-vm-provider-0.psk"
        self._helper(
            handle, handle.consumer, "auth-corrupt-psk", "psk-corrupt", psk_path
        )
        try:
            self._create_device(
                handle,
                "backed",
                provider_indices=(0,),
                capacity_gib=1,
                wait_for_connection=False,
            )
            self._helper(
                handle,
                handle.consumer,
                "auth-failure-counter",
                "wait-counter",
                "authentication_failures_total",
                "1",
                "200",
            )
            self._helper(
                handle,
                handle.consumer,
                "alert-auth-failure",
                "assert-alert",
                "auth-failure",
                "InfiniswapAuthenticationFailure",
                "present",
            )
        finally:
            self._stop_device(handle)
            self._helper(
                handle,
                handle.consumer,
                "auth-restore-psk",
                "psk-restore",
                psk_path,
                check=False,
            )
        self._create_device(handle, "backed", provider_indices=(0,), capacity_gib=1)
        self._helper(
            handle,
            handle.consumer,
            "alert-auth-cleared",
            "assert-alert",
            "auth-cleared",
            "InfiniswapAuthenticationFailure",
            "absent",
        )
        self._stop_device(handle)
        return ScenarioResult(
            status="passed",
            detail="supported GA kernel booted; Consumer and Providers built and deployed",
            metrics={
                "cloud_image_sha256": handle.image_sha256,
                "guest_count": len(handle.guests),
                "kernel_release": kernel_releases[0],
                "rdma_stack": "inbox",
            },
        )

    def _scenario_fio(self, handle):
        if getattr(getattr(self, "args", None), "scenario", "all") == "fio-verification":
            return self._scenario_focused_fio(handle)

        provider_count = len(handle.providers)
        self._create_device(handle, "backed")
        self._helper(
            handle,
            handle.consumer,
            "heat-backed",
            "heat-ranges",
            str(provider_count),
            timeout=180,
        )
        self._helper(handle, handle.consumer, "fio-backed", "fio", "backed")
        self._helper(handle, handle.consumer, "status-backed", "snapshot", "backed")
        self._stop_device(handle)

        self._create_device(handle, "remote-only", capacity_gib=provider_count)
        self._helper(
            handle,
            handle.consumer,
            "heat-remote-only",
            "heat-ranges",
            str(provider_count),
            timeout=180,
        )
        self._helper(
            handle,
            handle.consumer,
            "heartbeat-remote-only",
            "verify-remote-only-heartbeats",
            "5",
            timeout=30,
        )
        self._helper(
            handle, handle.consumer, "fio-remote-only", "fio", "remote-only"
        )
        self._helper(
            handle,
            handle.consumer,
            "status-remote-only",
            "snapshot",
            "remote-only",
        )
        self._stop_device(handle)
        self._check_kernel(handle)
        return ScenarioResult(
            status="passed",
            detail="verified fio passed in Backed and Remote-Only modes",
            artifacts=(
                "guest:consumer:fio-backed.json",
                "guest:consumer:fio-remote-only.json",
            ),
            metrics={"providers": provider_count},
        )

    def _scenario_focused_fio(self, handle):
        performance_dir = handle.case_dir / "performance"
        performance_dir.mkdir(parents=True, exist_ok=True)
        artifact_paths = []
        evidence = {}
        provider_count = len(handle.providers)

        for mode in ("backed", "remote-only"):
            capacity_gib = provider_count if mode == "remote-only" else None
            self._create_device(handle, mode, capacity_gib=capacity_gib)
            try:
                self._helper(
                    handle,
                    handle.consumer,
                    "performance-heat-%s" % mode,
                    "heat-ranges",
                    str(provider_count),
                    timeout=180,
                )
                if mode == "remote-only":
                    self._helper(
                        handle,
                        handle.consumer,
                        "performance-heartbeat-remote-only",
                        "verify-remote-only-heartbeats",
                        "5",
                        timeout=30,
                    )
                warmup_io_errors = None
                for phase, count in (("warmup", 1), ("measured", 5)):
                    for sample_index in range(count):
                        label = "performance-%s-%s-%d" % (
                            mode,
                            phase,
                            sample_index + 1,
                        )
                        result = self._helper(
                            handle,
                            handle.consumer,
                            label,
                            "fio-performance",
                            mode,
                            phase,
                            str(sample_index + 1),
                            "60",
                            timeout=420,
                        )
                        payload = json.loads(result.stdout)
                        raw_fio = payload["fio"]
                        verification = payload.get("verification")
                        fio_documents = [raw_fio]
                        if verification is not None:
                            fio_documents.append(verification)
                        if any(
                            job.get("error", 0)
                            for document in fio_documents
                            for job in document.get("jobs", ())
                        ):
                            raise BoundaryError("fio reported an I/O verification error")
                        envelope = {
                            "schema_version": 1,
                            "kind": "infiniswap.soft-roce-fio-sample",
                            "metadata": {
                                "source_commit": self.args.source_commit,
                                "cloud_image_sha256": handle.image_sha256,
                                "host_identity": self.args.host_identity,
                                "ubuntu_release": handle.entry["ubuntu"],
                                "kernel_release": handle.entry["kernel_release"],
                                "rdma_stack": handle.entry["rdma_stack"],
                                "topology": handle.entry["topology"],
                                "vm_resources": handle.entry["resources"],
                                "guests": handle.entry["guests"],
                                "mode": mode,
                                "phase": phase,
                                "sample_index": sample_index + 1,
                                "duration_seconds": 60,
                                "fio_arguments": payload["arguments"],
                                "fio_verification_arguments": payload.get(
                                    "verification_arguments", []
                                ),
                            },
                            "fio": raw_fio,
                            "verification": verification,
                        }
                        path = performance_dir / (label + ".json")
                        path.write_text(
                            json.dumps(envelope, indent=2, sort_keys=True) + "\n",
                            encoding="utf-8",
                        )
                        artifact_paths.append(
                            str(path.relative_to(handle.case_dir.parent))
                        )
                    if phase == "warmup":
                        warmup_result = self._helper(
                            handle,
                            handle.consumer,
                            "performance-warmup-evidence-%s" % mode,
                            "performance-evidence",
                            mode,
                        )
                        warmup_evidence = json.loads(warmup_result.stdout)
                        if warmup_evidence.get("status") != "passed":
                            raise BoundaryError(
                                "%s warmup safety evidence failed" % mode
                            )
                        warmup_io_errors = warmup_evidence["io_errors_total"]
                evidence_result = self._helper(
                    handle,
                    handle.consumer,
                    "performance-evidence-%s" % mode,
                    "performance-evidence",
                    mode,
                )
                evidence[mode] = json.loads(evidence_result.stdout)
                evidence[mode]["io_errors_before_measured"] = warmup_io_errors
                if evidence[mode].get("status") != "passed":
                    raise BoundaryError("%s performance safety evidence failed" % mode)
                if evidence[mode]["io_errors_total"] != warmup_io_errors:
                    raise BoundaryError(
                        "%s measured samples increased the I/O error counter" % mode
                    )
            finally:
                self._stop_device(handle)

        self._check_kernel(handle)
        return ScenarioResult(
            status="passed",
            detail=(
                "NON-CERTIFIABLE focused fio completed one warmup and five "
                "measured 60-second samples per mode"
            ),
            artifacts=tuple(artifact_paths),
            metrics={
                "providers": provider_count,
                "non_certifiable": True,
                "sample_duration_seconds": 60,
                "warmup_samples_per_mode": 1,
                "measured_samples_per_mode": 5,
                "evidence": evidence,
            },
        )

    def _scenario_swap_pressure(self, handle):
        self._create_device(handle, "backed")
        self._helper(handle, handle.consumer, "heat-swap", "heat-ranges", "1")
        self._helper(
            handle,
            handle.consumer,
            "swap-pressure",
            "swap-pressure",
            timeout=900,
        )
        self._helper(handle, handle.consumer, "status-swap", "snapshot", "swap")
        self._stop_device(handle)
        self._check_kernel(handle)
        return ScenarioResult(
            status="passed",
            detail="guest-only swap pressure used the Infiniswap Device",
            artifacts=("guest:consumer:swap-pressure.json",),
        )

    def _scenario_normal_shutdown(self, handle):
        self._create_device(handle, "backed")
        self._helper(
            handle, handle.consumer, "normal-write", "write-pattern", "normal", "64"
        )
        self._helper(
            handle, handle.consumer, "normal-read", "read-pattern", "normal", "64"
        )
        self._helper(
            handle, handle.consumer, "status-normal", "snapshot", "normal-shutdown"
        )
        self._stop_device(handle)
        self._check_kernel(handle)
        return ScenarioResult(
            status="passed",
            detail="normal Infiniswap Device shutdown completed without leaked I/O",
        )

    def _scenario_remote_chunk_certification(self, handle):
        provider = handle.providers[0]
        provider_id = "provider-0"
        consumer_rail = str(handle.consumer.links[0]["consumer_rail"])

        self._helper(
            handle,
            handle.consumer,
            "remote-chunk-contracts",
            "remote-chunk-contracts",
            timeout=180,
        )

        self._helper(
            handle,
            provider,
            "provider-eviction-policy",
            "provider-start",
            "19420",
            "/etc/infiniswap-vm.psk",
            provider_id,
            "auto",
            "1",
            "2",
        )
        self._create_device(
            handle,
            "backed",
            provider_indices=(0,),
            capacity_gib=1,
            failure_deadline_ms=5000,
        )
        self._helper(handle, handle.consumer, "eviction-heat", "heat-ranges", "1")
        self._helper(
            handle,
            handle.consumer,
            "eviction-delay",
            "rdma-delay",
            "add",
            consumer_rail,
            "3000",
        )
        try:
            self._helper(
                handle,
                handle.consumer,
                "eviction-io-start",
                "eviction-io",
                "start",
            )
            self._helper(
                handle,
                handle.consumer,
                "eviction-observer-start",
                "observe-eviction",
                "start",
                provider_id,
            )
            self._helper(
                handle,
                provider,
                "eviction-pressure-start",
                "provider-pressure",
                "start",
                "backed",
                timeout=180,
            )
            self._helper(
                handle,
                handle.consumer,
                "eviction-observe",
                "observe-eviction",
                "wait",
                timeout=90,
            )
            self._helper(
                handle,
                handle.consumer,
                "eviction-io-wait",
                "eviction-io",
                "wait",
                timeout=150,
            )
            self._helper(
                handle,
                handle.consumer,
                "eviction-fallback-write",
                "write-pattern",
                "eviction-fallback",
                "2048",
            )
            self._helper(
                handle,
                handle.consumer,
                "eviction-fallback-read",
                "read-pattern",
                "eviction-fallback",
                "2048",
            )
            self._helper(
                handle,
                handle.consumer,
                "status-atomic-eviction",
                "snapshot",
                "atomic-eviction",
            )
        finally:
            self._helper(
                handle,
                handle.consumer,
                "eviction-io-stop",
                "eviction-io",
                "stop",
                check=False,
            )
            self._helper(
                handle,
                handle.consumer,
                "eviction-observer-stop",
                "observe-eviction",
                "stop",
                check=False,
            )
            self._helper(
                handle,
                provider,
                "eviction-pressure-stop",
                "provider-pressure",
                "stop",
                check=False,
            )
            self._helper(
                handle,
                handle.consumer,
                "eviction-delay-clear",
                "rdma-delay",
                "clear",
                consumer_rail,
                check=False,
            )
        self._stop_device(handle)
        self._reset_transports(handle)

        provider = handle.providers[0]
        provider_spec = self._provider_specs(handle, (0,))[0]
        self._helper(
            handle,
            provider,
            "provider-committed-policy",
            "provider-start",
            "19420",
            "/etc/infiniswap-vm.psk",
            provider_id,
            "auto",
            "1",
            "2",
        )
        self._helper(
            handle,
            handle.consumer,
            "remote-only-admission-rejection",
            "expect-remote-only-admission-failure",
            str(3 * GIB),
            provider_spec,
            timeout=180,
        )
        self._create_device(
            handle, "remote-only", provider_indices=(0,), capacity_gib=1
        )
        try:
            self._helper(
                handle,
                provider,
                "committed-pressure-start",
                "provider-pressure",
                "start",
                "remote-only",
                timeout=180,
            )
            self._helper(
                handle,
                handle.consumer,
                "committed-under-pressure",
                "verify-remote-only-committed",
                provider_id,
                "5",
            )
            self._helper(
                handle,
                handle.consumer,
                "committed-write",
                "write-pattern",
                "committed-pressure",
                "0",
            )
            self._helper(
                handle,
                handle.consumer,
                "committed-read",
                "read-pattern",
                "committed-pressure",
                "0",
            )
        finally:
            self._helper(
                handle,
                provider,
                "committed-pressure-stop",
                "provider-pressure",
                "stop",
                check=False,
            )
        self._helper(
            handle, provider, "remote-only-provider-kill", "provider-kill"
        )
        self._helper(
            handle,
            handle.consumer,
            "remote-only-lost",
            "wait-field",
            "connection_state",
            "remote-lost",
            "400",
        )
        self._helper(
            handle,
            handle.consumer,
            "remote-only-operational-lost",
            "wait-field",
            "operational_state",
            "remote-lost",
            "20",
        )
        self._helper(
            handle,
            handle.consumer,
            "remote-only-lost-once",
            "wait-field",
            "remote_lost_transitions_total",
            "1",
            "20",
        )
        self._helper(
            handle,
            handle.consumer,
            "remote-only-io-failure",
            "expect-io-failure",
        )
        self._helper(
            handle,
            handle.consumer,
            "status-remote-chunk-remote-lost",
            "snapshot",
            "remote-chunk-remote-lost",
        )
        self._stop_device(handle)
        self._reset_transports(handle)
        self._check_kernel(handle)
        return ScenarioResult(
            status="passed",
            detail=(
                "atomic eviction drained accepted I/O, used Backing Store fallback, "
                "rejected insufficient aggregate Committed Pool capacity without "
                "partial exposure, preserved Committed Remote Chunks under pressure, "
                "and retained terminal Remote-Lost behavior"
            ),
            artifacts=(
                "guest:consumer:remote-chunk-contracts.json",
                "guest:consumer:atomic-eviction.json",
                "guest:consumer:fio-atomic-eviction.json",
                "guest:consumer:status-atomic-eviction.json",
                "guest:consumer:remote-only-admission-rejection.json",
                "guest:consumer:status-remote-chunk-remote-lost.json",
                "guest:provider-0:provider-pressure-backed.json",
                "guest:provider-0:provider-pressure-remote-only.json",
            ),
        )

    def _scenario_process_kill(self, handle):
        self._create_device(handle, "backed", provider_indices=(0,), capacity_gib=1)
        self._helper(handle, handle.consumer, "kill-heat", "heat-ranges", "1")
        self._helper(handle, handle.providers[0], "provider-kill-backed", "provider-kill")
        self._helper(
            handle,
            handle.consumer,
            "kill-trigger-backed",
            "write-pattern",
            "kill-backed",
            "128",
        )
        self._helper(
            handle, handle.consumer, "kill-exclusion", "wait-exclusion", "provider-0"
        )
        self._helper(
            handle,
            handle.consumer,
            "alert-kill-disconnected",
            "assert-alert",
            "kill-disconnected",
            "InfiniswapProviderDisconnected",
            "present",
        )
        self._helper(
            handle, handle.consumer, "kill-read-backed", "read-pattern", "heat-0", "0"
        )
        self._helper(
            handle, handle.consumer, "status-kill-backed", "snapshot", "kill-backed"
        )
        self._stop_device(handle)
        self._reset_transports(handle)

        self._create_device(
            handle, "remote-only", provider_indices=(0,), capacity_gib=1
        )
        self._helper(
            handle,
            handle.consumer,
            "kill-write-remote",
            "write-pattern",
            "kill-remote",
            "0",
        )
        self._helper(handle, handle.providers[0], "provider-kill-remote", "provider-kill")
        self._helper(
            handle,
            handle.consumer,
            "kill-remote-lost",
            "wait-field",
            "connection_state",
            "remote-lost",
            "400",
        )
        self._helper(
            handle, handle.consumer, "kill-io-failure", "expect-io-failure"
        )
        self._helper(
            handle,
            handle.consumer,
            "alert-kill-remote-lost",
            "assert-alert",
            "kill-remote-lost",
            "InfiniswapRemoteLost",
            "present",
        )
        self._helper(
            handle,
            handle.consumer,
            "alert-kill-io-errors",
            "assert-alert",
            "kill-io-errors",
            "InfiniswapIOErrors",
            "present",
        )
        self._helper(
            handle, handle.consumer, "status-kill-remote", "snapshot", "kill-remote"
        )
        self._stop_device(handle)
        self._reset_transports(handle)
        self._create_device(handle, "backed", provider_indices=(0,), capacity_gib=1)
        for alert_name in (
            "InfiniswapProviderDisconnected",
            "InfiniswapProviderDeadlineExpired",
            "InfiniswapRemoteLost",
            "InfiniswapIOErrors",
        ):
            self._helper(
                handle,
                handle.consumer,
                "alert-kill-cleared-%s" % alert_name,
                "assert-alert",
                "kill-cleared-%s" % alert_name,
                alert_name,
                "absent",
            )
        self._stop_device(handle)
        self._check_kernel(handle)
        return ScenarioResult(
            status="passed",
            detail="Provider SIGKILL preserved Backed data and made Remote-Only terminal",
        )

    def _scenario_network_interruption(self, handle):
        address = str(handle.consumer.links[0]["provider_ip"])
        self._create_device(handle, "backed", provider_indices=(0,), capacity_gib=1)
        self._helper(handle, handle.consumer, "network-heat", "heat-ranges", "1")
        self._helper(
            handle, handle.consumer, "network-drop-backed", "network-fault", "add", address
        )
        try:
            self._helper(
                handle,
                handle.consumer,
                "network-trigger-backed",
                "write-pattern",
                "network-backed",
                "256",
                timeout=30,
            )
            self._helper(
                handle,
                handle.consumer,
                "network-exclusion",
                "wait-exclusion",
                "provider-0",
            )
            self._helper(
                handle,
                handle.consumer,
                "alert-network-disconnected",
                "assert-alert",
                "network-disconnected",
                "InfiniswapProviderDisconnected",
                "present",
            )
        finally:
            self._helper(
                handle,
                handle.consumer,
                "network-clear-backed",
                "network-fault",
                "clear",
                address,
                check=False,
            )
        self._helper(
            handle,
            handle.consumer,
            "network-read-backed",
            "read-pattern",
            "network-backed",
            "256",
        )
        self._helper(
            handle,
            handle.consumer,
            "status-network-backed",
            "snapshot",
            "network-backed",
        )
        self._stop_device(handle)
        self._reset_transports(handle)

        self._create_device(
            handle, "remote-only", provider_indices=(0,), capacity_gib=1
        )
        self._helper(
            handle,
            handle.consumer,
            "network-write-remote",
            "write-pattern",
            "network-remote",
            "0",
        )
        self._helper(
            handle, handle.consumer, "network-drop-remote", "network-fault", "add", address
        )
        try:
            self._helper(
                handle,
                handle.consumer,
                "network-timeout-remote",
                "wait-counter",
                "provider_timeouts_total",
                "1",
                "500",
            )
            self._helper(
                handle,
                handle.consumer,
                "network-remote-lost",
                "wait-field",
                "connection_state",
                "remote-lost",
                "100",
            )
            self._helper(
                handle,
                handle.consumer,
                "alert-network-deadline",
                "assert-alert",
                "network-deadline",
                "InfiniswapProviderDeadlineExpired",
                "present",
            )
            self._helper(
                handle,
                handle.consumer,
                "alert-network-remote-lost",
                "assert-alert",
                "network-remote-lost",
                "InfiniswapRemoteLost",
                "present",
            )
        finally:
            self._helper(
                handle,
                handle.consumer,
                "network-clear-remote",
                "network-fault",
                "clear",
                address,
                check=False,
            )
        self._helper(
            handle, handle.consumer, "network-io-failure", "expect-io-failure"
        )
        self._helper(
            handle,
            handle.consumer,
            "status-network-remote",
            "snapshot",
            "network-remote",
        )
        self._stop_device(handle)
        self._reset_transports(handle)
        self._create_device(handle, "backed", provider_indices=(0,), capacity_gib=1)
        for alert_name in (
            "InfiniswapProviderDisconnected",
            "InfiniswapProviderDeadlineExpired",
            "InfiniswapRemoteLost",
        ):
            self._helper(
                handle,
                handle.consumer,
                "alert-network-cleared-%s" % alert_name,
                "assert-alert",
                "network-cleared-%s" % alert_name,
                alert_name,
                "absent",
            )
        self._stop_device(handle)
        self._check_kernel(handle)
        return ScenarioResult(
            status="passed",
            detail="guest RXE interruption produced bounded Backed fallback and Remote-Lost",
        )

    def _scenario_backing_error(self, handle):
        self._helper(handle, handle.consumer, "backing-create", "backing-create")
        self._create_device(
            handle,
            "backed",
            provider_indices=(0,),
            capacity_gib=1,
            policy="remote-first",
            backing="/dev/mapper/infiniswap-vm-backing",
        )
        self._helper(handle, handle.consumer, "backing-heat", "heat-ranges", "1")
        self._helper(handle, handle.consumer, "backing-fail", "backing-fail")
        try:
            self._helper(
                handle,
                handle.consumer,
                "backing-write",
                "write-pattern",
                "backing-fault",
                "32",
                "no-flush",
                "1",
            )
            self._helper(
                handle,
                handle.consumer,
                "backing-degraded",
                "wait-field",
                "backing_state",
                "backing-degraded",
            )
            self._helper(
                handle,
                handle.consumer,
                "alert-backing-degraded",
                "assert-alert",
                "backing-degraded",
                "InfiniswapBackingDegraded",
                "present",
            )
            self._helper(
                handle,
                handle.consumer,
                "backing-read",
                "read-pattern",
                "backing-fault",
                "32",
                "1",
            )
            self._helper(
                handle,
                handle.consumer,
                "backing-reject",
                "expect-io-failure",
            )
            self._helper(
                handle,
                handle.consumer,
                "status-backing-error",
                "snapshot",
                "backing-error",
            )
        finally:
            self._helper(
                handle,
                handle.consumer,
                "backing-restore",
                "backing-restore",
                check=False,
            )
        self._stop_device(handle)
        self._helper(
            handle, handle.consumer, "backing-remove", "backing-remove"
        )
        self._create_device(
            handle, "backed", provider_indices=(0,), capacity_gib=1
        )
        self._helper(
            handle,
            handle.consumer,
            "alert-backing-cleared",
            "assert-alert",
            "backing-cleared",
            "InfiniswapBackingDegraded",
            "absent",
        )
        self._stop_device(handle)
        self._check_kernel(handle)
        return ScenarioResult(
            status="passed",
            detail="Backing Store error entered Backing-Degraded without corrupting remote data",
        )

    def _scenario_guest_reboot(self, handle):
        self._create_device(handle, "backed", provider_indices=(0,), capacity_gib=1)
        self._helper(
            handle,
            handle.consumer,
            "reboot-write",
            "write-pattern",
            "reboot",
            "4096",
        )
        self._helper(
            handle, handle.consumer, "status-before-reboot", "snapshot", "before-reboot"
        )
        self._check_kernel(handle)
        boot_id = self._ssh_shell(
            handle,
            handle.consumer,
            "boot-id-before-reboot",
            "cat /proc/sys/kernel/random/boot_id",
        ).stdout.strip()
        if not boot_id:
            raise BoundaryError("guest boot ID is empty before reboot")
        try:
            self._ssh_shell(
                handle,
                handle.consumer,
                "force-reboot",
                "sync; systemctl reboot --force --force",
                sudo=True,
                timeout=15,
                check=False,
            )
        except subprocess.TimeoutExpired:
            # A force reboot can stop sshd before SSH observes the disconnect.
            pass
        self._wait_for_reboot(handle, handle.consumer, boot_id)
        self._reset_provider_guests(handle)
        for link in handle.consumer.links:
            self._helper(
                handle,
                handle.consumer,
                "rxe-reboot-%d" % link["index"],
                "setup-rxe",
                str(link["consumer_rail"]),
                str(link["consumer_mac"]),
                "%s/%s" % (link["consumer_ip"], link["prefix"]),
            )
        self._helper(handle, handle.consumer, "kernel-baseline-reboot", "mark-kernel")
        self._create_device(handle, "backed", provider_indices=(0,), capacity_gib=1)
        self._helper(
            handle,
            handle.consumer,
            "reboot-read",
            "read-pattern",
            "reboot",
            "4096",
        )
        self._helper(
            handle, handle.consumer, "status-after-reboot", "snapshot", "after-reboot"
        )
        self._stop_device(handle)
        self._check_kernel(handle)
        return ScenarioResult(
            status="passed",
            detail="Consumer guest reboot preserved Strict Backed data after reconfiguration",
        )

    def _scenario_module_reload(self, handle):
        self._helper(
            handle, handle.consumer, "safe-module-reload", "safe-module-reload"
        )
        self._check_kernel(handle)
        return ScenarioResult(
            status="passed",
            detail="empty Consumer module unloaded and reloaded safely",
        )

    def _scenario_soak(self, handle):
        seconds = max(0, int(self.args.soak_hours * 3600))
        backed_seconds = seconds // 2
        remote_only_seconds = seconds - backed_seconds
        self._create_device(handle, "backed")
        self._helper(handle, handle.consumer, "soak-heat-backed", "heat-ranges", "1")
        self._helper(
            handle,
            handle.consumer,
            "soak-backed",
            "soak",
            "backed",
            str(backed_seconds),
            timeout=backed_seconds + 900,
        )
        self._helper(
            handle, handle.consumer, "status-soak-backed", "snapshot", "soak-backed"
        )
        self._stop_device(handle)

        provider_count = len(handle.providers)
        self._create_device(handle, "remote-only", capacity_gib=provider_count)
        self._helper(
            handle,
            handle.consumer,
            "soak-heat-remote-only",
            "heat-ranges",
            str(provider_count),
        )
        self._helper(
            handle,
            handle.consumer,
            "soak-remote-only",
            "soak",
            "remote-only",
            str(remote_only_seconds),
            timeout=remote_only_seconds + 900,
        )
        self._helper(
            handle,
            handle.consumer,
            "status-soak-remote-only",
            "snapshot",
            "soak-remote-only",
        )
        self._stop_device(handle)
        self._check_kernel(handle)
        return ScenarioResult(
            status="passed",
            detail="Backed and Remote-Only verified fio soak completed for %.3f total hours"
            % self.args.soak_hours,
            artifacts=(
                "guest:consumer:soak-backed.json",
                "guest:consumer:soak-remote-only.json",
                "guest:consumer:fio-soak-*.json",
            ),
            metrics={
                "required_seconds": seconds,
                "backed_seconds": backed_seconds,
                "remote_only_seconds": remote_only_seconds,
            },
        )

    def _reset_transports(self, handle) -> None:
        for provider in handle.providers:
            self._helper(
                handle,
                provider,
                "provider-stop-reset-%s" % provider.name,
                "provider-stop",
                check=False,
            )
        self._helper(
            handle,
            handle.consumer,
            "module-reset",
            "safe-module-reload",
            check=False,
        )
        self._configure_links(handle)
        for index in range(len(handle.providers)):
            self._start_provider(handle, index)
        time.sleep(1)

    def _reset_provider_guests(self, handle) -> None:
        for provider in handle.providers:
            self._helper(
                handle,
                provider,
                "provider-stop-reboot-%s" % provider.name,
                "provider-stop",
                check=False,
            )
            link = provider.links[0]
            self._helper(
                handle,
                provider,
                "provider-rxe-reboot-%s" % provider.name,
                "setup-rxe",
                str(link["provider_rail"]),
                str(link["provider_mac"]),
                "%s/%s" % (link["provider_ip"], link["prefix"]),
            )
        for index in range(len(handle.providers)):
            self._start_provider(handle, index)

    def _wait_for_reboot(self, handle, guest, previous_boot_id: str) -> None:
        deadline = time.monotonic() + 180
        while time.monotonic() < deadline:
            try:
                result = self._ssh_shell(
                    handle,
                    guest,
                    "reboot-wait",
                    "cat /proc/sys/kernel/random/boot_id",
                    timeout=8,
                    check=False,
                )
            except subprocess.TimeoutExpired:
                result = None
            if (
                result is not None
                and result.returncode == 0
                and result.stdout.strip()
                and result.stdout.strip() != previous_boot_id
            ):
                return
            time.sleep(2)
        raise BoundaryError("guest reboot did not complete")

    def _download_guest_artifacts(self, handle, guest, destination: Path) -> None:
        command = self._ssh_base(handle, guest) + [
            "sudo tar -C /var/tmp -czf - infiniswap-vm-artifacts"
        ]
        error_path = destination.with_suffix(".stderr")
        with destination.open("wb") as output, error_path.open("wb") as error:
            result = subprocess.run(
                command,
                stdin=subprocess.DEVNULL,
                stdout=output,
                stderr=error,
                timeout=300,
                check=False,
            )
        if result.returncode != 0:
            raise BoundaryError(
                "artifact download from %s failed: %s"
                % (
                    guest.name,
                    error_path.read_text(encoding="utf-8", errors="replace")[-2000:],
                )
            )
        error_path.unlink(missing_ok=True)

    def _scp_to_guest(self, handle, guest, source: Path, destination: str) -> None:
        command = [
            "scp",
            "-q",
            "-i",
            str(handle.key_path),
            "-P",
            str(guest.ssh_port),
            "-o",
            "BatchMode=yes",
            "-o",
            "StrictHostKeyChecking=no",
            "-o",
            "UserKnownHostsFile=/dev/null",
            str(source),
            "infiniswap@127.0.0.1:%s" % destination,
        ]
        self._run(command, timeout=300)

    def _helper(
        self,
        handle,
        guest,
        label,
        *arguments,
        timeout=120,
        check=True,
    ):
        command = ["bash", "/opt/infiniswap/tests/vm/guest.sh"] + list(arguments)
        return self._ssh(
            handle,
            guest,
            label,
            command,
            sudo=True,
            timeout=timeout,
            check=check,
        )

    def _ssh_base(self, handle, guest):
        return [
            "ssh",
            "-i",
            str(handle.key_path),
            "-p",
            str(guest.ssh_port),
            "-o",
            "BatchMode=yes",
            "-o",
            "ConnectTimeout=5",
            "-o",
            "ServerAliveInterval=15",
            "-o",
            "ServerAliveCountMax=20",
            "-o",
            "StrictHostKeyChecking=no",
            "-o",
            "UserKnownHostsFile=/dev/null",
            "infiniswap@127.0.0.1",
        ]

    def _ssh(
        self,
        handle,
        guest,
        label,
        command: Sequence[str],
        sudo=False,
        timeout=120,
        check=True,
        input_text=None,
    ):
        remote = shlex.join(command)
        if sudo:
            remote = "sudo -- " + remote
        return self._logged_run(
            handle,
            label,
            self._ssh_base(handle, guest) + [remote],
            timeout=timeout,
            check=check,
            input_text=input_text,
        )

    def _ssh_shell(
        self,
        handle,
        guest,
        label,
        script,
        sudo=False,
        timeout=120,
        check=True,
        input_text=None,
    ):
        command = ["bash", "-lc", script]
        return self._ssh(
            handle,
            guest,
            label,
            command,
            sudo=sudo,
            timeout=timeout,
            check=check,
            input_text=input_text,
        )

    def _logged_run(
        self,
        handle,
        label,
        command,
        timeout,
        check=True,
        input_text=None,
    ):
        handle.command_index += 1
        safe_label = "".join(
            character if character.isalnum() or character in "-_" else "-"
            for character in label
        )
        log = handle.case_dir / (
            "command-%03d-%s.log" % (handle.command_index, safe_label)
        )
        result = self._run(
            command,
            timeout=timeout,
            input_text=input_text,
            check=False,
        )
        log.write_text(
            "$ %s\n--- stdout ---\n%s\n--- stderr ---\n%s\n--- status ---\n%d\n"
            % (
                shlex.join(command),
                result.stdout,
                result.stderr,
                result.returncode,
            ),
            encoding="utf-8",
        )
        if check and result.returncode != 0:
            raise BoundaryError(
                "%s failed with status %d: %s"
                % (label, result.returncode, result.stderr[-2000:])
            )
        return result

    @staticmethod
    def _run(
        command,
        timeout,
        cwd=None,
        input_text=None,
        check=True,
        text=True,
    ):
        result = subprocess.run(
            command,
            cwd=cwd,
            input=input_text,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
            check=False,
            text=text,
        )
        if check and result.returncode != 0:
            stderr = result.stderr if text else result.stderr.decode("utf-8", "replace")
            raise BoundaryError(
                "%s failed with status %d: %s"
                % (shlex.join(command), result.returncode, stderr[-2000:])
            )
        return result

    @staticmethod
    def _free_port() -> int:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
            listener.bind(("127.0.0.1", 0))
            return listener.getsockname()[1]

    def _force_processes(self, handle) -> None:
        for guest in handle.guests:
            process = guest.process
            if process is None or process.poll() is not None:
                continue
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                continue
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if all(
                guest.process is None or guest.process.poll() is not None
                for guest in handle.guests
            ):
                return
            time.sleep(0.2)
        for guest in handle.guests:
            process = guest.process
            if process is None or process.poll() is not None:
                continue
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        for guest in handle.guests:
            if guest.process is not None:
                try:
                    guest.process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    pass

    def _force_cleanup(self, handle) -> None:
        self._force_processes(handle)
        shutil.rmtree(handle.qmp_dir, ignore_errors=True)
        shutil.rmtree(handle.work, ignore_errors=True)
