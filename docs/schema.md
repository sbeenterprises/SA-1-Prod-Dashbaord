# InfluxDB Data Schema

Org: `usv` (configurable). All points carry tag `vessel` (e.g. `usv-01`) for
fleet expansion.

## Buckets

| Bucket | Retention | Contents |
|---|---|---|
| `telemetry` | 30 d (configurable) | Raw high-rate telemetry |
| `telemetry_1m` | 365 d | 1-minute means (downsample task), mission analysis/replay beyond raw retention |
| `events` | 365 d | Mission events, state changes, alarms |

## Measurements (bucket: telemetry)

### nav (FR-2) — sources: Signal K bridge, CAN bridge, MOOS bridge
| Field | Type | Unit | Notes |
|---|---|---|---|
| lat, lon | float | deg | |
| sog | float | kn | |
| cog, heading, yaw, pitch, roll | float | deg | |
| satellites | float | count | |
| hdop | float | | |
| fix_type | string | | e.g. "RTK Fixed", "3D" |
| cross_track_error | float | m | |
| x, y | float | m | MOOS local grid |

### mission (FR-1) — source: MOOS bridge
progress_pct (%), waypoint_index, waypoints_total, dist_to_waypoint (m),
eta_s (s), distance_traveled (m), elapsed_s (s)

### mission_state (FR-1/FR-3) — source: MOOS bridge (strings)
mission_name, mission_id, state (Idle | Autonomous | StationKeeping |
ReturnToHome | Completed), active_behavior, station_keep, helm_summary

### autopilot (FR-3) — sources: MOOS bridge, CAN bridge
desired_heading, actual_heading (deg), desired_speed, actual_speed (kn),
rudder_cmd (deg), rudder_actual (deg), throttle_cmd (%),
heading_error (deg), speed_error (kn), pid_heading_out, pid_speed_out

### engine (FR-4) — sources: CAN bridge (PGN 127488/127505), Signal K
Tag: `instance` (multi-engine). Fields: rpm, coolant_temp (°C),
oil_pressure (kPa), fuel_level (%), fuel_rate (L/h), runtime_h (h)

### electrical (FR-4) — sources: CAN bridge (PGN 127508), Signal K
Tag: `instance`. Fields: battery_voltage (V), battery_current (A), soc (%),
charging_status (string), alternator_voltage (V)

### power (FR-4) — source: Signal K
total_power (W), system_voltage (V), distribution_ok (0/1)

### canbus (FR-5) — source: C++ CAN bridge
msg_rate (msgs/s), utilization_pct (%), error_frames, dropped_frames,
active_pgns, interface_up (0/1)

### canbus_pgn (FR-5) — source: C++ CAN bridge
Tag: `pgn`. Field: rate (msgs/s per PGN)

### radar (FR-6) — source: radar integration (adapter TBD)
online (0/1), targets, closest_range_m, cpa_m, tcpa_s, detection_rate (0-1)

### camera (FR-7) — source: camera manager (adapter TBD)
Tag: `cam`. Fields: connected, stream_up, recording (0/1), fps,
bitrate_kbps, temp_c, ptz_pan, ptz_tilt

### comms (FR-8) — sources: bridges + Telegraf ping
internet_up, starlink_up, vpn_up, signalk_ws_up (0/1), latency_ms,
packet_loss_pct, up_mbps, down_mbps, moos_msg_rate, signalk_msg_rate

### System metrics (FR-9) — source: Telegraf
Standard Telegraf measurements: `cpu`, `mem`, `disk`, `diskio`, `net`,
`temp`, `system`, `ping`, `systemd_units` (tag `name`), `net_response`
(tag `service`: signalk | moos | influxdb). Tagged by `host`, plus global
tag `vessel`.

## Measurements (bucket: events)

### events (FR-1/FR-11)
Tags: `vessel`, `type` (state_change | waypoint | mission | alarm |
geofence), `severity` (info | warning | critical), `source`.
Fields: message (string), value (1)

## Conventions
- Units are display units (kn, deg, °C, %) — conversion from SI happens in
  the bridges, not in dashboards.
- Booleans are written as 0/1 floats so they can be graphed and alerted on.
- New sensors: add a measurement or field here first, then extend a bridge
  and regenerate dashboards (`tools/gen_dashboards.py`).
