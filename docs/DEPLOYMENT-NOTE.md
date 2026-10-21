# Swift Autonomous Dashboard — Deployment Technical Note

Field reference for installing the **Swift Autonomous Dashboard** (Grafana \+
InfluxDB monitoring stack) on the USV onboard computer. Native deployment,
no Docker. All vessel\-side code is C\+\+. The Grafana workspace presents as
"Swift Autonomous Dashboard" with the Swiftships logo top\-left, 14
tab\-navigable single\-screen dashboards and the Autopilot Live drill\-down.
One release serves every hull: what differs per boat (engine, drive bus,
vessel particulars) is set in `/etc/usv/usv.env`. Companion docs: `deployment.md` (full guide),
`schema.md`, `traceability.md`, `alpha-mission-integration.md` (the Alpha
mission variable map, derived state machine and design rationale).

* * *

## 1\. Architecture at a glance

```
                 ONBOARD COMPUTER (Debian/Ubuntu)
 ┌──────────────────────────────────────────────────────────────┐
 │  pAntler (your mission file)                                 │
 │    MOOSDB ── pHelmIvP ── pMarinePID ── ... ── grafana_bridge─┼─┐
 │                                                              │ │
 │  systemd services                                            │ │
 │    signalk_bridge  ◄── Signal K server (ws :3000)      ──────┼─┤
 │    can_bridge      ◄── SocketCAN (CAN_INTERFACE, 250k) ──────┼─┤
 │    mayara_bridge / frigate_bridge (radar, cameras; optional) ┼─┤
 │    telegraf        ◄── host metrics, ping, systemd units ────┼─┤
 │                                                              │ ▼
 │    InfluxDB 2.x (:8086)  buckets: telemetry / events /       │
 │                          telemetry_1m (downsampled)          │
 │    Grafana (:GRAFANA_PORT) 15 dashboards + 24 alert rules    │
 └──────────────────────────────────────────────────────────────┘
      Operator access: http://<obc>:<GRAFANA_PORT> (3001 vessel / 3300 sim)
```

Data path: all writers batch line protocol to InfluxDB over localhost HTTP;
Grafana reads via Flux. Everything auto\-reconnects and restarts
(`Restart=always` for services, pAntler for the MOOS app).

* * *

## 2\. Pre\-deployment checklist

Confirm before starting; each item is required.

- [ ] OBC runs Debian 12 / Ubuntu 22.04\+ (amd64 or arm64), root access
- [ ] Internet available dockside (apt repos \+ one GitHub clone \+ one
  Grafana plugin download for the analog instruments). The vessel does
  NOT need internet after install. Offline install: regenerate with
  `tools/gen_dashboards.py --classic` (no plugin needed)
- [ ] MOOS\-IvP built on the OBC; note the path to `MOOSConfig.cmake`
  (source tree: `~/moos-ivp/build/MOOS/MOOSCore`)
- [ ] Signal K server running; confirm `ws://localhost:3000/signalk/v1/stream`
- [ ] CAN interface present: `ip link show <iface>` (both hulls use the
  serial\-line adapter `slcan0`; `CAN_INTERFACE` in usv.env). A native
  interface is brought up with
  `sudo ip link set can0 up type can bitrate 250000` and persisted
  (systemd\-networkd or /etc/network/interfaces)
- [ ] The hull's engine and particulars at hand: engine type and model,
  drive bus (`HELM_PROFILE`), and the general arrangement drawing (class,
  dimensions, payload, fuel capacity, range, maximum speed, equipment).
  Both existing hulls' values are in `deploy/usv.env.example`
- [ ] \~2 GB free disk for packages \+ \~100 MB/day telemetry growth budget
- [ ] Clock sane (NTP or GPS time) — InfluxDB timestamps depend on it

## 3\. Installation procedure

```bash
# 0. OPTIONAL but recommended: drop the official Swiftships logo (SVG,
#    square aspect) over the placeholder BEFORE installing:
#    deploy/branding/swiftships_logo.svg

# 1. Unpack the repo on the OBC, then:
sudo bash deploy/install.sh            # creates /etc/usv/usv.env, exits

# 2. Edit secrets. Generate a token first:
openssl rand -hex 32
sudo nano /etc/usv/usv.env        # INFLUX_TOKEN, both passwords, VESSEL_ID,
                                  # GRAFANA_PORT, CAN_INTERFACE, engine profile
                                  # (ENGINE_TYPE/MODEL/SOURCE, HELM_PROFILE) and
                                  # VESSEL_* particulars; quote values with spaces

# 3. Run the installer for real:
sudo bash deploy/install.sh
#    If MOOS isn't auto-found:
sudo MOOS_DIR=$HOME/moos-ivp/build/MOOS/MOOSCore deploy/install.sh
```

Run the installer with sudo from the user that runs the mission (pAntler):
it gives `/etc/usv/usv.env` to that user's group with mode 640, because
`grafana_bridge` reads the file itself. Updates of an installed boat use
`bash deploy/update.sh <tag>` from the repo checkout (tags only).

The installer is idempotent. Re\-run it after editing `/etc/usv/usv.env`,
regenerating dashboards, changing alert rules, replacing the logo, or any
Grafana package upgrade (upgrades restore Grafana's stock logo; the
installer re\-applies the Swiftships branding).

## 4\. MOOS integration (manual step, you own this)

1. Merge `ingestion/moos-cpp/example.moos` into your mission file:
   `Run = grafana_bridge @ NewConsole = false` in ANTLER, plus the
   `ProcessConfig = grafana_bridge` block.

2. **Critical:** in the ProcessConfig block write the URL without a scheme
   (`influx_url = localhost:8086`). `//` starts a MOOS comment and will
   silently truncate `http://...`.

3. The variable map is **pre\-aligned with the Alpha mission**
   (`alpha.bhv`): mode flags, COMMS\_LOST failsafe \+ MISSION\_ARMED
   re\-arm latch, SURVEY\_ENTRY\_GATE, DIST\_TO\_NEXT\_WPT / WPT\_INDEX /
   VIEW\_SEGLIST, OPR\_BREACHED geofence, OBSTACLE\_ALERT and IVPHELM\_\*
   are all subscribed; the operational mission state is derived onboard
   (see `alpha-mission-integration.md`). Verify with `umm --spy` during a
   sim run, or poke a scenario by hand:

   ```bash
   moos_poke WAYPOINT=true DIST_TO_NEXT_WPT=350     # state -> Autonomous
   moos_poke COMMS_LOST=loiter MISSION_ARMED=false  # state -> CommsLost
   ```

   If your community publishes additional variables, extend the maps in
   `ingestion/moos-cpp/moos_bridge.cpp`, then:

   ```bash
   cmake --build ingestion/moos-cpp/build -j
   sudo cmake --install ingestion/moos-cpp/build
   ```

## 5\. Configuration quick reference

| Item | Location |
| --- | --- |
| Secrets, vessel id, retention | `/etc/usv/usv.env` (then re\-run install.sh) |
| Engine profile (dial scales, engine data source, drive bus) | `ENGINE_*`, `HELM_PROFILE` in `/etc/usv/usv.env` (deployment.md, Engine profile) |
| Vessel particulars (class name, card, fuel litres, speed cap) | `VESSEL_*` in `/etc/usv/usv.env`; restart the mission afterwards |
| MOOS variable map | `ingestion/moos-cpp/moos_bridge.cpp` |
| Signal K path map | `ingestion/signalk-cpp/signalk_bridge.cpp` (kPaths) |
| NMEA 2000 PGN decoders | `ingestion/can-cpp/main.cpp` (decode\_pgn) |
| Alert thresholds | `tools/gen_alerts.py` (vessel profile by default; `--profile sim` on a simulation host) → regenerate → re\-run install.sh |
| Alert email recipients | `grafana/provisioning/alerting/contact-points.yml` |
| Series colors / panels / tabs | `tools/gen_dashboards.py` → regenerate |
| Analog instruments on/off | `tools/gen_dashboards.py` (default) vs `--classic` (plugin\-free) |
| Instruments plugin version pin | `ECHARTS_PANEL_VERSION` in `/etc/usv/usv.env` |
| Swiftships logo | `deploy/branding/swiftships_logo.svg` → re\-run install.sh |
| Theme picker, fit\-to\-screen, nav title | `deploy/brand.sh` (scripts injected into Grafana's index.html; never two square brackets in a row, Grafana parses the file as a template) |
| Workspace (folder) name | `grafana/provisioning/dashboards/dashboards.yml` |
| Telegraf inputs | `telegraf/telegraf.conf` (template; installer fills tokens) |

Dev\-side regeneration (workstation, Python 3):
`python3 tools/gen_dashboards.py && python3 tools/gen_alerts.py`, run
`tools/smoke_test.sh` (FAIL: 0), commit, tag, then `bash deploy/update.sh
<tag>` on the OBC.

## 6\. Commissioning checklist (in order)

| \# | Check | Command | Expect |
| --- | --- | --- | --- |
| 1 | Services up | `systemctl status influxdb grafana-server telegraf usv-signalk-bridge usv-can-bridge` (plus `usv-mayara-bridge` / `usv-frigate-bridge` where configured) | all `active (running)` |
| 2 | DB reachable | `influx ping` | `OK` |
| 3 | Bench data (no vessel) | `sudo systemctl start usv-simulator` | dashboards populate in \~10 s; the scripted Alpha scenario cycles all mission states and fires the geofence/comms alerts once per \~25 min loop (that IS the alert bench test) |
| 4 | Grafana login | browse `http://<obc>:<GRAFANA_PORT>` | Swiftships login page; lands on the Readiness dashboard; folder "Swift Autonomous Dashboard", 15 dashboards; Swiftships logo and "SA-1 Dashboard" (BRAND_APP_TITLE) in the navigation menu; header band shows the class name (VESSEL_CLASS) and engine |
| 5 | Tab navigation | open any dashboard; click the link chips across the top | one click switches dashboard; time range and vessel selection carry over |
| 6 | Theme picker | Theme at the top: Dark, Navy, Light | page reloads once in that theme and keeps it on every page; Navy is the dark theme in the Swiftships navy |
| 7 | Tablet check | open any dashboard on a tablet (landscape) | grid scales; below \~768 px panels stack single\-column |
| 8 | Alert rules loaded | Grafana → Alerting → Alert rules | 24 rules, folder "Swift Autonomous Dashboard" |
| 9 | Stop simulator | `sudo systemctl stop usv-simulator` | required before real ops |
| 10 | MOOS feed | launch mission with grafana\_bridge; watch Autopilot dashboard | desired/actual heading move |
| 10a | Readiness evaluation | Readiness dashboard within 15 s of the community start | 17 rows with evidence; Vessel particulars card filled; "Apps N / N running"; `readiness_profile = vessel` in the ProcessConfig on a real vessel (missing sensors FAIL) |
| 10b | Console mirrors | tick items in the console, select a chart; or `/usr/local/bin/moos_poke 'CHECKLIST_STATUS=done=11,total=11,pending=,by=ops' ENC_STATUS=selected:test` | Console tile 0, checklist table lists every item, ENC row PASS |
| 10b' | ENC from the chart server | ENC cells uploaded to Signal K, console not publishing ENC_STATUS | ENC row "ENC <cell> · covers position · N cells loaded"; bridge log "chart server lists N ENC cells" |
| 10c | Route plausibility | start a mission with a route and region | Course planning shows route, waypoints, length, region, "route inside region"; Endurance shows the litres (VESSEL_FUEL_L) and fuel / battery hours when the vessel constants are set; Speed cap compares SPEED_CAP with VESSEL_MAX_SPEED_KN |
| 11 | Signal K feed | Navigation \+ Vessel Health dashboards | position, engine, battery live |
| 12 | CAN feed | `candump <CAN_INTERFACE>` shows frames; CAN Bus dashboard | msg rate \> 0, interface UP |
| 13 | Alert fire test | unplug GPS antenna (or stop Signal K) for 1 min | alarm appears on Alarms dashboard \+ email |
| 14 | Reboot test | `sudo reboot` | everything returns without manual action |

Logs while commissioning:
`journalctl -u usv-can-bridge -u usv-signalk-bridge -f`
and the grafana\_bridge console output under pAntler.

## 7\. Security notes

- `/etc/usv/usv.env` is readable by root and by the group of the mission
  user only (640; grafana\_bridge reads it under pAntler). The Influx token
  also sits in your mission file's ProcessConfig: keep mission files off
  public repos.
- Bind exposure: Grafana (GRAFANA_PORT) and InfluxDB (8086) listen on all
  interfaces by default. When ufw is active, install.sh allows GRAFANA_PORT/tcp
  only from the LAN subnet of the default-route interface and the Tailscale
  range 100.64.0.0/10 (rule comment `usv-grafana`). Override the sources with
  `GRAFANA_ALLOW_FROM` in `/etc/usv/usv.env`, or set it to `none` to manage
  ufw yourself. Keep 8086 closed to the network; every writer uses localhost.
- Change the Grafana admin password at first login even though the
  installer sets one.
- The `usv` service user is unprivileged; can\_bridge gets only CAP\_NET\_RAW.

## 8\. Known limitations and gotchas

1. **MOOS `//` comment rule** truncates URLs in .moos files (see §4.2).
2. **Variable maps are aligned with alpha.bhv** (§4.3). Empty
   Mission/Autopilot panels on a different mission file almost always mean
   a naming mismatch — `moos_poke` + `umm --spy` localize it in minutes.
   The derived-state logic itself is unit-tested (`mission_logic_test`,
   also run by `tools/smoke_test.sh`).
3. **Fast\-packet PGNs are not decoded** by can\_bridge (counted in stats
   only). Position/engine data arrive via single\-frame PGNs and Signal K,
   so no dashboard gap; extend `decode_pgn()` if you need 129029 etc.
4. **Radar (FR\-6) and camera (FR\-7) feeds.** Radar targets come from
   mayara-server through `mayara_bridge` (`MAYARA_URL`) or from any MOOS
   app posting `RADAR_TARGET` / `NODE_REPORT`; camera status comes from the
   Frigate NVR through `frigate_bridge` (`FRIGATE_URL`). Bitrate,
   temperature and PTZ position need a camera-API adapter (no source yet).
   The simulator covers both for demo.
5. **Comms tiles.** Internet, latency and packet loss are Telegraf ICMP
   probes to 8.8.8.8; Starlink is an ICMP probe to the dish at
   192.168.100.1 (shipped enabled; the dish must be routable from the
   OBC). The VPN tile is UP when a Twingate, Tailscale, OpenVPN or
   WireGuard unit is active on the OBC (usv-722: `twingate-connector`), and
   reads N/A when the VPN terminates on the router.
6. **Alert rule provisioning is Grafana\-version\-sensitive.** Validated as
   YAML; confirm the 24 rules appear in the UI on your installed Grafana
   version during commissioning step 8 (verified on Grafana 13.2.1).
7. **Mission replay** is Grafana\-native time\-range scrubbing, not a
   play/pause button.
8. **Retention:** raw 30 d, events \+ 1\-min downsample 365 d. Missions older
   than 30 d replay at 1\-min resolution.
9. **Branding is an asset overwrite, not a supported OSS feature.** Grafana
   OSS has no white\-label config, so the installer overwrites the shipped
   nav logo and favicon. An `apt upgrade grafana` restores the stock logo
   until the installer is re\-run. The browser tab title and the name next
   to the logo come from `BRAND_TITLE` / `BRAND_APP_TITLE` in usv.env.
10. **Logo assets are the official Swiftships mark.**
    `deploy/branding/swiftships_logo.svg` (nav icon, vessel mark) and
    `swiftships_wordmark.png` (full horizontal logo) are built from the
    official artwork; `swiftships_logo_light.svg` and
    `swiftships_wordmark_light.png` are white\-knockout variants generated
    from them for dark surfaces. The installer puts the knockout mark in
    the (dark) nav bar so the logo is always visible, and keeps the navy
    original for the favicon. To update branding later, replace the
    official files, regenerate the knockouts, and re\-run the installer.
11. **Tab row is tag\-driven.** The chips list every dashboard tagged
    `usv`; a new dashboard saved with that tag automatically appears as a
    tab on all dashboards. The current dashboard is not highlighted in the
    row (Grafana OSS renders all chips identically).
12. **Theme picker is per browser.** The Theme variable at the top (Dark,
    Navy, Light) is remembered in the browser and carried between pages;
    Grafana picks Light or Dark when it serves a page, so a change reloads
    once. The ☀/☾ chips still open a page in a new tab in that theme. The
    server default per user stays Profile → Preferences → Interface theme.
    The series palette is validated for the dark and light themes.
13. **Analog instruments need the Business Charts plugin.** The compass,
    speed, rudder, throttle, heading\-error and machinery meters render
    through `volkovlabs-echarts-panel` (signed, React\-based, safe on
    Grafana 11\+; Angular gauge plugins were deliberately avoided). The
    installer installs it once; plugins live in
    `/var/lib/grafana/plugins` and survive `apt upgrade grafana`. Without
    the plugin those panels show "panel not found" — regenerate with
    `--classic` for a plugin\-free install.
14. **Responsive behavior is Grafana\-native.** PC and tablet\-landscape
    render the full 24\-column grid scaled to width; below \~768 px
    (tablet portrait / phone) panels stack in one column in dashboard
    order. No configuration needed; there are no separate mobile
    dashboards to maintain.

## 9\. Rollback / uninstall

```bash
sudo systemctl disable --now usv-signalk-bridge usv-can-bridge usv-mayara-bridge usv-frigate-bridge usv-simulator
sudo rm /etc/systemd/system/usv-*.service && sudo systemctl daemon-reload
sudo apt remove influxdb2 grafana telegraf     # add --purge to drop data
sudo bash deploy/brand.sh --restore            # Grafana's own logo and page back
sudo rm -rf /etc/usv /usr/local/bin/{can_bridge,signalk_bridge,frigate_bridge,mayara_bridge,usv_simulator,moos_bridge,grafana_bridge,moos_poke,usv_gpu_probe}
# and remove the grafana_bridge lines from your mission file
```

Data lives in /var/lib/influxdb and /var/lib/grafana — back up before purge:
`influx backup /path/backup --token $INFLUX_TOKEN`.

## 10\. Quick command reference

Two hulls share the same user, folders and service names: run `hostname`
before any command that changes a service or the network.

```bash
sudo journalctl -u usv-can-bridge -f       # bridge logs (sudo: the mission user is not in systemd-journal)
systemctl restart usv-signalk-bridge       # restart a bridge
influx query 'from(bucket:"telemetry") |> range(start:-1m) |> count()' \
  --org usv --token $INFLUX_TOKEN          # is data arriving?
candump slcan0 | head                      # raw CAN sanity (CAN_INTERFACE)
umm --spy                                  # live MOOS variables
du -sh /var/lib/influxdb                   # disk growth
```

Grafana wall display: append `?kiosk` to any dashboard URL.

```
http://<obc>:<GRAFANA_PORT>/d/usv-mission?kiosk
```
