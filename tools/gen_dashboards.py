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
                '/^(_start|_stop|_measurement|vessel|host|instance|source)$/)\n')

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
# Freeboard red, rotated by heading, name label above. Custom SVG symbols
# go through a texture path that rendered as a black square on Grafana
# 13.2.1, so the marker stays with the built-in shape.
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
# waypoints, the vessel's track (red) and the vessel itself.
TRACK_RED = "#e53935"
ROUTE_GREEN = "#2e7d32"
REGION_CYAN = "#00bcd4"


def overlay_layers(track_ref="A", vessel_ref="B", route_ref="C",
                   region_ref="D"):
    def coords():
        return {"mode": "coords", "latitude": "lat", "longitude": "lon"}
    return [
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
    "route_name", "allstop_reason", "active_behavior", "station_keep", "comms_failsafe",
    "entry_gate", "opr_status", "helm_summary", "arrival_state",
    "corridor_status", "crash_stop_status", "region_warning", "wpt_stat",
    "fix_type", "charging_status", "message", "worst_contact", "reason",
    "encounter", "type", "contact",
}


def _is_string_query(query):
    fields = _FIELD_RE.findall(query)
    return bool(fields) and all(f in STRING_FIELDS for f in fields)


def stat(title, query, x, y, w=4, h=4, unit=None, thresholds=None,
         mappings=None, text_mode="auto", decimals=None,
         color_mode="value", graph_mode="none", string_value=False,
         no_value=None):
    """color_mode='background' renders a solid status tile (ops wall look);
    graph_mode='area' adds a trend sparkline behind numeric values;
    no_value replaces Grafana's "No data" (e.g. "N/A" for hardware that is
    not fitted on this vessel)."""
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
                          "text": {"valueSize": (22 if is_str else 26)
                                   if h <= 3 else 34,
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
    "CommsLost": "Comms Lost",
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
};
"""


def _zone_fracs(min_, max_, zones):
    return ", ".join(f"[{(hi - min_) / (max_ - min_):.4f}, '{c}']"
                     for hi, c in zones)


def meter_js(min_, max_, unit, zones=None, decimals=0, split=6, sub="",
             label_js=None, detail_js=None):
    """ECharts option code for a 240° analog needle meter. zones =
    [(upper_bound, color), ...]; None -> neutral rail + teal progress arc."""
    if zones:
        axis = (f"axisLine: {{ lineStyle: {{ width: 9, "
                f"color: [{_zone_fracs(min_, max_, zones)}] }} }}, "
                f"progress: {{ show: false }}")
    else:
        axis = ("axisLine: { roundCap: true, lineStyle: { width: 9, "
                "color: [[1, C.rail]] } }, "
                "progress: { show: true, width: 9, roundCap: true, "
                "itemStyle: { color: { type: 'linear', x: 0, y: 0, "
                "x2: 1, y2: 0, colorStops: [ "
                "{ offset: 0, color: '#00bcd4' }, "
                "{ offset: 1, color: '#42a5f5' } ] }, "
                "shadowColor: 'rgba(0,188,212,0.45)', shadowBlur: 8 } }")
    label_fmt = f", formatter: {label_js}" if label_js else ""
    detail_fmt = (detail_js if detail_js
                  else f"(x) => x.toFixed({decimals}) + ' {unit}'")
    return ECH_PRELUDE + f"""\
let v = val();
if (v === null || isNaN(v)) {{ v = {min_}; }}
v = Math.max({min_}, Math.min({max_}, v));
return {{
  series: [{{
    type: 'gauge', min: {min_}, max: {max_},
    startAngle: 210, endAngle: -30,
    center: ['50%', '58%'], radius: '88%',
    {axis},
    splitNumber: {split},
    splitLine: {{ length: 12, distance: -12,
                 lineStyle: {{ color: C.tick, width: 2 }} }},
    axisTick: {{ length: 6, distance: -6, splitNumber: 5,
                lineStyle: {{ color: C.tick, width: 1 }} }},
    axisLabel: {{ distance: 15, color: C.label, fontSize: 10{label_fmt} }},
    pointer: {{ width: 4, length: '62%',
               itemStyle: {{ color: C.needle,
                            shadowColor: 'rgba(0,0,0,0.45)',
                            shadowBlur: 6, shadowOffsetY: 2 }} }},
    anchor: {{ show: true, size: 10, showAbove: true,
              itemStyle: {{ color: C.pivot, borderColor: C.needle,
                           borderWidth: 2 }} }},
    title: {{ show: true, offsetCenter: [0, '92%'], color: C.sub,
             fontSize: 10 }},
    detail: {{ valueAnimation: true, color: C.text, fontSize: 17,
              fontWeight: 600, offsetCenter: [0, '60%'],
              formatter: {detail_fmt} }},
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
    title: { show: true, offsetCenter: [0, '44%'], color: C.sub,
             fontSize: 10 },
    detail: { valueAnimation: true, color: C.text, fontSize: 18,
              fontWeight: 600, offsetCenter: [0, '26%'],
              formatter: (x) => x.toFixed(0) + '\\u00b0' },
    data: [{ value: hdg, name: 'HDG \\u2014 COG ' + cog.toFixed(0) +
             '\\u00b0' }],
  }), Object.assign({}, base, {
    axisLine: { show: false }, splitLine: { show: false },
    axisTick: { show: false }, axisLabel: { show: false },
    pointer: { width: 3, length: '52%',
               itemStyle: { color: C.accent2, opacity: 0.9 } },
    anchor: { show: false }, title: { show: false },
    detail: { show: false },
    data: [{ value: cog }],
  })],
};
"""


def rudder_js():
    """Rudder angle indicator, center-zero, port red / starboard green."""
    return ECH_PRELUDE + """\
let v = val(); if (v === null || isNaN(v)) { v = 0; }
v = Math.max(-35, Math.min(35, v));
return {
  series: [{
    type: 'gauge', min: -35, max: 35,
    startAngle: 210, endAngle: -30,
    center: ['50%', '58%'], radius: '88%',
    axisLine: { lineStyle: { width: 9, color: [
      [0.5, 'rgba(242,73,92,0.85)'], [1, 'rgba(115,191,105,0.85)'] ] } },
    splitNumber: 7,
    splitLine: { length: 12, distance: -12,
                 lineStyle: { color: C.tick, width: 2 } },
    axisTick: { length: 6, distance: -6, splitNumber: 5,
                lineStyle: { color: C.tick, width: 1 } },
    axisLabel: { distance: 15, color: C.label, fontSize: 10,
                 formatter: (x) => Math.abs(x) },
    pointer: { width: 4, length: '62%',
               itemStyle: { color: C.needle,
                            shadowColor: 'rgba(0,0,0,0.45)',
                            shadowBlur: 6, shadowOffsetY: 2 } },
    anchor: { show: true, size: 10, showAbove: true,
              itemStyle: { color: C.pivot, borderColor: C.needle,
                           borderWidth: 2 } },
    title: { show: true, offsetCenter: [0, '92%'], color: C.sub,
             fontSize: 10 },
    detail: { valueAnimation: true, color: C.text, fontSize: 17,
              fontWeight: 600, offsetCenter: [0, '60%'],
              formatter: (x) => (x < -0.5 ? 'PORT ' :
                                 (x > 0.5 ? 'STBD ' : '')) +
                                Math.abs(x).toFixed(0) + '\\u00b0' },
    data: [{ value: v, name: 'PORT \\u2039 RUDDER \\u203a STBD' }],
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


# Fixed tab order on every dashboard (Grafana's tag-based link list sorts
# alphabetically and drops the current dashboard, so the row shifted from
# page to page). Operational sequence: mission first, support last.
TAB_ORDER = [
    ("usv-mission", "Mission"), ("usv-navigation", "Navigation"),
    ("usv-autopilot", "Autopilot"), ("usv-health", "Vessel Health"),
    ("usv-canbus", "CAN Bus"), ("usv-traffic", "Traffic"),
    ("usv-camera", "Cameras"), ("usv-comms", "Comms"),
    ("usv-system", "System"), ("usv-replay", "Replay"),
    ("usv-alarms", "Alarms"), ("usv-fleet", "Fleet"),
]


def dashboard(uid, title, panels, tags, refresh="5s", time_from="now-30m",
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
            "keepTime": True, "includeVars": u != "usv-fleet",
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
        "templating": {"list": [vessel_var]},
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
DASHBOARDS.append(dashboard("usv-mission", "Mission", [
    # A — the operational picture: 8 equal status tiles (3 x 3).
    stat("Mission State", last_str("mission_state", "state"), 0, 0, 3, 3,
         mappings=mission_state_mappings, text_mode="value",
         color_mode="background"),
    stat("Helm", last_str("mission_state", "helm_state"), 3, 0, 3, 3,
         mappings=helm_state_mappings, text_mode="value",
         color_mode="background"),
    stat("Armed", flux("autonomy", "armed", last=True), 6, 0, 3, 3,
         mappings=[{"type": "value", "options": {
             "0": {"text": "DISARMED", "color": "orange", "index": 0},
             "1": {"text": "ARMED", "color": TILE_OK, "index": 1}}}],
         text_mode="value", color_mode="background"),
    stat("Operator Link", flux("autonomy", "comms_phase", last=True),
         9, 0, 3, 3, mappings=comms_phase_mappings,
         text_mode="value", color_mode="background"),
    stat("Geofence", flux("autonomy", "geofence_breached", last=True),
         12, 0, 3, 3, mappings=geofence_mappings, text_mode="value",
         color_mode="background"),
    stat("All-Stop", last_str("mission_state", "allstop_reason"),
         15, 0, 3, 3, mappings=allstop_mappings, text_mode="value",
         color_mode="background"),
    stat("Arrival", last_str("mission_state", "arrival_state"),
         18, 0, 3, 3, mappings=arrival_mappings, text_mode="value",
         color_mode="background"),
    stat("Obstacles", flux("autonomy", "obstacle_alerts", last=True),
         21, 0, 3, 3, decimals=0,
         thresholds=[(None, TILE_OK), (1, "yellow"), (3, "orange")],
         color_mode="background"),
    # B — identity, timers, route position: 8 equal readouts (3 x 3).
    stat("Mission", last_str("mission_state", "mission_name"), 0, 3, 3, 3,
         text_mode="value"),
    stat("Route", last_str("mission_state", "route_name"), 3, 3, 3, 3,
         text_mode="value"),
    stat("Elapsed", flux("mission", "elapsed_s", last=True), 6, 3, 3, 3,
         unit="dthms", decimals=0),
    stat("ETA to WP", flux("mission", "eta_s", last=True), 9, 3, 3, 3,
         unit="dthms", decimals=0),
    stat("Waypoint", waypoint_of_total_query, 12, 3, 3, 3, text_mode="value",
         string_value=True),
    stat("Remaining", flux("mission", "waypoints_remaining", last=True),
         15, 3, 3, 3, decimals=0),
    stat("Dist to WP", flux("mission", "dist_to_waypoint", last=True),
         18, 3, 3, 3, unit=UNIT_M, decimals=0,
         thresholds=approach_thresholds),
    stat("Traveled", flux("mission", "distance_traveled", last=True, nm=True),
         21, 3, 3, 3, unit=UNIT_NM, decimals=2),
    # C — progress bar across the width.
    panel("bargauge", "Mission Progress",
          [target(flux("mission", "progress_pct", last=True))],
          0, 6, 24, 2, unit="percent", min_=0, max_=100, decimals=0,
          thresholds=[(None, ACCENT)],
          options={"reduceOptions": {"calcs": ["lastNotNull"]},
                   "orientation": "horizontal", "displayMode": "lcd",
                   "showUnfilled": True, "valueMode": "color",
                   "text": {"valueSize": 18}}),
    # D — approach profile (governor tiers) + autonomy flag activity.
    panel("timeseries", "Approach Profile — Distance to Next Waypoint vs SOG",
          [target(flux("mission", "dist_to_waypoint"), "distance m"),
           target(flux("nav", "sog"), "SOG kn", ref="B")],
          0, 8, 12, 7, unit=UNIT_M,
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
          12, 8, 12, 7, mappings=flag_timeline_mappings,
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
          0, 15, 13, 5, mappings=mission_state_mappings,
          custom={"fillOpacity": 70, "lineWidth": 0},
          overrides=[{"matcher": {"id": "byRegexp", "options": ".*"},
                      "properties": [{"id": "displayName", "value": "state"}]}],
          options={"showValue": "auto", "legend": {"showLegend": False}}),
    panel("table", "Mission Events",
          [target(events_query)],
          13, 15, 11, 5,
          overrides=[{"matcher": {"id": "byName", "options": "Time"},
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
                                      "value": {"type": "color-text"}}]}],
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
           target(geometry_query("region"), ref="D")],
          0, 0, 14, 13,
          options={
              "view": {"id": "fit", "zoom": 15},
              "basemap": {"type": "osm-standard", "name": "OSM"},
              "layers": overlay_layers(),
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
    stat("GPS Fix", last_str("nav", "fix_type"), 14, 6, 5, 3,
         text_mode="value", color_mode="background",
         mappings=gps_fix_mappings),
    stat("Satellites", flux("nav", "satellites", last=True), 19, 6, 5, 3,
         decimals=0, thresholds=[(None, "red"), (5, "yellow"), (8, TILE_OK)],
         color_mode="background"),
    stat("Latitude", flux("nav", "lat", last=True), 14, 9, 5, 4, decimals=6),
    stat("Longitude", flux("nav", "lon", last=True), 19, 9, 5, 4, decimals=6),
    # KPI strip: 8 equal tiles.
    stat("Speed Cap", flux("autopilot", "speed_cap", last=True), 0, 13, 3, 3,
         unit="velocityknot", decimals=1),
    stat("COG", flux("nav", "cog", last=True), 3, 13, 3, 3,
         unit="degree", decimals=0),
    stat("WP Bearing", flux("traffic", "planned_course", last=True), 6, 13, 3, 3,
         unit="degree", decimals=0),
    stat("XTE", flux("nav", "cross_track_error", last=True), 9, 13, 3, 3,
         unit=UNIT_M, decimals=1, graph_mode="area"),
    stat("Dist to WP", flux("mission", "dist_to_waypoint", last=True),
         12, 13, 3, 3, unit=UNIT_M, decimals=0,
         thresholds=approach_thresholds),
    stat("HDOP", flux("nav", "hdop", last=True), 15, 13, 3, 3, decimals=1,
         thresholds=[(None, TILE_OK), (2, "yellow"), (5, "red")]),
    stat("Depth", flux("nav", "depth", last=True), 18, 13, 3, 3,
         unit=UNIT_M, decimals=1),
    stat("Roll / Pitch", flux("nav", "roll", last=True), 21, 13, 3, 3,
         unit="degree", decimals=1),
    # Trends.
    ts("Heading / COG", [target(flux("nav", ["heading", "cog"]))],
       0, 16, 6, 6, unit="degree", min_=0, max_=360),
    ts("Speed Over Ground", [target(flux("nav", "sog"), "SOG")],
       6, 16, 6, 6, unit="velocityknot"),
    ts("Cross-Track Error", [target(flux("nav", "cross_track_error"), "XTE")],
       12, 16, 6, 6, unit=UNIT_M),
    ts("Attitude — Roll / Pitch", [target(flux("nav", ["roll", "pitch"]))],
       18, 16, 6, 6, unit="degree"),
], ["navigation"]))

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
    # A — helm state and the Alpha mission's safety latches (8 x 3x3).
    stat("Helm State", last_str("mission_state", "helm_state"), 0, 0, 3, 3,
         mappings=helm_state_mappings, text_mode="value",
         color_mode="background"),
    stat("All-Stop", last_str("mission_state", "allstop_reason"),
         3, 0, 3, 3, mappings=allstop_mappings, text_mode="value",
         color_mode="background"),
    stat("Station Keeping", flux("autonomy", "stationing", last=True),
         6, 0, 3, 3, mappings=onoff_map("OFF", "ACTIVE"),
         text_mode="value", color_mode="background"),
    stat("Entry Gate", flux("autonomy", "entry_gate_open", last=True),
         9, 0, 3, 3,
         mappings=[{"type": "value", "options": {
             "0": {"text": "CLOSED", "color": "yellow", "index": 0},
             "1": {"text": "OPEN", "color": TILE_OK, "index": 1}}}],
         text_mode="value", color_mode="background"),
    stat("Re-Arm Hold", flux("autonomy", "awaiting_rearm", last=True),
         12, 0, 3, 3, mappings=onoff_map("CLEAR", "AWAITING", "orange"),
         text_mode="value", color_mode="background"),
    stat("Remote Control", flux("autonomy", "remote_control", last=True),
         15, 0, 3, 3, mappings=onoff_map("OFF", "MANUAL", "orange"),
         text_mode="value", color_mode="background"),
    stat("Corridor", last_str("mission_state", "corridor_status"),
         18, 0, 3, 3, mappings=corridor_mappings, text_mode="value",
         color_mode="background"),
    stat("Crash Stop", last_str("mission_state", "crash_stop_status"),
         21, 0, 3, 3, mappings=crash_mappings, text_mode="value",
         color_mode="background"),
    # B — commands and errors.
    stat("Active Behaviors", last_str("mission_state", "active_behavior"),
         0, 3, 6, 3, text_mode="value"),
    stat("Heading Error", flux("autopilot", "heading_error", last=True),
         6, 3, 3, 3, unit="degree", decimals=1,
         thresholds=[(None, OK), (10, "yellow"), (25, "red")],
         graph_mode="area"),
    stat("Speed Error", flux("autopilot", "speed_error", last=True), 9, 3, 3, 3,
         unit="velocityknot", decimals=2,
         thresholds=[(None, OK), (1, "yellow"), (2, "red")],
         graph_mode="area"),
    stat("XTE", flux("nav", "cross_track_error", last=True), 12, 3, 3, 3,
         unit=UNIT_M, decimals=1, graph_mode="area"),
    stat("Dist to Station", flux("mission", "dist_to_station", last=True),
         15, 3, 3, 3, unit=UNIT_M, decimals=0),
    stat("Speed Cap", flux("autopilot", "speed_cap", last=True), 18, 3, 3, 3,
         unit="velocityknot", decimals=1),
    stat("Next Turn", flux("autopilot", "next_turn_angle", last=True),
         21, 3, 3, 3, unit="degree", decimals=0),
    # C — instruments: rudder indicator, throttle, heading error.
    (instrument("Rudder Angle",
                [target(flux("autopilot", "rudder_cmd", last=True))],
                0, 6, 8, 6, rudder_js(),
                {"kind": "rudder", "preview": 3})
     if ANALOG else
     stat("Rudder Angle", flux("autopilot", "rudder_cmd", last=True),
          0, 6, 8, 6, unit="degree", decimals=1)),
    meter("Throttle", flux("autopilot", "throttle_cmd", last=True),
          8, 6, 8, 6, 0, 100, "%", zones=None, decimals=0, split=5,
          sub="COMMANDED THRUST", preview=64,
          fallback_thresholds=[(None, ACCENT)]),
    meter("Heading Error", flux("autopilot", "heading_error", last=True),
          16, 6, 8, 6, -45, 45, "°",
          zones=[(-25, ZRED), (-8, ZYELLOW), (8, ZGREEN), (25, ZYELLOW),
                 (45, ZRED)],
          decimals=1, split=6, sub="OFF COURSE", preview=1.8,
          fallback_thresholds=[(None, OK)]),
    # D — desired vs actual.
    ts("Desired vs Actual Heading",
       [target(flux("autopilot", ["desired_heading", "actual_heading"]))],
       0, 12, 12, 5, unit="degree", min_=0, max_=360),
    ts("Speed Governor — desired / cap / commanded / actual",
       [target(flux("autopilot", ["desired_speed", "speed_cap", "cmd_speed",
                                  "actual_speed"]))],
       12, 12, 12, 5, unit="velocityknot"),
    # E — actuator outputs and errors.
    ts("Rudder Output", [target(flux("autopilot", "rudder_cmd"), "rudder")],
       0, 17, 8, 5, unit="degree"),
    ts("Throttle Output", [target(flux("autopilot", "throttle_cmd"), "throttle")],
       8, 17, 8, 5, unit="percent", min_=0, max_=100),
    ts("Heading & Speed Error",
       [target(flux("autopilot", ["heading_error", "speed_error"]))],
       16, 17, 8, 5),
], ["autopilot"]))

# ---------------------------------------------------------- FR-4 Vessel Health
DASHBOARDS.append(dashboard("usv-health", "Vessel Health", [
    # Instrument cluster: the six primary machinery/electrical meters.
    meter("Tachometer", flux("engine", "rpm", last=True), 0, 0, 4, 6,
          0, 6000, "rpm",
          zones=[(4500, ZGREEN), (5500, ZYELLOW), (6000, ZRED)],
          decimals=0, split=6, sub="×1000 RPM",
          label_js="(x) => (x / 1000).toFixed(0)", preview=2547,
          preview_labels={i * 1000: str(i) for i in range(7)},
          fallback_thresholds=[(None, OK), (4500, "yellow"),
                               (5500, "red")]),
    meter("Coolant", flux("engine", "coolant_temp", last=True), 4, 0, 4, 6,
          40, 120, "°C",
          zones=[(95, ZGREEN), (105, ZYELLOW), (120, ZRED)],
          decimals=0, split=4, sub="TEMP °C", preview=85,
          fallback_thresholds=[(None, OK), (95, "yellow"),
                               (105, "red")]),
    meter("Oil Pressure", flux("engine", "oil_pressure", last=True),
          8, 0, 4, 6, 0, 600, "kPa",
          zones=[(150, ZRED), (250, ZYELLOW), (520, ZGREEN), (600, ZYELLOW)],
          decimals=0, split=6, sub="OIL kPa", preview=402,
          fallback_thresholds=[(None, "red"), (150, "yellow"),
                               (250, OK)]),
    meter("Fuel", flux("engine", "fuel_level", last=True), 12, 0, 4, 6,
          0, 100, "%",
          zones=[(15, ZRED), (30, ZYELLOW), (100, ZGREEN)],
          decimals=0, split=4, sub="FUEL",
          label_js=("(x) => (x === 0 ? 'E' : (x === 50 ? '\\u00bd' : "
                    "(x === 100 ? 'F' : '')))"),
          preview=91, preview_labels={0: "E", 50: "½", 100: "F"},
          fallback_thresholds=[(None, "red"), (15, "yellow"),
                               (30, OK)]),
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
    # Secondary tiles: 8 x 3x3.
    stat("Fuel Rate", flux("engine", "fuel_rate", last=True), 0, 6, 3, 3,
         unit="litreh", decimals=1, graph_mode="area"),
    stat("Engine Hours", flux("engine", "runtime_h", last=True), 3, 6, 3, 3,
         unit="h", decimals=1),
    stat("Oil Temp", flux("engine", "oil_temp", last=True), 6, 6, 3, 3,
         unit="celsius", decimals=0,
         thresholds=[(None, OK), (110, "yellow"), (125, "red")]),
    stat("Engine Load", flux("engine", "percent_load", last=True), 9, 6, 3, 3,
         unit="percent", decimals=0, graph_mode="area"),
    stat("Battery Current", flux("electrical", "battery_current", last=True),
         12, 6, 3, 3, unit="amp", decimals=1, graph_mode="area"),
    stat("Charging", last_str("electrical", "charging_status"),
         15, 6, 3, 3, text_mode="value", color_mode="background",
         mappings=[{"type": "value", "options": {
             "charging": {"text": "CHARGING", "color": TILE_OK, "index": 0},
             "discharging": {"text": "DISCHARGING", "color": "yellow",
                             "index": 1}}}]),
    stat("Alternator", flux("electrical", "alternator_voltage", last=True),
         18, 6, 3, 3, unit="volt", decimals=1),
    stat("Total Power", flux("power", "total_power", last=True), 21, 6, 3, 3,
         unit="watt", decimals=0, graph_mode="area"),
    # Trends: two rows of three.
    ts("Engine RPM", [target(flux("engine", "rpm"), "RPM")], 0, 9, 8, 6,
       unit="rotrpm"),
    ts("Coolant Temp / Oil Pressure",
       [target(flux("engine", "coolant_temp"), "coolant °C"),
        target(flux("engine", "oil_pressure"), "oil kPa", ref="B")],
       8, 9, 8, 6,
       overrides=[{"matcher": {"id": "byFrameRefID", "options": "B"},
                   "properties": [{"id": "unit", "value": "pressurekpa"},
                                  {"id": "custom.axisPlacement",
                                   "value": "right"}]}]),
    ts("Battery Voltage / Current",
       [target(flux("electrical", ["battery_voltage"]), "voltage"),
        target(flux("electrical", ["battery_current"]), "current", ref="B")],
       16, 9, 8, 6, unit="volt",
       overrides=[{"matcher": {"id": "byFrameRefID", "options": "B"},
                   "properties": [{"id": "unit", "value": "amp"},
                                  {"id": "custom.axisPlacement",
                                   "value": "right"}]}]),
    ts("Power / System Voltage",
       [target(flux("power", "total_power"), "power W"),
        target(flux("power", "system_voltage"), "sys V", ref="B")],
       0, 15, 8, 6, unit="watt",
       overrides=[{"matcher": {"id": "byFrameRefID", "options": "B"},
                   "properties": [{"id": "unit", "value": "volt"},
                                  {"id": "custom.axisPlacement",
                                   "value": "right"}]}]),
    ts("State of Charge", [target(flux("electrical", "soc"), "SOC")],
       8, 15, 8, 6, unit="percent", min_=0, max_=100),
    ts("Fuel Level / Consumption",
       [target(flux("engine", "fuel_level"), "level %"),
        target(flux("engine", "fuel_rate"), "rate L/h", ref="B")],
       16, 15, 8, 6, unit="percent",
       overrides=[{"matcher": {"id": "byFrameRefID", "options": "B"},
                   "properties": [{"id": "unit", "value": "litreh"},
                                  {"id": "custom.axisPlacement",
                                   "value": "right"}]}]),
], ["health"]))

# ------------------------------------------------------------- FR-5 CAN Bus
DASHBOARDS.append(dashboard("usv-canbus", "CAN Bus", [
    stat("Interface", flux("canbus", "interface_up", last=True), 0, 0, 4, 4,
         mappings=updown_map("DOWN", "UP"), text_mode="value",
         color_mode="background"),
    gauge("Bus Utilization", flux("canbus", "utilization_pct", last=True),
          4, 0, 4, 4, thresholds=[(None, OK), (50, "yellow"), (75, "red")]),
    stat("Messages / s", flux("canbus", "msg_rate", last=True), 8, 0, 4, 4,
         decimals=0, graph_mode="area"),
    stat("Error Frames (5s)", flux("canbus", "error_frames", last=True),
         12, 0, 4, 4, decimals=0,
         thresholds=[(None, OK), (1, "yellow"), (10, "red")]),
    stat("Dropped Frames", flux("canbus", "dropped_frames", last=True),
         16, 0, 4, 4, decimals=0,
         thresholds=[(None, TILE_OK), (1, "red")], color_mode="background"),
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

# ------------------------------------------------ FR-6 Traffic (collision avoidance)
# Contacts come from grafana_bridge: NODE_REPORT / AIS-style reports and
# radar targets, with range, bearings, CPA / TCPA, encounter type and a
# risk level computed onboard (mission_logic.hpp). Rows are written only
# while a contact is active, so a 30 s window is the live picture.
CONTACT_FIELDS = ["range_m", "bearing", "rel_bearing", "spd_kn", "hdg",
                  "rel_speed_kn", "cpa_m", "tcpa_s", "risk"]
CONTACT_STR_FIELDS = ["encounter", "type"]


def contacts_query(limit=None, rank=True):
    """One row per live contact with every field as a column, riskiest first."""
    fields = " or ".join(f'r._field == "{f}"' for f in CONTACT_FIELDS + CONTACT_STR_FIELDS)
    q = ('from(bucket: "telemetry")\n'
         '  |> range(start: -30s)\n'
         f'  |> filter(fn: (r) => r._measurement == "contacts" and ({fields}))\n'
         '  |> filter(fn: (r) => r.vessel == "${vessel}")\n'
         '  |> last()\n'
         '  |> drop(columns: ["_start", "_stop", "_measurement", "vessel", "_time"])\n'
         '  |> group()\n'
         '  |> pivot(rowKey: ["contact", "source"], columnKey: ["_field"], valueColumn: "_value")\n'
         '  |> filter(fn: (r) => exists r.range_m)\n')
    if rank:
        q += ('  |> sort(columns: ["cpa_m"])\n'
              '  |> sort(columns: ["risk"], desc: true)\n')
    if limit:
        q += f'  |> limit(n: {limit})\n'
    return q


def worst_contact_query(column, string=False):
    """A single value (the riskiest contact's column) for a stat tile."""
    q = contacts_query(limit=1)
    q += f'  |> map(fn: (r) => ({{_value: r.{column}}}))\n'
    return q


def traffic_js():
    """Heading-up polar traffic view: own ship at the centre, range rings
    in nautical miles (auto-scaled 0.5 / 1 / 2 / 5 nm), contacts as
    heading-rotated triangles coloured by source, HIGH-risk contacts red
    with a dashed CPA line to the riskiest one."""
    return ECH_PRELUDE + r"""
const ownHdg = val('B') || 0;
const rows = [];
for (const s of context.panel.data.series) {
  if (s.refId === 'B') { continue; }
  const col = {};
  for (const f of s.fields) { col[f.name] = f.values && f.values.toArray ? f.values.toArray() : f.values; }
  const n = (col.range_m || []).length;
  for (let i = 0; i < n; i++) {
    rows.push({ id: col.contact ? col.contact[i] : ('#' + i),
      source: col.source ? col.source[i] : 'radar',
      range: col.range_m[i], rb: col.rel_bearing ? col.rel_bearing[i] : null,
      hdg: col.hdg ? col.hdg[i] : null, risk: col.risk ? col.risk[i] : 0,
      cpa: col.cpa_m ? col.cpa_m[i] : null, tcpa: col.tcpa_s ? col.tcpa_s[i] : null });
  }
}
const NM = 1852;
let maxR = 0.5;
for (const r of rows) { if (r.rb !== null && r.range / NM > maxR * 0.9) { maxR = r.range / NM > 2 ? 5 : (r.range / NM > 1 ? 2 : 1); } }
if (maxR > 2 && rows.some(r => r.range / NM > 4.5)) { maxR = 10; }
const colorOf = (r) => r.risk >= 2 ? '#ff6b6b' : (r.source === 'report' ? '#5ad8a6' : '#f0b429');
const pts = rows.filter(r => r.rb !== null);
const scatter = pts.map(r => ({
  value: [r.range / NM, ((r.rb % 360) + 360) % 360],
  symbolRotate: r.hdg === null ? 0 : -(r.hdg - ownHdg),
  itemStyle: { color: colorOf(r) },
  label: { show: r.risk >= 1, formatter: r.id, color: dark ? '#fff' : '#24292e', fontSize: 11, fontWeight: 600, position: 'top' },
  name: r.id, risk: r.risk, cpa: r.cpa, tcpa: r.tcpa, source: r.source }));
const worst = pts.slice().sort((a, b) => (b.risk - a.risk) || ((a.cpa ?? 1e9) - (b.cpa ?? 1e9)))[0];
const series = [{
  type: 'scatter', coordinateSystem: 'polar', symbol: 'triangle', symbolSize: 14, data: scatter,
  tooltip: { formatter: (p) => p.data.name + '<br>' + p.data.source + ' · risk ' + ['clear','MEDIUM','HIGH'][p.data.risk] +
    (p.data.cpa != null ? '<br>CPA ' + Math.round(p.data.cpa) + ' m' : '') +
    (p.data.tcpa != null && p.data.tcpa >= 0 ? ' · TCPA ' + Math.round(p.data.tcpa) + ' s' : '') }
}, {
  type: 'scatter', coordinateSystem: 'polar', symbol: 'triangle', symbolSize: 18,
  data: [{ value: [0, 0], itemStyle: { color: '#ffffff', borderColor: '#00bcd4', borderWidth: 2 } }], silent: true, z: 5
}];
if (worst && worst.risk >= 1) {
  series.push({ type: 'line', coordinateSystem: 'polar', silent: true, z: 4, showSymbol: false,
    lineStyle: { color: '#ff6b6b', type: 'dashed', width: 1.5 },
    data: [[0, 0], [worst.range / NM, ((worst.rb % 360) + 360) % 360]] });
}
const fmtT = (t) => t == null || t < 0 ? '—' : Math.floor(t / 60) + ':' + ('0' + Math.round(t % 60)).slice(-2);
return {
  backgroundColor: 'transparent',
  polar: { center: ['50%', '52%'], radius: '78%' },
  angleAxis: { type: 'value', min: 0, max: 360, startAngle: 90, clockwise: true, splitNumber: 12,
    axisLine: { lineStyle: { color: C.tick } }, axisTick: { show: false },
    axisLabel: { color: C.label, fontSize: 10, formatter: (v) => v === 0 ? 'N' : ('00' + v).slice(-3) },
    splitLine: { lineStyle: { color: dark ? 'rgba(124,156,176,0.18)' : 'rgba(36,41,46,0.15)' } } },
  radiusAxis: { min: 0, max: maxR, splitNumber: 4, axisLine: { show: false }, axisTick: { show: false },
    axisLabel: { color: C.label, fontSize: 9, formatter: (v) => v > 0 ? v + ' nm' : '' },
    splitLine: { lineStyle: { color: dark ? 'rgba(124,156,176,0.35)' : 'rgba(36,41,46,0.3)', type: 'dashed' } } },
  tooltip: { trigger: 'item' },
  graphic: [
    { type: 'text', left: 8, top: 4, style: { text: 'HEADING UP · range ' + maxR + ' nm', fill: C.sub, fontSize: 10, fontWeight: 600 } },
    { type: 'text', left: 8, bottom: 6, style: { text: '▲ report / AIS   ▲ radar   ▲ HIGH risk', fill: C.label, fontSize: 10 } },
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
    """Key/value readout tile for the contact and MOOS action cards."""
    return stat(title, query, x, y, w, h, **kw)


DASHBOARDS.append(dashboard("usv-traffic", "Traffic", [
    # A — risk and counts (8 x 3x3)
    stat("Collision Risk", flux("traffic", "risk_level", last=True), 0, 0, 3, 3,
         mappings=risk_mappings, text_mode="value", color_mode="background"),
    stat("Avoidance", flux("traffic", "avoidance_active", last=True), 3, 0, 3, 3,
         mappings=[{"type": "value", "options": {
             "0": {"text": "IDLE", "color": "#3d434b", "index": 0},
             "1": {"text": "ACTIVE", "color": "orange", "index": 1}}}],
         text_mode="value", color_mode="background"),
    stat("Contacts", flux("traffic", "contacts", last=True), 6, 0, 3, 3, decimals=0),
    stat("Reports / AIS", flux("traffic", "reports", last=True), 9, 0, 3, 3, decimals=0),
    stat("Radar Only", flux("traffic", "radar_only", last=True), 12, 0, 3, 3, decimals=0),
    stat("High Risk", flux("traffic", "high_risk", last=True), 15, 0, 3, 3, decimals=0,
         thresholds=[(None, OK), (1, "red")]),
    stat("Closest Range", flux("traffic", "closest_range_m", last=True, nm=True),
         18, 0, 3, 3, unit=UNIT_NM, decimals=2,
         thresholds=[(None, "red"), (0.11, "yellow"), (0.27, OK)]),
    stat("Min CPA", flux("traffic", "min_cpa_m", last=True), 21, 0, 3, 3,
         unit=UNIT_M, decimals=0,
         thresholds=[(None, "red"), (500, "yellow"), (1000, OK)]),
    # B — traffic view | highest-risk contact | MOOS action
    (instrument("Traffic View",
                [target(contacts_query()),
                 target(flux("nav", "heading", last=True), ref="B")],
                0, 3, 10, 13,
                traffic_js(), {"kind": "traffic"})
     if ANALOG else
     panel("table", "Contacts", [target(contacts_query())], 0, 3, 10, 13,
           options={"showHeader": True})),
    stat("Highest-Risk Contact", worst_contact_query("contact"), 10, 3, 7, 2,
         string_value=True, text_mode="value"),
    kv_tile("CPA", worst_contact_query("cpa_m"), 10, 5, 4, 3, unit=UNIT_M, decimals=0,
            thresholds=[(None, "red"), (500, "yellow"), (1000, OK)]),
    kv_tile("TCPA", worst_contact_query("tcpa_s"), 14, 5, 3, 3, unit="dtdurations",
            decimals=0, thresholds=[(None, "red"), (120, "yellow"), (300, OK)],
            mappings=[{"type": "value", "options": {
                "-1": {"text": "OPENING", "color": TILE_OK, "index": 0}}}]),
    kv_tile("Range", worst_contact_query("range_m"), 10, 8, 4, 3, unit=UNIT_M, decimals=0),
    kv_tile("Bearing", worst_contact_query("bearing"), 14, 8, 3, 3, unit="degree", decimals=0),
    kv_tile("Rel. Bearing", worst_contact_query("rel_bearing"), 10, 11, 4, 3,
            unit="degree", decimals=0),
    kv_tile("Rel. Speed", worst_contact_query("rel_speed_kn"), 14, 11, 3, 3,
            unit="velocityknot", decimals=1),
    stat("Type", worst_contact_query("type"), 10, 14, 4, 2, string_value=True,
         text_mode="value"),
    stat("Encounter", worst_contact_query("encounter"), 14, 14, 3, 2, string_value=True,
         text_mode="value", mappings=encounter_mappings, color_mode="background"),
    kv_tile("Planned Course", flux("traffic", "planned_course", last=True), 17, 3, 4, 3,
            unit="degree", decimals=0),
    kv_tile("Commanded Course", flux("autopilot", "desired_heading", last=True), 21, 3, 3, 3,
            unit="degree", decimals=0),
    kv_tile("Actual Heading", flux("nav", "heading", last=True), 17, 6, 4, 3,
            unit="degree", decimals=0),
    kv_tile("Deviation", flux("traffic", "course_deviation", last=True), 21, 6, 3, 3,
            unit="degree", decimals=0,
            thresholds=[(None, OK), (10, "yellow"), (30, "orange")]),
    kv_tile("Speed Command", flux("autopilot", "desired_speed", last=True), 17, 9, 4, 3,
            unit="velocityknot", decimals=1),
    kv_tile("Actual SOG", flux("nav", "sog", last=True), 21, 9, 3, 3,
            unit="velocityknot", decimals=1),
    stat("Avoidance Behavior", last_str("mission_state", "active_behavior"), 17, 12, 7, 2,
         text_mode="value"),
    stat("Reason", last_str("traffic", "reason"), 17, 14, 7, 2, text_mode="value",
         string_value=True),
    # C — contact table and CPA / TCPA trend
    panel("table", "Contacts — riskiest first", [target(contacts_query(limit=12))],
          0, 16, 12, 5, options={"showHeader": True},
          mappings=risk_mappings,
          overrides=[{"matcher": {"id": "byName", "options": "range_m"},
                      "properties": [{"id": "unit", "value": UNIT_M}, {"id": "decimals", "value": 0},
                                     {"id": "displayName", "value": "Range"}]},
                     {"matcher": {"id": "byName", "options": "cpa_m"},
                      "properties": [{"id": "unit", "value": UNIT_M}, {"id": "decimals", "value": 0},
                                     {"id": "displayName", "value": "CPA"}]},
                     {"matcher": {"id": "byName", "options": "tcpa_s"},
                      "properties": [{"id": "unit", "value": "s"}, {"id": "decimals", "value": 0},
                                     {"id": "displayName", "value": "TCPA"}]},
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
    ts("Closest CPA / TCPA",
       [target(flux("traffic", "min_cpa_m"), "CPA m"),
        target(flux("traffic", "min_tcpa_s"), "TCPA s", ref="B")],
       12, 16, 12, 5, unit=UNIT_M,
       overrides=[{"matcher": {"id": "byFrameRefID", "options": "B"},
                   "properties": [{"id": "unit", "value": "s"},
                                  {"id": "custom.axisPlacement", "value": "right"}]}]),
], ["traffic"]))

# --------------------------------------------------------------- FR-7 Camera
DASHBOARDS.append(dashboard("usv-camera", "Cameras", [
    stat("Connection", flux("camera", "connected", last=True), 0, 0, 4, 4,
         mappings=updown_map("DISCONNECTED", "CONNECTED"), text_mode="value",
         color_mode="background"),
    stat("Live Stream", flux("camera", "stream_up", last=True), 4, 0, 4, 4,
         mappings=updown_map("UNAVAILABLE", "AVAILABLE"), text_mode="value",
         color_mode="background"),
    stat("Recording", flux("camera", "recording", last=True), 8, 0, 4, 4,
         mappings=updown_map("STOPPED", "RECORDING"), text_mode="value",
         color_mode="background"),
    stat("Frame Rate", flux("camera", "fps", last=True), 12, 0, 4, 4,
         unit="fps" if True else None, decimals=1,
         thresholds=[(None, "red"), (15, "yellow"), (24, OK)]),
    stat("Bitrate", flux("camera", "bitrate_kbps", last=True), 16, 0, 4, 4,
         unit="Kbits", decimals=0),
    stat("Camera Temp", flux("camera", "temp_c", last=True), 20, 0, 4, 4,
         unit="celsius", decimals=1,
         thresholds=[(None, OK), (60, "yellow"), (75, "red")]),
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
# Telegraf tags that pick ONE series per host metric; the rest (per-core
# cpu, loop/device-mapper block devices, the loopback interface, non-root
# mounts) would otherwise stack as extra series or a "No data" split.
TELE_SELECT = {
    "cpu": '  |> filter(fn: (r) => r.cpu == "cpu-total")\n',
    "disk": '  |> filter(fn: (r) => r.path == "/")\n',
    "diskio": '  |> filter(fn: (r) => r.name !~ /^(loop|dm-|sr)/)\n',
    "net": '  |> filter(fn: (r) => r.interface != "lo")\n',
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


def tele_last(measurement, field, extra=""):
    return tele(measurement, field, extra=extra).replace(AGG, LAST)



# Link status comes from Telegraf host probes (telegraf.conf), not from the
# bench simulator: Internet = ICMP to 8.8.8.8, VPN = tailscaled.service,
# Starlink = ICMP to the dish router (input enabled on vessels only; the
# tile reads N/A where it is not fitted).
PING_INTERNET = '  |> filter(fn: (r) => r.url == "8.8.8.8")\n'
PING_STARLINK = '  |> filter(fn: (r) => r.url == "192.168.100.1")\n'
UNIT_SERVICE = '  |> filter(fn: (r) => r.name == "tailscaled.service")\n'
BYTES_TO_BITS = '  |> map(fn: (r) => ({r with _value: r._value * 8.0}))\n'


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
    probe_tile("VPN / Tailscale",
               tele_last("systemd_units", "active_code", extra=UNIT_SERVICE), 8),
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
    ts("Bandwidth (host interfaces)",
       [target(tele("net", "bytes_recv", rate=True) + BYTES_TO_BITS, "down"),
        target(tele("net", "bytes_sent", rate=True) + BYTES_TO_BITS, "up", ref="B")],
       0, 11, 12, 7, unit="bps"),
    ts("Telemetry Message Rates",
       [target(flux("comms", ["moos_msg_rate", "signalk_msg_rate"]))],
       12, 11, 12, 7),
], ["comms"]))

# ---------------------------------------------------------- FR-9 System Health


def service_tile(title, service, x):
    return stat(title, tele_last("net_response", "result_code",
                                 extra=f'  |> filter(fn: (r) => r.service == "{service}")'),
                x, 0, 3, 4, text_mode="value", color_mode="background",
                mappings=[{"type": "value", "options": {
                    "0": {"text": "UP", "color": TILE_OK, "index": 0}}},
                    {"type": "range", "options": {
                        "from": 1, "to": 99,
                        "result": {"text": "DOWN", "color": "red", "index": 1}}}])


# Single screen: 8 equal tiles (CPU / GPU / RAM / Disk gauges, CPU temp,
# InfluxDB / Signal K / MOOS reachability), six trend charts, service table. GPU utilisation comes
# from Telegraf's nvidia_smi input (see telegraf.conf); the tile reads
# "No data" on hosts without a supported GPU.
DASHBOARDS.append(dashboard("usv-system", "System", [
    gauge("CPU", tele_last("cpu", "usage_user"), 0, 0, 3, 4,
          thresholds=[(None, OK), (70, "yellow"), (90, "red")]),
    gauge("GPU", tele_last("nvidia_smi", "utilization_gpu"), 3, 0, 3, 4,
          thresholds=[(None, OK), (70, "yellow"), (90, "red")]),
    gauge("RAM", tele_last("mem", "used_percent"), 6, 0, 3, 4,
          thresholds=[(None, OK), (75, "yellow"), (90, "red")]),
    gauge("Disk", tele_last("disk", "used_percent"), 9, 0, 3, 4,
          thresholds=[(None, OK), (75, "yellow"), (90, "red")]),
    stat("CPU Temp", tele_last("temp", "temp"), 12, 0, 3, 4, unit="celsius",
         decimals=0, thresholds=[(None, OK), (70, "yellow"), (85, "red")]),
    service_tile("InfluxDB", "influxdb", 15),
    service_tile("Signal K", "signalk", 18),
    service_tile("MOOS", "moos", 21),
    ts("CPU", [target(tele("cpu", "usage_user"), "user"),
               target(tele("cpu", "usage_system"), "system", ref="B")],
       0, 4, 8, 6, unit="percent", min_=0, max_=100),
    ts("GPU", [target(tele("nvidia_smi", "utilization_gpu"), "util %"),
               target(tele("nvidia_smi", "temperature_gpu"), "temp °C", ref="B")],
       8, 4, 8, 6, unit="percent", min_=0, max_=100,
       overrides=[{"matcher": {"id": "byFrameRefID", "options": "B"},
                   "properties": [{"id": "unit", "value": "celsius"},
                                  {"id": "custom.axisPlacement",
                                   "value": "right"}]}]),
    ts("Memory", [target(tele("mem", "used_percent"), "used %")],
       16, 4, 8, 6, unit="percent", min_=0, max_=100),
    ts("Disk I/O", [target(tele("diskio", "read_bytes", rate=True), "read"),
                    target(tele("diskio", "write_bytes", rate=True),
                           "write", ref="B")],
       0, 10, 8, 6, unit="Bps"),
    ts("Load Average", [target(tele("system", "load1"), "1 min"),
                        target(tele("system", "load5"), "5 min", ref="B"),
                        target(tele("system", "load15"), "15 min", ref="C")],
       8, 10, 8, 6),
    ts("CPU Temp", [target(tele("temp", "temp"), "°C")],
       16, 10, 8, 6, unit="celsius"),
    panel("table", "Services (systemd)",
          [target('from(bucket: "telemetry")\n'
                  '  |> range(start: -1m)\n'
                  '  |> filter(fn: (r) => r._measurement == "systemd_units" '
                  'and r._field == "active_code")\n'
                  '  |> last()\n'
                  '  |> keep(columns: ["name", "active", "sub", "_value"])\n')],
          0, 16, 24, 5, options={"showHeader": True},
          mappings=[{"type": "value", "options": {
              "0": {"text": "active", "color": OK, "index": 0}}},
              {"type": "range", "options": {
                  "from": 1, "to": 99,
                  "result": {"text": "not active", "color": "red",
                             "index": 1}}}]),
], ["system"]))

# -------------------------------------------------------- FR-10 Mission Replay
DASHBOARDS.append(dashboard("usv-replay", "Replay", [
    panel("text", "How to replay",
          [], 0, 0, 24, 2,
          options={"mode": "markdown", "content": (
              "Set the **time range** (top right) to the mission window, then "
              "scrub with the shared crosshair. Data older than the raw "
              "retention (30 d) is served from the 1-minute downsampled "
              "bucket. Use the **Mission events** annotations to locate "
              "waypoint hits and state changes.")}),
    panel("geomap", "GPS Track",
          [target(track_query("10s", archive=True)), target(position_query(), ref="B"),
           target(geometry_query("route"), ref="C"),
           target(geometry_query("region"), ref="D")],
          0, 2, 14, 9,
          options={"view": {"id": "fit", "zoom": 15},
                   "basemap": {"type": "osm-standard", "name": "OSM"},
                   "layers": overlay_layers()}),
    panel("state-timeline", "Mission State",
          [target(flux("mission_state", "state", every="30s", fn="last"))],
          14, 2, 10, 4, mappings=mission_state_mappings,
          custom={"fillOpacity": 70, "lineWidth": 0},
          options={"showValue": "auto"}),
    panel("table", "Mission Events",
          [target('from(bucket: "events")\n'
                  '  |> range(start: v.timeRangeStart, stop: v.timeRangeStop)\n'
                  '  |> filter(fn: (r) => r._measurement == "events" and r._field == "message")\n'
                  '  |> filter(fn: (r) => r.vessel == "${vessel}")\n'
                  '  |> group()\n'
                  '  |> keep(columns: ["_time", "_value", "type", "severity"])\n'
                  '  |> rename(columns: {_time: "Time", _value: "Event", type: "Type", severity: "Severity"})\n'
                  '  |> sort(columns: ["Time"], desc: true)\n')],
          14, 6, 10, 5, options={"showHeader": True}),
    ts("Heading History", [target(flux("nav", "heading", archive=True), "heading")],
       0, 11, 8, 5, unit="degree", min_=0, max_=360),
    ts("Speed History", [target(flux("nav", "sog", archive=True), "SOG")],
       8, 11, 8, 5, unit="velocityknot"),
    ts("Actuator Commands",
       [target(flux("autopilot", "rudder_cmd", archive=True), "rudder °"),
        target(flux("autopilot", "throttle_cmd", archive=True), "throttle %", ref="B")],
       16, 11, 8, 5, unit="degree",
       overrides=[{"matcher": {"id": "byFrameRefID", "options": "B"},
                   "properties": [{"id": "unit", "value": "percent"},
                                  {"id": "custom.axisPlacement",
                                   "value": "right"}]}]),
    ts("Cross-Track Error", [target(flux("nav", "cross_track_error", archive=True), "XTE")],
       0, 16, 8, 5, unit=UNIT_M),
    panel("state-timeline", "Autonomy Flags",
          [target(flux("autonomy",
                       ["waypoint", "pause", "stationing", "returning",
                        "armed", "comms_lost", "manual_override"],
                       every="30s", fn="last"))],
          8, 16, 8, 5, mappings=flag_timeline_mappings,
          custom={"fillOpacity": 70, "lineWidth": 0},
          options={"showValue": "never", "legend": {"showLegend": False},
                   "tooltip": {"mode": "single"}}),
    ts("Distance to Next Waypoint",
       [target(flux("mission", "dist_to_waypoint", archive=True), "distance m")],
       16, 16, 8, 5, unit=UNIT_M),
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
    fleet_link(stat("Operator Link", fleet_last("autonomy", "comms_phase"),
                    10, 11, 3, 3, mappings=comms_phase_mappings,
                    text_mode="value", color_mode="background")),
    fleet_link(stat("Geofence", fleet_last("autonomy", "geofence_breached"),
                    13, 11, 3, 3, mappings=geofence_mappings,
                    text_mode="value", color_mode="background")),
    fleet_link(stat("SOG", fleet_last("nav", "sog"), 16, 11, 2, 3,
                    unit="velocityknot", decimals=1)),
    fleet_link(stat("Progress", fleet_last("mission", "progress_pct"),
                    18, 11, 2, 3, unit="percent", decimals=0)),
    fleet_link(stat("Signal K", fleet_last("comms", "signalk_ws_up"),
                    20, 11, 2, 3, mappings=updown_map("DOWN", "UP"),
                    text_mode="value", color_mode="background")),
    fleet_link(stat("MOOS", fleet_last("comms", "moos_connected"),
                    22, 11, 2, 3, mappings=updown_map("DOWN", "UP"),
                    text_mode="value", color_mode="background")),
]
DASHBOARDS.append(dashboard("usv-fleet", "Fleet", fleet_panels,
                            ["fleet"], fleet=True))

# ------------------------------------------------------------- FR-11 Alarms
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
         unit="percent", decimals=0,
         thresholds=[(None, "red"), (25, "yellow"), (50, TILE_OK)], color_mode="background"),
    stat("GPS Sats", flux("nav", "satellites", last=True), 4, 10, 4, 4,
         decimals=0, thresholds=[(None, "red"), (5, "yellow"), (8, TILE_OK)], color_mode="background"),
    stat("CAN Bus", flux("canbus", "interface_up", last=True), 8, 10, 4, 4,
         mappings=updown_map("FAIL", "OK"), text_mode="value", color_mode="background"),
    stat("Coolant", flux("engine", "coolant_temp", last=True), 12, 10, 4, 4,
         unit="celsius", decimals=0,
         thresholds=[(None, TILE_OK), (95, "yellow"), (105, "red")], color_mode="background"),
    stat("Fuel", flux("engine", "fuel_level", last=True), 16, 10, 4, 4,
         unit="percent", decimals=0,
         thresholds=[(None, "red"), (15, "yellow"), (30, TILE_OK)], color_mode="background"),
    stat("Radar", flux("radar", "online", last=True), 20, 10, 4, 4,
         mappings=updown_map(), text_mode="value", color_mode="background"),
    stat("Camera", flux("camera", "connected", last=True), 0, 14, 4, 4,
         mappings=updown_map(), text_mode="value", color_mode="background"),
    stat("Signal K", flux("comms", "signalk_ws_up", last=True), 4, 14, 4, 4,
         mappings=updown_map("DISCONNECTED", "CONNECTED"), text_mode="value", color_mode="background"),
    stat("MOOS Rate", flux("comms", "moos_msg_rate", last=True), 8, 14, 4, 4,
         decimals=0, thresholds=[(None, "red"), (1, "yellow"), (10, TILE_OK)], color_mode="background"),
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
         mappings=mission_state_mappings, text_mode="value", color_mode="background"),
    stat("Operator Link", flux("autonomy", "comms_lost", last=True),
         4, 18, 4, 4, mappings=alert_map("OK", "FAILSAFE"),
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


# ------------------------------------------------------------- header band
# Swiftships wordmark + dashboard name + selected vessel across the top of
# every dashboard (also visible in ?kiosk mode, where Grafana's own chrome is
# hidden). The image is served from Grafana's public/img, put there by
# deploy/brand.sh, so it works offline. The white chip renders on both
# themes without any styling.
BRAND_H = 2


def brand_panel(title):
    # Markdown mode: Grafana 13's HTML sanitizer strips <img>, Markdown images
    # are supported. The chip is pre-sized (44 px high) for the band.
    return {
        "id": nid(), "type": "text", "title": "", "transparent": True,
        "gridPos": {"x": 0, "y": 0, "w": 24, "h": BRAND_H},
        "options": {"mode": "markdown", "content": (
            f"![Swiftships](/public/img/swiftships_wordmark_chip_sm.png) "
            f"&nbsp;&nbsp; **{title.upper()}** &nbsp;&nbsp;&middot;&nbsp;&nbsp; "
            f"Vessel **$vessel**")},
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
    d["panels"].insert(0, brand_panel(d["title"]))
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


def main():
    os.makedirs(OUT, exist_ok=True)
    for d in DASHBOARDS:
        add_header_band(d)
        if d["uid"] == "usv-fleet":
            use_fleet_variable(d)
        path = os.path.join(OUT, f"{d['uid']}.json")
        with open(path, "w") as f:
            json.dump(d, f, indent=2)
        print(f"wrote {path}  ({len(d['panels'])} panels)")


if __name__ == "__main__":
    main()
