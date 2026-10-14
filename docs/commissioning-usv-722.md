# Commissioning record — usv-722, 2026-10-01

Production installation of the SA-1 Dashboard on the Swiftships USV
`usv-722`, alongside at Morgan City, LA. Boat systems off except the
onboard computer, Signal K and the VectorNav INS. Follows the checklist in
`docs/DEPLOYMENT-NOTE.md` section 6.

## Hardware and software found

| Item | Found |
|---|---|
| OBC | OnLogic K800, Intel Arc Pro A60 (PCI 56b3, i915), Ubuntu 22.04.5, 938 GB NVMe, NTP synced |
| Autonomy | MOOS-IvP at `~/Documents/automation/moos-ivp`, mission repo `moos-ivp-morgan-stern-drive-mercury` (the name is historical; this hull's engine is a Yamaha outboard, the Mercury diesel sterndrive is usv-725), production mission `missions/s1_morgan_city_signalk/real.moos` (community `alpha`, 14 apps incl. iCAN, iCANRec, iWebSocket, iRadarTargets, pContactMgrV20) |
| Signal K | 2.13.5 on :3000; position, heading, attitude from the VectorNav via the `signalk-vectornav-vnins` plugin; no GNSS satellite count or HDOP |
| CAN | `slcan0` on the K800 eMCU, `slcand -o -c -F -s5` at boot; interface UP, 0 frames (backbone off / wiring to confirm) |
| Network | Starlink dish reachable at 192.168.100.1; OpenVPN terminates on the boat router (no tunnel interface on the OBC); ufw inactive |
| Console | Swiftships UI (Freeboard-SK fork) talking to MOOS through iWebSocket :8000; checklist panel local only; no ENC |

## Releases produced during commissioning

Each one found on the boat, fixed in the prod repo, tagged and installed
the same day. All from `sbeenterprises/SA-1-Prod-Dashbaord`.

| Tag | Change | Trigger |
|---|---|---|
| v1.2.1 | Vessel alert profile, SMTP support, example mission in vessel profile, plugin pin, operator contact point mali@swiftships.com | Repo was committed in simulation configuration |
| v1.2.2 | GPS readiness from Signal K position + VectorNav INS mode | GPS row FAIL "no position" while the map showed the boat |
| v1.2.3 | Telegraf stat tiles collapse to the latest sample; CPU Temp hottest core; Starlink probe on | MOOS tile DOWN+UP at once; acpitz -273 C |
| v1.2.4 / v1.2.5 | Intel GPU probe (intel_gpu_top, measurement `gpu`), hang fix | Arc GPU: tile No data; probe never returned under sudo |
| v1.2.6 | No-data colours neutral, meters N/A, WP bearing sentinel, services table, bandwidth filter, COG only under way, 20 alert rules (position-based GPS loss, INS and satellites rules, engine/battery not alerting on no data) | Dockside walk-through of all 13 dashboards |
| v1.2.7 | `enc_fitted` / `console_checklist` bridge options | CONSOLE rows held the verdict at NOT READY permanently |
| v1.2.8 | Background tiles read N/A; checklist panel line | Grafana's No data placeholder painted empty tiles as healthy |
| v1.2.9 | Documentation: this record, runbook, schema/comms/GPU/alert text | |
| v1.2.10 | AIS contacts positioned from LAT/LON on the mission datum; own ship on the same datum; datum-mismatch warnings | Traffic tab read ~1,794 nm for every AIS contact |
| v1.2.11 | mayara_bridge: mayara-server ARPA targets posted as RADAR_TARGET for the Traffic tab (display only), usv-mayara-bridge.service | Halo radar targets tracked by mayara did not reach the dashboard |
| v1.2.12 | Battery from any Signal K instance (House), readiness reads Signal K battery/engine, own ship from Signal K while MOOS NAV_* is 0, frigate_bridge camera status and per-camera Cameras page, short radar ids and CPA gate | Power panels and Electrical "no data"; AIS contacts ~3,300 km away (MOOS own ship at 0N 0E); Cameras "no camera data" |

## Checklist results

| # | Check | Result |
|---|---|---|
| 1 | Services up | PASS: influxdb, grafana-server, telegraf, usv-signalk-bridge, usv-can-bridge active |
| 2 | DB reachable | PASS: `influx ping` OK |
| 3 | Bench data | Skipped on the vessel (`usv-simulator` disabled, as required) |
| 4 | Grafana login | PASS: Swiftships login, Readiness home, 13 dashboards, "SA-1 Dashboard" in the menu, port 3100 |
| 5 | Tab navigation | PASS |
| 6 | Theme toggle | PASS (dark and light verified) |
| 7 | Tablet check | Not performed |
| 8 | Alert rules loaded | PASS: 20 rules (v1.2.6), no false firing alongside (before v1.2.6 three rules fired on missing data for 1.5 h) |
| 9 | Stop simulator | n/a (never started) |
| 10 | MOOS feed | PASS: grafana_bridge under pAntler, MOOS 4-5 msg/s, helm PARK, 14/14 apps |
| 10a | Readiness evaluation | PASS: 16 rows with evidence, vessel profile |
| 10b | Console mirrors | N/A by configuration (`enc_fitted = false`, `console_checklist = false`) |
| 10c | Route plausibility | Pending: no mission with a route started yet |
| 11 | Signal K feed | PASS for position, heading, attitude (INS tracking); engine/battery pending (boat off) |
| 12 | CAN feed | Pending: interface UP, 0 msg/s; backbone power / wiring / termination to check with the bus live |
| 13 | Alert fire test | Pending (and email needs SMTP in usv.env) |
| 14 | Reboot test | Pending; mission is launched by hand (no autostart unit) |

Readiness verdict alongside: NOT READY, 7 PASS / 6 FAIL / 3 N/A / 0
CONSOLE. The six FAILs are boat inputs: route, steering feedback, engine,
electrical, AIS/radar, cameras.

## Findings worth keeping

- Two `pAntler real.moos` launches on one host attach duplicate apps to
  the first MOOSDB (port 9000 already bound); the dashboard then shows
  stale readiness rows from the old bridge. Check `pgrep -a MOOSDB` before
  launching (runbook).
- The INS COG is meaningless at rest; the bridge now drops it below 0.5 kn.
- `iDivisorNMEA` publishes the rudder angle as NAV_YAW on this hull;
  `ignore_vars = NAV_YAW` is required in the bridge block.
- `intel_gpu_top` must run under the same uid as its `timeout`, otherwise
  an unprivileged timeout cannot stop it and the probe hangs.

## Open items (owner)

1. Helm position: confirm pHelmIvP receives NAV_X/NAV_Y on this hull
   (`uQueryDB real.moos --condition="NAV_X > -999999"`) — autonomy.
2. Operating region: `BHV_OpRegion` with a polygon in the `.bhv` — autonomy.
3. CAN: frames on `slcan0` with the N2K backbone powered — electrical.
4. SMTP credentials in `/etc/usv/usv.env`, then the alert fire test — ops.
5. VPN tile: tunnel address the OBC can ping — network.
6. Reboot test and a mission autostart unit — ops/autonomy.
7. ENC (planned), console checklist publication (if ever wanted) — product.
8. MOOS own-ship NAV_* = 0: iDivisorNMEA valid-RMC/HDT fix and Signal K
   RMC/HDT output (note handed to the developer) — autonomy.
9. Cameras 3 to 6 and FLIR: PoE switch / network, then enable in Frigate — electrical.
