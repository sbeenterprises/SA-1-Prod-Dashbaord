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
| course_next_point | float | m | Signal K course computer (Swiftships-UI manual courses) — NOT the autonomy route; that is mission.dist_to_waypoint |

### mission (FR-1) — source: MOOS bridge
progress_pct (%), waypoint_index, waypoints_total, waypoints_remaining,
region_vertices (vertex count of the current operating region from
REGION_POLY, OPR_POLY_ACTUAL or a REGION_UPDATES with a pts block, written
with the geometry set), route_length_m, route_in_region (0/1, every
waypoint inside the region; the readiness course check reads them back),
dist_to_waypoint (m), dist_to_station (m, planchaPID DIST_TO_STATION),
eta_s (s), distance_traveled (m), elapsed_s (s), base_lat / base_lon (deg,
the comms-loss return base from iWebSocket's BASE_COORDS, re-written every
second while known).
`dist_to_waypoint` comes from the Alpha mission's `DIST_TO_NEXT_WPT`;
`waypoints_total`/`progress_pct`/`eta_s`/`distance_traveled` are derived by
the bridge (VIEW_SEGLIST count, index/total, dist/speed, NAV_X/Y odometry)
and yield automatically to any external publisher of the legacy
`WPT_TOTAL`/`MISSION_PROGRESS`/`WPT_ETA`/`DIST_TRAVELED` variables.

### mission_state (FR-1/FR-3) — source: MOOS bridge (strings)
mission_name, mission_id, route_name, state (derived: Idle | Autonomous |
Remote | Paused | HoldingAtBase | StationKeeping | ReturnToBase | Following |
EnteringRegion | AwaitingRearm | CommsLost | Parked |
Completed | Moving; outside autonomy Remote while throttle or rudder is
commanded, Moving while drifting with no command), state_external (a
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
Community name). route_plan: the safe-route planner note read from the same
route's description ("water >= 3 m, 10 m clearance, US3LA1CB", "-" when the
route has none). nav_status_msg: the navigation guard's reason
(NAV_STATUS_MSG, "-" when empty). control_holder is "lost:<user>" while
the last holder is not heard (until control is taken, handed over or
released). region_state: the region behaviour's state
(V26_STATE: contained, lapsed, breached ...). control_holder and
control_role: who holds control of the boat ("-" nobody), control_since_ms
(numeric, epoch ms of the last change), c2_sessions (open logins as
"alice (operator, 192.168.22.5); ...", "-" none) and c2_session_count
(numeric), from iWebSocket's audit lines, re-written every 5 s.
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
avoidance_active (0 idle, 1 helm avoiding: a contact in COLAV_ALERTS or an
avd*/colreg*/avoid* behavior running, 2 in extremis), colav_avoiding and
colav_extremis (counts of active COLAV_ALERTS entries and of those in
extremis; written once COLAV_ALERTS has been received), closest_range_m,
min_cpa_m and min_tcpa_s (the smallest CPA among the contacts, a contact
still closing to a CPA within 12 nm ranking ahead of one already opening;
-1 = none), planned_course (bearing to the
current waypoint), course_deviation (commanded minus planned), strings
worst_contact and reason (with COLAV_ALERTS: the contact and COLREGS
situation the helm is avoiding first). The legacy `radar` aggregates are
kept for the alert rules.
`colav`: one series per contact listed in COLAV_ALERTS (production
iWebSocket: the contacts the helm is steering around), tag `contact`
(the NODE_REPORT name, as in `contacts`), 1 Hz while listed: active (1
avoiding, 0 avoidance ended and held for colav_alert_hold, 15 s),
extremis (1 stand-on in extremis, COLREGS Rule 17(b)), for_s, range_m,
bearing, rel_bearing (0..360), cpa_m, tcpa_s (0 = opening), hdg, spd_kn,
mmsi (AIS contacts), -1 = unknown; strings situation (Head-on, Overtaking,
passing to port, Give-way, crossing astern, Stand-on, Stand-on, being
overtaken, Stand-on, in extremis, Keep clear (CPA), Avoiding), rule
(Rule 13 | 14 | 16 | 17 | 17(b), "-" for none) and mode (the behaviour's
own `base:submode`). A list that stops updating for 10 s is dropped (the
iWebSocket that posted it has stopped).

### readiness / readiness_summary (FR-12) — source: MOOS bridge, every readiness_period_s (5 s)
readiness, tag `check` (gps | course | enc | autonomy | apps | steering |
comms | can | engine | electrical | endurance | speed_cap | ais | cameras |
obc | safety | checklist): seq (display order), status (0 PASS, 1 FAIL, 2 N/A, 3
CONSOLE), name (string, operator wording), evidence (string, what was
seen). readiness_summary: ready (0/1), passed, failed, na, console,
checks, blocking (string, "; "-separated names of the failed checks, "-"
when none), profile ("sim" | "vessel"). checklist, tag `seq`: item,
description and state (strings; one row per item, TICKED | PENDING, then
a footer "Console checklist d / t · ticked by X" with state COMPLETE |
OPEN), erased and rewritten when the console's list changes. Decided by usv::EvaluateReadiness
(mission_logic.hpp) from the latest value of the bridge's own fields (the
chart.enc_* coverage among them) plus comms.signalk_ws_up, ping.*,
canbus.*, camera.connected and the Telegraf cpu/mem/disk fields polled
from InfluxDB. A change of verdict raises an event of type `readiness`;
a check turning FAIL or PASS raises one of type `readiness_check`.

### chart — source: MOOS bridge, on change
What the onboard chart server lists (Signal K `/signalk/v1/api/resources/charts`,
polled every enc_poll_s, default 60 s): enc_cells (number of cells of
type S-57; base maps and raster charts are not counted), enc_cell
(string: the most detailed ENC cell whose bounds contain the boat's
position, "-" when none), enc_covers (1 a cell covers the position, 0
none does, -1 no position or no cells to check). Feeds the Chart / ENC
readiness row when the console publishes no ENC_STATUS.

### geometry (map overlays) — source: MOOS bridge, on change
Vertex sets for the Navigation / Replay maps, tags `kind` (route |
region) and `idx` (zero-padded vertex index), fields lat, lon, seq. The
bridge converts the MOOS local grid (VIEW_SEGLIST of the survey/return
behavior, OPR_POLY_ACTUAL or the console's REGION_UPDATES) to geographic coordinates with the mission
file's LatOrigin/LongOrigin, deletes the previous set of the same kind
and writes all vertices with one timestamp, so `last()` per idx is
exactly the current geometry. The region outline is closed (first vertex
repeated). "NONE" clears the set. Written only when the datum is present.

### c2_session: source MOOS bridge, every 5 s
One row per open Swiftships-UI login, tag `session` (the tab session, or
user@ip for a login made with the Signal K cookie): user ("no login" for a
connection without one), role, ip. The Audit page reads the last 30 s.

### can_drive: source MOOS bridge (iCAN drive interface), on change and 1 Hz
status (string: ACTIVE | STANDBY | NO_BUS | INHIBITED), owner (AUTONOMY |
MANUAL), tx_gear (fwd | neutral | rev), trim_state (up | down | idle),
profile (drive profile name), conflict ("none" or the IDs another node
sends), tx_thrust_pct (negative in reverse), tx_rudder_pct,
rudder_limit_pct (while the speed-scaled limit is active), send (CAN_SEND
0/1), lost_link (0/1), rudder_mismatch (0/1).

### autonomy (FR-1/FR-3/FR-11) — source: MOOS bridge, 1 Hz snapshot
Numeric mirrors of the Alpha mission flags so they can be graphed and
alerted on (0/1 unless noted): deploy, waypoint, returning, stationing,
pause, armed, comms_lost, entry_gate_open, manual_override,
survey_complete, awaiting_rearm, helm_drive, all_stop (the operator ALL
STOP latch from iWebSocket, once it has been posted), nav_guard (0 ok |
1 warning | 2 fault, written only while the navigation guard reports, so
missing = silent or not fitted), allstop (1 only for
unexpected all-stop reasons; ManualOverride/NothingToDo are benign),
geofence_breached (0 ok | 1 breach | 2 recovering; decays to 0 after 30 s
without an OPR_BREACHED post), obstacle_alerts (count, 120 s TTL),
comms_phase (0 link ok | 1 grace, mission continues | 2 failsafe hold |
3 return to base; the simulation iWebSocket's COMMS_LOST phases),
remote_control (1 with REMOTE_CONTROL or while the derived state is Remote),
operator_cmd (1 while a throttle or rudder command is in force outside
autonomy), drifting (1 while the derived state is Moving), region_active
(REGION: the operating-region fence is enforced), crash_stop (CRASH_STOP), comms_loss
(COMMS_LOSS from the boats' iWebSocket: no frame from any Swiftships-UI for
comms_loss_timeout s), return_hold (RETURN_HOLD: holding station on the
base), comms_return (1 while the link is lost and the boat returns to or
holds on its base), operator_link (0 ok | 1 grace | 2 failsafe hold |
3 lost, returning to base | 4 no UI link, boat stays (helm parked and
idle) | 5 lost, holding on the base; the Operator Link tiles read it).
All numeric fields are written with 12 significant digits (6 before
v1.2.23, about 10 m of resolution for a latitude).

### autopilot (FR-3) — sources: MOOS bridge, CAN bridge
desired_heading, actual_heading (deg), desired_speed, actual_speed (kn),
rudder_cmd (deg in simulation; PERCENT on the vessel's iCAN), rudder_actual
(deg, $AGRSA via ANGULO_LEME or N2K 127245), throttle_cmd (%),
heading_error (deg, derived by the bridge as wrap(desired - actual), 2 Hz),
speed_error (kn, derived), speed_cap, cmd_speed (kn, planchaPID governor),
next_turn_angle (deg), pid_heading_out, pid_speed_out (legacy pMarinePID
names; planchaPID does not publish them), service_mode (0/1, SERVICE_MODE:
manual tuning gains in use), gain_heading_kp / gain_heading_ki /
gain_heading_kd / gain_speed_kp / gain_speed_ki / gain_speed_kd (the gains
planchaPID applies, from PID_GAINS, on change)

usv-722 CAN (can_bridge, usv722_can.hpp, steering actuator id 0x210 and the
Glendinning helm 18FF11D0): rudder_actual (deg, actuator feedback),
rudder_actuator_cmd (deg, command the actuator is following), helm_rudder
(deg, helm wheel), steer_drive_pct (%, steering motor), steer_pc_control
(1 = PC / autopilot steering, 0 = helm). Scale 0 / 511 / 1012 raw = full
port / centre / full starboard, x STEER_MAX_DEG (50).

usv-725 CAN (can_bridge, usv725_can.hpp, HELM_PROFILE=usv725): the same
fields from the two 0x210 frame kinds: byte 5 = 00 is the wheel command
(helm_rudder), byte 5 = 64 the actuator status (rudder_actual,
rudder_actuator_cmd, steer_drive_pct). Scale 10 / 511 / 1022 raw = full
port / centre / full starboard, x STEER_MAX_DEG (17). steer_pc_control is
not written (no "who it follows" byte on this actuator).

### engine (FR-4) — sources: MOOS bridge (iCANRec ENGINE_*, SAE J1939 on
slcan0), CAN bridge (N2K PGN 127488/127505 if an N2K bus exists), Signal K
One writer per vessel: `ENGINE_SOURCE` in /etc/usv/usv.env (signalk | moos |
can) names the producer of the engine fields and the other two leave them
alone (usv-722: signalk once the Yamaha gateway is fitted; usv-725: moos).
Unset, every producer writes, as before v1.2.20.
Tags: `instance` (0 for a single engine, 0/1 port/starboard; on every engine
point), `engine_type` (ENGINE_TYPE from usv.env, e.g. yamaha_outboard,
mercury_diesel; the hidden `engine_type` dashboard variable reads it for the
Vessel Health and Engine dial scales) and `engine_model` (ENGINE_MODEL, e.g.
"Yamaha Outboard Engine F115 B 4-Stroke", line-protocol escaped; the hidden
`engine_model` variable shows the most recent one in the header chip, and
the Fleet engine tile prints it). Fields: rpm,
coolant_temp, oil_temp (°C), oil_pressure, intake_manifold_pressure (kPa),
fuel_level (%), fuel_remaining_l (L, fuel_level x VESSEL_FUEL_L, only when
usv.env sets the capacity), fuel_rate (L/h), runtime_h (h), percent_load,
percent_torque, pedal_position, trim_position (%), active_dtcs (count),
run_state (string: running | stopped | unknown, from pEngineWatch's
ENGINE_RUN_STATE through grafana_bridge, on change and once a second),
profile (1, written every 60 s by signalk_bridge on every vessel with the
engine_type and engine_model tags of its usv.env, so the dashboards name the
engine before or without any engine data; never charted).
Signal K (signalk_bridge, signalk_engine.hpp; the Yamaha CL7 / N2K gateway
on usv-722 through the server's n2k-signalk mapping): every
`propulsion.<id>.*` the server publishes, `<id>` = N2K engine instance or
the name the server was given (0 / port -> instance 0, 1 / starboard ->
instance 1): revolutions (Hz x 60 -> rpm), temperature or
coolantTemperature (K -> coolant_temp °C), oilPressure (Pa -> oil_pressure
kPa), oilTemperature (-> oil_temp), boostPressure (Pa ->
intake_manifold_pressure kPa), fuel.rate (m³/s -> fuel_rate L/h), runTime
(s -> runtime_h), engineLoad and engineTorque (ratio -> percent_load /
percent_torque), drive.trimState (ratio -> trim_position %),
transmission.gear (Forward 1 / Neutral 0 / Reverse -1 -> gear),
alternatorVoltage (-> electrical.alternator_voltage);
`tanks.fuel.<SIGNALK_FUEL_TANK>.currentLevel` (ratio -> fuel_level %, the
first fuel tank seen when the key is unset); `notifications.propulsion.
<id>.<name>` (one per N2K 127489 status bit, state alarm / warn / alert /
emergency, back to normal or null when clear) -> active_dtcs (count of
active notifications), dtc_text ("NONE" or "Check Engine (alarm); Low Oil
Pressure (alarm): <message>; ..."), lamp_mil (checkEngine), lamp_red
(emergencyStopMode, engineShuttingDown, overTemperature, lowOilPressure),
lamp_amber (any other active notification). Written only when
ENGINE_SOURCE is signalk (or unset).
MOOS (grafana_bridge, from iCANRec's ENGINE_* variables on the J1939 bus
of usv-725): the fields above in display units as iCANRec publishes them,
ecu_voltage (V, the ECM supply, J1939 VEP1), dtc_spn / dtc_fmi / dtc_oc
(first active fault), fault_lamps and the four lamp fields from
ENGINE_LAMPS, dtc_text from ENGINE_DTC_TEXT with the Mercury fault names
added on a mercury_diesel hull ("SPN 100 FMI 18 J143 Oil_Press_Low (x1)",
cpp-common/j1939_faults.hpp, 61 SPN / FMI pairs from the Mercury
specification) or the J1939 failure-mode wording elsewhere. The instance
tag comes from the variable suffix (ENGINE_SPEED instance 0, ENGINE_SPEED_1
instance 1). ENGINE_BATTERY_VOLTAGE no longer feeds electrical or power.
J1939 engine ECU (can_bridge, j1939_engine.hpp; ENGINE_SOURCE=can, engine
at J1939_ENGINE_SA, 0 on usv-725 Mercury): rpm, percent_torque (actual),
demand_torque (%), percent_load, pedal_position, coolant_temp, fuel_temp,
oil_temp (16-bit SPN 175), oil_level (%), oil_pressure, fuel_rate,
intake_manifold_pressure, intake_manifold_temp, exhaust_temp (°C),
baro_pressure (kPa), air_inlet_temp (°C), ecu_voltage (V), water_in_fuel
(0/1), runtime_h (only when CAN_ENGINE_HOURS_REQUEST=1, the ECU sends it on
request), and from the engine's DM1 (single frame or BAM) active_dtcs,
fault_lamps, lamp_protect / lamp_amber / lamp_red / lamp_mil, dtc_spn /
dtc_fmi / dtc_oc and dtc_text (Mercury J codes on a mercury_diesel hull).
fuel_level from the NMEA 2000 tank sender (PGN 127505, fuel tank 0). Values
in the J1939 not-available range are not written. Each value is written on
change and at least every 0.5 s (10 Hz at most).
usv-725 helm (can_bridge, usv725_can.hpp, HELM_PROFILE=usv725):
throttle_helm_pct and throttle_pct (EEC2 from 208, the lever; usv-725 has
no control processor, so the lever is also what the engine controller
acts on), gear_helm and gear (ETC2 from 208), trim_switch (PGN 65288 from
208), key_state (PGN 65295 from 16, any priority), speed_request (rpm, TSC1
from the controller at 23 to the engine: 700 at idle to 4200 at full
lever).
usv-722 CAN (can_bridge, usv722_can.hpp; RPM and temperatures are not on
its bus): key_state (0 off, 1 run, 2 start), gear and gear_helm (-1 reverse,
0 neutral, 1 forward; to the engine / at the lever), throttle_pct and
throttle_helm_pct (% thrust, J1939 EEC2), trim_switch (1 up, -1 down,
0 idle; the helm button only, trim position is not on this bus), and the
J1939 DM1 from address 210 (single frame, or the BAM multi-frame form when
two or more faults are active): active_dtcs (count of active faults),
fault_lamps (raw lamp byte), lamp_protect / lamp_amber / lamp_red / lamp_mil
(0/1, lamp on), dtc_spn / dtc_fmi / dtc_oc (first active fault: suspect
parameter number, failure mode, occurrence count; 0 when none) and the string
dtc_text ("NONE", "N/A" when the fault bytes are not available, or
"SPN 110 FMI 3 voltage high / short to high (x1); SPN ..." listing every
active fault; with the Mercury fault names on a mercury_diesel hull).
Not produced in the simulation environment (no engine).

### electrical (FR-4) — sources: CAN bridge (PGN 127508), Signal K
Tag: `instance`. Fields: battery_voltage (V), battery_current (A), soc (%),
charging_status (string), alternator_voltage (V)

### power (FR-4) — source: Signal K
total_power (W), system_voltage (V), distribution_ok (0/1)

### vessel — source: signalk_bridge, every 60 s (VESSEL_* in usv.env)
Static hull particulars from the general arrangement drawing. Tag
`vessel_class` (VESSEL_CLASS). Fields: class_name (string); one string field
per particulars-card row, named "<nn> <label>" in display order ("01 Length
x beam x height" = "5.9 m x 1.8 m x 2.5 m", "02 Payload", "03 Fuel
capacity", ...; rows without data are not written); fuel_l (L),
max_speed_kn (kn), range_nm (nm), loa_m (m). Written only when at least one
VESSEL_* key is set. Used by the header band (vessel_class), the Readiness
particulars card and nothing else; grafana_bridge reads the same keys from
usv.env for the Endurance and Speed cap checks.

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

### audit (v1.2.43): the boat control audit trail, kept a year
One row per iWebSocket audit line (CONTROL_EVENT / UI_AUTH_EVENT) and per
navigation-guard and region event. Tags: `vessel`, `category` (control |
login | refused | all_stop | nav_guard | region), `action` (take, handover,
force, release, request, deny, cancel, lost, revoke, refused, all_stop,
all_stop_reset, start, login, logout, anonymous, disconnect, rejected,
fault, warn, ok, stop, note), `severity` (info | warning | critical),
`user` and `role` ("-" when none), `source`. Fields: message (operator
wording), details (generation, from, reason, ip, session, level, why ...;
"-" when none), value (1). A forced takeover's grant and reason lines are
one row. The events bucket's retention is at least 365 days (installer).

### events (FR-1/FR-11)
Tags: `vessel`, `type` (state_change | waypoint | mission | alarm |
geofence | obstacle | radar | control | engine), `severity` (info | warning | critical),
`source`, `mission_id` (type=mission only, from ID_ROUTE).
type=engine (v1.2.51): pEngineWatch's ENGINE_ALERT
(`key=<key>,level=<warn|alarm|emergency|clear>,msg=<text>`), one event per
change of an alert's level: emergency and alarm critical, warn warning,
clear info with "(cleared)" after the text; a repeat of the same level and
a clear for an alert not raised in this bridge session write nothing.
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
