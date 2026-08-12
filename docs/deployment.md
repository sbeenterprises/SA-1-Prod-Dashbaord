# Deployment & Maintenance Guide (native, no Docker)

Everything runs directly on the onboard computer: InfluxDB, Grafana and
Telegraf as apt-installed systemd services, `grafana_bridge` as a MOOS
community app under pAntler, `can_bridge` and the Signal K bridge as
systemd services.

## Prerequisites
- Debian/Ubuntu onboard computer (amd64 or arm64) with internet access for
  the one-time install (apt repos of InfluxData and Grafana).
- MOOS-IvP built on the machine (for linking `moos_bridge`).
- Signal K server running (default port 3001).
- SocketCAN interface up for NMEA 2000:
  `ip link set can0 up type can bitrate 250000` (persist via systemd-networkd
  or /etc/network/interfaces).

## Install
```bash
sudo deploy/install.sh          # first run creates /etc/usv/usv.env and exits
sudo nano /etc/usv/usv.env      # set ALL secrets
sudo deploy/install.sh          # installs and starts everything
# If MOOS isn't found automatically:
sudo MOOS_DIR=~/moos-ivp/build/MOOS/MOOSCore deploy/install.sh
```
The installer is idempotent — re-run it after changing configs or updating
dashboards.

What it does: adds InfluxData + Grafana apt repos; installs influxdb2,
telegraf, grafana; runs `influx setup` and creates the `events` and
`telemetry_1m` buckets plus the 1-minute downsample task; builds
IXWebSocket once from source, then builds and installs the C++ programs
`can_bridge`, `signalk_bridge`, `usv_simulator` and `moos_bridge` to
/usr/local/bin; deploys systemd units under a `usv` service user;
provisions the Grafana datasource, dashboards and alert rules; enables
everything at boot.

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
}
```
`grafana_bridge` starts, restarts and stops with the rest of the community
under pAntler. Align the variable maps in
`ingestion/moos-cpp/moos_bridge.cpp` (kNumericVars / kStringVars) with what
your helm publishes, then rebuild:
`cmake --build ingestion/moos-cpp/build -j && sudo cmake --install ingestion/moos-cpp/build`.

## Services
| Unit | Role |
|---|---|
| influxdb.service | time-series database (localhost:8086) |
| grafana-server.service | dashboards + alerting (port 3000) |
| telegraf.service | OBC metrics, ping, service supervision |
| usv-signalk-bridge.service | Signal K deltas -> Influx |
| usv-can-bridge.service | CAN/N2K frames -> Influx (CAP_NET_RAW) |
| usv-simulator.service | bench-test data (installed, NOT enabled) |
| grafana_bridge (pAntler) | MOOS -> Influx, managed by your mission |

`journalctl -u usv-can-bridge -f` etc. for logs.

## Bench testing without the vessel
```bash
sudo systemctl start usv-simulator     # full fake telemetry stream
sudo systemctl stop usv-simulator      # stop before real operations
```

## Configuration points
| What | Where |
|---|---|
| Secrets, vessel id, retention | /etc/usv/usv.env (re-run install.sh after edits) |
| MOOS variable map | ingestion/moos-cpp/moos_bridge.cpp |
| Signal K path map | ingestion/signalk-cpp/signalk_bridge.cpp kPaths |
| PGN decoders | ingestion/can-cpp/main.cpp decode_pgn() |
| Alert thresholds | tools/gen_alerts.py -> regenerate -> re-run install.sh |
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
| No MOOS data | grafana_bridge in pAntler list? `umm --spy` shows the vars? token/org in ProcessConfig correct? |
| No CAN data | `candump can0`; `journalctl -u usv-can-bridge -f` |
| Signal K panel red | Signal K on :3001? URL in /etc/usv/usv.env correct? |
| Alerts not firing | Grafana -> Alerting -> Rules; datasource health |
