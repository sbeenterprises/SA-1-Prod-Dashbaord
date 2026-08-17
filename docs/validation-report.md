# Validation Report: Alpha Mission Integration

Date: 2026-08-17. Scope: integration of the Alpha mission (alpha.bhv) with
the Swift Autonomous Dashboard: grafana_bridge rewrite, derived mission
state machine, new autonomy schema, dashboard and alert regeneration,
simulator scenario, docs. This report records what was validated, how, and
what remains to verify on the vessel.

## 1. Environment

Debian-based build host; g++ 13, CMake 3.28, libcurl 8.5,
nlohmann-json 3.11, IXWebSocket (built from source, as the installer does),
core-MOOS v10 built from source (github.com/themoos/core-moos) providing a
real MOOSDB and umm for the integration test. Grafana/InfluxDB themselves
were not installed here; dashboard and alert provisioning files were
validated structurally (JSON/YAML load, unique panel ids and rule uids) and
cross-checked against the data producers (section 4).

## 2. Unit tests (mission_logic_test)

79 assertions, all passing. Coverage follows the semantics documented in
alpha.bhv:

- Flag parsing: "true"/"TRUE"/"1", COMMS_LOST action strings vs "false",
  SURVEY_ENTRY_GATE open/closed defaults.
- All-stop classification: clear / ManualOverride / NothingToDo benign;
  MissingDecVars:course and friends unexpected (alert-worthy).
- State machine walk-through: Idle, survey start, pause, mid-route
  station-keep, comms loss dominating concurrent STATIONING, link restored
  WITHOUT silent resume (AwaitingRearm), explicit re-arm, return-to-base,
  pause gating the return, park at base, survey completion (parked and
  still-recovering variants), start-outside entry gate (EnteringRegion,
  then latch open), CommsLost dominating every other flag.
- IVPHELM_SUMMARY active-behavior extraction (multi-behavior, none, single,
  malformed input), VIEW_SEGLIST point counting and labels, obstacle id
  parsing, geofence decay latch, obstacle TTL tracker, odometry integrator
  with GPS jump guard and reset, progress clamp on repeat routes, ETA
  speed floor, key=value continuation parsing.

Run anywhere with:
`cmake -S ingestion/moos-cpp -B build -DMISSION_LOGIC_TESTS_ONLY=ON && cmake --build build && ctest --test-dir build`

## 3. Build validation

moos_bridge, moos_poke and mission_logic_test compile clean (-Wall -Wextra,
zero warnings) and link against real core-MOOS v10. can_bridge,
signalk_bridge and usv_simulator build as before. ctest passes.

## 4. End-to-end smoke test (tools/smoke_test.sh)

Result: 40 PASS, 0 FAIL, 1 SKIP. The two structural cross-checks are the
important production guards:

- Every (measurement, field) pair queried by any of the 11 dashboards is
  actually produced by the stack (101 queried pairs vs 103 produced;
  Telegraf-sourced host metrics excluded by design).
- All 15 alert rules query fields the stack actually writes, so no rule
  can be permanently silent the way the old geofence rule was.

The MOOS section runs a real MOOSDB, launches the bridge from a mission
file, and replays the Alpha variable sequence with moos_poke: survey start
with seglist and helm summary, comms loss (COMMS_LOST=loiter +
MISSION_ARMED=false + STATIONING=true), link restore without re-arm,
operator re-arm, geofence breach, obstacle alert. All 20 assertions on the
resulting InfluxDB line protocol passed, including the derived state
sequence Autonomous -> CommsLost -> AwaitingRearm, waypoints_total=5 from
VIEW_SEGLIST, active-behavior parsing from IVPHELM_SUMMARY, and the event
stream (state changes, geofence, obstacle, failsafe, re-arm).

The single SKIP is the live SocketCAN capture (the vcan kernel module is
unavailable in the build sandbox); the CAN bridge failure-path tests
(missing interface: retries, logs errno, publishes interface_up=0) all ran
and passed. Re-verify live CAN during commissioning step 12 as usual.

Full log:

```
=== Build ===
  [PASS] build ingestion/moos-cpp
  [PASS] build ingestion/can-cpp
  [PASS] build ingestion/signalk-cpp
  [PASS] build simulator
=== Mission-logic unit tests (Alpha state machine) ===
  [PASS] mission_logic_test: 79 passed, 0 failed
=== Simulator -> InfluxDB line protocol ===
  [PASS] simulator wrote 608 lines
  [PASS] VESSEL_ID tag applied
  (103 distinct measurement/field pairs)
=== Dashboard queries vs simulator output ===
  dashboard pairs: 101 | simulator emits: 103 | telegraf-sourced: skipped
  [PASS] every non-Telegraf field queried by a dashboard is produced
=== Alert rule queries vs simulator output ===
  [PASS] all 15 alert rules query fields the stack actually writes
=== Signal K bridge (mock WebSocket server) ===
  [PASS] SOG written
  [PASS] position decoded
  [PASS] engine RPM decoded
  [PASS] link-up heartbeat
  [PASS] SOG m/s->kn: 4.8596
  [PASS] RPM Hz->rpm: 2520.0
  [PASS] coolant K->C: 85.0
=== MOOS bridge (real MOOSDB) ===
Terminated
  [PASS] mission-file vessel_id applied
  [PASS] scheme-less influx_url expanded
  [PASS] NAV_SPEED -> nav.sog
  [PASS] NAV_HEADING -> nav.heading
  [PASS] DESIRED_RUDDER -> autopilot
  [PASS] MISSION_PROGRESS -> mission
  [PASS] MOOS connect heartbeat
  [PASS] WAYPOINT=true -> state Autonomous
  [PASS] COMMS_LOST -> state CommsLost
  [PASS] link back w/o re-arm -> AwaitingRearm
  [PASS] DIST_TO_NEXT_WPT -> mission.dist_to_waypoint
  [PASS] VIEW_SEGLIST -> waypoints_total=5
  [PASS] autonomy.comms_lost flag written
  [PASS] autonomy.awaiting_rearm hold visible
  [PASS] OPR_BREACHED -> geofence_breached=1
  [PASS] IVPHELM_SUMMARY -> active behaviors
  [PASS] geofence breach event emitted
  [PASS] obstacle alert event emitted
  [PASS] comms-loss failsafe event emitted
  [PASS] re-arm event emitted
=== CAN bridge (virtual CAN) ===
  [SKIP] CAN bridge live capture (vcan module unavailable in this environment)
  [PASS] CAN bridge survives missing interface (retries, no exit)
  [PASS] CAN failure logged with errno reason
  [PASS] canbus.interface_up=0 published while down
=== Resilience: InfluxDB down ===
  [PASS] simulator survives unreachable InfluxDB (no crash)
=== Summary ===
  PASS: 40   FAIL: 0   SKIP: 1
  RESULT: SMOKE TEST PASSED
```

## 5. Dashboard rendering

Dashboard JSON regenerated from tools/gen_dashboards.py: 11 dashboards,
unique panel ids, valid JSON, provisioning-compatible. Preview renders of
the reworked Mission dashboard (dark, light, tablet) were regenerated and
committed to docs/images/, showing the new status row (state, helm, armed,
operator link, geofence, obstacles), the approach profile with the
governor-tier staircase, the autonomy flags strip, and the full Alpha state
timeline. Grafana-version note from DEPLOYMENT-NOTE.md section 8.6 still
applies: confirm the 15 rules appear in your installed Grafana during
commissioning.

## 6. To verify on the vessel (not coverable off-target)

1. grafana_bridge under pAntler against the production MOOSDB with the
   live alpha.bhv mission (commissioning step 10); confirm state
   transitions in the Mission dashboard during a dockside DEPLOY/PAUSE/
   station-keep drill, and a controlled comms-loss drill for the failsafe
   and re-arm flow.
2. OPR_BREACHED posting cadence of BHV_OpRegionV24 in your build: the
   30 s decay window assumed for geofence self-clear is tunable in
   moos_bridge.cpp (DecayLatch construction) if your behavior re-posts
   differently.
3. planchaPID PID_* variable names if PID trend panels are wanted (the
   desired/actual pairs work without them).
4. Live CAN capture (vcan skip above) and the radar/camera adapters,
   unchanged from the base delivery.

## 7. Addendum: analog instrument and tile update

A second pass restyled the dashboards (analog instrument cluster + status
tiles) after the initial validation above. Revalidated end to end on the
same rig; results unchanged: 40 PASS, 0 FAIL, 1 SKIP, and the two
structural cross-checks still hold with the new panels included (every
field queried by the 11 instrument panels is produced by the stack; all
15 alert rules remain backed by live fields).

What changed and how it was validated:

- 11 analog instrument panels (compass with HDG/COG needles, SOG meter,
  rudder angle indicator, throttle, heading-error meter, tachometer,
  coolant, oil pressure, fuel, battery voltage, SOC) generated as
  volkovlabs-echarts-panel panels. JSON validated; each carries its Flux
  target, so the smoke test's dashboard-vs-producer check covers them.
- Instrument band colors are the same semantic thresholds as the alert
  rules (e.g. fuel red below 15, battery red below 11.8 V), so a needle
  in a red band and a firing alert can never disagree.
- Status tiles switched to background color mode (Grafana native); the
  inactive states use explicit neutral hexes so nothing washes out in
  either theme.
- Classic mode verified: `gen_dashboards.py --classic` emits zero
  plugin-dependent panels (native gauges take the instrument slots) for
  plugin-free or offline installs.
- The plugin itself (signed, React-based Business Charts) is installed by
  deploy/install.sh; on-vessel rendering of the ECharts option code is a
  commissioning check (browse Vessel Health after step 4) since Grafana
  was not installed on this rig.
- Previews of Mission, Navigation, Autopilot and Vessel Health were
  re-rendered (dark, light, tablet) from the generated JSON and committed
  under docs/images/.
- Final theme is matched to the vessel control UI's ENGINE STATUS
  widget (colors measured from the provided screenshot): cyan #00bcd4
  accents and value text, white instrument values, azure #42a5f5
  secondary, deep azure-navy #175d8d status tiles, navy #1c2a3d gauge
  rails, with glossy gradients and needle shadows. Yellow/orange/red
  remain strictly semantic. Full smoke test re-run after each restyle:
  40 PASS, 0 FAIL, 1 SKIP.

Note: the Signal K Freeboard UI archive (Swiftships-UI_.tar.gz) had not
arrived in the workspace by delivery time, so the UI cross-check remains
based on the alpha.bhv iWebSocket contract and the stack's existing
Signal K path map. If UI inspection surfaces different variable names or
theme hexes, the touch points are kNumericVars/kStringPassthrough in
moos_bridge.cpp and PALETTE in tools/gen_dashboards.py.
