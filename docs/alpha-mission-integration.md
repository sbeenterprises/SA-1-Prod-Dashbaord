# Alpha Mission Integration — Gap Analysis & Design

This document records the integration of the **Alpha mission** (`alpha.bhv`,
the production behavior file: distance-governed survey, comms-loss failsafe
with explicit re-arm, OpRegionV24 geofence with auto-recovery, ENC obstacle
avoidance) with the Swift Autonomous Dashboard. It is the reference for what
`grafana_bridge` subscribes to, how the operational mission state is derived,
and why.

The older `alpha.fixed.bhv` revision uses a subset of the same variables
(DEPLOY / RETURN / WAYPOINT / STATIONING / PAUSE, STATION_UPDATES,
WPT_UPDATE); everything below degrades gracefully to that file — panels tied
to variables it does not publish (geofence, failsafe, re-arm) simply stay in
their safe/default state.

## 1. Variable inventory of the Alpha mission

What the running community actually publishes (behavior file + standard
pHelmIvP outputs + the community apps referenced by the behavior comments —
`planchaPID`, `iWebSocket`, `pObstacleMgr`):

| Variable | Producer | Meaning |
|---|---|---|
| `WAYPOINT`, `RETURN`, `STATIONING`, `PAUSE`, `DEPLOY` | iWebSocket / planchaPID / endflags | Mode request flags ("true"/"false") |
| `MISSION_ARMED` | iWebSocket (cleared on comms loss, set only by operator AUTONOMOUS_START/RESUME) | Explicit re-arm latch gating the survey |
| `COMMS_LOST` | iWebSocket operator-link watchdog | `false`, or the failsafe action while the link is lost (e.g. `loiter`) |
| `SURVEY_ENTRY_GATE` | iWebSocket | Start-only latch: `closed` when a mission starts outside the region, latches `open` on first entry |
| `SURVEY_COMPLETE` | `waypt_survey` endflag | Finite survey finished (boat stops at last waypoint) |
| `DIST_TO_NEXT_WPT` | `waypt_survey` / `waypt_return` (`wpt_dist_to_next`) | Live distance to next waypoint (m); drives planchaPID's speed governor |
| `WPT_INDEX` | `waypt_survey` (`wpt_index_var`) | Current target waypoint index |
| `VIEW_SEGLIST` | BHV_Waypoint (visual) | Route polyline; source for waypoint total |
| `SPEED_RAMP` | `waypt_survey` wptflag | Posted on each intermediate waypoint capture |
| `MOOS_MANUAL_OVERRIDE` | endflags / iWebSocket | Helm parked (thrust actively zeroed by planchaPID) |
| `IVPHELM_STATE` | pHelmIvP | `DRIVE` / `PARK` / `DISABLED` / `MALCONFIG` / `STANDBY` |
| `IVPHELM_ALLSTOP` | pHelmIvP | `clear` or the all-stop reason (e.g. `MissingDecVars:course`) |
| `IVPHELM_SUMMARY` | pHelmIvP | Per-iteration summary; contains the active behavior set |
| `OPR_BREACHED` | `opr24` (BHV_OpRegionV24 flags) | `poly` on boundary breach, `recovering` beyond save_dist |
| `OPR_STATUS` | opr24 status (forwarded to the UI by iWebSocket) | Geofence status detail |
| `OBSTACLE_ALERT` | pObstacleMgr | Spawns one `avoid_obstacle_` behavior per charted danger in range |
| `BHV_WARNING`, `BHV_ERROR` | pHelmIvP behaviors | Behavior diagnostics (e.g. the MALCONFIG condition-parse class of failure) — bridged to `alarm` events on first occurrence |
| `NAV_*`, `DESIRED_*` | iM/simulation + helm + planchaPID | Navigation / autopilot (already bridged) |
| `WPT_UPDATE`, `RETURN_UPDATE`, `STATION_UPDATES`, `OPR_UPDATES` | iWebSocket | Behavior update channels (route, return point + speed, station point, region) |

## 2. Gaps found (pre-integration)

The bridge shipped with a *generic* variable convention. Against the Alpha
mission that meant:

| Gap | Symptom on a live boat |
|---|---|
| Bridge registered `WPT_DIST`; mission publishes `DIST_TO_NEXT_WPT` | "Distance to Waypoint" empty on Mission + Navigation dashboards |
| Bridge registered `MISSION_STATE`; nothing publishes it | Mission State stat + both state timelines (Mission, Replay) permanently empty |
| Bridge registered `ACTIVE_BEHAVIOR`; nothing publishes it | Autopilot "Active Behavior" empty (the data exists inside `IVPHELM_SUMMARY`) |
| Bridge registered `STATION_KEEP_STATUS`; mission uses `STATIONING` | "Station Keeping" stat empty |
| `MISSION_PROGRESS` / `WPT_TOTAL` / `WPT_ETA` / `DIST_TRAVELED` never published | Progress gauge, waypoint total, ETA, distance traveled empty |
| `COMMS_LOST`, `MISSION_ARMED`, `SURVEY_ENTRY_GATE` not subscribed | The comms-loss failsafe and the no-silent-resume re-arm hold — the most safety-relevant states in the mission — invisible to shore |
| `MOOS_MANUAL_OVERRIDE`, `IVPHELM_STATE`, `IVPHELM_ALLSTOP` not subscribed | Helm park / unexpected all-stop (`MissingDecVars:course` class of failure) invisible |
| `OPR_BREACHED` / `OPR_STATUS` not subscribed | Geofence activity invisible **and** the provisioned "Geofence Violation" alert queried `events` of type `geofence` that no producer ever wrote — the rule could never fire |
| `OBSTACLE_ALERT` not subscribed | Obstacle-avoidance spawns invisible |
| Signal K bridge wrote `navigation.courseRhumbline.nextPoint.distance` into `mission.dist_to_waypoint` | Would fight with the authoritative MOOS value once both flow (SK "next point" is whatever course the SK server holds, not the autonomy route) |

## 3. Design decisions

**Mission state is derived in `grafana_bridge`** (decision confirmed with
Swiftships): the mission publishes mode *flags*, not a state string, and the
bridge is the single consumer that needs the composite. The derivation lives
in `ingestion/moos-cpp/mission_logic.hpp` as pure C++ (no MOOS dependency)
with unit tests in `mission_logic_test.cpp`, so the state machine is testable
off-vessel and reusable.

Priority order (first match wins), mirroring the semantics documented in
`alpha.bhv`:

| # | Condition | State | Rationale |
|---|---|---|---|
| 1 | `COMMS_LOST` in its hold phase | `CommsLost` | Failsafe dominates; planchaPID station-keeps at the loss position, but shore must see *why* |
| 2 | `REMOTE_CONTROL` | `Remote` | Operator driving through the passthrough; it parks the helm underneath, so it must outrank Parked |
| 3 | Under way and (`MOOS_MANUAL_OVERRIDE` or no autonomy mode) | `Remote` | Operator driving from Swiftships-UI: the production iWebSocket posts DESIRED_THRUST / DESIRED_RUDDER straight from the console and no REMOTE_CONTROL. Under way = \|DESIRED_THRUST\| at or above `remote_thrust_pct` (1 %) or SOG at or above `remote_speed_kn` (1 kn; 0 = thrust only), held `remote_hold_s` (10 s). For 30 s after an autonomy mode ends only thrust counts (the boat coasts). Autonomy modes: WAYPOINT, RETURN, STATIONING, TRAILING, REGION, RETURN_HOLD, PAUSE |
| 4 | `MOOS_MANUAL_OVERRIDE` and `SURVEY_COMPLETE` | `Completed` | Post-survey park (iWebSocket parks once inside the region) |
| 5 | `MOOS_MANUAL_OVERRIDE` | `Parked` | Helm parked; thrust actively zeroed |
| 6 | `PAUSE` | `Paused` | Zero-speed hold (pause behaviors active) |
| 7 | `RETURN_HOLD` | `HoldingAtBase` | Holding station on the base after a comms-loss return |
| 8 | `STATIONING` | `StationKeeping` | Operator station-keep or mid-route station waypoint |
| 9 | `RETURN` | `ReturnToBase` | `waypt_return` driving to base |
| 10 | `TRAILING` | `Following` | Following an AIS contact (BHV_Trail, started from Swiftships-UI) |
| 11 | `REGION` | `RegionHold` | Operator region hold (BHV_OpRegionV24, started from Swiftships-UI) |
| 12 | `WAYPOINT` and gate `closed` | `EnteringRegion` | Start-outside latch: opr24 recovers the boat inside before the survey may run |
| 13 | `WAYPOINT` and not `MISSION_ARMED` | `AwaitingRearm` | Link back but no operator re-arm — the explicit no-silent-resume hold |
| 14 | `WAYPOINT` | `Autonomous` | Survey running |
| 15 | `SURVEY_COMPLETE` | `Completed` | Completed but not yet parked (e.g. recovering inside first) |
| 16 | otherwise | `Idle` | Boat at rest, no mission requested |

Helm health (`IVPHELM_STATE`, `IVPHELM_ALLSTOP`) is deliberately **not**
folded into the mission state: a frozen helm while `Paused` must show as
*Paused + ALLSTOP*, not mask one with the other. It gets its own fields,
panels and alert. The all-stop alert treats `ManualOverride` and
`NothingToDo` as benign reasons (normal park / idle) and anything else as
critical.

**Geofence** posts (`OPR_BREACHED` = `poly` | `recovering`) are edge events,
not a level signal, and BHV_OpRegionV24 posts no explicit "recovered". The
bridge therefore writes `autonomy.geofence_breached` = 1/2 on each post and
decays it to 0 after 30 s without a new post, and writes a `geofence` event
on every transition. The alert fires on the live numeric field (0 s pending),
so it fires during the breach and resolves itself after recovery.

**Obstacle alerts** decay the same way (120 s; `completed_dist=70` kills the
behavior without telling us), with an `obstacle` event per newly alerted
obstacle ID.

**Progress** = `WPT_INDEX` / waypoint count parsed from the `VIEW_SEGLIST`
labeled `waypt_survey`. On a `repeat` route the index cycles and so does
progress — that is the honest representation of a looping survey.
**ETA** = `DIST_TO_NEXT_WPT` / max(`NAV_SPEED`, 0.2 m/s) while a waypoint
behavior is active. **Distance traveled** integrates `NAV_X/Y` steps (jump
guard 100 m) and resets on survey start.

**Legacy compatibility:** the original generic variables (`WPT_DIST`,
`WPT_TOTAL`, `MISSION_PROGRESS`, `WPT_ETA`, `DIST_TRAVELED`,
`STATION_KEEP_STATUS`, `MISSION_NAME`, `MISSION_ID`) remain subscribed. If a
future app publishes one of the numeric ones, the bridge permanently yields
that field to the external producer (no dual writers). An externally
published `MISSION_STATE` is stored as `mission_state.state_external` and
never overwrites the derived state.

**Signal K next-point distance** moved to `nav.course_next_point` —
`mission.dist_to_waypoint` now has exactly one producer (MOOS).

## 4. New schema surface

Measurement `autonomy` (bucket `telemetry`, all numeric, 0/1 unless noted):
`deploy`, `waypoint`, `returning`, `stationing`, `pause`, `armed`,
`comms_lost`, `entry_gate_open`, `manual_override`, `survey_complete`,
`awaiting_rearm`, `helm_drive`, `allstop` (benign reasons excluded),
`geofence_breached` (0 ok / 1 breach / 2 recovering), `obstacle_alerts`
(count).

Measurement `mission_state` gains string fields: `helm_state`,
`allstop_reason`, `comms_failsafe` (`ok` or the failsafe action),
`entry_gate`, `opr_status`, `state_external`. `state` is now the derived
state above; `active_behavior` is parsed from `IVPHELM_SUMMARY`;
`station_keep` mirrors `STATIONING`.

Event `type` values gain `obstacle` (alongside `state_change`, `waypoint`,
`mission`, `alarm`, `geofence`).

## 5. Freeboard / Signal K UI relationship

The Freeboard UI (Signal K frontend of the VTMS stack) remains the *control*
surface: it draws the OP_REGION, sets the route/return/station points and
cruise speed, and its iWebSocket bridge owns the failsafe and re-arm latches.
The dashboard is the *observability* surface for the same mission: it renders
the states those controls produce, plus the deep telemetry Freeboard does not
show (helm internals, approach speed profile, events history, health,
replay). The two stay consistent because both read the same MOOS variables —
the dashboard adds no second source of truth and issues no commands
(read-only by design; the InfluxDB write path is outbound only).

Threshold styling on the Mission dashboard's approach panels intentionally
matches the governor tiers documented in `alpha.bhv` (full UI speed beyond
500 m, tier-1 zone 100–500 m, tier-2/capture zone below 100 m,
capture_radius 20 m), so an operator can see the deceleration staircase
against the same distances the helm is using.
