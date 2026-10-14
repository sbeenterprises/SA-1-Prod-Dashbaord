# Changelog

All notable changes to the Swift Autonomous Dashboard. The format follows
Keep a Changelog; versions are git tags on `main`.

## [1.2.27] - 2026-10-14 (Min CPA ignores far closing ships)

grafana_bridge only. Update with `bash deploy/update.sh v1.2.27`, then
restart the mission (grafana_bridge changed). No usv.env change.

### Fixed
- Traffic Min CPA / TCPA (tiles, Closest CPA / TCPA trend, radar.cpa_m):
  a contact closing on its CPA ranked ahead of every opening contact, so an
  AIS ship about 40 nm out closing to a CPA of about 75 km took over from
  two contacts opening at 1.7 km (usv-722, 2026-10-14 01:13 to 01:27) and
  pushed the trend to 80,000 m. Closing-first now applies only to a CPA
  within 12 nm; beyond that a contact ranks by CPA with the rest. Risk
  levels, the High Risk count and the collision alert are unchanged.

## [1.2.26] - 2026-10-14 (Traffic table and trend labels)

Dashboards only. Update with `bash deploy/update.sh v1.2.26`, Ctrl+F5. No
bridge change, no mission restart.

### Fixed
- Traffic, Contacts table: TCPA reads "opening" for a contact past its
  closest approach instead of "-1 s", as the TCPA tile does.
- Traffic, Closest CPA / TCPA trend: the series are named CPA and TCPA
  (they showed the field names min_cpa_m / min_tcpa_s) and drawn in two
  colours, CPA blue on the left axis, TCPA orange on the right.

## [1.2.25] - 2026-10-14 (Traffic layout)

Dashboards only. Update with `bash deploy/update.sh v1.2.25`, Ctrl+F5. No
bridge change, no mission restart.

### Fixed
- Traffic: the Contacts table (narrowed in 1.2.24 for the Helm Avoiding
  table) hid CPA, TCPA, Encounter and Risk behind a scrollbar. It now shows
  Contact, Range, CPA, TCPA, Encounter and Risk without scrolling; source
  and bearing stay on the Traffic View and the highest-risk card.
- Traffic View: the longer legend ran under the 180 bearing label. Legend
  shortened (AIS, radar, HIGH, avoiding, 6 min) and the plot raised
  slightly so the label clears it.

## [1.2.24] - 2026-10-14 (helm collision avoidance on the Traffic page)

Follows the 13 October changes in moos-ivp-morgan-stern-drive-mercury
(iWebSocket: COLAV_ALERTS, the contacts the helm is avoiding, with the
COLREGS situation). Update with `bash deploy/update.sh v1.2.24`, restart
the MOOS community (grafana_bridge changed), Ctrl+F5. No usv.env change.
Nothing changes on a vessel or simulator that does not publish
COLAV_ALERTS.

### Added
- grafana_bridge reads COLAV_ALERTS (JSON list from iWebSocket) and writes
  a `colav` row per contact (situation, COLREGS rule, CPA, TCPA, range,
  bearings, contact course and speed, active / ended, in extremis) and
  traffic.colav_avoiding / colav_extremis. A list that stops updating for
  10 s is dropped. Events: helm avoiding (warning, with the situation and
  CPA), in extremis (critical), avoidance ended (info).
- Traffic: Avoidance tile IN EXTREMIS (red) next to IDLE / ACTIVE; the
  Reason tile names the contact and situation; orange rings (red in
  extremis) on the contacts the helm is avoiding in the Traffic View; new
  Helm Avoiding (COLREGS) table in the bottom row.
- Alert rule Collision Avoidance IN EXTREMIS, critical (24 rules).

## [1.2.23] - 2026-10-14 (comms-loss return to base, return base, helm speed units)

Follows the 13 October changes in moos-ivp-morgan-stern-drive-mercury
(iWebSocket: return to base on loss of the Swiftships-UI link). Update with
`bash deploy/update.sh v1.2.23`, add `NAV_SPEED_UNITS=knots` to
/etc/usv/usv.env on a vessel, restart the MOOS community (grafana_bridge
changed), Ctrl+F5.

### Added
- grafana_bridge reads the boats' comms-loss failsafe: COMMS_LOSS (no frame
  from any Swiftships-UI for comms_loss_timeout s), RETURN_HOLD (holding
  station on the base after the return) and BASE_COORDS (the return base,
  `lat,lon`). New mission state HoldingAtBase ("Holding at Base"); new
  autonomy fields comms_loss, return_hold, comms_return and operator_link;
  mission.base_lat / base_lon. Events: link lost, failsafe return
  (critical), arrival hold, link restored (the return or hold continues
  until the operator's next command), return base changed.
- Operator Link tiles (Mission, Readiness, Fleet, Alarms) read
  autonomy.operator_link: LINK OK, GRACE / LOST — FAILSAFE / LOST — RTB
  (simulation), NO UI LINK (lost with the boat parked and idle: it stays,
  no alarm), LOST — RTB, LOST — AT BASE.
- Alert rule Operator Link Lost (Returning to Base), critical, while the
  comms-loss failsafe drives the boat (23 rules).
- Navigation: the return base as an orange BASE square on the map, and a
  Dist to Base tile (nm, great circle from the last position).
- `NAV_SPEED_UNITS` (usv.env) / `nav_speed_units` (mission block): the
  units of the helm's speeds. The boats publish NAV_SPEED in knots; the
  bridge had converted it as m/s, so the Autopilot actual / desired speed,
  speed cap, commanded speed and speed error read 1.94 times too high on a
  vessel. Unset keeps m/s (the simulator).

### Fixed
- Numeric fields are written with 12 significant digits instead of 6: a
  latitude of 29.7219xx was stored as 29.7219, about 10 m of resolution, on
  the map track, the vessel position and every derived distance.
- The smoke test's dashboard-field check pairs each measurement with its
  own fields (a query reading nav lat/lon and mission base_lat/base_lon no
  longer counts mission.lat as queried).

### Unchanged
- The simulation iWebSocket's COMMS_LOST / MISSION_ARMED handling, the
  CommsLost and AwaitingRearm states and their alert rules.

## [1.2.22] - 2026-10-14 (engine name in the header without engine data)

Update with `bash deploy/update.sh v1.2.22`, then Ctrl+F5. No MOOS restart.

### Fixed
- The header read "Engine ****" on a vessel that has written no engine
  point yet (usv-722 before the Yamaha gateway): the engine name came only
  from engine data. signalk_bridge now writes `engine.profile = 1` every
  60 s with the engine_type and engine_model tags of this vessel's usv.env
  (metadata, on every vessel whatever ENGINE_SOURCE says), and the header
  falls back to the engine type, then to "not reported", never to an empty
  name. The variable query maps before grouping, so the engine
  measurement's float and string fields cannot collide.
- tools/check_flux.js also type-checks the Flux queries behind dashboard
  variables.

## [1.2.21] - 2026-10-14 (Vessel Health summary, dial legibility, engine name, charging state)

Dashboards and signalk_bridge. Update with `bash deploy/update.sh v1.2.21`,
then Ctrl+F5. Add `ENGINE_MODEL="..."` (quoted) to /etc/usv/usv.env first;
restart the MOOS community afterwards so grafana_bridge tags the engine
points with the name.

### Added
- `ENGINE_MODEL` in usv.env -> `engine_model` tag on every engine point
  (line-protocol escaped); the header chip and the Fleet engine tile show
  it (usv-722 "Yamaha Outboard Engine F115 B 4-Stroke", usv-725 "Mercury
  Diesel 2.0L"). Default from the type. install.sh stops with a clear
  message when a value with spaces is not quoted (the file is read by
  bash).
- signalk_bridge: with a battery monitor that publishes no chargingMode
  (Victron SmartShunt / BMV over VE.Direct, usv-722) the Charging tile
  follows the battery current: CHARGING above +0.5 A, DISCHARGING below
  -0.5 A, IDLE between. `SIGNALK_BATTERY_CURRENT_SIGN=-1` corrects a shunt
  that reports charging as negative current (current and derived power).
  Charger modes (bulk, absorption, float, off) have their own wording.
- Vessel Health tiles: Engine Faults (links to the Engine page), System
  Voltage, Alternator, ECU Supply (the Mercury ECM supply, mapped since
  v1.2.20 but not shown).

### Changed
- Vessel Health is the whole-vessel summary: the Fuel Rate, Engine Hours,
  Oil Temp, Engine Load and Boost tiles and the Engine RPM and Coolant /
  Oil Pressure trends were duplicates of the Engine page and are removed;
  the electrical and power trends are wider (two rows of two).
- Needle meters and the rudder indicator size themselves to their panel
  (geometry 0.6x to 3x of the 260 px reference, fonts with a legible
  floor): the readout no longer runs into the label on the small Vessel
  Health dials; ticks sit clear of the numerals, which had read like minus
  signs ("-0", "-40"); no fill dot at the start of the scale at zero.
- A tile whose lowest threshold is an alarm colour (Fuel Level) shows "No
  data" in the neutral colour instead of red.
- "Freeboard" -> "Swiftships-UI" in panel titles, descriptions and docs
  (Readiness console checklist).

## [1.2.20] - 2026-10-14 (engine profile, Engine page, USV-725)

Per-vessel engine profile so one release serves the Yamaha hull (usv-722)
and the Mercury diesel hull (usv-725). Requirements:
docs/requirements-usv-725.md; release and installation steps:
docs/release-1.2.20-usv-725.md. First install on usv-725 with
`deploy/install.sh` from this tag; on usv-722 add ENGINE_TYPE,
ENGINE_SOURCE and HELM_PROFILE to /etc/usv/usv.env, then
`bash deploy/update.sh v1.2.20`, restart the MOOS community (grafana_bridge
reads the profile at start) and Ctrl+F5 in the browser.

### Added
- `/etc/usv/usv.env` engine profile keys: ENGINE_TYPE, ENGINE_SOURCE
  (signalk | moos | can), ENGINE_COUNT, ENGINE_INSTANCES, HELM_PROFILE, and
  the overrides SIGNALK_ENGINE, CAN_N2K_ENGINE, CAN_DM1, MOOS_ENGINE
  (ingestion/cpp-common/engine_profile.hpp, shared by the three bridges;
  unit tests in engine_profile_test.cpp).
- Single writer per engine field: with ENGINE_SOURCE set, only the named
  producer writes the `engine` measurement. signalk_bridge skips the
  propulsion paths unless the source is signalk; can_bridge skips N2K
  127488 / 127505 unless the source is can and skips the J1939 DM1 faults
  when the source is moos (iCANRec reports the engine's own faults then);
  grafana_bridge passes ENGINE_* through only when the source is moos.
  grafana_bridge reads the keys from /etc/usv/usv.env itself (it runs under
  pAntler, outside systemd); `engine_passthrough` and `engine_type` in the
  mission file override them.
- `engine_type` tag on every engine point, and `instance=0` on the Signal K
  and drive-bus engine points (the N2K points already carried the instance).
- install.sh validates the profile, prints it, and for ENGINE_SOURCE=moos
  prints the grafana_bridge block to put in the mission file.
- can_bridge: HELM_PROFILE=none turns the usv-722 helm / drive decoder off
  on a hull whose frames have not been captured yet; bus statistics stay.
- grafana_bridge (MOOS, usv-725): ENGINE_<NAME>_<n> variables carry
  engine instance n (ENGINE_SPEED_1 -> instance=1), one write-rate ceiling
  per field and instance; ENGINE_LAMPS (DM1 lamp byte) -> fault_lamps and
  lamp_protect / lamp_amber / lamp_red / lamp_mil; ENGINE_DTC_SPN / FMI /
  OC -> dtc_spn / dtc_fmi / dtc_oc; ENGINE_DTC_TEXT -> dtc_text, written on
  change and every 5 s, with the Mercury fault names added on a
  mercury_diesel hull ("SPN 100 FMI 18 J143 Oil_Press_Low (x1)");
  ENGINE_BATTERY_VOLTAGE -> engine.ecu_voltage (it no longer feeds
  electrical.battery_voltage or power.system_voltage, which come from the
  Victron shunt through Signal K on both hulls).
- cpp-common/j1939_faults.hpp: the J1939-73 failure-mode wording and the
  Mercury Diesel 2.0L SPN / FMI fault table (65 rows, 61 pairs, Table 2 of
  the Mercury J1939 specification) shared by can_bridge and grafana_bridge;
  can_bridge's DM1 text uses the Mercury names when ENGINE_TYPE is
  mercury_diesel. Unit test j1939_faults_test; the usv722_can_test covers
  the Mercury wording.
- signalk_bridge engine paths for the Yamaha gateway on usv-722
  (ingestion/signalk-cpp/signalk_engine.hpp): every `propulsion.<id>.*`
  the server publishes, with `0` / `port` -> instance 0 and `1` /
  `starboard` -> instance 1 (the fixed `propulsion.0.*` list is gone);
  `temperature` as well as `coolantTemperature` for coolant; new
  oilTemperature, boostPressure, engineLoad, engineTorque,
  drive.trimState (trim_position), transmission.gear and
  alternatorVoltage mappings; the fuel level from
  `tanks.fuel.<SIGNALK_FUEL_TANK>.currentLevel`; the server's
  `notifications.propulsion.<id>.*` tracked per engine into active_dtcs,
  dtc_text ("Check Engine (alarm); ...") and the lamp fields. Unit test
  signalk_engine_test (95 checks, builds without IXWebSocket) and smoke-test
  vectors for every path in both id forms (SK-1 to SK-3 of the
  requirements).

- Engine page (tab after Vessel Health, own panel ids 9401+): four large
  dials (Tachometer, Coolant, Oil Pressure, Boost) at 1 s, oil temp, fuel
  rate, load, torque, hours, fuel level, fault lamps and active fault tiles,
  the fault codes across the width, RPM / temperature / pressure trends over
  15 min. The Vessel Health engine dials link to it.
- Per-engine-type dial scales: the Tachometer, Coolant and Oil Pressure
  dials (Vessel Health, Engine) read the hidden `engine_type` dashboard
  variable (the engine_type tag of the selected vessel) when they render
  and pick their scale from the ENGINE_TYPES table in gen_dashboards.py:
  yamaha_outboard (0 to 6000 rpm, as before) or mercury_diesel (0 to 5000
  rpm, green to 4000 = wide-open throttle per the Mercury 2.0L spec sheet;
  oil pressure 0 to 800 kPa). Unknown or empty = the outboard scale.
- Header band shows the engine type after the vessel name on every page.
- Fleet page: Engine tile per vessel (engine type and last RPM).
- Alert rules: Engine Low Oil Pressure (oil_pressure below 150 kPa while
  rpm above 700, so a stopped engine never fires it) and Engine Fault
  (active_dtcs above 0). 22 rules.
- Readiness "Engine pre-start": rpm in the evidence; oil pressure below
  150 kPa with the engine running above 700 rpm fails the check; 0 kPa with
  the engine stopped does not.

### Changed
- Analog instruments restyled (every needle meter: the engine dials on
  Vessel Health and the Engine page, Fuel, Battery, State of Charge, Speed
  Over Ground, Throttle, Heading Error, and the rudder indicator): a dark
  radial dial face, a glowing teal arc that fills to the value over a faint
  rail, the alarm bands as a thin ring outside the arc (green normal, yellow
  caution, red alarm, thresholds unchanged), tick marks and numerals inside
  it, a hairline needle and a large digital readout with the unit in small
  type and the label beneath. The heading-error meter keeps the needle and
  the ring without the fill (its scale is centre-zero); the rudder keeps
  its red / green halves on the rail. N/A: no needle, no fill, "N/A" in the
  readout. Light theme carries the same design on a light face. The
  preview renderer mirrors the new look. Yamaha oil pressure numerals every
  200 kPa (ticks every 100) so the small dial stays legible.
- Vessel Health: the Boost tile (intake manifold pressure, J1939 IC1)
  replaces the Alternator tile, which no hull feeds.
- usv.env.example documents the profile with the usv-722 and usv-725 values,
  and SIGNALK_FUEL_TANK.
- Fault Codes tiles colour any fault list red, whichever bridge wrote it
  (the mapping matched only the J1939 "SPN ..." form).
- Docs: the usv-722 runbook and commissioning record said the engine sat
  behind a Mercury SmartCraft network; usv-722 is the Yamaha outboard hull
  (its data arrives through the Yamaha Command Link / N2K gateway and
  Signal K) and usv-725 is the Mercury diesel sterndrive. The mission
  repository name `moos-ivp-morgan-stern-drive-mercury` is historical.
- ENGINE_SOURCE unset keeps the previous behaviour (every producer writes),
  so an existing install is unchanged until the key is added.

- tools/smoke_test.sh pins an empty engine profile (USV_ENV_FILE, ENGINE_*
  unset) so a vessel's own usv.env cannot change the test vectors, and
  accepts a Flux query built from a variable alone (array.from, the
  Traffic View range), flagged as incomplete since v1.2.15.
- docs/validation-report.md section 10: smoke test 120 PASS / 0 FAIL / 1
  SKIP for this release candidate, with the bridge-level checks.

### Notes
- After this update the drive-bus engine fields on usv-722 (gear, throttle,
  key, trim, faults) carry `instance=0`; for one dashboard time range after
  the update a tile can show the pre-update and the post-update series side
  by side. It clears once the range no longer spans the update.

## [1.2.19] - 2026-10-12 (trim switch and fault details)

CAN bridge and dashboards. Update with `bash deploy/update.sh v1.2.19`, then
Ctrl+F5 in the browser. The installer rebuilds and restarts the CAN bridge
(usv-can-bridge); no MOOS restart needed.

### Added
- Trim switch tile (UP / DOWN / IDLE, the helm's trim button; trim position
  is not on the usv-722 bus) on the Autopilot page (its own row), Autopilot
  Live and CAN Bus.
- CAN bridge decodes the J1939 DM1 fault message from address 210 in full:
  each warning lamp (lamp_protect, lamp_amber, lamp_red, lamp_mil), the first
  fault's SPN / FMI / occurrence count, the number of active faults, and a
  summary text (dtc_text) listing every fault with its failure-mode meaning.
  Two or more faults arrive as a J1939 broadcast (TP.CM BAM + TP.DT), which
  the bridge now reassembles; before, only the single-frame form was read.
- CAN Bus page: Trim Switch, Active Faults, Fault Lamps and Fault Codes row.
- Autopilot Live: Engine Faults tile.

### Changed
- Traffic page refreshes every 1 s (was 5 s).
- Autopilot page: the three trend charts are shorter (the trim row is added
  and the page still fits one screen).
- engine.active_dtcs is now the number of active faults (was 0/1); the
  Engine Faults tiles read NONE / ACTIVE as before.

### Verified
- usv722_can_test: 74 passed (37 new: lamps, SPN / FMI / OC, 19-bit SPN,
  not-available bytes, summary text, BAM reassembly, lost packet, other PGN
  and other sender ignored). can_bridge built with -Wall -Wextra, no
  warnings, and run end to end on a scripted frame sequence (trim up / down,
  one fault, three faults via BAM, cleared): every value written correctly.
- 205 Flux queries type-check; 10 of 14 dashboards byte-identical to
  v1.2.18.

## [1.2.18] - 2026-10-12 (live monitoring)

Dashboards and installer. Update with `bash deploy/update.sh v1.2.18` (the
installer step allows the 1 s refresh), then Ctrl+F5 in the browser; no MOOS
restart needed.

### Added
- Autopilot Live (`usv-autopilot-live`): the rudder, throttle and
  heading-error dials drawn large for sea trials and troubleshooting, with
  helm state, steering mode, gear, engine thrust, speed error and XTE tiles
  and the heading / rudder / speed trends over the last 5 minutes. Refreshes
  every 1 s. Dial text, rails and needle scale with the panel size (1x to
  3x). Opened from the link on each dial of the Autopilot page; not in the
  tab row, which is full at 1532 px. It uses only queries the Autopilot page
  already runs, so no bridge or database change.

### Changed
- Autopilot page refreshes every 1 s (was 5 s): the dials followed the boat
  3 to 4 s late because of the refresh interval, not the data path
  (grafana_bridge writes at up to max_rate_hz).
- deploy/install.sh sets Grafana's `min_refresh_interval` to 1 s (Grafana's
  default floor is 5 s, which hides the 1 s option). `GRAFANA_MIN_REFRESH`
  in usv.env overrides it. Other pages keep 5 s.

### Fixed
- Autopilot and Autopilot Live list 1 s in their refresh picker. Grafana
  drops a refresh interval that is not in the picker's list (the built-in
  list starts at 5 s), so the first 1 s build loaded with auto-refresh off
  (seen on usv-722).

## [1.2.17] - 2026-10-10 (Replay page tidy-up)

Dashboards only. Update with `bash deploy/update.sh v1.2.17`, then Ctrl+F5
in the browser; no MOOS restart needed.

### Fixed
- Replay: the Mission State timeline (3 rows) showed its legend, which
  pushed the bar 11 px out of the panel and gave it a scrollbar. The legend
  is hidden, as on the Mission page.
- Replay: Mission Events uses the same table layout as the Mission page
  (ISO time 150 px, coloured severity 80 px, newest first), so the event
  text gets the rest of the width. Both pages now share one definition
  (`EVENTS_TABLE_OVERRIDES`).

### Verified
- All 13 pages measured live on usv-722 (Grafana 13.2, 1532 x 900, menu
  closed): no page scroll, lowest panel at 867 px or above, no overlapping
  panels, no cut titles, no clipped tile values. The only scrolling left
  is inside the event and contact tables, which scroll by design.

## [1.2.16] - 2026-10-10 (no cut panel titles)

Dashboards only. Update with `bash deploy/update.sh v1.2.16`, then Ctrl+F5
in the browser; no MOOS restart needed.

### Fixed
- Panel titles: measured on usv-722 (Grafana 13.2, 1532 px), a 3-column
  tile shows 106 px of title. "Station Keeping", "Steering Control" and
  "Thrust to Engine" were cut; they are now "Station Keep", "Steering
  Mode" and "Engine Thrust", and Readiness shows its counts two per row.
  The generator fails the build when any title would be cut
  (`check_titles`, Inter Medium 14 px widths).

## [1.2.15] - 2026-10-10 — usv-722 CAN helm and drive, one-screen pages, Traffic zoom

Update an installed vessel with `bash deploy/update.sh v1.2.15` (see
`docs/runbook-usv-722.md`). Restart the MOOS mission afterwards to pick up
the rebuilt `grafana_bridge`.

### Added
- `deploy/update.sh`: one-step update to a release tag (fetch, checkout,
  install, service status, printed rollback command); refuses to run over
  local edits.
- usv-722 helm and drive on the CAN bus (`can_bridge`, `usv722_can.hpp`,
  from USV-722-CAN-ID-Mapping.xlsx): steering actuator feedback (0x210),
  the command it follows, steering motor drive and helm / autopilot control,
  helm wheel, thrust and gear at the lever and to the engine, key switch,
  trim switch and J1939 DM1 faults. Steering calibration via
  STEER_MAX_DEG / STEER_RAW_PORT / STEER_RAW_CENTER / STEER_RAW_STBD.
- Readiness: Steering gets real rudder feedback from the actuator; Engine
  pre-start uses key switch, gear, thrust and DM1 when the engine's own
  sensors are not on the bus ("key RUN · gear NEUTRAL · thrust 0 % ·
  fuel 23 % · no DTC"; key OFF fails).
- Autopilot page: rudder command vs actual trend and a CAN row (Rudder
  Actual, Helm Wheel, Steering Drive, Steering Control, Gear, Thrust to
  Engine, Key Switch, Engine Faults).

### Fixed
- `install.sh` restarts `usv-can-bridge` and `usv-signalk-bridge` after
  rebuilding them. Before, an update left both running the old binary until
  someone restarted them by hand.
- Professional one-screen layouts, laid out by hand rather than squeezed:
  every page is checked for no overlap, no gap, no clipped text and at most
  19 grid rows (1532 px wide reference screen).
  - Autopilot: two tile rows, three dials, three full-height trends
    (heading, rudder, speed), CAN row. The Throttle Output and Heading &
    Speed Error trends were dropped (they repeat the throttle dial and the
    error tiles).
  - Navigation: map and instrument column end on the same line; compass
    readouts (HDG, COG) moved outside the dial, where they overlapped the
    120 / 240 labels.
  - Mission: bottom row aligned to the 12 / 12 grid.
  - Traffic: even 2-row tiles, "Cmd Course" (was truncated), contact table
    16 columns wide and 5 rows high.
  - Cameras: table shows all 7 cameras; reserved Bitrate / PTZ /
    Temperature charts shorter.
  - System: services as one UP / DOWN tile per unit (the table showed two
    rows); Disk I/O named by device; CPU Temp one line (hottest core)
    instead of 24.
  - Replay: help strip removed (text in the user guide); Autonomy Flags
    given room so its labels no longer overlap.
  - CAN Bus: PGN table is one table ("PGN / CAN id", "msgs/s"); it had
    split into a frame per series with a selector.
- Tile text: 2-row tiles use 18 / 20 px values (26 px clipped), and every
  tile's value size is capped so its longest state fits the tile width
  (Grafana does not shrink a fixed size). Rudder dial: the ±35 end labels
  are hidden (they sat under the PORT / STBD readout).
- Readiness: all 16 automatic checks show without scrolling (full-height
  table on the left, no column header) and the verdict, counts, mission
  state, operator link and route stack on the right at full tile height
  (the squeezed tiles clipped "NOT READY" and "LINK OK").
- Every dashboard page now fits one screen (19 grid rows) without
  scrolling. Layouts are compressed by the generator (`fit_to_screen`),
  keeping side-by-side panels aligned; dials keep at least 5 rows.
- Comms: the VPN tile reads Twingate (client or connector), Tailscale,
  OpenVPN or WireGuard units (Telegraf now watches `twingate*.service`,
  `openvpn*.service` and `wg-quick@*.service` too) and is UP when
  any is active. The bandwidth chart shows the 3 busiest links only, and CAN
  and VPN interfaces are no longer listed as network links.
- Fleet: tiles no longer truncate their titles (second line for Mission
  Progress, Signal K and MOOS); empty SOG shows "—" and no mission shows
  "No mission".
- Traffic: + / − / A buttons on the Traffic View zoom the range
  (0.25 to 24 nm, A = auto); also a "Traffic range" selector in the
  variable bar. Contacts outside a zoomed range are counted ("+N beyond").
- Autopilot rudder units: the Rudder Angle dial now shows the real rudder
  angle from the steering actuator (`rudder_actual`, degrees) and falls back
  to the command, labelled COMMAND without a degree sign, when there is no
  CAN feedback. The rudder trend plots actuator command vs actual in degrees
  and the PID output (DESIRED_RUDDER, percent on the vessel) on its own
  right-hand axis. Replay's Actuator Commands panel labels the rudder
  command as percent.
- Dial sub-labels (PORT / RUDDER / STBD, COMMANDED THRUST, OFF COURSE and
  the Vessel Health gauges) no longer cut off at the bottom of short panels.
- Course planning check / map region: the operator console sends the
  selected operating region to MOOS as `REGION_UPDATES`
  (`core_poly=pts={...}`, for BHV_OpRegionV24), which the bridge did not
  read, so Readiness reported "no operating region" and the maps drew no
  outline. The bridge now takes the region from `REGION_UPDATES` as well as
  `OPR_POLY_ACTUAL`.
- Alarms page Camera tile: showed one ONLINE/OFFLINE value per camera in a
  single small tile, so the texts overlapped. It now shows one number, the
  enabled Frigate cameras that are not streaming ("ALL ONLINE" or
  "N offline"), from `camera_summary`; per-camera detail stays on the
  Cameras page.

## [1.2.14] - 2026-10-08 — Signal K own-ship fix

### Fixed
- Own-ship track, position, speed and heading jumping between other boats:
  `signalk_bridge` subscribed with `subscribe=all` and wrote every delta,
  including AIS targets' `navigation.position`, `speedOverGround` and
  `headingTrue`, as own-ship `nav` fields. It now reads the own-vessel
  context from the Signal K hello message and skips deltas for any other
  context; the default URL and `usv.env.example` use `subscribe=self`. The
  traffic range / bearing / CPA, computed from the own fix, were affected
  too. Smoke test: an AIS delta in the mock stream must not be written
  (fails on the previous bridge).

## [1.2.13] - 2026-10-07 — Traffic symbols, speed vectors, test coverage

### Changed
- Traffic View: contacts drawn in the ECDIS / OpenCPN AIS target style, a
  hull rotated to the contact's heading (green report / AIS, amber radar,
  red HIGH risk) with a 6-minute speed vector ahead of the bow; speed added
  to the contact tooltip.

### Added
- Traffic View: own ship drawn as a red map pin with a vessel glyph at the
  centre. The Navigation, Replay and Fleet maps keep the red heading
  triangle: Grafana 13.2 draws custom SVG map markers as black squares
  (seen on usv-722), and the smoke test now rejects them.
- Smoke test: builds the Frigate bridge and runs
  `mayara_logic_test` and `frigate_logic_test`; runs every ECharts
  instrument in a mocked Grafana context; type-checks all dashboard and
  alert Flux queries; checks that map markers use built-in shapes and the speed vector wiring; counts
  `frigate_bridge` as a producer (fixes a false "camera_summary.connected
  queried but never written").
- Navigation and Replay maps: own-ship 6-minute speed vector (lime line
  along COG, heading if COG is missing; hidden below 0.5 kn).

## [1.2.12] - 2026-10-06 — battery, readiness and Frigate cameras

### Added
- `frigate_bridge` (ingestion/frigate-cpp, service usv-frigate-bridge):
  camera status from the Frigate NVR API (`/api/config`, `/api/stats`).
  Per enabled camera: connected (RTSP port answers), stream_up, fps,
  recording; plus a camera_summary row. Readiness "Cameras" now reads
  "N / M cameras streaming"; cameras disabled in Frigate (configured but
  unpowered) are not counted. Enabled by `FRIGATE_URL` in usv.env.
- Cameras dashboard: the top row shows Frigate NVR up/down and counts
  (enabled, streaming, network connected); a table lists each camera with
  Network / Stream / Frame rate / Recording. The single-camera stat tiles
  drew every camera on top of each other once several reported.

### Fixed
- Battery and power panels read "No data" on usv-722: signalk_bridge only
  knew battery instance `0`, while the Victron VE.Direct shunt publishes
  `electrical.batteries.House.*`. Any instance is now accepted (the first
  seen, or `SIGNALK_BATTERY` in usv.env): voltage -> battery_voltage and,
  without an `electrical.system.voltage` sensor, power.system_voltage;
  current -> battery_current and, without `electrical.loads.total.power`,
  power.total_power = V x -I (power drawn from the battery);
  stateOfCharge -> soc; chargingMode -> charging_status.
- Readiness "Electrical" / "Engine pre-start" read "no data" when the
  battery and engine reach Signal K rather than MOOS: grafana_bridge now
  also reads battery voltage, SOC, coolant, oil pressure and fuel level
  written by signalk_bridge.

## [1.2.11] - 2026-10-05 — radar targets from mayara-server

usv-722 runs mayara-server 3.x for the Simrad Halo. mayara tracks ARPA /
MARPA targets itself, but nothing posted them to the MOOSDB, so the Traffic
dashboard showed AIS contacts only.

### Added
- `mayara_bridge` (ingestion/moos-cpp): polls the mayara Signal K Radar API
  (`/radars`, `/{id}/controls/power`, `/{id}/targets`) and posts each
  tracked target as `RADAR_TARGET`
  ("id=R3,range=..,bearing=..,spd=..,cog=..,cpa=..,tcpa=..", degrees true,
  knots), plus `RADAR_TTM_RATE` (radar liveness) and
  `MAYARA_BRIDGE_STATUS`. grafana_bridge already consumes these, so radar
  targets appear on the Traffic tab as `source=radar` contacts with
  CPA/TCPA, risk and the radar CPA alert. No grafana_bridge or mission
  change is needed.
- Display only: no NODE_REPORT is posted, so pContactMgrV20 and the helm
  never see radar targets and collision avoidance is unchanged.
- Posts only while the radar transmits; lost targets are skipped, targets
  still acquiring are skipped unless `MAYARA_INCLUDE_ACQUIRING=1`.
- Short contact ids: mayara numbers targets radar_index x 100000000 + n;
  the dashboard shows `R<n>` for automatic and `M<n>` for hand-acquired
  (MARPA) targets (`MAYARA_ID_PREFIX`, `MAYARA_MANUAL_PREFIX`).
- CPA/TCPA are posted only for a target moving at least 2 kn whose CPA is
  ahead and within 30 min (`MAYARA_CPA_MIN_KN`, `MAYARA_CPA_MAX_TCPA_S`).
  On usv-722 stationary echoes showed 0.5-1.2 kn of tracker jitter and
  CPAs down to 135 m, which would raise radar CPA alerts for moored boats.
- `usv-mayara-bridge.service` (plain MOOS client, reconnects to MOOSDB
  and mayara on its own); install.sh builds it with moos_bridge and
  enables it only when `MAYARA_URL` is set in `/etc/usv/usv.env`.
- `mayara_logic.hpp` with unit tests (`mayara_logic_test`, ctest).

### Fixed
- Own ship on the Traffic tab when MOOS has no fix. On usv-722
  iDivisorNMEA posts `NAV_LAT`/`NAV_LONG` = 0 (and `NAV_HEADING`/`NAV_SPEED`
  = 0) on every sentence because no RMC/HDT reaches it; pNodeReporter turns
  that into `NAV_X`/`NAV_Y` at the equator, so own ship sat 3,300 km south
  and every AIS contact read about 3,300 km. grafana_bridge now drops a 0,0
  fix, and while no real `NAV_LAT` has arrived for 10 s it also ignores
  `NAV_HEADING`, `NAV_SPEED`, `NAV_X` and `NAV_Y` and takes own position,
  heading and SOG from Signal K. One "traffic" warning event per run names
  the cause. The MOOS-side fix (iDivisorNMEA) is with the mission
  developer; until then the helm itself still has the wrong own position.

## [1.2.10] - 2026-10-05 — AIS contacts on the mission datum

On usv-722 every AIS contact on the Traffic dashboard read about
1,794 nm / 3,320,000 m away: the app posting the AIS NODE_REPORTs computes
X/Y from a different local-grid origin than the navigation, so range, CPA,
bearing and risk were all computed across two datums.

### Fixed
- grafana_bridge positions a NODE_REPORT contact from its LAT/LON (or
  LONG), converted with the mission file's LatOrigin/LongOrigin, whenever
  the report carries them; X/Y is used only when it does not.
- Own ship for the traffic picture comes from a fresh own fix (NAV_LAT /
  NAV_LONG, or Signal K nav.lat/lon via the poller, under 10 s old) on the
  same datum, falling back to NAV_X/NAV_Y. Own ship and contacts now always
  share one frame.

### Added
- One "traffic" warning event per run when a NODE_REPORT's X/Y and LAT/LON
  disagree by more than 1 km (names the contact and the offset).
- One "traffic" warning event per run when a reported contact is more than
  100 nm away (AIS cannot hear that far; a datum mismatch on a poster that
  sends X/Y only).
- `LatLonToLocal` / `ReportLatLon` in mission_logic.hpp with unit tests
  (round trip, the usv-722 symptom, parsing edge cases); smoke-test checks
  for an AIS report posted on a foreign datum.

## [1.2.9] - 2026-10-01 — documentation

### Added
- `docs/commissioning-usv-722.md`: commissioning record for the
  production vessel (hardware found, releases produced, checklist
  results, open items).
- `docs/runbook-usv-722.md`: operations runbook, including the exact
  grafana_bridge block added to `real.moos` and what each line does.

### Changed
- README, DEPLOYMENT-NOTE, deployment, traceability and schema docs
  updated to the production state: GPU measurement `gpu`, Starlink probe
  enabled, VPN tile semantics, 20 alert rules, INS fix_type / ins_mode.

## [1.2.8] - 2026-10-01 — N/A tiles, checklist placeholder

### Fixed
- Background status tiles read "N/A" in their base colour when empty;
  Grafana's own "No data" placeholder ignored the thresholds and painted
  Coolant, Radar, Camera and the camera status tiles as healthy.
- With `console_checklist = false` the Readiness checklist panel shows
  one "not published by this console" line instead of the placeholder.

## [1.2.7] - 2026-10-01 — console-fed rows optional

usv-722 has no ENC chart system and its console (Swiftships UI) does not
publish the pre-departure checklist; the two CONSOLE rows held the
verdict at NOT READY permanently.

### Added
- grafana_bridge ProcessConfig `enc_fitted = false` (Chart / ENC row N/A)
  and `console_checklist = false` (Console checklist row N/A, checklist
  table shows one N/A line). Default true: unchanged behaviour.
  mission_logic_test: 285 assertions.

## [1.2.6] - 2026-10-01 — dockside review of all 13 dashboards on usv-722

Walk-through of every dashboard with the boat alongside (engine, CAN,
AIS, radar and cameras off; INS position only).

### Fixed
- "No data" never looks like a verdict: stat tiles driven by value
  mappings get a neutral base colour, and numeric tiles with a red or
  OK base (Satellites, HDOP, Dist to WP, CPA, TCPA, Closest Range,
  Min CPA, Battery SOC, Fuel, MOOS Rate, Frame Rate) paint grey when
  empty while real values keep their colours (`neutral_base`).
- Analog meters (tachometer, coolant, oil, fuel, battery, SOC, rudder,
  throttle, heading error, speed) show "N/A" with no needle when there
  is no sample, instead of resting at the gauge minimum (0 rpm, 40 C,
  10.00 V, -45 deg off course).
- WP Bearing reads "no route" for the bridge's -1 sentinel.
- Satellites / HDOP tiles read N/A on an INS feed; the Alarms "GPS
  Sats" tile is now "GPS Fix" (fix type), which INS and GNSS both fill.
- Services (systemd) table: the Flux result is ungrouped so unit, state
  and detail arrive as columns (Grafana showed one raw labelled cell).
- Bandwidth chart shows physical interfaces only (docker/bridge/veth/
  tunnel interfaces excluded).
- COG is written only above 0.5 kn: at rest an INS derives it from
  near-zero velocities and the compass/trend swung through 360 deg.

### Changed
- Alert rules (20): "GPS Signal Loss" now means no position for two
  minutes from any source (no-data alerting on the vessel profile);
  "GPS Fix Degraded" (satellites < 4, warning) and "INS Not Tracking"
  (VectorNav mode other than 2, critical) added. "Engine Over
  Temperature" and "Low Battery Voltage" no longer alert on missing data
  in either profile: alongside with the engine off they fired for hours;
  the Readiness dashboard reports silent sensors instead.
- Simulator writes `nav.ins_mode = 2`.

## [1.2.5] - 2026-10-01 — GPU probe hang

Found on usv-722: `usv_gpu_probe` never returned. It ran `timeout` as the
telegraf user around `sudo intel_gpu_top`; the unprivileged timeout could
not stop the root process, which kept the pipe open.

### Fixed
- Telegraf runs the probe itself as root (`sudo -n /usr/local/bin/usv_gpu_probe`;
  sudoers rule for that root-owned script only, `!requiretty`), so
  `timeout -k 1 -s INT 2.6` and intel_gpu_top share one uid and the exit
  is unconditional. Verified against a SIGINT-ignoring stand-in: returns
  in 3.6 s with the last sample.

## [1.2.4] - 2026-10-01 — Intel Arc GPU on the System dashboard

usv-722 carries an Intel Arc Pro A60 (i915); the GPU tile only knew
nvidia_smi.

### Added
- `deploy/usv_gpu_probe` (installed to /usr/local/bin): Telegraf exec
  probe that samples `intel_gpu_top -J` for 2.5 s and writes measurement
  `gpu` (utilization_gpu = busiest engine, render_busy, video_busy,
  frequency_mhz, power_w, rc6_pct). Prints nothing without an Intel GPU.
- The installer writes `/etc/sudoers.d/usv-telegraf-gpu` (telegraf may
  run intel_gpu_top as root, that binary only) when intel_gpu_top is
  installed, validated with visudo; removed otherwise.

### Changed
- GPU tile and trend read measurement `gpu`; the nvidia_smi input (still
  commented out) carries `name_override = "gpu"` so NVIDIA hosts write
  the same series. Smoke test treats `gpu` as Telegraf-sourced.

## [1.2.3] - 2026-10-01 — System tiles on real hardware, Starlink probe

Found on usv-722 (OnLogic K800): the System dashboard's CPU Temp tile
drew every sensor at once, including acpitz at -273 C, and the MOOS
service tile showed DOWN and UP side by side after MOOSDB came up
(two net_response series in the window, result=connection_failed and
result=success).

### Fixed
- Telegraf stat tiles (`tele_last`) collapse to the single most recent
  sample across series: service tiles (InfluxDB, Signal K, MOOS), ping
  tiles, host gauges. CPU Temp takes the hottest CPU sensor; acpitz,
  nvme, pch and wifi sensors and readings outside -50..150 C are dropped
  from the tile and the trend.

### Changed
- `telegraf.conf` ships the Starlink dish probe (ping 192.168.100.1)
  enabled: the production vessel has one. Comment it out on hosts
  without a routable dish.

## [1.2.2] - 2026-10-01 — GPS readiness from Signal K / VectorNav INS

Found on usv-722 at commissioning: position enters through Signal K (the
`signalk-vectornav-vnins` plugin) and MOOSDB carries no NAV_LAT, so the
Readiness GPS row read "no position" while the Navigation map showed the
boat.

### Added
- `signalk_bridge` maps `navigation.vectornav.insMode` (0 not tracking,
  1 aligning, 2 tracking, 3 GNSS lost) to `nav.fix_type` ("no fix",
  "INS aligning", "INS tracking", "GNSS lost") and writes `nav.ins_mode`.
  The Navigation "GPS Fix" tile shows it.
- `grafana_bridge` polls `nav.lat`, `nav.lon`, `nav.fix_type`,
  `nav.satellites` and `nav.hdop` back from InfluxDB, so the GPS check
  accepts a position written by the Signal K bridge; a fresher own
  NAV_LAT/NAV_LONG sample still takes precedence.

### Changed
- GPS readiness check fails on fix_type "INS aligning" or "GNSS lost"
  (in addition to "none" / "no fix"). mission_logic_test: 279 assertions.

## [1.2.1] - 2026-10-01 — production profile

First release intended for a real vessel OBC. Everything that shipped in a
simulation configuration now defaults to the vessel profile.

### Changed
- Alert rules are generated with `--profile vessel`: Low Battery Voltage,
  GPS Loss, CAN Bus Failure and Engine Overtemperature now alert on no
  data (a silent sensor is a fault). Simulation hosts regenerate with
  `python3 tools/gen_alerts.py --profile sim` before installing.
- `example.moos` sets `readiness_profile = vessel`.
- `ECHARTS_PANEL_VERSION` is pinned (7.2.5) in `usv.env.example` for
  reproducible installs.

### Added
- Grafana SMTP from `SMTP_HOST`, `SMTP_USER`, `SMTP_PASSWORD`,
  `SMTP_FROM_ADDRESS`, `SMTP_FROM_NAME` (optional `SMTP_STARTTLS_POLICY`,
  `SMTP_SKIP_VERIFY`) in usv.env; the installer writes a marked `[smtp]`
  block into grafana.ini and removes it when `SMTP_HOST` is empty.
- The installer warns when contact-points.yml still carries the
  placeholder address `ops@example.com`.

### Fixed
- Installer opens `GRAFANA_PORT` in ufw when ufw is active. Found on usv01:
  Grafana answered on localhost, but ufw dropped port 3300 from the LAN, so
  operator browsers hung on a dark screen. Rules allow the LAN subnet of the
  default-route interface and 100.64.0.0/10 (Tailscale) only, tagged
  `usv-grafana`; `GRAFANA_ALLOW_FROM` in usv.env overrides the sources or
  disables the step (`none`).

## [1.2.0] - 2026-09-03 — readiness checks, SA-1 branding

Verified live on usv01: 16 automatic checks with evidence, the MOOS app
inventory (found pMarineViewer absent on the headless host, now an
optional app), the console checklist mirrored item by item, Readiness as
the first tab and home dashboard, "SA-1 Dashboard" in the navigation
menu, alert rules in the dashboards folder. Smoke test 89 PASS, 0 FAIL;
mission_logic_test 273 assertions.

### Added
- Readiness: three more onboard checks and two sharper ones. MOOS apps
  (every Antler `Run =` app, or `required_apps`, present in DB_CLIENTS),
  Endurance (fuel and battery hours from the vessel constants against
  the route time with a margin), route inside the operating region and
  route length on the course row, active J1939 DTCs fail the engine row;
  AIS and radar merged into one row (16 checks). GUI viewers listed in
  `optional_apps` are reported but never fail the apps row; stale rows
  from a previous bridge build are erased at start.
- Console checklist table shows every item with its description and
  TICKED / PENDING like the console: full-form `CHECKLIST_STATUS`
  (`items=Name:state:Description|...`) or `checklist_items` in the bridge
  ProcessConfig as the fallback for the short form.
- Alert rules are provisioned into the "Swift Autonomous Dashboard"
  folder (one folder in Grafana; delete the old empty "USV Alerts"
  folder once after upgrading).
- Readiness is the first tab and Grafana's home dashboard
  (`default_home_dashboard_path`, `GRAFANA_HOME_DASHBOARD` in usv.env).
- Branding: `BRAND_APP_TITLE` (default "SA-1 Dashboard") replaces the
  "Grafana" product name next to the logo in the navigation menu.

## [1.1.0] - 2026-09-03 — map overlays, Traffic, Readiness

Verified live on the simulation host usv01 (Grafana 13.2.1 on port 3300,
six Signal K instances): map overlays, route name and Remote state,
Traffic with scripted and fake contacts, Readiness with the console
variables poked by hand. Smoke test 82 PASS, 0 FAIL; mission_logic_test
238 assertions.

### Added
- Readiness dashboard (FR-12, second tab): pre-departure verdict with
  pass / fail / N/A / console counts, automatic checks table (GPS fix,
  course planning, chart/ENC, autonomy stack, steering / actuators,
  operator link and comms, CAN bus, engine, electrical, AIS, radar,
  cameras, onboard computer, safety flags, console checklist) with the
  evidence behind each verdict, and a mirror of the console's manual
  checklist. All checks are decided onboard in C++ (`usv::EvaluateReadiness`,
  mission_logic.hpp, unit-tested) and written by `grafana_bridge` as
  `readiness` / `readiness_summary`; a `LatestPoller` (cpp-common)
  fetches the few inputs other producers write. Console mirrors:
  `CHECKLIST_STATUS` and `ENC_STATUS` MOOS variables ->
  `mission_state.checklist_*` / `enc_status`. New ProcessConfig
  parameters `readiness_profile` (sim | vessel) and `readiness_period_s`.
  The console checklist table reads rows the bridge writes (`checklist`
  measurement); the course check falls back to the last known route,
  waypoints and region (`mission.region_vertices`) after a community
  restart.
- `tools/fake_traffic.py`: fake AIS / radar scene (harbour: 6 vessels +
  radar track; busy: 10) moving around the boat, published as
  NODE_REPORT / RADAR_TARGET for Traffic dashboard and avoidance tests.

### Added
- `tools/traffic_scenario.sh`: scripted crossing / head-on / overtaking /
  radar-only / opening contacts for an end-to-end Traffic test on the
  simulation host.

### Fixed
- Traffic tiles (closest range, min CPA, planned course, deviation) clear
  when there is no contact or no active route instead of holding the last
  value for the whole window.

### Added
- Traffic dashboard (replaces Radar): collision-risk and avoidance tiles,
  contact counts by source, heading-up polar traffic view with range
  rings, contacts and CPA line, highest-risk contact card, MOOS action
  block (planned vs commanded course, speed, avoidance behavior, reason),
  contact table, CPA/TCPA trend. `grafana_bridge` computes range,
  bearings, CPA/TCPA, encounter type and risk per contact onboard and
  raises a critical event and the `usv-collision-risk` alert on HIGH risk.
- Fixed tab order on every dashboard (Mission ... Fleet); the tag-based
  list sorted alphabetically and dropped the current page.

### Changed
- De-duplicated panels: Navigation KPI strip shows waypoint bearing and
  speed cap instead of repeating the compass and speed meter; Autopilot
  shows XTE and distance to station instead of repeating the rudder and
  throttle instruments; System shows load average instead of the network
  throughput already on Comms.

### Fixed
- Vessel markers on the Fleet, Navigation and Replay maps did not render:
  the heading rotation dimension lacked the `max` Grafana needs
  (`value % max`), giving a NaN rotation that the WebGL marker layer
  silently dropped. The marker is Grafana's built-in triangle (heading
  arrow); custom SVG symbols render as a black square on 13.2.1.

### Added
- Navigation and Replay maps draw the operating region, the planned
  route with numbered waypoints and the red track around the vessel
  hull; `grafana_bridge` publishes the helm's route / region vertices in
  geographic coordinates (`geometry` measurement, mission datum).
- Comms dashboard: Internet, latency, packet loss, VPN (tailscaled) and
  Starlink tiles read Telegraf host probes instead of simulator fields.

- Navigation: attitude (roll / pitch) trend alongside heading, speed and
  XTE. System: InfluxDB reachability tile next to Signal K and MOOS.
  Comms: bandwidth up / down from the host interfaces.
- Replay reads the 1-minute archive (`telemetry_1m`) automatically for
  windows older than the 30-day raw retention; the installer's downsample
  task now skips string fields (it errored on them before).

- Mission Events: behavior changes are logged only after the new set has
  held for 5 s (station-keep runs intermittently and flooded the log).
- Waypoint labels on the maps are 1-based (WP 1 is the first waypoint).
- Smoke test checks every generated query is complete (from + range).

### Changed
- Fleet dashboard uses its own `fleet_vessel` variable (default All) so
  the vessel selected on another tab no longer narrows the fleet view.
- System dashboard: CPU / GPU / RAM / Disk back as gauges.
- Mission state `ReturnToHome` renamed `ReturnToBase` (the mission's own
  term); state tiles and timelines show operator wording ("Return to
  Base", "Station Keeping", "Awaiting Re-arm", "Comms Lost", "Entering
  Region"). Data written before this change still maps to the new label.

## [1.0.0] - 2026-09-03 — simulation baseline

First baseline validated end to end on the simulation host (usv01:
MOOS-IvP simulation community, six Signal K instances, Grafana 13.2.1,
InfluxDB 2.x, Telegraf).

### Added
- Twelve single-screen operator dashboards (Mission, Navigation,
  Autopilot, Vessel Health, CAN Bus, Radar, Cameras, Comms, System, Replay,
  Alarms, Fleet) generated by `tools/gen_dashboards.py`; equal 3x3 status
  tiles, instruments, trends; nautical units only (knots, nautical miles,
  fixed metres); dark and light themes.
- Fleet support: `usv-signalk-bridge@<vessel>` template unit, `VESSELS`
  in `usv.env`, Fleet dashboard with last known positions and a status
  row per vessel; heading-rotated Swiftships hull markers on every map.
- Swiftships branding (`deploy/brand.sh`): logo, favicon, login page
  (artwork, heading, subtitle, card colours), browser title, dashboard
  header band; cache-proof against browsers holding Grafana's immutable
  assets.
- MOOS bridge (`grafana_bridge`) aligned with the simulation and vessel
  communities: planchaPID outputs (SPEED_CAP, CMD_SPEED, XTE_NOW,
  NEXT_TURN_ANGLE, ARRIVAL_STATE, CORRIDOR_STATUS, CRASH_STOP_STATUS),
  iWebSocket flags (REMOTE_CONTROL, CRASH_STOP, REGION_WARNING, ID_ROUTE,
  COMMS_LOST phases), J1939 ENGINE_* from iCANRec, radar/contact
  aggregates, route name resolved from Signal K, derived mission state
  including `Remote`, rate limiting and 1 Hz re-emit of change-only
  strings.
- Asynchronous InfluxDB writer shared by all C++ producers (worker
  thread, bounded queue, timeout) so a slow database never stalls MOOS.
- Alert rule profiles (`gen_alerts.py --profile sim|vessel`, 17 rules).
- GPU tile and trend on the System dashboard (Telegraf `nvidia_smi`).
- Smoke test (`tools/smoke_test.sh`, 56 assertions) covering builds,
  the 141-assertion `mission_logic_test`, simulator / CAN / Signal K /
  MOOS bridge runs against an InfluxDB stub, and dashboard-to-producer
  field coverage.

### Changed
- Installer hardened for unattended runs: non-interactive apt with
  retries, keyring conffile handling, unattended-upgrades lock, stale
  CMake cache reset, pAntler PATH drop-in (`MOOS_ANTLER_UNIT`).
- Grafana port is `GRAFANA_PORT` (3001 vessel default, 3300 on the
  simulation host).
- Documentation rewritten for the baseline (README, deployment guide,
  deployment note, user guide, schema, Alpha integration, traceability,
  validation report).

### Removed
- Docker-era InfluxDB init script (`influxdb/init`); the installer creates
  the buckets and the downsample task.

### Known gaps
- GPU input disabled until the host GPU vendor is confirmed; CPU
  temperature needs hwmon sensors on the host.
- bravo..foxtrot MOOS communities not yet run as services.
- Post-mission report generator and autonomy performance statistics
  planned for the next release.
