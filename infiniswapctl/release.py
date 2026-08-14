"""Release, upgrade, and configuration migration contracts."""

import copy
import hashlib
import json
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Mapping, Optional, Tuple

from .config import (
    HOT_RANGE_READ_WEIGHT_DEFAULT,
    HOT_RANGE_THRESHOLD_DEFAULT,
    HOT_RANGE_WRITE_WEIGHT_DEFAULT,
)


class ReleaseError(ValueError):
    """Release input is incomplete, unsafe, or outside the support policy."""


@dataclass(frozen=True)
class DrainCapacityAssessment:
    target_swap_used_bytes: int
    memory_available_bytes: int
    alternate_swap_free_bytes: int
    reserve_bytes: int
    absorbable_bytes: int
    shortfall_bytes: int
    sufficient: bool


@dataclass(frozen=True)
class KernelAbiEvidence:
    commit: str
    ubuntu_release: str
    kernel_release: str
    rdma_stack: str


def _nonnegative_integer(value: Any, field: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise ReleaseError(field + " must be a non-negative integer")
    return value


def assess_drain_capacity(
    *,
    target_swap_used_bytes: int,
    memory_available_bytes: int,
    alternate_swap_free_bytes: int,
    reserve_bytes: int,
) -> DrainCapacityAssessment:
    """Determine whether swapoff can preserve an explicit host-memory reserve."""

    used = _nonnegative_integer(target_swap_used_bytes, "target swap usage")
    memory = _nonnegative_integer(memory_available_bytes, "available memory")
    alternate = _nonnegative_integer(
        alternate_swap_free_bytes, "alternate swap capacity"
    )
    reserve = _nonnegative_integer(reserve_bytes, "host memory reserve")
    absorbable = max(0, memory - reserve) + alternate
    shortfall = max(0, used - absorbable)
    return DrainCapacityAssessment(
        target_swap_used_bytes=used,
        memory_available_bytes=memory,
        alternate_swap_free_bytes=alternate,
        reserve_bytes=reserve,
        absorbable_bytes=absorbable,
        shortfall_bytes=shortfall,
        sufficient=shortfall == 0,
    )


def migrate_config_document(kind: str, document: Mapping[str, Any]) -> Dict[str, Any]:
    """Migrate one previous configuration contract to the current contract."""

    if kind not in ("consumer", "provider", "provider-directory"):
        raise ReleaseError("unknown configuration kind: " + kind)
    if not isinstance(document, dict):
        raise ReleaseError("configuration must be a JSON object")
    version = document.get("schema_version")
    supported = (1, 2) if kind == "consumer" else (1,)
    current = 3 if kind == "consumer" else 2
    if version == current:
        raise ReleaseError(
            "%s configuration is already schema version %d" % (kind, current)
        )
    if version not in supported:
        raise ReleaseError(
            "%s schema version must be one of %s for migration"
            % (kind, ", ".join(str(item) for item in supported))
        )

    migrated = copy.deepcopy(document)
    migrated["schema_version"] = current
    if kind != "consumer":
        return migrated

    identity = migrated.get("identity")
    device = migrated.get("device")
    if not isinstance(identity, dict) or not isinstance(device, dict):
        raise ReleaseError("consumer identity and device must be JSON objects")
    identity["remote_only_eligible"] = False
    if version == 1 and device.get("mode") == "backed":
        device["hot_range"] = {
            "mapping_threshold": HOT_RANGE_THRESHOLD_DEFAULT,
            "read_weight": HOT_RANGE_READ_WEIGHT_DEFAULT,
            "write_weight": HOT_RANGE_WRITE_WEIGHT_DEFAULT,
        }
    return migrated


def _required_string(document: Mapping[str, Any], field: str) -> str:
    value = document.get(field)
    if not isinstance(value, str) or not value:
        raise ReleaseError(field + " must be a non-empty string")
    return value


def _verify_report(
    evidence: Mapping[str, Any], field: str, evidence_root: Path
) -> Tuple[Mapping[str, Any], Mapping[str, Any]]:
    report = evidence.get(field)
    if not isinstance(report, dict) or report.get("passed") is not True:
        raise ReleaseError(field + " evidence must report passed=true")
    path_value = _required_string(report, "path")
    digest = _required_string(report, "sha256")
    if not re.fullmatch(r"[0-9a-f]{64}", digest):
        raise ReleaseError(field + " evidence sha256 must be lowercase hexadecimal")
    path = Path(path_value)
    if not path.is_absolute():
        path = evidence_root / path
    try:
        payload = path.read_bytes()
        actual = hashlib.sha256(payload).hexdigest()
        document = json.loads(payload.decode("utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ReleaseError("could not read %s evidence: %s" % (field, exc)) from exc
    if actual != digest:
        raise ReleaseError(field + " evidence checksum does not match")
    if not isinstance(document, dict):
        raise ReleaseError(field + " evidence report must be a JSON object")
    return report, document


def _verify_report_identity(
    report: Mapping[str, Any],
    field: str,
    *,
    commit: str,
    ubuntu_release: str,
    kernel_release: str,
    rdma_stack: str,
) -> None:
    if (
        report.get("schema_version") != 1
        or report.get("status") != "passed"
        or report.get("source_commit") != commit
        or report.get("ubuntu_release") != ubuntu_release
        or report.get("kernel_release") != kernel_release
        or report.get("rdma_stack") != rdma_stack
    ):
        raise ReleaseError(field + " report identity or result does not match the gate")


def _verify_vm_report(
    entry: Mapping[str, Any],
    report: Mapping[str, Any],
    *,
    commit: str,
    ubuntu_release: str,
    kernel_release: str,
) -> None:
    profile = report.get("profile")
    matrix = report.get("matrix")
    safety = report.get("safety")
    cleanup = report.get("cleanup")
    kernel_line = ".".join(kernel_release.split(".")[:2])
    matching_entries = (
        [
            item
            for item in matrix
            if isinstance(item, dict)
            and item.get("ubuntu") == ubuntu_release
            and item.get("kernel") == kernel_line
            and item.get("kernel_release") == kernel_release
            and item.get("rdma_stack") == "inbox"
        ]
        if isinstance(matrix, list)
        else []
    )
    if (
        entry.get("certifiable") is not True
        or report.get("schema_version") != 1
        or report.get("source_commit") != commit
        or report.get("status") != "passed"
        or not isinstance(profile, dict)
        or profile.get("certifiable") is not True
        or not matching_entries
        or any(item.get("status") != "passed" for item in matching_entries)
        or not isinstance(safety, dict)
        or safety.get("status") != "passed"
        or not isinstance(cleanup, dict)
        or cleanup.get("status") != "passed"
    ):
        raise ReleaseError("vm report is not a matching certifiable regression")


def verify_kernel_abi_evidence(
    evidence: Mapping[str, Any],
    *,
    expected_commit: str,
    evidence_root: Optional[Path] = None,
) -> KernelAbiEvidence:
    """Verify the evidence required to admit a new supported kernel ABI."""

    if not isinstance(evidence, dict) or evidence.get("schema_version") != 1:
        raise ReleaseError("kernel ABI evidence must use schema version 1")
    evidence_root = evidence_root or Path.cwd()
    commit = _required_string(evidence, "commit")
    if not re.fullmatch(r"[0-9a-f]{40}", commit) or commit != expected_commit:
        raise ReleaseError("kernel ABI evidence commit does not match the release")
    ubuntu = _required_string(evidence, "ubuntu_release")
    kernel = _required_string(evidence, "kernel_release")
    rdma_stack = _required_string(evidence, "rdma_stack")
    supported_tuple = (
        ubuntu == "22.04"
        and kernel.startswith("5.15.")
        and rdma_stack in ("inbox", "mlnx-ofed-5.8")
    ) or (
        ubuntu == "24.04"
        and kernel.startswith("6.8.")
        and rdma_stack == "inbox"
    )
    if not supported_tuple:
        raise ReleaseError("kernel ABI evidence names an unsupported platform tuple")

    _, dkms_report = _verify_report(evidence, "dkms", evidence_root)
    _verify_report_identity(
        dkms_report,
        "dkms",
        commit=commit,
        ubuntu_release=ubuntu,
        kernel_release=kernel,
        rdma_stack=rdma_stack,
    )
    if dkms_report.get("kind") != "infiniswap.dkms-report":
        raise ReleaseError("dkms report has the wrong kind")

    vm_entry, vm_report = _verify_report(evidence, "vm", evidence_root)
    _verify_vm_report(
        vm_entry,
        vm_report,
        commit=commit,
        ubuntu_release=ubuntu,
        kernel_release=kernel,
    )

    canary_entry, canary_report = _verify_report(
        evidence, "canary", evidence_root
    )
    _verify_report_identity(
        canary_report,
        "canary",
        commit=commit,
        ubuntu_release=ubuntu,
        kernel_release=kernel,
        rdma_stack=rdma_stack,
    )
    duration = canary_entry.get("duration_seconds")
    if (
        canary_report.get("kind") != "infiniswap.canary-report"
        or isinstance(duration, bool)
        or not isinstance(duration, int)
        or duration <= 0
        or canary_report.get("duration_seconds") != duration
    ):
        raise ReleaseError("canary report has invalid or mismatched duration")
    return KernelAbiEvidence(
        commit=commit,
        ubuntu_release=ubuntu,
        kernel_release=kernel,
        rdma_stack=rdma_stack,
    )
