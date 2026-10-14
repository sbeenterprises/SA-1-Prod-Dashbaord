# USV-725 Dashboard Requirements

Engine telemetry for the Mercury diesel from J1939 through MOOS, same dashboard release as USV-722

Status: draft 2 for review (draft 1 assumed Signal K as the engine source; that was wrong)
Date: 2026-10-13
Owner: mali
Applies to: SA-1-Dashboard-prod, next release after v1.2.19, and iCANRec in the MOOS mission repo

## 1. Purpose

Deploy the USV dashboard (Grafana, InfluxDB, Telegraf and the C++ bridges) on USV-725 so that every page works as on USV-722 and the Vessel Health engine gauges show live, correct values from the Mercury diesel.

On USV-725 the engine broadcasts standard SAE J1939 PGNs on the drive CAN bus (`slcan0`). The MOOS app `iCANRec` decodes them into `ENGINE_*` variables, `iWebSocket` forwards them to the Swiftships UI, and the dashboard's MOOS bridge (`grafana_bridge`) already maps them into the InfluxDB `engine` measurement. The dashboard therefore reads the engine through MOOS on this hull. Signal K is not an engine source on USV-725.

USV-722 (Yamaha) is waiting for the Yamaha Command Link Plus / Command Link NMEA 2000 gateway kit (MAR-GTWAY-KT-20). Its engine data will arrive through Signal K. The configuration layer in this document lets the same release serve both hulls by changing one setting.

## 2. Hull facts

| Item | USV-725 | USV-722 |
| --- | --- | --- |
| Engine | Mercury diesel sterndrive, turbocharged. Sea-trial logs of 29 May 2026 show idle 450 to 640 rpm, maximum 3315 rpm, boost 102 to 286 kPa. [Model and rated rpm to be filled from the engine plate] | Yamaha outboard |
| Engine count | 1 (one `ENGINE_*` stream in the logs) [confirm] | [to confirm] |
| Engine network | J1939 on `slcan0`, shared with the drive commands | Yamaha network; NMEA 2000 gateway on order |
| Engine data path today | J1939 → iCANRec → MOOS `ENGINE_*` → iWebSocket → Swiftships UI | none yet |
| Dashboard installed | No | v1.2.19 |
| Helm frames on the drive bus | 18F30A1A steering and 18F005D0 gear sent by iCAN; receive side to confirm with a capture | Glendinning helm frames, decoded by `usv722_can.hpp` |

J1939 traffic verified from the iCANRec appcast and `ENGINE_*` values in the four sea-trial logs (`missions/s1_morgan_city_signalk/MOOSLog_29_5_2026_*`):

| PGN | Name | Rate on the bus | Values recorded |
| --- | --- | --- | --- |
| 61444 | EEC1, engine speed | 100 Hz | 446 to 3315 rpm |
| 61443 | EEC2, percent load | 20 Hz | 0 to 100 % |
| 65266 | LFE, fuel rate | 10 Hz | 0 to 53 L/h |
| 65263 | EFL/P1, oil pressure | 2 Hz | 240 to 640 kPa |
| 65270 | IC1, intake manifold pressure | 2 Hz | 102 to 286 kPa |
| 65262 | ET1, coolant and oil temperature | 1 Hz | coolant 77 to 83 °C; oil temperature decoded wrongly (sweeps −40 to 212 °C) |
| 65271 | VEP1, battery potential | 1 Hz | decoded wrongly (0.05 to 3264 V) |
| 65226 | DM1, active faults | 1 Hz | 0 faults |

Not seen because iCANRec does not decode them: 65253 engine hours, 65276 fuel level. Whether the engine sends them is unknown until a raw capture is taken.

## 3. Scope

In scope

- Install the dashboard stack on the USV-725 onboard computer with the standard installer.
- Engine values from MOOS `ENGINE_*` into the InfluxDB `engine` measurement: RPM, coolant temperature, oil temperature, oil pressure, fuel rate, engine load, torque, intake manifold pressure, active faults, and, if the engine sends them, engine hours and fuel level.
- Corrections to iCANRec so that every published value is right.
- Per-vessel engine configuration (engine type, source, engine count, instances, helm profile) so one release serves both hulls.
- Dashboard pages, alert rules and readiness checks reading the fields, with gauge ranges for a 3300 rpm diesel.
- Signal K bridge changes prepared for USV-722, tested with recorded deltas, not gating USV-725 acceptance.
- Commissioning and runbook documents for USV-725.

Out of scope

- Decoding Mercury SmartCraft.
- A second dashboard or a Mercury-specific page.
- The USV-722 Yamaha gateway installation (separate task when the kit arrives).
- Changes to the Swiftships UI engine panel (its own defects are listed in section 9 for the UI owner).

## 4. Functional requirements

### 4.1 Engine data source

| ID | Requirement |
| --- | --- |
| DS-1 | MOOS is the only engine data source on USV-725. `grafana_bridge` writes every engine field from the `ENGINE_*` variables published by `iCANRec`. |
| DS-2 | `grafana_bridge` runs on USV-725 as a MOOS app (`Run = grafana_bridge` in the mission file) with `vessel_id = usv-725`, `engine_rate_hz = 2` and `readiness_profile = vessel`. |
| DS-3 | The Signal K bridge does not write engine fields on USV-725. Its engine path mapping is switched off by configuration (`SIGNALK_ENGINE=0` in the service environment), not by removing code. |
| DS-4 | The CAN bridge does not write engine RPM or fuel level from NMEA 2000 PGNs 127488 and 127505 on USV-725. The decode is switched off by configuration (`CAN_N2K_ENGINE=0`), not by removing code. |
| DS-5 | Exactly one series exists in InfluxDB per engine field per engine instance per vessel. A query of `engine.rpm` for `vessel=usv-725` grouped by `instance` returns one series, tagged `instance=0`, and no untagged series. |
| DS-6 | `grafana_bridge` limits each engine field to 2 writes per second (EEC1 arrives at 100 Hz) and still writes a steady value at least once every 5 s so a flat value reads as live. |

### 4.2 MOOS variable mapping

| ID | MOOS variable | J1939 source | InfluxDB field | Unit | Status in the code today |
| --- | --- | --- | --- | --- | --- |
| DM-1 | `ENGINE_SPEED` | PGN 61444 EEC1, SPN 190, bytes 4-5, 0.125 rpm/bit | `engine.rpm` | rpm | Correct |
| DM-2 | `ENGINE_COOLANT_TEMP` | PGN 65262 ET1, SPN 110, byte 1, 1 °C/bit, −40 offset | `engine.coolant_temp` | °C | Correct |
| DM-3 | `ENGINE_OIL_TEMP` | PGN 65262 ET1, SPN 175, bytes 3-4, 0.03125 °C/bit, −273 offset | `engine.oil_temp` | °C | Wrong: read as one byte with −40 offset (REC-1) |
| DM-4 | `ENGINE_OIL_PRESSURE` | PGN 65263 EFL/P1, SPN 100, byte 4, 4 kPa/bit | `engine.oil_pressure` | kPa | Correct |
| DM-5 | `ENGINE_FUEL_RATE` | PGN 65266 LFE, SPN 183, bytes 1-2, 0.05 L/h per bit | `engine.fuel_rate` | L/h | Correct |
| DM-6 | `ENGINE_PERCENT_LOAD` | PGN 61443 EEC2, SPN 92, byte 3, 1 %/bit | `engine.percent_load` | % | Correct |
| DM-7 | `ENGINE_PERCENT_TORQUE` | PGN 61444 EEC1, SPN 513 actual torque, byte 3, −125 offset | `engine.percent_torque` | % | Wrong byte: reads byte 2, SPN 512 driver's demand (REC-2) |
| DM-8 | `ENGINE_ACCELERATOR_PEDAL_POSITION` | PGN 61443 EEC2, SPN 91, byte 2, 0.4 %/bit | `engine.pedal_position` | % | Correct; the engine sends not-available, so nothing is published |
| DM-9 | `ENGINE_INTAKE_MANIFOLD_PRESSURE` | PGN 65270 IC1, SPN 102, byte 2, 2 kPa/bit | `engine.intake_manifold_pressure` | kPa | Correct |
| DM-10 | `ENGINE_BATTERY_VOLTAGE` | PGN 65271 VEP1, SPN 168, bytes 5-6, 0.05 V/bit | `electrical.battery_voltage` and `power.system_voltage` | V | Wrong bytes: reads bytes 6-7 (REC-3). On USV-725 the house battery comes from the Victron through Signal K, so this variable is mapped to a new field `engine.ecu_voltage` instead (DM-13) |
| DM-11 | `ENGINE_ACTIVE_DTCS` | PGN 65226 DM1 | `engine.active_dtcs` | count | Partly: single frame only, faults never cleared (REC-4, REC-5) |
| DM-12 | `ENGINE_HOURS` | PGN 65253 HOURS, SPN 247, bytes 1-4, 0.05 h/bit | `engine.runtime_h` | h | Not decoded (REC-6); only if the engine sends the PGN |
| DM-13 | `ENGINE_FUEL_LEVEL` | PGN 65276 DD, SPN 96, byte 2, 0.4 %/bit | `engine.fuel_level` | % | Not decoded (REC-6); only if the engine or a tank sender sends the PGN |
| DM-14 | `ENGINE_DTC_TEXT` | PGN 65226 DM1, every active fault as "SPN n FMI m (xk)" joined with "; ", "NONE" when empty | `engine.dtc_text` | text | New (REC-5) |
| DM-15 | `ENGINE_LAMPS` | PGN 65226 DM1, byte 1 lamp status | `engine.fault_lamps`, `engine.lamp_red`, `engine.lamp_amber`, `engine.lamp_protect`, `engine.lamp_mil` | 0/1 | New (REC-5) |

Rules

| ID | Requirement |
| --- | --- |
| DM-16 | A J1939 field whose raw value is the not-available code (0xFF, 0xFFFF, 0xFFFFFFFF) or the error code (0xFE, 0xFEFF) is not published. The gauge then reads N/A. No placeholder value is ever published. |
| DM-17 | Every engine point carries the tags `vessel`, `instance` and `engine_type`. `grafana_bridge` takes the instance from the J1939 source address of the frame (REC-7), 0 for a single engine. |
| DM-18 | `ENGINE_BATTERY_VOLTAGE` no longer feeds `electrical.battery_voltage` or `power.system_voltage`; both come from the Victron shunt through the Signal K bridge, as on USV-722. |

### 4.3 iCANRec corrections (MOOS mission repo, `src/iCANRec/CANRec.cpp`)

| ID | Requirement | Evidence |
| --- | --- | --- |
| REC-1 | Oil temperature (SPN 175) decoded from bytes 3-4 as a 16-bit little-endian value, 0.03125 °C/bit, −273 °C offset, not published when 0xFFFF. | Logged values sweep −40 to 212 °C while coolant stays at 77 to 83 °C |
| REC-2 | Actual engine torque (SPN 513) decoded from EEC1 byte 3, −125 offset. Driver's demand (SPN 512, byte 2) published separately as `ENGINE_DEMAND_TORQUE` if available. | Code reads byte 2 and labels it SPN 513 |
| REC-3 | Battery potential (SPN 168) decoded from VEP1 bytes 5-6, 0.05 V/bit. | Logged values 0.05 to 3264 V; appcast showed 76.85 V |
| REC-4 | DM1: the active fault list is rebuilt from every DM1 message; a fault absent from the latest message is cleared; a DM1 with SPN 0 means no faults. | Code only ever adds to the list |
| REC-5 | DM1 with two or more faults arrives as a J1939 BAM transport (TP.CM 0xECFF + TP.DT 0xEBFF, PGN 65226 inside) and is reassembled. Lamp byte and per-fault SPN, FMI and occurrence count are published (DM-14, DM-15). The reassembly already exists in the dashboard's `usv722_can.hpp` (`Dm1Receiver`) and is reused. | Single-frame DM1 holds at most one fault |
| REC-6 | PGN 65253 (engine hours) and PGN 65276 (fuel level) are decoded when present. | Not decoded today |
| REC-7 | The J1939 source address of each frame is kept; `ENGINE_*` variables carry the engine instance when more than one source address publishes EEC1 (`ENGINE_SPEED` for instance 0, `ENGINE_SPEED_1` for instance 1). A single engine keeps the current names. | Source address is discarded in `extractPGN` |
| REC-8 | Every decode has a unit test with a recorded or hand-built frame, including the not-available codes, in the MOOS repo build. | No tests exist |
| REC-9 | The iCANRec and iCAN source committed in the mission repo is the source running on USV-725. The 29 May 2026 logs show an iCAN with thrust slew limiting ("ramped, slew 20 %/s") that is in neither the repo nor the PC copy; that build is recovered from the boat and committed before any iCANRec change is deployed. | Log appcast vs repo |

### 4.4 Per-vessel engine configuration

| ID | Requirement |
| --- | --- |
| CFG-1 | `/etc/usv/usv.env` holds the engine profile. New keys: `ENGINE_TYPE`, `ENGINE_SOURCE`, `ENGINE_COUNT`, `ENGINE_INSTANCES`, `HELM_PROFILE`, `SIGNALK_ENGINE`, `CAN_N2K_ENGINE`, `SIGNALK_FUEL_TANK`. `deploy/usv.env.example` documents each key with its allowed values. |
| CFG-2 | USV-725 values: `VESSEL_ID=usv-725`, `ENGINE_TYPE=mercury_diesel`, `ENGINE_SOURCE=moos`, `ENGINE_COUNT=1`, `ENGINE_INSTANCES=0`, `HELM_PROFILE=[usv722, usv725 or none, after the capture]`, `SIGNALK_ENGINE=0`, `CAN_N2K_ENGINE=0`. |
| CFG-3 | USV-722 values after the Yamaha gateway: `VESSEL_ID=usv-722`, `ENGINE_TYPE=yamaha_outboard`, `ENGINE_SOURCE=signalk`, `HELM_PROFILE=usv722`, `SIGNALK_ENGINE=1`, `CAN_N2K_ENGINE=0`. Until then `SIGNALK_ENGINE=0` and the gauges read N/A. |
| CFG-4 | `install.sh` reads the keys and passes them to the services it enables. `ENGINE_SOURCE` decides which producer writes engine fields; the installer never enables two. For `moos` the installer prints the `grafana_bridge` block to add to the mission file. |
| CFG-5 | `ENGINE_TYPE` is written as the `engine_type` tag by every bridge that writes engine fields and shown on the dashboards (UI-1). |
| CFG-6 | The dashboard generator keeps a table of gauge ranges per `ENGINE_TYPE`. `mercury_diesel`: tachometer 0 to [RPM_MAX, 4000 until the plate is read], green to [3000], yellow to [3400], red above; coolant green below 95 °C, yellow to 105 °C, red above; oil pressure red below 150 kPa, yellow to 200 kPa; intake manifold pressure 0 to 300 kPa. `yamaha_outboard`: tachometer 0 to 6000, green to 4500, yellow to 5500, red above; coolant green below 95 °C, yellow to 105 °C, red above; oil pressure red below 150 kPa. Values in brackets are placeholders taken from the sea-trial logs and are replaced from the engine manual before the release. |
| CFG-7 | Changing `ENGINE_TYPE` in `usv.env` and re-running the installer is the only step needed to switch a hull between range sets. No dashboard JSON is edited by hand. |

### 4.5 Dashboard pages

| ID | Requirement |
| --- | --- |
| UI-1 | Every dashboard header band shows the engine type and source next to the vessel name: `Vessel usv-725 · MERCURY DIESEL · J1939 via MOOS`. |
| UI-2 | Vessel Health keeps its layout: six dials (Tachometer, Coolant, Oil Pressure, Fuel, Battery, State of Charge), eight tiles (Fuel Rate, Engine Hours, Oil Temp, Engine Load, Battery Current, Charging, Alternator, Total Power), six trends. Dial scales and colour bands come from CFG-6. |
| UI-3 | A dial or tile whose field has no data in the selected time range reads N/A in the neutral colour, never 0 and never red. On USV-725 this applies to Engine Hours and Fuel until REC-6 confirms the PGNs exist, and to Alternator. |
| UI-4 | The Engine Load tile shows `percent_load`; a Boost tile (intake manifold pressure, kPa) and a Torque tile (actual torque, %) are added to the secondary row on diesel hulls, replacing nothing on outboard hulls (they read N/A there). |
| UI-5 | On a two-engine hull (`ENGINE_COUNT=2`) the Tachometer, Coolant and Oil Pressure dials show two needles (port in white, starboard in orange) and the value line reads `P <value> · S <value>`; trends show one line per engine. On a one-engine hull the page is unchanged. |
| UI-6 | The Engine Faults tiles (Autopilot Live, CAN Bus) show the active fault count, the lamp state and the fault text from DM-11, DM-14 and DM-15 whichever bridge wrote them. |
| UI-7 | The Gear, Engine Thrust, Key Switch and Trim tiles read their fields regardless of which bridge wrote them. Fields without a producer on USV-725 read N/A. |
| UI-8 | The Fleet page shows an Engine column (engine type) and an engine status column (last RPM, or N/A). |
| UI-9 | Refresh rates stay as released: Autopilot, Autopilot Live and Traffic at 1 s, the other pages at 5 s, Grafana `min_refresh_interval` 1 s. |
| UI-10 | The User Guide describes the engine chip, the diesel tiles, the two-needle dials and the N/A rule. |

### 4.6 Alerts and readiness

| ID | Requirement |
| --- | --- |
| AL-1 | The Engine Over Temperature rule uses the coolant alarm value of the hull's `ENGINE_TYPE` (CFG-6). |
| AL-2 | A Low Oil Pressure rule fires when `engine.oil_pressure` is below the red band of the hull's `ENGINE_TYPE` while `engine.rpm` is above idle (above 700 rpm), severity critical. |
| AL-3 | An Engine Fault rule fires when `engine.active_dtcs` is above 0 or `engine.lamp_red` is 1, severity critical, with `engine.dtc_text` in the alert message. |
| AL-4 | The Low Fuel rule stays at 15 % and applies only when `engine.fuel_level` exists. |
| AL-5 | Engine rules do not fire on missing data. A boat alongside with the engine off raises no engine alert. |
| AL-6 | The Readiness "Engine pre-start" check passes when coolant temperature, oil pressure and active faults are present and within limits. Fuel level is included only when it exists. Key switch, gear and throttle evidence is included when a helm decoder exists on the hull and omitted otherwise, without failing the check. |
| AL-7 | The Readiness "Endurance" check uses `engine.fuel_rate` with the tank and consumption constants set for USV-725 in the `grafana_bridge` block (`fuel_tank_l`, `cruise_fuel_lph`, `cruise_speed_kn`), and `engine.fuel_level` when it exists. |

### 4.7 CAN bus on USV-725

| ID | Requirement |
| --- | --- |
| CAN-1 | `can_bridge` runs on USV-725 on `slcan0` for bus statistics (interface state, message rate, utilisation, per-PGN counts). The CAN Bus page shows the J1939 engine PGNs in its PGN table. |
| CAN-2 | `HELM_PROFILE` selects the helm decoder: `usv722` for the Glendinning frames known today, `usv725` for a decoder written from a USV-725 capture, `none` for no helm decode. With `none` the gear, throttle, key and trim switch tiles read N/A. |
| CAN-3 | `can_bridge` and `iCANRec` both read `slcan0`. Two SocketCAN readers on one interface are allowed; neither filters frames for the other. The J1939 DM1 decode in `can_bridge` is disabled when `ENGINE_SOURCE=moos` so that the fault fields have one writer (DS-5). |
| CAN-4 | A USV-725 helm decoder, if written, has the same unit-test harness as the USV-722 decoder, with test vectors taken from the USV-725 capture. |

### 4.8 Signal K bridge, prepared for USV-722

| ID | Requirement |
| --- | --- |
| SK-1 | When `SIGNALK_ENGINE=1`, the Signal K bridge reads `propulsion.<id>.*` by prefix for every engine id Signal K publishes (`0`, `1`, `port`, `starboard`), normalises `0`/`port` to instance 0 and `1`/`starboard` to instance 1, and writes `instance=<n>` and `engine_type` on every engine point. |
| SK-2 | Paths read and conversions: `revolutions` Hz × 60 → `rpm`; `temperature` and `coolantTemperature` K − 273.15 → `coolant_temp`; `oilPressure` Pa ÷ 1000 → `oil_pressure`; `oilTemperature` K − 273.15 → `oil_temp`; `fuel.rate` m³/s × 3 600 000 → `fuel_rate`; `runTime` s ÷ 3600 → `runtime_h`; `engineLoad` ratio × 100 → `percent_load`; `alternatorVoltage` V → `electrical.alternator_voltage`; `drive.trimState` ratio × 100 → `trim_position`; `transmission.gear` Forward/Neutral/Reverse → `gear` 1/0/−1; `tanks.fuel.<SIGNALK_FUEL_TANK>.currentLevel` ratio × 100 → `fuel_level`; `notifications.propulsion.<id>.*` states other than normal → `active_dtcs` count and `dtc_text`. |
| SK-3 | The smoke test feeds recorded deltas for every path in SK-2 in both id forms and checks each field and unit. This runs on the PC; it does not need a boat. |
| SK-4 | USV-725 acceptance does not depend on SK-1 to SK-3. |

### 4.9 Deployment

| ID | Requirement |
| --- | --- |
| DEP-1 | Installation on USV-725 uses `deploy/install.sh` from a release tag, with `/etc/usv/usv.env` prepared first. Beyond editing `usv.env` and adding the `grafana_bridge` block to the mission file, no step is done by hand on the boat. |
| DEP-2 | Updates use `deploy/update.sh <tag>`. |
| DEP-3 | The dashboard release is tagged `v1.2.20` (or the next free number), its commits are authored as `mali <mali@sunraysint.com>`, and the CHANGELOG lists every change. The iCANRec changes are committed to the MOOS mission repo with the same authorship. |
| DEP-4 | The release is installed on USV-725 first and accepted there (section 6) before USV-722 is updated to the same tag. On USV-722 the engine gauges keep reading N/A until the Yamaha gateway is fitted, and every other page is unchanged. |
| DEP-5 | `docs/commissioning-usv-725.md` and `docs/runbook-usv-725.md` are written during installation and record the PGNs seen, the `usv.env` used, the mission file block, and the acceptance results. |
| DEP-6 | `docs/runbook-usv-722.md` is corrected: USV-722 is the Yamaha hull and its engine data is behind the Yamaha network until the Command Link gateway is fitted. `docs/deployment.md` gains the hull table of section 2. |

## 5. Non-functional requirements

| ID | Requirement |
| --- | --- |
| NFR-1 | Engine values appear on the dashboard within 5 s of the value changing on the bus (1 s pages: within 2 s). |
| NFR-2 | `grafana_bridge` handles the 100 Hz `ENGINE_SPEED` stream without raising its own iteration time above 50 ms (`GRAFANA_BRIDGE_ITER_LEN` in the MOOS log). |
| NFR-3 | All bridges restart automatically and resume writing after a reboot of the onboard computer without operator action. |
| NFR-4 | Schema additions are limited to the fields `engine.dtc_text`, `engine.fault_lamps`, `engine.lamp_*`, `engine.ecu_voltage`, `engine.trim_position`, the `engine_type` tag, and `instance` on every engine point. Retention and downsampling stay as released. |
| NFR-5 | `tools/smoke_test.sh` passes, including the new Signal K vectors (SK-3), before the tag is cut. |

## 6. Acceptance tests on USV-725

| # | Test | Pass criterion |
| --- | --- | --- |
| A-1 | `candump -L slcan0` for 10 minutes with the engine running, saved to the commissioning record | PGN list with rates; presence or absence of 65253 and 65276 recorded; source addresses recorded |
| A-2 | `influx query` of `engine` fields for `vessel=usv-725`, grouped by `instance` and `_field` | One series per field, `instance=0`, `engine_type=mercury_diesel`; no series from `signalk_bridge` or `can_bridge` for RPM, coolant, oil, fuel, load or faults |
| A-3 | Gauge comparison against VesselView (or the engine's own display) at idle, 1500 and 3000 rpm, 5 minutes each | RPM within 1 %; coolant within 2 °C; oil temperature within 3 °C and between 60 and 130 °C with the engine warm (no sweep); oil pressure within 10 kPa; fuel rate within 5 %; load within 5 %; hours equal if shown |
| A-4 | ECU voltage against a meter at the engine battery | `engine.ecu_voltage` within 0.2 V and between 11 and 15 V |
| A-5 | Gear changes: neutral, forward, neutral, reverse | Gear tile follows within 2 s on every page that shows it (if a helm decoder is enabled) |
| A-6 | Fault injection: disconnect a sensor the engine monitors, then restore | Engine Faults tile shows the fault count, lamp and SPN/FMI text within 5 s; count returns to 0 and text to NONE within 5 s of restore; the Engine Fault alert fires and clears |
| A-7 | Two faults at once (two sensors) | Both faults listed in `dtc_text` (BAM path) |
| A-8 | Engine off | Engine dials and tiles read N/A; no engine alert fires; Readiness engine row reads FAIL with "no engine data" |
| A-9 | Reboot the onboard computer | All dashboard services and the MOOS apps return; engine values resume within 60 s of the engine running |
| A-10 | Regression on USV-722 after updating to the same tag | All 14 pages render as before; Autopilot and CAN Bus tiles from the Glendinning helm unchanged; engine gauges N/A |
| A-11 | Bench: change `ENGINE_TYPE` to `yamaha_outboard` and re-run the installer | Tachometer and coolant ranges switch to the Yamaha set; header chip changes; nothing else changes |
| A-12 | Swiftships UI side by side with the dashboard at 1500 rpm | RPM, coolant, load and fuel rate agree; oil temperature and battery voltage differ because the UI still shows the uncorrected values until the UI owner updates it |

## 7. Information needed

| # | Question | Needed for | How to get it |
| --- | --- | --- | --- |
| Q-1 | Raw CAN capture from USV-725 with the engine running (A-1) | REC-6, REC-7, CAN-2, test vectors for REC-8 | `candump -L slcan0 > usv725-engine.log` for 10 minutes, idle to 3000 rpm |
| Q-2 | Engine model, rated rpm, redline, coolant alarm temperature, minimum oil pressure | CFG-6 | Engine plate and manual |
| Q-3 | The iCAN and iCANRec source actually running on USV-725 (the May build has slew limiting that the repo lacks) | REC-9 | `git status` and `git diff` in the mission repo on the boat; copy of `src/iCAN` and `src/iCANRec` |
| Q-4 | Helm and control system on USV-725: is the Glendinning helm fitted, which receive frames exist | CAN-2 | The same capture as Q-1 |
| Q-5 | Fuel tank capacity, cruise consumption and cruise speed | AL-7 | Vessel data sheet |
| Q-6 | Which battery instance the Victron publishes in Signal K on USV-725 | DM-18 | Signal K Data Browser, `electrical.batteries.*` |
| Q-7 | Who owns the MOOS mission repo and the Swiftships UI, and how iCANRec changes are reviewed and deployed | REC-1 to REC-9, section 9 | You |

## 8. Deliverables

1. Dashboard release `v1.2.20`: `usv.env` keys and installer wiring, single-source switches in the three bridges, `grafana_bridge` mapping additions (DM-12 to DM-15, DM-17, DM-18), dashboard generator (engine chip, range table, diesel tiles, two-needle dials, Fleet column), new alert rules, readiness changes, Signal K bridge changes with smoke-test vectors, CHANGELOG, schema and user guide updates.
2. iCANRec corrections REC-1 to REC-8 in the MOOS mission repo, with unit tests, after REC-9 is settled.
3. `docs/commissioning-usv-725.md` with the Q-1 capture summary and the section 6 results.
4. `docs/runbook-usv-725.md`.
5. Corrected `docs/runbook-usv-722.md` and the hull table in `docs/deployment.md`.

## 9. Defects found in the Swiftships UI engine panel (for the UI owner, not part of this release)

| Item | Finding | File |
| --- | --- | --- |
| Oil pressure gauge | Labelled PSI with a 0 to 80 range; the value received is kPa (240 to 640), so the gauge is always pegged | `engine-status.component.ts` |
| Active DTCs | Hard-coded to 0 with the comment "Changed for a video"; the panel never shows a fault | `app.component.ts` line 2531 |
| Oil temperature | Shows the uncorrected iCANRec value (sweeps −40 to 212 °C) | depends on REC-1 |
| Battery voltage | Reads `electrical.batteries.House.voltage` from Signal K instead of `ENGINE_BATTERY_VOLTAGE`, a workaround for REC-3 | `app.component.ts` line 2555 |
| Torque and pedal gauges | Always 0: the engine sends those bytes as not available and torque reads the wrong byte | depends on REC-2 |
