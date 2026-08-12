# Requirements Traceability Matrix

| Req | Delivered by | Status / notes |
|---|---|---|
| FR-1 Mission | `usv-mission.json` (14 panels); mission/mission_state measurements; events bucket timeline | Complete — MOOS variable names must be aligned with your helm config |
| FR-2 Navigation | `usv-navigation.json`; geomap live track; nav measurement | Complete |
| FR-3 Autopilot | `usv-autopilot.json`; all 5 required time-series + stat row | Complete — PID vars need pMarinePID publishing enabled |
| FR-4 Vessel Health | `usv-health.json` (engine, electrical, power rows) | Complete |
| FR-5 CAN Bus | `usv-canbus.json`; C++ SocketCAN bridge with per-PGN stats | Partial — fast-packet (multi-frame) PGNs counted, not decoded |
| FR-6 Radar | `usv-radar.json`; radar measurement schema | Dashboard complete — needs radar adapter to feed the schema |
| FR-7 Camera | `usv-camera.json`; camera measurement schema | Dashboard complete — needs camera manager to feed the schema |
| FR-8 Communication | `usv-comms.json`; comms measurement; Telegraf ping | Complete — starlink_up/vpn_up need small host probes (see review notes) |
| FR-9 System Health | `usv-system.json`; Telegraf host + systemd_units + net_response | Complete — Docker container panel replaced by systemd services (native deployment) |
| FR-10 Mission Replay | `usv-replay.json`; downsample task + 365 d archive | Complete — replay is time-range based (Grafana native), not a play button |
| FR-11 Alarms | 12 provisioned alert rules; `usv-alarms.json` alert list + history | Complete — geofence rule expects a geofence event producer |
| NFR Performance | 5 s default refresh (1–5 s selectable); batched writes | Complete |
| NFR Reliability | Auto-reconnect in all bridges; restart: unless-stopped; bounded buffers | Complete |
| NFR Scalability | `vessel` tag + dashboard variable everywhere | Complete |
| NFR Usability | Dark theme default; role mapping documented; responsive grid | Complete |
| NFR Data Storage | InfluxDB buckets w/ configurable retention; downsampling | Complete |
| Integration | MOOS-IvP (native C++ CMOOSApp bridge), Signal K, CAN/N2K (C++), InfluxDB, Grafana | Complete; MQTT optional — not wired (add a Telegraf mqtt_consumer input if needed) |
