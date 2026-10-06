# Runbook — SA-1 Dashboard on usv-722 (production)

Day-to-day operation of the dashboard stack on the production vessel.
`docs/deployment.md` is the full reference; this page is what the person
on the boat needs.

## The installation

| Item | Value |
|---|---|
| Onboard computer | OnLogic K800, Ubuntu 22.04.5, hostname `usv-722`, user `douglas` |
| Repo checkout | `~/Documents/SA-1-Prod-Dashbaord` (GitHub `sbeenterprises/SA-1-Prod-Dashbaord`, install from a tag, never from `main`) |
| Installed release | v1.2.8 (see `git -C ~/Documents/SA-1-Prod-Dashbaord describe --tags`) |
| Configuration | `/etc/usv/usv.env` (root only): `VESSEL_ID=usv-722`, `GRAFANA_PORT=3100`, `CAN_INTERFACE=slcan0`, `ECHARTS_PANEL_VERSION=7.2.5`, SMTP not yet set |
| Grafana | `http://<obc-ip>:3100`, user `admin`, password in `usv.env`; folder "Swift Autonomous Dashboard", home page Readiness |
| InfluxDB | localhost:8086, org `usv`, buckets `telemetry` (30 d), `events` and `telemetry_1m` (365 d) |
| Position source | VectorNav VN-x00 INS through the Signal K plugin `signalk-vectornav-vnins` (no MOOS NAV_LAT) |
| CAN | `slcan0` on the K800 eMCU, `slcand -s5` (250 kbit/s) at boot |
| GPU | Intel Arc Pro A60 (`intel_gpu_top` probe, sudoers rule `usv-telegraf-gpu`) |
| Starlink | dish at 192.168.100.1, probed by Telegraf |
| VPN | OpenVPN terminates on the boat router; Comms VPN tile reads N/A |
| MOOS mission | `~/Documents/automation/moos-ivp-morgan-stern-drive-mercury/missions/s1_morgan_city_signalk/real.moos`, community `alpha`, MOOSDB on 9000, launched by hand with `pAntler real.moos` (no systemd unit yet) |

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
  enc_fitted = false          // no ENC yet: row reads N/A
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
| `enc_fitted = false` | No ENC chart system yet. Delete this line when the ENC is installed and the console publishes `ENC_STATUS`. |
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
cd ~/Documents/automation/moos-ivp-morgan-stern-drive-mercury/missions/s1_morgan_city_signalk
pAntler real.moos
```

A second `pAntler` on the same host attaches duplicate apps to the first
MOOSDB (seen on 2026-10-01: two `grafana_bridge` instances, stale
readiness rows). Always check before launching.

**Stop the mission.** Ctrl-C in the pAntler terminal, or
`pkill -f "pAntler real.moos"`, then confirm with `pgrep -a MOOSDB`.

**Read the Readiness page.** PASS / FAIL rows are decided onboard from
live data; N/A rows are not fitted on this hull; CONSOLE would mean the
console is expected to publish something (none on usv-722 at present).
READY needs zero FAIL and zero CONSOLE. Alongside with the engine off and
no mission loaded, Course planning, Steering and Engine read FAIL; that is
correct. GPS reads FAIL while the VectorNav reports "INS aligning" (it
re-aligns after a long stop or a power cycle; it tracks once under way).
Cameras reads "N / M cameras streaming" for the cameras enabled in Frigate.

**Services.** `systemctl status influxdb grafana-server telegraf usv-signalk-bridge usv-can-bridge usv-mayara-bridge usv-frigate-bridge`.
All are enabled at boot. `usv-simulator` must stay disabled on the vessel
(`systemctl is-enabled usv-simulator` -> `disabled`).

**Logs.** `sudo journalctl -u usv-signalk-bridge -u usv-can-bridge -u usv-mayara-bridge -u usv-frigate-bridge -f`;
`grafana_bridge` prints to the pAntler console.

## Upgrading to a new release

Tags only. A bridge change needs the mission stopped (pAntler holds the
binary open); dashboard-only changes do not.

```bash
pkill -f "pAntler real.moos"; sleep 8; pgrep -a MOOSDB     # empty
cd ~/Documents/SA-1-Prod-Dashbaord && git fetch --tags && git checkout vX.Y.Z
sudo MOOS_DIR=/home/douglas/Documents/automation/moos-ivp/build/MOOS/MOOSCore \
  bash deploy/install.sh 2>&1 | grep -E "error|WARNING|Built target|Done"
sudo systemctl restart usv-signalk-bridge     # if signalk_bridge changed
cd ~/Documents/automation/moos-ivp-morgan-stern-drive-mercury/missions/s1_morgan_city_signalk && pAntler real.moos
```

Then Ctrl+F5 in the browser (Grafana caches assets for a year). The
installer is idempotent; it re-provisions dashboards and alert rules,
rebuilds the bridges, re-applies branding and the GPU sudoers rule.

Rollback: `git checkout <previous tag>` and the same install command.

## Changing configuration

| Change | Where | Then |
|---|---|---|
| Secrets, port, vessel id, SMTP, plugin pin | `/etc/usv/usv.env` | `sudo bash deploy/install.sh` |
| Radar targets on/off, radar id (MAYARA_URL, MAYARA_RADAR) | `/etc/usv/usv.env` | `sudo bash deploy/install.sh` |
| Camera status on/off (FRIGATE_URL), charted battery (SIGNALK_BATTERY) | `/etc/usv/usv.env` | `sudo bash deploy/install.sh` |
| Which cameras count for Readiness | Frigate: enable / disable the camera (Configuration Editor, Save & Restart) | nothing; picked up within 60 s |
| Bridge options, readiness profile, vessel constants, ENC / checklist flags | `real.moos` grafana_bridge block | restart the community |
| Alert recipients | `grafana/provisioning/alerting/contact-points.yml` in the repo | commit, tag, install |
| Alert thresholds | `tools/gen_alerts.py --profile vessel` on a workstation | commit, tag, install |
| Dashboards | `tools/gen_dashboards.py` on a workstation (never edit the JSON) | commit, tag, install |
| Telegraf probes (Starlink, GPU) | `telegraf/telegraf.conf` in the repo | commit, tag, install |

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
| GPS row "INS aligning" / "GNSS lost" | VectorNav state, not the dashboard: wait for alignment / check antenna |
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

## Known gaps on this hull (as of v1.2.12)

- Own-ship navigation in MOOS: iDivisorNMEA posts NAV_LAT/NAV_LONG/
  NAV_HEADING = 0 because its UDP feed (port 10111) carries only AIS; the
  helm and pContactMgrV20 therefore see own ship at 0N 0E. Fix with the
  mission developer (valid-RMC/HDT publishing, any talker) plus RMC and
  HDT enabled in Signal K's NMEA0183 output. No autonomous missions until
  `uXMS --server_port=9000 NAV_LAT` reads about 29.73. The dashboard uses
  Signal K meanwhile.
- Rudder feedback: no `$AGRSA` on the same feed, so Steering reads FAIL.
- Cameras: cameras 3 to 6 and the FLIR are not powered / networked yet;
  keep them disabled in Frigate until they are, so Readiness counts only
  live cameras. Bitrate, temperature and PTZ have no source.
- State of charge and charging mode: the VE.Direct shunt publishes
  voltage and current only.
- ENC: none fitted; `enc_fitted = false`.
- Console checklist: not published by the UI; `console_checklist = false`.
- VPN tile: N/A (tunnel on the router).
- Helm position: `uQueryDB real.moos --condition="NAV_X > -999999"` to
  confirm pHelmIvP receives NAV_X/NAV_Y on this hull (autonomy stack, not
  the dashboard).
- Operating region: confirm `BHV_OpRegion` with a polygon in the `.bhv`;
  the geofence alert and the Course planning row depend on it.
- Mission autostart: pAntler is launched by hand; the reboot test
  (`sudo reboot`, everything returns) is pending.
