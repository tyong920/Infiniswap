"""Stable observability contracts shared by the administration commands."""

from typing import Any, Dict, Iterable, List, Tuple

METRICS_SCHEMA_VERSION = 1


def _alert(
    name: str,
    severity: str,
    summary: str,
    runbook: str,
    recovery: str,
) -> Dict[str, str]:
    return {
        "name": name,
        "severity": severity,
        "summary": summary,
        "runbook": "docs/runbooks/" + runbook,
        "recovery": recovery,
    }


def evaluate_consumer_alerts(status: Dict[str, Any]) -> List[Dict[str, str]]:
    """Return active baseline alerts for one Consumer status document."""
    device = status["device"]
    connection = status["connection"]
    io_status = status["io"]
    metrics = status["metrics"]
    alerts: List[Dict[str, str]] = []
    disconnected = device["lifecycle"] == "active" and any(
        provider["state"] != "connected" for provider in connection["providers"]
    )

    if disconnected:
        alerts.append(
            _alert(
                "InfiniswapProviderDisconnected",
                "critical" if device["mode"] == "remote-only" else "warning",
                "One or more configured Memory Providers are disconnected.",
                "provider-disconnected.md",
                "clear after every Provider reconnects or the device is recreated",
            )
        )
    if metrics["provider_timeouts_total"] and disconnected:
        alerts.append(
            _alert(
                "InfiniswapProviderDeadlineExpired",
                "critical",
                "A Provider Failure Deadline expired while a Provider is unavailable.",
                "provider-deadline-expired.md",
                "clear after Provider recovery; the event-rate alert clears after 5 minutes",
            )
        )
    if device["operational_state"] == "backing-degraded":
        alerts.append(
            _alert(
                "InfiniswapBackingDegraded",
                "critical",
                "The Backing Store can no longer preserve the recovery copy.",
                "backing-degraded.md",
                "clear only after the backed device is restored and recreated",
            )
        )
    if device["operational_state"] == "remote-lost":
        alerts.append(
            _alert(
                "InfiniswapRemoteLost",
                "critical",
                "A Remote-Only Mode device has lost Remote Memory and rejects all I/O.",
                "remote-lost.md",
                "clear only after the device is stopped and recreated",
            )
        )
    if metrics["admission_rejections_total"] and connection["healthy_providers"] == 0:
        alerts.append(
            _alert(
                "InfiniswapAdmissionRejected",
                "warning",
                "A Memory Provider rejected Remote Chunk admission.",
                "admission-rejected.md",
                "clear after capacity is restored and a Provider reconnects",
            )
        )
    if metrics["authentication_failures_total"] and connection["healthy_providers"] == 0:
        alerts.append(
            _alert(
                "InfiniswapAuthenticationFailure",
                "critical",
                "A Memory Provider rejected Consumer authentication.",
                "authentication-failure.md",
                "clear after credentials are corrected and a Provider reconnects",
            )
        )
    if io_status["errors_total"]:
        alerts.append(
            _alert(
                "InfiniswapIOErrors",
                "critical",
                "The Infiniswap Device completed block I/O with errors.",
                "io-errors.md",
                "clear after the event-rate window or device recreation",
            )
        )
    if (
        io_status["in_flight"] > 0
        and io_status["oldest_in_flight_ms"]
        > 2 * device["provider_failure_deadline_ms"]
    ):
        alerts.append(
            _alert(
                "InfiniswapHungRequests",
                "critical",
                "Block I/O remains in flight beyond twice the Provider Failure Deadline.",
                "hung-requests.md",
                "clear when requests complete or after the device is safely recreated",
            )
        )
    return alerts


def _escape_label(value: str) -> str:
    return value.replace("\\", "\\\\").replace("\n", "\\n").replace('"', '\\"')


def _labels(values: Iterable[Tuple[str, str]]) -> str:
    encoded = [
        '%s="%s"' % (name, _escape_label(value)) for name, value in values
    ]
    return "{" + ",".join(encoded) + "}"


def render_consumer_metrics(status: Dict[str, Any]) -> str:
    """Render one Consumer status document as OpenMetrics 1.0 text."""
    device = status["device"]
    connection = status["connection"]
    capacity = status["capacity"]
    mapping = status["mapping"]
    io_status = status["io"]
    metric_values = status["metrics"]
    device_label = (("device", device["name"]),)
    lines: List[str] = [
        "# infiniswap_metrics_schema_version %d" % METRICS_SCHEMA_VERSION,
        "# HELP infiniswap_consumer_info Static Consumer mode and policy information.",
        "# TYPE infiniswap_consumer_info gauge",
        "infiniswap_consumer_info%s 1"
        % _labels(
            device_label
            + (
                ("mode", device["mode"]),
                ("policy", device["acknowledgement_policy"]),
            )
        ),
        "# HELP infiniswap_consumer_operational_state Current Consumer operational state.",
        "# TYPE infiniswap_consumer_operational_state gauge",
    ]
    for state in ("healthy", "backing-degraded", "remote-lost"):
        lines.append(
            "infiniswap_consumer_operational_state%s %d"
            % (
                _labels(device_label + (("state", state),)),
                int(device["operational_state"] == state),
            )
        )

    lines.extend(
        (
            "# HELP infiniswap_consumer_provider_connected Whether a configured Provider is connected.",
            "# TYPE infiniswap_consumer_provider_connected gauge",
            "# TYPE infiniswap_consumer_provider_available_chunks gauge",
            "# TYPE infiniswap_consumer_provider_mapped_chunks gauge",
            "# TYPE infiniswap_consumer_provider_last_error_errno gauge",
        )
    )
    for provider in connection["providers"]:
        provider_labels = _labels(
            device_label + (("provider", provider["provider_id"]),)
        )
        lines.append(
            "infiniswap_consumer_provider_connected%s %d"
            % (
                provider_labels,
                int(provider["state"] == "connected"),
            )
        )
        lines.append(
            "infiniswap_consumer_provider_available_chunks%s %d"
            % (provider_labels, provider["available_chunks"])
        )
        lines.append(
            "infiniswap_consumer_provider_mapped_chunks%s %d"
            % (provider_labels, provider["mapped_chunks"])
        )
        lines.append(
            "infiniswap_consumer_provider_last_error_errno%s %d"
            % (
                provider_labels,
                provider["last_error"]["errno"]
                if provider["last_error"]
                else 0,
            )
        )

    gauges = (
        (
            "infiniswap_consumer_active",
            int(device["lifecycle"] == "active"),
        ),
        ("infiniswap_consumer_advertised_bytes", capacity["advertised_bytes"]),
        ("infiniswap_consumer_backing_bytes", capacity["backing_bytes"]),
        ("infiniswap_consumer_remote_bytes", capacity["remote_bytes"]),
        (
            "infiniswap_consumer_mapped_remote_chunks",
            mapping["mapped_remote_chunks"],
        ),
        ("infiniswap_consumer_inflight_io", io_status["in_flight"]),
        (
            "infiniswap_consumer_oldest_inflight_io_milliseconds",
            io_status["oldest_in_flight_ms"],
        ),
        (
            "infiniswap_consumer_provider_failure_deadline_milliseconds",
            device["provider_failure_deadline_ms"],
        ),
        (
            "infiniswap_consumer_last_error_errno",
            status["last_error"]["errno"] if status["last_error"] else 0,
        ),
    )
    for name, value in gauges:
        lines.extend(
            (
                "# TYPE %s gauge" % name,
                "%s%s %d" % (name, _labels(device_label), value),
            )
        )

    counters = dict(metric_values)
    counters.update(
        {
            "io_requests_total": io_status["requests_total"],
            "io_completed_total": io_status["completed_total"],
            "io_errors_total": io_status["errors_total"],
        }
    )
    for name in sorted(counters):
        metric_name = "infiniswap_consumer_" + name
        lines.extend(
            (
                "# TYPE %s counter" % metric_name,
                "%s%s %d"
                % (metric_name, _labels(device_label), counters[name]),
            )
        )
    lines.append("# EOF")
    return "\n".join(lines) + "\n"
