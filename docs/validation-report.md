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

## 10. Engine profile and USV-725 release candidate (2026-10-14, branch usv-725-engine)

Scope: the per-vessel engine profile (ENGINE_SOURCE / ENGINE_TYPE, one
writer per engine field), the Engine page and per-engine-type dial
scales, the restyled analog instruments, the Signal K engine paths for
the Yamaha gateway, the grafana_bridge additions for the Mercury diesel
(instance suffix, lamps, fault text with the Mercury J codes, ECU
voltage) and the shared J1939 fault table. Requirements:
docs/requirements-usv-725.md.

Environment: Ubuntu build host; g++ 13, CMake 3.28, libcurl 8.5,
nlohmann-json 3.11, IXWebSocket v11.4.5 built from source, core-MOOS v10
built from source (MOOSDB and umm), Node 22 with `ws` and
`@influxdata/flux-lsp-node`. Repository state: commit 46ffc43 plus the
smoke-test change below.

1. **Smoke test (`tools/smoke_test.sh`): 120 PASS, 0 FAIL, 1 SKIP** (the
   CAN live capture needs a vcan kernel module the host lacks; the same
   frames were replayed through the built can_bridge with a SocketCAN
   stand-in: trim UP / DOWN / IDLE, the single-frame DM1, the three-fault
   BAM DM1 with its text, and the cleared state). Every C++ program built,
   including moos_bridge against real MOOS 10 headers with -Wall -Wextra
   clean. Unit tests: mission_logic_test 316, mayara_logic_test 59,
   frigate_logic_test 19, usv722_can_test 75, engine_profile_test 44,
   j1939_faults_test 18, signalk_engine_test 95, all passing. The Signal K
   mock server fed every engine path in both id forms (17 unit checks);
   the MOOS section ran the full Alpha scenario against MOOSDB with
   ENGINE_SPEED -> engine.rpm{instance=0}; 232 Flux queries type-check;
   all 22 alert rules query fields the stack writes; 19 ECharts
   instruments return an option with and without data in both themes.
2. **Smoke-test changes from this run**: the test now pins an empty engine
   profile (`USV_ENV_FILE` to an empty file, ENGINE_* unset) so a vessel's
   own `/etc/usv/usv.env` cannot change the vectors when the test runs on
   the boat; and the query-completeness check accepts a query built from a
   variable alone (`array.from`, the Traffic View range), which it had
   flagged since v1.2.15.
3. **Bridges driven directly** (outside the smoke test, same binaries):
   grafana_bridge with ENGINE_* mail: ENGINE_SPEED_1 -> instance=1,
   ENGINE_LAMPS 0x14 -> amber and red on, ENGINE_DTC_TEXT "SPN 100 FMI 18
   (x1); SPN 190 FMI 0 (x3)" -> "SPN 100 FMI 18 J143 Oil_Press_Low (x1);
   SPN 190 FMI 0 J234 Engine_Overspeed (x3)" on a mercury_diesel profile,
   ENGINE_BATTERY_VOLTAGE -> engine.ecu_voltage only, and no engine point
   at all with ENGINE_SOURCE=signalk. signalk_bridge with ENGINE_SOURCE=moos
   writes no engine point. usv-725 profile through install.sh's checks:
   ENGINE_SOURCE=moos, HELM_PROFILE=none accepted, the grafana_bridge
   mission block printed.
4. **Dashboards rendered** from the generated JSON with ECharts at 1080p
   (mocked Grafana context, both themes, both engine types, no-data
   state): Engine page, Vessel Health cluster, Autopilot, Autopilot Live,
   Navigation. The typed dials switch scale with the engine_type variable
   (Mercury 0 to 5000 rpm / 0 to 800 kPa, Yamaha 0 to 6000 / 0 to 600) and
   fall back to the Yamaha scale without it.

To verify on the target (USV-725 first, section 6 of the requirements):
the header chip reads mercury_diesel, the Engine page shows RPM, coolant,
oil pressure and boost from iCANRec within 10 s of pAntler start, Oil Temp
and the ECU voltage are the values iCANRec publishes today (wrong until
REC-1 and REC-3 land in the MOOS repo), Engine Hours and Fuel Level read
N/A until REC-6, the Fault Codes tile reads NONE or the J-coded text, and
the Readiness engine row passes with the engine running above 700 rpm.

## 11. Releases v1.2.28 to v1.2.34 (2026-10-15 to 2026-10-17)

Scope: can_bridge reading the Mercury ECU directly (v1.2.28 to v1.2.30),
1 s refresh and fit-to-screen (v1.2.31, v1.2.32), vessel particulars,
fuel in litres, the Speed cap check and the Theme picker (v1.2.33), and
the fix for the empty Grafana pages v1.2.33 caused (v1.2.34).

Environment: as section 10, plus Go 1.24 for the template parse, run as
root. Every release ran `tools/smoke_test.sh` in full with 1 SKIP (CAN
live capture, no vcan module): v1.2.28 159 PASS, v1.2.29 and v1.2.30 158,
v1.2.33 162, v1.2.34 164; 0 FAIL each time. The CAN replays of both hulls'
recorded traffic (tools/fixtures) are part of every run.

Verified on the boats:
1. USV-725 (v1.2.30, engine off on the hard): engine values from the ECU,
   hours 30.75 h after the request went to the global address, the DM1
   with five entries folded to "(x75, x1, x77)", Readiness restored once
   the mission file's merge conflict was resolved.
2. USV-722 (v1.2.33): header band with the class name; the page itself
   came back empty (below).

Defect found on the boat, not by the smoke test: v1.2.33's theme script
contained two square brackets in a row. Grafana parses
`views/index.html` as a Go template with those delimiters, so the parse
failed and every page came back empty (ERR_EMPTY_RESPONSE). The smoke test
did not run `deploy/brand.sh` at all then. Since v1.2.34 it runs brand.sh
on a stand-in Grafana page, fails on the delimiters in any injected
block, and parses the result with Go's `html/template` exactly as Grafana
does; the check was confirmed to fail on the v1.2.33 script. v1.2.34 also
checks that the committed dashboards and alert rules equal the generators'
output, and `gen_alerts.py` now defaults to the vessel profile the boats
run. The Readiness particulars card of v1.2.33 showed raw HTML (Grafana
escapes variable values in an HTML text panel); v1.2.34 makes it a table.
The Navy colour remap was verified in Chromium against a stand-in page
(page, panel and border colours, the choice remembered across pages,
Light and Dark reloads), not yet against Grafana itself.

To verify on the target: v1.2.34 on both boats (USV-725 straight from
v1.2.30), the particulars card as a table, Fuel Remaining / Range Left /
Time Left with a fuel reading, the Speed cap row with SPEED_CAP
published, the Navy theme on a real Grafana page, and fit-to-screen on
USV-722's own display.

## 12. Release v1.2.36 (2026-10-19)

Scope: the Readiness Course planning row accepts a route planned safe on
the ENC (Swiftships-UI saves it with "(safe)" in its name) in place of an
operating region, and fails a route named "(unsafe)"; the Chart / ENC row
passes on such a route while the console does not publish ENC_STATUS.

Environment: as section 11. `tools/smoke_test.sh` in full: 165 PASS,
0 FAIL, 1 SKIP (CAN live capture, no vcan module). The readiness unit
tests (mission_logic_test, 417 checks) cover the safe route without a
region, a plain route without a region, an "(unsafe)" route with a region,
and a published ENC_STATUS taking precedence over the safe route.

Observed on USV-722 before the release (2026-10-19): Course planning
FAIL with route "10-19-2026 (safe) · 6 waypoints · 0.6 nm · no operating
region" and Chart / ENC N/A, while Swiftships-UI showed the ENC chart
and the safe route.

To verify on the target: on USV-722 with a safe route loaded, Course
planning PASS with "checked safe on ENC" and Chart / ENC PASS with "ENC
in use · route checked safe on chart".

## 13. Release v1.2.37 (2026-10-19)

Scope: mission state Remote whenever the boat is under way with no
autonomy mode (or with the helm parked), new states Following (TRAILING)
and Region Hold (REGION), the Readiness Route tile text size, and the
update script's tag fetch.

Reason: on USV-722 the operator drove the boat from Swiftships-UI and the
Mission State tile read Idle. The production iWebSocket (checked in the
mission repository) sends DESIRED_THRUST / DESIRED_RUDDER / DESIRED_GEAR
straight from the console and never posts REMOTE_CONTROL; planchaPID
publishes DESIRED_THRUST only under WAYPOINT, TRAILING, STATIONING or
REGION, so a throttle command outside those modes is the operator's.

Environment: as section 11. `tools/smoke_test.sh` in full: 169 PASS,
0 FAIL, 1 SKIP (CAN live capture, no vcan module). New checks: TRAILING
gives Following, REGION gives Region Hold, DESIRED_THRUST=25 with no
autonomy mode gives Remote and the Remote Control tile on. Unit tests
(mission_logic_test, 447 checks) cover every autonomy mode against a
moving boat, the parked helm, comms loss outranking Remote, and the
motion latch (throttle ahead and astern, speed alone, the 10 s hold, the
30 s coast grace after a mission, speed test off).

To verify on the target: drive USV-722 from Swiftships-UI with no
mission: Mission State Remote within a second of the throttle, back to
Idle about 10 s after the throttle returns to zero and the boat slows
below 1 kn; Follow and Region from the UI show Following and Region Hold.

## 14. Release v1.2.38 (2026-10-19)

Scope: the mission state outside autonomy as the operator defined it on
2026-10-19: autonomous mode running reads its own state; no autonomous
mode started (mission loaded or not) and Swiftships-UI sending throttle or
rudder reads Remote; moving with no command from the system or the
operator reads Moving; at rest Idle / Parked.

Environment: as section 11. `tools/smoke_test.sh` in full: 169 PASS,
0 FAIL, 1 SKIP (CAN live capture, no vcan module). The smoke scenario now
ends region hold, commands throttle (Remote), releases it while the boat
moves (Moving) and stops (Idle), and checks that exact sequence. Unit tests
(mission_logic_test, 453 checks) cover every autonomy mode against
commands and motion, the parked helm with a loaded survey, the rudder-only
command, astern throttle, the 10 s hold, dropping the autopilot's last
outputs when autonomy stops, and the Moving state switched off.

To verify on the target: with no mission started, throttle or rudder from
Swiftships-UI reads Remote; releasing both while the boat still moves
reads Moving after about 10 s, then Idle once it is below 0.5 kn; starting
a mission reads Autonomous and ending it does not flash Remote.

## 15. Release v1.2.39 (2026-10-21)

Scope: the login page footer (Documentation, Support, Community, Open
Source, platform version) replaced by the export-control notice
(`BRAND_DISCLAIMER`) through the brand CSS; `hide_version = true` and
`feedback_links_enabled = false` written to grafana.ini by the installer
in a marked block.

Reason: the login page on USV-725 (dashboard v1.2.38) still showed the
platform's footer links and version under the Swiftships login card; the
console carries no tool names.

Environment: as section 11. `tools/smoke_test.sh` in full: 170 PASS,
0 FAIL, 1 SKIP; the brand.sh stand-in check now also requires the footer
rule and the notice text in the injected CSS.

To verify on the target: Ctrl+F5 on the login page, the notice below the
card in place of the product links; after login, the help menu (?) shows
no version line.

## 16. Release v1.2.40 (2026-10-21)

Scope: the operating region read from REGION_POLY (route-corridor
missions), REGION as a fence flag instead of a "Region Hold" state,
per-check readiness events, and the operator popup script in the branded
page.

Reason: on USV-722 (mission "insidenewregion", region drawn and active in
Swiftships-UI) the Readiness course row read "no operating region": the
bus carried the polygon in REGION_POLY while REGION_UPDATES held only
"reset=true # breach_dist=15 # recover_speed=4". The operator also asked
for a popup when the INS is not aligned or any sensor fails.

Environment: as section 11. `tools/smoke_test.sh` in full: 173 PASS,
0 FAIL, 1 SKIP. New checks: REGION_POLY gives an 8-vertex region that a
parameter-only REGION_UPDATES leaves alone; REGION=true writes
region_active and changes no state; the state sequence outside autonomy
with the region enforced is Idle, Remote, Moving, Idle; the geofence
breach turns the Safety flags check FAIL and records the critical event;
the popup script is injected once with no template delimiters. Unit tests
457. The popup script was syntax-checked but not run against a live
Grafana here.

To verify on the target: with the region drawn in Swiftships-UI, Course
planning reads "... region N vertices" and the Navigation map draws the
outline; Mission State reads Autonomous during the survey; a popup
appears within 5 s of a check turning FAIL (test with the GPS antenna
covered or the radar server stopped) and the Mission Events list shows
the same line.

## 17. Release v1.2.41 (2026-10-21)

Scope: 2 s refresh on pages with more than 18 queries, popup cards moved
to the bottom-right corner, readiness verdict popups only on a flip.

Reason: on USV-722 at v1.2.40 the Navigation page showed blank lower
tiles and trend panels while the top row was live; with the refresh
turned off every panel filled (COG 144, WP bearing 166, roll/pitch, the
heading / SOG / attitude trends). The page issues 26 queries per refresh;
a browser runs six at a time and each 1 s refresh cancelled the rest.
The two readiness popups five seconds apart at 14:51 were the verdict
event repeating for a change in its blocking list.

Environment: as section 11. `tools/smoke_test.sh` in full: 173 PASS,
0 FAIL, 1 SKIP; the readiness check now requires the verdict event to be
a warning on the first NOT READY. Unit tests 457.

To verify on the target: Navigation with its default refresh (2 s) fills
every panel within a few seconds of opening; one popup per check failure.

## 18. Release v1.2.42 (2026-10-21)

Scope: Chart / ENC readiness row read from the boat's chart server.

Reason: on USV-722 the console was drawing the uploaded NOAA ENC cells
while the Readiness row read "no ENC chart system on this vessel" (N/A):
the console publishes no ENC_STATUS, the running route was not named
"(safe)" and the mission file carries `enc_fitted = false` from before
the ENC upload. The chart server itself lists what it serves: Signal K
on USV-722 answered `/signalk/v1/api/resources/charts` with ten S-57
cells, each with name, description and bounds (US1GLBCF, US3FL1EH,
US3LA1CB, US3LA1CC, US3MS1AA, US5LMOBB, US5LMOBC, US5LMOFF, US5LMOHF,
US5LMOIF).

Evidence:
- Unit tests (mission_logic_test, 490 checks): the listing parser on the
  USV-722 response shape (identifier, name, description, bounds; v1
  `tilemapUrl`/`chartLayers` and v2 `url`/`layers` members; a base map of
  type tilelayer not counted as an ENC), the coverage pick (Berwick
  29.7241 / -91.2179 -> US3LA1CB, east of -91.2 -> US3LA1CC, inside a
  harbour cell -> the US5 cell, Lumut -> no cell, no position -> count
  only), malformed and non-JSON bodies, and the readiness row for every
  source (covers / not covered / position unknown / one cell wording /
  console ENC_STATUS winning / stale listing and no cells falling back
  to N/A).
- Smoke test: the InfluxDB stub also answers the chart listing (two
  S-57 cells, the smaller covering the mission origin, plus a base map);
  the bridge polled it, wrote `chart enc_cells=2`, `enc_cell="US5SMOKE"`,
  the row read "ENC US5SMOKE · covers position · 2 cells loaded" until
  the console's ENC_STATUS was poked, after which "selected:US5LA31M"
  took the row.

Environment: as section 11. `tools/smoke_test.sh` in full: 177 PASS,
0 FAIL, 1 SKIP (vcan replay). Unit tests 490.

To verify on the target: after the mission restart, Readiness row 3
reads "ENC US3LA1CB · covers position · 10 cells loaded" at Berwick
(the US5 harbour cells lie south of 29.25 N) and the bridge log shows
"chart server lists 10 ENC cells".

## 19. Release v1.2.43 (2026-10-23)

Scope: the dashboard side of the mission and Swiftships-UI
`feature/command-auth` builds (22 Oct): boat control audit trail and Audit
page, navigation integrity, ALL STOP, drive interface, region state,
service mode and gains, safe route from the planner note, and the Signal
K login for the dashboard.

Source: the formats were read from the mission branch itself
(iWebSocket audit lines without their timestamp on CONTROL_EVENT /
UI_AUTH_EVENT, a forced takeover as two lines; pNavGuard NAV_STATUS once a
second and NAV_GUARD_STOP every 2 s in a fault; iCAN status once a second;
planchaPID PID_GAINS every tick; Swiftships-UI's planner note "Planned in
water >= X m with Y m clearance (cells)").

Evidence:
- Unit tests (mission_logic_test, 598 checks): the planner note parser and
  the Course row on a re-planned route; Navigation integrity for ok,
  warning, fault, a silent guard (expected or not), stale level; ALL STOP
  above every state and in Safety flags; region breach; service mode
  evidence; drive status, conflict and missing profile on the CAN row; a
  rudder mismatch on the Steering row with and without rudder values; the
  audit line parser and AuditTrail (every control and login action, the
  forced takeover joined or reported alone after 2 s, holder and open
  logins, a link restart clearing both, empty lines).
- Smoke test, MOOS bridge against a real MOOSDB: the route planner note
  read from a stand-in Signal K route; nav_guard 2 with the reason, one stop
  event for two NAV_GUARD_STOP posts, recovery event, the row N/A without
  pNavGuard; AllStop state, Safety flags and reset; audit rows with
  category, action, user and role; the forced takeover joined with its
  reason; holder and open logins; two refused commands audited with one
  popup; region refusal and breach; can_drive fields, conflict event and
  CAN row FAIL, Steering FAIL on a mismatch; gains and service mode.
- Smoke test, Signal K login: usv_auth logic (token from the Signal K
  cookie, the Swiftships-UI cookie or a bearer header; boat role from the
  roles file or the level, a read-only account always a viewer; role map;
  cache; Signal K unreachable), then the real nginx template in front of a
  stand-in Grafana with a stand-in Signal K: no login gives no user
  header, operator and supervisor become Editor, read-only and planner
  Viewer, an unknown token no user, and user / role headers sent by the
  browser are dropped. The Audit folder wiring in the provisioning and
  the installer is checked.
- Dashboards: 270 distinct Flux queries type-checked, every queried field
  produced by a bridge, generators in sync, panel titles fit.

Environment: as section 11, plus nginx 1.24 for the gateway chain.
`tools/smoke_test.sh` in full: 205 PASS, 0 FAIL, 1 SKIP (vcan replay).

Not verifiable here: Grafana's own handling of the role header (the
gateway also sets the role through Grafana's API, see deployment.md) and
the boats' new mission and Swiftships-UI builds. To verify on the target,
after the boat runs the 22 Oct builds: Readiness row 2 reads PASS at
speed; a take / force / ALL STOP from Swiftships-UI appears on the Audit
page within seconds with the right user; with SK_LOGIN=1 the dashboard
opens logged in as the Swiftships-UI user, an operator sees the Audit page
and a viewer does not.

## 20. Release v1.2.44 (2026-10-23)

Scope: fixes seen on usv-722's first run of the Audit page (mission
feature/command-auth, Signal K login on): Open Sessions empty for a cookie
login without a tab session, ALL STOP N/A before the first press, audit
log column order. Unit tests 603 (cookie logins kept by user and IP,
removed on disconnect and logout). `tools/smoke_test.sh` in full: 205
PASS, 0 FAIL, 1 SKIP.

## 21. Release v1.2.45 (2026-10-23)

Scope: the Audit page aligned with the agreed mockup (tile second lines,
"lost" holder, refused by reason, events per user with fixed columns, open
sessions table, Category filter, 24 h on open), the role on release / lost
/ logout rows, and the Signal K "admin" name clash in the login gateway.

Evidence: unit tests 609 (lost holder kept until control is taken again,
holder's role on release and lost rows, per-session list, "no login"
wording); smoke test: one c2_session row per open login, control_holder
"lost:carol" and its audit row with the holder's role, the gateway maps a
Signal K user "admin" to sk-admin and never calls the Grafana API for the
administrator; 273 Flux queries type-checked (each Audit tile returns one
row also with no audit data: a placeholder row is unioned before the
record is read). `tools/smoke_test.sh` in full: 208 PASS, 0 FAIL, 1 SKIP.

Not run here: the tile queries against a live InfluxDB (none reachable
from the build host). To verify on the target: every Audit tile shows a
value and its second line; on usv-725 (no audit data yet) the tiles read
0 / "none in this range" rather than an error.

## 22. Release v1.2.46 (2026-10-23)

Scope: findings from usv-722's first run of v1.2.45 (17:35 to 17:40 boat
time): seven logins with a logged-out token counted as "other" in
Commands Refused, Events by user hidden behind a horizontal scroll bar,
Refused commands by reason filled with zero rows.

Evidence: unit tests 611 (the logged-out-token refusal carries "why
logged-out session" ahead of ip and session); smoke test: the poke
`UI_AUTH_EVENT=rejected login=logged-out-token ...` writes a refused row
with that reason; 273 Flux queries type-checked. `tools/smoke_test.sh` in
full: 209 PASS, 0 FAIL, 1 SKIP.

Verified on usv-722 with v1.2.45 (17:39): an operator account (level
readwrite) opens the Audit page; Open sessions lists its login with role
and IP.

## 23. Release v1.2.47 (2026-10-23)

Scope: Drive Trim tile on the Engine page. Source on usv-722 (Signal K
data browser, 2026-10-23): `propulsion.port.drive.trimState` 0.31 from the
Yamaha gateway (PGN 127488, source 3.0), alongside revolutions,
temperature, engine load and torque, fuel rate, run time, alternator
voltage and 22 engine status notifications (all normal).

Evidence: signalk_engine_test maps drive.trimState 0.25 to trim_position
25 %; the smoke test's stand-in Signal K server sends the same path and
the bridge writes `trim_position=25`; the dashboard-to-writer check now
reads the Signal K engine path table (signalk_engine.hpp), so
engine.trim_position counts as written; 274 Flux queries type-checked.
`tools/smoke_test.sh` in full: 209 PASS, 0 FAIL, 1 SKIP.

To verify on the target: the Engine page shows Drive Trim near 31 % with
the drive as on 2026-10-23, and it follows the trim switch.

## 24. Release v1.2.48 (2026-10-23)

Scope: header band logo per theme (white knockout wordmark on Dark and
Navy, navy logo on the white chip on Light), as requested on 2026-10-23.

Evidence: brand.sh run twice on the stand-in Grafana public directory
injects the theme rule once, with the image's content hash, and copies
`swiftships_wordmark_light_sm.png`; the branded index.html still parses as
a Go template; the rule rendered in Chromium on a navy page (white
wordmark, no chip) and a light page (chip). `tools/smoke_test.sh` in
full: 210 PASS, 0 FAIL, 1 SKIP.

To verify on the target: Ctrl+F5, then switch Theme between Dark, Navy
and Light on any page.

