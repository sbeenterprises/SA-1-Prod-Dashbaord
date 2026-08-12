# Operator User Guide

## Getting in
Browse to http://<host>:3000, log in, open folder **USV Monitoring**.
All dashboards default to dark theme and a 5 s refresh (change per dashboard
via the refresh picker, 1–5 s supported). The **Vessel** dropdown selects
the vessel (fleet-ready).

## Dashboards
| Dashboard | What you watch |
|---|---|
| Mission | Name/ID, state, progress, waypoints, ETA, event timeline |
| Navigation | Live map + track, GPS quality, SOG/COG/heading, attitude, XTE |
| Autopilot (MOOS-IvP) | Desired vs actual heading/speed, rudder/throttle, PID errors, active behavior |
| Vessel Health | Engine (RPM, temps, oil, fuel), electrical (V/A/SOC), power |
| CAN Bus | Utilization, msgs/s, error/dropped frames, PGN table |
| Radar | Online status, targets, closest range, CPA/TCPA |
| Cameras | Connection, stream, FPS, bitrate, recording, PTZ |
| Communications | Internet/Starlink/VPN/Signal K status, latency, loss, bandwidth |
| System Health (OBC) | CPU/RAM/disk/temp, service reachability, systemd services |
| Mission Replay | Historical playback: set the time range, scrub with crosshair |
| Alarms | Active alerts + alarm history + at-a-glance status wall |

## Mission replay
1. Open **USV / Mission Replay**.
2. Pick the mission window with the time picker (annotations mark waypoint
   hits and state changes).
3. Hover any graph — the crosshair is shared, and the map shows the track
   for the selected window. Missions older than 30 days come from the
   1-minute archive.

## Alarms
Active alerts appear on the Alarms dashboard and are emailed to the
operations contact point. Severity `critical` repeats every 10 min until
resolved; `warning` hourly. Alarm history is kept 1 year in the events
bucket.

## Tips
- `?kiosk` in the URL gives a clean wall-display view.
- Time ranges and the vessel variable are sharable via the URL.
- Panels can be viewed full screen with `v` while hovering.
