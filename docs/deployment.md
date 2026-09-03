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
(`BRAND_SUBTITLE`), and every dashboard carries a header band with the
wordmark, the dashboard name and the selected vessel (visible in `?kiosk`
mode too), and the geomap vessel symbol (`swiftships_boat.svg`, the
Freeboard-style hull rotated by heading) is served from the same place.
Grafana OSS has no white-label setting, so these are file
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
(red) and the vessel itself (Swiftships hull, rotated by heading). Route
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

## Units
Speeds are knots everywhere (the bridges convert MOOS m/s). Distances use
fixed units chosen per panel: metres for approach distance, cross-track
error, depth and rudder/turn geometry; nautical miles for distance
traveled and radar ranges. Grafana's auto-scaling length unit is not used,
so no value ever turns into km.

## GPU tile (System dashboard)
The GPU tile and trend read Telegraf's `nvidia_smi` input. It is shipped
commented out in `telegraf/telegraf.conf`; on a host with an NVIDIA GPU
and `nvidia-smi` on the PATH uncomment it and restart telegraf. Hosts
without a supported GPU show "No data" there.

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
| Alert thresholds | tools/gen_alerts.py --profile sim\|vessel -> regenerate -> re-run install.sh (sim profile skips rules for sensors the simulation lacks) |
| Branding text | BRAND_TITLE / BRAND_WELCOME / BRAND_SUBTITLE in usv.env |
| Fleet instances | VESSELS in usv.env |
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
| Stock Grafana logo / "Welcome to Grafana" | Browser cache of Grafana's immutable assets: Ctrl+F5 once; `sudo bash deploy/brand.sh` re-applies after an upgrade |
| Light theme chip "does nothing" | It opens a new tab (Grafana reads ?theme only on page load); press `c` then `t` to switch in place |
| grafana_bridge missing under pAntler (`Failed exec - not good`) | set MOOS_ANTLER_UNIT in usv.env and re-run install.sh (PATH drop-in) |
| Route tile shows a short id | Signal K unreachable from grafana_bridge: check `signalk_http` in the ProcessConfig |
| No MOOS data | grafana_bridge in pAntler list? `umm --spy` shows the vars? token/org in ProcessConfig correct? |
| No CAN data | `candump can0`; `journalctl -u usv-can-bridge -f` |
| Signal K panel red | Signal K on :3000? URL in /etc/usv/usv.env correct? |
| Alerts not firing | Grafana -> Alerting -> Rules; datasource health |
