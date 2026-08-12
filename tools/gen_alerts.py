#!/usr/bin/env python3
"""Generate Grafana unified-alerting provisioning (FR-11).

Run:  python3 tools/gen_alerts.py
Output: grafana/provisioning/alerting/alerts.yml

Each rule: Flux query (A) -> reduce last (B) -> threshold (C).
"""
import os

import yaml

OUT = os.path.join(os.path.dirname(__file__), "..",
                   "grafana", "provisioning", "alerting")
DS_UID = "influxdb-usv"


def q(measurement, field, bucket="telemetry", extra=""):
    return (f'from(bucket: "{bucket}")\n'
            f'  |> range(start: -2m)\n'
            f'  |> filter(fn: (r) => r._measurement == "{measurement}" '
            f'and r._field == "{field}")\n'
            + (extra + "\n" if extra else ""))


def rule(uid, title, query, op, threshold, severity, summary,
         for_="1m", no_data="Alerting"):
    """op: 'lt' or 'gt' — alert when last value is < / > threshold."""
    return {
        "uid": uid,
        "title": title,
        "condition": "C",
        "for": for_,
        "noDataState": no_data,
        "execErrState": "Alerting",
        "labels": {"severity": severity, "system": "usv"},
        "annotations": {"summary": summary},
        "data": [
            {
                "refId": "A",
                "relativeTimeRange": {"from": 120, "to": 0},
                "datasourceUid": DS_UID,
                "model": {"refId": "A", "query": query},
            },
            {
                "refId": "B",
                "datasourceUid": "__expr__",
                "model": {"refId": "B", "type": "reduce", "reducer": "last",
                          "expression": "A",
                          "datasource": {"type": "__expr__", "uid": "__expr__"}},
            },
            {
                "refId": "C",
                "datasourceUid": "__expr__",
                "model": {"refId": "C", "type": "threshold", "expression": "B",
                          "datasource": {"type": "__expr__", "uid": "__expr__"},
                          "conditions": [{
                              "evaluator": {"type": op, "params": [threshold]},
                              "operator": {"type": "and"},
                              "query": {"params": ["C"]},
                              "reducer": {"type": "last", "params": []},
                              "type": "query",
                          }]},
            },
        ],
    }


RULES = [
    rule("usv-low-battery", "Low Battery SOC",
         q("electrical", "soc"), "lt", 25, "critical",
         "Battery state of charge below 25% — consider Return-to-Home."),
    rule("usv-low-batt-voltage", "Low Battery Voltage",
         q("electrical", "battery_voltage"), "lt", 11.8, "critical",
         "Battery voltage below 11.8 V."),
    rule("usv-gps-loss", "GPS Signal Loss",
         q("nav", "satellites"), "lt", 4, "critical",
         "GPS satellites below 4 — position solution unreliable.",
         for_="30s"),
    rule("usv-can-failure", "CAN Bus Failure",
         q("canbus", "interface_up"), "lt", 1, "critical",
         "CAN interface down or no frames received.", for_="30s"),
    rule("usv-engine-overtemp", "Engine Over Temperature",
         q("engine", "coolant_temp"), "gt", 105, "critical",
         "Engine coolant temperature above 105 °C."),
    rule("usv-low-fuel", "Low Fuel Level",
         q("engine", "fuel_level"), "lt", 15, "warning",
         "Fuel level below 15%."),
    rule("usv-radar-offline", "Radar Offline",
         q("radar", "online"), "lt", 1, "warning",
         "Radar is offline — collision avoidance degraded."),
    rule("usv-camera-offline", "Camera Offline",
         q("camera", "connected"), "lt", 1, "warning",
         "Camera disconnected."),
    rule("usv-signalk-disc", "Signal K Disconnected",
         q("comms", "signalk_ws_up"), "lt", 1, "warning",
         "Signal K WebSocket stream is down.", for_="30s"),
    rule("usv-moos-disc", "MOOS Disconnected",
         q("comms", "moos_msg_rate"), "lt", 1, "critical",
         "No MOOS traffic — autonomy stack may be down.", for_="30s"),
    rule("usv-comms-failure", "Communication Failure",
         q("comms", "internet_up"), "lt", 1, "critical",
         "Internet/Starlink link down — remote monitoring degraded.",
         for_="30s"),
    rule("usv-geofence", "Geofence Violation",
         q("events", "value", bucket="events",
           extra='  |> filter(fn: (r) => r.type == "geofence")'),
         "gt", 0, "critical",
         "Vessel crossed the geofence boundary.", for_="0s",
         no_data="OK"),
]


def main():
    os.makedirs(OUT, exist_ok=True)
    doc = {
        "apiVersion": 1,
        "groups": [{
            "orgId": 1,
            "name": "usv-alarms",
            "folder": "USV Alerts",
            "interval": "30s",
            "rules": RULES,
        }],
    }
    with open(os.path.join(OUT, "alerts.yml"), "w") as f:
        yaml.safe_dump(doc, f, sort_keys=False, width=100)
    print(f"wrote alerts.yml with {len(RULES)} rules")


if __name__ == "__main__":
    main()
