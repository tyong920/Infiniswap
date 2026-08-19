#!/usr/bin/env python3
"""Compare non-certifiable Soft-RoCE fio evidence reports."""

import argparse
import json
import math
import re
import statistics
import sys
from pathlib import Path
from typing import Dict, List, Sequence, TextIO, Tuple

IOPS_REGRESSION_PERCENT = 5.0
THROUGHPUT_REGRESSION_PERCENT = 5.0
P99_LATENCY_REGRESSION_PERCENT = 10.0
SYSTEM_CPU_GROWTH_PERCENT = 10.0
BASELINE_IOPS_CV_PERCENT = 5.0
BASELINE_THROUGHPUT_CV_PERCENT = 5.0
BASELINE_P99_CV_PERCENT = 10.0
REQUIRED_MEASURED_SAMPLES = 5
REQUIRED_WARMUP_SAMPLES = 1
REQUIRED_SAMPLE_SECONDS = 60
SOURCE_COMMIT = re.compile(r"^[0-9a-f]{40}$")
FLOAT_TOLERANCE = 1e-9
MODES = ("backed", "remote-only")
EVIDENCE_FIELDS = (
    "provider_timeouts_total",
    "late_rdma_completions_total",
    "io_errors_total",
    "inflight_io",
    "oldest_inflight_ms",
)
EVIDENCE_FAILURE_COUNTERS = (
    "provider_timeouts_total",
    "late_rdma_completions_total",
    "inflight_io",
    "oldest_inflight_ms",
)
ENVIRONMENT_FIELDS = (
    "cloud_image_sha256",
    "host_identity",
    "ubuntu_release",
    "kernel_release",
    "rdma_stack",
    "topology",
    "vm_resources",
    "guests",
    "duration_seconds",
    "fio_arguments",
    "fio_verification_arguments",
)


class ComparisonInputError(ValueError):
    pass


def _load_json(path: Path):
    try:
        with path.open(encoding="utf-8") as source:
            return json.load(source)
    except (OSError, json.JSONDecodeError) as error:
        raise ComparisonInputError("%s: %s" % (path, error)) from error


def _artifact_path(report_path: Path, artifact: str) -> Path:
    path = Path(artifact)
    if path.is_absolute() and path.exists():
        return path
    candidate = report_path.parent / path
    if candidate.exists():
        return candidate
    if path.is_absolute():
        candidate = report_path.parent / path.name
        if candidate.exists():
            return candidate
    raise ComparisonInputError("sample artifact is missing: %s" % artifact)


def _percentile_ns(direction: Dict[str, object]) -> float:
    for key, scale in (
        ("clat_ns", 1.0),
        ("lat_ns", 1.0),
        ("clat_us", 1000.0),
        ("lat_us", 1000.0),
        ("clat_ms", 1000000.0),
        ("lat_ms", 1000000.0),
    ):
        latency = direction.get(key)
        if not isinstance(latency, dict):
            continue
        percentiles = latency.get("percentile")
        if not isinstance(percentiles, dict) or not percentiles:
            continue
        try:
            percentile_key = min(
                percentiles, key=lambda value: abs(float(value) - 99.0)
            )
            return float(percentiles[percentile_key]) * scale
        except (TypeError, ValueError):
            continue
    raise ComparisonInputError("fio sample omits a numeric p99 completion latency")


def summarize_fio(raw_fio: Dict[str, object]) -> Dict[str, float]:
    jobs = raw_fio.get("jobs")
    if not isinstance(jobs, list) or not jobs:
        raise ComparisonInputError("fio sample has no jobs")
    iops = 0.0
    throughput = 0.0
    p99_values = []
    system_cpu = 0.0
    fio_errors = 0
    for job in jobs:
        if not isinstance(job, dict):
            raise ComparisonInputError("fio job is not an object")
        fio_errors += int(job.get("error", 0) != 0)
        try:
            system_cpu += float(job["sys_cpu"])
        except (KeyError, TypeError, ValueError) as error:
            raise ComparisonInputError("fio job omits system CPU") from error
        for operation in ("read", "write", "trim"):
            direction = job.get(operation, {})
            if not isinstance(direction, dict):
                continue
            try:
                direction_iops = float(direction.get("iops", 0.0))
                if "bw_bytes" in direction:
                    direction_throughput = float(direction["bw_bytes"])
                else:
                    direction_throughput = float(direction.get("bw", 0.0)) * 1024.0
            except (TypeError, ValueError) as error:
                raise ComparisonInputError("fio throughput is not numeric") from error
            iops += direction_iops
            throughput += direction_throughput
            if direction_iops > 0.0:
                p99_values.append(_percentile_ns(direction))
    if iops <= 0.0 or throughput <= 0.0 or not p99_values:
        raise ComparisonInputError("fio sample has no completed measured I/O")
    return {
        "iops": iops,
        "throughput_bytes_per_second": throughput,
        "p99_latency_ns": max(p99_values),
        "system_cpu_percent": system_cpu,
        "fio_error_jobs": fio_errors,
    }


def _normalized_fio_arguments(arguments, mode: str) -> List[str]:
    if not isinstance(arguments, list) or not all(
        isinstance(argument, str) for argument in arguments
    ):
        raise ComparisonInputError("fio arguments are malformed")
    required = ["--runtime=60", "--verify=crc32c", "--verify_fatal=1"]
    required.append("--do_verify=0" if mode == "remote-only" else "--do_verify=1")
    missing = [argument for argument in required if argument not in arguments]
    if missing:
        raise ComparisonInputError(
            "fio sample is not a verified 60-second workload: " + ", ".join(missing)
        )
    return [
        argument
        for argument in arguments
        if not argument.startswith("--name=") and not argument.startswith("--output=")
    ]


def _normalized_verification_arguments(arguments, mode: str) -> List[str]:
    if not isinstance(arguments, list) or not all(
        isinstance(argument, str) for argument in arguments
    ):
        raise ComparisonInputError("fio verification arguments are malformed")
    if mode == "backed":
        if arguments:
            raise ComparisonInputError(
                "Backed sample has an unexpected verification pass"
            )
        return []
    required = ("--rw=read", "--verify=crc32c", "--verify_only=1", "--verify_fatal=1")
    missing = [argument for argument in required if argument not in arguments]
    if missing:
        raise ComparisonInputError(
            "Remote-Only sample omits its verification pass: " + ", ".join(missing)
        )
    return [
        argument
        for argument in arguments
        if not argument.startswith("--name=") and not argument.startswith("--output=")
    ]


def _extract_case(
    report_path: Path,
    report: Dict[str, object],
    entry: Dict[str, object],
    case_index: int,
) -> Tuple[Dict[str, object], List[str]]:
    failures = []
    scenarios = entry.get("scenarios", ())
    if not isinstance(scenarios, list):
        raise ComparisonInputError(
            "matrix case %d scenarios are malformed" % case_index
        )
    by_id = {
        scenario.get("id"): scenario
        for scenario in scenarios
        if isinstance(scenario, dict)
    }
    performance = by_id.get("fio-verification")
    leak_check = by_id.get("resource-leak-check")
    if performance is None or leak_check is None:
        raise ComparisonInputError(
            "matrix case %d omits performance or cleanup evidence" % case_index
        )
    for name, item in (
        ("matrix", entry),
        ("fio-verification", performance),
        ("resource-leak-check", leak_check),
    ):
        if item.get("status") != "passed":
            failures.append("case %d %s status is not passed" % (case_index, name))
    leaks = leak_check.get("metrics", {}).get("leaks", [])
    if leaks:
        failures.append("case %d cleanup leakage is present" % case_index)

    samples = {mode: [] for mode in MODES}
    artifacts = performance.get("artifacts")
    if not isinstance(artifacts, list):
        raise ComparisonInputError(
            "matrix case %d sample artifacts are malformed" % case_index
        )
    for artifact in artifacts:
        if not isinstance(artifact, str) or not artifact.endswith(".json"):
            raise ComparisonInputError("performance sample artifact is malformed")
        sample = _load_json(_artifact_path(report_path, artifact))
        if sample.get("kind") != "infiniswap.soft-roce-fio-sample":
            raise ComparisonInputError(
                "artifact is not a Soft-RoCE fio sample: %s" % artifact
            )
        metadata = sample.get("metadata")
        if not isinstance(metadata, dict):
            raise ComparisonInputError("fio sample metadata is missing: %s" % artifact)
        mode = metadata.get("mode")
        if mode not in samples:
            raise ComparisonInputError("fio sample has an unsupported mode: %s" % mode)
        sample_commit = str(metadata.get("source_commit", ""))
        if not SOURCE_COMMIT.fullmatch(sample_commit):
            raise ComparisonInputError("fio sample source commit is invalid")
        if sample_commit != report.get("source_commit"):
            raise ComparisonInputError(
                "fio sample source commit does not match its report"
            )
        for field in ENVIRONMENT_FIELDS:
            if field not in metadata or metadata[field] in (None, ""):
                raise ComparisonInputError("fio sample omits %s" % field)
        if metadata["duration_seconds"] != REQUIRED_SAMPLE_SECONDS:
            raise ComparisonInputError("fio sample duration is not 60 seconds")
        _normalized_fio_arguments(metadata["fio_arguments"], mode)
        _normalized_verification_arguments(metadata["fio_verification_arguments"], mode)
        summary = summarize_fio(sample.get("fio", {}))
        if summary.pop("fio_error_jobs"):
            failures.append(
                "case %d %s fio workload reported an I/O error" % (case_index, mode)
            )
        verification = sample.get("verification")
        if mode == "remote-only":
            if not isinstance(verification, dict):
                raise ComparisonInputError(
                    "Remote-Only fio sample omits raw verification output"
                )
            verification_summary = summarize_fio(verification)
            if verification_summary.pop("fio_error_jobs"):
                failures.append(
                    "case %d %s fio verification reported an I/O error"
                    % (case_index, mode)
                )
        elif verification is not None:
            raise ComparisonInputError(
                "Backed fio sample has unexpected raw verification output"
            )
        samples[mode].append(
            {"artifact": artifact, "metadata": metadata, "summary": summary}
        )

    environments = {}
    measured = {}
    for mode in MODES:
        mode_samples = samples[mode]
        warmups = [
            sample
            for sample in mode_samples
            if sample["metadata"].get("phase") == "warmup"
        ]
        measurements = [
            sample
            for sample in mode_samples
            if sample["metadata"].get("phase") == "measured"
        ]
        if (
            len(warmups) != REQUIRED_WARMUP_SAMPLES
            or len(measurements) != REQUIRED_MEASURED_SAMPLES
        ):
            raise ComparisonInputError(
                "%s requires one warmup and five measured samples" % mode
            )
        expected_indices = list(range(1, REQUIRED_MEASURED_SAMPLES + 1))
        indices = sorted(
            sample["metadata"].get("sample_index") for sample in measurements
        )
        if (
            indices != expected_indices
            or warmups[0]["metadata"].get("sample_index") != 1
        ):
            raise ComparisonInputError("%s sample indices are incomplete" % mode)
        environment = {
            field: measurements[0]["metadata"][field] for field in ENVIRONMENT_FIELDS
        }
        environment["fio_arguments"] = _normalized_fio_arguments(
            environment["fio_arguments"], mode
        )
        environment["fio_verification_arguments"] = _normalized_verification_arguments(
            environment["fio_verification_arguments"], mode
        )
        for sample in mode_samples:
            actual = {field: sample["metadata"][field] for field in ENVIRONMENT_FIELDS}
            actual["fio_arguments"] = _normalized_fio_arguments(
                actual["fio_arguments"], mode
            )
            actual["fio_verification_arguments"] = _normalized_verification_arguments(
                actual["fio_verification_arguments"], mode
            )
            if actual != environment:
                raise ComparisonInputError(
                    "%s sample environment changes within one report" % mode
                )
        environments[mode] = environment
        measured[mode] = [sample["summary"] for sample in measurements]

    evidence = performance.get("metrics", {}).get("evidence")
    if not isinstance(evidence, dict):
        raise ComparisonInputError("performance safety evidence is missing")
    for mode in MODES:
        mode_evidence = evidence.get(mode)
        if not isinstance(mode_evidence, dict):
            raise ComparisonInputError("%s safety evidence is missing" % mode)
        if mode_evidence.get("status") != "passed":
            failures.append("case %d %s safety evidence failed" % (case_index, mode))
        for counter in EVIDENCE_FIELDS:
            try:
                value = int(mode_evidence[counter])
            except (KeyError, TypeError, ValueError) as error:
                raise ComparisonInputError(
                    "%s safety evidence omits %s" % (mode, counter)
                ) from error
            if counter in EVIDENCE_FAILURE_COUNTERS and value != 0:
                failures.append(
                    "case %d %s %s is %d" % (case_index, mode, counter, value)
                )
        try:
            errors_before_measured = int(mode_evidence["io_errors_before_measured"])
            errors_after_measured = int(mode_evidence["io_errors_total"])
        except (KeyError, TypeError, ValueError) as error:
            raise ComparisonInputError(
                "%s safety evidence omits measured I/O error bounds" % mode
            ) from error
        if errors_after_measured != errors_before_measured:
            failures.append(
                "case %d %s measured samples changed io_errors_total from %d to %d"
                % (
                    case_index,
                    mode,
                    errors_before_measured,
                    errors_after_measured,
                )
            )

    identity = environments[MODES[0]].copy()
    identity.pop("fio_arguments")
    case_id = "%s/%s/topology-%s" % (
        identity["ubuntu_release"],
        identity["kernel_release"],
        identity["topology"],
    )
    return {
        "id": case_id,
        "environments": environments,
        "samples": measured,
    }, failures


def _extract_report(report_path: Path) -> Tuple[Dict[str, object], List[str]]:
    report = _load_json(report_path)
    if not isinstance(report, dict):
        raise ComparisonInputError("VM report is not an object")
    source_commit = str(report.get("source_commit", ""))
    if not SOURCE_COMMIT.fullmatch(source_commit):
        raise ComparisonInputError("VM report source commit is invalid")
    if report.get("profile", {}).get("certifiable") is not False:
        raise ComparisonInputError(
            "performance report is not explicitly non-certifiable"
        )
    if report.get("certification", {}).get("status") != "non-certifiable":
        raise ComparisonInputError(
            "performance report omits its non-certifiable marker"
        )
    if report.get("selection", {}).get("scenario") != "fio-verification":
        raise ComparisonInputError("report is not a focused fio-verification run")

    failures = []
    if report.get("status") != "passed":
        failures.append("report status is not passed")
    for section in ("safety", "cleanup"):
        if report.get(section, {}).get("status") != "passed":
            failures.append("report %s status is not passed" % section)
    matrix = report.get("matrix")
    if not isinstance(matrix, list) or not matrix:
        raise ComparisonInputError("VM report matrix is empty")
    cases = {}
    for index, entry in enumerate(matrix):
        if not isinstance(entry, dict):
            raise ComparisonInputError("VM report matrix case is malformed")
        case, case_failures = _extract_case(report_path, report, entry, index)
        if case["id"] in cases:
            raise ComparisonInputError("VM report repeats environment %s" % case["id"])
        cases[case["id"]] = case
        failures.extend(case_failures)
    return {"source_commit": source_commit, "cases": cases}, failures


def _coefficient_of_variation(values: Sequence[float]) -> float:
    mean = statistics.fmean(values)
    if mean == 0.0:
        return 0.0 if all(value == 0.0 for value in values) else math.inf
    return statistics.pstdev(values) / mean * 100.0


def _increase_percent(baseline: float, candidate: float) -> float:
    if baseline == 0.0:
        return 0.0 if candidate == 0.0 else math.inf
    return (candidate - baseline) / baseline * 100.0


def _decrease_percent(baseline: float, candidate: float) -> float:
    if baseline == 0.0:
        return 0.0 if candidate == 0.0 else -math.inf
    return (baseline - candidate) / baseline * 100.0


def _compare_mode(
    case_id: str,
    mode: str,
    baseline: Sequence[Dict[str, float]],
    candidate: Sequence[Dict[str, float]],
) -> Tuple[Dict[str, object], List[str], List[str]]:
    invalid = []
    failed = []
    stability = {}
    for metric, limit in (
        ("iops", BASELINE_IOPS_CV_PERCENT),
        ("throughput_bytes_per_second", BASELINE_THROUGHPUT_CV_PERCENT),
        ("p99_latency_ns", BASELINE_P99_CV_PERCENT),
    ):
        cv = _coefficient_of_variation([sample[metric] for sample in baseline])
        stability[metric + "_cv_percent"] = cv
        if cv > limit + FLOAT_TOLERANCE:
            reason_name = {
                "iops": "iops",
                "throughput_bytes_per_second": "throughput",
                "p99_latency_ns": "p99_latency",
            }[metric]
            invalid.append(
                "%s %s baseline %s variation %.3f%% exceeds %.1f%%"
                % (case_id, mode, reason_name, cv, limit)
            )

    baseline_medians = {
        metric: statistics.median(sample[metric] for sample in baseline)
        for metric in baseline[0]
    }
    candidate_medians = {
        metric: statistics.median(sample[metric] for sample in candidate)
        for metric in candidate[0]
    }
    changes = {
        "iops_regression_percent": _decrease_percent(
            baseline_medians["iops"], candidate_medians["iops"]
        ),
        "throughput_regression_percent": _decrease_percent(
            baseline_medians["throughput_bytes_per_second"],
            candidate_medians["throughput_bytes_per_second"],
        ),
        "p99_latency_regression_percent": _increase_percent(
            baseline_medians["p99_latency_ns"],
            candidate_medians["p99_latency_ns"],
        ),
        "system_cpu_growth_percent": _increase_percent(
            baseline_medians["system_cpu_percent"],
            candidate_medians["system_cpu_percent"],
        ),
    }
    for key, limit, reason_name in (
        ("iops_regression_percent", IOPS_REGRESSION_PERCENT, "iops"),
        (
            "throughput_regression_percent",
            THROUGHPUT_REGRESSION_PERCENT,
            "throughput",
        ),
        (
            "p99_latency_regression_percent",
            P99_LATENCY_REGRESSION_PERCENT,
            "p99_latency",
        ),
    ):
        if changes[key] > limit + FLOAT_TOLERANCE:
            failed.append(
                "%s %s %s regression %.3f%% exceeds %.1f%%"
                % (case_id, mode, reason_name, changes[key], limit)
            )

    cpu_limit = baseline_medians["system_cpu_percent"] * (
        1.0 + SYSTEM_CPU_GROWTH_PERCENT / 100.0
    )
    reproduced = sum(sample["system_cpu_percent"] > cpu_limit for sample in candidate)
    changes["system_cpu_reproduced_samples"] = reproduced
    if (
        changes["system_cpu_growth_percent"]
        > SYSTEM_CPU_GROWTH_PERCENT + FLOAT_TOLERANCE
        and reproduced >= 3
    ):
        failed.append(
            "%s %s system_cpu growth %.3f%% exceeds %.1f%% in %d samples"
            % (
                case_id,
                mode,
                changes["system_cpu_growth_percent"],
                SYSTEM_CPU_GROWTH_PERCENT,
                reproduced,
            )
        )
    result = {
        "status": "invalid" if invalid else "failed" if failed else "passed",
        "baseline_stability": stability,
        "baseline_medians": baseline_medians,
        "candidate_medians": candidate_medians,
        "changes": changes,
    }
    return result, invalid, failed


def compare_reports(baseline_path: Path, candidate_path: Path) -> Dict[str, object]:
    baseline_path = Path(baseline_path)
    candidate_path = Path(candidate_path)
    result = {
        "schema_version": 1,
        "kind": "infiniswap.soft-roce-comparison",
        "certifiable": False,
        "thresholds": {
            "baseline_iops_cv_percent": BASELINE_IOPS_CV_PERCENT,
            "baseline_throughput_cv_percent": BASELINE_THROUGHPUT_CV_PERCENT,
            "baseline_p99_latency_cv_percent": BASELINE_P99_CV_PERCENT,
            "iops_regression_percent": IOPS_REGRESSION_PERCENT,
            "throughput_regression_percent": THROUGHPUT_REGRESSION_PERCENT,
            "p99_latency_regression_percent": P99_LATENCY_REGRESSION_PERCENT,
            "system_cpu_growth_percent": SYSTEM_CPU_GROWTH_PERCENT,
            "system_cpu_reproduction_samples": 3,
        },
        "baseline": {"report": str(baseline_path)},
        "candidate": {"report": str(candidate_path)},
        "cases": [],
        "reasons": [],
    }
    try:
        baseline, baseline_failures = _extract_report(baseline_path)
        candidate, candidate_failures = _extract_report(candidate_path)
    except ComparisonInputError as error:
        try:
            candidate_document = _load_json(candidate_path)
        except ComparisonInputError:
            candidate_document = {}
        candidate_failed = isinstance(candidate_document, dict) and (
            candidate_document.get("status") == "failed"
            or candidate_document.get("safety", {}).get("status") == "failed"
            or candidate_document.get("cleanup", {}).get("status") == "failed"
        )
        result["status"] = "failed" if candidate_failed else "invalid"
        result["reasons"] = [str(error)]
        return result

    result["baseline"]["source_commit"] = baseline["source_commit"]
    result["candidate"]["source_commit"] = candidate["source_commit"]
    failures = ["baseline " + reason for reason in baseline_failures]
    failures.extend("candidate " + reason for reason in candidate_failures)
    invalid = []
    baseline_cases = baseline["cases"]
    candidate_cases = candidate["cases"]
    if set(baseline_cases) != set(candidate_cases):
        invalid.append("baseline and candidate matrix environments differ")
    for case_id in sorted(set(baseline_cases) & set(candidate_cases)):
        baseline_case = baseline_cases[case_id]
        candidate_case = candidate_cases[case_id]
        case_result = {"id": case_id, "status": "passed", "modes": {}}
        for mode in MODES:
            baseline_environment = baseline_case["environments"][mode]
            candidate_environment = candidate_case["environments"][mode]
            if baseline_environment != candidate_environment:
                fields = [
                    field
                    for field in ENVIRONMENT_FIELDS
                    if baseline_environment.get(field)
                    != candidate_environment.get(field)
                ]
                invalid.append(
                    "%s %s environment mismatch: %s"
                    % (case_id, mode, ", ".join(fields))
                )
                continue
            mode_result, mode_invalid, mode_failures = _compare_mode(
                case_id,
                mode,
                baseline_case["samples"][mode],
                candidate_case["samples"][mode],
            )
            case_result["modes"][mode] = mode_result
            invalid.extend(mode_invalid)
            failures.extend(mode_failures)
        if any(mode["status"] == "invalid" for mode in case_result["modes"].values()):
            case_result["status"] = "invalid"
        elif any(mode["status"] == "failed" for mode in case_result["modes"].values()):
            case_result["status"] = "failed"
        result["cases"].append(case_result)

    if len(result["cases"]) == 1:
        result["modes"] = result["cases"][0]["modes"]
    if invalid:
        result["status"] = "invalid"
        result["reasons"] = invalid + failures
    elif failures:
        result["status"] = "failed"
        result["reasons"] = failures
    else:
        result["status"] = "passed"
    return result


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Compare focused non-certifiable Soft-RoCE fio reports"
    )
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--output", type=Path)
    return parser


def main(
    argv: Sequence[str] = None,
    *,
    stdout: TextIO = sys.stdout,
    stderr: TextIO = sys.stderr,
) -> int:
    del stderr
    args = _parser().parse_args(argv)
    comparison = compare_reports(args.baseline, args.candidate)
    payload = json.dumps(comparison, indent=2, sort_keys=True) + "\n"
    stdout.write(payload)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        temporary = args.output.with_suffix(args.output.suffix + ".tmp")
        temporary.write_text(payload, encoding="utf-8")
        temporary.replace(args.output)
    return {"passed": 0, "failed": 1, "invalid": 2}[comparison["status"]]


if __name__ == "__main__":
    raise SystemExit(main())
