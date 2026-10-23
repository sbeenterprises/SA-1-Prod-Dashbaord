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
- SocketCAN interface up at 250 kbit/s, named in `CAN_INTERFACE` (usv.env).
  Both hulls use a serial-line adapter brought up at boot with
  `slcand -o -c -F -s5 <serial device> slcan0` then `ip link set slcan0 up`; a
  native interface uses `ip link set can0 up type can bitrate 250000`
  (persist via systemd-networkd or /etc/network/interfaces).

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
bash deploy/update.sh vX.Y.Z      # from the repo checkout: fetch, checkout, install, status
```
`deploy/update.sh` with no argument takes the latest release tag, refuses to
run over local edits and prints the rollback command. It calls
`sudo bash deploy/install.sh`, which rebuilds the bridges and restarts their
services, re-provisions dashboards and alerts, reloads Telegraf and
re-applies branding.
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
mode too). The wordmark is the white knockout logo on the Dark and Navy
themes and the navy logo on a white chip on the Light theme. The login page's footer (Documentation, Support, Community,
Open Source and the platform version) is replaced by the export-control
notice (`BRAND_DISCLAIMER`, default "Disclaimer: Swiftships Autonomy
(SA-1) technical data, source code, and algorithms are non-releasable and
protected under U.S. Export Administration Regulations (EAR, ECCN 9D515),
DoDI 5200.39, and NDP-1."), and the installer sets `hide_version = true`
and `feedback_links_enabled = false` in grafana.ini (a marked
`usv-noversion` block), so the help menu names no version or feedback
links either. The same page script shows the operator popups: every
warning or critical event in the events bucket (readiness checks turning
FAIL, geofence, all-stop, comms loss, collision) appears as a card in the
top-right corner of any page within 5 s, read through the dashboards'
own data source; `?alerts=0` turns them off for one screen. Grafana OSS
has no white-label setting,
so these are file overwrites under `/usr/share/grafana/public`; originals
are kept as `*.grafana-orig` and `sudo bash deploy/brand.sh --restore`
puts them back.

The same `index.html` script fits every dashboard to the screen: the pages
are laid out to 19 grid rows (`SCREEN_ROWS` in tools/gen_dashboards.py; a
shorter page is padded to it), and the script zooms the whole page so the
grid's bottom edge meets the bottom of the window, on any display from a
laptop to a wall screen, re-evaluated on resize. `?fit=0` on the dashboard
URL turns it off for that tab (for example to inspect a panel at 100 %).

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
Every dashboard has a **Theme** picker at the top, next to Vessel:
**Dark** (Grafana's dark theme, the default), **Navy** (the dark theme in
the Swiftships navy: page #13203B, panels #1B2B4B, borders #2B3F66) and
**Light** (Grafana's light theme). The choice is remembered in that browser
and carried from page to page. Grafana chooses Light or Dark when it serves
the page, so changing the picker reloads the page once. Navy is applied by
a script brand.sh adds to Grafana's page (`swiftships-theme-js`), which
recolours the dark theme as Grafana draws it; re-run `deploy/brand.sh`
(install.sh and update.sh do) after a Grafana upgrade. Grafana reads
`views/index.html` as a Go template whose delimiters are two square
brackets: anything brand.sh injects must never contain two in a row, or
every page comes back empty (tools/smoke_test.sh checks this).

The ☀ Light / ☾ Dark chips in the tab row still open the page in a new tab
in that theme. To switch in place without the picker press `c` then `t`;
the lasting server default is Profile -> Preferences -> Interface theme (or
Administration -> Default preferences for every user).

## Vessel particulars (class name, card, fuel in litres)
The `VESSEL_*` keys in `/etc/usv/usv.env` (deploy/usv.env.example has both
hulls' values from their GA drawings) describe the hull: class name, main
dimensions, payload, fuel capacity, range, maximum speed and fitted
equipment. signalk_bridge writes them every 60 s as the `vessel`
measurement (`vessel_class` tag, one string field per card row, numeric
`fuel_l`, `max_speed_kn`, `range_nm`, `loa_m`). They appear as:

- the class name in the header band of every page ("READINESS · S3 Swift
  Sea-Stalker · Vessel usv-725 · Engine Mercury Diesel 2.0L");
- the **Vessel particulars** card on the Readiness page;
- `engine.fuel_remaining_l` (tank % x `VESSEL_FUEL_L`, written by the bridge
  that writes the fuel level) and, on Vessel Health, the **Fuel Remaining**
  (L), **Range Left** (nm at the current fuel rate and SOG; N/A below 0.5 kn
  or with no fuel rate) and **Time Left** (h at the current fuel rate)
  tiles and the Fuel Remaining / Consumption trend;
- the Readiness **Endurance** row, which uses `VESSEL_FUEL_L` when the
  mission file has no `fuel_tank_l` and shows the litres ("fuel 94 % =
  1,067 L of 1,135 L"), and the **Speed cap** row (SPEED_CAP against
  `VESSEL_MAX_SPEED_KN`).

A hull without the keys shows "no particulars" on the card and N/A on the
litre tiles and the Speed cap row; nothing else changes.

## Map overlays
The Navigation and Replay maps draw the operating region (cyan outline),
the planned route with numbered waypoints (green), the vessel's track
(red) and the vessel itself (red heading arrow). Route
and region come from the helm (VIEW_SEGLIST; REGION_POLY on the route-corridor missions, OPR_POLY_ACTUAL, or a REGION_UPDATES carrying a pts block) converted
with the mission file's `LatOrigin` / `LongOrigin`; without that datum
the overlays stay empty and nothing else is affected.

## Communications probes
The Comms dashboard's Internet, latency and packet-loss values come from
Telegraf's ICMP probe to 8.8.8.8, the VPN tile from Telegraf
`systemd_units` (UP when any `twingate*`, `tailscaled`, `openvpn*` or
`wg-quick@*` unit is active; usv-722 runs `twingate-connector.service`),
and the Starlink tile from a second ICMP probe to the dish at
192.168.100.1, shipped enabled in `telegraf/telegraf.conf` (the dish must
be routable from the OBC, otherwise comment the probe out and the tile
reads N/A). A VPN that terminates on the boat router has no local unit
and reads N/A. The bandwidth chart plots the three busiest physical links
(CAN, VPN, docker and bridge interfaces are excluded).

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
"route 3.2 nm needs 0.8 h · fuel 82 % = 164 L of 200 L = 13.7 h · battery
5.3 h" (`fuel_tank_l` falls back to `VESSEL_FUEL_L` in usv.env); the Speed
cap row compares the helm's SPEED_CAP with `VESSEL_MAX_SPEED_KN` in
usv.env (FAIL above the hull maximum or at zero, N/A when either is
missing); `required_apps`
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

A route planned with the Swiftships-UI safe-route tool is checked against
the ENC (depth, land, hazards). Current Swiftships-UI builds update the
selected route in place and record the check in the route's description:
"Planned in water >= 3 m with 10 m clearance (US3LA1CB, US5LMOBB)."
grafana_bridge reads that note with the route name (the same Signal K
lookup, once per ID_ROUTE and again when the same route is started after
30 s) and writes it as `mission_state.route_plan`; the course row then
reads "checked safe on ENC (water >= 3 m, 10 m clearance, US3LA1CB,
US5LMOBB)". Earlier builds saved a copy with "(safe)" in its name, for
example `10-19-2026 (safe)`; that name still counts. Such a route counts
as the operating region:
the course row passes with "checked safe on ENC" when the route has
waypoints, even without an operating region polygon. When both exist the
region rules apply as well (route inside the region). A route whose name
carries "(unsafe)" fails the course row. A safe route also shows that an
ENC chart is in use, so the Chart / ENC row passes with "ENC in use ·
route checked safe on chart" while the console does not publish
ENC_STATUS, even with `enc_fitted = false`; a published ENC_STATUS always
decides that row.

The Chart / ENC row also reads the chart server itself. The ENC cells the
console draws are served by Signal K (the ENC upload converts each S-57
cell to tiles and lists it under `/signalk/v1/api/resources/charts` with
its bounds). grafana_bridge fetches that listing every `enc_poll_s`
seconds (default 60; 0 turns it off) from `signalk_http`, the server it
already uses for route names, counts the cells of type S-57 (base maps
and raster charts are not ENCs) and picks the most detailed cell whose
bounds contain the boat's position. The row then reads, in order of
precedence:

| Row | Source |
|---|---|
| `selected:<chart>` PASS / `none` FAIL | the console's ENC_STATUS, whenever it is published |
| `ENC US3LA1CB · covers position · 10 cells loaded` PASS | chart server listing, a cell covers the position |
| `ENC 10 cells loaded · NO CELL COVERS POSITION` FAIL | cells loaded, none contains the position (wrong area loaded) |
| `ENC 10 cells loaded · position unknown` PASS | cells loaded, no position fix to check against |
| `ENC in use · route checked safe on chart` PASS | no listing (chart server unreachable or without ENC cells), route named "(safe)" |
| `no ENC chart system on this vessel` N/A | none of the above and `enc_fitted = false` |
| `console not publishing ENC_STATUS` CONSOLE | none of the above |

A listing older than three poll periods is dropped, so a chart server
that stops answering takes the row back to the lower sources rather than
freezing it. Which chart layer the operator has selected in the console
is client-side state; it reaches the dashboard only through ENC_STATUS.
The bridge writes what it found to the `chart` measurement (`enc_cells`,
`enc_cell`, `enc_covers`: 1 covered, 0 not covered, -1 no position) and
logs "chart server lists N ENC cells" when the count changes. Check the
listing by hand on the vessel: `curl -s http://localhost:3000/signalk/v1/api/resources/charts | head -c 600`.

### Navigation integrity (row 2)
The boats' navigation guard (pNavGuard in the mission) compares heading,
speed and course with what the GPS positions show and watches for frozen,
stale or jumping data. It posts `NAV_STATUS` (ok / warn / fault) once a
second with the reason in `NAV_STATUS_MSG`; in a fault while autonomy
drives it asks iWebSocket to stop (`NAV_GUARD_STOP`) and the boat refuses
mission, return, station-keep and follow starts until it is clean again.
grafana_bridge writes the level as `autonomy.nav_guard` (0 ok, 1 warning,
2 fault) while the guard reports, and the row reads:

| Row | When |
|---|---|
| heading, speed and track consistent with GPS, PASS | ok |
| warning: <reason>, PASS | warn (a check is out of tolerance, not yet for its fault time) |
| FAULT: <reason> · autonomy starts refused, FAIL | fault |
| navigation guard not reporting · autonomy starts refused, FAIL | pNavGuard is in the mission's app list but silent for 10 s |
| no navigation guard on this vessel, N/A | the mission does not run pNavGuard |

Events (and popups): "Navigation warning: ..." (warning), "Navigation
FAULT: ... (autonomy starts refused)" (critical), "Autonomy stopped by the
navigation guard: ..." (critical, once per fault although the stop is
repeated every 2 s), "Navigation consistent again", "Navigation guard not
reporting" and "... reporting again". The row raises no "Check FAIL" popup
of its own, so a fault pops up once. The guard's thresholds are
provisional until water-tested: for the first runs on a boat set
`stop_on_fault = false` in its pNavGuard block (warnings only) and compare
them with what the boat did.

### Console checklist and ENC selection
The manual pre-departure checklist lives in the operator console
(Swiftships-UI). For Grafana to mirror it, the console publishes two MOOS
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

## Boat control, ALL STOP and the Audit page
With login and exclusive control on the boat (iWebSocket `command_auth =
signalk`, `exclusive_control = true`; USV-722 since the 22 Oct mission
build), one Swiftships-UI tab at a time holds control; viewers watch,
operators take, release and request control, a supervisor may force a
takeover with a reason, and any operator may press ALL STOP. iWebSocket
writes every login, logout, control change, ALL STOP and refused command
to a daily file on the boat (`audit_dir`, `c2_audit_YYYY-MM-DD.log`) and
posts the same lines to MOOS as `CONTROL_EVENT` / `UI_AUTH_EVENT`.

grafana_bridge reads them and:

- writes one row per line to the events bucket, measurement `audit` (tags
  category control / login / refused / all_stop / nav_guard / region,
  action, severity, user, role; fields message, details). The events
  bucket keeps a year: the installer creates it with 365 days and raises
  an existing bucket with a shorter retention to 365 days;
- joins a forced takeover's two lines (the grant, then the reason) into
  one event;
- tracks who holds control and the open logins
  (`mission_state.control_holder`, `control_role`, `control_since_ms`,
  `c2_sessions`, `c2_session_count`), re-written every 5 s;
- raises popups for a forced takeover, control lost (holder not heard),
  control revoked, a refused command (at most one a minute per user and
  command) and a login with a logged-out token; control changes also go
  to Mission Events as info.

`ALL_STOP=true` is the latch itself: Mission State reads ALL STOP (above
every other state), Safety flags fails with "ALL STOP LATCHED", a critical
popup says so, and `ALL_STOP=false` (reset) brings an info event. The
audit line names who pressed and who reset it.

Also read from the boat: `REGION_MSG` (a refused region, warning; other
notes, info), `V26_STATE` (the region behaviour's state as
`mission_state.region_state`; "breached" fails Safety flags and raises a
critical event, "lapsed" a warning), the drive interface (iCAN:
`CAN_STATUS`, `CAN_CONTROL_OWNER`, `CAN_TX_GEAR`, `CAN_TX_THRUST_PCT`,
`CAN_TX_RUDDER_PCT`, `CAN_TRIM_STATE`, `CAN_PROFILE`, `CAN_CONFLICT`,
`CAN_SEND`, `CAN_LOST_LINK`, `CAN_RUDDER_MISMATCH` into measurement
`can_drive`; a conflict or a missing bus or profile fails the CAN row and
raises a critical event, a rudder mismatch fails the Steering row), and
the autopilot's `SERVICE_MODE` and `PID_GAINS` (autopilot.service_mode,
gain_heading_kp ... gain_speed_kd; service mode shows in the Autonomy
stack row and raises a warning). While the operator drives (drive owner
MANUAL) the thrust and rudder the drive interface sends also count as
console commands for the Remote state.

The **Audit** page (tab row, after Alarms; it opens at the last 24 h and
does not hand that range to the live pages) shows six tiles, each with a
second line: who holds control (role, for how long; LOST (user) while the
holder is not heard), ALL STOP (last pressed or reset, by whom, how long
ago), the navigation guard (stops in the range), logins (number of users),
refused commands (read-only / ALL STOP / not in control / other) and
forced takeovers (who, how long ago). Below: a timeline of who had control
("lost" in amber) with ALL STOP and the navigation guard; the audit log
(newest first; Category and Search at the top of the page, a filter in
each column header, CSV download from its link or the panel menu: Inspect
> Data); refused commands by reason; events per user (Control, Logins,
Refused); open sessions (user, role, IP; measurement `c2_session`, one row
per login, re-written every 5 s). It lives in its own folder,
**Swift Autonomous Audit**, which the installer opens to Grafana Editors
and Admins only (with the Signal K login below: operators and
supervisors). Grafana cannot take away access a folder grants, hence the
separate folder.

## Signal K login (SK_LOGIN)
`SK_LOGIN=1` in /etc/usv/usv.env gives Swiftships-UI and the dashboard
one login: an operator logged in to Swiftships-UI (or the Signal K admin
page) on the boat opens the dashboard without logging in again, with a
Grafana role from their boat role.

How it works: nginx answers on GRAFANA_PORT and asks usv-auth
(127.0.0.1:USV_AUTH_PORT, default 3102) about every request. usv-auth
takes the browser's Signal K login (the JAUTHENTICATION cookie Signal K
sets, or the sktoken cookie Swiftships-UI keeps; a browser sends cookies
to every port of the same host), asks the Signal K server who it is
(`/skServer/loginStatus`, as iWebSocket does) and returns the user and a
Grafana role. Grafana moves to 127.0.0.1:GRAFANA_INTERNAL_PORT (default
3099) and trusts the user and role headers from nginx only (`[auth.proxy]`,
whitelist 127.0.0.1); headers a browser sends itself are dropped.

| Boat role (Swiftships-UI roles file, else Signal K level) | Grafana role | Audit page |
|---|---|---|
| viewer (read-only account: always) | Viewer | no |
| planner | Viewer | no |
| operator (readwrite) | Editor | yes |
| supervisor (admin) | Editor | yes |

The roles file is the one the Swiftships-UI plugin writes for iWebSocket
(`/var/lib/swiftships-c2/roles.json`, `SK_ROLES_FILE`); `SK_ROLE_MAP`
changes the mapping (for example `supervisor=Admin`). A Signal K user
with the same name as Grafana's administrator (`GRAFANA_ADMIN_USER`,
default admin) comes in as `sk-admin`, so a Signal K login never becomes
Grafana's administrator. Results are cached
10 s per login. usv-auth also sets a user's Grafana role through the
Grafana API when it changes (using GRAFANA_ADMIN_PASSWORD), in case the
Grafana build ignores the role header.

Not logged in to Signal K, Signal K unreachable, or its security off:
Grafana shows its own login form, so the admin account always works.
Turn SK_LOGIN on only once Signal K security is set up on the boat
(users with read-only / read-write / admin levels). Enable:

```bash
sudo sed -i 's/^SK_LOGIN=.*/SK_LOGIN=1/' /etc/usv/usv.env   # or add the line
cd ~/Documents/SA-1-Prod-Dashbaord && bash deploy/update.sh <tag>
systemctl is-active nginx usv-auth grafana-server            # three "active"
curl -s -o /dev/null -w '%{http_code}\n' http://localhost:${GRAFANA_PORT:-3001}/login   # 200
```

Then log in to Swiftships-UI in the browser and open the dashboard: the
user menu shows the Signal K user. `SK_LOGIN=0` and the same update turn
it off again (nginx gives the port back to Grafana).

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
(`alpha.bhv`): mode flags (WAYPOINT/RETURN/STATIONING/PAUSE/TRAILING/REGION), the comms
failsafe (COMMS_LOST + MISSION_ARMED re-arm latch), SURVEY_ENTRY_GATE,
DIST_TO_NEXT_WPT / WPT_INDEX / VIEW_SEGLIST, geofence (OPR_BREACHED),
obstacle alerts and helm health (IVPHELM_*) are all subscribed, and the
operational mission state is derived onboard (unit-tested in
`mission_logic_test`; see docs/alpha-mission-integration.md). When no
autonomous mode is driving the boat (none started, or the helm parked) the
state reads Remote while the operator commands throttle or rudder from
Swiftships-UI (|DESIRED_THRUST| at or above 1 % or |DESIRED_RUDDER| at or
above 1), Moving while the boat drifts at 0.5 kn or more with no command,
and Idle / Parked at rest; each is held 10 s after its last sign. Tune with
`remote_thrust_pct`, `remote_rudder`, `moving_speed_kn` (0 = no Moving
state) and `remote_hold_s` in the grafana_bridge block. Legacy
generic variables (WPT_DIST, MISSION_PROGRESS, ...) still work and take
precedence if something publishes them. To extend the map, edit
`ingestion/moos-cpp/moos_bridge.cpp`, then rebuild:
`cmake --build ingestion/moos-cpp/build -j && sudo cmake --install ingestion/moos-cpp/build`.

Poke variables during commissioning with the bundled utility, e.g.
`moos_poke WAYPOINT=true DIST_TO_NEXT_WPT=350` (values parse as numbers
when numeric, strings otherwise).

## Radar targets from mayara-server (optional)
On a vessel whose radar runs through mayara-server (usv-722: Simrad Halo,
mayara on port 3001), `mayara_bridge` puts mayara's tracked ARPA targets
on the Traffic dashboard. Set in `/etc/usv/usv.env` and re-run the
installer:
```
MAYARA_URL=http://localhost:3001
#MAYARA_RADAR=nav6959      # default: first radar found
```
The bridge polls `/signalk/v2/api/vessels/self/radars/<id>/targets` once a
second and posts each tracked target to the MOOSDB (MOOS_HOST/MOOS_PORT,
default localhost:9000) as
`RADAR_TARGET = id=R3,range=1067.0,bearing=133.8,spd=5.70,cog=232.6,cpa=350.1,tcpa=138`
(metres, degrees true, knots, seconds; `R<n>` = acquired automatically,
`M<n>` = acquired by hand), plus `RADAR_TTM_RATE` and
`MAYARA_BRIDGE_STATUS` (`radar=..,power=..,targets=..,mayara=ok`).
grafana_bridge already subscribes to these; nothing is added to the
mission file. It is **display only**: no NODE_REPORT is posted, so the
helm and pContactMgrV20 do not react to radar targets.

CPA/TCPA are sent only for targets moving at 2 kn or more with the
closest approach ahead and within 30 minutes (`MAYARA_CPA_MIN_KN`,
`MAYARA_CPA_MAX_TCPA_S`): a stationary echo carries about 1 kn of tracker
jitter, and its CPA would otherwise raise false radar CPA alerts.

**Radar server version.** The bridge needs mayara-server from
`github.com/MarineYachtRadar/mayara-server` (Signal K v2 radar API; both
hulls run commit `d4deea0`, which reports API version 3.4.0), built with
Rust 1.90 or newer (`rustup update stable`, then `cargo build --release`).
The older `keesverruijt/mayara` builds serve only `/v1/api/radars` and
have no targets address: the bridge then logs "mayara unreachable" every
few seconds and never finds the radar. Start it on the radar's network
port with Signal K's TCP stream as the navigation source (target course and
speed need own position and heading), in `/etc/systemd/system/mayara.service`:
```
ExecStart=/home/douglas/Documents/radar/mayara-server/target/release/mayara-server -p 3001 -i <radar port> -n tcp:127.0.0.1:8375
```
**Radar network.** Navico radars (HALO) sit on 169.254.0.0/16 and announce
themselves by multicast to 236.6.7.x. The computer port the radar is
cabled to needs a 169.254.x.x address and the multicast route
`224.0.0.0/4` on that same port, and no other port may hold a 169.254
route: a dead port with the same network wins the route and the radar
becomes unreachable. Check the cabling first with
`sudo timeout 10 tcpdump -i <port> -n 'udp and dst net 236.6.7.0/24'`
(packets from the radar's 169.254 address = it is on that port).

| Hull | Radar | mayara id | Port | Computer address | Set by |
|---|---|---|---|---|---|
| usv-722 | HALO20 | nav6959 | enp11s0 | 169.254.10.10/16 | `ExecStartPre` lines in mayara.service (addr flush/add, link up, `ip route add 224.0.0.0/4 dev enp11s0`) |
| usv-725 | HALO24 | nav3080A (B = second range) | enp14s0 | 169.254.10.11/16 | NetworkManager profile "Wired connection 4" (manual address); profile `enp11s0` autoconnect off; mayara.service only `ExecStartPre=/sbin/ip route replace 224.0.0.0/4 dev enp14s0` |

The radar server's own web page is `http://<obc>:3001/` (radar picture,
standby / transmit, target acquisition).

Targets exist only while the radar **transmits** and mayara has acquired
them (guard zone, click-to-acquire, or Doppler auto track). If
`iRadarTargets` also publishes RADAR_TARGET on the same vessel, run only
one of the two to avoid duplicate contacts. Check with
`journalctl -u usv-mayara-bridge -f` and
`curl -s localhost:3001/signalk/v2/api/vessels/self/radars/<id>/targets`.

## Camera status from Frigate (optional)
On a vessel whose cameras are served by the Frigate NVR (usv-722: Frigate
0.16 in Docker, API on port 5000), `frigate_bridge` feeds the Cameras
dashboard and the Readiness "Cameras" check. Set in `/etc/usv/usv.env` and
re-run the installer:
```
FRIGATE_URL=http://127.0.0.1:5000
#FRIGATE_POLL_S=5
```
Every 5 s it reads `/api/config` and `/api/stats` and writes, per camera
enabled in Frigate, `camera,cam=<name>` connected (the camera answers on
its RTSP port, taken from Frigate's go2rtc / ffmpeg URLs), stream_up
(Frigate receives >= 1 fps), fps and recording, plus a `camera_summary`
row (frigate_up, enabled, streaming, connected). Readiness reads
"N / M cameras streaming". A camera that is configured but disabled in
Frigate (for example not yet powered) is not reported and is not a fault:
disable or remove it in Frigate (Configuration Editor, Save & Restart)
until it is cabled. Camera passwords in Frigate's URLs are never logged or
stored. Check with `journalctl -u usv-frigate-bridge -n 5`. Frigate's API
port 5000 must be published to the host (`docker ps` shows
`0.0.0.0:5000->5000/tcp`); port 8971 needs a login and cannot be used.
usv-725 (2026-10-18): `FRIGATE_URL=http://127.0.0.1:5000`, cameras
`bullet_camera` (streaming) and `flir_camera` (disabled in Frigate until
powered).

Camera clocks: a camera without a time source starts from 1970 after every
power cycle and stamps that date on its video. Make the onboard computer
its NTP server (`apt install chrony`, `allow <camera subnet>` in
`/etc/chrony/conf.d/cameras.conf`, restart chrony) and set the camera's
Time Settings to NTP with the computer's address on the camera network.

## Speed units of the helm (MOOS)
On the boats the helm's speeds are in knots: iDivisorNMEA posts NAV_SPEED
from the GPS in knots and the behaviours are set to match (the mission
repository's iWebSocket says so with `nav_speed_units = knots`); the
simulator's uSimMarineV22 uses m/s. grafana_bridge converts NAV_SPEED,
DESIRED_SPEED, SPEED_CAP, CMD_SPEED and PID_SPEED_ERROR by
`NAV_SPEED_UNITS` in `/etc/usv/usv.env` (`knots` on a vessel; unset = mps,
as before v1.2.23), or `nav_speed_units` in its mission block, which wins.
The pAntler console prints the value in use at start. With the wrong
setting the Autopilot speeds and the speed error read 1.94 times too high
(or too low).

## Battery instance (Signal K)
`signalk_bridge` charts one battery: the instance named in
`SIGNALK_BATTERY` in `/etc/usv/usv.env`, or the first
`electrical.batteries.<id>` Signal K reports (usv-722: `House`, Victron
VE.Direct). Its voltage also feeds System Voltage when no
`electrical.system.voltage` sensor exists, and voltage x -current feeds
Total Power when no `electrical.loads.total.power` exists.

## Fuel tank instance (Signal K)
With `ENGINE_SOURCE=signalk`, `signalk_bridge` takes the fuel level from one
tank: `tanks.fuel.<id>.currentLevel` for the `<id>` named in
`SIGNALK_FUEL_TANK` in `/etc/usv/usv.env`, or the first fuel tank Signal K
reports (an N2K 127505 fluid level sender maps to its instance number, 0
for a single tank). The engine's own `propulsion.<id>.fuel.level`, when a
source publishes it, is written as the same field. Engine ids: the bridge
reads every `propulsion.<id>` the server publishes and normalises `0` /
`port` to instance 0 and `1` / `starboard` to instance 1; any other id is
logged once and ignored (`journalctl -u usv-signalk-bridge`).

## Engine profile (one release, two hulls)
The engine data path differs per hull and is chosen in `/etc/usv/usv.env`,
never in the dashboards:

| Hull | Engine | Engine data path | usv.env |
|---|---|---|---|
| usv-722 | Yamaha Outboard Engine F115 B 4-Stroke | NMEA 2000 (Yamaha Command Link gateway, on order) -> Signal K `propulsion.*` -> signalk_bridge | `ENGINE_TYPE=yamaha_outboard` `ENGINE_MODEL="Yamaha Outboard Engine F115 B 4-Stroke"` `ENGINE_SOURCE=signalk` `HELM_PROFILE=usv722` |
| usv-725 | Mercury Diesel 2.0L sterndrive | SAE J1939 engine ECU (address 0) and N2K tank sender on slcan0 -> can_bridge | `ENGINE_TYPE=mercury_diesel` `ENGINE_MODEL="Mercury Diesel 2.0L"` `ENGINE_SOURCE=can` `HELM_PROFILE=usv725` |

`ENGINE_SOURCE` names the one producer of the `engine` fields; the other
bridges leave them alone (docs/schema.md, "engine"). `ENGINE_TYPE` becomes
the `engine_type` tag on every engine point and selects the dial scales;
`ENGINE_MODEL` (quoted, it contains spaces) becomes the `engine_model` tag,
the name in the header chip and on the Fleet page (default "Yamaha
outboard" / "Mercury diesel"). install.sh refuses a value with spaces that
is not quoted, because the file is read by bash. `ENGINE_COUNT` /
`ENGINE_INSTANCES` describe twin installations (0,1 = port, starboard).
`HELM_PROFILE` selects the drive-bus decoder in can_bridge (`usv722`, or
`none` to keep only the bus statistics). The derived switches can be forced
with `SIGNALK_ENGINE`, `CAN_N2K_ENGINE`, `CAN_DM1` and `MOOS_ENGINE` (0/1).

The systemd bridges receive the keys from usv.env directly. grafana_bridge
runs under pAntler as the mission user and reads `/etc/usv/usv.env` itself
at start (a `USV_ENV_FILE` environment variable points elsewhere);
`engine_passthrough` and `engine_type` in its ProcessConfig block override
the file. The file must therefore be readable by that user: install.sh,
run with sudo from the mission user, sets its group to that user with mode
640. A file grafana_bridge cannot open gives the default profile (every
producer writes, speeds in m/s) and its startup line in the pAntler output
ends with `WARNING: cannot read /etc/usv/usv.env`. install.sh prints the
resolved profile and, for `moos`, the mission-file block.

Leaving `ENGINE_SOURCE` empty keeps the pre-v1.2.20 behaviour: every
producer writes, which is only right while one of them has data.

On the MOOS path grafana_bridge maps the iCANRec variables ENGINE_SPEED,
ENGINE_COOLANT_TEMP, ENGINE_OIL_TEMP, ENGINE_OIL_PRESSURE, ENGINE_FUEL_RATE,
ENGINE_PERCENT_LOAD, ENGINE_PERCENT_TORQUE,
ENGINE_ACCELERATOR_PEDAL_POSITION, ENGINE_INTAKE_MANIFOLD_PRESSURE,
ENGINE_HOURS, ENGINE_FUEL_LEVEL, ENGINE_ACTIVE_DTCS, ENGINE_DTC_SPN /
ENGINE_DTC_FMI / ENGINE_DTC_OC (first active fault), ENGINE_LAMPS (the DM1
lamp byte, split into the four lamp fields), ENGINE_DTC_TEXT ("SPN n FMI m
(xk); ..." or "NONE"; the bridge adds the Mercury J code and fault name on
a mercury_diesel hull, the J1939 failure-mode wording elsewhere) and
ENGINE_BATTERY_VOLTAGE (the ECM's own supply, `engine.ecu_voltage`; the
house battery is the Victron's through Signal K). A variable with a `_<n>`
suffix (ENGINE_SPEED_1) is engine instance n. Variables iCANRec does not
publish yet simply leave their tiles at N/A.

## Services
| Unit | Role |
|---|---|
| influxdb.service | time-series database (localhost:8086) |
| grafana-server.service | dashboards + alerting (GRAFANA_PORT in usv.env) |
| telegraf.service | OBC metrics, ping, service supervision |
| usv-signalk-bridge.service | Signal K deltas -> Influx (single vessel) |
| usv-signalk-bridge@<vessel>.service | one per `VESSELS` entry (simulation fleet) |
| usv-can-bridge.service | CAN/N2K frames -> Influx (CAP_NET_RAW) |
| usv-mayara-bridge.service | mayara radar targets -> MOOSDB RADAR_TARGET (only when MAYARA_URL is set) |
| usv-frigate-bridge.service | camera status from the Frigate NVR -> Influx (only when FRIGATE_URL is set) |
| usv-auth.service | Signal K login for the dashboard (only with SK_LOGIN=1) |
| nginx.service | answers on GRAFANA_PORT in front of Grafana (only with SK_LOGIN=1) |
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
| Engine profile (type, source, count, helm decoder) | ENGINE_TYPE, ENGINE_SOURCE, ENGINE_COUNT, ENGINE_INSTANCES, HELM_PROFILE in usv.env; ingestion/cpp-common/engine_profile.hpp |
| MOOS variable map | ingestion/moos-cpp/moos_bridge.cpp |
| Signal K path map | ingestion/signalk-cpp/signalk_bridge.cpp kPaths |
| PGN decoders | ingestion/can-cpp/main.cpp decode_pgn() |
| Alert thresholds | tools/gen_alerts.py --profile sim\|vessel -> regenerate -> re-run install.sh (vessel profile: GPS Signal Loss and CAN Bus Failure also alert on missing data; engine and battery rules never do, the Readiness dashboard reports silent sensors) |
| Branding text | BRAND_TITLE / BRAND_WELCOME / BRAND_SUBTITLE / BRAND_APP_TITLE / BRAND_DISCLAIMER in usv.env |
| Home dashboard | GRAFANA_HOME_DASHBOARD in usv.env (default usv-readiness.json) |
| Readiness profile, vessel constants, app list, checklist items | grafana_bridge ProcessConfig in the mission file: readiness_profile, readiness_period_s, fuel_tank_l, cruise_fuel_lph, cruise_speed_kn, battery_kwh, cruise_power_kw, endurance_margin, required_apps, optional_apps, checklist_items, enc_fitted, console_checklist, enc_poll_s (see example.moos) |
| Console mirrors | CHECKLIST_STATUS and ENC_STATUS published by the operator console (spec above) |
| Tab order, tiles, checks table | tools/gen_dashboards.py (TAB_ORDER, usv-readiness section) -> regenerate -> re-run install.sh |
| Readiness rules | usv::EvaluateReadiness in ingestion/moos-cpp/mission_logic.hpp (unit-tested in mission_logic_test.cpp) |
| Fleet instances | VESSELS in usv.env |
| Firewall sources for Grafana | GRAFANA_ALLOW_FROM in usv.env (CIDR list; unset = LAN subnet + 100.64.0.0/10; `none` = leave ufw alone) |
| Signal K login | SK_LOGIN, GRAFANA_INTERNAL_PORT, USV_AUTH_PORT, SK_LOGIN_SIGNALK, SK_ROLES_FILE, SK_ROLE_MAP in usv.env (section above); nginx template deploy/nginx/usv-dashboard.conf.in |
| Audit page access | folder Swift Autonomous Audit, Editor and Admin (deploy/install.sh); audit rows kept 365 days (events bucket) |
| Alert recipients | grafana/provisioning/alerting/contact-points.yml |
| Dashboard refresh | 1 s on the lighter live pages, 2 s on pages with more than 18 queries (Navigation, Traffic, Autopilot, Mission, Vessel Health, System, Alarms): the browser runs six queries at a time and each refresh cancels what is still in flight, so at 1 s the lower panels of a heavy page never fill (Replay: none); the time picker changes it per page for the session. Grafana floor `GRAFANA_MIN_REFRESH` in usv.env, default 1 s (`500ms` allows faster at double the query load; `2s` eases a slow computer) |

## Roles (Operator / Engineer / Administrator)
With the Signal K login (SK_LOGIN=1) the role follows the boat role
(table above): viewers and planners are Grafana Viewers, operators and
supervisors Editors (the Audit page needs Editor). Without it, Grafana's
own users: Administrator -> Grafana Admin, Engineer -> Editor, Operator ->
Viewer (`?kiosk` URL for wall displays); create users under
Administration -> Users and make whoever must read the Audit page an
Editor.

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
| Readiness "no radar data" with the radar on | `journalctl -a -u usv-mayara-bridge -n 10`: "mayara unreachable" every few seconds = old mayara build (see Radar targets, radar server version); "using radar ..." + "power: standby" = switch it to transmit; nothing from the radar on `tcpdump -i <port> 236.6.7.0/24` = cabling or the wrong port in mayara.service (`-i`) |
| Every page "didn't send any data" (ERR_EMPTY_RESPONSE), `/api/health` fine | Grafana cannot parse `views/index.html` (a block brand.sh injected contains two square brackets in a row): `sudo bash deploy/brand.sh --restore && sudo systemctl restart grafana-server` brings the stock page back (header logo missing until brand.sh runs again); then update to a fixed release |
| Dashboard asks for a Grafana login although Swiftships-UI is logged in (SK_LOGIN=1) | `journalctl -u usv-auth -n 20`; `curl -s http://127.0.0.1:3000/skServer/loginStatus` (Signal K security on?); the browser must open the dashboard on the same host name as Swiftships-UI (cookies follow the host name, not the port) |
| Signal K login user lands as Viewer though an operator | the roles file (`sudo cat /var/lib/swiftships-c2/roles.json`) and `SK_ROLE_MAP`; a read-only Signal K account is always a viewer; Grafana > Administration > Users shows the role usv-auth set |
| Audit page "permission denied" | expected for Viewers; an operator needs the Editor role (Signal K login, or a Grafana user made Editor). Installer warning "could not restrict the Audit folder": set it by hand under Dashboards > Swift Autonomous Audit > Permissions |
| Navigation integrity "not reporting" | pNavGuard is in the mission's app list but not posting: Apps row (missing: pNavGuard) and the pAntler output |
| Readiness "MISSION OFF" ("NO BRIDGE" before v1.2.49), Mission State and Operator Link N/A | grafana_bridge is not writing: it runs under pAntler, so it stops with the mission. Start the mission (and restart it after an update that changes grafana_bridge); read its start line in the pAntler output |
| Dashboard hangs on a dark screen from another PC, but `curl http://localhost:<GRAFANA_PORT>/api/health` returns 200 on the OBC | ufw is dropping the port: `sudo ufw status \| grep <GRAFANA_PORT>`. Re-run install.sh (opens it to the LAN and the tailnet, see GRAFANA_ALLOW_FROM in usv.env) or `sudo ufw allow from <lan-cidr> to any port <GRAFANA_PORT> proto tcp` |
| Stock Grafana logo / "Welcome to Grafana" | Browser cache of Grafana's immutable assets: Ctrl+F5 once; `sudo bash deploy/brand.sh` re-applies after an upgrade |
| Light theme chip "does nothing" | It opens a new tab (Grafana reads ?theme only on page load); press `c` then `t` to switch in place |
| grafana_bridge missing under pAntler (`Failed exec - not good`) | set MOOS_ANTLER_UNIT in usv.env and re-run install.sh (PATH drop-in) |
| Two values alternate on an engine tile or trend (one of them iCANRec's), or Autopilot speed is in m/s on a boat | grafana_bridge cannot read usv.env (its pAntler output says `WARNING: cannot read /etc/usv/usv.env`): `sudo chgrp <mission user> /etc/usv/usv.env && sudo chmod 640 /etc/usv/usv.env`, restart the mission. Re-running install.sh with sudo from that user does the same |
| Route tile shows a short id | Signal K unreachable from grafana_bridge: check `signalk_http` in the ProcessConfig |
| No MOOS data | grafana_bridge in pAntler list? `umm --spy` shows the vars? token/org in ProcessConfig correct? |
| No CAN data | `candump <CAN_INTERFACE>` (slcan0 on both hulls); `sudo journalctl -u usv-can-bridge -f` |
| Signal K panel red | Signal K on :3000? URL in /etc/usv/usv.env correct? |
| Alerts not firing | Grafana -> Alerting -> Rules; datasource health |
