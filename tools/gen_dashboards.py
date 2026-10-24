#!/usr/bin/env python3
"""Generate the 11 USV Grafana dashboard JSON models (FR-1 .. FR-11).

Run:  python3 tools/gen_dashboards.py
Output: grafana/dashboards/*.json

Regenerate after editing panel definitions below; dashboards are provisioned
from files, so Grafana picks changes up within 30 s.
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import engine_limits  # noqa: E402  (tools/engine_limits.py: coolant and fuel limits)

DS = {"type": "influxdb", "uid": "influxdb-usv"}
OUT = os.path.join(os.path.dirname(__file__), "..", "grafana", "dashboards")

# Series palette matched to the vessel control UI (teal-led), validated for
# CVD separation and contrast on BOTH the dark and light Grafana surfaces
# (dashboards ship a light/dark toggle, so one palette must serve both).
# Threshold/status colors (green/yellow/orange/red) remain strictly semantic.
PALETTE = ["#00bcd4", "#42a5f5", "#c47b06", "#9256b8"]
ACCENT = PALETTE[0]
# Swiftships brand (extracted from deploy/branding wordmark + website
# theme): deep navy #08243f, steel blue #1e6692. Healthy/OK states use
# the steel blue so the ops wall reads as calm brand color; yellow/
# orange/red remain strictly semantic alarm colors.
SWIFT_NAVY = "#08243f"
# Theme matched to the vessel control UI (ENGINE STATUS widget):
# cyan #00bcd4 labels/accents, white values, navy surfaces.
OK = "#00bcd4"        # healthy VALUE text / arcs (control-UI cyan)
TILE_OK = "#175d8d"   # healthy STATUS-TILE background (deep azure navy)

import re as _re
_FIELD_RE = _re.compile(r'_field == "(\w+)"')

_id = [0]


def nid():
    _id[0] += 1
    return _id[0]


# Series are named by their field alone: the measurement / vessel / host
# tags would otherwise be appended to every legend entry as labels
# ("heading alpha", {name="autonomy", vessel="alpha"}).
CLEAN_LABELS = ('  |> drop(fn: (column) => column =~ '
                '/^(_start|_stop|_measurement|vessel|host|instance|source|engine_type|engine_model)$/)\n')

# Fixed units: Grafana's "lengthm" auto-scales to km above 1000 m, which
# operators must never see. Short ranges stay in metres, long ones are
# converted to nautical miles in the query (M_TO_NM).
UNIT_M = "suffix: m"
UNIT_NM = "suffix: nm"
M_TO_NM = '  |> map(fn: (r) => ({r with _value: r._value / 1852.0}))\n'


# Replay: the raw bucket keeps 30 days; older windows are served from the
# 1-minute archive (telemetry_1m, downsampled by the installer's task).
ARCHIVE_BUCKET = ('bucket = if int(v: now()) - int(v: v.timeRangeStart) > int(v: 30d) '
                  'then "telemetry_1m" else "telemetry"\n')


def flux(measurement, fields, bucket="telemetry", every='v.windowPeriod', fn="mean",
         extra="", last=False, nm=False, archive=False):
    if isinstance(fields, str):
        fields = [fields]
    fieldset = " or ".join(f'r._field == "{f}"' for f in fields)
    q = ((ARCHIVE_BUCKET + 'from(bucket: bucket)\n') if archive
         else f'from(bucket: "{bucket}")\n')
    q += (f'  |> range(start: v.timeRangeStart, stop: v.timeRangeStop)\n'
          f'  |> filter(fn: (r) => r._measurement == "{measurement}" and ({fieldset}))\n'
          f'  |> filter(fn: (r) => r.vessel == "${{vessel}}")\n')
    if extra:
        q += extra + "\n"
    if last:
        q += '  |> last()\n'
    else:
        q += f'  |> aggregateWindow(every: {every}, fn: {fn}, createEmpty: false)\n'
    if nm:
        q += M_TO_NM
    return q + CLEAN_LABELS


# Vessel marker: Grafana's WebGL-drawn triangle (a heading arrow) in the
# Swiftships-UI red, rotated by heading, name label above. Keep the built-in
# shapes (circle, square, triangle): Grafana 13.2 draws any custom SVG marker
# through its WebGL icon path as a black square (seen on 13.2.1 with the
# boat SVG and on 13.2.3 with a map pin, usv-722 2026-10-07). The own-ship
# pin is used on the Traffic View instead, where ECharts draws it.
VESSEL_MARKER = {
    "size": {"fixed": 14},
    "color": {"fixed": "#d9342b"},
    "opacity": 1,
    "symbol": {"fixed": "img/icons/marker/triangle.svg", "mode": "fixed"},
    # Scalar dimension: Grafana applies value % max, so max is required
    # (without it the rotation is NaN and the WebGL marker is not drawn).
    "rotation": {"field": "heading", "mode": "mod", "min": -360,
                 "max": 360, "fixed": 0},
    "text": {"field": "vessel", "mode": "field"},
    "textConfig": {"fontSize": 13, "offsetX": 0, "offsetY": -18,
                   "textAlign": "center", "textBaseline": "middle"},
}


def camera_offline_query():
    """Enabled Frigate cameras that are not streaming: enabled - streaming
    from the latest camera_summary row (one number for a status tile)."""
    return ('from(bucket: "telemetry")\n'
            '  |> range(start: v.timeRangeStart, stop: v.timeRangeStop)\n'
            '  |> filter(fn: (r) => r._measurement == "camera_summary" and (r._field == "enabled" or r._field == "streaming"))\n'
            '  |> filter(fn: (r) => r.vessel == "${vessel}")\n'
            '  |> last()\n'
            '  |> group()\n'
            '  |> pivot(rowKey: ["vessel"], columnKey: ["_field"], valueColumn: "_value")\n'
            '  |> filter(fn: (r) => exists r.enabled and exists r.streaming)\n'
            '  |> map(fn: (r) => ({_value: r.enabled - r.streaming}))\n')


def position_query():
    """Latest lat/lon/heading of the selected vessel in one row (markers
    layer: one heading-rotated vessel symbol, not one dot per track point)."""
    return ('from(bucket: "telemetry")\n'
            '  |> range(start: v.timeRangeStart, stop: v.timeRangeStop)\n'
            '  |> filter(fn: (r) => r._measurement == "nav" and (r._field == "lat" or r._field == "lon" or r._field == "heading"))\n'
            '  |> filter(fn: (r) => r.vessel == "${vessel}")\n'
            '  |> last()\n'
            '  |> group()\n'
            '  |> pivot(rowKey: ["vessel"], columnKey: ["_field"], valueColumn: "_value")\n'
            '  |> filter(fn: (r) => exists r.lat and exists r.lon)\n'
            '  |> keep(columns: ["vessel", "lat", "lon", "heading"])\n')


def base_query():
    """Return base of the comms-loss failsafe (mission.base_lat / base_lon,
    from BASE_COORDS) as one lat/lon row for the map."""
    return ('from(bucket: "telemetry")\n'
            '  |> range(start: v.timeRangeStart, stop: v.timeRangeStop)\n'
            '  |> filter(fn: (r) => r._measurement == "mission" and (r._field == "base_lat" or r._field == "base_lon"))\n'
            '  |> filter(fn: (r) => r.vessel == "${vessel}")\n'
            '  |> last()\n'
            '  |> group()\n'
            '  |> pivot(rowKey: ["vessel"], columnKey: ["_field"], valueColumn: "_value")\n'
            '  |> filter(fn: (r) => exists r.base_lat and exists r.base_lon)\n'
            '  |> map(fn: (r) => ({lat: r.base_lat, lon: r.base_lon}))\n')


# Great-circle distance from the vessel's last position to the return base,
# in nautical miles.
base_distance_query = (
    'import "math"\n'
    'from(bucket: "telemetry")\n'
    '  |> range(start: v.timeRangeStart, stop: v.timeRangeStop)\n'
    '  |> filter(fn: (r) => (r._measurement == "nav" and (r._field == "lat" or r._field == "lon"))\n'
    '      or (r._measurement == "mission" and (r._field == "base_lat" or r._field == "base_lon")))\n'
    '  |> filter(fn: (r) => r.vessel == "${vessel}")\n'
    '  |> last()\n'
    '  |> group()\n'
    '  |> pivot(rowKey: ["vessel"], columnKey: ["_field"], valueColumn: "_value")\n'
    '  |> filter(fn: (r) => exists r.lat and exists r.lon and exists r.base_lat and exists r.base_lon)\n'
    '  |> map(fn: (r) => {\n'
    '      p1 = r.lat * math.pi / 180.0\n'
    '      p2 = r.base_lat * math.pi / 180.0\n'
    '      dp = p2 - p1\n'
    '      dl = (r.base_lon - r.lon) * math.pi / 180.0\n'
    '      a = math.sin(x: dp / 2.0) * math.sin(x: dp / 2.0)\n'
    '          + math.cos(x: p1) * math.cos(x: p2) * math.sin(x: dl / 2.0) * math.sin(x: dl / 2.0)\n'
    '      return {_value: 2.0 * 6371008.8 * math.asin(x: math.sqrt(x: a)) / 1852.0}\n'
    '  })\n')


def vector_query():
    """Own-ship speed vector for the map (ECDIS style): a two-point line from
    the vessel to where it will be in 6 minutes at the current SOG, along
    COG (heading when COG is missing). Empty below 0.5 kn so a vessel
    alongside shows no vector. Flat-earth step, fine over a few nm."""
    return ('import "math"\n'
            'p = from(bucket: "telemetry")\n'
            '  |> range(start: v.timeRangeStart, stop: v.timeRangeStop)\n'
            '  |> filter(fn: (r) => r._measurement == "nav" and (r._field == "lat" or r._field == "lon" or r._field == "heading" or r._field == "cog" or r._field == "sog"))\n'
            '  |> filter(fn: (r) => r.vessel == "${vessel}")\n'
            '  |> last()\n'
            '  |> group()\n'
            '  |> pivot(rowKey: ["vessel"], columnKey: ["_field"], valueColumn: "_value")\n'
            '  |> filter(fn: (r) => exists r.lat and exists r.lon and exists r.sog and (exists r.cog or exists r.heading))\n'
            '  |> filter(fn: (r) => r.sog >= 0.5)\n'
            '  |> map(fn: (r) => ({r with crs: if exists r.cog then r.cog else r.heading}))\n'
            'a = p |> map(fn: (r) => ({seq: 0, lat: r.lat, lon: r.lon}))\n'
            'b = p |> map(fn: (r) => ({seq: 1,\n'
            '      lat: r.lat + r.sog / 600.0 * math.cos(x: r.crs * math.pi / 180.0),\n'
            '      lon: r.lon + r.sog / 600.0 * math.sin(x: r.crs * math.pi / 180.0) / math.cos(x: r.lat * math.pi / 180.0)}))\n'
            'union(tables: [a, b])\n'
            '  |> sort(columns: ["seq"])\n')


def geometry_query(kind):
    """Current planned route or operating region (grafana_bridge writes the
    vertex set with one timestamp and deletes the previous set first, so
    last() per vertex index is exactly the current geometry)."""
    return ('from(bucket: "telemetry")\n'
            '  |> range(start: -90d)\n'
            f'  |> filter(fn: (r) => r._measurement == "geometry" and r.kind == "{kind}")\n'
            '  |> filter(fn: (r) => r._field == "lat" or r._field == "lon" or r._field == "seq")\n'
            '  |> filter(fn: (r) => r.vessel == "${vessel}")\n'
            '  |> last()\n'
            '  |> group()\n'
            '  |> pivot(rowKey: ["idx"], columnKey: ["_field"], valueColumn: "_value")\n'
            '  |> filter(fn: (r) => exists r.lat and exists r.lon)\n'
            '  |> sort(columns: ["seq"])\n'
            '  |> map(fn: (r) => ({r with wp: string(v: int(v: r.seq) + 1)}))\n'
            '  |> keep(columns: ["seq", "wp", "lat", "lon"])\n')


# Map overlay layers shared by Navigation and Replay: operating region
# outline (cyan, like the console), planned route (green) with numbered
# waypoints, the vessel's track (red), the vessel itself and its 6-minute
# speed vector (lime line ahead of the bow, ECDIS / OpenCPN style).
TRACK_RED = "#e53935"
ROUTE_GREEN = "#2e7d32"
REGION_CYAN = "#00bcd4"
VECTOR_LIME = "#9be22d"
BASE_ORANGE = "#ff9830"


def overlay_layers(track_ref="A", vessel_ref="B", route_ref="C",
                   region_ref="D", vector_ref="E", base_ref=None):
    def coords():
        return {"mode": "coords", "latitude": "lat", "longitude": "lon"}
    # Return base of the comms-loss failsafe (BASE_COORDS): an orange square
    # labelled BASE, drawn under the vessel.
    base = [{"type": "markers", "name": "Return base",
             "location": coords(),
             "filterData": {"id": "byRefId", "options": base_ref},
             "config": {"style": {
                 "size": {"fixed": 9}, "color": {"fixed": BASE_ORANGE},
                 "opacity": 1,
                 "symbol": {"fixed": "img/icons/marker/square.svg", "mode": "fixed"},
                 "text": {"fixed": "BASE", "mode": "fixed"},
                 "textConfig": {"fontSize": 11, "offsetX": 0, "offsetY": -14,
                                "textAlign": "center", "textBaseline": "middle"},
             }, "showLegend": False}}] if base_ref else []
    return base + [
        {"type": "route", "name": "Operating region",
         "location": coords(),
         "filterData": {"id": "byRefId", "options": region_ref},
         "config": {"style": {"color": {"fixed": REGION_CYAN},
                              "lineWidth": 3, "opacity": 0.9}}},
        {"type": "route", "name": "Planned route",
         "location": coords(),
         "filterData": {"id": "byRefId", "options": route_ref},
         "config": {"style": {"color": {"fixed": ROUTE_GREEN},
                              "lineWidth": 2, "opacity": 0.9}}},
        {"type": "markers", "name": "Waypoints",
         "location": coords(),
         "filterData": {"id": "byRefId", "options": route_ref},
         "config": {"style": {
             "size": {"fixed": 6}, "color": {"fixed": ROUTE_GREEN},
             "opacity": 1,
             "symbol": {"fixed": "img/icons/marker/circle.svg", "mode": "fixed"},
             "text": {"field": "wp", "mode": "field"},
             "textConfig": {"fontSize": 11, "offsetX": 0, "offsetY": -12,
                            "textAlign": "center", "textBaseline": "middle"},
         }, "showLegend": False}},
        {"type": "route", "name": "Track",
         "location": coords(),
         "filterData": {"id": "byRefId", "options": track_ref},
         "config": {"style": {"color": {"fixed": TRACK_RED},
                              "lineWidth": 2, "opacity": 0.9}}},
        {"type": "route", "name": "Speed vector (6 min)",
         "location": coords(),
         "filterData": {"id": "byRefId", "options": vector_ref},
         "config": {"style": {"color": {"fixed": VECTOR_LIME},
                              "lineWidth": 3, "opacity": 1}}},
        {"type": "markers", "name": "Vessel",
         "location": coords(),
         "filterData": {"id": "byRefId", "options": vessel_ref},
         "config": {"style": VESSEL_MARKER, "showLegend": False}},
    ]


def track_query(every, archive=False):
    """lat/lon pivoted into one frame for the geomap route layer. The two
    fields arrive as separate tables, so they are merged (group()) before
    the pivot; pivoting first leaves lat and lon in different frames and
    aggregateWindow with no _value column errors out."""
    head = (ARCHIVE_BUCKET + 'from(bucket: bucket)\n') if archive \
        else 'from(bucket: "telemetry")\n'
    return (head +
            '  |> range(start: v.timeRangeStart, stop: v.timeRangeStop)\n'
            '  |> filter(fn: (r) => r._measurement == "nav" and (r._field == "lat" or r._field == "lon"))\n'
            '  |> filter(fn: (r) => r.vessel == "${vessel}")\n'
            f'  |> aggregateWindow(every: {every}, fn: last, createEmpty: false)\n'
            '  |> group()\n'
            '  |> pivot(rowKey: ["_time"], columnKey: ["_field"], valueColumn: "_value")\n'
            '  |> filter(fn: (r) => exists r.lat and exists r.lon)\n'
            '  |> keep(columns: ["_time", "lat", "lon"])\n'
            '  |> sort(columns: ["_time"])\n')


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
          mappings=None, custom=None, no_value=None):
    defaults = {"color": {"mode": "palette-classic"}}
    if no_value:
        defaults["noValue"] = no_value
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
                              "fillOpacity": 14, "showPoints": "never",
                              "spanNulls": 60000,
                              "gradientMode": "opacity",
                              "lineInterpolation": "smooth"}
    p = {
        "id": nid(), "type": ptype, "title": title, "datasource": DS,
        "gridPos": {"x": x, "y": y, "w": w, "h": h},
        "targets": targets,
        "fieldConfig": {"defaults": defaults, "overrides": overrides or []},
        "options": options or {},
    }
    return p


# String fields shown on stat tiles. Grafana's stat panel reduces NUMERIC
# fields only by default (reduceOptions.fields = ""), so a string value
# renders as "No data" even though the query returns it. For these the
# query keeps only _value and the panel is told to reduce every field.
STRING_FIELDS = {
    "state", "state_external", "helm_state", "mission_name", "mission_id",
    "route_name", "allstop_reason", "enc_status", "checklist_pending", "checklist_by",
    "blocking", "profile", "name", "evidence", "description", "item", "clients", "active_behavior", "station_keep", "comms_failsafe",
    "entry_gate", "opr_status", "helm_summary", "arrival_state",
    "corridor_status", "crash_stop_status", "region_warning", "wpt_stat",
    "fix_type", "charging_status", "message", "worst_contact", "reason",
    "encounter", "type", "contact",
}


def _is_string_query(query):
    fields = _FIELD_RE.findall(query)
    return bool(fields) and all(f in STRING_FIELDS for f in fields)


NEUTRAL = "#3d434b"   # "no data" / idle / none: never a verdict colour


def neutral_base(thresholds, floor=0):
    """Thresholds whose base step (what Grafana paints when there is NO
    data) is neutral grey, with the original base colour starting at
    `floor` instead. A tile with no data then reads grey, not red or OK,
    while every real value keeps its colour. Only for values >= floor."""
    base, *rest = thresholds
    return [(None, NEUTRAL), (floor, base[1])] + rest


def stat(title, query, x, y, w=4, h=4, unit=None, thresholds=None,
         mappings=None, text_mode="auto", decimals=None,
         color_mode="value", graph_mode="none", string_value=False,
         no_value=None, value_size=None):
    """color_mode='background' renders a solid status tile (ops wall look);
    graph_mode='area' adds a trend sparkline behind numeric values;
    no_value replaces Grafana's "No data" (e.g. "N/A" for hardware that is
    not fitted on this vessel). A tile driven by value mappings gets a
    neutral base colour: mappings colour the known states, and "No data"
    must not look like a healthy state."""
    if thresholds is None and mappings:
        thresholds = [(None, NEUTRAL)]
    # A solid status tile with nothing to show reads "N/A" in its base
    # colour; Grafana's own "No data" placeholder ignores the thresholds
    # and paints the tile as if healthy (seen on usv-722).
    if no_value is None and color_mode == "background":
        no_value = "N/A"
    # Grafana paints "No data" in the base threshold colour. A tile whose
    # base is an alarm colour (low fuel red) would show missing data as an
    # alarm: the base becomes neutral and the alarm colour starts at the
    # lowest real value instead.
    if thresholds and thresholds[0][0] is None and thresholds[0][1] in ("red", "yellow"):
        thresholds = [(None, NEUTRAL), (-1e9, thresholds[0][1])] + list(thresholds[1:])
    reduce_opts = {"calcs": ["lastNotNull"]}
    is_str = string_value or _is_string_query(query)
    if is_str:
        query = query + '  |> keep(columns: ["_value"])\n'
        reduce_opts["fields"] = "/.*/"
    # Operator-screen density: capped value size so a 3-row tile reads as a
    # readout, not a billboard (Grafana otherwise scales text to the tile).
    return panel("stat", title, [target(query)], x, y, w, h, unit=unit,
                 decimals=decimals,
                 thresholds=thresholds or [(None, OK)],
                 mappings=mappings,
                 options={"reduceOptions": reduce_opts,
                          "textMode": text_mode, "graphMode": graph_mode,
                          "colorMode": color_mode,
                          "justifyMode": "auto",
                          "text": {"valueSize": value_size or ((22 if is_str else 26)
                                                 if h <= 3 else 34),
                                   "titleSize": 12}},
                 no_value=no_value)


def gauge(title, query, x, y, w=4, h=5, unit="percent", min_=0, max_=100,
          thresholds=None):
    return panel("gauge", title, [target(query)], x, y, w, h, unit=unit,
                 min_=min_, max_=max_,
                 thresholds=thresholds or [(None, OK)],
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


# Operator wording for the derived mission states (stored values stay the
# CamelCase identifiers used by the bridge, alerts and docs).
STATE_LABELS = {
    "StationKeeping": "Station Keeping", "ReturnToBase": "Return to Base",
    "EnteringRegion": "Entering Region", "AwaitingRearm": "Awaiting Re-arm",
    "CommsLost": "Comms Lost", "HoldingAtBase": "Holding at Base",
    "AllStop": "ALL STOP",
    # value written by bridges before v1.0.1 (historical data / replay)
    "ReturnToHome": "Return to Base",
}


def state_map(pairs):
    """Value mappings for string states; display text from STATE_LABELS."""
    return [{"type": "value",
             "options": {k: {"text": STATE_LABELS.get(k, k), "color": v,
                             "index": i}
                         for i, (k, v) in enumerate(pairs)}}]


def updown_map(down_text="OFFLINE", up_text="ONLINE"):
    return [{"type": "value", "options": {
        "0": {"text": down_text, "color": "red", "index": 0},
        "1": {"text": up_text, "color": TILE_OK, "index": 1}}}]


def alert_map(ok_text, alert_text, alert_color="red"):
    """Inverse of updown_map: 0 is the good state, 1 the alert state."""
    return [{"type": "value", "options": {
        "0": {"text": ok_text, "color": TILE_OK, "index": 0},
        "1": {"text": alert_text, "color": alert_color, "index": 1}}}]


def value_color_map(triples):
    """[(value, text, color), ...] -> Grafana value mappings."""
    return [{"type": "value",
             "options": {str(v): {"text": t, "color": c, "index": i}
                         for i, (v, t, c) in enumerate(triples)}}]


# ------------------------------------------------------------------
# Analog instruments (Business Charts / Apache ECharts panel).
#
# True needle gauges are not part of core Grafana; the signed, maintained
# volkovlabs-echarts-panel plugin renders them (installed by
# deploy/install.sh). Regenerate WITHOUT instruments for a plugin-free
# install:  python3 tools/gen_dashboards.py --classic
# Each instrument panel also carries a "swiftMeta" block (ignored by
# Grafana) that tools/render_preview.py uses to draw faithful previews.
# ------------------------------------------------------------------
import sys as _sys
ANALOG = "--classic" not in _sys.argv

# Semantic band colors (Grafana classic palette, softened for gauge faces)
# + the teal/blue instrument accents shared with the vessel control UI.
ZGREEN = "rgba(115,191,105,0.90)"
ZYELLOW = "rgba(250,222,42,0.90)"
ZORANGE = "rgba(255,152,48,0.92)"
ZRED = "rgba(242,73,92,0.92)"
ZNEUTRAL_D = "rgba(204,212,220,0.16)"

ECH_PRELUDE = """\
const val = (ref) => {
  for (const s of context.panel.data.series) {
    if (ref && s.refId !== ref) { continue; }
    for (const f of s.fields) {
      if (f.type === 'number') {
        const a = f.values && f.values.toArray ? f.values.toArray() : f.values;
        if (a && a.length) { return a[a.length - 1]; }
      }
    }
  }
  return null;
};
const dark = context.grafana.theme.isDark;
const C = {
  rail: dark ? '#1c2a3d' : 'rgba(36,41,46,0.10)',
  tick: dark ? 'rgba(124,156,176,0.55)' : 'rgba(36,41,46,0.45)',
  label: dark ? '#7c9cb0' : '#57606a',
  needle: dark ? '#e8ebee' : '#24292e',
  pivot: '#00bcd4',
  text: dark ? '#ffffff' : '#24292e',
  sub: dark ? '#00bcd4' : '#1e6692',
  accent: '#00bcd4',
  accent2: '#42a5f5',
  // Dial face (radial gradient, centre to rim), meter rail, tick marks and
  // numerals, and the glow of the fill arc.
  face0: dark ? '#1c2634' : '#f7f8fa',
  face1: dark ? '#0a0e14' : '#d9dee5',
  mrail: dark ? 'rgba(124,156,176,0.16)' : 'rgba(36,41,46,0.10)',
  mtick: dark ? '#e6ecf2' : '#2b3138',
  mlabel: dark ? '#d2dbe4' : '#3b4249',
  glow: 'rgba(0,188,212,0.55)',
};
// Dial face shared by the needle instruments: a silent pie series drawn
// under the gauge. r = radius as a percentage of the panel's shorter side.
const face = (r) => ({
  type: 'pie', silent: true, radius: [0, r], center: ['50%', '50%'], z: 0,
  label: { show: false }, labelLine: { show: false },
  data: [{ value: 1, itemStyle: { color: { type: 'radial', x: 0.5, y: 0.5, r: 0.5,
    colorStops: [{ offset: 0, color: C.face0 }, { offset: 1, color: C.face1 }] } } }],
});
"""


def _zone_fracs(min_, max_, zones):
    return ", ".join(f"[{(hi - min_) / (max_ - min_):.4f}, '{c}']"
                     for hi, c in zones)


# Needle meters size themselves to their panel (SELF_SCALING marker: big_dial
# leaves them alone). Geometry follows the chart height, 1x at 260 px (the
# Engine page dials on a 1080p screen), from 0.6x on the small Vessel Health
# cluster to 3x on a wall display; fonts keep a legible floor, so the
# readout never runs into the label under it.
SCALE_JS = (
    "// SELF_SCALING\n"
    "const Zh = (() => { try { const h = context.panel.chart.getHeight();\n"
    "  return h > 0 ? h : 260; } catch (e) { return 260; } })();\n"
    "const G = Math.max(0.6, Math.min(3, Zh / 260));\n"
    "const F = (px, lo) => Math.round(Math.max(lo, px * G));\n")


def meter_js(min_, max_, unit, zones=None, decimals=0, split=6, sub="",
             label_js=None, detail_js=None):
    """ECharts option code for a 240° analog meter: dark radial dial face,
    a glowing teal arc that fills to the value over a faint rail, the
    alarm bands as a thin ring outside the arc, tick marks and numerals
    inside it, a hairline needle and a large digital readout with the unit
    in small type. zones = [(upper_bound, color), ...]; None -> no ring.
    A centre-zero scale (min < 0 < max, heading error) keeps the needle
    and the ring but not the fill, which would start at the negative end.
    detail_js replaces the readout formatter (plain text, no rich spans)."""
    # The fill arc is always drawn (show is a constant per scale) and made
    # transparent when there is nothing to fill. Toggling progress.show
    # between refreshes crashes ECharts 5.6 (GaugeView reads the previous
    # arc, which was never created: "Cannot read properties of undefined
    # (reading '0')"), e.g. no data then data, or 0 rpm then the engine
    # starting. A centre-zero scale never draws it.
    has_fill = "false" if (min_ < 0 < max_) else "true"
    fill_on = (f"!nodata && v > {min_ + (max_ - min_) * 0.004:g}"
               if not (min_ < 0 < max_) else "false")
    ring = ""
    if zones:
        ring = f"""\
  {{
    type: 'gauge', silent: true, min: {min_}, max: {max_},
    startAngle: 210, endAngle: -30,
    center: ['50%', '50%'], radius: '86%', z: 1,
    axisLine: {{ lineStyle: {{ width: 4 * G,
                 color: [{_zone_fracs(min_, max_, zones)}] }} }},
    splitLine: {{ show: false }}, axisTick: {{ show: false }},
    axisLabel: {{ show: false }}, pointer: {{ show: false }},
    detail: {{ show: false }}, data: [{{ value: v }}],
  }},
"""
    label_fmt = f", formatter: {label_js}" if label_js else ""
    if detail_js:
        detail = (f"detail: {{ valueAnimation: true, color: C.text, "
                  f"fontSize: F(22, 13), fontWeight: 700, offsetCenter: [0, '70%'], "
                  f"formatter: nodata ? (() => 'N/A') : (() => ({detail_js})(raw)) }}")
    else:
        detail = (f"detail: {{ valueAnimation: true, offsetCenter: [0, '70%'],\n"
                  f"              formatter: nodata ? (() => '{{v|N/A}}')\n"
                  f"                : (() => '{{v|' + raw.toFixed({decimals}) + "
                  f"'}}{{u| {unit}}}'),\n"
                  f"              rich: {{ v: {{ color: C.text, fontSize: F(22, 13), "
                  f"fontWeight: 700 }},\n"
                  f"                      u: {{ color: C.mlabel, fontSize: F(11, 8), "
                  f"fontWeight: 600 }} }} }}")
    return ECH_PRELUDE + SCALE_JS + f"""\
let v = val();
const nodata = (v === null || isNaN(v));
if (nodata) {{ v = {min_}; }}
// The needle stays on the scale; the readout shows the measured value (a
// coolant of 31 C on a 40 to 120 scale reads 31, the needle rests at 40).
const raw = v;
v = Math.max({min_}, Math.min({max_}, v));
return {{
  series: [face('84%'),
{ring}  {{
    type: 'gauge', min: {min_}, max: {max_},
    startAngle: 210, endAngle: -30,
    center: ['50%', '50%'], radius: '78%', z: 2,
    axisLine: {{ roundCap: true, lineStyle: {{ width: 12 * G, color: [[1, C.mrail]] }} }},
    progress: {{ show: {has_fill}, width: 12 * G, roundCap: true,
                itemStyle: {{ color: {{ type: 'linear', x: 0, y: 0, x2: 1, y2: 0,
                                      colorStops: [{{ offset: 0, color: C.accent2 }},
                                                   {{ offset: 1, color: C.accent }}] }},
                             opacity: ({fill_on}) ? 1 : 0,
                             shadowColor: C.glow, shadowBlur: 12 * G }} }},
    splitNumber: {split},
    // Major ticks just inside the rail, numerals clear of them (a tick
    // touching a numeral read like a minus sign).
    splitLine: {{ length: 4 * G, distance: 2 * G,
                 lineStyle: {{ color: C.mtick, width: 1.5 * G }} }},
    axisTick: {{ show: false }},
    axisLabel: {{ distance: 13 * G + 4, color: C.mlabel, fontSize: F(11, 8),
                 fontWeight: 500{label_fmt} }},
    pointer: {{ show: !nodata, icon: 'path://M-1.5,0 L0,-100 L1.5,0 L0,10 Z',
               length: '52%', width: 4 * G,
               itemStyle: {{ color: C.text, shadowColor: 'rgba(0,0,0,0.5)',
                            shadowBlur: 6 }} }},
    anchor: {{ show: true, size: 12 * G, showAbove: true,
              itemStyle: {{ color: C.face1, borderColor: C.accent,
                           borderWidth: 2 * G }} }},
    title: {{ show: true, offsetCenter: [0, '93%'], color: C.sub,
             fontSize: F(10, 8), fontWeight: 700 }},
    {detail},
    data: [{{ value: v, name: '{sub}' }}],
  }}],
}};
"""


def compass_js():
    """Marine compass: heading needle (teal) + COG needle (blue)."""
    return ECH_PRELUDE + """\
let hdg = val('A'); if (hdg === null || isNaN(hdg)) { hdg = 0; }
let cog = val('B'); if (cog === null || isNaN(cog)) { cog = hdg; }
hdg = ((hdg % 360) + 360) % 360;
cog = ((cog % 360) + 360) % 360;
const card = {0: 'N', 90: 'E', 180: 'S', 270: 'W', 360: ''};
const base = {
  type: 'gauge', min: 0, max: 360, startAngle: 90, endAngle: -270,
  clockwise: true, center: ['50%', '54%'], radius: '86%',
};
return {
  series: [Object.assign({}, base, {
    axisLine: { lineStyle: { width: 4, color: [[1, C.rail]] } },
    splitNumber: 12,
    splitLine: { length: 10, distance: -10,
                 lineStyle: { color: C.tick, width: 2 } },
    axisTick: { splitNumber: 3, length: 5, distance: -5,
                lineStyle: { color: C.tick, width: 1 } },
    axisLabel: { distance: 13, fontSize: 11, fontWeight: 600,
                 color: C.label,
                 formatter: (x) => (card[x] !== undefined ? card[x] : x) },
    pointer: { width: 5, length: '66%',
               itemStyle: { color: C.accent,
                            shadowColor: 'rgba(0,0,0,0.45)',
                            shadowBlur: 6, shadowOffsetY: 2 } },
    anchor: { show: true, size: 11, showAbove: true,
              itemStyle: { color: C.pivot, borderColor: C.needle,
                           borderWidth: 2 } },
    // Readouts sit outside the dial (graphic below): inside it they ran
    // into the 120 / 240 labels on a 5-column panel.
    title: { show: false }, detail: { show: false },
    data: [{ value: hdg }],
  }), Object.assign({}, base, {
    axisLine: { show: false }, splitLine: { show: false },
    axisTick: { show: false }, axisLabel: { show: false },
    pointer: { width: 3, length: '52%',
               itemStyle: { color: C.accent2, opacity: 0.9 } },
    anchor: { show: false }, title: { show: false },
    detail: { show: false },
    data: [{ value: cog }],
  })],
  graphic: [
    { type: 'text', left: 8, top: 4, style: { text: 'HDG', fill: C.label, fontSize: 10, fontWeight: 600 } },
    { type: 'text', left: 8, top: 17, style: { text: hdg.toFixed(0) + '\\u00b0', fill: C.accent, fontSize: 18, fontWeight: 700 } },
    { type: 'text', right: 8, top: 4, style: { text: 'COG', fill: C.label, fontSize: 10, fontWeight: 600, align: 'right' } },
    { type: 'text', right: 8, top: 17, style: { text: cog.toFixed(0) + '\\u00b0', fill: C.accent2, fontSize: 18, fontWeight: 700, align: 'right' } },
  ],
};
"""


def rudder_js():
    """Rudder angle indicator, center-zero, port red / starboard green.

    refId A = rudder_actual (deg, steering actuator feedback on CAN).
    refId B = rudder_cmd (DESIRED_RUDDER: PERCENT of full rudder on the
    vessel's iCAN, degrees in the simulator), shown only when there is no
    actuator feedback, without a degree sign and labelled COMMAND.
    """
    return ECH_PRELUDE + SCALE_JS + """\
let v = val('A'); let actual = true;
if (v === null || isNaN(v)) { v = val('B'); actual = false; }
if (v === null || isNaN(v)) { v = 0; }
v = Math.max(-35, Math.min(35, v));
return {
  series: [face('84%'), {
    type: 'gauge', min: -35, max: 35,
    startAngle: 210, endAngle: -30,
    center: ['50%', '50%'], radius: '78%', z: 2,
    axisLine: { lineStyle: { width: 12 * G, color: [
      [0.5, 'rgba(242,73,92,0.85)'], [1, 'rgba(115,191,105,0.85)'] ] } },
    splitNumber: 7,
    splitLine: { length: 4 * G, distance: 2 * G,
                 lineStyle: { color: C.mtick, width: 1.5 * G } },
    axisTick: { show: false },
    axisLabel: { distance: 13 * G + 4, color: C.mlabel, fontSize: F(11, 8), fontWeight: 500,
                 // The \u00b135 end labels sat under the PORT / STBD readout.
                 formatter: (x) => (Math.abs(x) >= 35 ? '' : Math.abs(x)) },
    pointer: { icon: 'path://M-1.5,0 L0,-100 L1.5,0 L0,10 Z',
               length: '52%', width: 4 * G,
               itemStyle: { color: C.text, shadowColor: 'rgba(0,0,0,0.5)',
                            shadowBlur: 6 } },
    anchor: { show: true, size: 12 * G, showAbove: true,
              itemStyle: { color: C.face1, borderColor: C.accent,
                           borderWidth: 2 * G } },
    title: { show: true, offsetCenter: [0, '93%'], color: C.sub,
             fontSize: F(10, 8), fontWeight: 700 },
    detail: { valueAnimation: true, color: C.text, fontSize: F(22, 13),
              fontWeight: 700, offsetCenter: [0, '70%'],
              formatter: (x) => (x < -0.5 ? 'PORT ' :
                                 (x > 0.5 ? 'STBD ' : '')) +
                                Math.abs(x).toFixed(0) +
                                (actual ? '\\u00b0' : '') },
    data: [{ value: v, name: actual ? 'PORT \\u2039 RUDDER \\u203a STBD'
                                     : 'PORT \\u2039 COMMAND \\u203a STBD' }],
  }],
};
"""


def instrument(title, targets_, x, y, w, h, js, meta):
    """volkovlabs-echarts-panel instrument. swiftMeta is preview-only."""
    return {
        "id": nid(), "type": "volkovlabs-echarts-panel", "title": title,
        "datasource": DS,
        "gridPos": {"x": x, "y": y, "w": w, "h": h},
        "targets": targets_,
        "options": {"getOption": js, "renderer": "svg"},
        "fieldConfig": {"defaults": {}, "overrides": []},
        "swiftMeta": meta,
    }


def meter(title, query, x, y, w, h, min_, max_, unit, zones=None,
          decimals=0, split=6, sub="", label_js=None, detail_js=None,
          preview=None, preview_labels=None, fallback_thresholds=None):
    """Analog meter, or a native gauge when --classic."""
    if ANALOG:
        meta = {"kind": "meter", "min": min_, "max": max_, "unit": unit,
                "zones": zones or [], "decimals": decimals, "split": split,
                "sub": sub, "preview": preview if preview is not None
                else min_}
        if preview_labels:
            meta["labels"] = preview_labels
        return instrument(
            title, [target(query)], x, y, w, h,
            meter_js(min_, max_, unit, zones, decimals, split, sub,
                     label_js, detail_js), meta)
    thr = fallback_thresholds or [(None, OK)]
    return gauge(title, query, x, y, w, h, unit="none", min_=min_,
                 max_=max_, thresholds=thr)


# Per-engine-type instrument scales. ENGINE_TYPE in /etc/usv/usv.env becomes
# the engine_type tag on every engine point and the hidden engine_type
# dashboard variable; typed_meter() builds one dial whose scale is chosen
# from that variable when the panel renders, so one usv-health.json serves
# every hull. Sources: the Yamaha outboard set is the one shipped so far;
# Mercury Diesel 2.0L: 4000 rpm at wide-open throttle (Mercury spec sheet),
# oil pressure 240 to 640 kPa and coolant 77 to 83 °C in the 29 May 2026 sea
# trials (docs/requirements-usv-725.md, section 2).
# Coolant bands per engine type, as the alert rules (tools/engine_limits.py:
# Engine Temperature High from the warning limit, Engine Over Temperature
# from the overheating limit).
def _coolant(engine_type):
    warn, crit = engine_limits.coolant(engine_type)
    return dict(min_=40, max_=120, zones=[(warn, ZGREEN), (crit, ZYELLOW), (120, ZRED)],
                split=4)
ENGINE_TYPES = {
    "yamaha_outboard": {
        "label": "Yamaha outboard",
        "tach": dict(min_=0, max_=6000, split=6,
                     zones=[(4500, ZGREEN), (5500, ZYELLOW), (6000, ZRED)],
                     labels={i * 1000: str(i) for i in range(7)}),
        "coolant": _coolant("yamaha_outboard"),
        # Tick every 100 kPa, numeral every 200 (seven numerals crowd the
        # small Vessel Health dial).
        "oil": dict(min_=0, max_=600, split=6,
                    zones=[(150, ZRED), (250, ZYELLOW), (520, ZGREEN), (600, ZYELLOW)],
                    label_js="(x) => (x % 200 === 0 ? x : '')",
                    labels={0: "0", 200: "200", 400: "400", 600: "600"}),
        "boost": dict(min_=0, max_=300, split=6,
                      zones=[(250, ZGREEN), (280, ZYELLOW), (300, ZRED)]),
    },
    "mercury_diesel": {
        "label": "Mercury Diesel",
        "tach": dict(min_=0, max_=5000, split=5,
                     zones=[(4000, ZGREEN), (4400, ZYELLOW), (5000, ZRED)],
                     labels={i * 1000: str(i) for i in range(6)}),
        "coolant": _coolant("mercury_diesel"),
        "oil": dict(min_=0, max_=800, split=4,
                    zones=[(150, ZRED), (200, ZYELLOW), (700, ZGREEN), (800, ZYELLOW)]),
        # Intake manifold pressure (J1939 IC1): 102 to 286 kPa in the sea
        # trials; Mercury fault J124 Boost_Press_High above the engine's limit.
        "boost": dict(min_=0, max_=300, split=6,
                      zones=[(250, ZGREEN), (280, ZYELLOW), (300, ZRED)]),
    },
}
DEFAULT_ENGINE_TYPE = "yamaha_outboard"   # unknown or empty engine_type

TYPED_SELECT_JS = (
    "const __ET = (() => { try {\n"
    "  return String(context.grafana.replaceVariables('${engine_type}') || '');\n"
    "} catch (e) { return ''; } })();\n")


def typed_meter(title, query, x, y, w, h, key, unit, decimals=0, sub="",
                label_js=None, preview=None, fallback_thresholds=None):
    """Analog meter whose min / max / colour bands come from
    ENGINE_TYPES[<engine_type variable>][key] at render time (key = tach |
    coolant | oil). The --classic gauge and the preview use the default type."""
    default = ENGINE_TYPES[DEFAULT_ENGINE_TYPE][key]
    if not ANALOG:
        return meter(title, query, x, y, w, h, default["min_"], default["max_"],
                     unit, zones=default["zones"], decimals=decimals,
                     split=default["split"], sub=sub,
                     label_js=default.get("label_js", label_js),
                     preview=preview, fallback_thresholds=fallback_thresholds)
    bodies = []
    for name, spec in ENGINE_TYPES.items():
        sp = spec[key]
        body = meter_js(sp["min_"], sp["max_"], unit, sp["zones"], decimals,
                        sp["split"], sub,
                        sp.get("label_js", label_js))[len(ECH_PRELUDE):]
        bodies.append(f"  '{name}': () => {{\n{body}  }},\n")
    js = (ECH_PRELUDE + TYPED_SELECT_JS + "const __BY = {\n" + "".join(bodies)
          + "};\n" + f"return (__BY[__ET] || __BY['{DEFAULT_ENGINE_TYPE}'])();\n")
    meta = {"kind": "meter", "min": default["min_"], "max": default["max_"],
            "unit": unit, "zones": default["zones"], "decimals": decimals,
            "split": default["split"], "sub": sub,
            "preview": preview if preview is not None else default["min_"],
            "typed": {name: {"min": sp[key]["min_"], "max": sp[key]["max_"],
                             "zones": sp[key]["zones"], "split": sp[key]["split"]}
                      for name, sp in ENGINE_TYPES.items()}}
    if default.get("labels"):
        meta["labels"] = default["labels"]
    return instrument(title, [target(query)], x, y, w, h, js, meta)



# Fixed tab order on every dashboard (Grafana's tag-based link list sorts
# alphabetically and drops the current dashboard, so the row shifted from
# page to page). Operational sequence: readiness (also Grafana's home
# dashboard, see deploy/install.sh) and mission first, support last.
TAB_ORDER = [
    ("usv-readiness", "Readiness"), ("usv-mission", "Mission"),
    ("usv-navigation", "Navigation"),
    ("usv-autopilot", "Autopilot"), ("usv-health", "Vessel Health"),
    ("usv-engine", "Engine"),
    ("usv-canbus", "CAN Bus"), ("usv-traffic", "Traffic"),
    ("usv-camera", "Cameras"), ("usv-comms", "Comms"),
    ("usv-system", "System"), ("usv-replay", "Replay"),
    ("usv-alarms", "Alarms"), ("usv-audit", "Audit"), ("usv-fleet", "Fleet"),
]


# Every live page refreshes once a second (Replay has no auto refresh).
# Theme picker at the top of every dashboard (v1.2.33). The brand.sh script
# in index.html reads the var-theme URL parameter: Light and Dark are
# Grafana's own themes, Navy restyles the dark theme in the Swiftships navy
# (deploy/brand.sh, swiftships-theme-js). The choice is remembered per
# browser and carried between tabs with the other variables.
THEME_VAR = {
    "name": "theme", "label": "Theme", "type": "custom",
    "query": "Dark : dark,Navy : navy,Light : light",
    "current": {"text": "Dark", "value": "dark"},
    "options": [{"text": t, "value": v, "selected": v == "dark"} for t, v in [
        ("Dark", "dark"), ("Navy", "navy"), ("Light", "light")]],
    "hide": 0, "includeAll": False, "multi": False,
}


def dashboard(uid, title, panels, tags, refresh="1s", time_from="now-30m",
              fleet=False):
    """fleet=True makes the vessel variable multi-select with "All" so rows
    can repeat per vessel (Fleet Overview)."""
    vessel_var = {
        "name": "vessel", "label": "Vessel", "type": "query",
        "datasource": DS,
        "query": ('import "influxdata/influxdb/schema"\n'
                  'schema.tagValues(bucket: "telemetry", tag: "vessel")'),
        "refresh": 2, "current": {"text": "usv-01", "value": "usv-01"},
        "includeAll": False, "multi": False,
    }
    if fleet:
        # Own variable name: the tab links carry var-vessel=<selection>
        # from the other dashboards, which must not narrow the fleet view.
        vessel_var.update({"name": "fleet_vessel", "label": "Vessels",
                           "includeAll": True, "multi": True,
                           "current": {"text": "All", "value": "$__all"}})
    # Engine type of the selected vessel: the engine_type tag every bridge
    # writes from ENGINE_TYPE in /etc/usv/usv.env (yamaha_outboard,
    # mercury_diesel). Hidden from the picker; the header band shows it and
    # the engine dials pick their scale from it (typed_meter). Empty until
    # the vessel has written an engine point with the tag.
    engine_var = {
        "name": "engine_type", "label": "Engine", "type": "query",
        "datasource": DS, "hide": 2,
        "query": ('import "influxdata/influxdb/schema"\n'
                  'schema.tagValues(bucket: "telemetry", tag: "engine_type", '
                  'predicate: (r) => r.vessel == "${vessel}", start: -30d)'),
        "refresh": 2, "current": {"text": "", "value": ""},
        "includeAll": False, "multi": False,
    }
    # Engine name for the header chip: the engine_model tag (ENGINE_MODEL in
    # usv.env, e.g. "Yamaha Outboard Engine F115 B 4-Stroke") of the most
    # recent engine point, so a renamed engine shows its new name at once;
    # the engine_type of points written before the model tag existed; and
    # "not reported" when the vessel has written no engine point in 30 days
    # (an empty variable would print the bare markdown "****").
    model_var = {
        "name": "engine_model", "label": "Engine model", "type": "query",
        "datasource": DS, "hide": 2,
        "query": ('import "array"\n'
                  'named = from(bucket: "telemetry")\n'
                  '  |> range(start: -30d)\n'
                  '  |> filter(fn: (r) => r._measurement == "engine" and r.vessel == "${vessel}")\n'
                  '  |> filter(fn: (r) => exists r.engine_model or exists r.engine_type)\n'
                  '  |> last()\n'
                  # map before group: the engine measurement holds float and
                  # string fields (dtc_text), which group() cannot merge.
                  '  |> map(fn: (r) => ({_time: r._time, _value: if exists r.engine_model '
                  'then r.engine_model else r.engine_type}))\n'
                  '  |> group()\n'
                  'none = array.from(rows: [{_time: 1970-01-01T00:00:00Z, _value: "not reported"}])\n'
                  'union(tables: [named, none])\n'
                  '  |> sort(columns: ["_time"], desc: true)\n'
                  '  |> limit(n: 1)\n'
                  '  |> keep(columns: ["_value"])\n'),
        "refresh": 2, "current": {"text": "", "value": ""},
        "includeAll": False, "multi": False,
    }
    # Vessel class for the header band: VESSEL_CLASS in usv.env, written by
    # signalk_bridge as the vessel_class tag of the `vessel` measurement
    # (v1.2.33). "Swiftships USV" until the vessel has written one.
    class_var = {
        "name": "vessel_class", "label": "Vessel class", "type": "query",
        "datasource": DS, "hide": 2,
        "query": ('import "array"\n'
                  'named = from(bucket: "telemetry")\n'
                  '  |> range(start: -30d)\n'
                  '  |> filter(fn: (r) => r._measurement == "vessel" and r.vessel == "${vessel}")\n'
                  '  |> filter(fn: (r) => r._field == "class_name" and exists r.vessel_class)\n'
                  '  |> last()\n'
                  '  |> map(fn: (r) => ({_time: r._time, _value: r.vessel_class}))\n'
                  '  |> group()\n'
                  'none = array.from(rows: [{_time: 1970-01-01T00:00:00Z, _value: "Swiftships USV"}])\n'
                  'union(tables: [named, none])\n'
                  '  |> sort(columns: ["_time"], desc: true)\n'
                  '  |> limit(n: 1)\n'
                  '  |> keep(columns: ["_value"])\n'),
        "refresh": 2, "current": {"text": "", "value": ""},
        "includeAll": False, "multi": False,
    }
    variables = [vessel_var, THEME_VAR] if fleet else [vessel_var, THEME_VAR, engine_var, model_var, class_var]
    return {
        "uid": uid, "title": title, "tags": ["usv"] + tags,
        "timezone": "browser", "schemaVersion": 39, "version": 1,
        "refresh": refresh, "editable": True, "style": "dark",
        "time": {"from": time_from, "to": "now"},
        # Tab-style navigation: every usv-tagged dashboard is rendered as a
        # clickable link chip across the top of every dashboard. keepTime +
        # includeVars carry the time range and vessel selection across tabs.
        # The Light/Dark chips re-open this dashboard with Grafana's ?theme=
        # override. Grafana reads that parameter only when the page loads
        # (its click interceptor turns same-window links into in-app
        # navigation, which keeps the current theme), so the chips open a
        # new tab; the whole session in that tab then stays in that theme.
        # Instant in-place switch: press "c" then "t" (Grafana shortcut);
        # lasting default: Profile -> Preferences -> Interface theme.
        "links": [{
            "type": "link", "title": name, "url": f"/d/{u}",
            # The Audit page opens at its own 24 h and does not hand that
            # range to the live pages.
            "keepTime": u != "usv-audit" and uid != "usv-audit",
            "includeVars": u != "usv-fleet",
            "targetBlank": False, "icon": "dashboard",
            "tooltip": f"Open {name}",
        } for u, name in TAB_ORDER] + [{
            "type": "link", "title": "☀ Light", "url": f"/d/{uid}?theme=light",
            "keepTime": True, "includeVars": True, "targetBlank": True,
            "icon": "external link",
            "tooltip": "Open in a new tab in the light theme (or press c, t)",
        }, {
            "type": "link", "title": "☾ Dark", "url": f"/d/{uid}?theme=dark",
            "keepTime": True, "includeVars": True, "targetBlank": True,
            "icon": "external link",
            "tooltip": "Open in a new tab in the dark theme (or press c, t)",
        }],
        "templating": {"list": variables},
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
# States derived by grafana_bridge from the Alpha mission's mode flags
# (see docs/alpha-mission-integration.md, table in §3).
mission_state_mappings = state_map([
    ("Idle", "#4d545c"), ("Autonomous", TILE_OK), ("Remote", "orange"),
    ("Paused", "yellow"),
    ("StationKeeping", ACCENT), ("ReturnToBase", "orange"),
    ("ReturnToHome", "orange"),
    ("EnteringRegion", "super-light-blue"), ("AwaitingRearm", "orange"),
    ("CommsLost", "red"), ("Parked", "#6e7680"), ("Completed", "purple"),
    ("HoldingAtBase", "orange"),
    ("Following", ACCENT),
    ("Moving", "light-yellow"),
    ("AllStop", "red"),
])
helm_state_mappings = state_map([
    ("DRIVE", TILE_OK), ("PARK", "#6e7680"), ("STANDBY", "yellow"),
    ("DISABLED", "orange"), ("MALCONFIG", "red"),
])
# IVPHELM_ALLSTOP: clear = helm deciding; ManualOverride / NothingToDo are
# normal (parked / idle) and shown neutral; anything else is a fault.
allstop_mappings = [
    {"type": "value", "options": {
        "clear": {"text": "CLEAR", "color": TILE_OK, "index": 0},
        "ManualOverride": {"text": "PARKED", "color": "#4d545c", "index": 1},
        "NothingToDo": {"text": "IDLE", "color": "#4d545c", "index": 2}}},
    {"type": "regex", "options": {
        "pattern": "(.+)",
        "result": {"text": "$1", "color": "red", "index": 3}}},
]
geofence_mappings = value_color_map([
    (0, "INSIDE", OK), (1, "BREACH", "red"),
    (2, "RECOVERING", "yellow"),
])
# iWebSocket COMMS_LOST phases (autonomy.comms_phase): 0 link ok, 1 grace
# (mission continues), 2 failsafe hold (disarmed), 3 return to base.
comms_phase_mappings = value_color_map([
    (0, "LINK OK", TILE_OK), (1, "GRACE", "yellow"),
    (2, "LOST — FAILSAFE", "red"), (3, "LOST — RTB", "orange"),
])
# Operator link for both iWebSocket variants (autonomy.operator_link,
# usv::operator_link): the simulation's COMMS_LOST phases 1 to 3, and the
# boats' COMMS_LOSS (production iWebSocket: return to base, hold there).
operator_link_mappings = value_color_map([
    (0, "LINK OK", TILE_OK), (1, "GRACE", "yellow"),
    (2, "LOST — FAILSAFE", "red"), (3, "LOST — RTB", "orange"),
    (4, "NO UI LINK", "yellow"), (5, "LOST — AT BASE", "orange"),
])
flag_timeline_mappings = [{"type": "value", "options": {
    "0": {"text": "—", "color": "rgba(110, 120, 130, 0.12)", "index": 0},
    "1": {"text": "ON", "color": ACCENT, "index": 1}}}]

# Approach-tier thresholds mirror the speed governor in alpha.bhv:
# full UI speed beyond 500 m, tier-1 zone 100–500 m, tier-2 inside 100 m,
# capture_radius 20 m.
approach_thresholds = [(None, "orange"), (100, "yellow"), (500, OK)]

arrival_mappings = state_map([
    ("cruising", TILE_OK), ("decelerating", "yellow"), ("arrived", ACCENT),
])

# "4 of 5": index and total joined into one string (stat renders strings
# when the panel reduces all fields; STRING_FIELDS covers "message").
waypoint_of_total_query = (
    'from(bucket: "telemetry")\n'
    '  |> range(start: v.timeRangeStart, stop: v.timeRangeStop)\n'
    '  |> filter(fn: (r) => r._measurement == "mission" and '
    '(r._field == "waypoint_index" or r._field == "waypoints_total"))\n'
    '  |> filter(fn: (r) => r.vessel == "${vessel}")\n'
    '  |> last()\n'
    '  |> pivot(rowKey: ["vessel"], columnKey: ["_field"], valueColumn: "_value")\n'
    '  |> map(fn: (r) => ({ _field: "message", _value: '
    'string(v: int(v: r.waypoint_index)) + " of " + string(v: int(v: r.waypoints_total)) }))\n')

events_query = (
    'from(bucket: "events")\n'
    '  |> range(start: v.timeRangeStart, stop: v.timeRangeStop)\n'
    '  |> filter(fn: (r) => r._measurement == "events" and r._field == "message")\n'
    '  |> filter(fn: (r) => r.vessel == "${vessel}")\n'
    '  |> group()\n'
    '  |> keep(columns: ["_time", "_value", "severity"])\n'
    '  |> rename(columns: {_time: "Time", _value: "Event", severity: "Severity"})\n'
    '  |> sort(columns: ["Time"], desc: true)\n'
    '  |> limit(n: 50)\n')

# Single-screen operator layout (24 cols x 22 rows incl. the header band):
#   A  status tiles (7)          h3
#   B  identity / timers / route h3
#   C  progress bar              h2
#   D  approach profile | flags  h7
#   E  state timeline | events   h5
# Mission Events tables (Mission and Replay): ISO time, coloured severity,
# and the remaining width to the event text (hover a cell for the full text).
EVENTS_TABLE_OVERRIDES = [{"matcher": {"id": "byName", "options": "Time"},
            "properties": [{"id": "custom.width", "value": 150},
                           {"id": "unit", "value": "dateTimeAsIso"}]},
           {"matcher": {"id": "byName", "options": "Severity"},
            "properties": [{"id": "custom.width", "value": 80},
                           {"id": "mappings", "value": [{
                               "type": "value", "options": {
                                   "info": {"color": TILE_OK, "index": 0},
                                   "warning": {"color": "yellow", "index": 1},
                                   "critical": {"color": "red", "index": 2}}}]},
                           {"id": "custom.cellOptions",
                            "value": {"type": "color-text"}}]}]

DASHBOARDS.append(dashboard("usv-mission", "Mission", [
    # A — the operational picture: 8 equal status tiles (3 x 3).
    stat("Mission State", last_str("mission_state", "state"), 0, 0, 3, 2,
         mappings=mission_state_mappings, text_mode="value",
         color_mode="background", no_value="MISSION OFF"),
    stat("Helm", last_str("mission_state", "helm_state"), 3, 0, 3, 2,
         mappings=helm_state_mappings, text_mode="value",
         color_mode="background"),
    stat("Armed", flux("autonomy", "armed", last=True), 6, 0, 3, 2,
         mappings=[{"type": "value", "options": {
             "0": {"text": "DISARMED", "color": "orange", "index": 0},
             "1": {"text": "ARMED", "color": TILE_OK, "index": 1}}}],
         text_mode="value", color_mode="background"),
    stat("Operator Link", flux("autonomy", "operator_link", last=True),
         9, 0, 3, 2, mappings=operator_link_mappings,
         text_mode="value", color_mode="background"),
    stat("Geofence", flux("autonomy", "geofence_breached", last=True),
         12, 0, 3, 2, mappings=geofence_mappings, text_mode="value",
         color_mode="background"),
    stat("All-Stop", last_str("mission_state", "allstop_reason"),
         15, 0, 3, 2, mappings=allstop_mappings, text_mode="value",
         color_mode="background"),
    stat("Arrival", last_str("mission_state", "arrival_state"),
         18, 0, 3, 2, mappings=arrival_mappings, text_mode="value",
         color_mode="background"),
    stat("Obstacles", flux("autonomy", "obstacle_alerts", last=True),
         21, 0, 3, 2, decimals=0,
         thresholds=[(None, TILE_OK), (1, "yellow"), (3, "orange")],
         color_mode="background"),
    # B — identity, timers, route position: 8 equal readouts (3 x 3).
    stat("Mission", last_str("mission_state", "mission_name"), 0, 2, 3, 2,
         text_mode="value"),
    stat("Route", last_str("mission_state", "route_name"), 3, 2, 3, 2,
         text_mode="value"),
    stat("Elapsed", flux("mission", "elapsed_s", last=True), 6, 2, 3, 2,
         unit="dthms", decimals=0),
    stat("ETA to WP", flux("mission", "eta_s", last=True), 9, 2, 3, 2,
         unit="dthms", decimals=0),
    stat("Waypoint", waypoint_of_total_query, 12, 2, 3, 2, text_mode="value",
         string_value=True),
    stat("Remaining", flux("mission", "waypoints_remaining", last=True),
         15, 2, 3, 2, decimals=0),
    stat("Dist to WP", flux("mission", "dist_to_waypoint", last=True),
         18, 2, 3, 2, unit=UNIT_M, decimals=0,
         thresholds=approach_thresholds),
    stat("Traveled", flux("mission", "distance_traveled", last=True, nm=True),
         21, 2, 3, 2, unit=UNIT_NM, decimals=2),
    # C — progress bar across the width.
    panel("bargauge", "Mission Progress",
          [target(flux("mission", "progress_pct", last=True))],
          0, 4, 24, 2, unit="percent", min_=0, max_=100, decimals=0,
          thresholds=[(None, ACCENT)],
          options={"reduceOptions": {"calcs": ["lastNotNull"]},
                   "orientation": "horizontal", "displayMode": "lcd",
                   "showUnfilled": True, "valueMode": "color",
                   "text": {"valueSize": 18}}),
    # D — approach profile (governor tiers) + autonomy flag activity.
    panel("timeseries", "Approach Profile — Distance to Next Waypoint vs SOG",
          [target(flux("mission", "dist_to_waypoint"), "distance m"),
           target(flux("nav", "sog"), "SOG kn", ref="B")],
          0, 6, 12, 5, unit=UNIT_M,
          thresholds=[(None, "transparent"), (20, "red"), (100, "orange"),
                      (500, OK)],
          custom={"drawStyle": "line", "lineWidth": 2, "fillOpacity": 14,
                  "showPoints": "never", "spanNulls": 60000,
                  "gradientMode": "opacity",
                  "lineInterpolation": "smooth",
                  "thresholdsStyle": {"mode": "line"}},
          overrides=[
              {"matcher": {"id": "byFrameRefID", "options": "B"},
               "properties": [{"id": "unit", "value": "velocityknot"},
                              {"id": "custom.axisPlacement", "value": "right"},
                              {"id": "color",
                               "value": {"mode": "fixed",
                                         "fixedColor": PALETTE[1]}}]},
              {"matcher": {"id": "byRegexp", "options": ".*distance.*"},
               "properties": [{"id": "color",
                               "value": {"mode": "fixed",
                                         "fixedColor": PALETTE[0]}}]}],
          options={"legend": {"displayMode": "list", "placement": "bottom",
                              "showLegend": True},
                   "tooltip": {"mode": "multi"}}),
    panel("state-timeline", "Autonomy Flags",
          [target(flux("autonomy",
                       ["waypoint", "pause", "stationing", "returning",
                        "armed", "comms_lost", "manual_override",
                        "remote_control", "crash_stop"],
                       every="5s", fn="last",
                       extra='  |> map(fn: (r) => ({ r with _field: r._field }))'))],
          12, 6, 12, 5, mappings=flag_timeline_mappings,
          custom={"fillOpacity": 70, "lineWidth": 0},
          overrides=[{"matcher": {"id": "byRegexp", "options": ".*"},
                      "properties": [{"id": "displayName",
                                      "value": "${__field.labels._field}"}]}],
          options={"showValue": "never",
                   "legend": {"showLegend": False},
                   "tooltip": {"mode": "single"}}),
    # E — history.
    panel("state-timeline", "Mission State",
          [target(flux("mission_state", "state", every="10s", fn="last"))],
          0, 11, 12, 6, mappings=mission_state_mappings,
          custom={"fillOpacity": 70, "lineWidth": 0},
          overrides=[{"matcher": {"id": "byRegexp", "options": ".*"},
                      "properties": [{"id": "displayName", "value": "state"}]}],
          options={"showValue": "auto", "legend": {"showLegend": False}}),
    panel("table", "Mission Events",
          [target(events_query)],
          12, 11, 12, 6,
          overrides=EVENTS_TABLE_OVERRIDES,
          options={"showHeader": True, "cellHeight": "sm",
                   "sortBy": [{"displayName": "Time", "desc": True}]}),
], ["mission"]))

# ------------------------------------------------------------- FR-2 Navigation
# Single-screen layout (24 x 23 rows incl. band): map + instruments and GPS
# on the right, an 8-tile KPI strip, three trend panels.
gps_fix_mappings = [{"type": "value", "options": {
    "RTK Fixed": {"text": "RTK FIXED", "color": TILE_OK, "index": 0},
    "RTK Float": {"text": "RTK FLOAT", "color": "yellow", "index": 1},
    "3D": {"text": "3D", "color": "yellow", "index": 2},
    "2D": {"text": "2D", "color": "orange", "index": 3},
    "None": {"text": "NO FIX", "color": "red", "index": 4}}}]

DASHBOARDS.append(dashboard("usv-navigation", "Navigation", [
    panel("geomap", "Position & Track",
          [target(track_query("5s")), target(position_query(), ref="B"),
           target(geometry_query("route"), ref="C"),
           target(geometry_query("region"), ref="D"),
           target(vector_query(), ref="E"),
           target(base_query(), ref="F")],
          0, 0, 14, 10,
          options={
              "view": {"id": "fit", "zoom": 15},
              "basemap": {"type": "osm-standard", "name": "OSM"},
              "layers": overlay_layers(base_ref="F"),
              "tooltip": {"mode": "details"},
          }),
    # Instruments: compass (HDG + COG needles) and SOG meter.
    (instrument("Compass — HDG / COG",
                [target(flux("nav", "heading", last=True), "HDG"),
                 target(flux("nav", "cog", last=True), "COG", ref="B")],
                14, 0, 5, 6, compass_js(),
                {"kind": "compass", "preview": 55, "preview2": 57})
     if ANALOG else
     stat("Heading / COG", flux("nav", "heading", last=True), 14, 0, 5, 6,
          unit="degree", decimals=0)),
    meter("Speed Over Ground", flux("nav", "sog", last=True), 19, 0, 5, 6,
          0, 40, "kn", zones=None, decimals=1, split=8, sub="KNOTS",
          preview=12.8, fallback_thresholds=[(None, ACCENT)]),
    # GPS quality and position.
    stat("GPS Fix", last_str("nav", "fix_type"), 14, 6, 5, 2,
         text_mode="value", color_mode="background",
         mappings=gps_fix_mappings),
    # Satellites / HDOP are N/A on an INS feed (VectorNav publishes neither).
    stat("Satellites", flux("nav", "satellites", last=True), 19, 6, 5, 2,
         decimals=0, thresholds=neutral_base([(None, "red"), (5, "yellow"), (8, TILE_OK)]),
         color_mode="background", no_value="N/A"),
    stat("Latitude", flux("nav", "lat", last=True), 14, 8, 3, 2, decimals=6),
    stat("Longitude", flux("nav", "lon", last=True), 17, 8, 4, 2, decimals=6),
    # Distance to the comms-loss return base (BASE_COORDS).
    stat("Dist to Base", base_distance_query, 21, 8, 3, 2, unit=UNIT_NM,
         decimals=2, no_value="no base"),
    # KPI strip: 8 equal tiles.
    stat("Speed Cap", flux("autopilot", "speed_cap", last=True), 0, 10, 3, 2,
         unit="velocityknot", decimals=1),
    stat("COG", flux("nav", "cog", last=True), 3, 10, 3, 2,
         unit="degree", decimals=0),
    stat("WP Bearing", flux("traffic", "planned_course", last=True), 6, 10, 3, 2,
         unit="degree", decimals=0, no_value="no route",
         mappings=[{"type": "range", "options": {"from": -1.5, "to": -0.5,
                    "result": {"text": "no route", "color": NEUTRAL, "index": 0}}}],
         thresholds=[(None, OK)]),
    stat("XTE", flux("nav", "cross_track_error", last=True), 9, 10, 3, 2,
         unit=UNIT_M, decimals=1, graph_mode="area"),
    stat("Dist to WP", flux("mission", "dist_to_waypoint", last=True),
         12, 10, 3, 2, unit=UNIT_M, decimals=0,
         thresholds=neutral_base(approach_thresholds), no_value="no route"),
    stat("HDOP", flux("nav", "hdop", last=True), 15, 10, 3, 2, decimals=1,
         thresholds=neutral_base([(None, TILE_OK), (2, "yellow"), (5, "red")]),
         no_value="N/A"),
    stat("Depth", flux("nav", "depth", last=True), 18, 10, 3, 2,
         unit=UNIT_M, decimals=1),
    stat("Roll / Pitch", flux("nav", "roll", last=True), 21, 10, 3, 2,
         unit="degree", decimals=1),
    # Trends.
    ts("Heading / COG", [target(flux("nav", ["heading", "cog"]))],
       0, 12, 6, 5, unit="degree", min_=0, max_=360),
    ts("Speed Over Ground", [target(flux("nav", "sog"), "SOG")],
       6, 12, 6, 5, unit="velocityknot"),
    ts("Cross-Track Error", [target(flux("nav", "cross_track_error"), "XTE")],
       12, 12, 6, 5, unit=UNIT_M),
    ts("Attitude — Roll / Pitch", [target(flux("nav", ["roll", "pitch"]))],
       18, 12, 6, 5, unit="degree"),
], ["navigation"]))
# Dist to Base (v1.2.23) takes its own id and the shared counter is put back,
# so adding it does not renumber the panels of the later dashboards.
for _p in DASHBOARDS[-1]["panels"]:
    if _p.get("title") == "Dist to Base":
        _id[0] -= 1
        _dist_old = _p["id"]
        _p["id"] = 9501
        for _q in DASHBOARDS[-1]["panels"]:
            if _q["id"] > _dist_old and _q is not _p:
                _q["id"] -= 1

# Helm trim switch and J1939 DM1 fault details (can_bridge, usv722_can.hpp).
# The trim switch is the helm button state only: trim POSITION is not on the
# usv-722 bus (it needs the engine's NMEA 2000 gateway).
TRIM_MAPPINGS = [{"type": "value", "options": {
    "-1": {"text": "DOWN", "color": ACCENT, "index": 0},
    "0": {"text": "IDLE", "color": NEUTRAL, "index": 1},
    "1": {"text": "UP", "color": ACCENT, "index": 2}}}]
# Battery state: a charger's chargingMode (bulk / absorption / float ...)
# or, from a battery monitor, charging / discharging / idle by the current.
CHARGING_MAPPINGS = [{"type": "value", "options": {
    "charging": {"text": "CHARGING", "color": TILE_OK, "index": 0},
    "bulk": {"text": "CHARGING (BULK)", "color": TILE_OK, "index": 1},
    "absorption": {"text": "CHARGING (ABSORPTION)", "color": TILE_OK, "index": 2},
    "float": {"text": "FLOAT", "color": TILE_OK, "index": 3},
    "discharging": {"text": "DISCHARGING", "color": "yellow", "index": 4},
    "idle": {"text": "IDLE", "color": NEUTRAL, "index": 5},
    "off": {"text": "OFF", "color": NEUTRAL, "index": 6}}}]
FAULTS_MAPPINGS = [{"type": "value", "options": {
    "0": {"text": "NONE", "color": TILE_OK, "index": 0}}},
                   {"type": "range", "options": {"from": 1, "to": 999, "result": {
                       "text": "ACTIVE", "color": "red", "index": 1}}}]


def trim_tile(x, y, w, h=2):
    return stat("Trim Switch", flux("engine", "trim_switch", last=True),
                x, y, w, h, mappings=TRIM_MAPPINGS, text_mode="value",
                color_mode="background")


# J1939 DM1 warning lamps (can_bridge writes engine.lamp_* as 0/1): one
# tile naming the lamps that are on, most severe first, or OFF.
LAMPS_QUERY = 'import "strings"\n' + flux(
    "engine", ["lamp_red", "lamp_mil", "lamp_amber", "lamp_protect"],
    last=True) + (
    '  |> group()\n'
    '  |> set(key: "row", value: "lamps")\n'
    '  |> pivot(rowKey: ["row"], columnKey: ["_field"], valueColumn: "_value")\n'
    '  |> map(fn: (r) => {\n'
    '      on = (if exists r.lamp_red and r.lamp_red == 1.0 then "RED STOP " else "")\n'
    '         + (if exists r.lamp_mil and r.lamp_mil == 1.0 then "MIL " else "")\n'
    '         + (if exists r.lamp_amber and r.lamp_amber == 1.0 then "AMBER " else "")\n'
    '         + (if exists r.lamp_protect and r.lamp_protect == 1.0 then "PROTECT " else "")\n'
    '      return {_value: if on == "" then "OFF" else strings.trimSpace(v: on)}\n'
    '  })\n')
# Fault text: "NONE", "N/A", or a list of faults ("SPN 110 FMI 3 ..." from
# the J1939 DM1, "Check Engine (alarm); ..." from Signal K notifications).
DTC_TEXT_MAPPINGS = [{"type": "value", "options": {
    "NONE": {"text": "NONE", "color": TILE_OK, "index": 0},
    "N/A": {"text": "N/A", "color": NEUTRAL, "index": 1}}},
                     {"type": "regex", "options": {"pattern": "^(?!(NONE|N/A)$).+",
                                                   "result": {"color": "red", "index": 2}}}]
LAMPS_MAPPINGS = [{"type": "value", "options": {
    "OFF": {"text": "OFF", "color": NEUTRAL, "index": 0}}},
                  {"type": "regex", "options": {"pattern": "^RED.*", "result": {
                      "color": "red", "index": 1}}},
                  {"type": "regex", "options": {"pattern": ".+", "result": {
                      "color": "yellow", "index": 2}}}]


# engine.dtc_text is "NONE", "N/A" or "SPN 110 FMI 3 ... (x2); SPN ...":
# the tile shows the first fault and how many more there are.
DTC_TEXT_QUERY = 'import "strings"\n' + flux(
    "engine", "dtc_text", last=True,
    extra=('  |> map(fn: (r) => {\n'
           '      parts = strings.split(v: r._value, t: "; ")\n'
           '      n = length(arr: parts)\n'
           '      return {r with _value: if n > 1 then parts[0] + " (+" + string(v: n - 1) + " more)" else r._value}\n'
           '  })'))

# -------------------------------------------------------------- FR-3 Autopilot
# Single-screen layout (24 x 23 incl. band): helm safety tiles, command
# readouts, instruments, desired-vs-actual, governor + errors.
onoff_map = lambda off, on, on_color=ACCENT: [{"type": "value", "options": {
    "0": {"text": off, "color": "#3d434b", "index": 0},
    "1": {"text": on, "color": on_color, "index": 1}}}]
corridor_mappings = state_map([
    ("none", TILE_OK), ("slow", "yellow"), ("stop", "red"),
    ("recovering", "orange"),
])
crash_mappings = state_map([
    ("off", TILE_OK), ("braking", "red"), ("stopped", "orange"),
])

DASHBOARDS.append(dashboard("usv-autopilot", "Autopilot", [
    # One screen: two tile rows (h2), dials (h5), three trends, CAN row (h2),
    # trim row (h2); fit_to_screen trims the trends to fit.
    # A — helm state and the Alpha mission's safety latches.
    stat("Helm State", last_str("mission_state", "helm_state"), 0, 0, 3, 2,
         mappings=helm_state_mappings, text_mode="value",
         color_mode="background"),
    stat("All-Stop", last_str("mission_state", "allstop_reason"),
         3, 0, 3, 2, mappings=allstop_mappings, text_mode="value",
         color_mode="background"),
    stat("Station Keep", flux("autonomy", "stationing", last=True),
         6, 0, 3, 2, mappings=onoff_map("OFF", "ACTIVE"),
         text_mode="value", color_mode="background"),
    stat("Entry Gate", flux("autonomy", "entry_gate_open", last=True),
         9, 0, 3, 2,
         mappings=[{"type": "value", "options": {
             "0": {"text": "CLOSED", "color": "yellow", "index": 0},
             "1": {"text": "OPEN", "color": TILE_OK, "index": 1}}}],
         text_mode="value", color_mode="background"),
    stat("Re-Arm Hold", flux("autonomy", "awaiting_rearm", last=True),
         12, 0, 3, 2, mappings=onoff_map("CLEAR", "AWAITING", "orange"),
         text_mode="value", color_mode="background"),
    stat("Remote Control", flux("autonomy", "remote_control", last=True),
         15, 0, 3, 2, mappings=onoff_map("OFF", "MANUAL", "orange"),
         text_mode="value", color_mode="background"),
    stat("Corridor", last_str("mission_state", "corridor_status"),
         18, 0, 3, 2, mappings=corridor_mappings, text_mode="value",
         color_mode="background"),
    stat("Crash Stop", last_str("mission_state", "crash_stop_status"),
         21, 0, 3, 2, mappings=crash_mappings, text_mode="value",
         color_mode="background"),
    # B — commands and errors.
    stat("Active Behaviors", last_str("mission_state", "active_behavior"),
         0, 2, 6, 2, text_mode="value"),
    stat("Heading Error", flux("autopilot", "heading_error", last=True),
         6, 2, 3, 2, unit="degree", decimals=1,
         thresholds=[(None, OK), (10, "yellow"), (25, "red")],
         graph_mode="area"),
    stat("Speed Error", flux("autopilot", "speed_error", last=True), 9, 2, 3, 2,
         unit="velocityknot", decimals=2,
         thresholds=[(None, OK), (1, "yellow"), (2, "red")],
         graph_mode="area"),
    stat("XTE", flux("nav", "cross_track_error", last=True), 12, 2, 3, 2,
         unit=UNIT_M, decimals=1, graph_mode="area"),
    stat("Dist to Station", flux("mission", "dist_to_station", last=True),
         15, 2, 3, 2, unit=UNIT_M, decimals=0),
    stat("Speed Cap", flux("autopilot", "speed_cap", last=True), 18, 2, 3, 2,
         unit="velocityknot", decimals=1),
    stat("Next Turn", flux("autopilot", "next_turn_angle", last=True),
         21, 2, 3, 2, unit="degree", decimals=0),
    # C — instruments: rudder indicator, throttle, heading error.
    (instrument("Rudder Angle",
                [target(flux("autopilot", "rudder_actual", last=True)),
                 target(flux("autopilot", "rudder_cmd", last=True), ref="B")],
                0, 4, 8, 5, rudder_js(),
                {"kind": "rudder", "preview": 3})
     if ANALOG else
     stat("Rudder Angle", flux("autopilot", "rudder_actual", last=True),
          0, 4, 8, 5, unit="degree", decimals=1)),
    meter("Throttle", flux("autopilot", "throttle_cmd", last=True),
          8, 4, 8, 5, 0, 100, "%", zones=None, decimals=0, split=5,
          sub="COMMANDED THRUST", preview=64,
          fallback_thresholds=[(None, ACCENT)]),
    meter("Heading Error", flux("autopilot", "heading_error", last=True),
          16, 4, 8, 5, -45, 45, "°",
          zones=[(-25, ZRED), (-8, ZYELLOW), (8, ZGREEN), (25, ZYELLOW),
                 (45, ZRED)],
          decimals=1, split=6, sub="OFF COURSE", preview=1.8,
          fallback_thresholds=[(None, OK)]),
    # D — trends: heading, rudder, speed (the throttle and error trends
    # repeat the throttle dial and the error tiles, so they are not drawn).
    ts("Heading — desired vs actual",
       [target(flux("autopilot", ["desired_heading", "actual_heading"]))],
       0, 9, 8, 6, unit="degree", min_=0, max_=360),
    # Actuator command and actual are both degrees (CAN 0x210). The PID
    # output DESIRED_RUDDER is PERCENT of full rudder on the vessel, so it
    # goes on its own right-hand axis instead of being read as degrees.
    ts("Rudder — actuator cmd / actual (°), PID (%)",
       [target(flux("autopilot", "rudder_actuator_cmd"), "actuator command"),
        target(flux("autopilot", "rudder_actual"), "actual", ref="B"),
        target(flux("autopilot", "rudder_cmd"), "PID output",
               ref="C")],
       8, 9, 8, 6, unit="degree",
       overrides=[{"matcher": {"id": "byFrameRefID", "options": "C"},
                   "properties": [{"id": "unit", "value": "percent"},
                                  {"id": "custom.axisPlacement",
                                   "value": "right"},
                                  {"id": "custom.lineStyle",
                                   "value": {"fill": "dash",
                                             "dash": [6, 4]}}]}]),
    ts("Speed — desired / cap / commanded / actual",
       [target(flux("autopilot", ["desired_speed", "speed_cap", "cmd_speed",
                                  "actual_speed"]))],
       16, 9, 8, 6, unit="velocityknot"),
    # F — helm and drive on the CAN bus (usv-722: steering actuator 0x210,
    # Glendinning helm, J1939 thrust / gear, key switch, DM1; can_bridge).
    stat("Rudder Actual", flux("autopilot", "rudder_actual", last=True),
         0, 15, 3, 2, unit="degree", decimals=1, graph_mode="area"),
    stat("Helm Wheel", flux("autopilot", "helm_rudder", last=True),
         3, 15, 3, 2, unit="degree", decimals=1),
    stat("Steering Drive", flux("autopilot", "steer_drive_pct", last=True),
         6, 15, 3, 2, unit="percent", decimals=0, graph_mode="area"),
    stat("Steering Mode", flux("autopilot", "steer_pc_control", last=True),
         9, 15, 3, 2, text_mode="value", color_mode="background",
         mappings=[{"type": "value", "options": {
             "0": {"text": "HELM", "color": "orange", "index": 0},
             "1": {"text": "AUTOPILOT", "color": TILE_OK, "index": 1}}}]),
    stat("Gear", flux("engine", "gear", last=True), 12, 15, 3, 2,
         text_mode="value", color_mode="background",
         mappings=[{"type": "value", "options": {
             "-1": {"text": "REVERSE", "color": "orange", "index": 0},
             "0": {"text": "NEUTRAL", "color": NEUTRAL, "index": 1},
             "1": {"text": "FORWARD", "color": TILE_OK, "index": 2}}}]),
    stat("Engine Thrust", flux("engine", "throttle_pct", last=True),
         15, 15, 3, 2, unit="percent", decimals=0, graph_mode="area"),
    stat("Key Switch", flux("engine", "key_state", last=True), 18, 15, 3, 2,
         text_mode="value", color_mode="background",
         mappings=[{"type": "value", "options": {
             "0": {"text": "OFF", "color": NEUTRAL, "index": 0},
             "1": {"text": "RUN", "color": TILE_OK, "index": 1},
             "2": {"text": "START", "color": "yellow", "index": 2}}}]),
    stat("Engine Faults", flux("engine", "active_dtcs", last=True),
         21, 15, 3, 2, text_mode="value", color_mode="background",
         mappings=[{"type": "value", "options": {
             "0": {"text": "NONE", "color": TILE_OK, "index": 0}}},
                   {"type": "range", "options": {"from": 1, "to": 999, "result": {
                       "text": "ACTIVE", "color": "red", "index": 1}}}]),
    # G — helm trim switch (fault lamps and codes are on the CAN Bus page),
    # the autopilot's service mode and the gains it applies (planchaPID
    # PID_GAINS: the manual values in service mode, the schedule otherwise).
    trim_tile(0, 17, 4),
], ["autopilot"]))
# The trim tile gets its own id and the shared counter is put back, so
# adding it does not renumber panels on other pages.
_ap = DASHBOARDS[-1]["panels"][-1:]
for _i, _p in enumerate(_ap):
    _p["id"] = 9201 + _i
_id[0] -= len(_ap)
_ap_new = [
    stat("Service Mode", flux("autopilot", "service_mode", last=True), 4, 17, 4, 2,
         mappings=[{"type": "value", "options": {
             "0": {"text": "OFF", "color": TILE_OK, "index": 0},
             "1": {"text": "ON (TUNING)", "color": "orange", "index": 1}}}],
         text_mode="value", color_mode="background"),
    stat("Applied Gains", flux("autopilot", ["gain_heading_kp", "gain_heading_ki", "gain_heading_kd",
                                             "gain_speed_kp", "gain_speed_ki", "gain_speed_kd"],
                               last=True), 8, 17, 16, 2,
         decimals=3, text_mode="value_and_name", no_value="not reported"),
]
for _i, _p in enumerate(_ap_new):
    _p["id"] = 9211 + _i
_id[0] -= len(_ap_new)
DASHBOARDS[-1]["panels"].extend(_ap_new)

# ------------------------------------------- Autopilot Live (troubleshooting)
# The same helm instruments as the Autopilot page, drawn large for sea trials
# and troubleshooting, with a 1 s refresh and a 5-minute window. Not in the
# tab row (the row is full at 1532 px); opened from the link on each dial of
# the Autopilot page. Every query is one the Autopilot page already uses.
LIVE_SCALE_JS = (
    "// Fonts, rails and needle grow with the panel (1x at 200 px high,\n"
    "// capped at 3x); falls back to 1x if the chart size is unavailable.\n"
    "const S = (() => { try { const h = context.panel.chart.getHeight();\n"
    "  return h > 0 ? Math.max(1, Math.min(3, h / 200)) : 1; }\n"
    "  catch (e) { return 1; } })();\n")


def big_dial(p):
    """Scale an ECharts instrument's fixed pixel sizes with the panel."""
    if p.get("type") != "volkovlabs-echarts-panel":
        return p
    js = p["options"]["getOption"]
    assert js.startswith(ECH_PRELUDE), p["title"]
    if "// SELF_SCALING" in js:
        return p
    body = _re.sub(r"\b(fontSize|width|length|distance|size): (-?\d+(?:\.\d+)?)\b",
                   r"\1: \2 * S", js[len(ECH_PRELUDE):])
    p["options"]["getOption"] = ECH_PRELUDE + LIVE_SCALE_JS + body
    return p


_id_before_live = _id[0]
_LIVE = dashboard("usv-autopilot-live", "Autopilot Live", [
    # One screen (17 content rows): status tiles (h2), large dials (h9),
    # trends (h6).
    stat("Helm State", last_str("mission_state", "helm_state"), 0, 0, 3, 2,
         mappings=helm_state_mappings, text_mode="value",
         color_mode="background"),
    stat("Steering Mode", flux("autopilot", "steer_pc_control", last=True),
         3, 0, 3, 2, text_mode="value", color_mode="background",
         mappings=[{"type": "value", "options": {
             "0": {"text": "HELM", "color": "orange", "index": 0},
             "1": {"text": "AUTOPILOT", "color": TILE_OK, "index": 1}}}]),
    stat("Gear", flux("engine", "gear", last=True), 6, 0, 3, 2,
         text_mode="value", color_mode="background",
         mappings=[{"type": "value", "options": {
             "-1": {"text": "REVERSE", "color": "orange", "index": 0},
             "0": {"text": "NEUTRAL", "color": NEUTRAL, "index": 1},
             "1": {"text": "FORWARD", "color": TILE_OK, "index": 2}}}]),
    stat("Engine Thrust", flux("engine", "throttle_pct", last=True),
         9, 0, 3, 2, unit="percent", decimals=0, graph_mode="area"),
    trim_tile(12, 0, 3),
    stat("Engine Faults", flux("engine", "active_dtcs", last=True), 15, 0, 3, 2,
         mappings=FAULTS_MAPPINGS, text_mode="value", color_mode="background"),
    stat("Speed Error", flux("autopilot", "speed_error", last=True),
         18, 0, 3, 2, unit="velocityknot", decimals=2,
         thresholds=[(None, OK), (1, "yellow"), (2, "red")],
         graph_mode="area"),
    stat("XTE", flux("nav", "cross_track_error", last=True), 21, 0, 3, 2,
         unit=UNIT_M, decimals=1, graph_mode="area"),
    big_dial(instrument("Rudder Angle",
                        [target(flux("autopilot", "rudder_actual", last=True)),
                         target(flux("autopilot", "rudder_cmd", last=True),
                                ref="B")],
                        0, 2, 8, 9, rudder_js(),
                        {"kind": "rudder", "preview": 3})
             if ANALOG else
             stat("Rudder Angle", flux("autopilot", "rudder_actual", last=True),
                  0, 2, 8, 9, unit="degree", decimals=1)),
    big_dial(meter("Throttle", flux("autopilot", "throttle_cmd", last=True),
                   8, 2, 8, 9, 0, 100, "%", zones=None, decimals=0, split=5,
                   sub="COMMANDED THRUST", preview=64,
                   fallback_thresholds=[(None, ACCENT)])),
    big_dial(meter("Heading Error", flux("autopilot", "heading_error",
                                         last=True),
                   16, 2, 8, 9, -45, 45, "°",
                   zones=[(-25, ZRED), (-8, ZYELLOW), (8, ZGREEN),
                          (25, ZYELLOW), (45, ZRED)],
                   decimals=1, split=6, sub="OFF COURSE", preview=1.8,
                   fallback_thresholds=[(None, OK)])),
    ts("Heading — desired vs actual",
       [target(flux("autopilot", ["desired_heading", "actual_heading"]))],
       0, 11, 8, 6, unit="degree", min_=0, max_=360),
    ts("Rudder — actuator cmd / actual (°), PID (%)",
       [target(flux("autopilot", "rudder_actuator_cmd"), "actuator command"),
        target(flux("autopilot", "rudder_actual"), "actual", ref="B"),
        target(flux("autopilot", "rudder_cmd"), "PID output",
               ref="C")],
       8, 11, 8, 6, unit="degree",
       overrides=[{"matcher": {"id": "byFrameRefID", "options": "C"},
                   "properties": [{"id": "unit", "value": "percent"},
                                  {"id": "custom.axisPlacement",
                                   "value": "right"},
                                  {"id": "custom.lineStyle",
                                   "value": {"fill": "dash",
                                             "dash": [6, 4]}}]}]),
    ts("Speed — desired / cap / commanded / actual",
       [target(flux("autopilot", ["desired_speed", "speed_cap", "cmd_speed",
                                  "actual_speed"]))],
       16, 11, 8, 6, unit="velocityknot"),
], ["autopilot"], time_from="now-5m")
# Own id range for this page, and the shared counter put back, so adding
# the page does not renumber panels on every other dashboard.
for _i, _p in enumerate(_LIVE["panels"]):
    _p["id"] = 9001 + _i
_id[0] = _id_before_live

# ------------------------------------------------------------ FR-4 Engine
# Large machinery dials at 1 s for sea trials and troubleshooting (Vessel
# Health stays the overview with the electrical rows). Scales follow the
# vessel's engine type (typed_meter); a value the hull's engine does not
# report reads N/A. One screen: dials (h8), tiles (h2), fault text (h2),
# trends (h5).
_id_before_engine = _id[0]
_ENGINE = dashboard("usv-engine", "Engine", [
    big_dial(typed_meter("Tachometer", flux("engine", "rpm", last=True),
                         0, 0, 6, 8, "tach", "rpm", decimals=0, sub="×1000 RPM",
                         label_js="(x) => (x / 1000).toFixed(0)", preview=1540,
                         fallback_thresholds=[(None, OK), (4500, "yellow"),
                                              (5500, "red")])),
    big_dial(typed_meter("Coolant", flux("engine", "coolant_temp", last=True),
                         6, 0, 6, 8, "coolant", "°C", decimals=0, sub="TEMP °C",
                         preview=81,
                         fallback_thresholds=[(None, OK), (95, "yellow"),
                                              (100, "red")])),
    big_dial(typed_meter("Oil Pressure", flux("engine", "oil_pressure", last=True),
                         12, 0, 6, 8, "oil", "kPa", decimals=0, sub="OIL kPa",
                         preview=340,
                         fallback_thresholds=[(None, "red"), (150, "yellow"),
                                              (250, OK)])),
    big_dial(typed_meter("Boost", flux("engine", "intake_manifold_pressure",
                                       last=True),
                         18, 0, 6, 8, "boost", "kPa", decimals=0,
                         sub="INTAKE kPa", preview=162,
                         fallback_thresholds=[(None, OK), (250, "yellow"),
                                              (280, "red")])),
    stat("Oil Temp", flux("engine", "oil_temp", last=True), 0, 8, 3, 2,
         unit="celsius", decimals=0, no_value="N/A",
         thresholds=[(None, OK), (110, "yellow"), (125, "red")]),
    stat("Fuel Rate", flux("engine", "fuel_rate", last=True), 3, 8, 3, 2,
         unit="litreh", decimals=1, graph_mode="area", no_value="N/A"),
    stat("Engine Load", flux("engine", "percent_load", last=True), 6, 8, 3, 2,
         unit="percent", decimals=0, graph_mode="area", no_value="N/A"),
    stat("Torque", flux("engine", "percent_torque", last=True), 9, 8, 3, 2,
         unit="percent", decimals=0, no_value="N/A"),
    # "suffix: h", not Grafana's "h" unit, which rescales 30.75 h to "1.3 days".
    stat("Engine Hours", flux("engine", "runtime_h", last=True), 12, 8, 3, 2,
         unit="suffix: h", decimals=1, no_value="N/A"),
    stat("Fuel Level", flux("engine", "fuel_level", last=True), 15, 8, 3, 2,
         unit="percent", decimals=0, no_value="N/A",
         thresholds=[(None, "red"), (10, "yellow"), (20, OK)]),
    stat("Fault Lamps", LAMPS_QUERY, 18, 8, 3, 2, string_value=True,
         mappings=LAMPS_MAPPINGS, text_mode="value", color_mode="background"),
    stat("Active Faults", flux("engine", "active_dtcs", last=True), 21, 8, 3, 2,
         mappings=FAULTS_MAPPINGS, text_mode="value", color_mode="background"),
    # Every active fault (engine.dtc_text, "NONE" when clear) across the width.
    stat("Fault Codes", last_str("engine", "dtc_text"), 4, 10, 20, 2,
         string_value=True, mappings=DTC_TEXT_MAPPINGS,
         text_mode="value", color_mode="background"),
    ts("Engine RPM", [target(flux("engine", "rpm"), "RPM")], 0, 12, 8, 5,
       unit="rotrpm"),
    ts("Coolant / Oil Temperature",
       [target(flux("engine", "coolant_temp"), "coolant °C"),
        target(flux("engine", "oil_temp"), "oil °C", ref="B")],
       8, 12, 8, 5, unit="celsius"),
    ts("Oil Pressure / Boost",
       [target(flux("engine", "oil_pressure"), "oil kPa"),
        target(flux("engine", "intake_manifold_pressure"), "boost kPa",
               ref="B")],
       16, 12, 8, 5, unit="pressurekpa"),
    # Drive trim position (NMEA 2000 PGN 127488 trim, Signal K
    # propulsion.<id>.drive.trimState as a ratio; Yamaha gateway on usv-722).
    # Mercury on usv-725 sends no trim position: N/A. Last in the
    # list so the other panel ids stay as they were.
    stat("Drive Trim", flux("engine", "trim_position", last=True), 0, 10, 4, 2,
         unit="percent", decimals=0, graph_mode="area", no_value="N/A",
         thresholds=[(None, ACCENT)]),
], ["health"], time_from="now-15m")
for _i, _p in enumerate(_ENGINE["panels"]):
    _p["id"] = 9401 + _i
_id[0] = _id_before_engine
# Appended last (just before main) for the same reason: the header band
# ids are handed out in DASHBOARDS order.

# Autopilot page dials link to the large live view (panel link: no extra
# chip in the full tab row).
LIVE_LINK = {"title": "Open Autopilot Live (large dials, 1 s)",
             "url": "/d/usv-autopilot-live?var-vessel=${vessel}",
             "targetBlank": False}
for _d in DASHBOARDS:
    if _d["uid"] == "usv-autopilot":
        for _p in _d["panels"]:
            if _p.get("title") in ("Rudder Angle", "Throttle", "Heading Error") \
                    and _p["gridPos"]["h"] >= 5:
                _p.setdefault("links", []).append(dict(LIVE_LINK))

# ---------------------------------------------------------- FR-4 Vessel Health
def burn_query(kind):
    """Range (nm) or time (h) at the current burn from the last fuel
    litres, fuel rate and SOG of the past two minutes. No row (the stat
    shows N/A) when any input is missing, the rate is under 0.1 L/h or the
    boat is not under way (range only)."""
    expr = ("r.fuel_remaining_l / r.fuel_rate * r.sog" if kind == "range"
            else "r.fuel_remaining_l / r.fuel_rate")
    cond = ("exists r.fuel_remaining_l and exists r.fuel_rate and r.fuel_rate >= 0.1"
            + (" and exists r.sog and r.sog >= 0.5" if kind == "range" else ""))
    return ('from(bucket: "telemetry")\n'
            '  |> range(start: -2m)\n'
            '  |> filter(fn: (r) => r.vessel == "${vessel}")\n'
            '  |> filter(fn: (r) => (r._measurement == "engine" and '
            '(r._field == "fuel_remaining_l" or r._field == "fuel_rate")) or '
            '(r._measurement == "nav" and r._field == "sog"))\n'
            '  |> last()\n'
            '  |> keep(columns: ["_field", "_value"])\n'
            '  |> group()\n'
            '  |> map(fn: (r) => ({r with k: 0}))\n'
            '  |> pivot(rowKey: ["k"], columnKey: ["_field"], valueColumn: "_value")\n'
            f'  |> filter(fn: (r) => {cond})\n'
            f'  |> map(fn: (r) => ({{_value: {expr}}}))\n')


DASHBOARDS.append(dashboard("usv-health", "Vessel Health", [
    # Instrument cluster: the six primary machinery/electrical meters.
    # Tachometer, Coolant and Oil Pressure take their scale from the
    # vessel's engine type (ENGINE_TYPES / typed_meter).
    typed_meter("Tachometer", flux("engine", "rpm", last=True), 0, 0, 4, 6,
                "tach", "rpm", decimals=0, sub="×1000 RPM",
                label_js="(x) => (x / 1000).toFixed(0)", preview=2547,
                fallback_thresholds=[(None, OK), (4500, "yellow"),
                                     (5500, "red")]),
    typed_meter("Coolant", flux("engine", "coolant_temp", last=True), 4, 0, 4, 6,
                "coolant", "°C", decimals=0, sub="TEMP °C", preview=85,
                fallback_thresholds=[(None, OK), (95, "yellow"),
                                     (100, "red")]),
    typed_meter("Oil Pressure", flux("engine", "oil_pressure", last=True),
                8, 0, 4, 6, "oil", "kPa", decimals=0, sub="OIL kPa", preview=402,
                fallback_thresholds=[(None, "red"), (150, "yellow"),
                                     (250, OK)]),
    meter("Fuel", flux("engine", "fuel_level", last=True), 12, 0, 4, 6,
          0, 100, "%",
          zones=[(10, ZRED), (20, ZYELLOW), (100, ZGREEN)],
          decimals=0, split=4, sub="FUEL",
          label_js=("(x) => (x === 0 ? 'E' : (x === 50 ? '\\u00bd' : "
                    "(x === 100 ? 'F' : '')))"),
          preview=91, preview_labels={0: "E", 50: "½", 100: "F"},
          fallback_thresholds=[(None, "red"), (10, "yellow"),
                               (20, OK)]),
    meter("Battery", flux("electrical", "battery_voltage", last=True),
          16, 0, 4, 6, 10, 15, "V",
          zones=[(11.8, ZRED), (12.4, ZYELLOW), (14.6, ZGREEN),
                 (15, ZYELLOW)],
          decimals=2, split=5, sub="VOLTS", preview=12.91,
          fallback_thresholds=[(None, "red"), (11.8, "yellow"),
                               (12.4, OK)]),
    meter("State of Charge", flux("electrical", "soc", last=True),
          20, 0, 4, 6, 0, 100, "%",
          zones=[(25, ZRED), (50, ZYELLOW), (100, ZGREEN)],
          decimals=0, split=4, sub="BATTERY SOC", preview=87,
          fallback_thresholds=[(None, "red"), (25, "yellow"),
                               (50, OK)]),
    # Secondary tiles. Engine detail (fuel rate, hours, oil temperature,
    # load, boost, trends) lives on the Engine page; this page keeps the
    # vessel picture: engine fault summary, electrical and power.
    stat("Engine Faults", flux("engine", "active_dtcs", last=True), 0, 6, 3, 3,
         mappings=FAULTS_MAPPINGS, text_mode="value", color_mode="background"),
    stat("Battery Current", flux("electrical", "battery_current", last=True),
         3, 6, 3, 3, unit="amp", decimals=1, graph_mode="area"),
    stat("Charging", last_str("electrical", "charging_status"),
         6, 6, 3, 3, text_mode="value", color_mode="background",
         mappings=CHARGING_MAPPINGS),
    # Fuel in litres and what it buys (v1.2.33): engine.fuel_remaining_l is
    # the tank % x VESSEL_FUEL_L (usv.env), written by the bridge that
    # writes fuel_level. Range and time at the current burn divide it by the
    # live fuel rate (and multiply by SOG for range): N/A while the engine
    # is stopped, the boat is not under way (SOG < 0.5 kn) or the vessel
    # has no fuel rate or capacity.
    stat("Fuel Remaining", flux("engine", "fuel_remaining_l", last=True),
         9, 6, 3, 3, unit="litre", decimals=0, graph_mode="area",
         no_value="N/A"),
    stat("Range Left", burn_query("range"), 12, 6, 3, 3,
         unit=UNIT_NM, decimals=0, no_value="N/A"),
    stat("Time Left", burn_query("time"), 15, 6, 3, 3,
         unit="suffix: h", decimals=1, no_value="N/A"),
    stat("Sys Voltage", flux("power", "system_voltage", last=True),
         18, 6, 3, 3, unit="volt", decimals=2, graph_mode="area"),
    # ECU supply: J1939 VEP1 from iCANRec on the Mercury diesel. Total
    # power stays in the Power / System Voltage trend; the alternator
    # (Signal K electrical.alternators.0.voltage or the Yamaha gateway's
    # alternatorVoltage) joined the Battery Voltage / Current trend.
    stat("ECU Supply", flux("engine", "ecu_voltage", last=True),
         21, 6, 3, 3, unit="volt", decimals=1),
    # Trends: two rows of two.
    ts("Battery Voltage / Current",
       [target(flux("electrical", ["battery_voltage"]), "voltage"),
        target(flux("electrical", ["alternator_voltage"]), "alternator", ref="C"),
        target(flux("electrical", ["battery_current"]), "current", ref="B")],
       0, 9, 12, 6, unit="volt",
       overrides=[{"matcher": {"id": "byFrameRefID", "options": "B"},
                   "properties": [{"id": "unit", "value": "amp"},
                                  {"id": "custom.axisPlacement",
                                   "value": "right"}]}]),
    ts("Power / System Voltage",
       [target(flux("power", "total_power"), "power W"),
        target(flux("power", "system_voltage"), "sys V", ref="B")],
       12, 9, 12, 6, unit="watt",
       overrides=[{"matcher": {"id": "byFrameRefID", "options": "B"},
                   "properties": [{"id": "unit", "value": "volt"},
                                  {"id": "custom.axisPlacement",
                                   "value": "right"}]}]),
    ts("State of Charge", [target(flux("electrical", "soc"), "SOC")],
       0, 15, 12, 6, unit="percent", min_=0, max_=100),
    # Litres on the left axis when the vessel has a fuel capacity, the
    # tank % as well (same axis range 0..100 reads oddly next to litres, so
    # the % line is hidden from the axis and kept in the tooltip / legend).
    ts("Fuel Remaining / Consumption",
       [target(flux("engine", "fuel_remaining_l"), "remaining L"),
        target(flux("engine", "fuel_level"), "level %", ref="C"),
        target(flux("engine", "fuel_rate"), "rate L/h", ref="B")],
       12, 15, 12, 6, unit="litre",
       overrides=[{"matcher": {"id": "byFrameRefID", "options": "B"},
                   "properties": [{"id": "unit", "value": "litreh"},
                                  {"id": "custom.axisPlacement",
                                   "value": "right"}]},
                  {"matcher": {"id": "byFrameRefID", "options": "C"},
                   "properties": [{"id": "unit", "value": "percent"},
                                  {"id": "custom.axisPlacement",
                                   "value": "hidden"}]}]),
], ["health"]))

# ------------------------------------------------------------- FR-5 CAN Bus
DASHBOARDS.append(dashboard("usv-canbus", "CAN Bus", [
    stat("Interface", flux("canbus", "interface_up", last=True), 0, 0, 4, 3,
         mappings=updown_map("DOWN", "UP"), text_mode="value",
         color_mode="background"),
    gauge("Bus Utilization", flux("canbus", "utilization_pct", last=True),
          4, 0, 4, 3, thresholds=[(None, OK), (50, "yellow"), (75, "red")]),
    stat("Messages / s", flux("canbus", "msg_rate", last=True), 8, 0, 4, 3,
         decimals=0, graph_mode="area"),
    stat("Error Frames (5s)", flux("canbus", "error_frames", last=True),
         12, 0, 4, 3, decimals=0,
         thresholds=[(None, OK), (1, "yellow"), (10, "red")]),
    stat("Dropped Frames", flux("canbus", "dropped_frames", last=True),
         16, 0, 4, 3, decimals=0,
         thresholds=[(None, TILE_OK), (1, "red")], color_mode="background"),
    stat("Active PGNs", flux("canbus", "active_pgns", last=True), 20, 0, 4, 3,
         decimals=0),
    # Helm trim switch and engine / helm faults (J1939 DM1 from address 210).
    trim_tile(0, 3, 4),
    stat("Active Faults", flux("engine", "active_dtcs", last=True), 4, 3, 4, 2,
         mappings=FAULTS_MAPPINGS, text_mode="value", color_mode="background"),
    stat("Fault Lamps", LAMPS_QUERY, 8, 3, 5, 2, string_value=True,
         mappings=LAMPS_MAPPINGS, text_mode="value", color_mode="background"),
    stat("Fault Codes", DTC_TEXT_QUERY, 13, 3, 11, 2, string_value=True,
         mappings=DTC_TEXT_MAPPINGS,
         text_mode="value", color_mode="background"),
    ts("Message Rate", [target(flux("canbus", "msg_rate"), "msgs/s")],
       0, 7, 12, 6),
    ts("Bus Utilization", [target(flux("canbus", "utilization_pct"), "util %")],
       12, 7, 12, 6, unit="percent", min_=0, max_=100),
    ts("Error / Dropped Frames",
       [target(flux("canbus", ["error_frames", "dropped_frames"]))],
       0, 13, 12, 6),
    panel("table", "PGN Frequency (msgs/s)",
          [target('from(bucket: "telemetry")\n'
                  '  |> range(start: v.timeRangeStart, stop: v.timeRangeStop)\n'
                  '  |> filter(fn: (r) => r._measurement == "canbus_pgn" and r._field == "rate")\n'
                  '  |> filter(fn: (r) => r.vessel == "${vessel}")\n'
                  '  |> last()\n'
                  '  |> group()\n'
                  '  |> keep(columns: ["pgn", "_value"])\n'
                  '  |> sort(columns: ["_value"], desc: true)\n'
                  '  |> rename(columns: {pgn: "PGN / CAN id", _value: "msgs/s"})\n')],
          12, 13, 12, 6,
          options={"showHeader": True}),
], ["canbus"]))
# The four helm / fault tiles get their own id range and the shared counter
# is put back, so adding them does not renumber panels on other pages.
_can = DASHBOARDS[-1]["panels"]
_new = [p for p in _can if p["title"] in ("Trim Switch", "Active Faults",
                                           "Fault Lamps", "Fault Codes")]
_first = min(p["id"] for p in _new)
for _i, _p in enumerate(_new):
    _p["id"] = 9101 + _i
for _p in _can:
    if _p not in _new and _p["id"] > _first:
        _p["id"] -= len(_new)
_id[0] -= len(_new)
# Drive interface (iCAN, v1.2.43): what is really sent on the bus, who
# drives, the CAN switch and IDs blocked by another node. Own id range.
ON_OFF_MAP = [{"type": "value", "options": {
    "0": {"text": "OFF", "color": NEUTRAL, "index": 0},
    "1": {"text": "ON", "color": TILE_OK, "index": 1}}}]
_drive = [
    stat("Drive Interface", flux("can_drive", "status", last=True), 0, 5, 3, 2, string_value=True,
         mappings=[{"type": "value", "options": {
             "ACTIVE": {"text": "ACTIVE", "color": TILE_OK, "index": 0},
             "STANDBY": {"text": "STANDBY", "color": NEUTRAL, "index": 1},
             "NO_BUS": {"text": "NO BUS", "color": "red", "index": 2},
             "INHIBITED": {"text": "NO PROFILE", "color": "red", "index": 3}}}],
         text_mode="value", color_mode="background"),
    stat("Driven By", flux("can_drive", "owner", last=True), 3, 5, 3, 2, string_value=True,
         mappings=[{"type": "value", "options": {
             "AUTONOMY": {"text": "AUTONOMY", "color": TILE_OK, "index": 0},
             "MANUAL": {"text": "MANUAL", "color": "orange", "index": 1}}}],
         text_mode="value", color_mode="background"),
    stat("CAN Control", flux("can_drive", "send", last=True), 6, 5, 3, 2,
         mappings=ON_OFF_MAP, text_mode="value", color_mode="background"),
    stat("Gear Sent", flux("can_drive", "tx_gear", last=True), 9, 5, 3, 2, string_value=True,
         mappings=[{"type": "value", "options": {
             "fwd": {"text": "FORWARD", "color": TILE_OK, "index": 0},
             "neutral": {"text": "NEUTRAL", "color": NEUTRAL, "index": 1},
             "rev": {"text": "REVERSE", "color": "orange", "index": 2}}}],
         text_mode="value", color_mode="background"),
    stat("Thrust Sent", flux("can_drive", "tx_thrust_pct", last=True), 12, 5, 3, 2,
         unit="percent", decimals=0, no_value="N/A"),
    stat("Rudder Sent", flux("can_drive", "tx_rudder_pct", last=True), 15, 5, 3, 2,
         unit="percent", decimals=0, no_value="N/A"),
    stat("Trim Sent", flux("can_drive", "trim_state", last=True), 18, 5, 3, 2, string_value=True,
         mappings=[{"type": "value", "options": {
             "idle": {"text": "IDLE", "color": NEUTRAL, "index": 0},
             "up": {"text": "UP", "color": ACCENT, "index": 1},
             "down": {"text": "DOWN", "color": ACCENT, "index": 2}}}],
         text_mode="value", color_mode="background"),
    stat("Bus Conflict", flux("can_drive", "conflict", last=True), 21, 5, 3, 2, string_value=True,
         mappings=[{"type": "value", "options": {
             "none": {"text": "NONE", "color": TILE_OK, "index": 0}}},
             {"type": "regex", "options": {"pattern": "(.+)", "result": {
                 "text": "BLOCKED: $1", "color": "red", "index": 1}}}],
         text_mode="value", color_mode="background"),
]
for _i, _p in enumerate(_drive):
    _p["id"] = 9501 + _i
_id[0] -= len(_drive)
_can.extend(_drive)

# ------------------------------------------------ FR-12 Pre-Departure Readiness
# Every check is decided onboard by grafana_bridge (usv::EvaluateReadiness in
# mission_logic.hpp) and written as one row per check (measurement
# "readiness", tag check: seq, status, name, evidence) plus a
# "readiness_summary". The dashboard only displays them. Status codes:
# 0 PASS, 1 FAIL, 2 N/A (sensor not fitted / no data; FAIL under the
# vessel profile), 3 CONSOLE (the operator console has not published it).
READINESS_WINDOW = "-60s"   # bridge writes every readiness_period_s (5 s)


def readiness_query():
    """One row per check, in check order. Numeric and string fields are
    pivoted separately and joined (Flux refuses to group float and string
    _value columns into one table)."""
    return ('base = from(bucket: "telemetry")\n'
            f'  |> range(start: {READINESS_WINDOW})\n'
            '  |> filter(fn: (r) => r._measurement == "readiness")\n'
            '  |> filter(fn: (r) => r.vessel == "${vessel}")\n'
            '  |> last()\n'
            '  |> drop(columns: ["_start", "_stop", "_measurement", "vessel", "_time"])\n'
            'nums = base |> filter(fn: (r) => r._field == "seq" or r._field == "status")\n'
            '  |> group()\n'
            '  |> pivot(rowKey: ["check"], columnKey: ["_field"], valueColumn: "_value")\n'
            'strs = base |> filter(fn: (r) => r._field == "name" or r._field == "evidence")\n'
            '  |> group()\n'
            '  |> pivot(rowKey: ["check"], columnKey: ["_field"], valueColumn: "_value")\n'
            'join(tables: {n: nums, s: strs}, on: ["check"])\n'
            '  |> sort(columns: ["seq"])\n'
            '  |> keep(columns: ["seq", "name", "evidence", "status"])\n')


def checklist_query():
    """Console checklist mirror: rows written by grafana_bridge from the
    console's CHECKLIST_STATUS (measurement checklist, tag seq: one row per
    item with its description and TICKED / PENDING, then a footer with the
    count and the operator)."""
    return ('from(bucket: "telemetry")\n'
            f'  |> range(start: {READINESS_WINDOW})\n'
            '  |> filter(fn: (r) => r._measurement == "checklist" and (r._field == "item" or r._field == "description" or r._field == "state"))\n'
            '  |> filter(fn: (r) => r.vessel == "${vessel}")\n'
            '  |> last()\n'
            '  |> drop(columns: ["_start", "_stop", "_measurement", "vessel", "_time"])\n'
            '  |> group()\n'
            '  |> pivot(rowKey: ["seq"], columnKey: ["_field"], valueColumn: "_value")\n'
            '  |> sort(columns: ["seq"])\n'
            '  |> keep(columns: ["item", "description", "state"])\n')


status_mappings = [{"type": "value", "options": {
    "0": {"text": "PASS", "color": TILE_OK, "index": 0},
    "1": {"text": "FAIL", "color": "red", "index": 1},
    "2": {"text": "N/A", "color": "#3d434b", "index": 2},
    "3": {"text": "CONSOLE", "color": "#6e7680", "index": 3}}}]
checklist_state_mappings = state_map([("COMPLETE", TILE_OK), ("OPEN", "orange"),
                                      ("TICKED", "#4d545c"), ("PENDING", "orange"),
                                      ("UNKNOWN", "#6e7680"), ("N/A", "#3d434b")])
ready_mappings = value_color_map([(0, "NOT READY", "red"), (1, "READY", TILE_OK)])


def status_table(title, query, x, y, w, h, status_col, mappings, name_col,
                 columns, description=None, no_value=None, show_header=True):
    """Table with a coloured status column (mappings on that column only)
    and a fixed column order. show_header=False saves one row height where
    every row must fit without scrolling."""
    p = panel("table", title, [target(query)], x, y, w, h,
              options={"showHeader": show_header, "cellHeight": "sm"},
              no_value=no_value,
              overrides=[{"matcher": {"id": "byName", "options": status_col},
                          "properties": [{"id": "displayName", "value": "Status"},
                                         {"id": "custom.width", "value": 100},
                                         {"id": "custom.align", "value": "center"},
                                         {"id": "mappings", "value": mappings},
                                         {"id": "custom.cellOptions",
                                          "value": {"type": "color-background", "mode": "basic"}}]},
                         {"matcher": {"id": "byName", "options": name_col},
                          "properties": [{"id": "displayName", "value": "Check"},
                                         {"id": "custom.width", "value": 170}]},
                         {"matcher": {"id": "byName", "options": "evidence"},
                          "properties": [{"id": "displayName", "value": "Evidence"},
                                         {"id": "custom.minWidth", "value": 300}]},
                         {"matcher": {"id": "byName", "options": "seq"},
                          "properties": [{"id": "displayName", "value": "#"},
                                         {"id": "custom.width", "value": 36}]},
                         {"matcher": {"id": "byName", "options": "item"},
                          "properties": [{"id": "displayName", "value": "Item"},
                                         {"id": "custom.width", "value": 230}]},
                         {"matcher": {"id": "byName", "options": "description"},
                          "properties": [{"id": "displayName", "value": "Description"},
                                         {"id": "custom.minWidth", "value": 260},
                                         {"id": "color", "value": {"mode": "fixed", "fixedColor": "#8e9299"}},
                                         {"id": "custom.cellOptions", "value": {"type": "color-text"}}]}])
    p["transformations"] = [{"id": "organize", "options": {
        "indexByName": {c: i for i, c in enumerate(columns)}}}]
    if description:
        p["description"] = description
    return p


# Vessel particulars card: the rows signalk_bridge writes from VESSEL_* in
# usv.env as string fields "01 Length x beam x height" ... of the `vessel`
# measurement, one table row per field in field order, so the card lists
# whatever the hull's usv.env defines without a panel per row. (v1.2.33
# built HTML in a hidden variable; Grafana escapes variable values in an
# HTML text panel, so the card showed the raw tags.)
def particulars_query():
    return ('import "strings"\n'
            'from(bucket: "telemetry")\n'
            '  |> range(start: -1d)\n'
            '  |> filter(fn: (r) => r._measurement == "vessel" and r.vessel == "${vessel}")\n'
            '  |> filter(fn: (r) => r._field =~ /^[0-9][0-9] /)\n'
            '  |> last()\n'
            '  |> keep(columns: ["_field", "_value"])\n'
            '  |> group()\n'
            '  |> sort(columns: ["_field"])\n'
            '  |> map(fn: (r) => ({particular: strings.substring(v: r._field, start: 3, '
            'end: strings.strlen(v: r._field)), value: r._value}))\n')


def readiness_counts_panel(x, y, w, h):
    """The four readiness counts as one stat panel: one query per count, so
    they keep their order (Passed, Failed, N/A, Console), each named and
    coloured by its own override (Failed red from 1, Console orange from 1,
    N/A grey, Passed cyan)."""
    counts = [("A", "passed", "Passed", [(None, OK)]),
              ("B", "failed", "Failed", [(None, OK), (1, "red")]),
              ("C", "na", "N/A", [(None, "#8e9299")]),
              ("D", "console", "Console", [(None, "#8e9299"), (1, "orange")])]
    p = panel("stat", "Checks",
              [target(flux("readiness_summary", f, last=True), ref=r) for r, f, _, _ in counts],
              x, y, w, h, decimals=0, thresholds=[(None, OK)],
              overrides=[{"matcher": {"id": "byFrameRefID", "options": r},
                          "properties": [{"id": "displayName", "value": name},
                                         {"id": "thresholds", "value": _thresholds(thr)},
                                         {"id": "color", "value": {"mode": "thresholds"}}]}
                         for r, _, name, thr in counts],
              options={"reduceOptions": {"calcs": ["lastNotNull"]},
                       "textMode": "value_and_name", "graphMode": "none",
                       "colorMode": "value", "justifyMode": "center",
                       "orientation": "vertical",
                       "text": {"valueSize": 24, "titleSize": 12}})
    p["description"] = ("Automatic checks by result. N/A: sensor not fitted or not "
                        "reporting (FAIL under readiness_profile = vessel). Console: "
                        "waiting on the operator console (ENC selection, checklist); "
                        "it holds the verdict at NOT READY.")
    return p


def particulars_panel(x, y, w, h):
    p = panel("table", "Vessel particulars — ${vessel_class}, hull ${vessel}",
              [target(particulars_query())], x, y, w, h,
              options={"showHeader": False, "cellHeight": "sm"},
              no_value="no particulars: set VESSEL_* in /etc/usv/usv.env (deploy/usv.env.example)",
              overrides=[{"matcher": {"id": "byName", "options": "particular"},
                          "properties": [{"id": "custom.width", "value": 170},
                                         {"id": "color", "value": {"mode": "fixed", "fixedColor": "#8e9299"}},
                                         {"id": "custom.cellOptions", "value": {"type": "color-text"}}]},
                         {"matcher": {"id": "byName", "options": "value"},
                          "properties": [{"id": "custom.cellOptions", "value": {"type": "auto", "wrapText": True}}]}])
    p["transformations"] = [{"id": "organize", "options": {
        "indexByName": {"particular": 0, "value": 1}}}]
    p["description"] = ("From the general arrangement drawing, as VESSEL_* in "
                        "/etc/usv/usv.env (deploy/usv.env.example). Fuel capacity "
                        "gives the litres on the Vessel Health page and the "
                        "Endurance check; max speed gives the Speed cap check.")
    return p


# One screen, no scrolling: the 18 automatic checks (v1.2.43: Navigation
# integrity) need the full page height (36 px rows, no column header, no
# title bar; the title is the hover description), so the verdict, the four
# counts, mission state / operator link / route and the vessel particulars
# card stack in a column on the right, above the console checklist. This
# page is one grid row taller than the others (READINESS_ROWS).
DASHBOARDS.append(dashboard("usv-readiness", "Readiness", [
    # A — verdict and counts (right column, x 16..24)
    stat("Readiness", flux("readiness_summary", "ready", last=True), 16, 0, 8, 3,
         mappings=ready_mappings, text_mode="value", color_mode="background",
         no_value="MISSION OFF"),
    # Passed / Failed / N/A / Console in one panel, side by side, each with
    # its own colour: four 2-column tiles would cut their titles, and two
    # rows of 4-column tiles leave no room for the particulars card.
    readiness_counts_panel(16, 3, 8, 2),
    stat("Mission State", last_str("mission_state", "state"), 16, 5, 3, 2,
         mappings=mission_state_mappings, text_mode="value", color_mode="background",
         no_value="MISSION OFF"),
    stat("Operator Link", flux("autonomy", "operator_link", last=True), 19, 5, 3, 2,
         mappings=operator_link_mappings, text_mode="value", color_mode="background"),
    # Narrow tile: smaller text so "10-19-2026 (safe)" reads on two lines.
    stat("Route", last_str("mission_state", "route_name"), 22, 5, 2, 2, text_mode="value",
         no_value="no route", value_size=14),
    particulars_panel(16, 7, 8, 6),
    # B — the two lists
    status_table("",
                 readiness_query(), 0, 0, 16, 18, "status", status_mappings, "name",
                 ["seq", "name", "evidence", "status"], show_header=False,
                 # grafana_bridge runs under pAntler: no rows while the mission is off.
                 no_value="Mission not running: grafana_bridge starts with the mission (pAntler). Start the mission and the checks appear within 5 s",
                 description=(
                     "Automatic checks, verified onboard from live telemetry. "
                     "Decided by grafana_bridge every 5 s from the latest telemetry "
                     "(GPS ≥ 6 satellites and HDOP ≤ 2.5, route inside the region, every "
                     "Antler app connected, rudder within ±35° and following the command, "
                     "coolant below the engine type's overheating limit (Yamaha outboard 85 °C, "
                     "Mercury diesel 100 °C), fuel ≥ 20 %, no active DTC, SOC ≥ 30 %, fuel and "
                     "battery hours ≥ route time x margin, helm speed cap within the hull "
                     "maximum, host load < 90 %). N/A = sensor "
                     "not fitted or no data; under "
                     "readiness_profile = vessel it counts as FAIL. CONSOLE = the "
                     "operator console has not published it (ENC selection, checklist). "
                     "READY needs no FAIL and no CONSOLE item.")),
    status_table("Console checklist — manual items ticked in Swiftships-UI",
                 checklist_query(), 16, 13, 8, 5, "state", checklist_state_mappings, "item",
                 ["item", "description", "state"],
                 no_value="console not publishing CHECKLIST_STATUS (docs/deployment.md, Console checklist)",
                 description=(
                     "Mirror of the console's pre-departure checklist "
                     "(CHECKLIST_STATUS from the operator UI). Items are ticked in "
                     "Swiftships-UI; Grafana shows every item with its state, who ticked "
                     "and the count. Item names and descriptions come from the console "
                     "(items=) or from checklist_items in the bridge ProcessConfig.")),
], ["readiness"]))

# ------------------------------------------------ FR-6 Traffic (collision avoidance)
# Contacts come from grafana_bridge: NODE_REPORT / AIS-style reports and
# radar targets, with range, bearings, CPA / TCPA, encounter type and a
# risk level computed onboard (mission_logic.hpp). Rows are written only
# while a contact is active, so a 30 s window is the live picture.
CONTACT_FIELDS = ["range_m", "bearing", "rel_bearing", "spd_kn", "hdg",
                  "rel_speed_kn", "cpa_m", "tcpa_s", "risk"]
CONTACT_STR_FIELDS = ["encounter", "type"]
NO_SENTINEL = '  |> filter(fn: (r) => r._value >= 0.0)'   # drop the "nothing to report" -1 markers


def contacts_query(limit=None, rank=True):
    """One row per live contact with every field as a column, riskiest
    first. The window matches the bridge's contact expiry (contact_ttl_s,
    10 s) so an expired contact leaves the table within seconds. Numeric
    and string fields are pivoted separately and joined: Flux refuses to
    group float and string _value columns into one table."""
    nums = " or ".join(f'r._field == "{f}"' for f in CONTACT_FIELDS)
    strs = " or ".join(f'r._field == "{f}"' for f in CONTACT_STR_FIELDS)
    q = ('base = from(bucket: "telemetry")\n'
         '  |> range(start: -12s)\n'
         '  |> filter(fn: (r) => r._measurement == "contacts")\n'
         '  |> filter(fn: (r) => r.vessel == "${vessel}")\n'
         '  |> last()\n'
         '  |> drop(columns: ["_start", "_stop", "_measurement", "vessel", "_time"])\n'
         f'nums = base |> filter(fn: (r) => {nums})\n'
         '  |> group()\n'
         '  |> pivot(rowKey: ["contact", "source"], columnKey: ["_field"], valueColumn: "_value")\n'
         f'strs = base |> filter(fn: (r) => {strs})\n'
         '  |> group()\n'
         '  |> pivot(rowKey: ["contact", "source"], columnKey: ["_field"], valueColumn: "_value")\n'
         'join(tables: {n: nums, s: strs}, on: ["contact", "source"])\n'
         '  |> filter(fn: (r) => exists r.range_m)\n')
    if rank:
        q += ('  |> sort(columns: ["cpa_m"])\n'
              '  |> sort(columns: ["risk"], desc: true)\n')
    if limit:
        q += f'  |> limit(n: {limit})\n'
    return q


# Helm avoidance (COLAV_ALERTS from the production iWebSocket): one `colav`
# row per contact the helm is steering around, written every second while
# iWebSocket lists it (15 s after the avoidance ends, marked inactive).
COLAV_FIELDS = ["active", "extremis", "for_s", "range_m", "cpa_m", "tcpa_s"]
COLAV_STR_FIELDS = ["situation", "rule"]


def colav_query():
    """One row per contact the helm is avoiding or has just cleared: the
    COLREGS situation with its rule, CPA / TCPA, range and a state column
    (IN EXTREMIS, AVOIDING, CLEAR); in extremis first, then by CPA."""
    nums = " or ".join(f'r._field == "{f}"' for f in COLAV_FIELDS)
    strs = " or ".join(f'r._field == "{f}"' for f in COLAV_STR_FIELDS)
    return ('base = from(bucket: "telemetry")\n'
            '  |> range(start: -5s)\n'
            '  |> filter(fn: (r) => r._measurement == "colav")\n'
            '  |> filter(fn: (r) => r.vessel == "${vessel}")\n'
            '  |> last()\n'
            '  |> drop(columns: ["_start", "_stop", "_measurement", "vessel", "_time"])\n'
            f'nums = base |> filter(fn: (r) => {nums})\n'
            '  |> group()\n'
            '  |> pivot(rowKey: ["contact"], columnKey: ["_field"], valueColumn: "_value")\n'
            f'strs = base |> filter(fn: (r) => {strs})\n'
            '  |> group()\n'
            '  |> pivot(rowKey: ["contact"], columnKey: ["_field"], valueColumn: "_value")\n'
            'join(tables: {n: nums, s: strs}, on: ["contact"])\n'
            '  |> map(fn: (r) => ({\n'
            '      contact: r.contact,\n'
            '      situation: if r.rule == "-" then r.situation else r.situation + " (" + r.rule + ")",\n'
            '      cpa_m: r.cpa_m, tcpa_s: r.tcpa_s, range_m: r.range_m, for_s: r.for_s,\n'
            '      state: if r.extremis > 0.0 then "IN EXTREMIS" else if r.active > 0.0 then "AVOIDING" else "CLEAR",\n'
            '      rank: if r.extremis > 0.0 then 0 else if r.active > 0.0 then 1 else 2,\n'
            '      cpa_rank: if r.cpa_m >= 0.0 then r.cpa_m else 1000000000.0}))\n'
            '  |> sort(columns: ["rank", "cpa_rank"])\n'
            '  |> drop(columns: ["rank", "cpa_rank"])\n')


# Contacts the helm is avoiding, for the rings on the traffic view (refId V).
COLAV_RING_QUERY = ('from(bucket: "telemetry")\n'
                    '  |> range(start: -5s)\n'
                    '  |> filter(fn: (r) => r._measurement == "colav")\n'
                    '  |> filter(fn: (r) => r.vessel == "${vessel}")\n'
                    '  |> filter(fn: (r) => r._field == "active" or r._field == "extremis")\n'
                    '  |> last()\n'
                    '  |> drop(columns: ["_start", "_stop", "_measurement", "vessel", "_time"])\n'
                    '  |> group()\n'
                    '  |> pivot(rowKey: ["contact"], columnKey: ["_field"], valueColumn: "_value")\n'
                    '  |> filter(fn: (r) => r.active > 0.0)\n')

colav_state_mappings = [{"type": "value", "options": {
    "IN EXTREMIS": {"text": "IN EXTREMIS", "color": "red", "index": 0},
    "AVOIDING": {"text": "AVOIDING", "color": "orange", "index": 1},
    "CLEAR": {"text": "CLEAR", "color": TILE_OK, "index": 2}}}]


def worst_contact_query(column, string=False):
    """A single value (the riskiest contact's column) for a stat tile."""
    q = contacts_query(limit=1)
    q += f'  |> map(fn: (r) => ({{_value: r.{column}}}))\n'
    return q


# The selected display range reaches the panel as data (refId R), so it
# works whatever the ECharts plugin version exposes to panel code.
TRAFFIC_RANGE_QUERY = ('import "array"\n'
                       'array.from(rows: [{_value: float(v: "${traffic_range}")}])\n')
TRAFFIC_RANGE_VAR = {
    "name": "traffic_range", "label": "Traffic range", "type": "custom",
    "query": "Auto : 0,0.25 nm : 0.25,0.5 nm : 0.5,1 nm : 1,2 nm : 2,"
             "3 nm : 3,6 nm : 6,12 nm : 12,24 nm : 24",
    "current": {"text": "Auto", "value": "0"},
    "options": [{"text": t, "value": v, "selected": v == "0"} for t, v in [
        ("Auto", "0"), ("0.25 nm", "0.25"), ("0.5 nm", "0.5"), ("1 nm", "1"),
        ("2 nm", "2"), ("3 nm", "3"), ("6 nm", "6"), ("12 nm", "12"),
        ("24 nm", "24")]],
    "hide": 0, "includeAll": False, "multi": False,
}


def traffic_js():
    """Heading-up polar traffic view: own ship at the centre, range rings
    in nautical miles (auto-scaled 0.5 / 1 / 2 / 5 nm). Contacts use the
    ECDIS / OpenCPN AIS target style: a hull rotated to the contact's
    heading, coloured by source (green report / AIS, amber radar, red HIGH
    risk), with a 6-minute speed vector ahead of the bow. A dashed CPA line
    runs to the riskiest contact."""
    return ECH_PRELUDE + r"""
const ownHdg = val('B') || 0;
const rows = [];
for (const s of context.panel.data.series) {
  if (s.refId === 'B' || s.refId === 'R' || s.refId === 'V') { continue; }
  const col = {};
  for (const f of s.fields) { col[f.name] = f.values && f.values.toArray ? f.values.toArray() : f.values; }
  const n = (col.range_m || []).length;
  for (let i = 0; i < n; i++) {
    rows.push({ id: col.contact ? col.contact[i] : ('#' + i),
      source: col.source ? col.source[i] : 'radar',
      range: col.range_m[i], rb: col.rel_bearing ? col.rel_bearing[i] : null,
      hdg: col.hdg ? col.hdg[i] : null, spd: col.spd_kn ? col.spd_kn[i] : null,
      risk: col.risk ? col.risk[i] : 0,
      cpa: col.cpa_m ? col.cpa_m[i] : null, tcpa: col.tcpa_s ? col.tcpa_s[i] : null });
  }
}
// Contacts the helm is avoiding (COLAV_ALERTS, refId V): ringed orange,
// red in extremis.
const avoid = {};
for (const s of context.panel.data.series) {
  if (s.refId !== 'V') { continue; }
  const col = {};
  for (const f of s.fields) { col[f.name] = f.values && f.values.toArray ? f.values.toArray() : f.values; }
  const n = (col.contact || []).length;
  for (let i = 0; i < n; i++) { avoid[col.contact[i]] = col.extremis && col.extremis[i] > 0 ? 2 : 1; }
}
const NM = 1852;
// Display range: the traffic_range variable (refId R, 0 = auto) or, on
// auto, the smallest step that holds every contact. The + / - buttons step
// through RANGES; contacts outside a zoomed-in range are counted, not drawn.
const RANGES = [0.25, 0.5, 1, 2, 3, 6, 12, 24];
const sel = val('R') || 0;
const all = rows.filter(r => r.rb !== null);
const far = all.reduce((m, r) => Math.max(m, r.range / NM), 0);
let maxR = sel > 0 ? sel : (RANGES.find(s => far <= s * 0.95) || RANGES[RANGES.length - 1]);
if (sel <= 0 && maxR < 0.5) { maxR = 0.5; }
const colorOf = (r) => r.risk >= 2 ? '#ff6b6b' : (r.source === 'report' ? '#5ad8a6' : '#f0b429');
const pts = all.filter(r => r.range / NM <= maxR * 1.02);
const beyond = all.length - pts.length;
const loc = context.grafana && context.grafana.locationService;
const setRange = (v) => { if (loc && loc.partial) { loc.partial({ 'var-traffic_range': String(v) }, true); } };
const idx = RANGES.indexOf(maxR) >= 0 ? RANGES.indexOf(maxR) : RANGES.findIndex(s => s >= maxR);
const btn = (id, label, right, top, onclick, active) => ({ type: 'group', id: id, right: right, top: top, onclick: onclick,
  cursor: loc ? 'pointer' : 'default', children: [
    { type: 'rect', shape: { width: 26, height: 22, r: 4 },
      style: { fill: active ? (dark ? '#1f6f8b' : '#cfe8f3') : (dark ? '#22252b' : '#eef0f2'), stroke: dark ? '#3a3f47' : '#c9ced4' } },
    { type: 'text', x: 13, y: 11, style: { text: label, fill: C.text, fontSize: label.length > 1 ? 9 : 15, fontWeight: 700,
        align: 'center', verticalAlign: 'middle', textAlign: 'center', textVerticalAlign: 'middle' } }] });
// Top-view hull, bow up: rotated to the contact's heading (heading-up view).
const HULL = 'path://M5 0 C8 4 10 9 10 15 L10 26 C10 28 9 29 7 29 L3 29 C1 29 0 28 0 26 L0 15 C0 9 2 4 5 0 Z';
const wrap = (a) => ((a % 360) + 360) % 360;
const OWN_PIN = 'image://data:image/svg+xml;utf8,' + encodeURIComponent(
  '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 40 52" width="40" height="52">' +
  '<path d="M9 33 L20 50 L31 33 Z" fill="#d32f2f" stroke="#ffffff" stroke-width="2" stroke-linejoin="round"/>' +
  '<circle cx="20" cy="20" r="18" fill="#d32f2f" stroke="#ffffff" stroke-width="2"/>' +
  '<circle cx="20" cy="20" r="12.5" fill="#0b2a49"/>' +
  '<rect x="19.2" y="11" width="1.6" height="5" fill="#ffffff"/>' +
  '<rect x="15" y="15.5" width="9.5" height="5.5" rx="1" fill="#ffffff"/>' +
  '<path d="M10.5 22 L29.5 22 L26.5 27.5 L13.5 27.5 Z" fill="#ffffff"/>' +
  '<path d="M12 30 q2 -1.5 4 0 t4 0 t4 0 t4 0" fill="none" stroke="#ffffff" stroke-width="1.2" stroke-linecap="round"/>' +
  '</svg>');
const scatter = pts.map(r => ({
  value: [r.range / NM, wrap(r.rb)],
  symbolRotate: r.hdg === null ? 0 : -(r.hdg - ownHdg),
  itemStyle: { color: colorOf(r), borderColor: dark ? '#0b0f14' : '#ffffff', borderWidth: 1 },
  label: { show: r.risk >= 1 || !!avoid[r.id], formatter: r.id, color: dark ? '#fff' : '#24292e', fontSize: 11, fontWeight: 600, position: 'top' },
  name: r.id, risk: r.risk, cpa: r.cpa, tcpa: r.tcpa, source: r.source, spd: r.spd }));
// Speed vector: where the contact will be in 6 minutes (spd_kn / 10 nm),
// drawn from the bow along its heading. Kept visible but never longer than
// 40 % of the display range; none for contacts below 0.5 kn.
const vectors = [];
for (const r of pts) {
  if (r.hdg === null || r.spd === null || r.spd < 0.5) { continue; }
  const r0 = r.range / NM, a0 = r.rb * Math.PI / 180;
  const len = Math.min(Math.max(r.spd / 10, maxR * 0.06), maxR * 0.4);
  const d = (r.hdg - ownHdg) * Math.PI / 180;
  const x = r0 * Math.sin(a0) + len * Math.sin(d), y = r0 * Math.cos(a0) + len * Math.cos(d);
  vectors.push({ type: 'line', coordinateSystem: 'polar', silent: true, z: 2, showSymbol: false,
    lineStyle: { color: colorOf(r), width: 1.5 },
    data: [[r0, wrap(r.rb)], [Math.hypot(x, y), wrap(Math.atan2(x, y) * 180 / Math.PI)]] });
}
const worst = all.slice().sort((a, b) => (b.risk - a.risk) || ((a.cpa ?? 1e9) - (b.cpa ?? 1e9)))[0];
const series = [{
  type: 'scatter', coordinateSystem: 'polar', symbol: HULL, symbolSize: [9, 24], z: 3, data: scatter,
  tooltip: { formatter: (p) => p.data.name + '<br>' + p.data.source + ' · risk ' + ['clear','MEDIUM','HIGH'][p.data.risk] +
    (p.data.cpa != null ? '<br>CPA ' + Math.round(p.data.cpa) + ' m' : '') +
    (p.data.tcpa != null && p.data.tcpa >= 0 ? ' · TCPA ' + Math.round(p.data.tcpa) + ' s' : '') +
    (p.data.spd != null ? '<br>' + p.data.spd.toFixed(1) + ' kn' : '') }
}, {
  type: 'scatter', coordinateSystem: 'polar', symbol: 'circle', symbolSize: 34, z: 2, silent: true,
  data: pts.filter(r => avoid[r.id]).map(r => ({ value: [r.range / NM, wrap(r.rb)],
    itemStyle: { color: 'rgba(0,0,0,0)', borderColor: avoid[r.id] > 1 ? '#ff4d4f' : '#ff9830', borderWidth: 2.5 } }))
}, ...vectors, {
  // Own ship: red map pin with a white vessel glyph, tip on the centre.
  type: 'scatter', coordinateSystem: 'polar', symbol: OWN_PIN, symbolSize: [28, 36], symbolOffset: [0, '-50%'],
  data: [{ value: [0, 0] }], silent: true, z: 5
}];
if (worst && worst.risk >= 1 && worst.range / NM <= maxR * 1.02) {
  series.push({ type: 'line', coordinateSystem: 'polar', silent: true, z: 4, showSymbol: false,
    lineStyle: { color: '#ff6b6b', type: 'dashed', width: 1.5 },
    data: [[0, 0], [worst.range / NM, ((worst.rb % 360) + 360) % 360]] });
}
const fmtT = (t) => t == null || t < 0 ? '—' : Math.floor(t / 60) + ':' + ('0' + Math.round(t % 60)).slice(-2);
return {
  backgroundColor: 'transparent',
  polar: { center: ['50%', '49%'], radius: '74%' },
  angleAxis: { type: 'value', min: 0, max: 360, startAngle: 90, clockwise: true, splitNumber: 12,
    axisLine: { lineStyle: { color: C.tick } }, axisTick: { show: false },
    axisLabel: { color: C.label, fontSize: 10, formatter: (v) => v === 0 ? 'N' : ('00' + v).slice(-3) },
    splitLine: { lineStyle: { color: dark ? 'rgba(124,156,176,0.18)' : 'rgba(36,41,46,0.15)' } } },
  radiusAxis: { min: 0, max: maxR, splitNumber: 4, axisLine: { show: false }, axisTick: { show: false },
    axisLabel: { color: C.label, fontSize: 9, formatter: (v) => v > 0 ? v + ' nm' : '' },
    splitLine: { lineStyle: { color: dark ? 'rgba(124,156,176,0.35)' : 'rgba(36,41,46,0.3)', type: 'dashed' } } },
  tooltip: { trigger: 'item' },
  graphic: [
    { type: 'text', left: 8, top: 4, style: { text: 'HEADING UP · range ' + maxR + ' nm' + (sel > 0 ? '' : ' (auto)') +
        (beyond > 0 ? '   +' + beyond + ' beyond' : ''), fill: C.sub, fontSize: 10, fontWeight: 600 } },
    btn('zoom-in', '+', 8, 4, () => setRange(RANGES[Math.max(0, idx - 1)]), false),
    btn('zoom-out', '\u2212', 8, 30, () => setRange(RANGES[Math.min(RANGES.length - 1, idx + 1)]), false),
    btn('zoom-auto', 'A', 8, 56, () => setRange(0), sel <= 0),
    { type: 'text', left: 8, bottom: 6, style: { text: '{g|●} AIS  {a|●} radar  {r|●} HIGH  {o|○} avoiding  — 6 min', fill: C.label, fontSize: 10,
        rich: { g: { fill: '#5ad8a6', fontSize: 12 }, a: { fill: '#f0b429', fontSize: 12 }, r: { fill: '#ff6b6b', fontSize: 12 },
          o: { fill: '#ff9830', fontSize: 13, fontWeight: 700 } } } },
    { type: 'text', right: 8, bottom: 6, style: { text: worst && worst.risk >= 1 ? worst.id + '  CPA ' + Math.round(worst.cpa) + ' m · TCPA ' + fmtT(worst.tcpa) : 'no risk contacts',
        fill: worst && worst.risk >= 2 ? '#ff6b6b' : C.text, fontSize: 11, fontWeight: 600 } }
  ],
  series: series
};
"""


risk_mappings = [{"type": "value", "options": {
    "0": {"text": "CLEAR", "color": TILE_OK, "index": 0},
    "1": {"text": "MEDIUM", "color": "orange", "index": 1},
    "2": {"text": "HIGH", "color": "red", "index": 2}}}]
encounter_mappings = state_map([("head-on", "red"), ("crossing", "orange"),
                                ("overtaking", "yellow"), ("-", "#3d434b")])


def kv_tile(title, query, x, y, w, h, **kw):
    """Key/value readout tile for the contact and MOOS action cards; reads
    "none" instead of Grafana's "No data" while there is no contact."""
    kw.setdefault("no_value", "none")
    return stat(title, query, x, y, w, h, **kw)


DASHBOARDS.append(dashboard("usv-traffic", "Traffic", [
    # A — risk and counts (8 x 3x3)
    stat("Collision Risk", flux("traffic", "risk_level", last=True), 0, 0, 3, 2,
         mappings=risk_mappings, text_mode="value", color_mode="background"),
    stat("Avoidance", flux("traffic", "avoidance_active", last=True), 3, 0, 3, 2,
         mappings=[{"type": "value", "options": {
             "0": {"text": "IDLE", "color": "#3d434b", "index": 0},
             "1": {"text": "ACTIVE", "color": "orange", "index": 1},
             "2": {"text": "IN EXTREMIS", "color": "red", "index": 2}}}],
         text_mode="value", color_mode="background"),
    stat("Contacts", flux("traffic", "contacts", last=True), 6, 0, 3, 2, decimals=0),
    stat("Reports / AIS", flux("traffic", "reports", last=True), 9, 0, 3, 2, decimals=0),
    stat("Radar Only", flux("traffic", "radar_only", last=True), 12, 0, 3, 2, decimals=0),
    stat("High Risk", flux("traffic", "high_risk", last=True), 15, 0, 3, 2, decimals=0,
         thresholds=[(None, OK), (1, "red")]),
    stat("Closest Range", flux("traffic", "closest_range_m", last=True, nm=True),
         18, 0, 3, 2, unit=UNIT_NM, decimals=2, no_value="none",
         mappings=[{"type": "range", "options": {"from": -1, "to": 0,
                    "result": {"text": "none", "color": "#3d434b", "index": 0}}}],
         thresholds=neutral_base([(None, "red"), (0.11, "yellow"), (0.27, OK)])),
    stat("Min CPA", flux("traffic", "min_cpa_m", last=True), 21, 0, 3, 2,
         unit=UNIT_M, decimals=0, no_value="none",
         mappings=[{"type": "range", "options": {"from": -1, "to": -0.5,
                    "result": {"text": "none", "color": "#3d434b", "index": 0}}}],
         thresholds=neutral_base([(None, "red"), (500, "yellow"), (1000, OK)])),
    # B — traffic view | highest-risk contact | MOOS action
    (instrument("Traffic View",
                [target(contacts_query()),
                 target(flux("nav", "heading", last=True), ref="B"),
                 target(TRAFFIC_RANGE_QUERY, ref="R"),
                 target(COLAV_RING_QUERY, ref="V")],
                0, 2, 10, 10,
                traffic_js(), {"kind": "traffic"})
     if ANALOG else
     panel("table", "Contacts", [target(contacts_query())], 0, 2, 10, 10,
           options={"showHeader": True})),
    stat("Highest-Risk Contact", worst_contact_query("contact"), 10, 2, 7, 2,
         string_value=True, text_mode="value", no_value="no contacts"),
    kv_tile("CPA", worst_contact_query("cpa_m"), 10, 4, 4, 2, unit=UNIT_M, decimals=0,
            thresholds=neutral_base([(None, "red"), (500, "yellow"), (1000, OK)])),
    kv_tile("TCPA", worst_contact_query("tcpa_s"), 14, 4, 3, 2, unit="dtdurations",
            decimals=0, thresholds=neutral_base([(None, "red"), (120, "yellow"), (300, OK)]),
            mappings=[{"type": "value", "options": {
                "-1": {"text": "OPENING", "color": TILE_OK, "index": 0}}}]),
    kv_tile("Range", worst_contact_query("range_m"), 10, 6, 4, 2, unit=UNIT_M, decimals=0),
    kv_tile("Bearing", worst_contact_query("bearing"), 14, 6, 3, 2, unit="degree", decimals=0),
    kv_tile("Rel. Bearing", worst_contact_query("rel_bearing"), 10, 8, 4, 2,
            unit="degree", decimals=0),
    kv_tile("Rel. Speed", worst_contact_query("rel_speed_kn"), 14, 8, 3, 2,
            unit="velocityknot", decimals=1),
    stat("Type", worst_contact_query("type"), 10, 10, 4, 2, string_value=True,
         text_mode="value", no_value="none"),
    stat("Encounter", worst_contact_query("encounter"), 14, 10, 3, 2, string_value=True,
         text_mode="value", mappings=encounter_mappings, color_mode="background",
         no_value="none", thresholds=[(None, "#3d434b")]),
    kv_tile("Planned Course", flux("traffic", "planned_course", last=True), 17, 2, 4, 2,
            unit="degree", decimals=0, no_value="no route",
            mappings=[{"type": "range", "options": {"from": -1, "to": -0.5,
                       "result": {"text": "no route", "color": "#3d434b", "index": 0}}}]),
    kv_tile("Cmd Course", flux("autopilot", "desired_heading", last=True), 21, 2, 3, 2,
            unit="degree", decimals=0),
    kv_tile("Actual Heading", flux("nav", "heading", last=True), 17, 4, 4, 2,
            unit="degree", decimals=0),
    kv_tile("Deviation", flux("traffic", "course_deviation", last=True), 21, 4, 3, 2,
            unit="degree", decimals=0, no_value="no route",
            mappings=[{"type": "range", "options": {"from": -1000, "to": -500,
                       "result": {"text": "no route", "color": "#3d434b", "index": 0}}}],
            thresholds=[(None, OK), (10, "yellow"), (30, "orange")]),
    kv_tile("Speed Command", flux("autopilot", "desired_speed", last=True), 17, 6, 4, 2,
            unit="velocityknot", decimals=1),
    kv_tile("Actual SOG", flux("nav", "sog", last=True), 21, 6, 3, 2,
            unit="velocityknot", decimals=1),
    stat("Avoidance Behavior", last_str("mission_state", "active_behavior"), 17, 8, 7, 2,
         text_mode="value", no_value="none"),
    stat("Reason", last_str("traffic", "reason"), 17, 10, 7, 2, text_mode="value",
         string_value=True),
    # C — contact table, helm avoidance (COLAV_ALERTS) and CPA / TCPA trend
    panel("table", "Contacts — riskiest first", [target(contacts_query(limit=12))],
          0, 12, 10, 5, options={"showHeader": True}, no_value="no contacts",
          mappings=risk_mappings,
          overrides=[{"matcher": {"id": "byName", "options": "range_m"},
                      "properties": [{"id": "unit", "value": UNIT_M}, {"id": "decimals", "value": 0},
                                     {"id": "displayName", "value": "Range"}]},
                     {"matcher": {"id": "byName", "options": "cpa_m"},
                      "properties": [{"id": "unit", "value": UNIT_M}, {"id": "decimals", "value": 0},
                                     {"id": "displayName", "value": "CPA"}]},
                     {"matcher": {"id": "byName", "options": "tcpa_s"},
                      "properties": [{"id": "unit", "value": "s"}, {"id": "decimals", "value": 0},
                                     {"id": "displayName", "value": "TCPA"},
                                     # -1 = opening (past its closest approach), as on the TCPA tile
                                     {"id": "mappings", "value": [{"type": "value", "options": {
                                         "-1": {"text": "opening", "index": 0}}}]}]},
                     {"matcher": {"id": "byName", "options": "bearing"},
                      "properties": [{"id": "unit", "value": "degree"}, {"id": "decimals", "value": 0},
                                     {"id": "displayName", "value": "Bearing"}]},
                     {"matcher": {"id": "byName", "options": "spd_kn"},
                      "properties": [{"id": "unit", "value": "velocityknot"}, {"id": "decimals", "value": 1},
                                     {"id": "displayName", "value": "Speed"}]},
                     {"matcher": {"id": "byName", "options": "risk"},
                      "properties": [{"id": "displayName", "value": "Risk"},
                                     {"id": "custom.cellOptions", "value": {"type": "color-text"}}]},
                     {"matcher": {"id": "byName", "options": "contact"},
                      "properties": [{"id": "displayName", "value": "Contact"}]},
                     {"matcher": {"id": "byName", "options": "source"},
                      "properties": [{"id": "displayName", "value": "Source"}]},
                     {"matcher": {"id": "byName", "options": "type"},
                      "properties": [{"id": "displayName", "value": "Type"}]},
                     {"matcher": {"id": "byName", "options": "encounter"},
                      "properties": [{"id": "displayName", "value": "Encounter"}]},
                     {"matcher": {"id": "byRegexp", "options": "rel_bearing|rel_speed_kn|hdg"},
                      "properties": [{"id": "custom.hidden", "value": True}]}]),
    panel("table", "Helm Avoiding (COLREGS)", [target(colav_query())],
          10, 12, 8, 5, options={"showHeader": True}, no_value="helm not avoiding",
          overrides=[{"matcher": {"id": "byName", "options": "contact"},
                      "properties": [{"id": "displayName", "value": "Contact"},
                                     {"id": "custom.width", "value": 105}]},
                     {"matcher": {"id": "byName", "options": "situation"},
                      "properties": [{"id": "displayName", "value": "Situation"}]},
                     {"matcher": {"id": "byName", "options": "cpa_m"},
                      "properties": [{"id": "unit", "value": UNIT_M}, {"id": "decimals", "value": 0},
                                     {"id": "displayName", "value": "CPA"},
                                     {"id": "custom.width", "value": 62},
                                     {"id": "mappings", "value": [{"type": "range", "options": {
                                         "from": -1.5, "to": -0.5,
                                         "result": {"text": "?", "index": 0}}}]}]},
                     {"matcher": {"id": "byName", "options": "tcpa_s"},
                      "properties": [{"id": "unit", "value": "s"}, {"id": "decimals", "value": 0},
                                     {"id": "displayName", "value": "TCPA"},
                                     {"id": "custom.width", "value": 66},
                                     {"id": "mappings", "value": [
                                         {"type": "range", "options": {"from": -1.5, "to": -0.5,
                                          "result": {"text": "?", "index": 0}}},
                                         {"type": "value", "options": {
                                          "0": {"text": "opening", "index": 1}}}]}]},
                     {"matcher": {"id": "byName", "options": "state"},
                      "properties": [{"id": "displayName", "value": "State"},
                                     {"id": "custom.width", "value": 100},
                                     {"id": "mappings", "value": colav_state_mappings},
                                     {"id": "custom.cellOptions", "value": {"type": "color-text"}}]},
                     {"matcher": {"id": "byRegexp", "options": "range_m|for_s"},
                      "properties": [{"id": "custom.hidden", "value": True}]}]),
    ts("Closest CPA / TCPA",
       [target(flux("traffic", "min_cpa_m", extra=NO_SENTINEL), "CPA m"),
        target(flux("traffic", "min_tcpa_s", extra=NO_SENTINEL), "TCPA s", ref="B")],
       18, 12, 6, 5, unit=UNIT_M,
       # Flux names the series after the field (min_cpa_m / min_tcpa_s) and
       # the palette gave both the same blue: name and colour them here.
       overrides=[{"matcher": {"id": "byFrameRefID", "options": "A"},
                   "properties": [{"id": "displayName", "value": "CPA"},
                                  {"id": "color", "value": {"mode": "fixed", "fixedColor": "#5794f2"}}]},
                  {"matcher": {"id": "byFrameRefID", "options": "B"},
                   "properties": [{"id": "unit", "value": "s"},
                                  {"id": "custom.axisPlacement", "value": "right"},
                                  {"id": "displayName", "value": "TCPA"},
                                  {"id": "color", "value": {"mode": "fixed", "fixedColor": "#ff9830"}}]}]),
], ["traffic"]))

# Helm Avoiding (v1.2.24) takes its own id and the shared counter is put
# back, so adding it does not renumber the panels of the later dashboards.
for _p in DASHBOARDS[-1]["panels"]:
    if _p.get("title") == "Helm Avoiding (COLREGS)":
        _id[0] -= 1
        _colav_old = _p["id"]
        _p["id"] = 9601
        for _q in DASHBOARDS[-1]["panels"]:
            if _q["id"] > _colav_old and _q is not _p:
                _q["id"] -= 1
        _p["transformations"] = [{"id": "organize", "options": {
            "excludeByName": {"range_m": True, "for_s": True},
            "indexByName": {"contact": 0, "situation": 1, "cpa_m": 2, "tcpa_s": 3,
                            "state": 4}}}]

# Contacts table: the join emits columns alphabetically; fix the reading
# order and drop the working columns.
_ct = next(p for p in DASHBOARDS[-1]["panels"] if p["title"].startswith("Contacts —"))
_ct["transformations"] = [{"id": "organize", "options": {
    "excludeByName": {"rel_bearing": True, "rel_speed_kn": True, "hdg": True,
                      "type": True, "spd_kn": True, "source": True, "bearing": True},
    "indexByName": {"contact": 0, "range_m": 1, "cpa_m": 2, "tcpa_s": 3,
                    "encounter": 4, "risk": 5}}}]
# Six columns across w10 (about 620 px at 1080p) without a scrollbar: the
# contact name gets the room, the numbers stay compact. Source and bearing
# are on the Traffic View and the highest-risk card.
for _o in _ct["fieldConfig"]["overrides"]:
    _n = _o["matcher"]["options"]
    _w = {"contact": 130, "range_m": 80, "cpa_m": 75, "tcpa_s": 70, "risk": 75}.get(_n)
    if _w:
        _o["properties"].append({"id": "custom.width", "value": _w})

# --------------------------------------------------------------- FR-7 Camera
# Source: frigate_bridge (Frigate NVR API). One `camera` series per camera
# (tag cam) plus `camera_summary` counts, so the top row shows the fleet of
# cameras as counts and the table shows each camera on its own row; a stat
# tile over several cameras would draw them all on top of each other.
def camera_table_query():
    return ('from(bucket: "telemetry")\n'
            '  |> range(start: -30s)\n'
            '  |> filter(fn: (r) => r._measurement == "camera")\n'
            '  |> filter(fn: (r) => r.vessel == "${vessel}")\n'
            '  |> filter(fn: (r) => r._field == "connected" or r._field == "stream_up"'
            ' or r._field == "fps" or r._field == "recording")\n'
            '  |> last()\n'
            '  |> drop(columns: ["_start", "_stop", "_measurement", "vessel", "_time"])\n'
            '  |> group()\n'
            '  |> pivot(rowKey: ["cam"], columnKey: ["_field"], valueColumn: "_value")\n'
            '  |> sort(columns: ["cam"])\n')


def _cam_col(name, title, mappings, width=130):
    return {"matcher": {"id": "byName", "options": name},
            "properties": [{"id": "displayName", "value": title},
                           {"id": "custom.width", "value": width},
                           {"id": "custom.align", "value": "center"},
                           {"id": "mappings", "value": mappings},
                           {"id": "custom.cellOptions",
                            "value": {"type": "color-background", "mode": "basic"}}]}


_cam = panel("table", "Cameras", [target(camera_table_query())], 0, 3, 12, 9,
             options={"showHeader": True, "cellHeight": "sm"},
             no_value="no camera enabled in Frigate",
             overrides=[
                 {"matcher": {"id": "byName", "options": "cam"},
                  "properties": [{"id": "displayName", "value": "Camera"}]},
                 _cam_col("connected", "Network", updown_map("NO ANSWER", "CONNECTED")),
                 _cam_col("stream_up", "Stream", updown_map("NO VIDEO", "LIVE")),
                 _cam_col("recording", "Recording", [{"type": "value", "options": {
                     "0": {"text": "off", "color": NEUTRAL, "index": 0},
                     "1": {"text": "RECORDING", "color": TILE_OK, "index": 1}}}]),
                 {"matcher": {"id": "byName", "options": "fps"},
                  "properties": [{"id": "displayName", "value": "Frame rate"},
                                 {"id": "unit", "value": "fps"},
                                 {"id": "decimals", "value": 1},
                                 {"id": "custom.width", "value": 110}]}])
_cam["transformations"] = [{"id": "organize", "options": {"indexByName": {
    "cam": 0, "connected": 1, "stream_up": 2, "fps": 3, "recording": 4}}}]
_cam["description"] = ("One row per camera enabled in Frigate. A camera that is "
                       "configured but disabled there (not yet powered) is not listed.")

DASHBOARDS.append(dashboard("usv-camera", "Cameras", [
    stat("Frigate NVR", flux("camera_summary", "frigate_up", last=True), 0, 0, 6, 3,
         mappings=updown_map("DOWN", "UP"), text_mode="value",
         color_mode="background"),
    stat("Cameras Enabled", flux("camera_summary", "enabled", last=True), 6, 0, 6, 3,
         decimals=0, thresholds=[(None, OK)]),
    stat("Streaming", flux("camera_summary", "streaming", last=True), 12, 0, 6, 3,
         decimals=0, thresholds=[(None, OK)]),
    stat("Network Connected", flux("camera_summary", "connected", last=True), 18, 0, 6, 3,
         decimals=0, thresholds=[(None, OK)]),
    _cam,
    ts("Frame Rate", [target(flux("camera", "fps"), "fps")], 12, 3, 12, 9,
       unit="fps"),
    ts("Bitrate", [target(flux("camera", "bitrate_kbps"), "kbps")],
       0, 12, 8, 5, unit="Kbits"),
    ts("PTZ Position",
       [target(flux("camera", ["ptz_pan", "ptz_tilt"]))], 8, 12, 8, 5,
       unit="degree"),
    ts("Camera Temperature", [target(flux("camera", "temp_c"), "temp")],
       16, 12, 8, 5, unit="celsius"),
], ["camera"]))

# ----------------------------------------------------------- FR-8 Communication
# Telegraf tags that pick ONE series per host metric; the rest (per-core
# cpu, loop/device-mapper block devices, the loopback interface, non-root
# mounts) would otherwise stack as extra series or a "No data" split.
TELE_SELECT = {
    "cpu": '  |> filter(fn: (r) => r.cpu == "cpu-total")\n',
    # CPU sensors only: acpitz reports -273 C on some boards (OnLogic K800)
    # and chipset/NVMe sensors are not the CPU.
    "temp": ('  |> filter(fn: (r) => r.sensor !~ /^(acpitz|nvme|pch|iwlwifi)/)\n'
             '  |> filter(fn: (r) => r._value > -50.0 and r._value < 150.0)\n'),
    "disk": '  |> filter(fn: (r) => r.path == "/")\n',
    "diskio": '  |> filter(fn: (r) => r.name !~ /^(loop|dm-|sr)/)\n',
    # Physical links only: docker/bridge/veth/tunnel interfaces would
    # otherwise fill the legend (12+ series on the OBC).
    "net": '  |> filter(fn: (r) => r.interface !~ /^(lo|docker|br-|veth|virbr|tun|tap|wg|tailscale|sdwan|twingate|slcan|can|vcan)/)\n',
}


def tele(measurement, field, fn="mean", extra="", rate=False):
    """Telegraf-produced metrics are tagged by host, not vessel. rate=True
    turns a monotonic counter (bytes read/sent) into a per-second rate:
    derivative() is a separate step, not an aggregateWindow fn."""
    q = (f'from(bucket: "telemetry")\n'
         f'  |> range(start: v.timeRangeStart, stop: v.timeRangeStop)\n'
         f'  |> filter(fn: (r) => r._measurement == "{measurement}" '
         f'and r._field == "{field}")\n'
         + TELE_SELECT.get(measurement, "")
         + (extra + "\n" if extra else ""))
    if rate:
        q += ('  |> aggregateWindow(every: v.windowPeriod, fn: last, createEmpty: false)\n'
              '  |> derivative(unit: 1s, nonNegative: true)\n')
    else:
        q += f'  |> aggregateWindow(every: v.windowPeriod, fn: {fn}, createEmpty: false)\n'
    return q + CLEAN_LABELS


LAST = "last()"
AGG = "aggregateWindow(every: v.windowPeriod, fn: mean, createEmpty: false)"


# A Telegraf series can split over the time range (net_response gains a
# second series tagged result=connection_failed while the probed service
# is down; temp has one per sensor). A stat tile must show the single
# most recent sample, never one value per series side by side.
LATEST_ACROSS_SERIES = ('  |> group()\n'
                        '  |> sort(columns: ["_time"])\n'
                        '  |> last()\n')


def tele_last(measurement, field, extra="", reduce="latest"):
    """Latest Telegraf sample for a stat tile. reduce='latest' takes the
    most recent sample across all series; 'max' the highest of each
    series' latest sample (hottest CPU sensor)."""
    q = tele(measurement, field, extra=extra).replace(AGG, LAST)
    collapse = ('  |> group()\n  |> max()\n' if reduce == "max"
                else '  |> group()\n  |> min()\n' if reduce == "min"
                else LATEST_ACROSS_SERIES)
    return q.replace(CLEAN_LABELS, collapse + CLEAN_LABELS)



# Link status comes from Telegraf host probes (telegraf.conf), not from the
# bench simulator: Internet = ICMP to 8.8.8.8, VPN = tailscaled.service,
# Starlink = ICMP to the dish router (input enabled on vessels only; the
# tile reads N/A where it is not fitted).
PING_INTERNET = '  |> filter(fn: (r) => r.url == "8.8.8.8")\n'
PING_STARLINK = '  |> filter(fn: (r) => r.url == "192.168.100.1")\n'
# Any VPN daemon counts: Twingate (used on usv-722: client or connector),
# Tailscale, OpenVPN or WireGuard.
# Each unit's latest active_code, then the lowest: 0 (active) if any is up.
UNIT_SERVICE = ('  |> filter(fn: (r) => r.name =~ '
                '/^(twingate.*|tailscaled|openvpn.*|wg-quick@.*)\\.service$/)\n')
BYTES_TO_BITS = '  |> map(fn: (r) => ({r with _value: r._value * 8.0}))\n'


def busiest_net(field, n=3):
    """Per-interface bit rate for the n busiest links over the time range
    only. The OBC has a dozen NICs (enp5s0 to enp16s0), most idle; plotting
    all of them made the legend scroll."""
    return ('data = ' + tele("net", field, rate=True).lstrip() + BYTES_TO_BITS
            + f'busiest = data\n  |> mean()\n  |> group()\n  |> top(n: {n})\n'
            '  |> findColumn(fn: (key) => true, column: "interface")\n'
            'data\n  |> filter(fn: (r) => contains(value: r.interface, set: busiest))\n')


def probe_tile(title, query, x, down="DOWN", up="UP", no_value=None):
    return stat(title, query, x, 0, 4, 4, text_mode="value",
                color_mode="background", no_value=no_value,
                mappings=[{"type": "value", "options": {
                    "0": {"text": up, "color": TILE_OK, "index": 0}}},
                    {"type": "range", "options": {
                        "from": 1, "to": 99,
                        "result": {"text": down, "color": "red", "index": 1}}}])


DASHBOARDS.append(dashboard("usv-comms", "Comms", [
    probe_tile("Internet", tele_last("ping", "result_code", extra=PING_INTERNET), 0),
    probe_tile("Starlink", tele_last("ping", "result_code", extra=PING_STARLINK), 4,
               no_value="N/A"),
    probe_tile("VPN (Twingate)",
               tele_last("systemd_units", "active_code", extra=UNIT_SERVICE,
                         reduce="min"), 8),
    stat("Signal K WebSocket", flux("comms", "signalk_ws_up", last=True),
         12, 0, 4, 4, mappings=updown_map(), text_mode="value",
         color_mode="background"),
    stat("Latency", tele_last("ping", "average_response_ms", extra=PING_INTERNET),
         16, 0, 4, 4, unit="ms", decimals=0,
         thresholds=[(None, OK), (150, "yellow"), (400, "red")],
         graph_mode="area"),
    stat("Packet Loss", tele_last("ping", "percent_packet_loss", extra=PING_INTERNET),
         20, 0, 4, 4, unit="percent", decimals=1,
         thresholds=[(None, OK), (2, "yellow"), (10, "red")],
         graph_mode="area"),
    ts("Latency", [target(tele("ping", "average_response_ms", extra=PING_INTERNET),
                          "latency")],
       0, 4, 12, 7, unit="ms"),
    ts("Packet Loss", [target(tele("ping", "percent_packet_loss", extra=PING_INTERNET),
                              "loss")],
       12, 4, 12, 7, unit="percent", min_=0),
    ts("Bandwidth (3 busiest links)",
       [target(busiest_net("bytes_recv"), "down"),
        target(busiest_net("bytes_sent"), "up", ref="B")],
       0, 11, 12, 7, unit="bps"),
    ts("Telemetry Message Rates",
       [target(flux("comms", ["moos_msg_rate", "signalk_msg_rate"]))],
       12, 11, 12, 7),
], ["comms"]))

# ---------------------------------------------------------- FR-9 System Health


def service_tile(title, service, x):
    return stat(title, tele_last("net_response", "result_code",
                                 extra=f'  |> filter(fn: (r) => r.service == "{service}")'),
                x, 0, 3, 3, text_mode="value", color_mode="background",
                mappings=[{"type": "value", "options": {
                    "0": {"text": "UP", "color": TILE_OK, "index": 0}}},
                    {"type": "range", "options": {
                        "from": 1, "to": 99,
                        "result": {"text": "DOWN", "color": "red", "index": 1}}}])


# Single screen: 8 equal tiles (CPU / GPU / RAM / Disk gauges, CPU temp,
# InfluxDB / Signal K / MOOS reachability), six trend charts, service table. GPU utilisation comes
# from Telegraf's "gpu" measurement (telegraf.conf: intel_gpu_top probe for
# Intel Arc/Xe, nvidia_smi with name_override for NVIDIA); the tile reads
# "No data" on hosts without a supported GPU.
DISK_LABEL = ('  |> map(fn: (r) => ({r with _field: r.name}))\n'
              '  |> keep(columns: ["_time", "_value", "_field"])\n')
HOTTEST = ('  |> group()\n'
           '  |> aggregateWindow(every: v.windowPeriod, fn: max, createEmpty: false)\n')
SERVICES_QUERY = ('import "strings"\n'
                  'from(bucket: "telemetry")\n'
                  '  |> range(start: -2m)\n'
                  '  |> filter(fn: (r) => r._measurement == "systemd_units" '
                  'and r._field == "active_code")\n'
                  '  |> last()\n'
                  '  |> map(fn: (r) => ({r with name: strings.trimSuffix(v: r.name, suffix: ".service")}))\n'
                  '  |> group(columns: ["name"])\n'
                  '  |> keep(columns: ["name", "_value", "_time"])\n')


DASHBOARDS.append(dashboard("usv-system", "System", [
    gauge("CPU", tele_last("cpu", "usage_user"), 0, 0, 3, 3,
          thresholds=[(None, OK), (70, "yellow"), (90, "red")]),
    gauge("GPU", tele_last("gpu", "utilization_gpu"), 3, 0, 3, 3,
          thresholds=[(None, OK), (70, "yellow"), (90, "red")]),
    gauge("RAM", tele_last("mem", "used_percent"), 6, 0, 3, 3,
          thresholds=[(None, OK), (75, "yellow"), (90, "red")]),
    gauge("Disk", tele_last("disk", "used_percent"), 9, 0, 3, 3,
          thresholds=[(None, OK), (75, "yellow"), (90, "red")]),
    stat("CPU Temp", tele_last("temp", "temp", reduce="max"), 12, 0, 3, 3, unit="celsius",
         decimals=0, thresholds=[(None, OK), (70, "yellow"), (85, "red")]),
    service_tile("InfluxDB", "influxdb", 15),
    service_tile("Signal K", "signalk", 18),
    service_tile("MOOS", "moos", 21),
    ts("CPU", [target(tele("cpu", "usage_user"), "user"),
               target(tele("cpu", "usage_system"), "system", ref="B")],
       0, 3, 8, 5, unit="percent", min_=0, max_=100),
    ts("GPU", [target(tele("gpu", "utilization_gpu"), "util %"),
               target(tele("gpu", "temperature_gpu"), "temp °C", ref="B")],
       8, 3, 8, 5, unit="percent", min_=0, max_=100,
       overrides=[{"matcher": {"id": "byFrameRefID", "options": "B"},
                   "properties": [{"id": "unit", "value": "celsius"},
                                  {"id": "custom.axisPlacement",
                                   "value": "right"}]}]),
    ts("Memory", [target(tele("mem", "used_percent"), "used %")],
       16, 3, 8, 5, unit="percent", min_=0, max_=100),
    ts("Disk I/O", [target(tele("diskio", "read_bytes", rate=True)
                           + DISK_LABEL, "read"),
                    target(tele("diskio", "write_bytes", rate=True)
                           + DISK_LABEL, "write", ref="B")],
       0, 8, 8, 6, unit="Bps"),
    ts("Load Average", [target(tele("system", "load1"), "1 min"),
                        target(tele("system", "load5"), "5 min", ref="B"),
                        target(tele("system", "load15"), "15 min", ref="C")],
       8, 8, 8, 6),
    # One line: the hottest CPU sensor (24 per-core series filled the legend).
    ts("CPU Temp", [target(tele("temp", "temp", fn="max") + HOTTEST, "hottest core")],
       16, 8, 8, 6, unit="celsius"),
    # One tile per watched systemd unit (Telegraf systemd_units): the table
    # showed two rows at this height.
    stat("Services (systemd)", SERVICES_QUERY, 0, 14, 24, 3,
         text_mode="value_and_name", color_mode="background",
         mappings=[{"type": "value", "options": {
             "0": {"text": "UP", "color": TILE_OK, "index": 0}}},
             {"type": "range", "options": {
                 "from": 1, "to": 99,
                 "result": {"text": "DOWN", "color": "red", "index": 1}}}]),
], ["system"]))

# -------------------------------------------------------- FR-10 Mission Replay
DASHBOARDS.append(dashboard("usv-replay", "Replay", [
    # How to replay: set the time range (top right) to the mission window
    # and scrub with the shared crosshair; data older than the raw retention
    # (30 d) comes from the 1-minute bucket; the Mission events annotations
    # mark waypoint hits and state changes (also in docs/user-guide.md).
    panel("geomap", "GPS Track",
          [target(track_query("10s", archive=True)), target(position_query(), ref="B"),
           target(geometry_query("route"), ref="C"),
           target(geometry_query("region"), ref="D"),
           target(vector_query(), ref="E")],
          0, 0, 14, 9,
          options={"view": {"id": "fit", "zoom": 15},
                   "basemap": {"type": "osm-standard", "name": "OSM"},
                   "layers": overlay_layers()}),
    panel("state-timeline", "Mission State",
          [target(flux("mission_state", "state", every="30s", fn="last"))],
          14, 0, 10, 3, mappings=mission_state_mappings,
          custom={"fillOpacity": 70, "lineWidth": 0},
          overrides=[{"matcher": {"id": "byRegexp", "options": ".*"},
                      "properties": [{"id": "displayName", "value": "state"}]}],
          # No legend: at 3 rows it pushed the bar out of the panel.
          options={"showValue": "auto", "legend": {"showLegend": False}}),
    panel("table", "Mission Events",
          [target('from(bucket: "events")\n'
                  '  |> range(start: v.timeRangeStart, stop: v.timeRangeStop)\n'
                  '  |> filter(fn: (r) => r._measurement == "events" and r._field == "message")\n'
                  '  |> filter(fn: (r) => r.vessel == "${vessel}")\n'
                  '  |> group()\n'
                  '  |> keep(columns: ["_time", "_value", "type", "severity"])\n'
                  '  |> rename(columns: {_time: "Time", _value: "Event", type: "Type", severity: "Severity"})\n'
                  '  |> sort(columns: ["Time"], desc: true)\n')],
          12, 13, 12, 4, overrides=EVENTS_TABLE_OVERRIDES,
          options={"showHeader": True, "cellHeight": "sm",
                   "sortBy": [{"displayName": "Time", "desc": True}]}),
    ts("Heading History", [target(flux("nav", "heading", archive=True), "heading")],
       0, 9, 8, 4, unit="degree", min_=0, max_=360),
    ts("Speed History", [target(flux("nav", "sog", archive=True), "SOG")],
       8, 9, 8, 4, unit="velocityknot"),
    ts("Actuator Commands",
       [target(flux("autopilot", "rudder_cmd", archive=True), "rudder cmd %"),
        target(flux("autopilot", "throttle_cmd", archive=True), "throttle %", ref="B")],
       16, 9, 8, 4, unit="percent",
       overrides=[{"matcher": {"id": "byFrameRefID", "options": "B"},
                   "properties": [{"id": "unit", "value": "percent"},
                                  {"id": "custom.axisPlacement",
                                   "value": "right"}]}]),
    ts("Cross-Track Error", [target(flux("nav", "cross_track_error", archive=True), "XTE")],
       0, 13, 6, 4, unit=UNIT_M),
    panel("state-timeline", "Autonomy Flags",
          [target(flux("autonomy",
                       ["waypoint", "pause", "stationing", "returning",
                        "armed", "comms_lost", "manual_override"],
                       every="30s", fn="last"))],
          14, 3, 10, 6, mappings=flag_timeline_mappings,
          custom={"fillOpacity": 70, "lineWidth": 0},
          options={"showValue": "never", "legend": {"showLegend": False},
                   "tooltip": {"mode": "single"}}),
    ts("Distance to Next Waypoint",
       [target(flux("mission", "dist_to_waypoint", archive=True), "distance m")],
       6, 13, 6, 4, unit=UNIT_M),
], ["replay"], refresh="", time_from="now-6h"))

# ------------------------------------------------------------ Fleet Overview
# One row per vessel (Grafana row repeat over the multi-select vessel
# variable) plus a fleet map. Each tile links to that vessel's Mission
# dashboard. Data arrives per vessel from its own signalk_bridge instance
# (usv-signalk-bridge@<vessel>) and grafana_bridge (vessel_id in the
# mission file); vessels with no data simply show "No data" tiles.
def fleet_link(p, vessel="${vessel}"):
    p["fieldConfig"]["defaults"]["links"] = [{
        "title": "Open Mission dashboard",
        "url": f"/d/usv-mission?var-vessel={vessel}&${{__url_time_range}}",
    }]
    return p


def fleet_last(measurement, field):
    """last() of a field for the row's vessel (row-repeat scopes ${vessel})."""
    return flux(measurement, field, last=True)


fleet_map_query = (
    'from(bucket: "telemetry")\n'
    '  |> range(start: v.timeRangeStart, stop: v.timeRangeStop)\n'
    '  |> filter(fn: (r) => r._measurement == "nav" and (r._field == "lat" or r._field == "lon" or r._field == "heading"))\n'
    '  |> last()\n'
    '  |> group()\n'
    '  |> pivot(rowKey: ["vessel"], columnKey: ["_field"], valueColumn: "_value")\n'
    '  |> filter(fn: (r) => exists r.lat and exists r.lon)\n'
    '  |> keep(columns: ["vessel", "lat", "lon", "heading"])\n')


# One line per vessel: "<engine model>  <rpm> rpm" from the latest engine.rpm
# point (engine_model / engine_type tags from every engine producer; "engine"
# when a vessel has neither yet).
fleet_engine_query = (
    'from(bucket: "telemetry")\n'
    '  |> range(start: v.timeRangeStart, stop: v.timeRangeStop)\n'
    '  |> filter(fn: (r) => r._measurement == "engine" and r._field == "rpm")\n'
    '  |> filter(fn: (r) => r.vessel == "${vessel}")\n'
    '  |> last()\n'
    '  |> group()\n'
    '  |> last()\n'
    '  |> map(fn: (r) => ({_time: r._time, _value: '
    '(if exists r.engine_model then r.engine_model else if exists r.engine_type '
    'then r.engine_type else "engine") + "  " + '
    'string(v: int(v: r._value)) + " rpm"}))\n')


fleet_panels = [
    panel("geomap", "Fleet Map — last known positions",
          [target(fleet_map_query)], 0, 0, 24, 10,
          options={
              "view": {"id": "fit", "zoom": 14},
              "basemap": {"type": "osm-standard", "name": "OSM"},
              "layers": [{
                  "type": "markers", "name": "Vessels",
                  "location": {"mode": "coords", "latitude": "lat",
                               "longitude": "lon"},
                  "config": {"style": VESSEL_MARKER, "showLegend": False},
              }],
              "controls": {"showZoom": True, "mouseWheelZoom": True},
          }),
    # Repeated row: everything below belongs to it (non-collapsed row).
    {"id": nid(), "type": "row", "title": "Vessel: $vessel",
     "repeat": "vessel", "collapsed": False,
     "gridPos": {"x": 0, "y": 10, "w": 24, "h": 1}, "panels": []},
    fleet_link(stat("Mission State", fleet_last("mission_state", "state"),
                    0, 11, 4, 3, mappings=mission_state_mappings,
                    text_mode="value", color_mode="background")),
    fleet_link(stat("Helm", fleet_last("mission_state", "helm_state"),
                    4, 11, 3, 3, mappings=helm_state_mappings,
                    text_mode="value", color_mode="background")),
    fleet_link(stat("Armed", fleet_last("autonomy", "armed"), 7, 11, 3, 3,
                    mappings=[{"type": "value", "options": {
                        "0": {"text": "DISARMED", "color": "orange", "index": 0},
                        "1": {"text": "ARMED", "color": TILE_OK, "index": 1}}}],
                    text_mode="value", color_mode="background")),
    fleet_link(stat("Operator Link", fleet_last("autonomy", "operator_link"),
                    10, 11, 3, 3, mappings=operator_link_mappings,
                    text_mode="value", color_mode="background")),
    fleet_link(stat("Geofence", fleet_last("autonomy", "geofence_breached"),
                    13, 11, 3, 3, mappings=geofence_mappings,
                    text_mode="value", color_mode="background")),
    # Second line of the row: 2-column tiles truncated their titles
    # ("Progr...", "Sign...").
    fleet_link(stat("SOG", fleet_last("nav", "sog"), 16, 11, 8, 3,
                    unit="velocityknot", decimals=1, no_value="—")),
    fleet_link(stat("Mission Progress", fleet_last("mission", "progress_pct"),
                    0, 14, 6, 3, unit="percent", decimals=0,
                    no_value="No mission")),
    fleet_link(stat("Signal K", fleet_last("comms", "signalk_ws_up"),
                    6, 14, 6, 3, mappings=updown_map("DOWN", "UP"),
                    text_mode="value", color_mode="background")),
    fleet_link(stat("MOOS", fleet_last("comms", "moos_connected"),
                    12, 14, 6, 3, mappings=updown_map("DOWN", "UP"),
                    text_mode="value", color_mode="background")),
    # Engine type (the engine_type tag) and the last RPM of the vessel.
    fleet_link(stat("Engine", fleet_engine_query, 18, 14, 6, 3,
                    text_mode="value", string_value=True,
                    no_value="No engine data")),
]
DASHBOARDS.append(dashboard("usv-fleet", "Fleet", fleet_panels,
                            ["fleet"], fleet=True))

# ------------------------------------------------------------- FR-11 Alarms
# Coolant status for the Alarms wall: band 0 / 1 / 2 from the row's
# engine_type (tools/engine_limits.py), the hottest engine of the vessel,
# its temperature as the "detail" label (the tile's name line).
COOLANT_BAND_QUERY = (
    'from(bucket: "telemetry")\n'
    '  |> range(start: v.timeRangeStart, stop: v.timeRangeStop)\n'
    '  |> filter(fn: (r) => r._measurement == "engine" and r._field == "coolant_temp")\n'
    '  |> filter(fn: (r) => r.vessel == "${vessel}")\n'
    '  |> last()\n'
    f'  |> map(fn: (r) => ({{_time: r._time, t: r._value, _value: {engine_limits.flux_coolant_band()}}}))\n'
    '  |> group()\n'
    '  |> sort(columns: ["t"], desc: true)\n'
    '  |> limit(n: 1)\n'
    '  |> map(fn: (r) => ({_time: r._time, _value: r._value, '
    'detail: string(v: int(v: r.t + 0.5)) + " °C"}))\n'
    '  |> group(columns: ["detail"])\n')
COOLANT_BAND_MAPPINGS = [{"type": "value", "options": {
    "0": {"text": "NORMAL", "color": TILE_OK, "index": 0},
    "1": {"text": "HIGH", "color": "yellow", "index": 1},
    "2": {"text": "OVERHEAT", "color": "red", "index": 2}}}]


def coolant_band_tile(x, y, w, h):
    p = stat("Coolant", COOLANT_BAND_QUERY, x, y, w, h, mappings=COOLANT_BAND_MAPPINGS,
             text_mode="value_and_name", color_mode="background")
    p["options"]["reduceOptions"]["fields"] = "/.*/"
    p["fieldConfig"]["defaults"]["displayName"] = "${__field.labels.detail}"
    return p


DASHBOARDS.append(dashboard("usv-alarms", "Alarms", [
    panel("alertlist", "Active Alerts", [], 0, 0, 12, 8,
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
                  '  |> group()\n'
                  '  |> keep(columns: ["_time", "_value", "severity"])\n'
                  '  |> rename(columns: {_time: "Time", _value: "Alarm", severity: "Severity"})\n'
                  '  |> sort(columns: ["Time"], desc: true)\n')],
          12, 0, 12, 8, options={"showHeader": True}),
    stat("Battery SOC", flux("electrical", "soc", last=True), 0, 10, 4, 4,
         unit="percent", decimals=0, no_value="N/A",
         thresholds=neutral_base([(None, "red"), (25, "yellow"), (50, TILE_OK)]), color_mode="background"),
    # Fix quality rather than a satellite count: INS feeds report no count.
    stat("GPS Fix", last_str("nav", "fix_type"), 4, 10, 4, 4,
         text_mode="value", color_mode="background", mappings=gps_fix_mappings),
    stat("CAN Bus", flux("canbus", "interface_up", last=True), 8, 10, 4, 4,
         mappings=updown_map("FAIL", "OK"), text_mode="value", color_mode="background"),
    # Status per the engine type's limits (tools/engine_limits.py): NORMAL /
    # HIGH / OVERHEAT, the hottest engine's temperature on the name line.
    coolant_band_tile(12, 10, 4, 4),
    stat("Fuel", flux("engine", "fuel_level", last=True), 16, 10, 4, 4,
         unit="percent", decimals=0, no_value="N/A",
         thresholds=neutral_base([(None, "red"), (10, "yellow"), (20, TILE_OK)]), color_mode="background"),
    stat("Radar", flux("radar", "online", last=True), 20, 10, 4, 4,
         mappings=updown_map(), text_mode="value", color_mode="background"),
    # One value for the whole camera set: how many ENABLED cameras are not
    # streaming (camera_summary from frigate_bridge). A per-camera query
    # put one ONLINE/OFFLINE value per camera into this small tile and the
    # texts overlapped; the per-camera detail is on the Cameras page.
    stat("Camera", camera_offline_query(), 0, 14, 4, 4,
         unit="suffix: offline", decimals=0, text_mode="value", color_mode="background",
         mappings=[{"type": "value", "options": {
             "0": {"text": "ALL ONLINE", "color": TILE_OK, "index": 0}}}],
         thresholds=neutral_base([(None, TILE_OK), (1, "red")])),
    stat("Signal K", flux("comms", "signalk_ws_up", last=True), 4, 14, 4, 4,
         mappings=updown_map("DISCONNECTED", "CONNECTED"), text_mode="value", color_mode="background"),
    stat("MOOS Rate", flux("comms", "moos_msg_rate", last=True), 8, 14, 4, 4,
         decimals=0, thresholds=neutral_base([(None, "red"), (1, "yellow"), (10, TILE_OK)]), color_mode="background"),
    stat("Comms Link", tele_last("ping", "result_code", extra=PING_INTERNET),
         12, 14, 4, 4, text_mode="value", color_mode="background",
         mappings=[{"type": "value", "options": {
             "0": {"text": "OK", "color": TILE_OK, "index": 0}}},
             {"type": "range", "options": {
                 "from": 1, "to": 99,
                 "result": {"text": "FAILED", "color": "red", "index": 1}}}]),
    stat("Latency", tele_last("ping", "average_response_ms", extra=PING_INTERNET),
         16, 14, 4, 4,
         unit="ms", decimals=0,
         thresholds=[(None, TILE_OK), (150, "yellow"), (400, "red")], color_mode="background"),
    stat("Packet Loss", tele_last("ping", "percent_packet_loss", extra=PING_INTERNET),
         20, 14, 4, 4,
         unit="percent", decimals=1,
         thresholds=[(None, TILE_OK), (2, "yellow"), (10, "red")], color_mode="background"),
    # Autonomy / mission safety row (Alpha mission).
    stat("Mission State", last_str("mission_state", "state"), 0, 18, 4, 4,
         mappings=mission_state_mappings, text_mode="value", color_mode="background",
         no_value="MISSION OFF"),
    stat("Operator Link", flux("autonomy", "operator_link", last=True),
         4, 18, 4, 4, mappings=operator_link_mappings,
         text_mode="value", color_mode="background"),
    stat("Mission Armed", flux("autonomy", "armed", last=True), 8, 18, 4, 4,
         mappings=[{"type": "value", "options": {
             "0": {"text": "DISARMED", "color": "orange", "index": 0},
             "1": {"text": "ARMED", "color": TILE_OK, "index": 1}}}],
         text_mode="value", color_mode="background"),
    stat("Geofence", flux("autonomy", "geofence_breached", last=True),
         12, 18, 4, 4, mappings=geofence_mappings, text_mode="value", color_mode="background"),
    stat("Helm All-Stop", flux("autonomy", "allstop", last=True), 16, 18, 4, 4,
         mappings=alert_map("CLEAR", "ALLSTOP"), text_mode="value", color_mode="background"),
    stat("Obstacles", flux("autonomy", "obstacle_alerts", last=True),
         20, 18, 4, 4, decimals=0,
         thresholds=[(None, TILE_OK), (1, "yellow"), (3, "orange")], color_mode="background"),
], ["alarms"]))

# Navigation guard, ALL STOP and boat control (v1.2.43). The guard writes
# its level once a second while it reports, so a window of 15 s tells a
# silent guard (N/A) from a healthy one.
NAV_GUARD_MAP = [{"type": "value", "options": {
    "0": {"text": "OK", "color": TILE_OK, "index": 0},
    "1": {"text": "WARNING", "color": "yellow", "index": 1},
    "2": {"text": "FAULT", "color": "red", "index": 2}}}]
ALL_STOP_MAP = [{"type": "value", "options": {
    "0": {"text": "CLEAR", "color": TILE_OK, "index": 0},
    "1": {"text": "LATCHED", "color": "red", "index": 1}}}]
HOLDER_MAP = [{"type": "value", "options": {
    "-": {"text": "NOBODY", "color": NEUTRAL, "index": 0}}},
    {"type": "regex", "options": {"pattern": "lost:(.+)", "result": {
        "text": "LOST ($1)", "color": "orange", "index": 1}}},
    {"type": "regex", "options": {"pattern": "(.+)", "result": {
        "text": "$1", "color": TILE_OK, "index": 2}}}]


def recent(measurement, field, window="-15s"):
    return ('from(bucket: "telemetry")\n'
            f'  |> range(start: {window})\n'
            f'  |> filter(fn: (r) => r._measurement == "{measurement}" and r._field == "{field}")\n'
            '  |> filter(fn: (r) => r.vessel == "${vessel}")\n'
            '  |> last()\n')


_alarm_row = [
    stat("Nav Integrity", recent("autonomy", "nav_guard"), 0, 22, 4, 4,
         mappings=NAV_GUARD_MAP, text_mode="value", color_mode="background"),
    stat("ALL STOP", flux("autonomy", "all_stop", last=True), 4, 22, 4, 4,
         mappings=ALL_STOP_MAP, text_mode="value", color_mode="background"),
    stat("Control", flux("mission_state", "control_holder", last=True), 8, 22, 4, 4,
         string_value=True, mappings=HOLDER_MAP, text_mode="value", color_mode="background"),
    stat("Drive Interface", flux("can_drive", "status", last=True), 12, 22, 4, 4, string_value=True,
         mappings=[{"type": "value", "options": {
             "ACTIVE": {"text": "ACTIVE", "color": TILE_OK, "index": 0},
             "STANDBY": {"text": "STANDBY", "color": NEUTRAL, "index": 1},
             "NO_BUS": {"text": "NO BUS", "color": "red", "index": 2},
             "INHIBITED": {"text": "NO PROFILE", "color": "red", "index": 3}}}],
         text_mode="value", color_mode="background"),
    stat("Rudder vs Drive", flux("can_drive", "rudder_mismatch", last=True), 16, 22, 4, 4,
         mappings=[{"type": "value", "options": {
             "0": {"text": "FOLLOWING", "color": TILE_OK, "index": 0},
             "1": {"text": "NOT FOLLOWING", "color": "red", "index": 1}}}],
         text_mode="value", color_mode="background"),
    stat("Service Mode", flux("autopilot", "service_mode", last=True), 20, 22, 4, 4,
         mappings=[{"type": "value", "options": {
             "0": {"text": "OFF", "color": TILE_OK, "index": 0},
             "1": {"text": "ON (TUNING)", "color": "orange", "index": 1}}}],
         text_mode="value", color_mode="background"),
]
for _i, _p in enumerate(_alarm_row):
    _p["id"] = 9601 + _i
_id[0] -= len(_alarm_row)
DASHBOARDS[-1]["panels"].extend(_alarm_row)

# ------------------------------------------------------------- Audit (v1.2.43)
# The boat's control audit trail: logins, control changes (take, hand-over,
# forced takeover with its reason, lost), ALL STOP, refused commands, and
# the navigation guard's and the region's events. grafana_bridge writes one
# row per line (events bucket, measurement "audit", kept a year; tags
# category, action, severity, user, role; fields message, details). Open to
# operators and supervisors only (deploy/install.sh sets the permission).
AUDIT_BASE = ('from(bucket: "events")\n'
              '  |> range(start: v.timeRangeStart, stop: v.timeRangeStop)\n'
              '  |> filter(fn: (r) => r._measurement == "audit" and r.vessel == "${vessel}")\n')
AUDIT_CATEGORY_MAP = [{"type": "value", "options": {
    "control": {"text": "Control", "color": "blue", "index": 0},
    "login": {"text": "Login", "color": "#3a4a66", "index": 1},
    "refused": {"text": "Refused", "color": "orange", "index": 2},
    "all_stop": {"text": "ALL STOP", "color": "red", "index": 3},
    "nav_guard": {"text": "Nav guard", "color": "purple", "index": 4},
    "region": {"text": "Region", "color": "#0f6f6a", "index": 5}}}]
AUDIT_SEARCH_VAR = {
    "name": "search", "label": "Search", "type": "textbox",
    "query": "", "current": {"text": "", "value": ""}, "hide": 0,
}
# Category filter (the mockup's chips): one choice for the audit log.
_AUDIT_CATS = [("All", "all"), ("Control", "control"), ("Login", "login"),
               ("Refused", "refused"), ("ALL STOP", "all_stop"),
               ("Nav guard", "nav_guard"), ("Region", "region")]
AUDIT_CATEGORY_VAR = {
    "name": "category", "label": "Category", "type": "custom",
    "query": ",".join(f"{t} : {v}" for t, v in _AUDIT_CATS),
    "current": {"text": "All", "value": "all"},
    "options": [{"text": t, "value": v, "selected": v == "all"} for t, v in _AUDIT_CATS],
    "hide": 0, "includeAll": False, "multi": False,
}

# Flux helpers for the tiles' second line: "12 min" / "3 h" since a time.
AUDIT_FN = (
    'import "array"\n'
    'import "strings"\n'
    'span = (t) => {\n'
    '    m = (int(v: now()) - int(v: t)) / 60000000000\n'
    '    return if m < 1 then "under a minute" else if m < 120 then string(v: m) + " min" '
    'else string(v: m / 60) + " h"\n'
    '}\n'
    'plural = (n, one) => string(v: n) + " " + one + (if n == 1 then "" else "s")\n')


def audit_last(pred, field="message"):
    """The newest audit row matching pred as a record (_time, action, user);
    action "" when there is none (a 1970 placeholder row keeps the stream
    from being empty)."""
    return ('union(tables: [' + AUDIT_BASE.replace("\n", "\n    ") +
            f'  |> filter(fn: (r) => r._field == "{field}" and ({pred}))\n'
            '      |> group()\n'
            '      |> keep(columns: ["_time", "action", "user"]),\n'
            '    array.from(rows: [{_time: 1970-01-01T00:00:00Z, action: "", user: ""}])])\n'
            '  |> sort(columns: ["_time"])\n'
            '  |> last(column: "_time")\n'
            '  |> findRecord(fn: (key) => true, idx: 0)\n')


def audit_n(pred, field="value"):
    """Number of audit rows matching pred, as an int (0 when none)."""
    return ('(union(tables: [' + AUDIT_BASE.replace("\n", "\n    ") +
            f'  |> filter(fn: (r) => r._field == "{field}" and ({pred}))\n'
            '      |> group()\n'
            '      |> count()\n'
            '      |> keep(columns: ["_value"]),\n'
            '    array.from(rows: [{_value: 0}])])\n'
            '  |> sum()\n'
            '  |> findRecord(fn: (key) => true, idx: 0))._value\n')


def detail_rows(value, detail, ok="true"):
    """One row {_value, detail} with detail as the series label (shown
    above the value by the tile), none when ok is false."""
    return (f'array.from(rows: [{{_value: {value}, detail: {detail}, ok: {ok}}}])\n'
            '  |> filter(fn: (r) => r.ok)\n'
            '  |> drop(columns: ["ok"])\n'
            '  |> group(columns: ["detail"])\n')


def detail_tile(title, query, x, y, w, h, **kw):
    """Stat tile with a second line: the series label "detail" as the tile's
    name line (value_and_name), the value large beneath it."""
    p = stat(title, query, x, y, w, h, **kw)
    p["options"]["textMode"] = "value_and_name"
    # the row carries only _value (detail is a label): every field is it
    p["options"]["reduceOptions"]["fields"] = "/.*/"
    p["fieldConfig"]["defaults"]["displayName"] = "${__field.labels.detail}"
    return p


def _cur(measurement, field, name, default, window="-1m"):
    """The latest value of a telemetry field as {name} (default when none)."""
    return (f'{name} = (union(tables: [from(bucket: "telemetry")\n'
            f'      |> range(start: {window})\n'
            f'      |> filter(fn: (r) => r._measurement == "{measurement}" and r._field == "{field}" '
            'and r.vessel == "${vessel}")\n'
            '      |> last()\n'
            '      |> group()\n'
            '      |> keep(columns: ["_time", "_value"]),\n'
            f'    array.from(rows: [{{_time: 1970-01-01T00:00:00Z, _value: {default}}}])])\n'
            '  |> sort(columns: ["_time"])\n'
            '  |> last(column: "_time")\n'
            '  |> findRecord(fn: (key) => true, idx: 0))._value\n')


control_tile_query = (
    AUDIT_FN +
    _cur("mission_state", "control_holder", "h", '""') +
    _cur("mission_state", "control_role", "role", '""') +
    _cur("mission_state", "control_since_ms", "since", "0.0") +
    'for = if since > 0.0 then " · for " + span(t: time(v: int(v: since * 1000000.0))) else ""\n'
    'd = if h == "-" then "nobody in control"\n'
    '    else if strings.hasPrefix(v: h, prefix: "lost:") then "holder not heard" + for\n'
    '    else role + for\n' +
    detail_rows("h", "d", 'h != ""'))

allstop_tile_query = (
    AUDIT_FN +
    _cur("autonomy", "all_stop", "st", "-1.0") +
    'a = ' + audit_last('r.category == "all_stop"') +
    'd = if a.action == "" then "not pressed in this range"\n'
    '    else (if a.action == "all_stop_reset" then "reset by " else "pressed by ") + a.user '
    '+ ", " + span(t: a._time) + " ago"\n' +
    detail_rows("st", "d", "st >= 0.0"))

navguard_tile_query = (
    AUDIT_FN +
    _cur("autonomy", "nav_guard", "g", "-1.0", "-15s") +
    'c = ' + audit_n('r.category == "nav_guard" and r.action == "stop"') +
    'd = if c == 0 then "no stops in this range" else plural(n: c, one: "stop") + " in this range"\n' +
    detail_rows("g", "d", "g >= 0.0"))

logins_tile_query = (
    AUDIT_FN +
    'c = ' + audit_n('r.category == "login" and r.action == "login"') +
    'k = (union(tables: [' + AUDIT_BASE.replace("\n", "\n    ") +
    '  |> filter(fn: (r) => r._field == "value" and r.category == "login" and r.action == "login")\n'
    '      |> group()\n'
    '      |> distinct(column: "user")\n'
    '      |> count()\n'
    '      |> keep(columns: ["_value"]),\n'
    '    array.from(rows: [{_value: 0}])])\n'
    '  |> sum()\n'
    '  |> findRecord(fn: (key) => true, idx: 0))._value\n' +
    detail_rows("c", 'plural(n: k, one: "user")'))

_ref = 'r.category == "refused"'
refused_tile_query = (
    AUDIT_FN +
    'n = ' + audit_n(_ref) +
    'ro = ' + audit_n(_ref + ' and strings.containsStr(v: r._value, substr: "read-only")', "details") +
    'st = ' + audit_n(_ref + ' and strings.containsStr(v: r._value, substr: "ALL STOP")', "details") +
    'nc = ' + audit_n(_ref + ' and strings.containsStr(v: r._value, substr: "not in control")', "details") +
    'lo = ' + audit_n(_ref + ' and strings.containsStr(v: r._value, substr: "logged-out")', "details") +
    'other = n - ro - st - nc - lo\n'
    'part = (c, t) => if c > 0 then string(v: c) + " " + t else ""\n'
    'parts = [part(c: ro, t: "read-only"), part(c: st, t: "ALL STOP"), '
    'part(c: nc, t: "not in control"), part(c: lo, t: "logged-out login"), part(c: other, t: "other")]\n'
    'd = strings.joinStr(arr: array.filter(arr: parts, fn: (x) => x != ""), v: " · ")\n' +
    detail_rows("n", 'if d == "" then "none in this range" else d'))

forced_tile_query = (
    AUDIT_FN +
    'c = ' + audit_n('r.action == "force"') +
    'f = ' + audit_last('r.action == "force"') +
    'd = if f.action == "" then "none in this range" else f.user + ", " + span(t: f._time) + " ago"\n' +
    detail_rows("c", "d"))

audit_table_query = (
    'import "strings"\n' + AUDIT_BASE +
    '  |> filter(fn: (r) => r._field == "message" or r._field == "details")\n'
    '  |> filter(fn: (r) => "${category}" == "all" or r.category == "${category}")\n'
    '  |> pivot(rowKey: ["_time"], columnKey: ["_field"], valueColumn: "_value")\n'
    '  |> group()\n'
    '  |> filter(fn: (r) => "${search}" == "" or strings.containsStr(v: strings.toLower(v: '
    'r.message + " " + r.details + " " + r.user), substr: strings.toLower(v: "${search}")))\n'
    '  |> keep(columns: ["_time", "severity", "category", "message", "user", "role", "details"])\n'
    '  |> sort(columns: ["_time"], desc: true)\n'
    '  |> limit(n: 1000)\n'
    '  |> rename(columns: {_time: "Time", severity: "Severity", category: "Category", '
    'message: "Event", user: "User", role: "Role", details: "Details"})\n')

# Control: user colours, nobody grey, "lost" (holder not heard) amber.
CONTROL_TIMELINE_MAP = [
    {"type": "value", "options": {"-": {"text": "nobody", "color": NEUTRAL, "index": 0}}},
    {"type": "regex", "options": {"pattern": "lost:(.+)", "result": {
        "text": "lost ($1)", "color": "orange", "index": 1}}}]

audit_timeline = panel("state-timeline", "Who had control", [
    target('from(bucket: "telemetry")\n'
           '  |> range(start: v.timeRangeStart, stop: v.timeRangeStop)\n'
           '  |> filter(fn: (r) => r._measurement == "mission_state" and r._field == "control_holder")\n'
           '  |> filter(fn: (r) => r.vessel == "${vessel}")\n'
           '  |> aggregateWindow(every: v.windowPeriod, fn: last, createEmpty: false)\n'
           '  |> keep(columns: ["_time", "_value"])\n'
           '  |> rename(columns: {_value: "Control"})\n', ref="A"),
    target('from(bucket: "telemetry")\n'
           '  |> range(start: v.timeRangeStart, stop: v.timeRangeStop)\n'
           '  |> filter(fn: (r) => r._measurement == "autonomy" and r._field == "all_stop")\n'
           '  |> filter(fn: (r) => r.vessel == "${vessel}")\n'
           '  |> aggregateWindow(every: v.windowPeriod, fn: max, createEmpty: false)\n'
           '  |> keep(columns: ["_time", "_value"])\n'
           '  |> rename(columns: {_value: "ALL STOP"})\n', ref="B"),
    target('from(bucket: "telemetry")\n'
           '  |> range(start: v.timeRangeStart, stop: v.timeRangeStop)\n'
           '  |> filter(fn: (r) => r._measurement == "autonomy" and r._field == "nav_guard")\n'
           '  |> filter(fn: (r) => r.vessel == "${vessel}")\n'
           '  |> aggregateWindow(every: v.windowPeriod, fn: max, createEmpty: false)\n'
           '  |> keep(columns: ["_time", "_value"])\n'
           '  |> rename(columns: {_value: "Navigation guard"})\n', ref="C"),
], 0, 3, 24, 4,
    overrides=[
        {"matcher": {"id": "byName", "options": "ALL STOP"},
         "properties": [{"id": "mappings", "value": ALL_STOP_MAP}]},
        {"matcher": {"id": "byName", "options": "Navigation guard"},
         "properties": [{"id": "mappings", "value": NAV_GUARD_MAP}]},
        {"matcher": {"id": "byName", "options": "Control"},
         "properties": [{"id": "mappings", "value": CONTROL_TIMELINE_MAP},
                        {"id": "color", "value": {"mode": "palette-classic"}}]}],
    options={"showValue": "auto", "mergeValues": True, "rowHeight": 0.8,
             "alignValue": "left", "legend": {"showLegend": False}})

AUDIT_TABLE_OVERRIDES = [
    {"matcher": {"id": "byName", "options": "Time"},
     "properties": [{"id": "custom.width", "value": 160},
                    {"id": "unit", "value": "dateTimeAsIso"}]},
    {"matcher": {"id": "byName", "options": "Severity"},
     "properties": [{"id": "custom.width", "value": 85},
                    {"id": "mappings", "value": [{"type": "value", "options": {
                        "info": {"text": "Info", "color": TILE_OK, "index": 0},
                        "warning": {"text": "Warning", "color": "orange", "index": 1},
                        "critical": {"text": "Critical", "color": "red", "index": 2}}}]},
                    {"id": "custom.cellOptions", "value": {"type": "color-background"}}]},
    {"matcher": {"id": "byName", "options": "Category"},
     "properties": [{"id": "custom.width", "value": 95},
                    {"id": "mappings", "value": AUDIT_CATEGORY_MAP},
                    {"id": "custom.cellOptions", "value": {"type": "color-background"}}]},
    {"matcher": {"id": "byName", "options": "Event"},
     "properties": [{"id": "custom.minWidth", "value": 300}]},
    {"matcher": {"id": "byName", "options": "User"},
     "properties": [{"id": "custom.width", "value": 90}]},
    {"matcher": {"id": "byName", "options": "Role"},
     "properties": [{"id": "custom.width", "value": 90}]},
    {"matcher": {"id": "byName", "options": "Details"},
     "properties": [{"id": "custom.minWidth", "value": 180}]},
]

REFUSED_REASONS = [("read-only", "read-only account"), ("ALL STOP", "ALL STOP latched"),
                   ("not in control", "not in control"),
                   ("changed hands", "stale (control changed)"),
                   ("may not command", "role may not command"),
                   ("logged-out", "logged-out session"), ("boat link", "boat link down")]
_reason_expr = "".join(
    f'if strings.containsStr(v: r._value, substr: "{k}") then "{t}" else '
    for k, t in REFUSED_REASONS) + '"other"'
refused_by_reason_query = (
    'import "strings"\n' + AUDIT_BASE +
    '  |> filter(fn: (r) => r._field == "details" and r.category == "refused")\n'
    f'  |> map(fn: (r) => ({{reason: {_reason_expr}, _value: 1}}))\n'
    '  |> group(columns: ["reason"])\n'
    '  |> sum()\n'
    '  |> group()\n'
    '  |> sort(columns: ["_value"], desc: true)\n'
    '  |> rename(columns: {reason: "Reason", _value: "Refused"})\n')

events_by_user_query = (
    AUDIT_BASE +
    '  |> filter(fn: (r) => r._field == "value" and r.user != "-")\n'
    '  |> group(columns: ["user"])\n'
    '  |> reduce(identity: {control: 0, logins: 0, refused: 0}, fn: (r, accumulator) => ({\n'
    '      control: accumulator.control + (if r.category == "control" or r.category == "all_stop" then 1 else 0),\n'
    '      logins: accumulator.logins + (if r.category == "login" and r.action == "login" then 1 else 0),\n'
    '      refused: accumulator.refused + (if r.category == "refused" then 1 else 0)}))\n'
    '  |> group()\n'
    '  |> keep(columns: ["user", "control", "logins", "refused"])\n'
    '  |> sort(columns: ["user"])\n'
    '  |> rename(columns: {user: "User", control: "Control", logins: "Logins", refused: "Refused"})\n')

open_sessions_query = (
    'from(bucket: "telemetry")\n'
    '  |> range(start: -30s)\n'
    '  |> filter(fn: (r) => r._measurement == "c2_session" and r.vessel == "${vessel}")\n'
    '  |> last()\n'
    '  |> pivot(rowKey: ["session"], columnKey: ["_field"], valueColumn: "_value")\n'
    '  |> group()\n'
    '  |> keep(columns: ["user", "role", "ip"])\n'
    '  |> sort(columns: ["user"])\n'
    '  |> rename(columns: {user: "User", role: "Role", ip: "IP"})\n')

_count_tile = dict(decimals=0, no_value="0", text_mode="value_and_name")
DASHBOARDS.append(dashboard("usv-audit", "Audit", [
    # The three state tiles read what grafana_bridge wrote in the last
    # seconds: nothing while the mission (pAntler) is off.
    detail_tile("Control", control_tile_query, 0, 0, 4, 3,
                mappings=HOLDER_MAP, color_mode="background", string_value=False,
                no_value="MISSION OFF"),
    detail_tile("ALL STOP", allstop_tile_query, 4, 0, 4, 3,
                mappings=ALL_STOP_MAP, color_mode="background", no_value="MISSION OFF"),
    detail_tile("Navigation Guard", navguard_tile_query, 8, 0, 4, 3,
                mappings=NAV_GUARD_MAP, color_mode="background", no_value="MISSION OFF"),
    detail_tile("Logins", logins_tile_query, 12, 0, 4, 3, **_count_tile),
    detail_tile("Commands Refused", refused_tile_query, 16, 0, 4, 3,
                thresholds=[(None, OK), (1, "orange")], **_count_tile),
    detail_tile("Forced Takeovers", forced_tile_query, 20, 0, 4, 3,
                thresholds=[(None, OK), (1, "orange")], **_count_tile),
    audit_timeline,
    dict(panel("table", "Audit log", [target(audit_table_query)], 0, 7, 16, 10,
               overrides=AUDIT_TABLE_OVERRIDES, custom={"filterable": True, "align": "auto"},
               options={"showHeader": True, "cellHeight": "sm",
                        "footer": {"show": False}}),
         description=("Newest first. Category and Search at the top of the page filter it; "
                      "the funnel in each column header filters that column. CSV: panel "
                      "menu > Inspect > Data > Download CSV."),
         # keep() does not order the columns: fix the reading order.
         transformations=[{"id": "organize", "options": {"indexByName": {
             "Time": 0, "Severity": 1, "Category": 2, "Event": 3,
             "User": 4, "Role": 5, "Details": 6}}}],
         links=[{"title": "Download CSV (Inspect > Data)", "url": "?inspect=${__panel.id}&inspectTab=data",
                 "targetBlank": False}]),
    panel("table", "Refused commands by reason", [target(refused_by_reason_query)],
          16, 7, 8, 3, options={"showHeader": True, "cellHeight": "sm"},
          no_value="none in this range", custom={"minWidth": 50},
          overrides=[{"matcher": {"id": "byName", "options": "Refused"},
                      "properties": [{"id": "custom.cellOptions",
                                      "value": {"type": "gauge", "mode": "basic"}},
                                     {"id": "color", "value": {"mode": "fixed",
                                                               "fixedColor": "orange"}},
                                     {"id": "min", "value": 0}]}]),
    # Narrow number columns: four columns fit the 8-wide panel without a
    # horizontal scroll bar (Grafana's default minimum is 150 px a column).
    panel("table", "Events by user", [target(events_by_user_query)],
          16, 10, 8, 3, options={"showHeader": True, "cellHeight": "sm"},
          no_value="none in this range", custom={"minWidth": 50},
          overrides=[{"matcher": {"id": "byName", "options": n},
                      "properties": [{"id": "custom.width", "value": 70}]}
                     for n in ("Control", "Logins", "Refused")]),
    dict(panel("table", "Open sessions", [target(open_sessions_query)],
               16, 13, 8, 4, options={"showHeader": True, "cellHeight": "sm"},
               no_value="none", custom={"minWidth": 50}),
         description="Logged in to Swiftships-UI and not yet logged out or disconnected."),
], ["audit"], refresh="10s", time_from="now-24h"))
# Own id range so the header-band ids of the other pages stay as they were.
_audit = DASHBOARDS[-1]["panels"]
for _i, _p in enumerate(_audit):
    _p["id"] = 9701 + _i
_id[0] -= len(_audit)


# ------------------------------------------------------------- header band
# Swiftships wordmark + dashboard name + selected vessel across the top of
# every dashboard (also visible in ?kiosk mode, where Grafana's own chrome is
# hidden). The image is served from Grafana's public/img, put there by
# deploy/brand.sh, so it works offline. The white chip renders on both
# themes without any styling.
BRAND_H = 2


def brand_panel(title, fleet=False):
    # Markdown mode: Grafana 13's HTML sanitizer strips <img>, Markdown images
    # are supported. The chip is pre-sized (44 px high) for the band.
    return {
        "id": nid(), "type": "text", "title": "", "transparent": True,
        "gridPos": {"x": 0, "y": 0, "w": 24, "h": BRAND_H},
        "options": {"mode": "markdown", "content": (
            f"![Swiftships](/public/img/swiftships_wordmark_chip_sm.png) "
            f"&nbsp;&nbsp; **{title.upper()}** &nbsp;&nbsp;&middot;&nbsp;&nbsp; "
            # Class name (VESSEL_CLASS in usv.env) before the vessel id.
            + ("" if fleet else "${vessel_class} &nbsp;&nbsp;&middot;&nbsp;&nbsp; ")
            + "Vessel **$vessel**"
            # Engine chip (the engine_model variable; blank until the vessel
            # writes the tag). The Fleet page has no single vessel.
            + ("" if fleet else
               " &nbsp;&nbsp;&middot;&nbsp;&nbsp; Engine **${engine_model}**"))},
    }


TILE_H = {"stat": 3, "gauge": 4}


def compact_rows(d):
    """Operator-screen density for the dashboards without a hand layout:
    a grid row made only of stat/gauge tiles is shrunk to the tile height
    and everything below moves up. Rows are never collapsed or hidden."""
    panels = d["panels"]
    for y in sorted({p["gridPos"]["y"] for p in panels}):
        row = [p for p in panels if p["gridPos"]["y"] == y]
        if not row or not all(p["type"] in TILE_H for p in row):
            continue
        if sum(p["gridPos"]["w"] for p in row) < 24:
            continue  # partial row beside a tall panel: hand layout, keep
        h_old = max(p["gridPos"]["h"] for p in row)
        h_new = max(TILE_H[p["type"]] for p in row)
        if h_new >= h_old:
            continue
        delta = h_old - h_new
        for p in row:
            p["gridPos"]["h"] = h_new
        for p in panels:
            if p["gridPos"]["y"] > y:
                p["gridPos"]["y"] -= delta
    return d


def add_header_band(d):
    compact_rows(d)
    for p in d["panels"]:
        p["gridPos"]["y"] += BRAND_H
    d["panels"].insert(0, brand_panel(d["title"], fleet=d["uid"] == "usv-fleet"))
    return d


def use_fleet_variable(d):
    """Fleet dashboard: every ${vessel} reference (queries, row repeat,
    links, header band) follows the fleet_vessel variable instead."""
    text = json.dumps(d).replace("${vessel}", "${fleet_vessel}") \
                        .replace("$vessel", "$fleet_vessel") \
                        .replace('"repeat": "vessel"', '"repeat": "fleet_vessel"')
    d.clear()
    d.update(json.loads(text))
    return d


# Every page must fit one operator screen without scrolling (1080p console,
# browser chrome + Grafana header + link bar leave ~19 grid rows; the Comms
# page at 19 rows was the reference that just fits).
SCREEN_ROWS = 19
# Pages allowed one more row (their auto-fit zoom is a little smaller).
SCREEN_ROWS_FOR = {"usv-readiness": 20}
MIN_PANEL_ROWS = 2
MIN_GAUGE_ROWS = 5   # dials (ECharts) stay legible: value + sub-label


def fit_to_screen(d, rows=SCREEN_ROWS):
    """Compress a dashboard vertically to `rows` grid rows. Panel edges are
    remapped monotonically, so panels that were side by side stay aligned
    and nothing overlaps; each panel keeps at least MIN_PANEL_ROWS (or its
    own height if smaller). The scale is lowered until the page fits."""
    panels = [p for p in d["panels"]]
    for p in d["panels"]:
        if p.get("type") == "row":
            panels.extend(p.get("panels", []))
    bottom = max(p["gridPos"]["y"] + p["gridPos"]["h"] for p in panels)
    if bottom <= rows:
        return
    edges = sorted({e for p in panels
                    for e in (p["gridPos"]["y"], p["gridPos"]["y"] + p["gridPos"]["h"])})
    f = 1.0
    while f > 0.2:
        new = {}
        for e in edges:
            pos = new[edges[edges.index(e) - 1]] if e != edges[0] else 0
            for p in panels:
                g = p["gridPos"]
                if g["y"] + g["h"] == e:
                    floor = (MIN_GAUGE_ROWS
                             if p.get("type") == "volkovlabs-echarts-panel"
                             else MIN_PANEL_ROWS)
                    hmin = g["h"] if g["h"] < floor else \
                        max(floor, round(g["h"] * f))
                    pos = max(pos, new[g["y"]] + hmin)
            new[e] = pos
        if new[edges[-1]] <= rows:
            break
        f -= 0.01
    for p in panels:
        g = p["gridPos"]
        y0, y1 = new[g["y"]], new[g["y"] + g["h"]]
        g["y"], g["h"] = y0, y1 - y0


def pad_to_screen(d, rows=SCREEN_ROWS):
    """A page shorter than `rows` (Alarms, 18) is stretched: the panels on
    its bottom edge grow by the difference, so every page ends on the same
    line and the auto-fit zoom (deploy/brand.sh) gives the same scale on
    each page."""
    panels = list(d["panels"])
    for p in d["panels"]:
        if p.get("type") == "row":
            panels.extend(p.get("panels", []))
    bottom = max(p["gridPos"]["y"] + p["gridPos"]["h"] for p in panels)
    if bottom >= rows:
        return
    for p in panels:
        if p["gridPos"]["y"] + p["gridPos"]["h"] == bottom:
            p["gridPos"]["h"] += rows - bottom


SCREEN_PX = 1532      # operator browser width the layout is checked at
CHAR_EM = 0.6         # average glyph width / font size (Inter, bold caps)


def size_compact_tiles(d):
    """Fixed value sizes that never clip. A 2-row stat tile is 68 px:
    Grafana's panel header takes 32 px, so the value gets ~36 px; 26 px
    text clipped its descenders on usv-722, 18 px (text) / 20 px (numbers)
    sits inside the band. A fixed size is not shrunk by Grafana, so a
    tile's longest mapped state (e.g. "LOST — FAILSAFE") also has to fit
    its width; the size is lowered until it does."""
    col = SCREEN_PX / 24.0
    panels = list(d["panels"])
    for p in d["panels"]:
        if p.get("type") == "row":
            panels.extend(p.get("panels", []))
    for p in panels:
        if p.get("type") != "stat":
            continue
        text = p["options"].setdefault("text", {})
        is_str = p["options"]["reduceOptions"].get("fields") == "/.*/"
        if p["gridPos"]["h"] <= 2:
            text["valueSize"] = 18 if is_str else 20
            text["titleSize"] = 12
        words = []
        for m in p["fieldConfig"]["defaults"].get("mappings", []):
            opts = m.get("options", {})
            res = [opts["result"]] if "result" in opts else \
                [v for v in opts.values() if isinstance(v, dict)]
            words += [r.get("text", "") for r in res]
        words.append(p["fieldConfig"]["defaults"].get("noValue", ""))
        longest = max((len(w) for w in words), default=0)
        if longest:
            room = p["gridPos"]["w"] * col - 24
            fit = int(room / (longest * CHAR_EM))
            text["valueSize"] = max(12, min(text.get("valueSize", 26), fit))


# Panel-header title room measured in Grafana 13.2 on usv-722 at 1532 px
# (navigation menu closed): the grid is 59.7 px per column and a panel
# shows w x 59.7 - 73 px of title (the rest is padding and the hover menu);
# a 3-column tile shows 106 px of a 14 px Inter Medium title.
TITLE_PX_PER_COL, TITLE_PX_FIXED, TITLE_SLACK = 59.7, 73, 2
_INTER = "/usr/share/fonts/opentype/inter/Inter-Medium.otf"


def _title_px(text):
    """Rendered width of a panel title: Inter Medium 14 px when the font is
    installed (matches Grafana to 1 px), else a conservative estimate."""
    try:
        from PIL import ImageFont
        return ImageFont.truetype(_INTER, 14).getlength(text)
    except (ImportError, OSError):
        return len(text) * 14 * 0.52


def check_titles(d):
    """Fail the build when a panel title would be cut with an ellipsis."""
    cut = []
    for p in d["panels"]:
        t = p.get("title") or ""
        if not t or p.get("type") in ("row", "text"):
            continue
        room = p["gridPos"]["w"] * TITLE_PX_PER_COL - TITLE_PX_FIXED
        if _title_px(t) > room - TITLE_SLACK:
            cut.append(f'{d["title"]}: "{t}" (w{p["gridPos"]["w"]}, '
                       f'{_title_px(t):.0f} px > {room - TITLE_SLACK:.0f} px)')
    if cut:
        raise SystemExit("panel titles cut at 1532 px:\n  " + "\n  ".join(cut))


DASHBOARDS.append(_LIVE)  # last: see the Autopilot Live section
DASHBOARDS.append(_ENGINE)  # same reason: own id range 9401+
# Vessel Health engine dials link to the Engine page (the Health page is
# defined after the Engine section, so the links are added here).
ENGINE_LINK = {"title": "Open Engine (large dials, 1 s)",
               "url": "/d/usv-engine?var-vessel=${vessel}",
               "targetBlank": False}
for _d in DASHBOARDS:
    if _d["uid"] == "usv-health":
        for _p in _d["panels"]:
            if _p.get("title") in ("Tachometer", "Coolant", "Oil Pressure", "Engine Faults"):
                _p.setdefault("links", []).append(dict(ENGINE_LINK))


LIVE_REFRESH_INTERVALS = ["1s", "2s", "5s", "10s", "30s", "1m", "5m",
                          "15m", "30m", "1h"]
# A page refreshes every 1 s only while the browser can finish all its
# queries in that second: it runs six at a time and each refresh cancels
# whatever is still in flight, so on a page with many queries the lower
# panels never complete and stay blank (usv-722 Navigation, 2026-10-21:
# 26 queries, blank until the refresh was turned off). Pages with more
# queries than this refresh every 2 s; the picker still offers 1 s.
MAX_QUERIES_AT_1S = 18


def main():
    os.makedirs(OUT, exist_ok=True)
    for d in DASHBOARDS:
        if d.get("refresh") == "1s":
            # Grafana keeps a dashboard's refresh only if it is in the
            # picker's list, and the built-in list starts at 5 s: without
            # this, "1s" is dropped and auto-refresh is off (usv-722,
            # 2026-10-12). Needs min_refresh_interval <= 1s (install.sh).
            d["timepicker"] = {"refresh_intervals": LIVE_REFRESH_INTERVALS}
            if sum(len(p.get("targets", [])) for p in d["panels"]) > MAX_QUERIES_AT_1S:
                d["refresh"] = "2s"
        add_header_band(d)
        fit_to_screen(d, SCREEN_ROWS_FOR.get(d["uid"], SCREEN_ROWS))
        pad_to_screen(d, SCREEN_ROWS_FOR.get(d["uid"], SCREEN_ROWS))
        size_compact_tiles(d)
        check_titles(d)
        if d["uid"] == "usv-traffic":
            d["templating"]["list"].append(TRAFFIC_RANGE_VAR)
        if d["uid"] == "usv-audit":
            d["templating"]["list"].extend([AUDIT_CATEGORY_VAR, AUDIT_SEARCH_VAR])
        if d["uid"] == "usv-fleet":
            use_fleet_variable(d)
        path = os.path.join(OUT, f"{d['uid']}.json")
        with open(path, "w") as f:
            json.dump(d, f, indent=2)
        print(f"wrote {path}  ({len(d['panels'])} panels)")


if __name__ == "__main__":
    main()
