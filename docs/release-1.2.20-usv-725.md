# Release v1.2.20 and the first installation on USV-725

One page for the two people involved: whoever cuts the release on the
workstation (part A) and whoever is on the boat (parts B to E). Every
command is complete; nothing has to be looked up elsewhere. Release
content: per-vessel engine profile, Engine page, per-engine-type dial
scales, restyled instruments, Signal K engine paths for the Yamaha
gateway, grafana_bridge mapping for the Mercury diesel, Engine Low Oil
Pressure and Engine Fault alerts.

Order of operations: USV-725 first (fresh install, part B), acceptance
there (part D), then USV-722 (part E). On USV-722 the engine dials keep
reading N/A until the Yamaha gateway is fitted; every other page is
unchanged.

## A. Cut the release (workstation, git bash)

The branch `usv-725-engine` is linear on top of `v1.2.19`, so the merge is
a fast-forward. The tag is annotated and authored like the commits.

```bash
cd /c/Users/ICSADMIN/source/SA-1-Dashboard-prod
git fetch origin
git checkout main
git merge --ff-only usv-725-engine
git -c user.name="mali" -c user.email="mali@sunraysint.com" tag -a v1.2.20 -m "v1.2.20: engine profile, Engine page, USV-725 (Mercury diesel) support"
git push origin main
git push origin v1.2.20
git describe --tags          # prints v1.2.20
```

If `merge --ff-only` refuses, `main` has a commit the branch lacks: stop
and compare (`git log --oneline main..usv-725-engine` and the reverse)
before doing anything else.

## B. First installation on USV-725

### B.1 What must already be on the onboard computer

- Ubuntu 22.04, internet access for apt and GitHub, `sudo`.
- MOOS-IvP built, with the mission that runs the boat (pAntler launches
  iCAN, iCANRec and the rest) and its mission file known. Below it is
  called `MISSION.moos`; substitute the real path.
- `slcan0` up at 250 kbit/s at boot (the J1939 engine bus; the engine's
  own address is 0x00, the vessel controller sends from 0x17).
- Signal K on port 3000 with the Victron shunt publishing
  `electrical.batteries.<id>.*` (the house battery; note the `<id>`).
- Nothing listening on the Grafana port chosen below (3001 by default;
  usv-722 uses 3100).

### B.2 Clone the release

```bash
cd ~/Documents
git clone https://github.com/sbeenterprises/SA-1-Prod-Dashbaord.git
cd SA-1-Prod-Dashbaord
git checkout v1.2.20
git describe --tags          # v1.2.20
```

### B.3 First installer run: creates the configuration file and stops

```bash
sudo bash deploy/install.sh
# "Created /etc/usv/usv.env — edit secrets now, then re-run install."
```

### B.4 Edit /etc/usv/usv.env

`sudo nano /etc/usv/usv.env`. Set every line below; leave the rest at
the shipped defaults.

```
INFLUX_PASSWORD=<strong password>
INFLUX_TOKEN=<long random token>        # openssl rand -hex 32
GRAFANA_ADMIN_PASSWORD=<strong password>
GRAFANA_PORT=3001                       # or 3100 to match usv-722
VESSEL_ID=usv-725
ENGINE_TYPE=mercury_diesel
ENGINE_MODEL="Mercury Diesel 2.0L"
NAV_SPEED_UNITS=knots
ENGINE_SOURCE=can
HELM_PROFILE=usv725
CAN_INTERFACE=slcan0
SIGNALK_URL=ws://localhost:3000/signalk/v1/stream?subscribe=self
SIGNALK_BATTERY=<the Victron battery id from B.1, e.g. House>
```

What the engine lines do: `ENGINE_SOURCE=can` makes can_bridge the only
writer of the engine fields: it reads the Mercury J1939 engine ECU
(address 0) on slcan0 directly, and the fuel level from the NMEA 2000 tank
sender; signalk_bridge ignores `propulsion.*` and grafana_bridge ignores
the MOOS `ENGINE_*` variables, so no MOOS application is involved in the
engine data. `ENGINE_TYPE=mercury_diesel` tags every engine point, scales
the tachometer to 0 to 5000 rpm with the green band to 4000 and the oil
pressure dial to 800 kPa, and names faults with the Mercury J codes.
`HELM_PROFILE=usv725` decodes this hull's helm and drive frames: rudder
(wheel and actuator, 17 degrees at full lock), lever thrust and gear, trim
switch, key panel and the engine speed request. It must be usv725 on this
hull: the usv722 decoder reads the rudder on the wrong scale and misses
the trim, and can_bridge logs a warning if the profile does not match the
bus (`journalctl -u usv-can-bridge`). Engine hours: the ECU sends them only
when asked; add `CAN_ENGINE_HOURS_REQUEST=1` to let can_bridge ask every
5 s (the only frame the dashboard transmits).

Keep `usv.env` root-only (`chmod 600`, the installer sets it) and never
commit it.

### B.5 Second installer run: installs and starts everything

```bash
sudo bash deploy/install.sh
# if it ends with "WARNING: MOOS not found":
sudo MOOS_DIR=<path containing MOOSConfig.cmake, e.g. ~/moos-ivp/build/MOOS/MOOSCore> bash deploy/install.sh
```

The installer prints the resolved profile (`Engine profile: source=moos
type=mercury_diesel count=1 helm=none`) and, at the end, the
grafana_bridge lines for the mission file. It installs InfluxDB, Telegraf,
Grafana and the ECharts plugin, builds and installs `can_bridge`,
`signalk_bridge`, `moos_bridge` (as `/usr/local/bin/grafana_bridge`) and
`moos_poke`, provisions the 15 dashboards and 22 alert rules, and starts
the systemd units. It is idempotent: re-run it after any change to
`usv.env`.

### B.6 Add grafana_bridge to the mission file

Back up first: `cp MISSION.moos MISSION.moos.bak-$(date +%F)`.

1. In `ProcessConfig = ANTLER`, after the last `Run =` line:

```
  Run = grafana_bridge  @ NewConsole = false
```

2. At the end of the file (the token is the `INFLUX_TOKEN` value from
   `usv.env`; `//` starts a MOOS comment, which is why the URL has no
   scheme):

```
//------------------------------------------
// grafana_bridge: MOOS -> InfluxDB (SA-1 Dashboard)
ProcessConfig = grafana_bridge
{
  AppTick   = 10
  CommsTick = 20

  influx_url    = localhost:8086
  influx_tls    = false
  influx_token  = <INFLUX_TOKEN from /etc/usv/usv.env>
  influx_org    = usv
  influx_bucket = telemetry
  events_bucket = events
  vessel_id     = usv-725

  mission_name   = <the Community name of this mission>
  max_rate_hz    = 5
  engine_rate_hz = 2
  radar_cpa_alert_m = 500
  tcpa_alert_s   = 300
  contact_ttl_s  = 10
  signalk_http   = localhost:3000

  readiness_profile  = vessel
  readiness_period_s = 5
  optional_apps = pMarineViewer,pContactSpawn
  enc_fitted = false          // no ENC chart system: row reads N/A
  console_checklist = false   // UI keeps its checklist local
}
```

`vessel_id` must equal `VESSEL_ID` in usv.env. The engine profile is not
repeated here: grafana_bridge reads `ENGINE_SOURCE` and `ENGINE_TYPE` from
`/etc/usv/usv.env` at start. Add `ignore_vars = NAV_YAW` only if this
hull's `iDivisorNMEA` also publishes the rudder angle as NAV_YAW (check
with `uQueryDB MISSION.moos --condition="NAV_YAW > -999"` while the helm
is turned: if the value follows the rudder, add the line).

3. Restart the mission so pAntler launches grafana_bridge (stop it the way
   it is normally stopped, then `pAntler MISSION.moos`, or
   `sudo systemctl restart moos-antler` if a unit exists). If pAntler
   reports `Failed exec` for grafana_bridge, its PATH does not include
   `/usr/local/bin`: the installer prints the fix for the systemd unit
   case.

## C. Check that data flows (10 minutes, engine running)

```bash
systemctl --no-pager status influxdb grafana-server telegraf usv-signalk-bridge usv-can-bridge | grep -E "●|Active"
journalctl -u usv-signalk-bridge -n 3 --no-pager      # "engine paths ignored on this vessel" is expected here
journalctl -u usv-can-bridge -n 3 --no-pager          # slcan0 up, frames counted
candump slcan0 | head                                 # J1939 frames (ids 0CF004xx, 18FEEExx ...)
uQueryDB MISSION.moos --condition="ENGINE_SPEED > 0"  # iCANRec publishing
source /etc/usv/usv.env
influx query --org usv --token "$INFLUX_TOKEN" '
from(bucket:"telemetry") |> range(start:-2m)
  |> filter(fn:(r) => r._measurement == "engine" and r.vessel == "usv-725")
  |> last() |> keep(columns:["_field","_value","instance","engine_type"])'
```

Expected: one row per field, `instance=0`, `engine_type=mercury_diesel`,
rpm / coolant_temp / oil_pressure / fuel_rate / percent_load /
intake_manifold_pressure present. Then in a browser,
`http://<obc-ip>:<GRAFANA_PORT>` (admin / the password from usv.env):

- Header band on every page: `Vessel usv-725 · Engine Mercury Diesel 2.0L` (from v1.2.21; v1.2.20 shows `mercury_diesel`).
- Vessel Health and the Engine tab: the tachometer scale ends at 5, the
  oil pressure dial at 800; RPM, coolant, oil pressure and boost move
  with the engine; Fuel, Engine Hours, Fuel Level read N/A (not sent by
  this engine / not requested by iCANRec yet).
- Fault Codes reads NONE (or the J-coded text if a fault is active).
- Readiness: the Engine row PASS with "rpm N · coolant N °C · oil N kPa ·
  no DTC" while the engine runs above 700 rpm.
- Alarms: nothing engine-related firing with the engine healthy.

Known limits of today's iCANRec (fixed by the separate MOOS-repo change,
not by this release): Oil Temp and the ECU voltage show wrong values
(oil temperature sweeps, ECU voltage not 11 to 15 V), Torque reads the
wrong byte, and a fault once seen stays in the count until iCANRec
restarts (so the Engine Fault alert and the Readiness engine row stay red
after a fault until then).

## D. Acceptance on USV-725

Record each result in `docs/commissioning-usv-725.md` (date, who, value
seen, pass / fail) together with the `usv.env` used (secrets removed), the
mission-file block and the capture from A-1.

| # | Test | Pass when |
| --- | --- | --- |
| A-1 | `candump -L slcan0 > ~/usv725-engine-$(date +%F).log` for 10 minutes, idle to 3000 rpm; then `cut -d' ' -f3 ~/usv725-engine-*.log \| cut -c1-8 \| sort \| uniq -c \| sort -rn \| head -30` | PGN list with counts; whether 65253 (hours) and 65276 (fuel level) appear; source addresses seen. Keep the file: it is the test vector set for the iCANRec work and the helm decoder |
| A-2 | The `influx query` from part C, grouped by instance and field | One series per field, instance 0, engine_type mercury_diesel; nothing from signalk_bridge or can_bridge for RPM, coolant, oil, fuel, load or faults |
| A-3 | Dials against the engine's own display at idle, 1500 and 3000 rpm, 5 minutes each, Vessel Health and Engine page | RPM within 1 %, coolant within 2 °C, oil pressure within 10 kPa, fuel rate within 5 %, load within 5 %, boost within 10 kPa. Oil temperature and hours are expected to fail until the iCANRec change (record the values anyway) |
| A-4 | `engine.ecu_voltage` against a meter at the engine battery | Expected to fail until the iCANRec change; record both values |
| A-5 | Gear neutral, forward, neutral, reverse | Gear tile follows the lever (engine running; with the engine off the helm reports neutral) |
| A-6 | Disconnect one sensor the engine monitors, restore it | Active Faults, lamps and Fault Codes within 5 s; the Engine Fault alert fires. Clearing within 5 s of restore will fail with today's iCANRec (record it) |
| A-7 | Two faults at once | Both listed in Fault Codes (BAM reassembly): expected to fail until the iCANRec change |
| A-8 | Engine off | Engine dials and tiles N/A; no engine alert; Readiness engine row FAIL "no engine data" |
| A-9 | `sudo reboot` | All units return (`systemctl status` as in part C); the mission is relaunched the usual way; engine values within 60 s of the engine running |
| A-11 | Bench: `ENGINE_TYPE=yamaha_outboard` in usv.env, `sudo bash deploy/install.sh`, restart the mission, reload the browser; then set it back and repeat | Header chip, tachometer (0 to 6) and oil pressure (0 to 600) scales switch; nothing else changes |
| A-12 | Swiftships UI beside the dashboard at 1500 rpm | RPM, coolant, load and fuel rate agree; oil temperature and battery voltage differ (the UI shows uncorrected values) |

A-10 is the USV-722 regression in part E.

Rollback on USV-725 (should anything block the boat): stop the mission,
remove the `Run = grafana_bridge` line and the ProcessConfig block from
the mission file, restart the mission. The dashboard services can stay
running; they do not touch the helm.

## E. USV-722 afterwards

USV-722 runs v1.2.19 from `~/Documents/SA-1-Prod-Dashbaord`. Add the
engine profile before updating, because the installer validates it:

```bash
sudo nano /etc/usv/usv.env       # add the three lines:
#   ENGINE_TYPE=yamaha_outboard
#   ENGINE_SOURCE=signalk
#   HELM_PROFILE=usv722
cd ~/Documents/SA-1-Prod-Dashbaord
bash deploy/update.sh v1.2.20    # fetch, checkout, install, status
sudo systemctl restart moos-antler   # or relaunch pAntler: grafana_bridge reads the profile at start
```

Then Ctrl+F5 in the browser and check (A-10): the header reads
`Engine yamaha_outboard`; all pages render as before; the Autopilot and
CAN Bus tiles from the Glendinning helm are unchanged (trim switch, gear,
throttle, key, fault lamps and codes from the DM1 on address 210); the
engine dials on Vessel Health and the Engine page read N/A, as they did
before, until the Yamaha Command Link gateway is fitted and Signal K
publishes `propulsion.*`. When that day comes nothing in the dashboard
changes: signalk_bridge already reads every `propulsion.<id>.*` path, the
fuel tank and the engine notifications.

Note for the first dashboard time range after the update on USV-722: the
drive-bus engine fields (gear, throttle, key, trim, faults) gain an
`instance=0` tag, so a tile can show the pre-update and post-update series
side by side until the range no longer spans the update.

## F. After both boats

- Update `docs/runbook-usv-722.md` ("Installed release" row) and write
  `docs/runbook-usv-725.md` from what part B used (ports, paths, the
  mission file, the usv.env keys without secrets).
- Hand the A-1 capture and the Q-3 answer (the iCAN / iCANRec source
  actually running on USV-725) to the iCANRec work; its fixes turn the
  expected failures of A-3, A-4, A-6 and A-7 into passes without another
  dashboard release.
