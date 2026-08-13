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
        open("$LINES", "a").write(self.rfile.read(n).decode() + "\n")
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

hdr "Simulator -> InfluxDB line protocol"
start_stub
timeout 6 "$REPO/simulator/build/usv_simulator" >/dev/null 2>&1
stop_stub
COUNT=$(grep -c . "$LINES" 2>/dev/null || echo 0)
[ "$COUNT" -gt 100 ] && ok "simulator wrote $COUNT lines" || bad "simulator wrote only $COUNT lines"
grep -q "vessel=usv-smoke" "$LINES" && ok "VESSEL_ID tag applied" || bad "VESSEL_ID tag missing"
seen > "$TMP/produced.txt"
echo "  ($(wc -l < "$TMP/produced.txt") distinct measurement/field pairs)"

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
external = {"cpu","mem","disk","diskio","net","temp","net_response","systemd_units","canbus_pgn"}
missing = sorted((m,f) for m,f in want if m not in external and (m,f) not in have)
print(f"  dashboard pairs: {len(want)} | simulator emits: {len(have)} | telegraf-sourced: skipped")
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
if missing:
    print("  [FAIL] alert queries with no data source:")
    for t,m,f in missing: print(f"      {t}: {m}.{f}")
    sys.exit(1)
print("  [PASS] all 12 alert rules query fields the stack actually writes")
PY
[ $? -eq 0 ] && PASS=$((PASS+1)) || FAIL=$((FAIL+1))

hdr "Signal K bridge (mock WebSocket server)"
if command -v node >/dev/null && node -e "require('ws')" 2>/dev/null; then
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
  nohup node "$TMP/sk.js" >/dev/null 2>&1 & SK=$!
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
  kill $UM $DB 2>/dev/null; pkill -f moos_bridge 2>/dev/null; stop_stub
  grep -q "vessel=usv-moos" "$LINES"        && ok "mission-file vessel_id applied" || bad "ProcessConfig vessel_id not applied"
  grep -q "influx=http://127.0.0.1:8086" "$TMP/bridge.log" && ok "scheme-less influx_url expanded" || bad "influx_url not expanded"
  grep -q "nav,.* sog=" "$LINES"            && ok "NAV_SPEED -> nav.sog"          || bad "NAV_SPEED not mapped"
  grep -q "nav,.* heading=" "$LINES"        && ok "NAV_HEADING -> nav.heading"    || bad "NAV_HEADING not mapped"
  grep -q "autopilot,.* rudder_cmd=" "$LINES" && ok "DESIRED_RUDDER -> autopilot" || bad "DESIRED_RUDDER not mapped"
  grep -q "mission,.* progress_pct=" "$LINES" && ok "MISSION_PROGRESS -> mission" || bad "MISSION_PROGRESS not mapped"
  grep -q "moos_connected=1" "$LINES"       && ok "MOOS connect heartbeat"        || bad "no connect heartbeat"
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
