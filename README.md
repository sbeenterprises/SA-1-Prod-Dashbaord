<p align="center">
  <img src="deploy/branding/swiftships_wordmark.png" alt="Swiftships" width="420"/>
</p>

# Swift Autonomous Dashboard

Grafana + InfluxDB monitoring stack for the Swiftships MOOS-IvP / Signal K
unmanned surface vehicle. Fourteen single-screen operator dashboards
(Readiness, Mission, Navigation, Autopilot, Vessel Health, Engine, CAN Bus,
Traffic, Cameras, Comms, System, Replay, Alarms, Fleet) plus the Autopilot
Live drill-down, 24 provisioned alert rules and a
Swiftships-branded Grafana, fed by native C++ bridges from the MOOS
community, the Signal K server and the NMEA 2000 bus. Implements SRS
FR-1..FR-12 and is integrated with the **Alpha mission** (`alpha.bhv`): the
bridge subscribes to the mission's real variable set and derives the
operational mission state onboard (`docs/alpha-mission-integration.md`).

![Mission dashboard (dark theme)](docs/images/dashboard-dark.png)

![Analog instrument cluster — Vessel Health](docs/images/instruments-dark.png)

<table>
  <tr>
    <td><img src="docs/images/dashboard-light.png" alt="Light theme"/></td>
    <td><img src="docs/images/dashboard-tablet.png" alt="Tablet layout"/></td>
  </tr>
  <tr>
    <td align="center">Light theme (Theme picker: Dark, Navy or Light)</td>
    <td align="center">Tablet layout (responsive grid)</td>
  </tr>
</table>

## Design rules

- Nautical units only: speeds in knots, long distances in nautical miles,
  short ranges in fixed metres. Nothing on any dashboard ever scales to km.
- One screen per dashboard on a 1080p display: rows of equal 3x3 status
  tiles above instruments and trends, no scrolling during operations. The
  generator enforces it (`fit_to_screen`, `SCREEN_ROWS = 19` in
  `tools/gen_dashboards.py`).
- Every dashboard carries the Swiftships header band with the dashboard
  name, the hull's class name (`VESSEL_CLASS`), the selected vessel and its
  engine, and works in the three themes of the Theme picker (Dark, Navy,
  Light).
- Per-hull facts live in `/etc/usv/usv.env`, never in the dashboards: the
  engine profile (`ENGINE_*`, `HELM_PROFILE`) and the particulars from the
  general arrangement drawing (`VESSEL_*`). One release serves every hull.
- Fleet-ready: every measurement is tagged `vessel`; the Fleet dashboard
  repeats a status row per vessel and maps last known positions.

## Deployment model

**Fully native, no Docker.** InfluxDB, Grafana and Telegraf run as
apt-installed systemd services on the onboard computer. All vessel-side
telemetry code is C++: `grafana_bridge` runs under pAntler as a regular MOOS
community app, `can_bridge` and `signalk_bridge` as systemd services (one
`usv-signalk-bridge@<vessel>` instance per Signal K server when several
vessels are simulated on one host). Two optional systemd services feed
vessel sensors when configured in `/etc/usv/usv.env`: `mayara_bridge`
(radar ARPA targets from mayara-server, `MAYARA_URL`) and `frigate_bridge`
(camera status from the Frigate NVR, `FRIGATE_URL`).

```
deploy/
  install.sh                 # one-shot native installer (Debian/Ubuntu), idempotent
  update.sh                  # update an installed vessel to a release tag (one command)
  brand.sh                   # Swiftships branding for Grafana OSS (called by install.sh)
  branding/                  # logos, favicon, wordmark chips
  usv.env.example            # -> /etc/usv/usv.env (secrets, vessel id, fleet, branding)
  systemd/                   # service units (signalk, signalk@vessel, can, mayara,
                             #   frigate, simulator)
telegraf/telegraf.conf       # OBC metrics, GPU (Intel via usv_gpu_probe / NVIDIA), ping, systemd units
ingestion/                   # all C++
  cpp-common/                #   async Influx line-protocol writer, HTTP GET helper,
                             #   latest-value poller (readiness inputs from other producers),
                             #   engine_profile.hpp / vessel_profile.hpp (usv.env per hull),
                             #   j1939_faults.hpp (Mercury fault table); unit-tested
  moos-cpp/                  #   grafana_bridge (CMOOSApp) + example.moos
                             #   + mission_logic.hpp (Alpha state machine, traffic
                             #     geometry, pre-departure readiness; unit-tested)
                             #   + mayara_bridge (radar targets) + moos_poke utility
  can-cpp/                   #   SocketCAN / NMEA 2000 / J1939 bridge
                             #   + usv722_can.hpp, usv725_can.hpp (helm, steering,
                             #     thrust, gear, key, DM1 frames per hull; unit-tested)
                             #   + j1939_engine.hpp (engine ECU, hours request)
  signalk-cpp/               #   Signal K WebSocket delta bridge
  frigate-cpp/               #   camera status from the Frigate NVR
simulator/                   # C++ bench generator (scripted Alpha mission)
grafana/
  provisioning/              # datasource, dashboards provider, alerting
  dashboards/*.json          # 15 generated dashboards (never edit by hand)
tools/                       # dev-side generators (Python, never on the vessel)
  gen_dashboards.py          #   regenerate dashboard JSON
  gen_alerts.py              #   regenerate alert rules (vessel; --profile sim on sim hosts)
  render_preview.py          #   render dashboard preview pages (dark/light)
  smoke_test.sh              #   build + unit tests + end-to-end line-protocol checks
  fixtures/                  #   recorded CAN traffic of both hulls (replayed by the smoke test)
  traffic_scenario.sh        #   scripted single-contact situations (sim host)
  fake_traffic.py            #   fake AIS / radar scene, many contacts at once (sim host)
docs/                        # schema, deployment, user guide, traceability, validation
```

## Install on the onboard computer

```bash
sudo bash deploy/install.sh     # first run creates /etc/usv/usv.env and exits
sudo nano /etc/usv/usv.env      # secrets, GRAFANA_PORT, VESSEL_ID, engine profile,
                                # VESSEL_* particulars (both hulls' values are in
                                # deploy/usv.env.example), branding text
sudo bash deploy/install.sh     # installs, provisions and starts everything
```

## Update an installed vessel

From the repo checkout on the onboard computer, as the normal user:

```bash
bash deploy/update.sh            # latest release tag
bash deploy/update.sh v1.2.51    # or a given tag; prints the rollback command
```

It refuses to run over local edits, re-runs the installer (rebuilds and
restarts the bridges, re-provisions dashboards and alerts, reloads
Telegraf) and lists service status. Then Ctrl+F5 in the browser, and
restart the MOOS mission if `grafana_bridge` changed. Production vessels
install tags only (`docs/runbook-usv-722.md`).

## First install: MOOS

Merge `ingestion/moos-cpp/example.moos` into the mission file
(`Run = grafana_bridge` plus its ProcessConfig). Grafana listens on
`GRAFANA_PORT` (3001 by default; the simulation host uses 3300), folder
"Swift Autonomous Dashboard". Bench test without the vessel:
`sudo systemctl start usv-simulator`.

## Development workflow

```bash
python3 tools/gen_dashboards.py          # after editing the generator
python3 tools/gen_alerts.py              # vessel profile (what the boats run)
sudo bash tools/smoke_test.sh            # must end with FAIL: 0
```

Release: add the changes under a new version in `CHANGELOG.md`, commit,
then tag on `main` (`git tag -a vX.Y.Z -m "..." && git push origin vX.Y.Z`)
so vessels can `deploy/update.sh` to it.

Run the smoke test from a tree without stale `build/` directories. As root
it also checks `deploy/brand.sh`; with Go installed it parses the branded
Grafana page the way Grafana does; with `ws`, `echarts` and
`@influxdata/flux-lsp-node` on `NODE_PATH` it runs the Signal K, instrument
and Flux checks. Anything it cannot run is reported as SKIP, so read the
SKIP lines before tagging.

The smoke test builds every C++ program, runs the
`mission_logic_test`, `mayara_logic_test`, `frigate_logic_test`,
`usv722_can_test`, `j1939_engine_test`, `engine_profile_test`,
`vessel_profile_test`, `j1939_faults_test` and
`signalk_engine_test` unit tests, checks that the committed dashboards and
alert rules equal the generators' output, runs `deploy/brand.sh` on a
stand-in Grafana page and checks it still parses as Grafana's template,
replays the recorded CAN traffic of both hulls, drives the simulator, the CAN bridge, the Signal K bridge (every
engine path in both id forms, SK-1 to SK-3 of docs/requirements-usv-725.md)
and (when MOOSDB is on the PATH) the MOOS bridge against an InfluxDB stub,
and cross-checks every field the dashboards query against what the
producers actually write. It also runs every
ECharts instrument in a mocked Grafana context (`tools/check_echarts_panels.js`),
type-checks every Flux query when `@influxdata/flux-lsp-node` is installed
(`tools/check_flux.js`), and checks that map markers use Grafana's built-in shapes and the
speed vector wiring. All vessel-side logic
(mission state, traffic geometry and risk, pre-departure readiness) is
C++ in the bridge; Grafana only displays it. Work on feature branches, keep commits
conventional (`dashboards: ...`, `bridge: ...`, `deploy: ...`) and merge
to `main` through a pull request.

See `docs/deployment.md` for operations, `docs/user-guide.md` for
operators, `docs/schema.md` for the InfluxDB schema,
`docs/traceability.md` for requirement coverage,
`docs/validation-report.md` for what was verified and how, and
`CHANGELOG.md` for release history (current release: v1.2.51, for usv-725
and usv-722; do not install v1.2.33; simulation baseline v1.0.0),
`docs/release-1.2.20-usv-725.md` for the first installation on usv-725
(engine profile, vessel particulars),
`docs/requirements-usv-725.md` for the Mercury diesel hull requirements,
`docs/commissioning-usv-722.md` for the commissioning record and
`docs/runbook-usv-722.md` for day-to-day operation of the production
vessel.
