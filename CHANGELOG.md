# Changelog

All notable changes to the Swift Autonomous Dashboard. The format follows
Keep a Changelog; versions are git tags on `main`.

## [Unreleased]

### Added
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
