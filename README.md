<p align="center">
  <img src="deploy/branding/swiftships_wordmark.png" alt="Swiftships" width="420"/>
</p>

# Swift Autonomous Dashboard

Grafana + InfluxDB monitoring stack for a MOOS-IvP / Signal K / NMEA 2000
unmanned surface vehicle. Implements SRS FR-1..FR-11, integrated with the
**Alpha mission** (`alpha.bhv`): the bridge subscribes to the mission's real
variable set (mode flags, comms-loss failsafe + explicit re-arm, entry
gate, distance-governed approach, geofence, obstacle alerts, helm health)
and derives the operational mission state onboard — see
`docs/alpha-mission-integration.md`.

![Mission dashboard (dark theme)](docs/images/dashboard-dark.png)

![Analog instrument cluster — Vessel Health](docs/images/instruments-dark.png)

<table>
  <tr>
    <td><img src="docs/images/dashboard-light.png" alt="Light theme"/></td>
    <td><img src="docs/images/dashboard-tablet.png" alt="Tablet layout"/></td>
  </tr>
  <tr>
    <td align="center">Light theme (one-click ☀/☾ toggle)</td>
    <td align="center">Tablet layout (responsive grid)</td>
  </tr>
</table>

**Deployment model: fully native, no Docker.** InfluxDB, Grafana and
Telegraf run as apt-installed systemd services on the onboard computer.
All vessel-side telemetry code is C++: `grafana_bridge` runs under pAntler
as a regular MOOS community app, `can_bridge` and `signalk_bridge` as
systemd services.

```
deploy/
  install.sh                 # one-shot native installer (Debian/Ubuntu)
  usv.env.example            # -> /etc/usv/usv.env (secrets, vessel id)
  systemd/                   # service units (signalk, can, simulator)
influxdb/init/               # bucket/downsample reference (installer does this)
telegraf/telegraf.conf       # OBC metrics, ping, systemd unit supervision
ingestion/                   # all C++
  cpp-common/                #   shared Influx line-protocol writer
  moos-cpp/                  #   grafana_bridge (CMOOSApp) + example.moos
                             #   + mission_logic.hpp (Alpha state machine,
                             #     unit-tested) + moos_poke utility
  can-cpp/                   #   SocketCAN / NMEA 2000 bridge
  signalk-cpp/               #   Signal K WebSocket delta bridge
simulator/                   # C++ bench generator (scripted Alpha mission)
grafana/
  provisioning/              # datasource, dashboards provider, alerting
  dashboards/*.json          # 11 generated dashboards
tools/                       # dev-side generators (Python, never on vessel)
  gen_dashboards.py          #   regenerate dashboard JSON
  gen_alerts.py              #   regenerate alert rules
  render_preview.py          #   render dashboard preview images
docs/                        # schema, deployment, user guide, traceability
```

Install on the onboard computer:
```bash
sudo deploy/install.sh          # first run creates /etc/usv/usv.env
sudo nano /etc/usv/usv.env      # set secrets
sudo deploy/install.sh          # installs everything
```
Then add `Run = grafana_bridge` to your mission file
(`ingestion/moos-cpp/example.moos`). Grafana: http://\<obc\>:3001,
folder "Swift Autonomous Dashboard".

Bench test without the vessel: `sudo systemctl start usv-simulator`.

See `docs/deployment.md` for details and `docs/traceability.md` for
requirement coverage.
