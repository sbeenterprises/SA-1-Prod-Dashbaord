#!/usr/bin/env python3
"""Generate Grafana unified-alerting provisioning (FR-11).

Run:  python3 tools/gen_alerts.py [--profile sim|vessel]   (committed: vessel)
Output: grafana/provisioning/alerting/alerts.yml

Each rule: Flux query (A) -> reduce last (B) -> threshold (C).

Profiles decide what "no data" means for the sensor-backed rules:
  sim     (default) the simulation community has no CAN bus, engine, GPS
          quality, battery or fuel sensors, so those rules must NOT page on
          missing data; they still fire if a value crosses the threshold.
  vessel  sensor rules page on missing data (a silent sensor is a fault).
"""
import os
import sys

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
    # noDataState=OK on the rules whose field has no producer yet in the
    # simulation environment (no battery monitor, fuel sender, radar, camera
    # or link probe). They evaluate normally once data exists; flip to
    # "Alerting" per rule at vessel commissioning (see docs/deployment.md).
    rule("usv-low-battery", "Low Battery SOC",
         q("electrical", "soc"), "lt", 25, "critical",
         "Battery state of charge below 25% — consider Return-to-Home.",
         no_data="OK"),
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
         "Fuel level below 15%.", no_data="OK"),
    rule("usv-collision-risk", "Collision Risk HIGH",
         q("traffic", "high_risk"), "gt", 0, "critical",
         "A contact is closing inside the alert CPA within the alert TCPA "
         "(grafana_bridge radar_cpa_alert_m / tcpa_alert_s).", no_data="OK"),
    rule("usv-radar-offline", "Radar Offline",
         q("radar", "online"), "lt", 1, "warning",
         "Radar is offline — collision avoidance degraded.", no_data="OK"),
    rule("usv-camera-offline", "Camera Offline",
         q("camera", "connected"), "lt", 1, "warning",
         "Camera disconnected.", no_data="OK"),
    rule("usv-signalk-disc", "Signal K Disconnected",
         q("comms", "signalk_ws_up"), "lt", 1, "warning",
         "Signal K WebSocket stream is down.", for_="30s"),
    rule("usv-moos-disc", "MOOS Disconnected",
         q("comms", "moos_msg_rate"), "lt", 1, "critical",
         "No MOOS traffic — autonomy stack may be down.", for_="30s"),
    rule("usv-comms-failure", "Communication Failure",
         q("comms", "internet_up"), "lt", 1, "critical",
         "Internet/Starlink link down — remote monitoring degraded.",
         for_="30s", no_data="OK"),
    # ------------------------------------------------- Alpha mission rules
    # These fire on the live autonomy fields grafana_bridge derives from the
    # Alpha mission (alpha.bhv). noDataState=OK: the fields only exist while
    # the bridge runs, and "MOOS Disconnected" already covers absence.
    rule("usv-geofence", "Geofence Violation",
         q("autonomy", "geofence_breached"), "gt", 0, "critical",
         "Vessel outside the operating region (OPR_BREACHED) — opr24 "
         "recovery in progress. Self-clears 30 s after recovery.",
         for_="0s", no_data="OK"),
    rule("usv-comms-grace", "Operator Link Lost (Grace Period)",
         q("autonomy", "comms_phase"), "gt", 0, "warning",
         "Operator link down; the mission keeps running for the grace period. "
         "Escalates to the failsafe hold if the link does not return.",
         for_="0s", no_data="OK"),
    rule("usv-comms-failsafe", "Operator Link Lost (Failsafe Engaged)",
         q("autonomy", "comms_lost"), "gt", 0, "critical",
         "iWebSocket operator-link watchdog tripped — failsafe action in "
         "force, mission disarmed. Boat holds at the loss position.",
         for_="0s", no_data="OK"),
    rule("usv-helm-allstop", "Helm All-Stop (Unexpected)",
         q("autonomy", "allstop"), "gt", 0, "critical",
         "IVPHELM_ALLSTOP with an unexpected reason (e.g. MissingDecVars) — "
         "helm frozen while a mission may be active.",
         for_="30s", no_data="OK"),
    rule("usv-awaiting-rearm", "Survey Held — Awaiting Operator Re-Arm",
         q("autonomy", "awaiting_rearm"), "gt", 0, "warning",
         "Operator link is back but MISSION_ARMED is false: the survey will "
         "NOT resume until an operator AUTONOMOUS_START/RESUME.",
         for_="1m", no_data="OK"),
    rule("usv-crash-stop", "Crash Stop Commanded",
         q("autonomy", "crash_stop"), "gt", 0, "critical",
         "Emergency stop commanded from the console — planchaPID is braking "
         "the boat to rest and parking the helm.",
         for_="0s", no_data="OK"),
]


# Rules whose data source is a physical sensor that the simulation does not
# have. In the "vessel" profile their noDataState becomes "Alerting".
SENSOR_RULES = {"usv-low-batt-voltage", "usv-gps-loss", "usv-can-failure",
                "usv-engine-overtemp"}


def apply_profile(rules, profile):
    for r in rules:
        if r["uid"] in SENSOR_RULES:
            r["noDataState"] = "Alerting" if profile == "vessel" else "OK"
    return rules


def main():
    profile = "sim"
    if "--profile" in sys.argv:
        profile = sys.argv[sys.argv.index("--profile") + 1]
    if profile not in ("sim", "vessel"):
        sys.exit("--profile must be sim or vessel")
    apply_profile(RULES, profile)
    os.makedirs(OUT, exist_ok=True)
    doc = {
        "apiVersion": 1,
        "groups": [{
            "orgId": 1,
            "name": "usv-alarms",
            "folder": "Swift Autonomous Dashboard",   # same folder as the dashboards
            "interval": "30s",
            "rules": RULES,
        }],
    }
    with open(os.path.join(OUT, "alerts.yml"), "w") as f:
        yaml.safe_dump(doc, f, sort_keys=False, width=100)
    print(f"wrote alerts.yml with {len(RULES)} rules (profile: {profile})")


if __name__ == "__main__":
    main()
