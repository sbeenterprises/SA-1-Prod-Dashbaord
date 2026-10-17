# Requirements Traceability Matrix

| Req | Delivered by | Status / notes |
|---|---|---|
| FR-1 Mission | `usv-mission.json` (single-screen status wall + readouts); mission/mission_state/autonomy measurements; derived Alpha state machine incl. Remote (unit-tested); route name from Signal K; events bucket timeline | Complete — variable map aligned with alpha.bhv (see docs/alpha-mission-integration.md) |
| FR-2 Navigation | `usv-navigation.json`; geomap live track; nav measurement | Complete |
| FR-3 Autopilot | `usv-autopilot.json`; helm/safety tiles (helm state, all-stop, station keeping, entry gate, re-arm hold, remote control, corridor, crash stop), command readouts, instruments, desired vs actual, speed governor; planchaPID outputs mapped (SPEED_CAP, CMD_SPEED, XTE_NOW, NEXT_TURN_ANGLE) | Complete |
| FR-4 Vessel Health | `usv-health.json` (engine, electrical, power rows) | Complete |
| FR-5 CAN Bus | `usv-canbus.json`; C++ SocketCAN bridge with per-PGN stats | Partial — fast-packet (multi-frame) PGNs counted, not decoded |
| FR-6 Radar / Traffic | `usv-traffic.json`: polar traffic view, risk and avoidance tiles, highest-risk contact, MOOS action, contact table; contacts/traffic measurements with onboard CPA/TCPA, encounter and risk; `usv-collision-risk` alert | Complete in simulation (pContactSpawn contacts); on the vessel radar targets feed the same schema through RADAR_TARGET (`mayara_bridge` from mayara-server) |
| FR-7 Camera | `usv-camera.json` (Frigate up, camera counts, per-camera table, frame-rate trend); camera / camera_summary measurements from `frigate_bridge` (Frigate NVR API); Readiness "N / M cameras streaming" | Complete for connection, stream, fps and recording; bitrate, temperature and PTZ need a camera-API adapter |
| FR-8 Communication | `usv-comms.json`; comms measurement; Telegraf ping | Complete — Internet/Starlink are Telegraf ICMP probes; VPN tile watches Twingate / Tailscale / OpenVPN / WireGuard units (N/A where the VPN terminates on the router) |
| FR-9 System Health | `usv-system.json`; Telegraf host (CPU/GPU/RAM/disk/temp/load) + systemd_units + net_response | Complete — GPU via measurement `gpu`: usv_gpu_probe (intel_gpu_top) for Intel Arc/Xe, nvidia_smi with name_override on NVIDIA hosts; Docker container panel replaced by systemd services |
| FR-10 Mission Replay | `usv-replay.json`; downsample task + 365 d archive | Complete — replay is time-range based (Grafana native), not a play button |
| FR-11 Alarms | 20 provisioned alert rules (`gen_alerts.py --profile sim|vessel`); `usv-alarms.json` alert list + history + autonomy status row | Complete — geofence/comms-failsafe/all-stop/re-arm rules fire on live autonomy fields fed by grafana_bridge from alpha.bhv |
| FR-12 Pre-departure readiness | `usv-readiness.json`: verdict, counts, automatic checks table with evidence, console checklist mirror; `readiness` / `readiness_summary` measurements decided onboard by `usv::EvaluateReadiness` (17 checks incl. steering, CAN bus, MOOS apps, endurance in litres, speed cap against the hull maximum, route inside region, DTCs; vessel particulars card from VESSEL_* in usv.env) from bridge fields plus values polled from InfluxDB; console `CHECKLIST_STATUS` / `ENC_STATUS` mirrors; `readiness` change events | Complete in simulation (sim profile: engine / CAN / cameras / rudder feedback N/A); the vessel profile needs the console to publish the two variables |
| NFR Performance | 5 s default refresh (1–5 s selectable); batched writes | Complete |
| NFR Reliability | Auto-reconnect in all bridges; restart: unless-stopped; bounded buffers | Complete |
| NFR Scalability | `vessel` tag + dashboard variable everywhere; Fleet dashboard; one signalk_bridge instance per vessel | Complete |
| NFR Usability | Single-screen layouts, equal status tiles, nautical units, Swiftships branding, dark and light themes; role mapping documented | Complete |
| NFR Data Storage | InfluxDB buckets w/ configurable retention; downsampling | Complete |
| Integration | MOOS-IvP (native C++ CMOOSApp bridge), Signal K, CAN/N2K (C++), InfluxDB, Grafana | Complete; MQTT optional — not wired (add a Telegraf mqtt_consumer input if needed) |
