# Requirements Traceability Matrix

| Req | Delivered by | Status / notes |
|---|---|---|
| FR-1 Mission | `usv-mission.json` (single-screen status wall + readouts); mission/mission_state/autonomy measurements; derived Alpha state machine incl. Remote (unit-tested); route name from Signal K; events bucket timeline | Complete — variable map aligned with alpha.bhv (see docs/alpha-mission-integration.md) |
| FR-2 Navigation | `usv-navigation.json`; geomap live track; nav measurement | Complete |
| FR-3 Autopilot | `usv-autopilot.json`; helm/safety tiles (helm state, all-stop, station keeping, entry gate, re-arm hold, remote control, corridor, crash stop), command readouts, instruments, desired vs actual, speed governor; planchaPID outputs mapped (SPEED_CAP, CMD_SPEED, XTE_NOW, NEXT_TURN_ANGLE) | Complete |
| FR-4 Vessel Health | `usv-health.json` (engine, electrical, power rows) | Complete |
| FR-5 CAN Bus | `usv-canbus.json`; C++ SocketCAN bridge with per-PGN stats | Partial — fast-packet (multi-frame) PGNs counted, not decoded |
| FR-6 Radar | `usv-radar.json`; radar measurement schema | Dashboard complete — needs radar adapter to feed the schema |
| FR-7 Camera | `usv-camera.json`; camera measurement schema | Dashboard complete — needs camera manager to feed the schema |
| FR-8 Communication | `usv-comms.json`; comms measurement; Telegraf ping | Complete — starlink_up/vpn_up need small host probes (see review notes) |
| FR-9 System Health | `usv-system.json`; Telegraf host (CPU/GPU/RAM/disk/temp/load) + systemd_units + net_response | Complete — GPU via Telegraf nvidia_smi (enable on NVIDIA hosts); Docker container panel replaced by systemd services |
| FR-10 Mission Replay | `usv-replay.json`; downsample task + 365 d archive | Complete — replay is time-range based (Grafana native), not a play button |
| FR-11 Alarms | 17 provisioned alert rules (`gen_alerts.py --profile sim|vessel`); `usv-alarms.json` alert list + history + autonomy status row | Complete — geofence/comms-failsafe/all-stop/re-arm rules fire on live autonomy fields fed by grafana_bridge from alpha.bhv |
| NFR Performance | 5 s default refresh (1–5 s selectable); batched writes | Complete |
| NFR Reliability | Auto-reconnect in all bridges; restart: unless-stopped; bounded buffers | Complete |
| NFR Scalability | `vessel` tag + dashboard variable everywhere; Fleet dashboard; one signalk_bridge instance per vessel | Complete |
| NFR Usability | Single-screen layouts, equal status tiles, nautical units, Swiftships branding, dark and light themes; role mapping documented | Complete |
| NFR Data Storage | InfluxDB buckets w/ configurable retention; downsampling | Complete |
| Integration | MOOS-IvP (native C++ CMOOSApp bridge), Signal K, CAN/N2K (C++), InfluxDB, Grafana | Complete; MQTT optional — not wired (add a Telegraf mqtt_consumer input if needed) |
