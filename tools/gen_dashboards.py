#!/usr/bin/env python3
"""Generate the 11 USV Grafana dashboard JSON models (FR-1 .. FR-11).

Run:  python3 tools/gen_dashboards.py
Output: grafana/dashboards/*.json

Regenerate after editing panel definitions below; dashboards are provisioned
from files, so Grafana picks changes up within 30 s.
"""
import json
import os

DS = {"type": "influxdb", "uid": "influxdb-usv"}
OUT = os.path.join(os.path.dirname(__file__), "..", "grafana", "dashboards")

# Series palette matched to the vessel control UI (teal-led), validated for
# CVD separation and contrast on BOTH the dark and light Grafana surfaces
# (dashboards ship a light/dark toggle, so one palette must serve both).
# Threshold/status colors (green/yellow/orange/red) remain strictly semantic.
PALETTE = ["#0f9e92", "#2f6fd8", "#c47b06", "#9256b8"]
ACCENT = PALETTE[0]

import re as _re
_FIELD_RE = _re.compile(r'_field == "(\w+)"')

_id = [0]


def nid():
    _id[0] += 1
    return _id[0]


def flux(measurement, fields, bucket="telemetry", every='v.windowPeriod', fn="mean",
         extra="", last=False):
    if isinstance(fields, str):
        fields = [fields]
    fieldset = " or ".join(f'r._field == "{f}"' for f in fields)
    q = (f'from(bucket: "{bucket}")\n'
         f'  |> range(start: v.timeRangeStart, stop: v.timeRangeStop)\n'
         f'  |> filter(fn: (r) => r._measurement == "{measurement}" and ({fieldset}))\n'
         f'  |> filter(fn: (r) => r.vessel == "${{vessel}}")\n')
    if extra:
        q += extra + "\n"
    if last:
        q += '  |> last()\n'
    else:
        q += f'  |> aggregateWindow(every: {every}, fn: {fn}, createEmpty: false)\n'
    return q


def target(query, alias=None, ref="A"):
    t = {"datasource": DS, "query": query, "refId": ref}
    if alias:
        t["alias"] = alias
    return t


def _thresholds(steps):
    return {"mode": "absolute",
            "steps": [{"color": c, "value": v} for v, c in steps]}


def panel(ptype, title, targets, x, y, w, h, unit=None, thresholds=None,
          options=None, overrides=None, decimals=None, min_=None, max_=None,
          mappings=None, custom=None):
    defaults = {"color": {"mode": "palette-classic"}}
    if unit:
        defaults["unit"] = unit
    if decimals is not None:
        defaults["decimals"] = decimals
    if min_ is not None:
        defaults["min"] = min_
    if max_ is not None:
        defaults["max"] = max_
    if thresholds:
        defaults["thresholds"] = _thresholds(thresholds)
        if ptype in ("stat", "gauge", "bargauge"):
            defaults["color"] = {"mode": "thresholds"}
    if mappings:
        defaults["mappings"] = mappings
    if custom:
        defaults["custom"] = custom
    elif ptype == "timeseries":
        defaults["custom"] = {"drawStyle": "line", "lineWidth": 2,
                              "fillOpacity": 8, "showPoints": "never",
                              "spanNulls": 60000}
    p = {
        "id": nid(), "type": ptype, "title": title, "datasource": DS,
        "gridPos": {"x": x, "y": y, "w": w, "h": h},
        "targets": targets,
        "fieldConfig": {"defaults": defaults, "overrides": overrides or []},
        "options": options or {},
    }
    return p


def stat(title, query, x, y, w=4, h=4, unit=None, thresholds=None,
         mappings=None, text_mode="auto", decimals=None):
    return panel("stat", title, [target(query)], x, y, w, h, unit=unit,
                 decimals=decimals,
                 thresholds=thresholds or [(None, "green")],
                 mappings=mappings,
                 options={"reduceOptions": {"calcs": ["lastNotNull"]},
                          "textMode": text_mode, "graphMode": "none",
                          "colorMode": "value"})


def gauge(title, query, x, y, w=4, h=5, unit="percent", min_=0, max_=100,
          thresholds=None):
    return panel("gauge", title, [target(query)], x, y, w, h, unit=unit,
                 min_=min_, max_=max_,
                 thresholds=thresholds or [(None, "green")],
                 options={"reduceOptions": {"calcs": ["lastNotNull"]}})


def ts(title, targets_, x, y, w=12, h=8, unit=None, overrides=None,
       min_=None, max_=None):
    # Assign the themed palette per series, in fixed field order.
    fields = []
    for t in targets_:
        for f in _FIELD_RE.findall(t.get("query", "")):
            if f not in fields:
                fields.append(f)
    color_overrides = [
        {"matcher": {"id": "byRegexp", "options": f".*{f}.*"},
         "properties": [{"id": "color",
                         "value": {"mode": "fixed",
                                   "fixedColor": PALETTE[i % len(PALETTE)]}}]}
        for i, f in enumerate(fields)
    ]
    p = panel("timeseries", title, targets_, x, y, w, h, unit=unit,
              overrides=color_overrides + (overrides or []),
              min_=min_, max_=max_,
              options={"legend": {"displayMode": "list",
                                  "placement": "bottom",
                                  "showLegend": True},
                       "tooltip": {"mode": "multi"}})
    return p


def state_map(pairs):
    """Value mappings for string states."""
    return [{"type": "value",
             "options": {k: {"text": k, "color": v, "index": i}
                         for i, (k, v) in enumerate(pairs)}}]


def updown_map(down_text="OFFLINE", up_text="ONLINE"):
    return [{"type": "value", "options": {
        "0": {"text": down_text, "color": "red", "index": 0},
        "1": {"text": up_text, "color": "green", "index": 1}}}]


def dashboard(uid, title, panels, tags, refresh="5s", time_from="now-30m"):
    return {
        "uid": uid, "title": title, "tags": ["usv"] + tags,
        "timezone": "browser", "schemaVersion": 39, "version": 1,
        "refresh": refresh, "editable": True, "style": "dark",
        "time": {"from": time_from, "to": "now"},
        # Tab-style navigation: every usv-tagged dashboard is rendered as a
        # clickable link chip across the top of every dashboard. keepTime +
        # includeVars carry the time range and vessel selection across tabs.
        # The Light/Dark chips re-open this dashboard with Grafana's ?theme=
        # override (per-view; set the lasting default in user preferences).
        "links": [{
            "type": "dashboards", "title": "", "tags": ["usv"],
            "asDropdown": False, "keepTime": True, "includeVars": True,
            "targetBlank": False, "icon": "dashboard",
        }, {
            "type": "link", "title": "☀ Light", "url": f"/d/{uid}?theme=light",
            "keepTime": True, "includeVars": True, "targetBlank": False,
            "icon": "external link", "tooltip": "View in light theme",
        }, {
            "type": "link", "title": "☾ Dark", "url": f"/d/{uid}?theme=dark",
            "keepTime": True, "includeVars": True, "targetBlank": False,
            "icon": "external link", "tooltip": "View in dark theme",
        }],
        "templating": {"list": [{
            "name": "vessel", "label": "Vessel", "type": "query",
            "datasource": DS,
            "query": ('import "influxdata/influxdb/schema"\n'
                      'schema.tagValues(bucket: "telemetry", tag: "vessel")'),
            "refresh": 2, "current": {"text": "usv-01", "value": "usv-01"},
            "includeAll": False, "multi": False,
        }]},
        "panels": panels,
        "annotations": {"list": [{
            "name": "Mission events", "datasource": DS, "enable": True,
            "iconColor": "yellow",
            "target": {"query": (
                'from(bucket: "events")\n'
                '  |> range(start: v.timeRangeStart, stop: v.timeRangeStop)\n'
                '  |> filter(fn: (r) => r._measurement == "events" and r._field == "message")\n'
                '  |> filter(fn: (r) => r.vessel == "${vessel}")\n')},
        }]},
    }


def last_str(measurement, field, bucket="telemetry"):
    return flux(measurement, field, bucket=bucket, last=True)


DASHBOARDS = []

# ---------------------------------------------------------------- FR-1 Mission
mission_state_mappings = state_map([
    ("Idle", "text"), ("Autonomous", "green"), ("StationKeeping", "blue"),
    ("ReturnToHome", "orange"), ("Completed", "purple"),
])
DASHBOARDS.append(dashboard("usv-mission", "Mission", [
    stat("Mission Name", last_str("mission_state", "mission_name"), 0, 0, 5, 4,
         text_mode="value"),
    stat("Mission ID", last_str("mission_state", "mission_id"), 5, 0, 4, 4,
         text_mode="value"),
    stat("Mission State", last_str("mission_state", "state"), 9, 0, 5, 4,
         mappings=mission_state_mappings, text_mode="value"),
    stat("Elapsed Time", flux("mission", "elapsed_s", last=True), 14, 0, 5, 4,
         unit="s", decimals=0),
    stat("ETA to Waypoint", flux("mission", "eta_s", last=True), 19, 0, 5, 4,
         unit="s", decimals=0),
    gauge("Mission Progress", flux("mission", "progress_pct", last=True),
          0, 4, 5, 5, thresholds=[(None, ACCENT)]),
    stat("Current Waypoint", flux("mission", "waypoint_index", last=True),
         5, 4, 4, 5, decimals=0),
    stat("Waypoints Total", flux("mission", "waypoints_total", last=True),
         9, 4, 4, 5, decimals=0),
    stat("Distance to Waypoint", flux("mission", "dist_to_waypoint", last=True),
         13, 4, 5, 5, unit="lengthm", decimals=0),
    stat("Distance Traveled", flux("mission", "distance_traveled", last=True),
         18, 4, 6, 5, unit="lengthm", decimals=0),
    ts("Mission Progress Over Time",
       [target(flux("mission", "progress_pct"), "progress %")],
       0, 9, 12, 7, unit="percent", min_=0, max_=100),
    ts("Distance to Waypoint",
       [target(flux("mission", "dist_to_waypoint"), "distance m")],
       12, 9, 12, 7, unit="lengthm"),
    panel("state-timeline", "Mission State Timeline",
          [target(flux("mission_state", "state", every="10s", fn="last"))],
          0, 16, 24, 5, mappings=mission_state_mappings,
          custom={"fillOpacity": 70, "lineWidth": 0},
          options={"showValue": "auto", "legend": {"showLegend": True,
                                                    "placement": "bottom",
                                                    "displayMode": "list"}}),
    panel("table", "Mission Event Timeline",
          [target('from(bucket: "events")\n'
                  '  |> range(start: v.timeRangeStart, stop: v.timeRangeStop)\n'
                  '  |> filter(fn: (r) => r._measurement == "events" and r._field == "message")\n'
                  '  |> filter(fn: (r) => r.vessel == "${vessel}")\n'
                  '  |> keep(columns: ["_time", "_value", "type", "severity"])\n'
                  '  |> sort(columns: ["_time"], desc: true)\n')],
          0, 21, 24, 8,
          options={"showHeader": True,
                   "sortBy": [{"displayName": "_time", "desc": True}]}),
], ["mission"]))

# ------------------------------------------------------------- FR-2 Navigation
DASHBOARDS.append(dashboard("usv-navigation", "Navigation", [
    panel("geomap", "Live Position & Track",
          [target(flux("nav", ["lat", "lon"], every="5s", fn="last",
                       extra=('  |> pivot(rowKey: ["_time"], columnKey: ["_field"], '
                              'valueColumn: "_value")')))],
          0, 0, 14, 12,
          options={
              "view": {"id": "fit", "lat": 0, "lon": 0, "zoom": 12},
              "basemap": {"type": "osm-standard", "name": "OSM"},
              "layers": [{
                  "type": "route", "name": "Track",
                  "location": {"mode": "coords", "latitude": "lat",
                               "longitude": "lon"},
                  "config": {"style": {"color": {"fixed": "orange"},
                                       "lineWidth": 2, "opacity": 0.8}},
              }, {
                  "type": "markers", "name": "Vessel",
                  "location": {"mode": "coords", "latitude": "lat",
                               "longitude": "lon"},
                  "filterData": {"id": "byRefId", "options": "A"},
                  "config": {"style": {"size": {"fixed": 8},
                                       "color": {"fixed": "super-light-blue"},
                                       "symbol": {"fixed": "img/icons/marker/circle.svg",
                                                  "mode": "fixed"}}},
              }],
              "tooltip": {"mode": "details"},
          }),
    stat("Latitude", flux("nav", "lat", last=True), 14, 0, 5, 3, decimals=6),
    stat("Longitude", flux("nav", "lon", last=True), 19, 0, 5, 3, decimals=6),
    stat("SOG", flux("nav", "sog", last=True), 14, 3, 5, 3, unit="velocityknot",
         decimals=1),
    stat("COG", flux("nav", "cog", last=True), 19, 3, 5, 3, unit="degree",
         decimals=0),
    stat("Heading", flux("nav", "heading", last=True), 14, 6, 5, 3,
         unit="degree", decimals=0),
    stat("GPS Fix", last_str("nav", "fix_type"), 19, 6, 5, 3, text_mode="value"),
    stat("Satellites", flux("nav", "satellites", last=True), 14, 9, 5, 3,
         decimals=0, thresholds=[(None, "red"), (5, "yellow"), (8, "green")]),
    stat("Cross-Track Error", flux("nav", "cross_track_error", last=True),
         19, 9, 5, 3, unit="lengthm", decimals=1,
         thresholds=[(None, "green"), (10, "yellow"), (25, "red")]),
    ts("Speed Over Ground", [target(flux("nav", "sog"), "SOG")],
       0, 12, 8, 7, unit="velocityknot"),
    ts("Heading / COG",
       [target(flux("nav", ["heading", "cog"]))],
       8, 12, 8, 7, unit="degree", min_=0, max_=360),
    ts("Attitude (Roll / Pitch / Yaw)",
       [target(flux("nav", ["roll", "pitch"]))],
       16, 12, 8, 7, unit="degree"),
    ts("Cross-Track Error", [target(flux("nav", "cross_track_error"), "XTE")],
       0, 19, 12, 6, unit="lengthm"),
    ts("Distance to Next Waypoint",
       [target(flux("mission", "dist_to_waypoint"), "distance")],
       12, 19, 12, 6, unit="lengthm"),
], ["navigation"]))

# -------------------------------------------------------------- FR-3 Autopilot
DASHBOARDS.append(dashboard("usv-autopilot", "Autopilot (MOOS-IvP)", [
    stat("Active Behavior", last_str("mission_state", "active_behavior"),
         0, 0, 6, 4, text_mode="value"),
    stat("Station Keeping", last_str("mission_state", "station_keep"), 6, 0, 4, 4,
         mappings=[{"type": "value", "options": {
             "true": {"text": "ACTIVE", "color": "blue", "index": 0},
             "false": {"text": "OFF", "color": "text", "index": 1}}}],
         text_mode="value"),
    stat("Heading Error", flux("autopilot", "heading_error", last=True),
         10, 0, 4, 4, unit="degree", decimals=1,
         thresholds=[(None, "green"), (10, "yellow"), (25, "red")]),
    stat("Speed Error", flux("autopilot", "speed_error", last=True), 14, 0, 4, 4,
         unit="velocityknot", decimals=2,
         thresholds=[(None, "green"), (1, "yellow"), (2, "red")]),
    stat("Rudder Cmd", flux("autopilot", "rudder_cmd", last=True), 18, 0, 3, 4,
         unit="degree", decimals=1),
    stat("Throttle Cmd", flux("autopilot", "throttle_cmd", last=True), 21, 0, 3, 4,
         unit="percent", decimals=0),
    ts("Desired vs Actual Heading",
       [target(flux("autopilot", ["desired_heading", "actual_heading"]))],
       0, 4, 12, 8, unit="degree", min_=0, max_=360),
    ts("Desired vs Actual Speed",
       [target(flux("autopilot", ["desired_speed", "actual_speed"]))],
       12, 4, 12, 8, unit="velocityknot"),
    ts("Rudder Output", [target(flux("autopilot", "rudder_cmd"), "rudder")],
       0, 12, 8, 7, unit="degree", min_=-35, max_=35),
    ts("Throttle Output", [target(flux("autopilot", "throttle_cmd"), "throttle")],
       8, 12, 8, 7, unit="percent", min_=0, max_=100),
    ts("PID Error Trends",
       [target(flux("autopilot", ["heading_error", "speed_error"]))],
       16, 12, 8, 7),
    ts("PID Controller Outputs",
       [target(flux("autopilot", ["pid_heading_out", "pid_speed_out"]))],
       0, 19, 24, 6),
], ["autopilot"]))

# ---------------------------------------------------------- FR-4 Vessel Health
DASHBOARDS.append(dashboard("usv-health", "Vessel Health", [
    # Engine row
    stat("Engine RPM", flux("engine", "rpm", last=True), 0, 0, 4, 4,
         unit="rotrpm", decimals=0,
         thresholds=[(None, "green"), (4500, "yellow"), (5500, "red")]),
    stat("Coolant Temp", flux("engine", "coolant_temp", last=True), 4, 0, 4, 4,
         unit="celsius", decimals=1,
         thresholds=[(None, "green"), (95, "yellow"), (105, "red")]),
    stat("Oil Pressure", flux("engine", "oil_pressure", last=True), 8, 0, 4, 4,
         unit="pressurekpa", decimals=0,
         thresholds=[(None, "red"), (150, "yellow"), (250, "green")]),
    gauge("Fuel Level", flux("engine", "fuel_level", last=True), 12, 0, 4, 4,
          thresholds=[(None, "red"), (15, "yellow"), (30, "green")]),
    stat("Fuel Rate", flux("engine", "fuel_rate", last=True), 16, 0, 4, 4,
         unit="litreh", decimals=1),
    stat("Engine Runtime", flux("engine", "runtime_h", last=True), 20, 0, 4, 4,
         unit="h", decimals=1),
    ts("Engine RPM", [target(flux("engine", "rpm"), "RPM")], 0, 4, 12, 7,
       unit="rotrpm"),
    ts("Coolant Temp / Oil Pressure",
       [target(flux("engine", "coolant_temp"), "coolant °C"),
        target(flux("engine", "oil_pressure"), "oil kPa", ref="B")],
       12, 4, 12, 7,
       overrides=[{"matcher": {"id": "byFrameRefID", "options": "B"},
                   "properties": [{"id": "unit", "value": "pressurekpa"},
                                  {"id": "custom.axisPlacement",
                                   "value": "right"}]}]),
    # Electrical row
    stat("Battery Voltage", flux("electrical", "battery_voltage", last=True),
         0, 11, 4, 4, unit="volt", decimals=2,
         thresholds=[(None, "red"), (11.8, "yellow"), (12.4, "green")]),
    stat("Battery Current", flux("electrical", "battery_current", last=True),
         4, 11, 4, 4, unit="amp", decimals=1),
    gauge("State of Charge", flux("electrical", "soc", last=True), 8, 11, 4, 4,
          thresholds=[(None, "red"), (25, "yellow"), (50, "green")]),
    stat("Charging Status", last_str("electrical", "charging_status"),
         12, 11, 4, 4, text_mode="value",
         mappings=[{"type": "value", "options": {
             "charging": {"text": "CHARGING", "color": "green", "index": 0},
             "discharging": {"text": "DISCHARGING", "color": "yellow", "index": 1}}}]),
    stat("Alternator Voltage", flux("electrical", "alternator_voltage", last=True),
         16, 11, 4, 4, unit="volt", decimals=1),
    stat("Total Power", flux("power", "total_power", last=True), 20, 11, 4, 4,
         unit="watt", decimals=0),
    ts("Battery Voltage / Current",
       [target(flux("electrical", ["battery_voltage"]), "voltage"),
        target(flux("electrical", ["battery_current"]), "current", ref="B")],
       0, 15, 12, 7, unit="volt",
       overrides=[{"matcher": {"id": "byFrameRefID", "options": "B"},
                   "properties": [{"id": "unit", "value": "amp"},
                                  {"id": "custom.axisPlacement",
                                   "value": "right"}]}]),
    ts("Power Consumption / System Voltage",
       [target(flux("power", "total_power"), "power W"),
        target(flux("power", "system_voltage"), "sys V", ref="B")],
       12, 15, 12, 7, unit="watt",
       overrides=[{"matcher": {"id": "byFrameRefID", "options": "B"},
                   "properties": [{"id": "unit", "value": "volt"},
                                  {"id": "custom.axisPlacement",
                                   "value": "right"}]}]),
    ts("State of Charge", [target(flux("electrical", "soc"), "SOC")],
       0, 22, 12, 6, unit="percent", min_=0, max_=100),
    ts("Fuel Level / Consumption",
       [target(flux("engine", "fuel_level"), "level %"),
        target(flux("engine", "fuel_rate"), "rate L/h", ref="B")],
       12, 22, 12, 6, unit="percent",
       overrides=[{"matcher": {"id": "byFrameRefID", "options": "B"},
                   "properties": [{"id": "unit", "value": "litreh"},
                                  {"id": "custom.axisPlacement",
                                   "value": "right"}]}]),
], ["health"]))

# ------------------------------------------------------------- FR-5 CAN Bus
DASHBOARDS.append(dashboard("usv-canbus", "CAN Bus (NMEA 2000)", [
    stat("Interface", flux("canbus", "interface_up", last=True), 0, 0, 4, 4,
         mappings=updown_map("DOWN", "UP"), text_mode="value"),
    gauge("Bus Utilization", flux("canbus", "utilization_pct", last=True),
          4, 0, 4, 4, thresholds=[(None, "green"), (50, "yellow"), (75, "red")]),
    stat("Messages / s", flux("canbus", "msg_rate", last=True), 8, 0, 4, 4,
         decimals=0),
    stat("Error Frames (5s)", flux("canbus", "error_frames", last=True),
         12, 0, 4, 4, decimals=0,
         thresholds=[(None, "green"), (1, "yellow"), (10, "red")]),
    stat("Dropped Frames", flux("canbus", "dropped_frames", last=True),
         16, 0, 4, 4, decimals=0,
         thresholds=[(None, "green"), (1, "red")]),
    stat("Active PGNs", flux("canbus", "active_pgns", last=True), 20, 0, 4, 4,
         decimals=0),
    ts("Message Rate", [target(flux("canbus", "msg_rate"), "msgs/s")],
       0, 4, 12, 7),
    ts("Bus Utilization", [target(flux("canbus", "utilization_pct"), "util %")],
       12, 4, 12, 7, unit="percent", min_=0, max_=100),
    ts("Error / Dropped Frames",
       [target(flux("canbus", ["error_frames", "dropped_frames"]))],
       0, 11, 12, 7),
    panel("table", "PGN Frequency (msgs/s)",
          [target('from(bucket: "telemetry")\n'
                  '  |> range(start: v.timeRangeStart, stop: v.timeRangeStop)\n'
                  '  |> filter(fn: (r) => r._measurement == "canbus_pgn" and r._field == "rate")\n'
                  '  |> filter(fn: (r) => r.vessel == "${vessel}")\n'
                  '  |> last()\n'
                  '  |> keep(columns: ["pgn", "_value"])\n'
                  '  |> sort(columns: ["_value"], desc: true)\n')],
          12, 11, 12, 7,
          options={"showHeader": True}),
], ["canbus"]))

# ---------------------------------------------------------------- FR-6 Radar
DASHBOARDS.append(dashboard("usv-radar", "Radar", [
    stat("Radar Status", flux("radar", "online", last=True), 0, 0, 5, 4,
         mappings=updown_map(), text_mode="value"),
    stat("Tracked Targets", flux("radar", "targets", last=True), 5, 0, 5, 4,
         decimals=0),
    stat("Closest Target", flux("radar", "closest_range_m", last=True),
         10, 0, 5, 4, unit="lengthm", decimals=0,
         thresholds=[(None, "red"), (200, "yellow"), (500, "green")]),
    stat("CPA", flux("radar", "cpa_m", last=True), 15, 0, 4, 4,
         unit="lengthm", decimals=0,
         thresholds=[(None, "red"), (150, "yellow"), (500, "green")]),
    stat("TCPA", flux("radar", "tcpa_s", last=True), 19, 0, 5, 4, unit="s",
         decimals=0, thresholds=[(None, "red"), (120, "yellow"), (300, "green")]),
    ts("Tracked Targets", [target(flux("radar", "targets"), "targets")],
       0, 4, 12, 8),
    ts("Closest Target / CPA",
       [target(flux("radar", ["closest_range_m", "cpa_m"]))],
       12, 4, 12, 8, unit="lengthm"),
    ts("Detection Rate", [target(flux("radar", "detection_rate"), "rate")],
       0, 12, 12, 6, unit="percentunit", min_=0, max_=1),
    ts("TCPA", [target(flux("radar", "tcpa_s"), "TCPA")], 12, 12, 12, 6,
       unit="s"),
], ["radar"]))

# --------------------------------------------------------------- FR-7 Camera
DASHBOARDS.append(dashboard("usv-camera", "Cameras", [
    stat("Connection", flux("camera", "connected", last=True), 0, 0, 4, 4,
         mappings=updown_map("DISCONNECTED", "CONNECTED"), text_mode="value"),
    stat("Live Stream", flux("camera", "stream_up", last=True), 4, 0, 4, 4,
         mappings=updown_map("UNAVAILABLE", "AVAILABLE"), text_mode="value"),
    stat("Recording", flux("camera", "recording", last=True), 8, 0, 4, 4,
         mappings=updown_map("STOPPED", "RECORDING"), text_mode="value"),
    stat("Frame Rate", flux("camera", "fps", last=True), 12, 0, 4, 4,
         unit="fps" if True else None, decimals=1,
         thresholds=[(None, "red"), (15, "yellow"), (24, "green")]),
    stat("Bitrate", flux("camera", "bitrate_kbps", last=True), 16, 0, 4, 4,
         unit="Kbits", decimals=0),
    stat("Camera Temp", flux("camera", "temp_c", last=True), 20, 0, 4, 4,
         unit="celsius", decimals=1,
         thresholds=[(None, "green"), (60, "yellow"), (75, "red")]),
    ts("Frame Rate", [target(flux("camera", "fps"), "fps")], 0, 4, 12, 7),
    ts("Bitrate", [target(flux("camera", "bitrate_kbps"), "kbps")],
       12, 4, 12, 7, unit="Kbits"),
    ts("PTZ Position",
       [target(flux("camera", ["ptz_pan", "ptz_tilt"]))], 0, 11, 12, 6,
       unit="degree"),
    ts("Camera Temperature", [target(flux("camera", "temp_c"), "temp")],
       12, 11, 12, 6, unit="celsius"),
], ["camera"]))

# ----------------------------------------------------------- FR-8 Communication
DASHBOARDS.append(dashboard("usv-comms", "Communications", [
    stat("Internet", flux("comms", "internet_up", last=True), 0, 0, 4, 4,
         mappings=updown_map(), text_mode="value"),
    stat("Starlink", flux("comms", "starlink_up", last=True), 4, 0, 4, 4,
         mappings=updown_map(), text_mode="value"),
    stat("VPN / Tailscale", flux("comms", "vpn_up", last=True), 8, 0, 4, 4,
         mappings=updown_map(), text_mode="value"),
    stat("Signal K WebSocket", flux("comms", "signalk_ws_up", last=True),
         12, 0, 4, 4, mappings=updown_map(), text_mode="value"),
    stat("Latency", flux("comms", "latency_ms", last=True), 16, 0, 4, 4,
         unit="ms", decimals=0,
         thresholds=[(None, "green"), (150, "yellow"), (400, "red")]),
    stat("Packet Loss", flux("comms", "packet_loss_pct", last=True), 20, 0, 4, 4,
         unit="percent", decimals=1,
         thresholds=[(None, "green"), (2, "yellow"), (10, "red")]),
    ts("Latency", [target(flux("comms", "latency_ms"), "latency")],
       0, 4, 12, 7, unit="ms"),
    ts("Packet Loss", [target(flux("comms", "packet_loss_pct"), "loss")],
       12, 4, 12, 7, unit="percent", min_=0),
    ts("Bandwidth",
       [target(flux("comms", ["up_mbps", "down_mbps"]))], 0, 11, 12, 7,
       unit="Mbits"),
    ts("Telemetry Message Rates",
       [target(flux("comms", ["moos_msg_rate", "signalk_msg_rate"]))],
       12, 11, 12, 7),
], ["comms"]))

# ---------------------------------------------------------- FR-9 System Health
def tele(measurement, field, fn="mean", extra=""):
    """Telegraf-produced metrics are tagged by host, not vessel."""
    return (f'from(bucket: "telemetry")\n'
            f'  |> range(start: v.timeRangeStart, stop: v.timeRangeStop)\n'
            f'  |> filter(fn: (r) => r._measurement == "{measurement}" '
            f'and r._field == "{field}")\n'
            + (extra + "\n" if extra else "")
            + f'  |> aggregateWindow(every: v.windowPeriod, fn: {fn}, createEmpty: false)\n')


DASHBOARDS.append(dashboard("usv-system", "System Health (OBC)", [
    gauge("CPU", tele("cpu", "usage_user") .replace("aggregateWindow(every: v.windowPeriod, fn: mean, createEmpty: false)", "last()"),
          0, 0, 4, 5, thresholds=[(None, "green"), (70, "yellow"), (90, "red")]),
    gauge("Memory", tele("mem", "used_percent").replace("aggregateWindow(every: v.windowPeriod, fn: mean, createEmpty: false)", "last()"),
          4, 0, 4, 5, thresholds=[(None, "green"), (75, "yellow"), (90, "red")]),
    gauge("Disk", tele("disk", "used_percent").replace("aggregateWindow(every: v.windowPeriod, fn: mean, createEmpty: false)", "last()"),
          8, 0, 4, 5, thresholds=[(None, "green"), (75, "yellow"), (90, "red")]),
    stat("CPU Temp", tele("temp", "temp").replace("aggregateWindow(every: v.windowPeriod, fn: mean, createEmpty: false)", "last()"),
         12, 0, 4, 5, unit="celsius", decimals=1,
         thresholds=[(None, "green"), (70, "yellow"), (85, "red")]),
    stat("Signal K Service",
         tele("net_response", "result_code",
              extra='  |> filter(fn: (r) => r.service == "signalk")').replace(
             "aggregateWindow(every: v.windowPeriod, fn: mean, createEmpty: false)", "last()"),
         16, 0, 4, 5, text_mode="value",
         mappings=[{"type": "value", "options": {
             "0": {"text": "UP", "color": "green", "index": 0}}},
             {"type": "range", "options": {
                 "from": 1, "to": 99,
                 "result": {"text": "DOWN", "color": "red", "index": 1}}}]),
    stat("MOOS Service",
         tele("net_response", "result_code",
              extra='  |> filter(fn: (r) => r.service == "moos")').replace(
             "aggregateWindow(every: v.windowPeriod, fn: mean, createEmpty: false)", "last()"),
         20, 0, 4, 5, text_mode="value",
         mappings=[{"type": "value", "options": {
             "0": {"text": "UP", "color": "green", "index": 0}}},
             {"type": "range", "options": {
                 "from": 1, "to": 99,
                 "result": {"text": "DOWN", "color": "red", "index": 1}}}]),
    ts("CPU Utilization", [target(tele("cpu", "usage_user"), "user"),
                           target(tele("cpu", "usage_system"), "system", ref="B")],
       0, 5, 12, 7, unit="percent", min_=0, max_=100),
    ts("Memory", [target(tele("mem", "used_percent"), "used %")],
       12, 5, 12, 7, unit="percent", min_=0, max_=100),
    ts("Disk I/O", [target(tele("diskio", "read_bytes", fn="derivative"), "read"),
                    target(tele("diskio", "write_bytes", fn="derivative"),
                           "write", ref="B")],
       0, 12, 12, 7, unit="Bps"),
    ts("Network I/O", [target(tele("net", "bytes_recv", fn="derivative"), "rx"),
                       target(tele("net", "bytes_sent", fn="derivative"),
                              "tx", ref="B")],
       12, 12, 12, 7, unit="Bps"),
    panel("table", "Services (systemd)",
          [target('from(bucket: "telemetry")\n'
                  '  |> range(start: -1m)\n'
                  '  |> filter(fn: (r) => r._measurement == "systemd_units" '
                  'and r._field == "active_code")\n'
                  '  |> last()\n'
                  '  |> keep(columns: ["name", "active", "sub", "_value"])\n')],
          0, 19, 24, 7, options={"showHeader": True},
          mappings=[{"type": "value", "options": {
              "0": {"text": "active", "color": "green", "index": 0}}},
              {"type": "range", "options": {
                  "from": 1, "to": 99,
                  "result": {"text": "not active", "color": "red",
                             "index": 1}}}]),
], ["system"]))

# -------------------------------------------------------- FR-10 Mission Replay
DASHBOARDS.append(dashboard("usv-replay", "Mission Replay", [
    panel("text", "How to replay",
          [], 0, 0, 24, 2,
          options={"mode": "markdown", "content": (
              "Set the **time range** (top right) to the mission window, then "
              "scrub with the shared crosshair. Data older than the raw "
              "retention (30 d) is served from the 1-minute downsampled "
              "bucket. Use the **Mission events** annotations to locate "
              "waypoint hits and state changes.")}),
    panel("geomap", "GPS Track",
          [target(flux("nav", ["lat", "lon"], every="10s", fn="last",
                       extra=('  |> pivot(rowKey: ["_time"], columnKey: ["_field"], '
                              'valueColumn: "_value")')))],
          0, 2, 14, 12,
          options={"view": {"id": "fit"},
                   "basemap": {"type": "osm-standard", "name": "OSM"},
                   "layers": [{"type": "route", "name": "Track",
                               "location": {"mode": "coords",
                                            "latitude": "lat",
                                            "longitude": "lon"},
                               "config": {"style": {"color": {"fixed": "orange"},
                                                    "lineWidth": 2}}}]}),
    panel("state-timeline", "Mission State",
          [target(flux("mission_state", "state", every="30s", fn="last"))],
          14, 2, 10, 6, mappings=mission_state_mappings,
          custom={"fillOpacity": 70, "lineWidth": 0},
          options={"showValue": "auto"}),
    panel("table", "Mission Events",
          [target('from(bucket: "events")\n'
                  '  |> range(start: v.timeRangeStart, stop: v.timeRangeStop)\n'
                  '  |> filter(fn: (r) => r._measurement == "events" and r._field == "message")\n'
                  '  |> filter(fn: (r) => r.vessel == "${vessel}")\n'
                  '  |> keep(columns: ["_time", "_value", "type", "severity"])\n'
                  '  |> sort(columns: ["_time"], desc: true)\n')],
          14, 8, 10, 6, options={"showHeader": True}),
    ts("Heading History", [target(flux("nav", "heading"), "heading")],
       0, 14, 12, 6, unit="degree", min_=0, max_=360),
    ts("Speed History", [target(flux("nav", "sog"), "SOG")],
       12, 14, 12, 6, unit="velocityknot"),
    ts("Rudder Commands", [target(flux("autopilot", "rudder_cmd"), "rudder")],
       0, 20, 12, 6, unit="degree"),
    ts("Throttle Commands", [target(flux("autopilot", "throttle_cmd"), "throttle")],
       12, 20, 12, 6, unit="percent"),
    ts("CAN Bus Activity", [target(flux("canbus", "msg_rate"), "msgs/s")],
       0, 26, 12, 6),
    ts("Cross-Track Error", [target(flux("nav", "cross_track_error"), "XTE")],
       12, 26, 12, 6, unit="lengthm"),
], ["replay"], refresh="", time_from="now-6h"))

# ------------------------------------------------------------- FR-11 Alarms
DASHBOARDS.append(dashboard("usv-alarms", "Alarms & Notifications", [
    panel("alertlist", "Active Alerts", [], 0, 0, 12, 10,
          options={"stateFilter": {"firing": True, "pending": True,
                                   "noData": False, "normal": False,
                                   "error": True},
                   "alertName": "", "dashboardAlerts": False,
                   "groupMode": "default", "maxItems": 20,
                   "sortOrder": 1, "viewMode": "list"}),
    panel("table", "Alarm History (events bucket)",
          [target('from(bucket: "events")\n'
                  '  |> range(start: v.timeRangeStart, stop: v.timeRangeStop)\n'
                  '  |> filter(fn: (r) => r._measurement == "events" and r._field == "message")\n'
                  '  |> filter(fn: (r) => r.type == "alarm")\n'
                  '  |> filter(fn: (r) => r.vessel == "${vessel}")\n'
                  '  |> keep(columns: ["_time", "_value", "severity"])\n'
                  '  |> sort(columns: ["_time"], desc: true)\n')],
          12, 0, 12, 10, options={"showHeader": True}),
    stat("Battery SOC", flux("electrical", "soc", last=True), 0, 10, 4, 4,
         unit="percent", decimals=0,
         thresholds=[(None, "red"), (25, "yellow"), (50, "green")]),
    stat("GPS Sats", flux("nav", "satellites", last=True), 4, 10, 4, 4,
         decimals=0, thresholds=[(None, "red"), (5, "yellow"), (8, "green")]),
    stat("CAN Bus", flux("canbus", "interface_up", last=True), 8, 10, 4, 4,
         mappings=updown_map("FAIL", "OK"), text_mode="value"),
    stat("Coolant", flux("engine", "coolant_temp", last=True), 12, 10, 4, 4,
         unit="celsius", decimals=0,
         thresholds=[(None, "green"), (95, "yellow"), (105, "red")]),
    stat("Fuel", flux("engine", "fuel_level", last=True), 16, 10, 4, 4,
         unit="percent", decimals=0,
         thresholds=[(None, "red"), (15, "yellow"), (30, "green")]),
    stat("Radar", flux("radar", "online", last=True), 20, 10, 4, 4,
         mappings=updown_map(), text_mode="value"),
    stat("Camera", flux("camera", "connected", last=True), 0, 14, 4, 4,
         mappings=updown_map(), text_mode="value"),
    stat("Signal K", flux("comms", "signalk_ws_up", last=True), 4, 14, 4, 4,
         mappings=updown_map("DISCONNECTED", "CONNECTED"), text_mode="value"),
    stat("MOOS Rate", flux("comms", "moos_msg_rate", last=True), 8, 14, 4, 4,
         decimals=0, thresholds=[(None, "red"), (1, "yellow"), (10, "green")]),
    stat("Comms Link", flux("comms", "internet_up", last=True), 12, 14, 4, 4,
         mappings=updown_map("FAILED", "OK"), text_mode="value"),
    stat("Latency", flux("comms", "latency_ms", last=True), 16, 14, 4, 4,
         unit="ms", decimals=0,
         thresholds=[(None, "green"), (150, "yellow"), (400, "red")]),
    stat("Packet Loss", flux("comms", "packet_loss_pct", last=True), 20, 14, 4, 4,
         unit="percent", decimals=1,
         thresholds=[(None, "green"), (2, "yellow"), (10, "red")]),
], ["alarms"]))


def main():
    os.makedirs(OUT, exist_ok=True)
    for d in DASHBOARDS:
        path = os.path.join(OUT, f"{d['uid']}.json")
        with open(path, "w") as f:
            json.dump(d, f, indent=2)
        print(f"wrote {path}  ({len(d['panels'])} panels)")


if __name__ == "__main__":
    main()
