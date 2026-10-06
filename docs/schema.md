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
| fix_type | string | | GNSS: e.g. "RTK Fixed", "3D"; VectorNav INS via Signal K: "no fix", "INS aligning", "INS tracking", "GNSS lost" |
| ins_mode | float | | VectorNav INS mode 0..3 (signalk_bridge, from navigation.vectornav.insMode) |
| cross_track_error | float | m | |
| x, y | float | m | MOOS local grid |
| depth | float | m | NAV_DEPTH |
| gps_quality | float | GGA fix code | 0 none, 1 GPS, 2 DGPS, 4 RTK fixed, 5 RTK float (GPS_QUALITY, iDivisorNMEA) |
| course_next_point | float | m | Signal K course computer (Freeboard manual courses) — NOT the autonomy route; that is mission.dist_to_waypoint |

### mission (FR-1) — source: MOOS bridge
progress_pct (%), waypoint_index, waypoints_total, waypoints_remaining,
region_vertices (vertex count of the current operating region, written
with the geometry set), route_length_m, route_in_region (0/1, every
waypoint inside the region; the readiness course check reads them back),
dist_to_waypoint (m), dist_to_station (m, planchaPID DIST_TO_STATION),
eta_s (s), distance_traveled (m), elapsed_s (s).
`dist_to_waypoint` comes from the Alpha mission's `DIST_TO_NEXT_WPT`;
`waypoints_total`/`progress_pct`/`eta_s`/`distance_traveled` are derived by
the bridge (VIEW_SEGLIST count, index/total, dist/speed, NAV_X/Y odometry)
and yield automatically to any external publisher of the legacy
`WPT_TOTAL`/`MISSION_PROGRESS`/`WPT_ETA`/`DIST_TRAVELED` variables.

### mission_state (FR-1/FR-3) — source: MOOS bridge (strings)
mission_name, mission_id, route_name, state (derived: Idle | Autonomous |
Remote | Paused | StationKeeping | ReturnToBase | EnteringRegion |
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
at 1 Hz), enc_status (console ENC_STATUS: "selected:<chart>" or "none"),
checklist_pending ("|"-separated pending item names, "-" when none),
checklist_by (operator who ticked), plus the numeric checklist_done and
checklist_total (console CHECKLIST_STATUS, see docs/deployment.md). mission_id is fed by iWebSocket's ID_ROUTE (a Signal K route
uuid); route_name is that route's name looked up once per change from the
Signal K server (`signalk_http` in the ProcessConfig; v2 and v1 resource
paths are both tried, `signalk_token` when security denies anonymous reads), "Return to base" for
the built-in return route, or the id's first block when the lookup fails;
mission_name comes from the grafana_bridge ProcessConfig (default: the MOOS
Community name).
See docs/alpha-mission-integration.md for the state derivation rules.

### contacts / traffic (FR-6) — source: MOOS bridge, 1 Hz while active
`contacts`: one series per live contact, tags `contact` (name / target id)
and `source` (report = NODE_REPORT / AIS-style, radar = RADAR_TARGET);
fields range_m, bearing, rel_bearing (-180..180, port negative), spd_kn,
hdg, rel_speed_kn, cpa_m, tcpa_s (-1 = opening), risk (0 clear, 1 medium,
2 high), strings encounter (head-on | crossing | overtaking) and type.
Geometry is computed onboard from the own-ship state (mission_logic.hpp:
ComputeCpa, EncounterType, RiskLevel; thresholds radar_cpa_alert_m /
tcpa_alert_s). A NODE_REPORT contact is positioned from its LAT/LON on
the mission datum (LatOrigin/LongOrigin) when it carries them, else from
its X/Y; own ship from a fresh own fix (NAV_LAT/NAV_LONG or Signal K
nav.lat/lon) on the same datum, else NAV_X/NAV_Y. Rows stop when a contact
expires (contact_ttl_s), so a 30-second window is the live picture.
`traffic`: contacts, reports, radar_only, high_risk, risk_level,
avoidance_active (helm running an avd*/colreg*/avoid* behavior),
closest_range_m, min_cpa_m, min_tcpa_s, planned_course (bearing to the
current waypoint), course_deviation (commanded minus planned), strings
worst_contact and reason. The legacy `radar` aggregates are kept for the
alert rules.

### readiness / readiness_summary (FR-12) — source: MOOS bridge, every readiness_period_s (5 s)
readiness, tag `check` (gps | course | enc | autonomy | apps | steering |
comms | can | engine | electrical | endurance | ais | cameras | obc |
safety | checklist): seq (display order), status (0 PASS, 1 FAIL, 2 N/A, 3
CONSOLE), name (string, operator wording), evidence (string, what was
seen). readiness_summary: ready (0/1), passed, failed, na, console,
checks, blocking (string, "; "-separated names of the failed checks, "-"
when none), profile ("sim" | "vessel"). checklist, tag `seq`: item,
description and state (strings; one row per item, TICKED | PENDING, then
a footer "Console checklist d / t · ticked by X" with state COMPLETE |
OPEN), erased and rewritten when the console's list changes. Decided by usv::EvaluateReadiness
(mission_logic.hpp) from the latest value of the bridge's own fields plus
comms.signalk_ws_up, ping.*, canbus.*, camera.connected and the Telegraf
cpu/mem/disk fields polled from InfluxDB. A change of verdict raises an
event of type `readiness`.

### geometry (map overlays) — source: MOOS bridge, on change
Vertex sets for the Navigation / Replay maps, tags `kind` (route |
region) and `idx` (zero-padded vertex index), fields lat, lon, seq. The
bridge converts the MOOS local grid (VIEW_SEGLIST of the survey/return
behavior, OPR_POLY_ACTUAL) to geographic coordinates with the mission
file's LatOrigin/LongOrigin, deletes the previous set of the same kind
and writes all vertices with one timestamp, so `last()` per idx is
exactly the current geometry. The region outline is closed (first vertex
repeated). "NONE" clears the set. Written only when the datum is present.

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
pContactSpawn / AIS; RADAR_TARGET + RADAR_TTM_RATE from mayara_bridge,
i.e. the mayara-server ARPA tracker, while the radar transmits)
online (0/1: TTM heartbeat or any contact update within 30 s), targets
(active contacts, 10 s TTL), closest_range_m (own ship vs contacts, see above),
cpa_m, tcpa_s (RADAR_TARGET only), ttm_rate (/s), detection_rate (0-1)

### camera (FR-7) — source: frigate_bridge (Frigate NVR API), every 5 s
Tag: `cam` (the Frigate camera name). Fields: connected (camera answers on
its RTSP port), stream_up (Frigate receives >= 1 fps), recording (0/1), fps.
Written only for cameras enabled in Frigate. Reserved for a camera-API
adapter: bitrate_kbps, temp_c, ptz_pan, ptz_tilt.

### camera_summary — source: frigate_bridge
frigate_up (0/1), enabled, streaming, connected (camera counts). Read by the
Readiness "Cameras" check ("2 / 2 cameras streaming").

### comms (FR-8) — sources: bridges + Telegraf ping
internet_up, starlink_up, vpn_up, signalk_ws_up (0/1), latency_ms,
packet_loss_pct, up_mbps, down_mbps, moos_msg_rate, signalk_msg_rate

### System metrics (FR-9) — source: Telegraf
Standard Telegraf measurements: `cpu`, `mem`, `disk`, `diskio`, `net`,
`temp`, `system`, `ping`, `systemd_units` (tag `name`), `net_response`
(tag `service`: signalk | moos | influxdb). Tagged by `host`, plus global
tag `vessel`.

`gpu` (tag `vendor`): utilization_gpu (%), and per vendor: Intel
(usv_gpu_probe / intel_gpu_top) render_busy, video_busy, frequency_mhz,
power_w, rc6_pct; NVIDIA (nvidia_smi with name_override) temperature_gpu,
memory_used and the rest of that input's fields.

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
