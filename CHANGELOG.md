# Changelog

All notable changes to the Swift Autonomous Dashboard. The format follows
Keep a Changelog; versions are git tags on `main`.

## [Unreleased]

### Added
- Readiness: three more onboard checks and two sharper ones. MOOS apps
  (every Antler `Run =` app, or `required_apps`, present in DB_CLIENTS),
  Endurance (fuel and battery hours from the vessel constants against
  the route time with a margin), route inside the operating region and
  route length on the course row, active J1939 DTCs fail the engine row;
  AIS and radar merged into one row (16 checks).
- Console checklist table shows every item with its description and
  TICKED / PENDING like the console: full-form `CHECKLIST_STATUS`
  (`items=Name:state:Description|...`) or `checklist_items` in the bridge
  ProcessConfig as the fallback for the short form.
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
