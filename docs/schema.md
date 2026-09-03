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
| depth | float | m | NAV_DEPTH |
| gps_quality | float | GGA fix code | 0 none, 1 GPS, 2 DGPS, 4 RTK fixed, 5 RTK float (GPS_QUALITY, iDivisorNMEA) |
| course_next_point | float | m | Signal K course computer (Freeboard manual courses) — NOT the autonomy route; that is mission.dist_to_waypoint |

### mission (FR-1) — source: MOOS bridge
progress_pct (%), waypoint_index, waypoints_total, waypoints_remaining,
dist_to_waypoint (m), dist_to_station (m, planchaPID DIST_TO_STATION),
eta_s (s), distance_traveled (m), elapsed_s (s).
`dist_to_waypoint` comes from the Alpha mission's `DIST_TO_NEXT_WPT`;
`waypoints_total`/`progress_pct`/`eta_s`/`distance_traveled` are derived by
the bridge (VIEW_SEGLIST count, index/total, dist/speed, NAV_X/Y odometry)
and yield automatically to any external publisher of the legacy
`WPT_TOTAL`/`MISSION_PROGRESS`/`WPT_ETA`/`DIST_TRAVELED` variables.

### mission_state (FR-1/FR-3) — source: MOOS bridge (strings)
mission_name, mission_id, route_name, state (derived: Idle | Autonomous |
Remote | Paused | StationKeeping | ReturnToHome | EnteringRegion |
AwaitingRearm | CommsLost | Parked | Completed), state_external (a
MISSION_STATE published by another app, if any), active_behavior (parsed
from IVPHELM_SUMMARY: the helm's running behavior set; the per-iteration
active set only on helm builds that do not report running_bhvs), station_keep,
helm_state (IVPHELM_STATE), allstop_reason (IVPHELM_ALLSTOP),
comms_failsafe ("ok", "grace", the failsafe action e.g. "loiter", or "rtb"),
entry_gate (open | closed), opr_status, helm_summary, arrival_state
(cruising | decelerating | arrived), corridor_status (none | slow | stop |
recovering), crash_stop_status (off | braking | stopped), region_warning,
wpt_stat (all from planchaPID / iWebSocket, written on change and re-emitted
at 1 Hz). mission_id is fed by iWebSocket's ID_ROUTE (a Signal K route
uuid); route_name is that route's name looked up once per change from the
Signal K server (`signalk_http` in the ProcessConfig; v2 and v1 resource
paths are both tried, `signalk_token` when security denies anonymous reads), "Return to base" for
the built-in return route, or the id's first block when the lookup fails;
mission_name comes from the grafana_bridge ProcessConfig (default: the MOOS
Community name).
See docs/alpha-mission-integration.md for the state derivation rules.

### autonomy (FR-1/FR-3/FR-11) — source: MOOS bridge, 1 Hz snapshot
Numeric mirrors of the Alpha mission flags so they can be graphed and
alerted on (0/1 unless noted): deploy, waypoint, returning, stationing,
pause, armed, comms_lost, entry_gate_open, manual_override,
survey_complete, awaiting_rearm, helm_drive, allstop (1 only for
unexpected all-stop reasons; ManualOverride/NothingToDo are benign),
geofence_breached (0 ok | 1 breach | 2 recovering; decays to 0 after 30 s
without an OPR_BREACHED post), obstacle_alerts (count, 120 s TTL),
comms_phase (0 link ok | 1 grace, mission continues | 2 failsafe hold |
3 return to base), remote_control (REMOTE_CONTROL), crash_stop (CRASH_STOP).

### autopilot (FR-3) — sources: MOOS bridge, CAN bridge
desired_heading, actual_heading (deg), desired_speed, actual_speed (kn),
rudder_cmd (deg in simulation; PERCENT on the vessel's iCAN), rudder_actual
(deg, $AGRSA via ANGULO_LEME or N2K 127245), throttle_cmd (%),
heading_error (deg, derived by the bridge as wrap(desired - actual), 2 Hz),
speed_error (kn, derived), speed_cap, cmd_speed (kn, planchaPID governor),
next_turn_angle (deg), pid_heading_out, pid_speed_out (legacy pMarinePID
names; planchaPID does not publish them)

### engine (FR-4) — sources: MOOS bridge (iCANRec ENGINE_*, SAE J1939 on
slcan0), CAN bridge (N2K PGN 127488/127505 if an N2K bus exists), Signal K
Tag: `instance` (multi-engine; ENGINE_* is instance 0). Fields: rpm,
coolant_temp, oil_temp (°C), oil_pressure, intake_manifold_pressure (kPa),
fuel_level (%), fuel_rate (L/h), runtime_h (h), percent_load,
percent_torque, pedal_position (%), active_dtcs (count).
Not produced in the simulation environment (no engine).

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

### radar (FR-6) — source: MOOS bridge (NODE_REPORT from iRadarTargets /
pContactSpawn / AIS; RADAR_TARGET + RADAR_TTM_RATE once iRadarTargets
publishes them)
online (0/1: TTM heartbeat or any contact update within 30 s), targets
(active contacts, 10 s TTL), closest_range_m (from own NAV_X/Y),
cpa_m, tcpa_s (RADAR_TARGET only), ttm_rate (/s), detection_rate (0-1)

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
geofence | obstacle | radar), `severity` (info | warning | critical),
`source`, `mission_id` (type=mission only, from ID_ROUTE).
Fields: message (string), value (1), and for type=mission: start_ms
(epoch ms of the survey start) plus end_ms on the closing event, so the
Replay dashboard can index missions and jump to their time range.

## Conventions
- Units are display units (kn, deg, °C, %) — conversion from SI happens in
  the bridges, not in dashboards. The MOOS bridge converts NAV_SPEED /
  DESIRED_SPEED / PID_SPEED_ERROR from m/s to knots.
- Booleans are written as 0/1 floats so they can be graphed and alerted on.
- New sensors: add a measurement or field here first, then extend a bridge
  and regenerate dashboards (`tools/gen_dashboards.py`).
