#!/usr/bin/env python3
"""Render pixel-faithful Grafana-dark previews of the generated dashboards.

Reads the real dashboard JSON (layout, titles, panel types, units) and emits
a self-contained HTML page per dashboard with plausible simulator-like data,
for screenshotting. Preview only — the deployed product is Grafana itself.
"""
import json
import math
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DASH = os.path.join(HERE, "..", "grafana", "dashboards")
OUT = os.path.join(HERE, "..", "preview")

W = 1600          # set per render() call (PC 1600, tablet 1024)
COL = W / 24.0
ROW = 38          # grid row unit incl. gap
GAP = 8

THEMES = {
    "dark": dict(BG="#111217", PANEL="#181b1f", BORDER="#22252b",
                 TEXT="#ccccdc", DIM="#9da0a8", FAINT="#7b7f87",
                 GRID="#212327", GREEN="#73bf69", YELLOW="#fade2a",
                 RED="#f2495c", WATER="#0d1720", LAND="#1c2a20",
                 MAPGRID="#16202a", OK="#175d8d", CYAN="#00bcd4"),
    "light": dict(BG="#f4f5f6", PANEL="#ffffff", BORDER="#d5d9de",
                  TEXT="#24292e", DIM="#57606a", FAINT="#8b949e",
                  GRID="#e8eaed", GREEN="#1f8a3b", YELLOW="#b58b00",
                  RED="#c62f42", WATER="#dbe9f2", LAND="#dde9d5",
                  MAPGRID="#c9d6df", OK="#1e6692", CYAN="#0f7c9c"),
}
# Applied per render() call:
BG = PANEL = BORDER = TEXT = DIM = FAINT = GRID = GREEN = YELLOW = RED = OK = CYAN = ""
WATER = LAND = MAPGRID = ""
# Series palette: validated for both light and dark surfaces
TEAL, BLUE, AMBER, PURPLE = "#00bcd4", "#42a5f5", "#c47b06", "#9256b8"
SERIES = [TEAL, BLUE, AMBER, PURPLE]

rng = random.Random(42)


def C(v):
    """Resolve a theme color name ('GREEN') or pass a literal '#hex' through."""
    return globals().get(v, v) if isinstance(v, str) else v


def walk(n=140, lo=0.0, hi=1.0, start=None, drift=0.02, noise=0.05):
    v = start if start is not None else (lo + hi) / 2
    out = []
    for _ in range(n):
        v += rng.uniform(-1, 1) * (hi - lo) * noise + drift * (hi - lo) / n
        v = max(lo, min(hi, v))
        out.append(v)
    return out


def ramp(n=140, lo=0, hi=100, jitter=0.5):
    return [min(hi, lo + (hi - lo) * i / (n - 1) + rng.uniform(-jitter, jitter))
            for i in range(n)]


def saw(n=140, lo=0, hi=2000, teeth=3):
    out = []
    for i in range(n):
        f = (i * teeth / n) % 1.0
        out.append(hi - (hi - lo) * f + rng.uniform(-hi * 0.01, hi * 0.01))
    return out


# ---- per-panel data: matched by (dashboard uid, panel title) or title only
def series_for(uid, title):
    t = title.lower()
    if "progress over" in t:
        return [("progress %", ramp(lo=42, hi=62))]
    if "approach profile" in t:
        # Distance saw-tooth with the matching governor staircase:
        # cruise > 500 m, tier-1 to 100 m, tier-2 below (alpha.bhv).
        d = saw(lo=30, hi=1800)
        sog = []
        v = 16.0
        for dist in d:
            tgt = 16.0 if dist > 500 else (9.0 if dist > 100 else 4.0)
            v += max(-1.2, min(0.8, tgt - v)) + rng.uniform(-0.15, 0.15)
            sog.append(max(0.0, v))
        # Right-axis series approximated by scaling onto the meter axis
        # (the real dashboard uses a proper second axis).
        return [("distance m", d), ("SOG kn", [s * 100 for s in sog])]
    if "distance to" in t or "next waypoint" in t:
        return [("distance m", saw(lo=30, hi=1800))]
    if "desired vs actual heading" in t:
        a = walk(lo=30, hi=90, start=55, noise=0.04)
        return [("desired_heading", [x + rng.uniform(-2, 2) for x in a]),
                ("actual_heading", a)]
    if "desired vs actual speed" in t:
        a = walk(lo=4.1, hi=4.9, start=4.5, noise=0.06)
        return [("desired_speed", [4.5] * 140), ("actual_speed", a)]
    if "heading / cog" in t:
        a = walk(lo=30, hi=90, start=55, noise=0.04)
        return [("heading", a), ("cog", [x + rng.uniform(-3, 3) for x in a])]
    if "speed over ground" in t or t == "speed history":
        return [("SOG", walk(lo=4.1, hi=4.9, start=4.5, noise=0.06))]
    if "attitude" in t:
        return [("roll", walk(lo=-4, hi=4, start=0, noise=0.2)),
                ("pitch", walk(lo=-1, hi=3, start=1, noise=0.15))]
    if "cross-track" in t:
        return [("XTE", walk(lo=-8, hi=8, start=0, noise=0.15))]
    if "rudder" in t:
        return [("rudder", walk(lo=-14, hi=14, start=0, noise=0.2))]
    if "throttle" in t:
        return [("throttle", walk(lo=55, hi=75, start=64, noise=0.1))]
    if "pid error" in t:
        return [("heading_error", walk(lo=-8, hi=8, start=0, noise=0.2)),
                ("speed_error", walk(lo=-0.5, hi=0.5, start=0, noise=0.2))]
    if "pid controller" in t:
        return [("pid_heading_out", walk(lo=-0.5, hi=0.5, start=0, noise=0.2)),
                ("pid_speed_out", walk(lo=0.5, hi=0.8, start=0.64, noise=0.08))]
    if "engine rpm" in t:
        return [("RPM", walk(lo=2350, hi=2750, start=2550, noise=0.07))]
    if "coolant" in t and "oil" in t:
        return [("coolant °C", walk(lo=82, hi=88, start=85, noise=0.05)),
                ("oil kPa", walk(lo=390, hi=410, start=400, noise=0.05))]
    if "battery voltage / current" in t:
        return [("voltage", walk(lo=12.8, hi=13.0, start=12.9, noise=0.05)),
                ("current", walk(lo=-10, hi=8, start=5, noise=0.1))]
    if "power consumption" in t:
        return [("power W", walk(lo=220, hi=265, start=240, noise=0.08)),
                ("sys V", walk(lo=12.8, hi=13.0, start=12.9, noise=0.04))]
    if "state of charge" in t:
        return [("SOC", [88 - i * 0.008 + rng.uniform(-0.05, 0.05)
                          for i in range(140)])]
    if "fuel level" in t:
        return [("level %", [91.4 - i * 0.004 for i in range(140)]),
                ("rate L/h", walk(lo=13, hi=16, start=14.5, noise=0.08))]
    if "message rate" in t:
        return [("msgs/s", walk(lo=350, hi=410, start=380, noise=0.1))]
    if "utilization" in t:
        return [("util %", walk(lo=30, hi=38, start=34, noise=0.1))]
    if "heading history" in t:
        return [("heading", walk(lo=30, hi=90, start=55, noise=0.04))]
    return [("value", walk(lo=0, hi=1, start=0.5, noise=0.1))]


STATS = {
    "Mission Name": ("ALPHA-SURVEY", "TEXT"), "Mission ID": ("M-2026-042", "TEXT"),
    "Mission State": ("Autonomous", "OK"), "Elapsed Time": ("47.2 min", "CYAN"),
    "ETA to Waypoint": ("5.9 min", "CYAN"), "Current Waypoint": ("4", "CYAN"),
    "Waypoints Total": ("5", "CYAN"), "Distance to Waypoint": ("812 m", "CYAN"),
    "Distance to Next Waypoint": ("812 m", "CYAN"),
    "Distance Traveled": ("8.42 km", "CYAN"),
    "Helm": ("DRIVE", "OK"), "Helm State": ("DRIVE", "OK"),
    "Mission Armed": ("ARMED", "OK"), "Operator Link": ("LINK OK", "OK"),
    "Geofence": ("INSIDE", "OK"), "Obstacle Alerts": ("0", "OK"),
    "Obstacles": ("0", "OK"),
    "Armed": ("ARMED", "OK"), "All-Stop": ("CLEAR", "OK"),
    "Mission": ("alpha-sim", "TEXT"), "Route ID": ("R-2026-017", "TEXT"),
    "Elapsed": ("47m 12s", "CYAN"), "ETA to WP": ("5m 54s", "CYAN"),
    "Waypoint": ("4 of 5", "CYAN"), "Remaining": ("1", "CYAN"),
    "Arrival": ("cruising", "OK"),
    "XTE": ("2.1 m", "CYAN"), "HDOP": ("0.8", "OK"), "Depth": ("6.4 m", "CYAN"),
    "Roll / Pitch": ("1.2°", "CYAN"),
    "Dist to WP": ("812 m", "CYAN"), "Traveled": ("8420 m", "CYAN"),
    "Helm All-Stop": ("CLEAR", "OK"), "All-Stop Reason": ("CLEAR", "OK"),
    "Survey Entry Gate": ("OPEN", "OK"), "Re-Arm Hold": ("—", "#3d434b"),
    "Active Behaviors": ("waypt_survey, opr24", "TEXT"),
    "Station Keeping": ("OFF", "#3d434b"),
    "Latitude": ("29.618452", "CYAN"), "Longitude": ("-91.207310", "CYAN"),
    "SOG": ("4.5 kn", "CYAN"), "COG": ("57°", "CYAN"), "Heading": ("55°", "CYAN"),
    "GPS Fix": ("RTK Fixed", "OK"), "Satellites": ("12", "OK"),
    "Cross-Track Error": ("2.1 m", "CYAN"),
    "Active Behavior": ("waypt_survey", "TEXT"),
    "Heading Error": ("1.8°", "CYAN"), "Speed Error": ("0.04 kn", "CYAN"),
    "Rudder Cmd": ("3.2°", "CYAN"), "Throttle Cmd": ("64%", "CYAN"),
    "Engine RPM": ("2547 rpm", "CYAN"), "Coolant Temp": ("85.2 °C", "CYAN"),
    "Oil Pressure": ("402 kPa", "CYAN"), "Fuel Rate": ("14.4 L/h", "CYAN"),
    "Engine Runtime": ("512.6 h", "CYAN"),
    "Battery Voltage": ("12.91 V", "CYAN"), "Battery Current": ("5.2 A", "CYAN"),
    "Charging Status": ("CHARGING", "OK"), "Alternator Voltage": ("14.2 V", "CYAN"),
    "Total Power": ("243 W", "CYAN"),
    "Interface": ("UP", "OK"), "Messages / s": ("382", "CYAN"),
    "Error Frames (5s)": ("0", "CYAN"), "Dropped Frames": ("0", "OK"),
    "Active PGNs": ("27", "CYAN"),
    "Radar Status": ("ONLINE", "OK"), "Tracked Targets": ("3", "CYAN"),
    "Closest Target": ("1240 m", "CYAN"), "CPA": ("980 m", "CYAN"),
    "TCPA": ("410 s", "CYAN"),
    "Internet": ("ONLINE", "OK"), "Starlink": ("ONLINE", "OK"),
    "VPN / Tailscale": ("ONLINE", "OK"), "Signal K WebSocket": ("ONLINE", "OK"),
    "Latency": ("46 ms", "CYAN"), "Packet Loss": ("0.3%", "CYAN"),
}
GAUGES = {
    "Mission Progress": (62, TEAL), "Fuel Level": (91, "OK"),
    "State of Charge": (87, "OK"), "Bus Utilization": (34, "OK"),
    "CPU": (23, "OK"), "Memory": (41, "OK"), "Disk": (37, "OK"),
}
EVENTS = [
    ("14:29:44", "Mission state: Autonomous", "state_change", "info"),
    ("14:29:40", "Mission re-armed by operator (AUTONOMOUS_RESUME)",
     "mission", "info"),
    ("14:27:02", "Operator link restored — survey held until operator re-arm",
     "alarm", "info"),
    ("14:25:31", "Operator link LOST — failsafe 'loiter' engaged, mission "
     "disarmed", "alarm", "critical"),
    ("14:19:03", "Waypoint 3 captured — heading to 4 of 5", "waypoint",
     "info"),
    ("14:12:44", "Geofence clear — inside region", "geofence", "info"),
    ("14:12:12", "Geofence BREACHED — outside operating region", "geofence",
     "critical"),
    ("14:04:12", "Waypoint 2 captured — heading to 3 of 5", "waypoint",
     "info"),
    ("13:51:30", "Waypoint 1 captured — heading to 2 of 5", "waypoint",
     "info"),
    ("13:45:02", "Survey started (5 waypoints)", "mission", "info"),
]


_grad_id = [0]


def svg_line_chart(series, w, h, unit=""):
    pad_l, pad_r, pad_t, pad_b = 46, 10, 8, 34
    cw, ch = w - pad_l - pad_r, h - pad_t - pad_b
    allv = [v for _, d in series for v in d]
    lo, hi = min(allv), max(allv)
    if hi - lo < 1e-9:
        hi = lo + 1
    span = hi - lo
    nonneg = lo >= 0
    lo -= span * 0.08
    hi += span * 0.08
    if nonneg:
        lo = max(0.0, lo)
    parts = [f'<svg width="{w}" height="{h}">']
    for g in range(5):
        y = pad_t + ch * g / 4
        val = hi - (hi - lo) * g / 4
        parts.append(f'<line x1="{pad_l}" y1="{y:.1f}" x2="{w-pad_r}" '
                     f'y2="{y:.1f}" stroke="{GRID}" stroke-width="1"/>')
        parts.append(f'<text x="{pad_l-6}" y="{y+4:.1f}" fill="{FAINT}" '
                     f'font-size="11" text-anchor="end">{val:.4g}</text>')
    times = ["14:00", "14:10", "14:20", "14:30"]
    for i, tl in enumerate(times):
        x = pad_l + cw * (i + 0.5) / len(times)
        parts.append(f'<text x="{x:.0f}" y="{h-18}" fill="{FAINT}" '
                     f'font-size="11" text-anchor="middle">{tl}</text>')
    for si, (name, data) in enumerate(series):
        color = SERIES[si % len(SERIES)]
        pts = []
        n = len(data)
        for i, v in enumerate(data):
            x = pad_l + cw * i / (n - 1)
            y = pad_t + ch * (1 - (v - lo) / (hi - lo))
            pts.append(f"{x:.1f},{y:.1f}")
        path = " ".join(pts)
        first_x, last_x = pad_l, pad_l + cw
        base_y = pad_t + ch
        _grad_id[0] += 1
        gid = f"ag{_grad_id[0]}"
        parts.append(
            f'<defs><linearGradient id="{gid}" x1="0" y1="0" x2="0" y2="1">'
            f'<stop offset="0%" stop-color="{color}" stop-opacity="0.34"/>'
            f'<stop offset="100%" stop-color="{color}" stop-opacity="0.02"/>'
            f'</linearGradient></defs>')
        parts.append(f'<polygon points="{first_x},{base_y} {path} '
                     f'{last_x},{base_y}" fill="url(#{gid})"/>')
        parts.append(f'<polyline points="{path}" fill="none" '
                     f'stroke="{color}" stroke-width="2" '
                     f'stroke-linejoin="round"/>')
    lx = pad_l
    for si, (name, _) in enumerate(series):
        color = SERIES[si % len(SERIES)]
        parts.append(f'<rect x="{lx}" y="{h-10}" width="14" height="3" '
                     f'rx="1.5" fill="{color}"/>')
        parts.append(f'<text x="{lx+18}" y="{h-6}" fill="{DIM}" '
                     f'font-size="11">{name}</text>')
        lx += 18 + 7 * len(name) + 22
    parts.append("</svg>")
    return "".join(parts)


def svg_gauge(pct, color, w, h):
    r = min(w, h * 2) * 0.33
    cx, cy = w / 2, h * 0.62
    a0, a1 = math.pi * 1.25, math.pi * -0.25
    def arc(frac_from, frac_to, col, sw):
        b0 = a0 + (a1 - a0) * frac_from
        b1 = a0 + (a1 - a0) * frac_to
        x0, y0 = cx + r * math.cos(b0), cy - r * math.sin(b0)
        x1, y1 = cx + r * math.cos(b1), cy - r * math.sin(b1)
        large = 1 if abs(b1 - b0) > math.pi else 0
        return (f'<path d="M {x0:.1f} {y0:.1f} A {r:.1f} {r:.1f} 0 {large} 1 '
                f'{x1:.1f} {y1:.1f}" fill="none" stroke="{col}" '
                f'stroke-width="{sw}" stroke-linecap="round"/>')
    return (f'<svg width="{w}" height="{h}">'
            + arc(0, 1, "#2c3235", 10) + arc(0, pct / 100, color, 10)
            + f'<text x="{cx}" y="{cy+6}" fill="{color}" font-size="26" '
            f'font-weight="600" text-anchor="middle">{pct}%</text></svg>')


def svg_map(w, h):
    p = [f'<svg width="{w}" height="{h}" style="background:{WATER}">']
    p.append(f'<rect x="0" y="0" width="{w}" height="{h*0.22:.0f}" '
             f'fill="{LAND}"/>')
    p.append(f'<path d="M0 {h*0.22:.0f} Q {w*0.3:.0f} {h*0.30:.0f} '
             f'{w*0.55:.0f} {h*0.24:.0f} T {w} {h*0.28:.0f} L {w} 0 L 0 0 Z" '
             f'fill="{LAND}"/>')
    for i in range(1, 6):
        p.append(f'<line x1="{w*i/6:.0f}" y1="0" x2="{w*i/6:.0f}" y2="{h}" '
                 f'stroke="{MAPGRID}" stroke-width="1"/>')
        p.append(f'<line x1="0" y1="{h*i/6:.0f}" x2="{w}" y2="{h*i/6:.0f}" '
                 f'stroke="{MAPGRID}" stroke-width="1"/>')
    wps = [(0.18, 0.78), (0.38, 0.52), (0.60, 0.68), (0.78, 0.42),
           (0.62, 0.30)]
    pts = []
    for i in range(len(wps) - 1):
        (x0, y0), (x1, y1) = wps[i], wps[i + 1]
        for tstep in range(20):
            f = tstep / 20
            x = (x0 + (x1 - x0) * f) * w
            y = (y0 + (y1 - y0) * f) * h + math.sin(f * 6.3) * 4
            pts.append(f"{x:.0f},{y:.0f}")
    p.append(f'<polyline points="{" ".join(pts)}" fill="none" '
             f'stroke="{AMBER}" stroke-width="2.5" opacity="0.9"/>')
    for i, (fx, fy) in enumerate(wps):
        p.append(f'<circle cx="{fx*w:.0f}" cy="{fy*h:.0f}" r="5" '
                 f'fill="none" stroke="{YELLOW}" stroke-width="2"/>')
        p.append(f'<text x="{fx*w+9:.0f}" y="{fy*h+4:.0f}" fill="{DIM}" '
                 f'font-size="11">WP{i+1}</text>')
    # Operating region (OpRegion) and contacts — Phase B layers, mocked here.
    poly = [(0.10, 0.90), (0.30, 0.42), (0.70, 0.36), (0.90, 0.62), (0.75, 0.92)]
    p.append('<polygon points="' + " ".join(f"{fx*w:.0f},{fy*h:.0f}" for fx, fy in poly) +
             '" fill="rgba(146,86,184,0.10)" stroke="#b388d9" stroke-width="1.5" '
             'stroke-dasharray="6 4"/>')
    for (fx, fy, kind) in [(0.28, 0.62, "ais"), (0.50, 0.44, "ais"), (0.84, 0.55, "radar"),
                           (0.66, 0.80, "ais")]:
        col = "#73bf69" if kind == "ais" else "#f2495c"
        cx, cy = fx * w, fy * h
        p.append(f'<polygon points="{cx:.0f},{cy-6:.0f} {cx-5:.0f},{cy+5:.0f} {cx+5:.0f},{cy+5:.0f}" '
                 f'fill="{col}" opacity="0.9"/>')
    vx, vy = wps[-1][0] * w, wps[-1][1] * h
    p.append(f'<circle cx="{vx:.0f}" cy="{vy:.0f}" r="8" fill="{TEAL}" '
             f'stroke="#fff" stroke-width="2"/>')
    p.append(f'<rect x="{w-118}" y="{h-24}" width="114" height="20" rx="3" '
             f'fill="#00000066"/><text x="{w-110}" y="{h-10}" fill="{FAINT}" '
             f'font-size="10">© OpenStreetMap</text>')
    p.append(f'<rect x="10" y="10" width="26" height="52" rx="4" '
             f'fill="{PANEL}dd" stroke="{BORDER}"/>'
             f'<text x="23" y="30" fill="{TEXT}" font-size="16" '
             f'text-anchor="middle">+</text>'
             f'<text x="23" y="52" fill="{TEXT}" font-size="16" '
             f'text-anchor="middle">−</text>')
    p.append("</svg>")
    return "".join(p)


def svg_timeline(w, h):
    # The scripted Alpha scenario: start parked, enter region, survey with
    # pause + station-keep, comms-loss failsafe + re-arm hold, resume,
    # return-to-base, completed.
    segs = [("Parked", 0.035, "#5a5d63"),
            ("EnteringRegion", 0.05, "#6ed0e0"),
            ("Autonomous", 0.185, "OK"),
            ("Paused", 0.06, "YELLOW"),
            ("Autonomous", 0.06, "OK"),
            ("StationKeeping", 0.075, TEAL),
            ("CommsLost", 0.06, "RED"),
            ("AwaitingRearm", 0.06, AMBER),
            ("Autonomous", 0.26, "OK"),
            ("ReturnToBase", 0.09, AMBER),
            ("Completed", 0.065, PURPLE)]
    p = [f'<svg width="{w}" height="{h}">']
    x = 8.0
    bar_h = min(46, h - 42)
    for name, frac, col in segs:
        col = C(col)
        seg_w = (w - 16) * frac - 2
        p.append(f'<rect x="{x:.0f}" y="10" width="{seg_w:.0f}" '
                 f'height="{bar_h}" rx="3" fill="{col}" opacity="0.85"/>')
        if seg_w > 80:
            p.append(f'<text x="{x+seg_w/2:.0f}" y="{10+bar_h/2+4}" '
                     f'fill="#111" font-size="12" font-weight="600" '
                     f'text-anchor="middle">{name}</text>')
        x += seg_w + 2
    for i, tl in enumerate(["13:45", "14:00", "14:15", "14:30"]):
        p.append(f'<text x="{8 + (w-16)*i/3.2:.0f}" y="{10+bar_h+18}" '
                 f'fill="{FAINT}" font-size="11">{tl}</text>')
    p.append("</svg>")
    return "".join(p)


def svg_flags(w, h):
    """Multi-row autonomy flag activity strip (matches the state timeline)."""
    rows = [
        ("waypoint",        [(0.035, 0.845)]),
        ("pause",           [(0.27, 0.33)]),
        ("stationing",      [(0.39, 0.465), (0.465, 0.525)]),
        ("returning",       [(0.845, 0.935)]),
        ("armed",           [(0.0, 0.465), (0.585, 1.0)]),
        ("comms_lost",      [(0.465, 0.525)]),
        ("manual_override", [(0.0, 0.035), (0.935, 1.0)]),
    ]
    p = [f'<svg width="{w}" height="{h}">']
    label_w = 118
    track_w = w - label_w - 12
    row_h = min(30.0, (h - 34) / len(rows))
    for i, (name, spans) in enumerate(rows):
        y = 8 + i * row_h
        p.append(f'<text x="{label_w-8}" y="{y+row_h/2+3:.0f}" fill="{DIM}" '
                 f'font-size="11" text-anchor="end">{name}</text>')
        p.append(f'<rect x="{label_w}" y="{y:.0f}" width="{track_w:.0f}" '
                 f'height="{row_h-5:.0f}" rx="2" fill="{GRID}" '
                 f'opacity="0.55"/>')
        for a, b in spans:
            p.append(f'<rect x="{label_w+track_w*a:.0f}" y="{y:.0f}" '
                     f'width="{max(3, track_w*(b-a)):.0f}" '
                     f'height="{row_h-5:.0f}" rx="2" fill="{TEAL}" '
                     f'opacity="0.85"/>')
    base = 8 + len(rows) * row_h + 16
    for i, tl in enumerate(["13:45", "14:00", "14:15", "14:30"]):
        p.append(f'<text x="{label_w + track_w*i/3.2:.0f}" y="{base:.0f}" '
                 f'fill="{FAINT}" font-size="11">{tl}</text>')
    p.append("</svg>")
    return "".join(p)


# ---------------------------------------------------------------- analog
def _pol(cx, cy, r, deg):
    return (cx + r * math.cos(math.radians(deg)),
            cy - r * math.sin(math.radians(deg)))


def _arc(cx, cy, r, a0, a1, color, width):
    """Clockwise arc (math angles, a0 > a1) as an SVG stroke path."""
    x0, y0 = _pol(cx, cy, r, a0)
    x1, y1 = _pol(cx, cy, r, a1)
    large = 1 if (a0 - a1) > 180 else 0
    return (f'<path d="M {x0:.1f} {y0:.1f} A {r:.1f} {r:.1f} 0 {large} 1 '
            f'{x1:.1f} {y1:.1f}" fill="none" stroke="{color}" '
            f'stroke-width="{width}" stroke-linecap="butt"/>')


def _needle(cx, cy, deg, length, color, width=4):
    x1, y1 = _pol(cx, cy, length, deg)
    xt, yt = _pol(cx, cy, length * 0.18, deg + 180)
    return (f'<line x1="{xt:.1f}" y1="{yt:.1f}" x2="{x1:.1f}" y2="{y1:.1f}" '
            f'stroke="{color}" stroke-width="{width}" '
            f'stroke-linecap="round" '
            f'filter="drop-shadow(0 2px 3px rgba(0,0,0,0.45))"/>')


def svg_traffic(w, h):
    """Heading-up polar traffic view preview (sample contacts)."""
    cx, cy, r = w / 2, h / 2 + 6, min(w, h) * 0.40
    rings = "".join(f'<circle cx="{cx}" cy="{cy}" r="{r * k:.1f}" fill="none" '
                    f'stroke="{GRID}" stroke-dasharray="3 3"/>' for k in (0.25, 0.5, 0.75, 1))
    labels = "".join(f'<text x="{_pol(cx, cy, r + 12, 90 - a)[0]:.1f}" y="{_pol(cx, cy, r + 12, 90 - a)[1] + 4:.1f}" '
                     f'fill="{DIM}" font-size="10" text-anchor="middle">{"N" if a == 0 else f"{a:03d}"}</text>'
                     for a in (0, 45, 90, 135, 180, 225, 270, 315))
    def tri(deg, dist, color, rot=0):
        x, y = _pol(cx, cy, r * dist, 90 - deg)
        return (f'<polygon points="0,-7 6,5 -6,5" fill="{color}" '
                f'transform="translate({x:.1f},{y:.1f}) rotate({rot})"/>')
    sample = [(37, 0.23, RED, 215), (318, 0.19, GREEN, 40), (94, 0.55, GREEN, 10),
              (205, 0.8, YELLOW, 0), (150, 0.62, GREEN, 300), (265, 0.9, GREEN, 90),
              (10, 0.7, RED, 180)]
    contacts = "".join(tri(*c) for c in sample)
    wx, wy = _pol(cx, cy, r * 0.23, 90 - 37)
    cpa = (f'<line x1="{cx}" y1="{cy}" x2="{wx:.1f}" y2="{wy:.1f}" stroke="{RED}" stroke-dasharray="5 4"/>'
           f'<text x="{wx + 8:.1f}" y="{wy - 6:.1f}" fill="{TEXT}" font-size="11" font-weight="600">TGT-27</text>')
    own = f'<polygon points="0,-9 6,7 0,4 -6,7" fill="#fff" stroke="{CYAN}" transform="translate({cx},{cy})"/>'
    return (f'<svg width="{w}" height="{h}">'
            f'<circle cx="{cx}" cy="{cy}" r="{r}" fill="{PANEL}" stroke="{GRID}"/>{rings}'
            f'<line x1="{cx}" y1="{cy - r}" x2="{cx}" y2="{cy + r}" stroke="{GRID}"/>'
            f'<line x1="{cx - r}" y1="{cy}" x2="{cx + r}" y2="{cy}" stroke="{GRID}"/>'
            f'{labels}{cpa}{contacts}{own}'
            f'<text x="8" y="14" fill="{CYAN}" font-size="10" font-weight="600">HEADING UP · range 2 nm</text>'
            f'<text x="8" y="{h - 6}" fill="{DIM}" font-size="10">▲ report / AIS  ▲ radar  ▲ HIGH risk</text>'
            f'<text x="{w - 8}" y="{h - 6}" fill="{RED}" font-size="11" font-weight="600" text-anchor="end">TGT-27  CPA 82 m · TCPA 1:42</text></svg>')


def svg_meter_analog(meta, w, h):
    """240° needle meter mirroring the ECharts instrument styling."""
    mn, mx = meta["min"], meta["max"]
    v = max(mn, min(mx, meta.get("preview", mn)))
    zones = meta.get("zones") or []
    unit, dec = meta.get("unit", ""), meta.get("decimals", 0)
    split = meta.get("split", 6)
    labels = meta.get("labels")
    a_start, sweep = 210.0, 240.0
    ang = lambda x: a_start - sweep * (x - mn) / (mx - mn)
    cx, cy = w / 2, h * 0.56
    r = min(w * 0.44, h * 0.42)
    p = [f'<svg width="{w}" height="{h}">']
    if zones:
        prev = mn
        for hi, colz in zones:
            p.append(_arc(cx, cy, r, ang(prev), ang(hi), colz, 9))
            prev = hi
    else:
        p.append(_arc(cx, cy, r, a_start, a_start - sweep,
                      "#1c2a3d" if BG == "#111217"
                      else "rgba(36,41,46,0.10)", 9))
        _grad_id[0] += 1
        gid = f"mg{_grad_id[0]}"
        p.append(f'<defs><linearGradient id="{gid}" x1="0" y1="0" '
                 f'x2="1" y2="0">'
                 f'<stop offset="0%" stop-color="#00bcd4"/>'
                 f'<stop offset="100%" stop-color="#42a5f5"/>'
                 f'</linearGradient></defs>')
        p.append(_arc(cx, cy, r, a_start, ang(v), f"url(#{gid})", 9))
    for i in range(split + 1):
        val = mn + (mx - mn) * i / split
        a = ang(val)
        x0, y0 = _pol(cx, cy, r - 13, a)
        x1, y1 = _pol(cx, cy, r - 5, a)
        p.append(f'<line x1="{x0:.1f}" y1="{y0:.1f}" x2="{x1:.1f}" '
                 f'y2="{y1:.1f}" stroke="{FAINT}" stroke-width="2"/>')
        lx, ly = _pol(cx, cy, r - 24, a)
        if labels is not None:
            lbl = ""
            for k in (val, int(val), str(int(val)), f"{val:g}"):
                try:
                    if k in labels:
                        lbl = labels[k]
                        break
                except TypeError:
                    pass
        else:
            lbl = f"{val:g}"
        if lbl != "":
            p.append(f'<text x="{lx:.1f}" y="{ly+3.5:.1f}" fill="{DIM}" '
                     f'font-size="10" text-anchor="middle">{lbl}</text>')
    needle_c = "#e8ebee" if BG == "#111217" else "#24292e"
    p.append(_needle(cx, cy, ang(v), r - 22, needle_c))
    p.append(f'<circle cx="{cx:.1f}" cy="{cy:.1f}" r="6" fill="{TEAL}" '
             f'stroke="{needle_c}" stroke-width="2"/>')
    txt = meta.get("value_text") or f"{v:.{dec}f} {unit}"
    vcol = "#ffffff" if BG == "#111217" else TEXT
    p.append(f'<text x="{cx:.1f}" y="{cy + r*0.52:.1f}" fill="{vcol}" '
             f'font-size="16" font-weight="600" '
             f'text-anchor="middle">{txt}</text>')
    if meta.get("sub"):
        p.append(f'<text x="{cx:.1f}" y="{cy + r*0.52 + 15:.1f}" '
                 f'fill="{TEAL}" font-size="9" letter-spacing="1" '
                 f'text-anchor="middle">{meta["sub"]}</text>')
    p.append("</svg>")
    return "".join(p)


def svg_compass(meta, w, h):
    hdg = meta.get("preview", 0) % 360
    cog = meta.get("preview2", hdg) % 360
    cx, cy = w / 2, h * 0.52
    r = min(w * 0.42, h * 0.40)
    ang = lambda head: 90.0 - head
    p = [f'<svg width="{w}" height="{h}">']
    rail = ("rgba(204,212,220,0.16)" if BG == "#111217"
            else "rgba(36,41,46,0.12)")
    p.append(f'<circle cx="{cx:.1f}" cy="{cy:.1f}" r="{r:.1f}" fill="none" '
             f'stroke="{rail}" stroke-width="4"/>')
    for d in range(0, 360, 30):
        a = ang(d)
        x0, y0 = _pol(cx, cy, r - 9, a)
        x1, y1 = _pol(cx, cy, r - 2, a)
        p.append(f'<line x1="{x0:.1f}" y1="{y0:.1f}" x2="{x1:.1f}" '
                 f'y2="{y1:.1f}" stroke="{FAINT}" stroke-width="2"/>')
        lx, ly = _pol(cx, cy, r - 20, a)
        card = {0: "N", 90: "E", 180: "S", 270: "W"}
        lbl = card.get(d, str(d))
        weight = 700 if d in card else 400
        colr = TEXT if d in card else DIM
        p.append(f'<text x="{lx:.1f}" y="{ly+3.5:.1f}" fill="{colr}" '
                 f'font-size="10" font-weight="{weight}" '
                 f'text-anchor="middle">{lbl}</text>')
    needle_c = "#e8ebee" if BG == "#111217" else "#24292e"
    p.append(_needle(cx, cy, ang(cog), r * 0.55, BLUE, 3))
    p.append(_needle(cx, cy, ang(hdg), r * 0.70, TEAL, 5))
    p.append(f'<circle cx="{cx:.1f}" cy="{cy:.1f}" r="6" fill="{TEAL}" '
             f'stroke="{needle_c}" stroke-width="2"/>')
    p.append(f'<text x="{cx:.1f}" y="{cy + r*0.30:.1f}" fill="#ffffff" '
             f'font-size="17" font-weight="600" text-anchor="middle">'
             f'{hdg:.0f}&#176;</text>')
    p.append(f'<text x="{cx:.1f}" y="{cy + r*0.30 + 13:.1f}" fill="{TEAL}" '
             f'font-size="9" letter-spacing="1" text-anchor="middle">'
             f'HDG &#8212; COG {cog:.0f}&#176;</text>')
    p.append("</svg>")
    return "".join(p)


def svg_rudder(meta, w, h):
    v = max(-35, min(35, meta.get("preview", 0)))
    side = "PORT " if v < -0.5 else ("STBD " if v > 0.5 else "")
    m = dict(meta)
    m.update({"min": -35, "max": 35,
              "zones": [(0, "rgba(242,73,92,0.85)"),
                        (35, "rgba(115,191,105,0.85)")],
              "unit": "°", "decimals": 0, "split": 7,
              "labels": {-35: "35", -25: "25", -15: "15", -5: "5",
                         5: "5", 15: "15", 25: "25", 35: "35"},
              "sub": "PORT ‹ RUDDER › STBD",
              "value_text": f"{side}{abs(v):.0f}°"})
    return svg_meter_analog(m, w, h)


def table_html(headers, rows):
    th = "".join(f'<th>{h}</th>' for h in headers)
    trs = ""
    for r in rows:
        tds = "".join(f'<td>{c}</td>' for c in r)
        trs += f"<tr>{tds}</tr>"
    return (f'<table class="gtable"><thead><tr>{th}</tr></thead>'
            f'<tbody>{trs}</tbody></table>')


def sparkline(w, h, color, seed):
    r = random.Random(seed)
    n = 40
    vals = []
    v = 0.5
    for _ in range(n):
        v = max(0.05, min(0.95, v + r.uniform(-0.09, 0.09)))
        vals.append(v)
    pts = " ".join(f"{w*i/(n-1):.1f},{h*(1-v*0.9):.1f}"
                   for i, v in enumerate(vals))
    return (f'<svg width="{w}" height="{h}" style="position:absolute;'
            f'left:0;bottom:0;opacity:0.55">'
            f'<polygon points="0,{h} {pts} {w},{h}" fill="{color}" '
            f'opacity="0.25"/>'
            f'<polyline points="{pts}" fill="none" stroke="{color}" '
            f'stroke-width="1.5"/></svg>')


def render_panel(uid, p):
    x = p["gridPos"]["x"] * COL
    y = p["gridPos"]["y"] * ROW
    w = p["gridPos"]["w"] * COL - GAP
    h = p["gridPos"]["h"] * ROW - GAP
    title = p.get("title", "")
    ptype = p["type"]
    iw, ih = int(w) - 16, int(h) - 38
    pstyle = ""
    tstyle = ""
    if ptype == "stat":
        val, col = STATS.get(title, ("—", "TEXT"))
        col = C(col)
        fs = 30 if len(str(val)) < 10 else (22 if len(str(val)) < 14 else 17)
        opts = p.get("options", {})
        if opts.get("colorMode") == "background":
            # Solid status tile: colored panel, high-contrast text.
            txt = "#101214" if col in (C("YELLOW"), "#fade2a") else "#ffffff"
            pstyle = (f";background:linear-gradient(160deg,{col} 0%,"
                      f"{col}e6 55%,{col}c4 100%);"
                      f"border-color:transparent;"
                      f"box-shadow:inset 0 1px 0 rgba(255,255,255,0.22),"
                      f"0 4px 14px rgba(0,0,0,0.30)")
            tstyle = f';color:{txt};opacity:0.82'
            body = (f'<div class="statv" style="color:{txt};'
                    f'font-size:{fs}px;font-weight:650">{val}</div>')
        else:
            spark = (sparkline(int(w) - 4, int(ih * 0.55), col, title)
                     if opts.get("graphMode") == "area" else "")
            body = (spark +
                    f'<div class="statv" style="color:{col};'
                    f'font-size:{fs}px">{val}</div>')
    elif ptype == "bargauge":
        pct, col = GAUGES.get(title, (62, TEAL))
        col = C(col) if col != TEAL else col
        body = (f'<div style="position:relative;margin:4px 8px;height:{max(10, ih-8)}px;'
                f'background:rgba(120,140,160,0.18);border-radius:3px">'
                f'<div style="width:{pct}%;height:100%;background:{col};'
                f'border-radius:3px;opacity:.9"></div>'
                f'<div style="position:absolute;right:10px;top:0;line-height:{max(10, ih-8)}px;'
                f'font-size:13px;color:#fff;font-weight:600">{pct}%</div></div>')
    elif ptype == "gauge":
        pct, col = GAUGES.get(title, (50, "OK"))
        col = C(col)
        body = svg_gauge(pct, col, iw, ih)
    elif ptype == "timeseries":
        body = svg_line_chart(series_for(uid, title), iw, ih)
    elif ptype == "geomap":
        body = svg_map(iw, ih)
    elif ptype == "volkovlabs-echarts-panel":
        meta = p.get("swiftMeta", {})
        kind = meta.get("kind", "meter")
        if kind == "compass":
            body = svg_compass(meta, iw, ih)
        elif kind == "rudder":
            body = svg_rudder(meta, iw, ih)
        elif kind == "traffic":
            body = svg_traffic(iw, ih)
        else:
            body = svg_meter_analog(meta, iw, ih)
    elif ptype == "state-timeline":
        body = (svg_flags(iw, ih) if "flags" in title.lower()
                else svg_timeline(iw, ih))
    elif ptype == "table":
        if "PGN" in title:
            rows = [("127488", "10.0"), ("129025", "10.0"), ("129026", "4.0"),
                    ("127250", "10.0"), ("127508", "1.5"), ("127505", "0.5"),
                    ("127245", "5.0")]
            body = table_html(["pgn", "rate"], rows)
        else:
            body = table_html(["Time", "Message", "Type", "Severity"],
                              [(f"2026-08-12 {t}", m, ty, sv)
                               for t, m, ty, sv in EVENTS])
    elif ptype == "text":
        content = p["options"].get("content", "")
        if content.startswith("![Swiftships]"):
            import re as _re
            m = _re.search(r"\*\*([A-Z ()/-]+)\*\*", content)
            band_title = m.group(1) if m else ""
            chip = os.path.join(os.path.dirname(__file__), "..", "deploy",
                                "branding", "swiftships_wordmark_chip_sm.png")
            import base64 as _b64
            data = _b64.b64encode(open(chip, "rb").read()).decode()
            body = (f'<div style="display:flex;align-items:center;gap:14px;'
                    f'height:100%;padding:0 6px">'
                    f'<img src="data:image/png;base64,{data}" height="30">'
                    f'<b style="font-size:15px;letter-spacing:.04em">{band_title}</b>'
                    f'<span style="color:{DIM}">&middot;</span>'
                    f'<span style="color:{DIM}">Vessel <b>alpha</b></span></div>')
        else:
            body = f'<div class="ptext">{content}</div>'
    elif ptype == "alertlist":
        body = ('<div class="ptext" style="color:' + DIM + '">'
                '<div style="padding:24px;text-align:center">No firing alerts'
                '<br><span style="font-size:11px;color:' + FAINT +
                '">15 rules evaluated — all normal</span></div></div>')
    else:
        body = ""
    return (f'<div class="panel" style="left:{x:.0f}px;top:{y:.0f}px;'
            f'width:{w:.0f}px;height:{h:.0f}px{pstyle}">'
            f'<div class="ptitle" style="{tstyle.lstrip(";")}">{title}</div>'
            f'{body}</div>')


def _logo_uri(theme):
    name = ("swiftships_wordmark_light.png" if theme == "dark"
            else "swiftships_wordmark.png")
    path = os.path.join(HERE, "..", "deploy", "branding", name)
    import base64
    with open(path, "rb") as f:
        return "data:image/png;base64," + base64.b64encode(f.read()).decode()


def render(uid, theme="dark", width=1600):
    globals().update(THEMES[theme])
    global W, COL
    W, COL = width, width / 24.0
    path = os.path.join(DASH, f"{uid}.json")
    d = json.load(open(path))
    max_y = max(p["gridPos"]["y"] + p["gridPos"]["h"] for p in d["panels"])
    height = int(max_y * ROW) + 120
    panels = "".join(render_panel(uid, p) for p in d["panels"])
    title = d["title"]
    # Tab row: all usv dashboards, current one highlighted
    all_titles = []
    for f in sorted(os.listdir(DASH)):
        if f.endswith(".json"):
            dd = json.load(open(os.path.join(DASH, f)))
            all_titles.append((f[:-5], dd["title"]))
    order = ["usv-mission", "usv-navigation", "usv-autopilot", "usv-health",
             "usv-canbus", "usv-traffic", "usv-camera", "usv-comms",
             "usv-system", "usv-replay", "usv-alarms"]
    all_titles.sort(key=lambda t: order.index(t[0]) if t[0] in order else 99)
    tabs = "".join(
        f'<span class="tab{" active" if u == uid else ""}">{t}</span>'
        for u, t in all_titles)
    tabs += (f'<span style="flex:1"></span>'
             f'<span class="tab theme{" on" if theme == "light" else ""}">'
             f'☀ Light</span>'
             f'<span class="tab theme{" on" if theme == "dark" else ""}">'
             f'☾ Dark</span>')
    LOGO_URI = _logo_uri(theme)
    html = f"""<!doctype html><html><head><meta charset="utf-8"><style>
* {{ box-sizing: border-box; margin: 0; }}
body {{ background:{BG}; font-family: Inter,-apple-system,'Segoe UI',
  Roboto,Arial,sans-serif; width:{W + 32}px; }}
.topnav {{ height:52px; display:flex; align-items:center; gap:10px;
  padding:0 20px; border-bottom:1px solid {BORDER}; color:{DIM};
  font-size:13px; }}
.topnav .crumb b {{ color:{TEXT}; font-weight:600; }}
.controls {{ margin-left:auto; display:flex; gap:8px; align-items:center; }}
.ctrl {{ border:1px solid {BORDER}; border-radius:2px; padding:5px 10px;
  color:{TEXT}; font-size:12px; background:{PANEL}; }}
.varrow {{ padding:8px 20px; color:{DIM}; font-size:12px; }}
.varrow .ctrl {{ margin-left:6px; }}
.tabs {{ display:flex; flex-wrap:wrap; gap:6px; padding:2px 20px 10px; }}
.tab {{ border:1px solid {BORDER}; border-radius:2px; padding:4px 12px;
  color:{DIM}; font-size:12px; background:{PANEL}; }}
.tab.active {{ color:#fff; background:{TEAL}; border-color:{TEAL};
  font-weight:600; }}
.tab.theme {{ color:{TEXT}; }}
.tab.theme.on {{ border-color:{TEAL}; color:{TEAL}; font-weight:600; }}
.canvas {{ position:relative; margin:4px 16px; }}
.panel {{ position:absolute; background:{PANEL};
  border:1px solid {BORDER}; border-radius:2px; padding:6px 8px;
  overflow:hidden; }}
.ptitle {{ color:{TEXT}; font-size:13px; font-weight:500; padding:2px 4px 6px; }}
.statv {{ display:flex; align-items:center; justify-content:center;
  height:calc(100% - 30px); font-weight:600; }}
.gtable {{ width:100%; border-collapse:collapse; font-size:12px;
  color:{TEXT}; }}
.gtable th {{ text-align:left; color:{BLUE}; font-weight:500;
  padding:5px 8px; border-bottom:1px solid {BORDER}; }}
.gtable td {{ padding:4px 8px; border-bottom:1px solid {GRID}; }}
.ptext {{ color:{DIM}; font-size:12px; padding:4px; line-height:1.5; }}
</style></head><body>
<div class="topnav">
<img src="{LOGO_URI}" height="26" style="flex:none" alt="Swiftships"/>
<span class="crumb"><b>Swift Autonomous Dashboard</b> &rsaquo;
{title} ☆</span>
<div class="controls"><span class="ctrl">🖵 Last 30 minutes ▾</span>
<span class="ctrl">⟳ 5s ▾</span></div></div>
<div class="varrow">Vessel <span class="ctrl">usv-01 ▾</span></div>
<div class="tabs">{tabs}</div>
<div class="canvas" style="height:{height}px">{panels}</div>
</body></html>"""
    os.makedirs(OUT, exist_ok=True)
    suffix = ("" if theme == "dark" else f"-{theme}") + \
        ("" if width >= 1400 else "-tablet")
    out = os.path.join(OUT, f"{uid}{suffix}.html")
    with open(out, "w") as f:
        f.write(html)
    print(f"wrote {out} ({height}px tall)")
    return out, height


if __name__ == "__main__":
    # args: uid[:theme[:width]]   e.g. usv-mission:light  usv-mission:dark:1024
    specs = sys.argv[1:] or ["usv-mission", "usv-navigation", "usv-health"]
    for spec in specs:
        parts = spec.split(":")
        uid = parts[0]
        theme = parts[1] if len(parts) > 1 else "dark"
        width = int(parts[2]) if len(parts) > 2 else 1600
        render(uid, theme, width)
