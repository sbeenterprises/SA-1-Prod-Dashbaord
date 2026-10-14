# Operator User Guide

## Getting in
Browse to http://<host>:<port> (GRAFANA_PORT in /etc/usv/usv.env: 3001 on
a vessel, 3300 on the simulation host), log in on the Swiftships login
page, open folder **Swift Autonomous Dashboard**. The chips across the top
of every dashboard switch between the fourteen dashboards in the tab row and
keep the time range and vessel selection (Autopilot Live is the fifteenth,
opened from the dials of the Autopilot page). All dashboards refresh every
5 s (the Autopilot pages, Engine and Traffic every 1 s; 1 to 5 s
selectable) and fit one 1080p screen without scrolling. The **Vessel**
dropdown selects the vessel; the **Fleet** dashboard shows all of them. The
tab row keeps the same order on every dashboard: Readiness, Mission,
Navigation, Autopilot, Vessel Health, Engine, CAN Bus, Traffic, Cameras,
Comms, System, Replay, Alarms, Fleet.

Units are nautical throughout: knots, nautical miles for long distances,
fixed metres for short ranges (approach distance, cross-track error).

## Themes
Dashboards work in Grafana's dark and light themes. The ☀ Light / ☾ Dark
chips open the current dashboard in a new tab in that theme (Grafana only
reads the theme on page load). To switch the tab you are in, press `c`
then `t`. Set your lasting default under Profile -> Preferences ->
Interface theme.

## Dashboards
| Dashboard | What you watch |
|---|---|
| Mission | Status wall (mission state, helm, armed, operator link, geofence, all-stop, arrival, obstacles), readouts (mission, route name, elapsed, ETA, waypoint n of m, remaining, distance to waypoint, distance traveled in nm), progress bar, approach profile, autonomy flags, state timeline, mission events |
| Readiness | Pre-departure verdict (READY / NOT READY) with pass / fail / N/A / console counts, mission state, route and operator link; the automatic checks table (GPS fix, course planning incl. route inside the region, chart/ENC, autonomy stack, MOOS apps running, steering, operator link and comms, CAN bus, engine incl. DTCs, electrical, endurance against the route, AIS/radar, cameras, onboard computer, safety flags, console checklist) with the evidence behind each verdict; the console checklist mirror (every item with its description, ticked or pending, who ticked, count). Decided onboard every 5 s; the Mission Events list records when the boat became ready and what blocked it |
| Navigation | Live map (operating region in cyan, planned route with numbered waypoints in green, track in red, vessel as a red heading arrow with a lime 6-minute speed vector along its course), compass (HDG/COG needles), speed meter, GPS fix and satellites, lat/lon, KPI strip (SOG, COG, heading, XTE, distance to WP, HDOP, depth, roll/pitch), trends |
| Autopilot | Helm state, all-stop, station keeping, entry gate, re-arm hold, remote control, corridor, crash stop; active behaviors, heading/speed error, rudder/throttle commands, speed cap, next turn; rudder (actual angle from the steering actuator, or the command labelled COMMAND when there is no CAN feedback) / throttle / heading-error instruments; rudder trend (actuator command and actual in degrees, PID output in % on the right axis); CAN row (rudder actual, helm wheel, steering drive, helm / autopilot control, gear, thrust to engine, key switch, engine faults); trim switch (UP / DOWN / IDLE: the helm button, not the trim position); desired vs actual heading; speed governor (desired / cap / commanded / actual) |
| Autopilot Live | Large rudder / throttle / heading-error dials for sea trials and troubleshooting, helm state, steering mode, gear, engine thrust, trim switch, engine faults, speed error and XTE tiles, heading / rudder / speed trends over 5 min; 1 s refresh. Open it from the link icon on any dial of the Autopilot page |
| Vessel Health | Whole-vessel summary: instrument cluster (tachometer, coolant, oil pressure, fuel, battery volts, SOC), Engine Faults, battery current, charging state, total power, system voltage, alternator and ECU supply tiles, battery / power / SOC / fuel trends. The engine dial scales follow the vessel's engine type; the three engine dials and the Engine Faults tile open the Engine page, where all engine detail lives |
| Engine | Large machinery dials at 1 s for sea trials and troubleshooting: tachometer, coolant, oil pressure, boost (intake manifold pressure); oil temp, fuel rate, engine load, torque, engine hours, fuel level, fault lamps and active fault count; every active fault code across the width; RPM, coolant / oil temperature and oil pressure / boost trends over 15 min. A value the hull's engine does not report reads N/A |
| CAN Bus | Utilization, msgs/s, error/dropped frames, PGN table; trim switch, active fault count, fault lamps (OFF, or the lamps on, most severe first: RED STOP, MIL, AMBER, PROTECT) and fault codes (first active fault as SPN / FMI with its meaning and occurrence count, "+N more" when several) |
| Traffic | Collision avoidance picture: risk level, avoidance state, contact counts by source, closest range and CPA; heading-up polar traffic view with range rings, own ship as a red pin at the centre, contacts as hulls turned to their heading (green report / AIS, amber radar, red HIGH risk) with 6-minute speed vectors, and a CPA line to the riskiest contact (zoom with the **+ / − / A** buttons in its top-right corner or the **Traffic range** selector: 0.25 to 24 nm, A = auto; contacts outside a zoomed range are counted as "+N beyond"); highest-risk contact card (CPA, TCPA, range, bearings, relative speed, type, encounter); MOOS action (planned vs commanded course, actual heading, speed command vs SOG, avoidance behavior, reason); contact table, Helm Avoiding (COLREGS) table and CPA/TCPA trend |
| Cameras | Frigate NVR up/down; cameras enabled, streaming and network-connected; one row per camera (Network, Stream, Frame rate, Recording); frame-rate trend. Bitrate, PTZ and temperature charts are reserved for a camera-API source |
| Comms | Internet / Starlink / VPN (Twingate) / Signal K status, latency, loss, bandwidth of the three busiest links, telemetry message rates |
| System | CPU, GPU, RAM, disk, CPU temperature, load, Signal K and MOOS reachability, trends, systemd services |
| Replay | Historical playback: GPS track, mission state, events, heading/speed/actuator history |
| Alarms | Active alerts, alarm history, at-a-glance status wall |
| Fleet | Last known positions on one map and a status row per vessel (mission state, helm, armed, operator link, geofence, SOG; mission progress, Signal K, MOOS, engine type with the last RPM), each tile opening that vessel's Mission dashboard |

## Engine type
The header band of every page shows the vessel's engine next to its name
(usv-722: Yamaha Outboard Engine F115 B 4-Stroke, usv-725: Mercury Diesel
2.0L), taken from the `ENGINE_MODEL` setting of that vessel's installation;
`ENGINE_TYPE` (yamaha_outboard, mercury_diesel) sets the scales. The tachometer, coolant
and oil pressure dials on Vessel Health and the Engine page scale to that
type: 0 to 6000 rpm for the outboard, 0 to 5000 rpm with the green band to
4000 (wide-open throttle) for the Mercury diesel; oil pressure to 600 or
800 kPa. The chip stays blank until the vessel has written its first engine
value; the dials then use the outboard scale. Fault lamps and fault codes
come from the engine's own fault message: the J1939 DM1 on the Mercury
diesel (SPN / FMI codes with their meaning), the N2K engine status bits
through Signal K on the Yamaha outboard (named conditions such as "Check
Engine (alarm)" or "Low Oil Pressure (alarm)"; MIL lamp = check engine, RED
= a stop condition, AMBER = any other warning).

## Instruments
Navigation, Autopilot, Vessel Health and Engine carry analog instruments styled
like a marine helm cluster. Every needle meter has a dark dial face, a
glowing teal arc that fills from the start of the scale to the value, tick
marks and numerals inside the arc, a hairline needle and a large digital
readout (value, unit in small type, label beneath). Machinery meters carry
their alarm bands as a thin ring outside the arc (green normal, yellow
caution, red alarm) that match the alert thresholds exactly; a meter with
no data shows no needle and no fill and reads N/A. The compass shows the
heading needle (teal) with a thinner COG needle (blue); the rudder
indicator follows marine convention (red to port, green to starboard) and
the heading-error meter, whose scale is centre-zero, has the needle and the
bands without a fill. All instruments follow the dark/light theme toggle.

## Pre-departure readiness (Readiness dashboard)
Readiness is the first tab and the page Grafana opens on (a user can
pick another home dashboard in Profile > Preferences). The big tile is the verdict:
READY only when no automatic check FAILS and nothing is still waiting on
the console. Each row of the automatic table shows what was actually
seen (evidence) so a FAIL can be traced: for example "rudder 20.0° · cmd
3.0° · NOT FOLLOWING" or "9 / 11 ticked · pending: Weather Conditions,
Port Clearance". N/A means the sensor is not fitted or not reporting; on
a real vessel the bridge is configured to treat that as a FAIL (see
docs/deployment.md, readiness_profile). CONSOLE means the operator
console has not yet published the ENC selection or the checklist. The
manual items themselves (life jackets, VHF check, port clearance ...) are
ticked in Swiftships-UI as before; Grafana mirrors the count and the pending
items and adds the automatic verification on top.

## Mission states (Mission dashboard)
Derived onboard from the autonomy stack's own variables. Tiles show the
operator wording (Station Keeping, Return to Base, Entering Region,
Awaiting Re-arm, Comms Lost); the identifiers below are the stored values
used by alerts and queries:

| State | Meaning |
|---|---|
| Autonomous | Survey running (waypt_survey driving) |
| Remote | Operator driving the boat through remote control (REMOTE_CONTROL); autonomy is not in charge |
| Paused | Operator pause — boat holds at zero speed |
| StationKeeping | Holding a station point (operator request or station waypoint) |
| ReturnToBase (shown "Return to Base") | Driving back to the base/return point (operator command, or the comms-loss failsafe: the events say which) |
| HoldingAtBase (shown "Holding at Base") | Arrived at the return base after a return to base; holding station there until the operator's next command from the Swiftships-UI |
| EnteringRegion | Mission started outside the region — recovering inside before the survey may run |
| AwaitingRearm | Operator link is back but the mission is NOT re-armed; the survey stays held until an operator AUTONOMOUS_START/RESUME |
| CommsLost | Simulation iWebSocket only: operator-link failsafe in force (action shown in the events table); boat holds at the loss position |
| Parked | Helm parked (manual override) — thrust actively zeroed. All-Stop shows PARKED / IDLE in grey for these benign causes; only unexpected all-stops are red |
| Completed | Survey finished; boat stopped at the final waypoint / parked |
| Idle | No mission requested |

`AwaitingRearm` and `CommsLost` are the two states that need operator
action — both also raise alerts.

**Operator link on the boats.** The boats' iWebSocket returns the boat to
its base when nothing is heard from any Swiftships-UI for 30 s, with the
helm in DRIVE or the boat underway, and holds station there on arrival.
The Operator Link tiles read LINK OK, NO UI LINK (lost, but the boat is
parked and idle, so it stays where it is; no alarm), LOST — RTB (returning
to base) or LOST — AT BASE (holding on the base). The Operator Link Lost
(Returning to Base) alert fires for the last two. The return base set from
the Swiftships-UI (right click > Define base) is the orange BASE square on
the Navigation map, and Dist to Base gives the distance to it in nautical
miles.

## Collision avoidance (Traffic dashboard)
On the boats the helm reports every contact it is steering around
(COLAV_ALERTS, the same list as the Swiftships-UI Alert List). The
Traffic page shows it three ways:
- **Avoidance** tile: IDLE, ACTIVE (the helm is avoiding a contact) or
  IN EXTREMIS in red.
- **Traffic View**: an orange ring around each contact the helm is
  avoiding, red when in extremis.
- **Helm Avoiding (COLREGS)** table: one row per contact with the
  situation and its rule, CPA, TCPA and state. AVOIDING is orange; CLEAR
  means the avoidance has ended, and the row stays for 15 s before it
  goes; IN EXTREMIS is red.

The **Reason** tile names the contact and situation the helm is dealing
with first.

| Situation | Rule | Meaning |
|---|---|---|
| Head-on | 14 | Meeting end-on, both alter to starboard |
| Overtaking, passing to port / starboard | 13 | Own ship is overtaking and keeps clear |
| Give-way, crossing astern / ahead | 16 | Crossing, own ship gives way |
| Stand-on / Stand-on, being overtaken | 17 | Own ship holds course and speed |
| Stand-on, in extremis / Being overtaken, in extremis | 17(b) | The give-way vessel has not acted and own ship is manoeuvring to avoid collision |
| Keep clear (CPA) | none | Closest-approach avoidance outside a COLREGS situation |

In extremis is the only situation that raises an alarm (Collision
Avoidance IN EXTREMIS, critical). Starting and ending an avoidance are
recorded in the events (warning and info). The simulator does not
publish COLAV_ALERTS: there the Avoidance tile follows the helm's active
behaviours and the table stays empty.

## Mission replay
1. Open **Replay**.
2. Pick the mission window with the time picker (annotations mark waypoint
   hits and state changes).
3. Hover any graph — the crosshair is shared, and the map shows the track
   for the selected window. Windows older than 30 days are read from the
   1-minute archive automatically (numeric series; state timelines keep
   the raw 30 days).

## Alarms
Active alerts appear on the Alarms dashboard and are emailed to the
operations contact point. Engine rules: Engine Over Temperature (coolant
above 105 °C), Engine Low Oil Pressure (below 150 kPa with the engine
running above 700 rpm), Engine Fault (any active J1939 fault), Low Fuel
Level (below 15 %); none of them fires on missing data. Operator Link
Lost (Returning to Base) is critical while the comms-loss failsafe drives
the boat. Collision Avoidance IN EXTREMIS is critical while the helm
reports a stand-on encounter in extremis. Severity `critical` repeats every 10 min until
resolved; `warning` hourly. Alarm history is kept 1 year in the events
bucket.

## Tips
- `?kiosk` in the URL gives a clean wall-display view.
- Time ranges and the vessel variable are sharable via the URL.
- Panels can be viewed full screen with `v` while hovering.
