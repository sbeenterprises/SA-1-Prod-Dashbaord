# Operator User Guide

## Getting in
Browse to http://<host>:<port> (GRAFANA_PORT in /etc/usv/usv.env: 3001 on
a vessel, 3300 on the simulation host), log in on the Swiftships login
page, open folder **Swift Autonomous Dashboard**. The chips across the top
of every dashboard switch between the thirteen dashboards and keep the time
range and vessel selection. All dashboards refresh every 5 s (1 to 5 s
selectable) and fit one 1080p screen without scrolling. The **Vessel**
dropdown selects the vessel; the **Fleet** dashboard shows all of them. The
tab row keeps the same order on every dashboard: Mission, Navigation,
Autopilot, Vessel Health, CAN Bus, Traffic, Cameras, Comms, System, Replay,
Alarms, Fleet.

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
| Navigation | Live map (operating region in cyan, planned route with numbered waypoints in green, track in red, vessel as a red heading arrow), compass (HDG/COG needles), speed meter, GPS fix and satellites, lat/lon, KPI strip (SOG, COG, heading, XTE, distance to WP, HDOP, depth, roll/pitch), trends |
| Autopilot | Helm state, all-stop, station keeping, entry gate, re-arm hold, remote control, corridor, crash stop; active behaviors, heading/speed error, rudder/throttle commands, speed cap, next turn; rudder/throttle/heading-error instruments; desired vs actual heading; speed governor (desired / cap / commanded / actual) |
| Vessel Health | Instrument cluster (tachometer, coolant, oil pressure, fuel, battery volts, SOC), engine and electrical tiles, trends |
| CAN Bus | Utilization, msgs/s, error/dropped frames, PGN table |
| Traffic | Collision avoidance picture: risk level, avoidance state, contact counts by source, closest range and CPA; heading-up polar traffic view with range rings, contacts and CPA line; highest-risk contact card (CPA, TCPA, range, bearings, relative speed, type, encounter); MOOS action (planned vs commanded course, actual heading, speed command vs SOG, avoidance behavior, reason); contact table and CPA/TCPA trend |
| Cameras | Connection, stream, FPS, bitrate, recording, PTZ |
| Comms | Internet/Starlink/VPN/Signal K status, latency, loss, bandwidth |
| System | CPU, GPU, RAM, disk, CPU temperature, load, Signal K and MOOS reachability, trends, systemd services |
| Replay | Historical playback: GPS track, mission state, events, heading/speed/actuator history |
| Alarms | Active alerts, alarm history, at-a-glance status wall |
| Fleet | Last known positions on one map and a status row per vessel, each tile opening that vessel's Mission dashboard |

## Instruments
Navigation, Autopilot and Vessel Health carry analog instruments styled
like a marine helm cluster: the compass shows the heading needle (teal)
with a thinner COG needle (blue); the rudder indicator follows marine
convention (red to port, green to starboard); machinery meters carry
semantic bands (green normal, yellow caution, red alarm) that match the
alert thresholds exactly. Every needle instrument also shows a digital
readout, and all instruments follow the dark/light theme toggle.

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
ticked in Freeboard as before; Grafana mirrors the count and the pending
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
| ReturnToBase (shown "Return to Base") | Driving back to the base/return point |
| EnteringRegion | Mission started outside the region — recovering inside before the survey may run |
| AwaitingRearm | Operator link is back but the mission is NOT re-armed; the survey stays held until an operator AUTONOMOUS_START/RESUME |
| CommsLost | Operator-link failsafe in force (action shown in the events table); boat holds at the loss position |
| Parked | Helm parked (manual override) — thrust actively zeroed. All-Stop shows PARKED / IDLE in grey for these benign causes; only unexpected all-stops are red |
| Completed | Survey finished; boat stopped at the final waypoint / parked |
| Idle | No mission requested |

`AwaitingRearm` and `CommsLost` are the two states that need operator
action — both also raise alerts.

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
operations contact point. Severity `critical` repeats every 10 min until
resolved; `warning` hourly. Alarm history is kept 1 year in the events
bucket.

## Tips
- `?kiosk` in the URL gives a clean wall-display view.
- Time ranges and the vessel variable are sharable via the URL.
- Panels can be viewed full screen with `v` while hovering.
