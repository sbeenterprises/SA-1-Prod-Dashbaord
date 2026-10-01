# Deployment & Maintenance Guide (native, no Docker)

Everything runs directly on the onboard computer: InfluxDB, Grafana and
Telegraf as apt-installed systemd services, `grafana_bridge` as a MOOS
community app under pAntler, `can_bridge` and the Signal K bridge as
systemd services.

## Prerequisites
- Debian/Ubuntu onboard computer (amd64 or arm64) with internet access for
  the one-time install (apt repos of InfluxData and Grafana).
- MOOS-IvP built on the machine (for linking `moos_bridge`).
- Signal K server running (vessel standard: port 3000 — Grafana therefore
  runs on `GRAFANA_PORT`, 3001 by default; the simulation host uses 3300).
- Simulation host with several MOOS communities / Signal K instances: set
  `VESSELS="alpha:3000 bravo:3001 ..."` in usv.env; the installer runs one
  `usv-signalk-bridge@<vessel>` per entry (`/etc/usv/vessels/<vessel>.env`).
- SocketCAN interface up for NMEA 2000:
  `ip link set can0 up type can bitrate 250000` (persist via systemd-networkd
  or /etc/network/interfaces).

## Install
```bash
sudo bash deploy/install.sh     # first run creates /etc/usv/usv.env and exits
sudo nano /etc/usv/usv.env      # set ALL secrets
sudo bash deploy/install.sh     # installs and starts everything
# If MOOS isn't found automatically:
sudo MOOS_DIR=~/moos-ivp/build/MOOS/MOOSCore bash deploy/install.sh
```
(`bash deploy/install.sh` rather than `./install.sh`: a checkout made on
Windows may lose the executable bit.) The installer is idempotent, runs apt
non-interactively with retries, resets a stale CMake cache, and re-applies
branding; re-run it after changing configs, dashboards, alert rules or a
Grafana upgrade.

### Updating a running installation from git
After the v1.2 upgrade the alert rules live in the "Swift Autonomous
Dashboard" folder; the old, now empty "USV Alerts" folder can be deleted
once in Grafana (Dashboards > USV Alerts > Folder actions > Delete).

```bash
cd ~/Swift-Autonmous_Dashbaord && git pull
sudo bash deploy/install.sh                 # rebuilds bridges, re-provisions, re-brands
```
For dashboard-only changes the short path is copying the JSON into the
provisioning folder (Grafana reloads it within ~10 s):
`sudo cp grafana/dashboards/*.json /var/lib/grafana/dashboards/ && sudo chown grafana:grafana /var/lib/grafana/dashboards/*.json`.
Bridge changes need the rebuild (`install.sh`) and, for `grafana_bridge`, a
restart of the MOOS community (`sudo systemctl restart moos-antler`).

What it does: adds InfluxData + Grafana apt repos; installs influxdb2,
telegraf, grafana; runs `influx setup` and creates the `events` and
`telemetry_1m` buckets plus the 1-minute downsample task; builds
IXWebSocket once from source, then builds and installs the C++ programs
`can_bridge`, `signalk_bridge`, `usv_simulator` and `moos_bridge` to
/usr/local/bin; deploys systemd units under a `usv` service user;
installs the Business Charts (Apache ECharts) Grafana plugin for the
analog instrument panels; provisions the Grafana datasource, dashboards
and alert rules; enables everything at boot.

## Branding
`deploy/brand.sh` (run by the installer) puts the Swiftships mark on the
nav bar, favicon, touch icon and login page, replaces the login artwork
with a navy Swiftships background, sets the browser-tab title
(`BRAND_TITLE`), the login heading (`BRAND_WELCOME`) and its subtitle
(`BRAND_SUBTITLE`), the product name next to the logo in the navigation
menu (`BRAND_APP_TITLE`, default "SA-1 Dashboard"; Grafana's own name is a
constant in the OSS build, so `brand.sh` injects a small script into
`index.html` that rewrites that text node), and every dashboard carries a header band with the
wordmark, the dashboard name and the selected vessel (visible in `?kiosk`
mode too). Grafana OSS has no white-label setting, so these are file
overwrites under `/usr/share/grafana/public`; originals are kept as
`*.grafana-orig` and `sudo bash deploy/brand.sh --restore` puts them back.

Grafana serves its hashed build assets (logo, login artwork, JS chunks)
with a one-year immutable cache, so a browser that saw stock Grafana once
would keep showing it after the files are overwritten. The login look is
therefore also applied from `views/index.html`, which browsers never
cache: a `<style id="swiftships-brand">` block swaps the logo and artwork
for Swiftships-named copies (cache-busted by content hash) and sets the
heading and subtitle text. Any browser, cached or not, gets the branded
page; the nav-bar logo in an old tab follows after one Ctrl+F5.

Re-run the installer (or `sudo bash deploy/brand.sh`) after any
`apt upgrade grafana`; the script restarts `grafana-server`, which is
required because Grafana parses `index.html` once at startup and serves it
from memory.

## Themes
Dashboards are designed for both Grafana themes. Grafana applies the
`?theme=` URL parameter only on a full page load, and its click
interceptor turns same-window links into in-app navigation, so the ☀ Light
/ ☾ Dark chips open the dashboard in a new tab; that tab then stays in the
chosen theme. To switch in place press `c` then `t`; the lasting default is
Profile -> Preferences -> Interface theme (or Administration -> Default
preferences for every user).

## Map overlays
The Navigation and Replay maps draw the operating region (cyan outline),
the planned route with numbered waypoints (green), the vessel's track
(red) and the vessel itself (red heading arrow). Route
and region come from the helm (VIEW_SEGLIST, OPR_POLY_ACTUAL) converted
with the mission file's `LatOrigin` / `LongOrigin`; without that datum
the overlays stay empty and nothing else is affected.

## Communications probes
The Comms dashboard's Internet, latency and packet-loss values come from
Telegraf's ICMP probe to 8.8.8.8, the VPN tile from `tailscaled.service`
(Telegraf `systemd_units`), and the Starlink tile from a second ICMP
probe to the dish router (192.168.100.1) that is shipped commented out in
`telegraf/telegraf.conf`; enable it on vessels with a Starlink, the tile
reads N/A elsewhere.

## Pre-departure readiness (Readiness dashboard)
The installer makes it Grafana's home dashboard
(`default_home_dashboard_path` in grafana.ini; `GRAFANA_HOME_DASHBOARD`
in usv.env picks another file) and the generator puts it first in the
tab row. Every automatic check is decided onboard by `grafana_bridge`
(`usv::EvaluateReadiness`, unit-tested) and written as one row per check;
Grafana displays them. Two ProcessConfig parameters control it:
`readiness_profile = vessel | sim` (vessel: a check whose sensor data is
missing FAILS; sim: it reads N/A, because the simulation has no engine,
CAN, rudder-feedback or camera data) and `readiness_period_s` (default
5). Values other producers write (Signal K link, Internet probe, CAN
interface, cameras, host CPU/RAM/disk, and the Signal K position with its
fix quality) are polled from InfluxDB by the
bridge with the same credentials it writes with. The GPS row therefore
passes on a vessel whose GNSS/INS feeds Signal K only (no NAV_LAT in
MOOSDB); with a VectorNav INS through the `signalk-vectornav-vnins`
plugin the row shows the INS mode ("INS tracking" passes, "INS aligning"
and "GNSS lost" fail). Optional vessel
constants in the same block sharpen three checks: `fuel_tank_l`,
`cruise_fuel_lph`, `cruise_speed_kn`, `battery_kwh`, `cruise_power_kw`
and `endurance_margin` (default 1.5) turn the Endurance row from N/A into
"route 3.2 nm needs 0.8 h · fuel 13.7 h · battery 5.3 h"; `required_apps`
lists the MOOS apps that must be connected (default: every `Run =` line
of the mission file's Antler block, checked against DB_CLIENTS) and
`optional_apps` those that are only reported when absent (default
pMarineViewer, uMACView, uPlotViewer: GUI viewers on a headless host);
`checklist_items = Name:Description|Name2:Description2` names the
console's checklist items so the short-form CHECKLIST_STATUS still yields
a full table (every item not pending is shown TICKED). On a vessel with
no ENC chart system set `enc_fitted = false`, and where the console does
not publish CHECKLIST_STATUS set `console_checklist = false`: those rows
then read N/A and no longer hold the verdict at NOT READY (a CONSOLE row
blocks READY because it is undecided). The course row
also reports the route length and fails when any waypoint lies outside
the operating region; the engine row fails on any active J1939 DTC.

### Console checklist and ENC selection
The manual pre-departure checklist lives in the operator console
(Freeboard). For Grafana to mirror it, the console publishes two MOOS
variables (strings) whenever they change and at least every minute:
```
CHECKLIST_STATUS = done=9,total=11,pending=Weather Conditions|Port Clearance,by=douglas
ENC_STATUS       = selected:US5LA31M        // or: none
```
`pending` is the "|"-separated list of unticked items (empty when all are
ticked); `by` is the operator name. The preferred, full form also carries
every item with its state and description, so the dashboard shows the
whole list exactly as the console does (commas inside descriptions are
fine; avoid "|" and ":" in names):
```
CHECKLIST_STATUS = done=9,total=11,by=douglas,items=Chart Navigation System:ticked:Verify charts are loaded and current|GPS Position Fix:ticked:Confirm accurate GPS positioning|Weather Conditions:pending:Current and forecast weather acceptable|Port Clearance:pending:Harbor control and departure clearance
``` Until the console publishes them the
Readiness dashboard shows those two rows as CONSOLE and the verdict stays
NOT READY. Test by hand: `/usr/local/bin/moos_poke 'CHECKLIST_STATUS=done=11,total=11,pending=,by=ops' ENC_STATUS=selected:test` (installed with the bridge; `--port=` when the community is not on 9000).

Verify the bridge is evaluating (on the vessel, after a restart):
```
source /etc/usv/usv.env
curl -s -XPOST "$INFLUX_URL/api/v2/query?org=$INFLUX_ORG" -H "Authorization: Token $INFLUX_TOKEN" \
  -H 'Content-Type: application/vnd.flux' \
  -d 'from(bucket:"telemetry") |> range(start:-1m) |> filter(fn:(r) => r._measurement == "readiness_summary") |> last()'
```

## Traffic end-to-end test (simulation)
`tools/traffic_scenario.sh <mission.moos> [crossing|headon|overtaking|radar|opening|all|clean]`
spawns scripted contacts through pContactSpawn (and one radar-only target
through RADAR_TARGET) on the simulation host, holding each for 90 s.
Watch the Traffic tab: head-on must go HIGH with a critical "Collision
risk HIGH" event and the `usv-collision-risk` alert; the radar step must
show Radar Only = 1; the opening contact must stay CLEAR with TCPA
"OPENING". Contact headings are computed from the boat's current heading
(read from InfluxDB via `/etc/usv/usv.env`, hence sudo) or `OWN_HDG=<deg>`;
run it while the boat is under way on a steady leg.

For a full traffic picture, `tools/fake_traffic.py <mission.moos>
[--scene harbour|busy] [--duration 300]` feeds a scene of six to ten
vessels (ferry crossing, head-on workboat, slow tug, anchored barge, fast
small craft, distant cargo, a radar-only track) as NODE_REPORT / RADAR_TARGET
every 2 s, placed around the boat's current position and heading and moving
on their own courses. pContactMgrV20 sees them too, so this also exercises
the helm's avoidance behaviors. Stop with Ctrl-C; contacts expire within 10 s.

## Units
Speeds are knots everywhere (the bridges convert MOOS m/s). Distances use
fixed units chosen per panel: metres for approach distance, cross-track
error, depth and rudder/turn geometry; nautical miles for distance
traveled and radar ranges. Grafana's auto-scaling length unit is not used,
so no value ever turns into km.

## GPU tile (System dashboard)
The GPU tile and trend read the Telegraf measurement `gpu`
(`utilization_gpu`, plus `temperature_gpu` where the vendor reports it).
Intel Arc / Xe / iGPU: the installer puts `usv_gpu_probe` in
/usr/local/bin and, when `intel_gpu_top` is installed
(`apt install intel-gpu-tools`), a sudoers rule letting the telegraf user
run that root-owned probe script as root (intel_gpu_top needs perf
access); the probe reports the busiest engine as utilisation
plus render/video busy, frequency and power every 10 s. NVIDIA: uncomment
the `nvidia_smi` input in `telegraf/telegraf.conf` (it carries
`name_override = "gpu"`) and re-run the installer. Hosts without a
supported GPU show "No data" there. Check by hand:
`sudo -u telegraf sudo -n /usr/local/bin/usv_gpu_probe`.

## Analog instruments
The Navigation, Autopilot and Vessel Health dashboards carry an analog
instrument cluster (compass with HDG/COG needles, speed meter, rudder
angle indicator with port/starboard convention, throttle, heading-error
meter, tachometer, coolant, oil pressure, fuel, battery voltage and SOC
meters). These render through the signed `volkovlabs-echarts-panel`
plugin, installed by `install.sh` (pin a version via
`ECHARTS_PANEL_VERSION` in `/etc/usv/usv.env`). The instruments follow
Grafana's dark/light theme automatically and use the same semantic band
colors as the rest of the stack.

No internet at install time / plugin-free install: regenerate the
dashboards without instruments on your workstation
(`python3 tools/gen_dashboards.py --classic`, native gauges take their
place), copy the repo over and re-run `install.sh`.

## MOOS integration (the part you own)
Merge `ingestion/moos-cpp/example.moos` into your mission file:
```
ProcessConfig = ANTLER
{
  ...
  Run = grafana_bridge @ NewConsole = false
}

ProcessConfig = grafana_bridge
{
  AppTick   = 10
  CommsTick = 20
  influx_url    = localhost:8086   // no scheme: // starts a MOOS comment
  influx_tls    = false
  influx_token  = <token from /etc/usv/usv.env>
  influx_org    = usv
  influx_bucket = telemetry
  events_bucket = events
  vessel_id     = usv-01
  signalk_http  = localhost:3000   // route name lookup (ID_ROUTE uuid -> name)
  radar_cpa_alert_m = 500          // collision risk HIGH: CPA below this ...
  tcpa_alert_s  = 300              // ... within this many seconds
  readiness_profile = vessel       // sim on the simulation host
}
```
The console starts a mission with `ID_ROUTE=<Signal K route uuid>`; the
bridge looks the route's name up once per change
(`/signalk/v1/api/resources/routes/<id>`) and writes it as
`mission_state.route_name` for the Mission dashboard's Route tile, falling
back to a readable short id when Signal K is unreachable.
`grafana_bridge` starts, restarts and stops with the rest of the community
under pAntler. **If pAntler itself runs from a systemd unit with a private
PATH** (e.g. `moos-antler.service` with `Environment=PATH=<moos bins>`),
pAntler cannot exec `/usr/local/bin/grafana_bridge` and logs
`Failed exec - not good`; set `MOOS_ANTLER_UNIT=moos-antler.service` in
`/etc/usv/usv.env` and the installer adds a PATH drop-in for that unit.
The equivalent manual fix is a file
`/etc/systemd/system/moos-antler.service.d/10-usv-path.conf` with
`[Service]` / `Environment=PATH=<existing PATH>:/usr/local/bin:/usr/bin:/bin`,
then `systemctl daemon-reload && systemctl restart moos-antler`. The variable map is **pre-aligned with the Alpha mission**
(`alpha.bhv`): mode flags (WAYPOINT/RETURN/STATIONING/PAUSE), the comms
failsafe (COMMS_LOST + MISSION_ARMED re-arm latch), SURVEY_ENTRY_GATE,
DIST_TO_NEXT_WPT / WPT_INDEX / VIEW_SEGLIST, geofence (OPR_BREACHED),
obstacle alerts and helm health (IVPHELM_*) are all subscribed, and the
operational mission state is derived onboard (unit-tested in
`mission_logic_test`; see docs/alpha-mission-integration.md). Legacy
generic variables (WPT_DIST, MISSION_PROGRESS, ...) still work and take
precedence if something publishes them. To extend the map, edit
`ingestion/moos-cpp/moos_bridge.cpp`, then rebuild:
`cmake --build ingestion/moos-cpp/build -j && sudo cmake --install ingestion/moos-cpp/build`.

Poke variables during commissioning with the bundled utility, e.g.
`moos_poke WAYPOINT=true DIST_TO_NEXT_WPT=350` (values parse as numbers
when numeric, strings otherwise).

## Services
| Unit | Role |
|---|---|
| influxdb.service | time-series database (localhost:8086) |
| grafana-server.service | dashboards + alerting (GRAFANA_PORT in usv.env) |
| telegraf.service | OBC metrics, ping, service supervision |
| usv-signalk-bridge.service | Signal K deltas -> Influx (single vessel) |
| usv-signalk-bridge@<vessel>.service | one per `VESSELS` entry (simulation fleet) |
| usv-can-bridge.service | CAN/N2K frames -> Influx (CAP_NET_RAW) |
| usv-simulator.service | bench-test data (installed, NOT enabled) |
| grafana_bridge (pAntler) | MOOS -> Influx, managed by your mission |

`journalctl -u usv-can-bridge -f` etc. for logs.

## Bench testing without the vessel
```bash
sudo systemctl start usv-simulator     # full fake telemetry stream
sudo systemctl stop usv-simulator      # stop before real operations
```
The simulator loops a scripted Alpha mission (survey with tiered
deceleration, pause, station-keep, obstacle spawn, operator-link loss +
failsafe + explicit re-arm, geofence excursion + recovery, survey complete,
return-to-base). It intentionally fires the geofence / operator-link /
comms alerts once per cycle — that IS the FR-11 bench test. Expect alert
emails while it runs; always stop it before real operations.

## Configuration points
| What | Where |
|---|---|
| Secrets, vessel id, retention | /etc/usv/usv.env (re-run install.sh after edits) |
| MOOS variable map | ingestion/moos-cpp/moos_bridge.cpp |
| Signal K path map | ingestion/signalk-cpp/signalk_bridge.cpp kPaths |
| PGN decoders | ingestion/can-cpp/main.cpp decode_pgn() |
| Alert thresholds | tools/gen_alerts.py --profile sim\|vessel -> regenerate -> re-run install.sh (vessel profile: GPS Signal Loss and CAN Bus Failure also alert on missing data; engine and battery rules never do, the Readiness dashboard reports silent sensors) |
| Branding text | BRAND_TITLE / BRAND_WELCOME / BRAND_SUBTITLE / BRAND_APP_TITLE in usv.env |
| Home dashboard | GRAFANA_HOME_DASHBOARD in usv.env (default usv-readiness.json) |
| Readiness profile, vessel constants, app list, checklist items | grafana_bridge ProcessConfig in the mission file: readiness_profile, readiness_period_s, fuel_tank_l, cruise_fuel_lph, cruise_speed_kn, battery_kwh, cruise_power_kw, endurance_margin, required_apps, optional_apps, checklist_items, enc_fitted, console_checklist (see example.moos) |
| Console mirrors | CHECKLIST_STATUS and ENC_STATUS published by the operator console (spec above) |
| Tab order, tiles, checks table | tools/gen_dashboards.py (TAB_ORDER, usv-readiness section) -> regenerate -> re-run install.sh |
| Readiness rules | usv::EvaluateReadiness in ingestion/moos-cpp/mission_logic.hpp (unit-tested in mission_logic_test.cpp) |
| Fleet instances | VESSELS in usv.env |
| Firewall sources for Grafana | GRAFANA_ALLOW_FROM in usv.env (CIDR list; unset = LAN subnet + 100.64.0.0/10; `none` = leave ufw alone) |
| Alert recipients | grafana/provisioning/alerting/contact-points.yml |
| Dashboard refresh | per dashboard, default 5 s (1–5 s per SRS) |

## Roles (Operator / Engineer / Administrator)
Grafana OSS: Administrator -> Grafana Admin, Engineer -> Editor,
Operator -> Viewer (`?kiosk` URL for wall displays). Create users under
Administration -> Users.

## Maintenance
- **Backups**: /var/lib/influxdb (data) and /var/lib/grafana (dashboards
  DB). `influx backup /path/to/backup --token $INFLUX_TOKEN` for online
  backups.
- **Upgrades**: `apt update && apt upgrade influxdb2 grafana telegraf`.
  Test ashore first.
- **Disk**: raw telemetry at 1 Hz x ~80 fields is roughly 50–100 MB/day;
  30-day raw retention keeps this bounded. Check with
  `du -sh /var/lib/influxdb`.

## Troubleshooting
| Symptom | Check |
|---|---|
| Empty dashboards | `systemctl status usv-*` ; Influx Data Explorer at :8086 |
| Dashboard hangs on a dark screen from another PC, but `curl http://localhost:<GRAFANA_PORT>/api/health` returns 200 on the OBC | ufw is dropping the port: `sudo ufw status \| grep <GRAFANA_PORT>`. Re-run install.sh (opens it to the LAN and the tailnet, see GRAFANA_ALLOW_FROM in usv.env) or `sudo ufw allow from <lan-cidr> to any port <GRAFANA_PORT> proto tcp` |
| Stock Grafana logo / "Welcome to Grafana" | Browser cache of Grafana's immutable assets: Ctrl+F5 once; `sudo bash deploy/brand.sh` re-applies after an upgrade |
| Light theme chip "does nothing" | It opens a new tab (Grafana reads ?theme only on page load); press `c` then `t` to switch in place |
| grafana_bridge missing under pAntler (`Failed exec - not good`) | set MOOS_ANTLER_UNIT in usv.env and re-run install.sh (PATH drop-in) |
| Route tile shows a short id | Signal K unreachable from grafana_bridge: check `signalk_http` in the ProcessConfig |
| No MOOS data | grafana_bridge in pAntler list? `umm --spy` shows the vars? token/org in ProcessConfig correct? |
| No CAN data | `candump can0`; `journalctl -u usv-can-bridge -f` |
| Signal K panel red | Signal K on :3000? URL in /etc/usv/usv.env correct? |
| Alerts not firing | Grafana -> Alerting -> Rules; datasource health |
