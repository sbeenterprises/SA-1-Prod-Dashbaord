#!/bin/bash
# End-to-end smoke test for the Swift Autonomous Dashboard stack.
#
# Runs with NO vessel hardware and NO installed InfluxDB/Grafana: it stands up
# a stub InfluxDB receiver, a real MOOSDB, and a mock Signal K server, then
# exercises every C++ writer and cross-checks what they emit against what the
# dashboards actually query.
#
# Usage:  tools/smoke_test.sh            (from the repo root)
# Requires: g++, cmake, libcurl-dev, python3, and MOOS installed (for the
#           MOOS bridge test; that one test is skipped if MOOSDB is absent).
set -uo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
TMP=$(mktemp -d)
LINES="$TMP/lines.txt"
PASS=0; FAIL=0; SKIP=0
trap 'pkill -f "$TMP/stub.py" 2>/dev/null; rm -rf "$TMP"' EXIT

ok()   { echo "  [PASS] $*"; PASS=$((PASS+1)); }
bad()  { echo "  [FAIL] $*"; FAIL=$((FAIL+1)); }
skip() { echo "  [SKIP] $*"; SKIP=$((SKIP+1)); }
hdr()  { echo; echo "=== $* ==="; }

# ---------------------------------------------------------------- stub Influx
cat > "$TMP/stub.py" <<EOF
from http.server import BaseHTTPRequestHandler, HTTPServer
class H(BaseHTTPRequestHandler):
    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(n).decode()
        target = "$TMP/deletes.txt" if self.path.startswith("/api/v2/delete") else "$LINES"
        open(target, "a").write(body + "\n")
        self.send_response(204); self.end_headers()
    def log_message(self, *a): pass
HTTPServer(("127.0.0.1", 8086), H).serve_forever()
EOF
start_stub() { : > "$LINES"; nohup python3 "$TMP/stub.py" >/dev/null 2>&1 & sleep 1; }
stop_stub()  { pkill -f "$TMP/stub.py" 2>/dev/null; sleep 0.3; }

export INFLUX_URL=http://127.0.0.1:8086 INFLUX_TOKEN=smoke-token \
       INFLUX_ORG=usv INFLUX_BUCKET=telemetry VESSEL_ID=usv-smoke

# measurement,field pairs seen in the captured line protocol
seen() { grep -oE '^[a-z_]+,[^ ]* [a-z_]+' "$LINES" | sed 's/,[^ ]* / /' | sort -u; }

hdr "Build"
for d in ingestion/moos-cpp ingestion/can-cpp ingestion/signalk-cpp simulator; do
  if cmake -S "$REPO/$d" -B "$REPO/$d/build" -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1 \
     && cmake --build "$REPO/$d/build" -j"$(nproc)" >/dev/null 2>&1; then
    ok "build $d"
  else
    bad "build $d"
  fi
done

hdr "Mission-logic unit tests (Alpha state machine)"
MLT="$REPO/ingestion/moos-cpp/build/mission_logic_test"
if [[ ! -x $MLT ]]; then
  # MOOS absent -> the full configure failed; build the tests alone.
  cmake -S "$REPO/ingestion/moos-cpp" -B "$REPO/ingestion/moos-cpp/build-tests" \
    -DMISSION_LOGIC_TESTS_ONLY=ON >/dev/null 2>&1 \
    && cmake --build "$REPO/ingestion/moos-cpp/build-tests" -j"$(nproc)" >/dev/null 2>&1
  MLT="$REPO/ingestion/moos-cpp/build-tests/mission_logic_test"
fi
if [[ -x $MLT ]] && OUT=$("$MLT"); then
  ok "mission_logic_test: ${OUT##*: }"
else
  bad "mission_logic_test failed"; echo "$OUT" | tail -5
fi

hdr "Simulator -> InfluxDB line protocol"
start_stub
timeout 6 "$REPO/simulator/build/usv_simulator" >/dev/null 2>&1
stop_stub
COUNT=$(grep -c . "$LINES" 2>/dev/null || echo 0)
[ "$COUNT" -gt 100 ] && ok "simulator wrote $COUNT lines" || bad "simulator wrote only $COUNT lines"
grep -q "vessel=usv-smoke" "$LINES" && ok "VESSEL_ID tag applied" || bad "VESSEL_ID tag missing"
seen > "$TMP/produced.txt"
echo "  ($(wc -l < "$TMP/produced.txt") distinct measurement/field pairs)"

hdr "Dashboard query completeness"
python3 - "$REPO" <<'PY'
import json, glob, sys
bad = 0
for p in glob.glob(f"{sys.argv[1]}/grafana/dashboards/*.json"):
    for pan in json.load(open(p))["panels"]:
        for t in pan.get("targets", []):
            q = t.get("query", "")
            if q and ("from(bucket" not in q or "|> range(" not in q):
                bad += 1; print(f"  [FAIL] incomplete query: {p.split('/')[-1]} / {pan['title']} / {t['refId']}")
print("  [PASS] every dashboard query has from() and range()" if not bad else f"  {bad} incomplete")
sys.exit(1 if bad else 0)
PY
[ $? -eq 0 ] && PASS=$((PASS+1)) || FAIL=$((FAIL+1))

hdr "Dashboard queries vs simulator output"
python3 - "$REPO" "$TMP/produced.txt" <<'PY'
import json, glob, re, sys
repo, produced_file = sys.argv[1], sys.argv[2]
pm = re.compile(r'_measurement == "(\w+)"'); pf = re.compile(r'_field == "(\w+)"')
want = set()
for p in glob.glob(f"{repo}/grafana/dashboards/*.json"):
    for pan in json.load(open(p))["panels"]:
        for t in pan.get("targets", []):
            q = t.get("query", "")
            for m in pm.findall(q):
                for f in pf.findall(q):
                    want.add((m, f))
have = {tuple(l.split()) for l in open(produced_file) if l.strip()}
# Fields the MOOS bridge maps from the helm (its FieldMap table) count as
# produced too; the live bridge run below is skipped when MOOSDB is absent.
src = open(f"{repo}/ingestion/moos-cpp/moos_bridge.cpp").read()
moos = set(re.findall(r'\{"(\w+)",\s*"(\w+)"(?:,|\})', src))
moos |= set(re.findall(r'(?:str_)?field\("(\w+)",\s*[^,]+,\s*"(\w+)"', src))
external = {"cpu","mem","disk","diskio","net","temp","system","nvidia_smi","ping","net_response","systemd_units","canbus_pgn"}
missing = sorted((m,f) for m,f in want if m not in external and (m,f) not in have and (m,f) not in moos)
print(f"  dashboard pairs: {len(want)} | simulator emits: {len(have)} | moos bridge maps: {len(moos)} | telegraf-sourced: skipped")
if missing:
    print("  [FAIL] queried but never written:")
    for m,f in missing: print(f"      {m}.{f}")
    sys.exit(1)
print("  [PASS] every non-Telegraf field queried by a dashboard is produced")
PY
[ $? -eq 0 ] && PASS=$((PASS+1)) || FAIL=$((FAIL+1))

hdr "Alert rule queries vs simulator output"
python3 - "$REPO" "$TMP/produced.txt" <<'PY'
import yaml, re, sys
repo, produced_file = sys.argv[1], sys.argv[2]
pm = re.compile(r'_measurement == "(\w+)"'); pf = re.compile(r'_field == "(\w+)"')
d = yaml.safe_load(open(f"{repo}/grafana/provisioning/alerting/alerts.yml"))
have = {tuple(l.split()) for l in open(produced_file) if l.strip()}
# fields the MOOS bridge writes (its live run below needs MOOSDB)
src = open(f"{repo}/ingestion/moos-cpp/moos_bridge.cpp").read()
have |= set(re.findall(r'\{"(\w+)",\s*"(\w+)"(?:,|\})', src))
have |= set(re.findall(r'(?:str_)?field\("(\w+)",\s*[^,]+,\s*"(\w+)"', src))
external = {"cpu","mem","disk","diskio","net","temp","net_response","systemd_units"}
missing = []
for g in d["groups"]:
    for r in g["rules"]:
        for q in r["data"]:
            s = q.get("model", {}).get("query", "")
            for m in pm.findall(s):
                for f in pf.findall(s):
                    if m not in external and (m, f) not in have:
                        missing.append((r["title"], m, f))
n = sum(len(g["rules"]) for g in d["groups"])
if missing:
    print("  [FAIL] alert queries with no data source:")
    for t,m,f in missing: print(f"      {t}: {m}.{f}")
    sys.exit(1)
print(f"  [PASS] all {n} alert rules query fields the stack actually writes")
PY
[ $? -eq 0 ] && PASS=$((PASS+1)) || FAIL=$((FAIL+1))

hdr "Signal K bridge (mock WebSocket server)"
if [[ ! -x "$REPO/ingestion/signalk-cpp/build/signalk_bridge" ]]; then
  skip "Signal K test (bridge not built — IXWebSocket missing on this host)"
elif command -v node >/dev/null && NODE_PATH="${NODE_PATH:-$(npm root -g 2>/dev/null)}" node -e "require('ws')" 2>/dev/null; then
  cat > "$TMP/sk.js" <<'EOF'
const { WebSocketServer } = require('ws');
const wss = new WebSocketServer({ port: 3111 });
wss.on('connection', ws => setInterval(() => ws.send(JSON.stringify({updates:[{values:[
  {path:'navigation.speedOverGround', value:2.5},
  {path:'navigation.position', value:{latitude:29.61, longitude:-91.21}},
  {path:'navigation.attitude', value:{roll:0.02, pitch:0.01, yaw:0.9}},
  {path:'propulsion.0.revolutions', value:42.0},
  {path:'propulsion.0.coolantTemperature', value:358.15},
  {path:'electrical.batteries.0.voltage', value:12.9},
  {path:'navigation.gnss.type', value:'RTK Fixed'}]}]})), 200));
EOF
  start_stub
  NODE_PATH="${NODE_PATH:-$(npm root -g 2>/dev/null)}" nohup node "$TMP/sk.js" >/dev/null 2>&1 & SK=$!
  sleep 1
  SIGNALK_URL=ws://127.0.0.1:3111/signalk/v1/stream \
    timeout 6 "$REPO/ingestion/signalk-cpp/build/signalk_bridge" >/dev/null 2>&1
  kill $SK 2>/dev/null; stop_stub
  grep -q "nav,.* sog=" "$LINES"           && ok "SOG written"           || bad "SOG missing"
  grep -q "nav,.* lat=" "$LINES"           && ok "position decoded"      || bad "position missing"
  grep -q "engine,.* rpm=" "$LINES"        && ok "engine RPM decoded"    || bad "RPM missing"
  grep -q "signalk_ws_up=1" "$LINES"       && ok "link-up heartbeat"     || bad "no heartbeat"
  # unit conversions: 2.5 m/s -> ~4.86 kn ; 42 Hz -> 2520 rpm ; 358.15K -> 85 C
  python3 - "$LINES" <<'PY'
import re,sys
t=open(sys.argv[1]).read()
def val(pat):
    m=re.search(pat,t); return float(m.group(1)) if m else None
checks=[("SOG m/s->kn", val(r'nav,\S* sog=([0-9.]+)'), 4.86, 0.05),
        ("RPM Hz->rpm", val(r'engine,\S* rpm=([0-9.]+)'), 2520, 1),
        ("coolant K->C", val(r'engine,\S* coolant_temp=([0-9.]+)'), 85.0, 0.1)]
bad=0
for name,got,exp,tol in checks:
    if got is None or abs(got-exp)>tol:
        print(f"  [FAIL] {name}: got {got}, expected ~{exp}"); bad=1
    else:
        print(f"  [PASS] {name}: {got}")
sys.exit(bad)
PY
  [ $? -eq 0 ] && PASS=$((PASS+3)) || FAIL=$((FAIL+1))
else
  skip "Signal K test (node/ws unavailable)"
fi

hdr "MOOS bridge (real MOOSDB)"
if command -v MOOSDB >/dev/null && command -v umm >/dev/null; then
  cat > "$TMP/test.moos" <<EOF
ServerHost = localhost
ServerPort = 9010
Community  = usv-smoke
LatOrigin  = 29.71970895
LongOrigin = -91.14705165
ProcessConfig = grafana_bridge
{
  AppTick   = 10
  CommsTick = 20
  influx_url    = 127.0.0.1:8086
  influx_tls    = false
  influx_token  = smoke-token
  influx_org    = usv
  influx_bucket = telemetry
  events_bucket = events
  vessel_id     = usv-moos
}
EOF
  start_stub
  nohup MOOSDB --moos_port=9010 --moos_quiet >/dev/null 2>&1 & DB=$!
  sleep 1
  (cd "$TMP" && nohup "$REPO/ingestion/moos-cpp/build/moos_bridge" test.moos >"$TMP/bridge.log" 2>&1 &)
  sleep 2
  nohup umm --moos_port=9010 --moos_quiet --moos_name=smokepoke \
      -p=NAV_SPEED@5.0,NAV_HEADING@5.0,DESIRED_RUDDER@5.0,MISSION_PROGRESS@5.0 >/dev/null 2>&1 & UM=$!
  sleep 6
  kill $UM 2>/dev/null

  # ---- Alpha mission scenario replay (alpha.bhv variable sequence) ----
  POKE="$REPO/ingestion/moos-cpp/build/moos_poke"
  if [[ -x $POKE ]]; then
    P() { "$POKE" --port=9010 "$@" >/dev/null 2>&1; }
    P IVPHELM_STATE=DRIVE IVPHELM_ALLSTOP=clear \
      'VIEW_SEGLIST=pts={0,0:800,0:800,800:0,800:0,400},label=alpha_waypt_survey' \
      WAYPOINT=true DEPLOY=true DIST_TO_NEXT_WPT=650 WPT_INDEX=0 \
      'IVPHELM_SUMMARY=iter=10,active_bhvs=waypt_survey$0.6$100$4$0$0/0$1:opr24$0.6$300$5$0$0/0$1,idle_bhvs=station_keep'
    sleep 2
    P COMMS_LOST=grace                                         # link down, mission continues
    sleep 2
    P COMMS_LOST=loiter MISSION_ARMED=false STATIONING=true    # grace expired: failsafe hold
    sleep 2
    P COMMS_LOST=false STATIONING=false                        # link back, NOT re-armed
    sleep 2
    P MISSION_ARMED=true                                       # operator re-arm
    sleep 1
    P OPR_BREACHED=poly 'OBSTACLE_ALERT=name=ob_7#pts={60,60:70,60:70,70},label=ob_7'
    sleep 2
    P 'BHV_WARNING=Behavior avdcol_: unrecognized condition (MALCONFIG)'
    sleep 2
    # Vessel / simulation producers added in the alignment pass: planchaPID
    # governor + XTE, uSimMarine COG, iCANRec engine, iWebSocket route id,
    # contacts (NODE_REPORT), crash stop.
    P NAV_X=0 NAV_Y=0 DESIRED_HEADING=95 DESIRED_SPEED=3
    sleep 1
    P XTE_NOW=3.2 NAV_HEADING_OVER_GROUND=88 SPEED_CAP=4.0 NAV_HEADING=80 \
      ENGINE_SPEED=1850 ENGINE_COOLANT_TEMP=82 ID_ROUTE=R-17 \
      ARRIVAL_STATE=decelerating CRASH_STOP=true \
      'NODE_REPORT=NAME=contato_7,TYPE=ship,TIME=1,X=300,Y=400,SPD=5,HDG=90'
    sleep 1
    P 'OPR_POLY_ACTUAL=pts={-100,-100:100,-100:100,100:-100,100}'
    sleep 3
  fi
  kill $DB 2>/dev/null; pkill -f moos_bridge 2>/dev/null; stop_stub

  grep -q "vessel=usv-moos" "$LINES"        && ok "mission-file vessel_id applied" || bad "ProcessConfig vessel_id not applied"
  grep -q "influx=http://127.0.0.1:8086" "$TMP/bridge.log" && ok "scheme-less influx_url expanded" || bad "influx_url not expanded"
  grep -q "nav,.* sog=" "$LINES"            && ok "NAV_SPEED -> nav.sog"          || bad "NAV_SPEED not mapped"
  grep -q "nav,.* heading=" "$LINES"        && ok "NAV_HEADING -> nav.heading"    || bad "NAV_HEADING not mapped"
  grep -q "autopilot,.* rudder_cmd=" "$LINES" && ok "DESIRED_RUDDER -> autopilot" || bad "DESIRED_RUDDER not mapped"
  grep -q "mission,.* progress_pct=" "$LINES" && ok "MISSION_PROGRESS -> mission" || bad "MISSION_PROGRESS not mapped"
  grep -q "moos_connected=1" "$LINES"       && ok "MOOS connect heartbeat"        || bad "no connect heartbeat"
  if [[ -x $POKE ]]; then
    grep -q 'state="Autonomous"' "$LINES"     && ok "WAYPOINT=true -> state Autonomous"     || bad "derived state Autonomous missing"
    grep -q 'state="CommsLost"' "$LINES"      && ok "COMMS_LOST -> state CommsLost"         || bad "derived state CommsLost missing"
    grep -q 'state="AwaitingRearm"' "$LINES"  && ok "link back w/o re-arm -> AwaitingRearm" || bad "derived state AwaitingRearm missing"
    grep -q "mission,.* dist_to_waypoint=650" "$LINES" && ok "DIST_TO_NEXT_WPT -> mission.dist_to_waypoint" || bad "DIST_TO_NEXT_WPT not mapped"
    grep -q "mission,.* waypoints_total=5" "$LINES"    && ok "VIEW_SEGLIST -> waypoints_total=5"            || bad "seglist waypoint count missing"
    # Map overlays: route vertices in lat/lon from the mission datum, region
    # polygon closed (5 vertices for a 4-corner box), stale set deleted first.
    [ "$(grep -c 'geometry,vessel=usv-moos,kind=route,idx=' "$LINES")" -ge 15 ] \
      && ok "VIEW_SEGLIST -> geometry route vertices (lat/lon/seq)" || bad "route geometry missing"
    grep -q 'geometry,vessel=usv-moos,kind=region,idx=004 ' "$LINES" \
      && ok "OPR_POLY_ACTUAL -> closed region outline" || bad "region geometry missing/not closed"
    grep -q 'kind=route,idx=001 lat=29.7[0-9]*' "$LINES" \
      && ok "route vertex converted with the mission datum" || bad "datum conversion missing"
    grep -q 'kind=\\"region\\" AND vessel=\\"usv-moos\\"' "$TMP/deletes.txt" 2>/dev/null \
      && ok "previous geometry set deleted before rewrite" || bad "geometry delete not issued"
    # Traffic: NODE_REPORT contact (X=300,Y=400, 5 m/s, hdg 90) vs own ship at
    # the origin -> per-contact row with bearing ~36.9 and a risk level.
    grep -q 'contacts,vessel=usv-moos,contact=contato_7,source=report bearing=36\.[0-9]*' "$LINES" \
      && ok "NODE_REPORT -> contacts row with bearing" || bad "contact bearing missing"
    grep -q 'contacts,vessel=usv-moos,contact=contato_7,source=report risk=' "$LINES" \
      && ok "contact risk level written" || bad "contact risk missing"
    grep -q 'traffic,vessel=usv-moos contacts=1' "$LINES" \
      && ok "traffic summary counts the contact" || bad "traffic summary missing"
    grep -q "autonomy,.* comms_lost=1" "$LINES"        && ok "autonomy.comms_lost flag written"             || bad "autonomy.comms_lost missing"
    grep -q "autonomy,.* awaiting_rearm=1" "$LINES"    && ok "autonomy.awaiting_rearm hold visible"         || bad "awaiting_rearm missing"
    grep -q "autonomy,.* geofence_breached=1" "$LINES" && ok "OPR_BREACHED -> geofence_breached=1"          || bad "geofence level missing"
    grep -q 'active_behavior="waypt_survey, opr24"' "$LINES" && ok "IVPHELM_SUMMARY -> active behaviors"    || bad "active behavior parse missing"
    grep -q "Geofence BREACHED" "$LINES"      && ok "geofence breach event emitted"         || bad "geofence event missing"
    grep -q "Obstacle avoidance active: ob_7" "$LINES" && ok "obstacle alert event emitted" || bad "obstacle event missing"
    grep -q "Operator link LOST" "$LINES"     && ok "comms-loss failsafe event emitted"     || bad "comms-loss event missing"
    grep -q "Mission re-armed" "$LINES"       && ok "re-arm event emitted"                  || bad "re-arm event missing"
    grep -q "Helm behavior: " "$LINES"        && ok "BHV_WARNING event emitted"             || bad "BHV_WARNING event missing"
    grep -q "autonomy,.* comms_phase=1" "$LINES"       && ok "COMMS_LOST=grace -> comms_phase=1"        || bad "comms_phase grace missing"
    grep -q "grace period, mission continues" "$LINES" && ok "grace-period warning event emitted"      || bad "grace event missing"
    grep -q "nav,.* cross_track_error=3.2" "$LINES"    && ok "XTE_NOW -> nav.cross_track_error"         || bad "XTE_NOW not mapped"
    grep -q "nav,.* cog=88" "$LINES"                   && ok "NAV_HEADING_OVER_GROUND -> nav.cog"       || bad "COG not mapped"
    grep -q "autopilot,.* speed_cap=" "$LINES"         && ok "SPEED_CAP -> autopilot.speed_cap (kn)"    || bad "SPEED_CAP not mapped"
    grep -q "autopilot,.* heading_error=15" "$LINES"   && ok "heading_error derived (95-80=15)"   || bad "heading_error not derived"
    grep -q "engine,vessel=usv-moos,instance=0 rpm=1850" "$LINES" && ok "ENGINE_SPEED -> engine.rpm{instance=0}" || bad "ENGINE_SPEED not mapped"
    grep -q 'mission_id="R-17"' "$LINES"               && ok "ID_ROUTE -> mission_state.mission_id"     || bad "ID_ROUTE not mapped"
    grep -q 'arrival_state="decelerating"' "$LINES"    && ok "ARRIVAL_STATE -> mission_state"           || bad "ARRIVAL_STATE not mapped"
    grep -q "radar,.* targets=1" "$LINES"              && ok "NODE_REPORT -> radar.targets=1"           || bad "contact tracking missing"
    grep -q "radar,.* closest_range_m=500" "$LINES"    && ok "contact range from NAV_X/Y (500 m)"       || bad "contact range missing"
    grep -q "autonomy,.* crash_stop=1" "$LINES"        && ok "CRASH_STOP -> autonomy.crash_stop"        || bad "crash_stop missing"
    grep -q "CRASH STOP commanded" "$LINES"            && ok "crash-stop event emitted"                 || bad "crash-stop event missing"
    grep -q "mission,.* waypoints_remaining=5" "$LINES" && ok "waypoints_remaining derived"             || bad "waypoints_remaining missing"
    grep -q "Survey started" "$LINES" && grep -q "start_ms=" "$LINES" && ok "mission span event carries start_ms" || bad "mission start_ms missing"
  else
    skip "Alpha scenario replay (moos_poke not built)"
  fi
else
  skip "MOOS bridge test (MOOSDB/umm not on PATH)"
fi

hdr "CAN bridge (virtual CAN)"
if ip link show vcan-smoke >/dev/null 2>&1 || \
   (modprobe vcan 2>/dev/null && ip link add dev vcan-smoke type vcan 2>/dev/null && ip link set up vcan-smoke 2>/dev/null); then
  start_stub
  CAN_INTERFACE=vcan-smoke timeout 5 "$REPO/ingestion/can-cpp/build/can_bridge" >/dev/null 2>&1 &
  sleep 1
  command -v cansend >/dev/null && cansend vcan-smoke 09F20100#FF7D000000000000 2>/dev/null
  sleep 4; stop_stub
  grep -q "canbus," "$LINES" && ok "CAN bus stats written" || bad "no CAN stats"
  ip link del vcan-smoke 2>/dev/null
else
  skip "CAN bridge live capture (vcan module unavailable in this environment)"
  # Still exercise the failure path: it must degrade, log a reason, and flag
  # interface_up=0 rather than dying, since can0 is often down at first boot.
  start_stub
  ( CAN_INTERFACE=nosuchcan0 timeout 8 "$REPO/ingestion/can-cpp/build/can_bridge" \
      >/dev/null 2>"$TMP/can.err" ); rc=$?
  stop_stub
  [ $rc -eq 124 ] && ok "CAN bridge survives missing interface (retries, no exit)" \
                  || bad "CAN bridge exited rc=$rc on missing interface"
  grep -qE "failed:|not found \(" "$TMP/can.err" \
      && ok "CAN failure logged with errno reason" \
      || bad "CAN failure logged without a diagnosable reason"
  grep -q "interface_up=0" "$LINES" \
      && ok "canbus.interface_up=0 published while down" \
      || bad "no interface_up=0 signal while CAN is down"
fi

hdr "Resilience: InfluxDB down"
# writer must survive a dead endpoint without crashing
timeout 5 "$REPO/simulator/build/usv_simulator" >/dev/null 2>&1
rc=$?
[ $rc -eq 124 ] && ok "simulator survives unreachable InfluxDB (no crash)" \
                || bad "simulator exited rc=$rc with InfluxDB down"

hdr "Summary"
echo "  PASS: $PASS   FAIL: $FAIL   SKIP: $SKIP"
[ "$FAIL" -eq 0 ] && echo "  RESULT: SMOKE TEST PASSED" || echo "  RESULT: SMOKE TEST FAILED"
exit $([ "$FAIL" -eq 0 ] && echo 0 || echo 1)
