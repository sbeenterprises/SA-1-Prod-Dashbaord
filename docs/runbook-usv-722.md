# Runbook — SA-1 Dashboard on usv-722 (production)

Day-to-day operation of the dashboard stack on the production vessel.
`docs/deployment.md` is the full reference; this page is what the person
on the boat needs.

## The installation

| Item | Value |
|---|---|
| Onboard computer | OnLogic K800, Ubuntu 22.04.5, hostname `usv-722`, user `douglas` |
| Repo checkout | `~/Documents/SA-1-Prod-Dashbaord` (GitHub `sbeenterprises/SA-1-Prod-Dashbaord`, install from a tag, never from `main`) |
| Installed release | v1.2.33, installed 2026-10-16, with its theme script removed from `/usr/share/grafana/public/views/index.html` by hand because it emptied every page (fixed in v1.2.34; install v1.2.35 next). History: v1.2.19 to v1.2.28 on 2026-10-13/15, v1.2.31 on 2026-10-16 (see `git -C ~/Documents/SA-1-Prod-Dashbaord describe --tags`); configuration backup from before the engine profile in `/etc/usv/usv.env.bak-2026-10-13` |
| Configuration | `/etc/usv/usv.env` (group douglas, mode 640 from v1.2.29 so that grafana_bridge can read it; root only before): `VESSEL_ID=usv-722`, `GRAFANA_PORT=3100`, `CAN_INTERFACE=slcan0`, `ECHARTS_PANEL_VERSION=7.2.5`, SMTP not yet set; engine profile `ENGINE_TYPE=yamaha_outboard`, `ENGINE_MODEL="Yamaha Outboard Engine F115 B 4-Stroke"`, `ENGINE_SOURCE=signalk`, `HELM_PROFILE=usv722`; `SIGNALK_FUEL_TANK=none` and `SIGNALK_BATTERY_CURRENT_SIGN=-1` (see Known gaps); `NAV_SPEED_UNITS=knots` (v1.2.23); vessel particulars `VESSEL_CLASS="S2 Sea-Striker"`, `VESSEL_LOA_M=4.76`, `VESSEL_BEAM_M=1.68`, `VESSEL_DRAFT_M=1.04`, `VESSEL_PAYLOAD_KG=159`, `VESSEL_FUEL_L=265`, `VESSEL_RANGE_NM=320`, `VESSEL_MAX_SPEED_KN=30`, propulsion and ride control (v1.2.33) |
| Grafana | `http://<obc-ip>:3100`, user `admin`, password in `usv.env`; folder "Swift Autonomous Dashboard", home page Readiness |
| InfluxDB | localhost:8086, org `usv`, buckets `telemetry` (30 d), `events` and `telemetry_1m` (365 d) |
| Position source | VectorNav VN-x00 INS through the Signal K plugin `signalk-vectornav-vnins` (no MOOS NAV_LAT) |
| CAN | `slcan0` on the K800 eMCU, `slcand -s5` (250 kbit/s) at boot |
| Radar | Navico HALO20 (mayara id `nav6959`, 169.254.42.254) on `enp11s0`; mayara-server `d4deea0` in `~/Documents/radar/mayara-server` (the older checkout in `~/Documents/radar/mayara` is unused), `mayara.service`: `-p 3001 -i enp11s0 -n tcp:127.0.0.1:8375`, ExecStartPre sets 169.254.10.10/16 and the multicast route on enp11s0 |
| GPU | Intel Arc Pro A60 (`intel_gpu_top` probe, sudoers rule `usv-telegraf-gpu`) |
| Starlink | dish at 192.168.100.1, probed by Telegraf |
| VPN | Twingate connector on the OBC (`twingate-connector.service`); Comms tile "VPN (Twingate)" |
| Chart UI | Swiftships-UI on port 4200, `radar-ui.service` serving `~/Documents/Swiftships-UI-navy/public`; Signal K on 3000 |
| MOOS mission | `~/Documents/automation/moos-ivp-morgan-stern-drive-mercury-2026/missions/s1_morgan_city_signalk/real_usv722.moos` (branch feature/command-auth since 23 Oct 2026), community `alpha`, MOOSDB on 9000, launched by hand with `pAntler real_usv722.moos` (no systemd unit yet). The repository name is historical: this hull's engine is a Yamaha outboard; the Mercury diesel sterndrive is usv-725 |

## What was added to real.moos

Two additions, nothing removed (backup: `real.moos.bak-2026-10-01` next
to it). The bridge only subscribes to MOOS variables and writes its own
readiness rows; it never publishes to the helm.

1. In `ProcessConfig = ANTLER`, after `Run = iRadarTargets`:

```
  Run = grafana_bridge  @ NewConsole = false
```

2. At the end of the file:

```
//------------------------------------------
// grafana_bridge: MOOS -> InfluxDB (SA-1 Dashboard)
ProcessConfig = grafana_bridge
{
  AppTick   = 10
  CommsTick = 20

  influx_url    = localhost:8086   // no scheme: // starts a MOOS comment
  influx_tls    = false
  influx_token  = <INFLUX_TOKEN from /etc/usv/usv.env>
  influx_org    = usv
  influx_bucket = telemetry
  events_bucket = events
  vessel_id     = usv-722

  mission_name   = alpha
  max_rate_hz    = 5
  engine_rate_hz = 2
  ignore_vars    = NAV_YAW         // iDivisorNMEA posts rudder angle as NAV_YAW
  radar_cpa_alert_m = 500
  tcpa_alert_s   = 300
  contact_ttl_s  = 10
  signalk_http   = localhost:3000

  readiness_profile  = vessel
  readiness_period_s = 5
  optional_apps = pMarineViewer,pContactSpawn
  enc_fitted = false          // console does not publish ENC_STATUS; the ENC
                              // row reads the chart server (Signal K) instead
  console_checklist = false   // UI keeps its checklist local
}
```

What each non-default line does:

| Line | Why |
|---|---|
| `influx_token` | Same value as `INFLUX_TOKEN` in `/etc/usv/usv.env`. Keep `real.moos` off public repos. |
| `vessel_id = usv-722` | Tag on every point; must equal `VESSEL_ID` in usv.env. Do not change it: the history is keyed on it. |
| `ignore_vars = NAV_YAW` | `iDivisorNMEA` publishes the rudder angle as NAV_YAW on this hull; without this the heading tiles would show rudder angle. |
| `readiness_profile = vessel` | A check with no sensor data FAILS (on the simulation host it reads N/A). |
| `optional_apps` | GUI viewer and the test-contact injector: reported if absent, never a readiness fault. |
| `enc_fitted = false` | The console does not publish `ENC_STATUS`. Since v1.2.42 the Chart / ENC row reads the ENC cells Signal K serves (ten NOAA cells uploaded 2026-10-19: US1GLBCF, US3LA1CB, US3LA1CC, US3MS1AA, US5LMO**) and passes on the cell covering the boat, for example `ENC US3LA1CB · covers position · 10 cells loaded`; it fails when no loaded cell covers the position. This line only matters when the chart server is unreachable and the route is not named "(safe)": the row then reads N/A instead of CONSOLE. Delete it once the console publishes `ENC_STATUS`. |
| `console_checklist = false` | The Swiftships UI keeps its pre-departure checklist local (not published). Delete when the UI publishes `CHECKLIST_STATUS`. |

Optional lines to add once the numbers are known (turn the Endurance row
from N/A into hours remaining): `fuel_tank_l`, `cruise_fuel_lph`,
`cruise_speed_kn`, `battery_kwh`, `cruise_power_kw`, `endurance_margin`.

Any change to this block needs the community restarted; the bridge reads
its configuration at start.

## Daily operation

**Start the mission.** Check nothing is already running, then launch:

```bash
pgrep -a MOOSDB; pgrep -a pAntler          # both must print nothing
cd ~/Documents/automation/moos-ivp-morgan-stern-drive-mercury-2026/missions/s1_morgan_city_signalk
pAntler real_usv722.moos
```

Since 23 Oct 2026 the boat runs the `-2026` checkout (branch
feature/command-auth, local edit `stop_on_fault = false` in the pNavGuard
block). A second `pAntler` on the same host attaches duplicate apps to the first
MOOSDB (seen on 2026-10-01: two `grafana_bridge` instances, stale
readiness rows). Always check before launching.

**Stop the mission.** Ctrl-C in the pAntler terminal, or
`pkill -f pAntler`, then confirm with `pgrep -a MOOSDB`.

**Read the Readiness page.** PASS / FAIL rows are decided onboard from
live data; N/A rows are not fitted on this hull; CONSOLE would mean the
console is expected to publish something (none on usv-722 at present).
READY needs zero FAIL and zero CONSOLE. Alongside with the engine off and
no mission loaded, Course planning, Steering and Engine read FAIL; that is
correct. GPS reads FAIL while the VectorNav reports "INS aligning" (it
re-aligns after a long stop or a power cycle; it tracks once under way).
Cameras reads "N / M cameras streaming" for the cameras enabled in Frigate.

**Boat control.** This boat's mission (22 Oct build) runs with login and
exclusive control (`command_auth = signalk`, `exclusive_control = true`):
log in to Swiftships-UI (`http://<boat>:3000/c2/` with the new UI build)
and take control before commanding; others see your work read-only. ALL
STOP latches until the operator in control resets it; while latched the
dashboard's Mission State reads ALL STOP and Readiness fails Safety flags.
Every login, control change, ALL STOP and refused command is on the Audit
page (and in `/home/douglas/c2_audit/c2_audit_<date>.log` on the boat).
If control cannot be taken at all, check Signal K first: with
`command_auth = signalk` nobody can command while Signal K is unreachable.

**Navigation guard.** With pNavGuard in the mission, Readiness row 2
(Navigation integrity) must read PASS before a start; a FAULT means the
boat refuses autonomy starts (manual control still works). For the first
runs set `stop_on_fault = false` in the pNavGuard block (warnings only).

**Services.** `systemctl status influxdb grafana-server telegraf usv-signalk-bridge usv-can-bridge usv-mayara-bridge usv-frigate-bridge`
(and `usv-auth nginx` once the Signal K login is on).
All are enabled at boot. `usv-simulator` must stay disabled on the vessel
(`systemctl is-enabled usv-simulator` -> `disabled`).

**Logs.** `sudo journalctl -u usv-signalk-bridge -u usv-can-bridge -u usv-mayara-bridge -u usv-frigate-bridge -f`;
`grafana_bridge` prints to the pAntler console.

## Before any command on a boat

Run `hostname` first and read it. Both onboard computers have the same
user, folders and service names, so a command meant for one boat runs
without error on the other (2026-10-18: a radar change meant for usv-725
ran on usv-722 and took its radar off for 8 minutes). For anything that
changes a service or the network, start the pasted block with
`[ "$(hostname)" = usv-722 ] || { echo "WRONG BOAT: $(hostname)"; exit 1; }`.

## Upgrading to a new release

Tags only. One command, from the repo checkout:

```bash
cd ~/Documents/SA-1-Prod-Dashbaord && bash deploy/update.sh v1.2.47
```

It fetches the tag, re-runs the installer (rebuilds and restarts the
bridges, re-provisions dashboards and alerts, reloads Telegraf), lists the
services and prints the rollback command (`bash deploy/update.sh <old>`).
Then Ctrl+F5 in the browser. If `grafana_bridge` changed (see the
CHANGELOG), restart the mission afterwards; the script reminds you when
MOOS is running. Set `MOOS_DIR=...` in front of the command if the
installer cannot find MOOS.

Manual equivalent (when the mission must be stopped first):

```bash
pkill -f pAntler; sleep 8; pgrep -a MOOSDB     # empty
cd ~/Documents/SA-1-Prod-Dashbaord && git fetch --tags && git checkout vX.Y.Z
sudo MOOS_DIR=/home/douglas/Documents/automation/moos-ivp/build/MOOS/MOOSCore \
  bash deploy/install.sh 2>&1 | grep -E "error|WARNING|Built target|Done"
sudo systemctl restart usv-signalk-bridge     # if signalk_bridge changed
cd ~/Documents/automation/moos-ivp-morgan-stern-drive-mercury-2026/missions/s1_morgan_city_signalk && pAntler real_usv722.moos
```

Then Ctrl+F5 in the browser (Grafana caches assets for a year). The
installer is idempotent; it re-provisions dashboards and alert rules,
rebuilds the bridges, re-applies branding and the GPU sudoers rule.

Rollback: `git checkout <previous tag>` and the same install command.

## Signal K login for the dashboard (SK_LOGIN)

One login for Swiftships-UI and the dashboard. Do this once Signal K
security is set up (Signal K admin > Security: users as read-only,
read/write or admin; roles in Swiftships-UI's plugin settings, Boat
control roles):

```bash
[ "$(hostname)" = usv-722 ] || { echo "WRONG BOAT: $(hostname)"; exit 1; }
grep -q '^SK_LOGIN=' /etc/usv/usv.env && sudo sed -i 's/^SK_LOGIN=.*/SK_LOGIN=1/' /etc/usv/usv.env \
  || echo 'SK_LOGIN=1' | sudo tee -a /etc/usv/usv.env
cd ~/Documents/SA-1-Prod-Dashbaord && bash deploy/update.sh v1.2.43
systemctl is-active nginx usv-auth grafana-server
```

Check: log in to Swiftships-UI, open the dashboard at the same address on
its usual port (`http://<obc-ip>:3100`): no login form, the user menu shows
your Signal K user. An operator or supervisor sees the Audit tab's page; a
viewer gets "permission denied" there. Log out of Swiftships-UI and reload:
the Grafana login form returns (the Grafana admin account still works).
Off again: `SK_LOGIN=0` and the same update command.

## Changing configuration

| Change | Where | Then |
|---|---|---|
| Secrets, port, vessel id, SMTP, plugin pin | `/etc/usv/usv.env` | `sudo bash deploy/install.sh` |
| Radar targets on/off, radar id (MAYARA_URL, MAYARA_RADAR) | `/etc/usv/usv.env` | `sudo bash deploy/install.sh` |
| Camera status on/off (FRIGATE_URL), charted battery (SIGNALK_BATTERY) | `/etc/usv/usv.env` | `sudo bash deploy/install.sh` |
| Vessel particulars: class name, card, fuel capacity, max speed (VESSEL_*, v1.2.33; values for this hull in deploy/usv.env.example) | `/etc/usv/usv.env` | `sudo bash deploy/install.sh`, then restart the mission (grafana_bridge reads them at start) |
| Which cameras count for Readiness | Frigate: enable / disable the camera (Configuration Editor, Save & Restart) | nothing; picked up within 60 s |
| Bridge options, readiness profile, vessel constants, ENC / checklist flags | `real.moos` grafana_bridge block | restart the community |
| Alert recipients | `grafana/provisioning/alerting/contact-points.yml` in the repo | commit, tag, install |
| Alert thresholds | `tools/gen_alerts.py --profile vessel` on a workstation | commit, tag, install |
| Dashboards | `tools/gen_dashboards.py` on a workstation (never edit the JSON) | commit, tag, install |
| Telegraf probes (Starlink, GPU, VPN units) | `telegraf/telegraf.conf` in the repo | commit, tag, install |
| Screen height all pages fit | `SCREEN_ROWS` in `tools/gen_dashboards.py` (19 rows = 1080p) | regenerate, commit, tag, install |

## Alert email

Alerts show on the Alarms dashboard today; email needs the SMTP block in
`usv.env` and a re-run of the installer:

```
SMTP_HOST=smtp.office365.com:587
SMTP_USER=<sending mailbox>
SMTP_PASSWORD=<its password>
SMTP_FROM_ADDRESS=<sending mailbox>
SMTP_FROM_NAME="SA-1 Dashboard"
SMTP_STARTTLS_POLICY=MandatoryStartTLS
```

Recipient: `mali@swiftships.com` (contact-points.yml). Test from Grafana:
Alerting > Contact points > usv-operations > Test.

## Backups

```bash
source <(sudo grep ^INFLUX_TOKEN= /etc/usv/usv.env)
influx backup /path/to/backup --token $INFLUX_TOKEN
sudo tar czf /path/to/backup/grafana-usv-$(date +%F).tgz /var/lib/grafana /etc/usv/usv.env
```

`/var/lib/influxdb` grows about 50 to 100 MB/day with 30-day raw
retention (`du -sh /var/lib/influxdb`).

## Troubleshooting

| Symptom | Check |
|---|---|
| Readiness rows stuck / contradictory | `ps -o pid=,lstart=,cmd= -C pAntler -C MOOSDB -C grafana_bridge`: exactly one of each |
| GPS row FAIL "no position" while Signal K shows one | `sudo systemctl restart usv-signalk-bridge`; `curl -s localhost:3000/signalk/v1/api/vessels/self/navigation/vectornav/insMode` should be 2 |
| GPS row "INS aligning" / "GNSS lost"; SOG / COG No data | VectorNav state, not the dashboard (the plugin publishes SOG/COG only when tracking). Read the status: `sudo timeout 2 cat /dev/serial/by-id/usb-FTDI_USB-RS232_Cable_AU04Q2FP-if00-port0 \| grep -m3 VNINS`, 3rd field last digit: 1 aligning without GNSS fix (antenna / sky view), 5 aligning with fix (needs motion), 6 tracking. Reset the INS only if stuck: `printf '$VNRST*4D\r\n' \| sudo tee <same device> >/dev/null` |
| VPN tile N/A | `systemctl is-active twingate-connector`; Telegraf watches `twingate*`, `tailscaled`, `openvpn*`, `wg-quick@*` units |
| Bridge change not visible after an update | `systemctl status usv-can-bridge usv-signalk-bridge` start time; releases before v1.2.15 did not restart them: `sudo systemctl restart usv-can-bridge usv-signalk-bridge` |
| CAN bus 0 msg/s with interface UP | `candump slcan0`; N2K backbone powered? wiring/termination (~60 ohm CAN-H to CAN-L unpowered); `ps -ef | grep slcand` shows `-s5` |
| GPU tile No data | `sudo -u telegraf sudo -n /usr/local/bin/usv_gpu_probe` must print one `gpu,vendor=intel` line in ~3 s |
| Starlink tile N/A | `ping 192.168.100.1` from the OBC; dish must be routable |
| Stock Grafana logo / wrong tile colours after an upgrade | Ctrl+F5; `sudo bash deploy/brand.sh` after any `apt upgrade grafana` |
| Dashboard unreachable from the LAN | `curl localhost:3100/api/health` on the OBC; ufw is inactive on usv-722, so check the network path |
| Empty dashboards | `systemctl status usv-*`; `influx ping` |
| No radar contacts on the Traffic tab | Radar transmitting? Target acquired in mayara (guard zone / click)? `journalctl -u usv-mayara-bridge -n 20`; `curl -s localhost:3001/signalk/v2/api/vessels/self/radars/nav6959/targets` |
| Each radar target shown twice | iRadarTargets and mayara_bridge both post RADAR_TARGET: keep one |
| Cameras row FAIL "N / M cameras streaming" | The camera named DOWN on the Cameras page: power / cable (runbook of the camera system), or disable it in Frigate if it is not fitted yet; `journalctl -u usv-frigate-bridge -n 5` |
| Cameras row "Frigate NVR not reachable" | `sudo docker ps --filter name=frigate`; `curl -s http://127.0.0.1:5000/api/stats \| head -c 200` |
| Electrical / Power panels No data | `curl -s localhost:3000/signalk/v1/api/vessels/self/electrical`: battery instance name; set SIGNALK_BATTERY if it is not the first one |
| AIS contacts thousands of km away, Rel. Bearing wrong | MOOS NAV_LAT/NAV_LONG = 0 (no RMC/HDT reaching iDivisorNMEA); the dashboard falls back to Signal K and logs one "traffic" warning; fix on the mission side |

## Known gaps on this hull (as of v1.2.35)

- Own-ship navigation in MOOS: iDivisorNMEA posts NAV_LAT/NAV_LONG/
  NAV_HEADING = 0 because its UDP feed (port 10111) carries only AIS; the
  helm and pContactMgrV20 therefore see own ship at 0N 0E. Fix with the
  mission developer (valid-RMC/HDT publishing, any talker) plus RMC and
  HDT enabled in Signal K's NMEA0183 output. No autonomous missions until
  `uXMS --server_port=9000 NAV_LAT` reads about 29.73. The dashboard uses
  Signal K meanwhile.
- Rudder feedback: read from the steering actuator on CAN (0x210) since
  v1.2.15; Steering passes when the actuator reports. MOOS itself still has
  no rudder feedback (`$AGRSA` absent).
- Cameras: cameras 3 to 6 and the FLIR are not powered / networked yet;
  keep them disabled in Frigate until they are, so Readiness counts only
  live cameras. Bitrate, temperature and PTZ have no source.
- Fuel level: Signal K holds a `tanks.fuel.0.currentLevel` value (18.7 %
  on 2026-10-13) that it sends once at connect and never updates, so no
  live tank sender is on the bus. `SIGNALK_FUEL_TANK=none` keeps it off
  the Fuel dial, the Low Fuel alert and Readiness. When a sender is
  fitted, delete that line and `sudo systemctl restart usv-signalk-bridge`.
  The drive bus itself (slcan0) does carry a fuel level: the 9 Oct 2026
  capture shows an NMEA 2000 tank sender at address 112 (PGN 127505,
  19F21170) reading 22 to 24 %. can_bridge reads it only with
  `CAN_N2K_ENGINE=1` in usv.env (not set on this hull; check the reading
  against the tank before enabling it, then `sudo systemctl restart
  usv-can-bridge`).
- Signal K reports its own vessel as `vessels.undefined` (no MMSI / UUID
  set in Server > Settings > Vessel). Own-ship data is unaffected; set
  the MMSI when convenient so AIS targets are told apart by context.
- Camera Offline alert fires for the five cameras not yet powered;
  disable them in Frigate until cabled (Cameras section above).
- Comms-loss failsafe, return base and helm collision alerts: the
  dashboard reads COMMS_LOSS, RETURN_HOLD and BASE_COORDS (v1.2.23) and
  COLAV_ALERTS (v1.2.24), which only the 13 October 2026 iWebSocket of
  moos-ivp-morgan-stern-drive-mercury publishes. Until that build runs here
  the Operator Link tiles read LINK OK, Dist to Base reads "no base" and the
  Helm Avoiding (COLREGS) table on Traffic stays empty. Pulling those commits onto this boat conflicts with the local
  grafana_bridge lines in real.moos: merge by hand and keep them.
- Battery current sign: the Victron shunt reports charging as negative
  current (−13 A at 13.86 V on charge, 2026-10-13), the opposite of the
  Signal K convention. `SIGNALK_BATTERY_CURRENT_SIGN=-1` corrects Battery
  Current, the Charging state (derived from the current, the shunt
  publishes no charging mode) and Total Power, which reads negative while
  charging because it is the power drawn from the battery. Points written
  before 2026-10-13 ~23:35 boat time keep the old sign in the trends.
- State of charge and charging mode: the VE.Direct shunt publishes
  voltage and current only.
- ENC: NOAA cells served by Signal K (ENC upload, converted to tiles,
  listed under `/signalk/v1/api/resources/charts`); the Readiness row
  reads that listing. The console's layer selection is not published
  (`enc_fitted = false` only covers the chart server being unreachable).
- Console checklist: not published by the UI; `console_checklist = false`.
- VPN: Twingate connector on the OBC; the tile shows its service state,
  not whether an operator is connected.
- Engine RPM, temperatures, oil pressure, fuel: the hull's engine is a
  Yamaha outboard whose data lives on Yamaha's Command Link network, not on
  this CAN bus. The Yamaha Command Link Plus / NMEA 2000 gateway kit (on
  order) puts it on the N2K backbone, Signal K maps it to `propulsion.*`,
  and signalk_bridge reads it once `ENGINE_TYPE=yamaha_outboard` and
  `ENGINE_SOURCE=signalk` are set in usv.env (v1.2.20). Until the gateway
  is fitted the engine dials read N/A.
- Helm position: `uQueryDB real.moos --condition="NAV_X > -999999"` to
  confirm pHelmIvP receives NAV_X/NAV_Y on this hull (autonomy stack, not
  the dashboard).
- Operating region: confirm `BHV_OpRegion` with a polygon in the `.bhv`;
  the geofence alert and the Course planning row depend on it.
- Mission autostart: pAntler is launched by hand; the reboot test
  (`sudo reboot`, everything returns) is pending.
