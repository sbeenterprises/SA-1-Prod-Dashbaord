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

## 8. Addendum: independent re-validation and hardening pass (2026-08-17)

The full stack was re-validated from a clean checkout on a second rig
(Ubuntu 24.04, g++ 13.3, CMake 3.28, core-MOOS v10 and IXWebSocket built
from source exactly as the installer does). Everything reproduced:
79/79 mission-logic assertions, all four C++ builds clean, and the full
smoke test green. Fixes and changes applied in this pass:

1. **Status-tile label regression fixed.** A theming refactor had leaked
   the color constant name into four displayed labels: the Mission
   dashboard's Operator Link tile read "LINK TILE_OK", and the Alarms
   dashboard's CAN Bus / Comms Link / Operator Link tiles read "TILE_OK",
   instead of "OK". Fixed in gen_dashboards.py; dashboards regenerated
   (regeneration confirmed byte-stable for the nine unaffected dashboards,
   which also proves the checked-in JSON matches the generator).
2. **Port alignment to the vessel (operator-confirmed): Signal K owns
   port 3000.** The stack previously assumed Signal K on 3001 and put
   Grafana on 3000, which would collide on the OBC. Signal K defaults now
   use 3000 (signalk_bridge.cpp, usv.env.example, Telegraf net_response
   probe) and Grafana moves to 3001 via a new GRAFANA_PORT setting in
   /etc/usv/usv.env, applied by install.sh (grafana.ini http_port).
   All operator URLs in README/docs updated to :3001.
3. **BHV_WARNING / BHV_ERROR bridged to alarm events.** The helm's
   behavior diagnostics (the MALCONFIG condition-parse class of failure
   that the Alpha mission has actually hit) now land in the events bucket
   as warning/critical alarms, deduplicated on repeated identical posts.
   Covered by a new smoke-test assertion.
4. **Grafana folder naming unified** to "Swift Autonomous Dashboard"
   (README, user guide and installer said 'USV Monitoring' while
   provisioning created 'Swift Autonomous Dashboard').
5. **Preview images regenerated** from the updated dashboard JSON
   (docs/images/dashboard-{dark,light,tablet}.png).

Smoke test after this pass: **41 PASS, 0 FAIL, 1 SKIP** (the one SKIP
remains the live SocketCAN capture, unavailable in the sandbox; the CAN
failure-path assertions all pass). InfluxDB/Grafana apt repositories are
not reachable from this rig, so — as in section 1 — server-side rendering
on the installed Grafana version remains commissioning steps 4–8.

## 8. Simulation-alignment pass (2026-09-03, branch sim-alignment)

Scope: alignment with the simulation community (`simulated.moos`:
uSimMarineV23, planchaPID, pHelmIvP, iWebSocket, pContactSpawn,
pContactMgrV20) running on the simulation host usv01 with six Signal K
instances, plus the operator-screen redesign and Swiftships branding.
Validated on the target this time: InfluxDB 2.x, Grafana OSS 13.2.1 (port
3300), Telegraf, six `usv-signalk-bridge@` instances, `can_bridge` and
`grafana_bridge` under pAntler all running; dashboards checked live in the
browser.

What changed and how it was verified:

1. **Bridge variable map** extended to the simulation's planchaPID and
   iWebSocket outputs (SPEED_CAP, CMD_SPEED, XTE_NOW, NEXT_TURN_ANGLE,
   ARRIVAL_STATE, CORRIDOR_STATUS, CRASH_STOP_STATUS, REMOTE_CONTROL,
   CRASH_STOP, REGION_WARNING, ID_ROUTE, J1939 ENGINE_*), rate limiting
   (5 Hz nav, 2 Hz engine), change-only strings re-emitted at 1 Hz,
   COMMS_LOST phases (false / grace / action / rtb), radar and contact
   aggregates. Live check: 86 variables subscribed, nav / autopilot /
   autonomy / mission_state series present in InfluxDB.
2. **Mission state** gains `Remote` (REMOTE_CONTROL passthrough outranks
   the helm park it causes); the operator-facing behavior list is the helm's
   running set (the per-iteration active set flickers and flooded the
   events table; verified against a real pHelmIvP IVPHELM_SUMMARY from the
   s1_morgan_city_signalk log);
   `route_name` resolved from the Signal K route resource for the ID_ROUTE
   uuid. `mission_logic_test`: **141 assertions, all passing**.
3. **Async InfluxWriter** (worker thread, bounded queue, 2 s timeout) so a
   slow InfluxDB can never stall the MOOS thread; Threads linked in every
   CMake target.
4. **Fleet**: `usv-signalk-bridge@<vessel>` template unit, `VESSELS` in
   usv.env, Fleet dashboard (map of last known positions, repeated status
   row). Live check: six instances active.
5. **Dashboards** regenerated as single-screen operator layouts (equal 3x3
   tiles), Swiftships header band, knots / nautical miles / fixed metres,
   GPU tile, benign all-stop causes in grey. Live findings fixed on the
   target: string stat tiles need `reduceOptions.fields="/.*/"`; the text
   panel sanitizer strips `<img>` (Markdown image instead); geomap track
   and fleet queries must `group()` before `pivot()`; Telegraf counters
   need `derivative()` after `aggregateWindow()`, not as its `fn`; event
   tables `group()` so type/severity are columns; series labels dropped so
   legends read by field.
6. **Branding** (`deploy/brand.sh`): logo, favicon, login artwork,
   title, heading, subtitle and login card colours. Live finding: Grafana's
   hashed assets are cached immutably by browsers, so the login look is
   also injected from the never-cached `index.html` with content-hashed
   asset names. Verified on the target and on a mock Grafana tree (apply,
   re-apply, restore).
7. **Themes**: `?theme=` is read by Grafana only on page load and its click
   interceptor keeps same-window links in-app (confirmed in the 13.2.1
   bundle), so the theme chips open a new tab; `c t` documented.
8. **Installer hardening** from the target install: non-interactive apt
   with retries, keyring conffile handling, unattended-upgrades lock,
   stale CMake cache reset, pAntler PATH drop-in (`MOOS_ANTLER_UNIT`),
   `bash deploy/install.sh` when the executable bit is lost on Windows.
9. **Alert profiles**: `gen_alerts.py --profile sim|vessel` (17 rules;
   the sim profile omits rules for sensors the simulation lacks).

Smoke test after this pass: **56 PASS, 0 FAIL, 1 SKIP** (live SocketCAN
capture, unavailable in the sandbox). The dashboard coverage check now
also accepts fields mapped or written by the MOOS bridge, whose live run
needs MOOSDB on the PATH.

Known gaps carried forward: GPU input to be enabled once the host's GPU
vendor is confirmed; CPU temperature depends on the host exposing hwmon
sensors; the simulation's DESIRED_SPEED reaches the bridge as m/s per the
MOOS convention (a 20 kn console target that arrives as 20 m/s shows as
38.9 kn on the Speed Governor: a console-to-MOOS units question, not a
dashboard conversion); bravo..foxtrot MOOS communities still to be run as
services; post-mission report generator deferred.

## 9. Traffic and pre-departure readiness (2026-09-03, branches feat/traffic-view, feat/fake-traffic, feat/readiness)

Scope: the Traffic dashboard (collision-avoidance picture, onboard
CPA/TCPA, encounter and risk per contact), the fake-traffic test tooling,
and the Readiness dashboard with its onboard evaluation.

1. **Traffic**: verified live on usv01 with `tools/traffic_scenario.sh`
   (head-on reached HIGH with CPA 11 m, the critical event and the
   `usv-collision-risk` alert fired; radar-only target counted; opening
   contact stayed CLEAR) and with `tools/fake_traffic.py` (harbour scene,
   six contacts plus a radar track, MEDIUM). Finding for the autonomy
   team: the simulation helm never engaged an avoidance behavior.
2. **Readiness, C++ side**: `usv::EvaluateReadiness` decides all 15
   checks from a `LatestStore` of the newest sample per field;
   `mission_logic_test` covers the simulation profile (N/A for missing
   sensors, CONSOLE for the unpublished checklist / ENC), the vessel
   profile (missing data FAILS, GPS quality required), an all-pass set
   with the exact evidence strings, and the fault set (rudder not
   following, low fuel, pending checklist items, HIGH collision risk,
   stale position; blocking list in check order), the last-known course fallback and the checklist rows. 238 assertions, 0 failed.
3. **Readiness, bridge**: the smoke test's InfluxDB stub answers the
   bridge's poll query with annotated CSV (Signal K up, Internet up 38 ms,
   CAN up 212 msg/s, host 4 / 11 / 63 %), and the captured line protocol
   shows those values inside the comms, CAN and onboard-computer evidence,
   the console checklist and ENC rows evaluated from the poked
   `CHECKLIST_STATUS` / `ENC_STATUS`, the course row seeing route,
   waypoints and region, N/A for the engine under `readiness_profile =
   sim`, the safety row failing on the scenario's crash stop, and the
   change event. The poll query was checked to restrict CPU to
   `cpu-total`.
4. **Live on usv01** (first run of the branch): all 15 rows (16 since the endurance and apps checks) with evidence
   within 10 s of the restart, NOT READY with 6 / 2 / 5 / 2 counts. Fixes
   from that run: status mappings had also coloured the "#" column
   (override on Status only, fixed column order); the course check lost
   the route after `systemctl restart moos-antler` restarted MOOSDB
   (now reads the last known route / waypoints / region back from
   InfluxDB, flagged "(last known)" after 10 min, like the Route tile);
   the sim host's can_bridge reports the interface down with no CAN
   hardware (N/A under the sim profile); the console checklist table is
   now fed by rows the bridge writes (measurement `checklist`) instead of
   a Flux script.
   Second live run (usv01, after 0034): columns in order, CAN N/A,
   course row "Rt WP Alpha (last known) · 4 waypoints · no operating
   region" (region_vertices is new and is first written at the next
   mission start); poking CHECKLIST_STATUS / ENC_STATUS turned the ENC
   row PASS, Console 2 -> 0, the checklist table showed the head line
   and the two PENDING items, and the checklist row failed with the
   pending names. Cosmetic fixes: Item column min width, checks table
   column widths; installer now installs moos_poke with the bridge.
   Third pass (after the operator / developer review of what a
   pre-departure check must contain): MOOS apps against DB_CLIENTS,
   endurance against the route, route inside the region and route
   length, active DTCs, AIS and radar merged; checklist table with every
   item and description from the console's full form or the configured
   item list. Unit tests cover each (270 assertions); the smoke test
   configures `required_apps`, the fuel constants and `checklist_items`
   in test.moos and asserts the apps row ("1 / 2 running · missing:
   pHelmIvP"), route length 2800 m, route outside the ±100 m region,
   the endurance evidence and the TICKED / description rows (88 PASS).
   Branding: the navigation menu's "Grafana" label is rewritten to
   `BRAND_APP_TITLE` by a script injected in index.html; verified on a
   mock tree with a headless browser (only the exact "Grafana" text node
   changes, re-renders included).
5. **Dashboard**: `usv-readiness.json` fills the 24 x 23 grid with no
   overlap (8 tiles, two 18-row tables); tab order Mission, Readiness,
   Navigation ...; the Flux for the checks table reuses the pivot-then-join
   pattern proven live for the contacts table.

Smoke test after this pass: **82 PASS, 0 FAIL, 1 SKIP**. To verify on the
target: the Readiness tab shows 16 rows with evidence within 10 s of the
bridge restart; the Failed tile and the "Readiness NOT READY: ..." event
name the blocking checks; poking the two console variables turns the
CONSOLE rows into PASS.
