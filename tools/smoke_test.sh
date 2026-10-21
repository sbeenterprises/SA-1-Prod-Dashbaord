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
        if self.path.startswith("/api/v2/query"):
            # Readiness poller: answer as InfluxDB would (annotated CSV) with
            # the fields other producers write, stamped "now".
            import datetime
            open("$TMP/queries.txt", "a").write(body + "\n")
            t = datetime.datetime.utcnow().strftime("%Y-%m-%dT%H:%M:%S.000000000Z")
            rows = [("comms", "signalk_ws_up", 1), ("ping", "result_code", 0),
                    ("ping", "average_response_ms", 38.2), ("canbus", "interface_up", 1),
                    ("canbus", "msg_rate", 212), ("cpu", "usage_user", 4.1),
                    ("mem", "used_percent", 11.3), ("disk", "used_percent", 63)]
            csv = "#datatype,string,long,dateTime:RFC3339,double,string,string\r\n#group,false,false,false,false,true,true\r\n#default,_result,,,,,\r\n,result,table,_time,_value,_field,_measurement\r\n"
            csv += "".join(f",,{i},{t},{v},{f},{m}\r\n" for i, (m, f, v) in enumerate(rows)) + "\r\n"
            data = csv.encode()
            self.send_response(200); self.send_header("Content-Type", "text/csv")
            self.send_header("Content-Length", str(len(data))); self.end_headers()
            self.wfile.write(data); return
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
# The host's own engine profile (/etc/usv/usv.env on a vessel) must not
# shape the test: every bridge reads an empty profile, so every producer
# writes its engine fields and the vectors below hold on any host.
: > "$TMP/usv.env"
export USV_ENV_FILE="$TMP/usv.env"
unset ENGINE_TYPE ENGINE_SOURCE ENGINE_COUNT ENGINE_INSTANCES HELM_PROFILE \
      SIGNALK_ENGINE CAN_N2K_ENGINE CAN_DM1 MOOS_ENGINE SIGNALK_BATTERY SIGNALK_FUEL_TANK \
      CAN_J1939_ENGINE J1939_ENGINE_SA CAN_ENGINE_HOURS_REQUEST J1939_REQUEST_SA \
      STEER_MAX_DEG STEER_RAW_PORT STEER_RAW_CENTER STEER_RAW_STBD CAN_REPLAY

# measurement,field pairs seen in the captured line protocol
seen() { grep -oE '^[a-z_]+,[^ ]* [a-z_]+' "$LINES" | sed 's/,[^ ]* / /' | sort -u; }

hdr "Build"
for d in ingestion/moos-cpp ingestion/can-cpp ingestion/signalk-cpp ingestion/frigate-cpp simulator; do
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

hdr "Bridge logic unit tests (radar targets, cameras, usv-722 / usv-725 CAN, J1939 engine, engine profile, J1939 faults, Signal K engine paths)"
for t in ingestion/moos-cpp/build/mayara_logic_test ingestion/moos-cpp/build-tests/mayara_logic_test \
         ingestion/frigate-cpp/build/frigate_logic_test ingestion/can-cpp/build/usv722_can_test \
         ingestion/can-cpp/build/j1939_engine_test \
         ingestion/can-cpp/build/engine_profile_test ingestion/can-cpp/build/j1939_faults_test \
         ingestion/can-cpp/build/vessel_profile_test \
         ingestion/signalk-cpp/build/signalk_engine_test; do
  [[ -x "$REPO/$t" ]] || continue
  if OUT=$("$REPO/$t" 2>&1); then ok "$(basename "$t"): $(echo "$OUT" | tail -1)"
  else bad "$(basename "$t") failed"; echo "$OUT" | tail -5; fi
done
[[ -x "$REPO/ingestion/frigate-cpp/build/frigate_logic_test" ]] || bad "frigate_logic_test not built"
[[ -x "$REPO/ingestion/can-cpp/build/usv722_can_test" ]] || bad "usv722_can_test not built"
[[ -x "$REPO/ingestion/can-cpp/build/j1939_engine_test" ]] || bad "j1939_engine_test not built"
[[ -x "$REPO/ingestion/can-cpp/build/engine_profile_test" ]] || bad "engine_profile_test not built"
[[ -x "$REPO/ingestion/can-cpp/build/vessel_profile_test" ]] || bad "vessel_profile_test not built"
[[ -x "$REPO/ingestion/can-cpp/build/j1939_faults_test" ]] || bad "j1939_faults_test not built"
[[ -x "$REPO/ingestion/signalk-cpp/build/signalk_engine_test" ]] || bad "signalk_engine_test not built"

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
            # A query built from a variable alone (array.from) reads no bucket.
            if q and "array.from(" in q and "from(bucket" not in q:
                continue
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
            # Each _measurement test owns the _field tests that follow it,
            # up to the next _measurement test (a query may read two
            # measurements, e.g. nav lat/lon and mission base_lat/base_lon);
            # fields before the first one belong to the first.
            ms = list(pm.finditer(q))
            for i, mm in enumerate(ms):
                start = 0 if i == 0 else mm.start()
                end = ms[i + 1].start() if i + 1 < len(ms) else len(q)
                for f in pf.findall(q[start:end]):
                    want.add((mm.group(1), f))
have = {tuple(l.split()) for l in open(produced_file) if l.strip()}
# Fields the MOOS bridge maps from the helm (its FieldMap table) count as
# produced too; the live bridge run below is skipped when MOOSDB is absent.
src = open(f"{repo}/ingestion/moos-cpp/moos_bridge.cpp").read()
# Other C++ writers that the simulator does not exercise (Frigate cameras).
src += open(f"{repo}/ingestion/frigate-cpp/frigate_bridge.cpp").read()
src += open(f"{repo}/ingestion/can-cpp/usv722_can.hpp").read()
# Fields only the CAN and Signal K bridges write (engine.fuel_remaining_l).
src += open(f"{repo}/ingestion/can-cpp/main.cpp").read()
src += open(f"{repo}/ingestion/signalk-cpp/signalk_bridge.cpp").read()
moos = set(re.findall(r'\{"(\w+)",\s*"(\w+)"(?:,|\})', src))
moos |= set(re.findall(r'(?:str_)?field\("(\w+)",\s*[^,]+,\s*"(\w+)"', src))
external = {"cpu","mem","disk","diskio","net","temp","system","gpu","ping","net_response","systemd_units","canbus_pgn"}
missing = sorted((m,f) for m,f in want if m not in external and (m,f) not in have and (m,f) not in moos)
print(f"  dashboard pairs: {len(want)} | simulator emits: {len(have)} | moos bridge maps: {len(moos)} | telegraf-sourced: skipped")
if missing:
    print("  [FAIL] queried but never written:")
    for m,f in missing: print(f"      {m}.{f}")
    sys.exit(1)
print("  [PASS] every non-Telegraf field queried by a dashboard is produced")
PY
[ $? -eq 0 ] && PASS=$((PASS+1)) || FAIL=$((FAIL+1))

hdr "ECharts instruments (getOption in a mocked Grafana context)"
if command -v node >/dev/null; then
  if OUT=$(node "$REPO/tools/check_echarts_panels.js" "$REPO" 2>&1); then
    echo "$OUT" | grep -v "echarts panels:"; ok "ECharts instruments run clean"
  else echo "$OUT"; bad "ECharts instrument check"; fi
else skip "ECharts instruments (node unavailable)"; fi

hdr "Flux type check (dashboards and alert rules)"
python3 - "$REPO" "$TMP/alert_queries.json" <<'PY'
import json, sys, yaml
d = yaml.safe_load(open(f"{sys.argv[1]}/grafana/provisioning/alerting/alerts.yml"))
qs = [q["model"]["query"] for g in d["groups"] for r in g["rules"] for q in r["data"]
      if "query" in q.get("model", {})]
json.dump(qs, open(sys.argv[2], "w"))
PY
NODE_PATH="${NODE_PATH:-$(npm root -g 2>/dev/null)}" node "$REPO/tools/check_flux.js" "$REPO" "$TMP/alert_queries.json"
case $? in
  0) ok "every Flux query type-checks" ;;
  2) skip "Flux type check (npm i -g @influxdata/flux-lsp-node to enable)" ;;
  *) bad "Flux type check" ;;
esac

hdr "Generated files match their generators"
# grafana/dashboards/*.json and alerts.yml are generated: a hand edit, or a
# generator change committed without regenerating, ships something the
# next regeneration silently undoes. Regenerate in place, compare with the
# committed files, then put the committed files back.
GEN_SNAP="$TMP/gen-snapshot"; mkdir -p "$GEN_SNAP"
cp -a "$REPO/grafana/dashboards" "$GEN_SNAP/dashboards"
cp -a "$REPO/grafana/provisioning/alerting/alerts.yml" "$GEN_SNAP/alerts.yml"
if python3 "$REPO/tools/gen_dashboards.py" >/dev/null 2>"$TMP/gen.err" \
   && python3 "$REPO/tools/gen_alerts.py" >/dev/null 2>>"$TMP/gen.err"; then
  if diff -rq "$GEN_SNAP/dashboards" "$REPO/grafana/dashboards" >"$TMP/gen.diff" \
     && diff -q "$GEN_SNAP/alerts.yml" "$REPO/grafana/provisioning/alerting/alerts.yml" >>"$TMP/gen.diff"; then
    ok "committed dashboards and alert rules equal the generator output (vessel profile)"
  else bad "committed dashboards / alerts.yml differ from the generators: run tools/gen_dashboards.py and tools/gen_alerts.py"; sed 's/^/      /' "$TMP/gen.diff"; fi
else bad "a generator failed"; tail -5 "$TMP/gen.err"; fi
rm -rf "$REPO/grafana/dashboards" && cp -a "$GEN_SNAP/dashboards" "$REPO/grafana/dashboards"
cp -a "$GEN_SNAP/alerts.yml" "$REPO/grafana/provisioning/alerting/alerts.yml"

hdr "Branding script against Grafana's page template"
# deploy/brand.sh injects style and script blocks into Grafana's
# views/index.html, which Grafana parses as a Go template with [[ ]]
# delimiters at start-up. Two square brackets in a row inside an injected
# block make that parse fail and Grafana answers every page with an empty
# response (v1.2.33 on usv-722). Run brand.sh on a stand-in public dir and
# check the injected blocks, and the whole file with Go's parser when go is
# installed.
if [[ $EUID -ne 0 ]]; then
  skip "brand.sh template check (needs root: brand.sh refuses to run otherwise)"
else
  BT="$TMP/brandpub"; mkdir -p "$BT/img" "$BT/views" "$BT/build"
  cat > "$BT/views/index.html" <<'HTML'
<!doctype html><html lang="[[.User.Locale]]"><head><title>[[.AppTitle]]</title>
<base href="[[.AppSubUrl]]/" /><link rel="icon" href="[[.FavIcon]]" />
</head><body class="theme-[[ .ThemeType ]]"><div class="preloader">Loading Grafana</div>
<script nonce="[[.Nonce]]">window.grafanaBootData = { user: [[.User]], settings: [[.Settings]] };</script>
[[range $asset := .Assets.JSFiles]]<script src="[[$asset.FilePath]]"></script>[[end]]
</body></html>
HTML
  if GPUB="$BT" GPUB_NO_RESTART=1 bash "$REPO/deploy/brand.sh" >"$TMP/brand.log" 2>&1 \
     && GPUB="$BT" GPUB_NO_RESTART=1 bash "$REPO/deploy/brand.sh" >>"$TMP/brand.log" 2>&1; then
    python3 - "$BT/views/index.html" <<'PY' && ok "injected blocks: no template delimiters, each block once" || bad "brand.sh injected blocks break Grafana's index.html template"
import re, sys
h = open(sys.argv[1]).read()
bad = 0
for bid in ("swiftships-brand", "swiftships-brand-js", "swiftships-fit-js", "swiftships-theme-js", "swiftships-alerts-js"):
    blocks = re.findall(r'<(style|script) id="%s">(.*?)</\1>' % bid, h, re.S)
    if len(blocks) != 1:
        print(f"    {bid}: found {len(blocks)} times (expected 1)"); bad = 1; continue
    body = blocks[0][1]
    for d in ("[" "[", "]" "]"):
        if d in body:
            n = body.count(d); print(f"    {bid}: contains {d!r} x{n}"); bad = 1
sys.exit(bad)
PY
    grep -q 'body.login-page footer>\*' "$BT/views/index.html" && grep -q 'ECCN 9D515' "$BT/views/index.html" \
      && ok "login footer replaced by the export-control notice" || bad "login footer rule or disclaimer missing from the brand CSS"
    if command -v go >/dev/null 2>&1; then
      mkdir -p "$TMP/tplchk" && cat > "$TMP/tplchk/main.go" <<'GO'
package main

import (
	"fmt"
	"html/template"
	"os"
)

func main() {
	b, err := os.ReadFile(os.Args[1])
	if err != nil {
		fmt.Println(err)
		os.Exit(1)
	}
	if _, err := template.New("index").Delims("[[", "]]").Parse(string(b)); err != nil {
		fmt.Println("    ", err)
		os.Exit(1)
	}
}
GO
      if (cd "$TMP/tplchk" && GOFLAGS=-mod=mod GO111MODULE=off go run main.go "$BT/views/index.html"); then
        ok "branded index.html parses as a Go template (as Grafana loads it)"
      else bad "branded index.html no longer parses as a Go template (Grafana would serve empty pages)"; fi
    else skip "Go template parse of the branded index.html (go not installed)"; fi
  else
    bad "brand.sh failed on a stand-in Grafana public dir"; tail -5 "$TMP/brand.log"
  fi
fi

hdr "Map vessel marker and speed vector"
python3 - "$REPO" <<'PY'
import json, glob, sys
repo = sys.argv[1]; bad = 0
def fail(m):
    global bad; bad += 1; print("  [FAIL] " + m)
# Grafana 13.2 draws custom SVG map markers as black squares: built-in only.
BUILTIN = {f"img/icons/marker/{n}.svg" for n in ("circle", "square", "triangle")}
markers = 0
for p in sorted(glob.glob(f"{repo}/grafana/dashboards/*.json")):
    for pan in json.load(open(p))["panels"]:
        if pan.get("type") != "geomap": continue
        refs = {t["refId"] for t in pan.get("targets", [])}
        layers = pan["options"].get("layers", [])
        for L in layers:
            ref = (L.get("filterData") or {}).get("options")
            if ref and ref not in refs:
                fail(f"{pan['title']}: layer '{L['name']}' filters on missing query {ref}")
            sym = L.get("config", {}).get("style", {}).get("symbol", {}).get("fixed")
            if L.get("type") == "markers":
                markers += 1
                if sym not in BUILTIN:
                    fail(f"{pan['title']}: layer '{L['name']}' uses {sym} (custom SVG renders black in Grafana 13.2)")
        if any(L.get("name") == "Track" for L in layers):
            q = next((t["query"] for t in pan["targets"] if t["refId"] == "E"), "")
            for need in ('import "math"', 'r.sog >= 0.5', 'union(tables: [a, b])', '/ 600.0'):
                if need not in q: fail(f"{pan['title']}: speed vector query lacks {need!r}")
print(f"  [PASS] {markers} map marker layers use built-in shapes; speed vector wired on Navigation and Replay" if not bad else f"  {bad} problems")
sys.exit(1 if bad else 0)
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
# Other C++ writers that the simulator does not exercise (Frigate cameras).
src += open(f"{repo}/ingestion/frigate-cpp/frigate_bridge.cpp").read()
src += open(f"{repo}/ingestion/can-cpp/usv722_can.hpp").read()
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
const SELF = 'vessels.urn:mrn:imo:mmsi:367000001';
// An AIS target in the same stream (subscribe=all): must never be written as own ship.
const ais = {context:'vessels.urn:mrn:imo:mmsi:368274930', updates:[{values:[
  {path:'navigation.position', value:{latitude:29.95, longitude:-91.55}},
  {path:'navigation.speedOverGround', value:20.0},
  {path:'navigation.headingTrue', value:3.0}]}]};
wss.on('connection', ws => { ws.send(JSON.stringify({name:'signalk-server', version:'2.x', self:SELF, roles:['master']}));
  setInterval(() => { ws.send(JSON.stringify(ais)); ws.send(JSON.stringify({context:SELF, updates:[{values:[
  {path:'navigation.headingTrue', value:1.0}]}]})); }, 200);
  // Engine paths in both id forms (SK-1 / SK-2): N2K instance "0" and the
  // server-given name "port" are the same engine, "starboard" is instance 1.
  setInterval(() => ws.send(JSON.stringify({updates:[{values:[
  {path:'navigation.speedOverGround', value:2.5},
  {path:'navigation.position', value:{latitude:29.61, longitude:-91.21}},
  {path:'navigation.attitude', value:{roll:0.02, pitch:0.01, yaw:0.9}},
  {path:'propulsion.0.revolutions', value:42.0},
  {path:'propulsion.0.coolantTemperature', value:358.15},
  {path:'propulsion.port.oilPressure', value:340000},
  {path:'propulsion.port.oilTemperature', value:363.15},
  {path:'propulsion.0.boostPressure', value:186000},
  {path:'propulsion.0.fuel.rate', value:1.0e-5},
  {path:'propulsion.0.runTime', value:7200},
  {path:'propulsion.0.engineLoad', value:0.64},
  {path:'propulsion.0.alternatorVoltage', value:14.2},
  {path:'propulsion.0.drive.trimState', value:0.25},
  {path:'propulsion.0.transmission.gear', value:'Forward'},
  {path:'propulsion.starboard.revolutions', value:40.0},
  {path:'tanks.fuel.0.currentLevel', value:0.91},
  {path:'notifications.propulsion.0.lowOilPressure', value:{state:'alarm', method:['visual','sound'], message:'Low Oil Pressure'}},
  {path:'electrical.batteries.0.voltage', value:12.9},
  {path:'electrical.batteries.0.current', value:14.4},
  {path:'navigation.gnss.type', value:'RTK Fixed'}]}]})), 200); });
EOF
  start_stub
  NODE_PATH="${NODE_PATH:-$(npm root -g 2>/dev/null)}" nohup node "$TMP/sk.js" >/dev/null 2>&1 & SK=$!
  sleep 1
  SIGNALK_URL=ws://127.0.0.1:3111/signalk/v1/stream ENGINE_TYPE=yamaha_outboard \
    ENGINE_MODEL="Yamaha Outboard Engine F115 B 4-Stroke" \
    VESSEL_CLASS="S2 Sea-Striker" VESSEL_LOA_M=4.76 VESSEL_BEAM_M=1.68 VESSEL_FUEL_L=265 \
    VESSEL_RANGE_NM=320 VESSEL_MAX_SPEED_KN=30 \
    timeout 6 "$REPO/ingestion/signalk-cpp/build/signalk_bridge" >/dev/null 2>&1
  kill $SK 2>/dev/null; stop_stub
  # Vessel particulars heartbeat (VESSEL_* in usv.env, v1.2.33)
  grep -q 'vessel,vessel=[^ ]*vessel_class=S2\\ Sea-Striker .*01\\ Length\\ x\\ beam\\ x\\ height="4.8 m x 1.7 m"' "$LINES" \
    && ok "vessel particulars heartbeat (class tag, card rows)" || bad "vessel particulars heartbeat missing"
  grep -q "vessel,.* fuel_l=265" "$LINES" && ok "vessel fuel_l written" || bad "vessel fuel_l missing"
  grep -q "engine,.* fuel_remaining_l=241.15" "$LINES" && ok "fuel_remaining_l = 91 % of 265 L" \
    || bad "fuel_remaining_l missing or wrong"
  grep -q "nav,.* sog=" "$LINES"           && ok "SOG written"           || bad "SOG missing"
  grep -q "nav,.* lat=" "$LINES"           && ok "position decoded"      || bad "position missing"
  grep -q "engine,.* rpm=" "$LINES"        && ok "engine RPM decoded"    || bad "RPM missing"
  grep -q "signalk_ws_up=1" "$LINES"       && ok "link-up heartbeat"     || bad "no heartbeat"
  # AIS target deltas (other vessel context) must not become own-ship nav
  if grep -qE "nav,[^ ]* ([^ ]*,)?(lat=29\.95|lon=-91\.55|sog=3[0-9]\.|heading=171\.)" "$LINES"; then
    bad "AIS target position/speed written as own ship"
  else ok "AIS target deltas (other vessel context) skipped"; fi
  grep -qE "nav,[^ ]* ([^ ]*,)?heading=57\.2" "$LINES" && ok "own-ship delta with self context accepted" \
    || bad "own-ship delta with self context dropped"
  # unit conversions (SK-2): 2.5 m/s -> ~4.86 kn ; 42 Hz -> 2520 rpm ;
  # 358.15 K -> 85 C ; Pa -> kPa ; m3/s -> L/h ; s -> h ; ratios -> % ;
  # "port" -> instance 0, "starboard" -> instance 1 ; gear Forward -> 1 ;
  # one alarm notification -> active_dtcs 1, red lamp, fault text.
  python3 - "$LINES" <<'PY'
import re,sys
t=open(sys.argv[1]).read()
def val(pat):
    m=re.search(pat,t); return float(m.group(1)) if m else None
# Tag values may carry escaped spaces (engine_model=Yamaha\ Outboard ...).
E0=r'engine,vessel=(?:\\ |\S)*?instance=0(?:\\ |\S)* '
E1=r'engine,vessel=(?:\\ |\S)*?instance=1(?:\\ |\S)* '
checks=[("SOG m/s->kn", val(r'nav,\S* sog=([0-9.]+)'), 4.86, 0.05),
        ("RPM Hz->rpm (id 0)", val(E0+r'rpm=([0-9.]+)'), 2520, 1),
        ("RPM Hz->rpm (id starboard -> instance 1)", val(E1+r'rpm=([0-9.]+)'), 2400, 1),
        ("coolant K->C", val(E0+r'coolant_temp=([0-9.]+)'), 85.0, 0.1),
        ("oil pressure Pa->kPa (id port -> instance 0)", val(E0+r'oil_pressure=([0-9.]+)'), 340, 0.1),
        ("oil temperature K->C", val(E0+r'oil_temp=([0-9.]+)'), 90.0, 0.1),
        ("boost Pa->kPa", val(E0+r'intake_manifold_pressure=([0-9.]+)'), 186, 0.1),
        ("fuel rate m3/s->L/h", val(E0+r'fuel_rate=([0-9.]+)'), 36.0, 0.01),
        ("run time s->h", val(E0+r'runtime_h=([0-9.]+)'), 2.0, 0.001),
        ("engine load ratio->%", val(E0+r'percent_load=([0-9.]+)'), 64.0, 0.01),
        ("trim ratio->%", val(E0+r'trim_position=([0-9.]+)'), 25.0, 0.01),
        ("gear Forward->1", val(E0+r'gear=([0-9.-]+)'), 1, 0),
        ("alternator V", val(r'electrical,\S* alternator_voltage=([0-9.]+)'), 14.2, 0.001),
        ("fuel tank ratio->%", val(E0+r'fuel_level=([0-9.]+)'), 91.0, 0.01),
        ("notification -> active_dtcs", val(E0+r'active_dtcs=([0-9.]+)'), 1, 0),
        ("notification -> red lamp", val(E0+r'lamp_red=([0-9.]+)'), 1, 0)]
bad=0
for name,got,exp,tol in checks:
    if got is None or abs(got-exp)>tol:
        print(f"  [FAIL] {name}: got {got}, expected ~{exp}"); bad=1
    else:
        print(f"  [PASS] {name}: {got}")
if 'dtc_text="Low Oil Pressure (alarm)"' in t: print("  [PASS] notification -> fault text")
else: print("  [FAIL] notification -> fault text missing"); bad=1
# Engine profile heartbeat with the escaped model name, written at start.
if re.search(r'engine,vessel=\S*engine_type=yamaha_outboard,engine_model=Yamaha\\ Outboard\\ Engine\\ F115\\ B\\ 4-Stroke profile=1', t):
    print("  [PASS] engine profile heartbeat with engine_type and escaped engine_model")
else: print("  [FAIL] engine profile heartbeat missing"); bad=1
# A battery monitor publishes no chargingMode: +14.4 A into the battery
# reads CHARGING; total power is the draw (V x -I), negative while charging.
if 'charging_status="charging"' in t and val(r'electrical,\S* battery_current=([0-9.-]+)') == 14.4:
    print("  [PASS] battery current +14.4 A -> charging_status charging")
else: print("  [FAIL] charging state not derived from battery current"); bad=1
sys.exit(bad)
PY
  [ $? -eq 0 ] && PASS=$((PASS+19)) || FAIL=$((FAIL+1))
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
  readiness_profile = sim
  readiness_period_s = 2
  required_apps = pHelmIvP,grafana_bridge
  fuel_tank_l = 200
  cruise_fuel_lph = 12
  cruise_speed_kn = 6
  remote_hold_s = 2
  checklist_items = Chart Navigation System:Verify charts are loaded and current|Weather Conditions:Current and forecast weather acceptable|Port Clearance:Harbor control and departure clearance
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
    # AIS contact posted with X/Y on another datum (Y ~3,300 km, the usv-722
    # symptom) but correct LAT/LON 1000 m north of the mission origin.
    P 'NODE_REPORT=NAME=AIS367045970,TYPE=ship,TIME=1,X=0,Y=3300000,LAT=29.728730336,LON=-91.14705165,SPD=2,HDG=180'
    sleep 1
    P 'OPR_POLY_ACTUAL=pts={-100,-100:100,-100:100,100:-100,100}'
    sleep 2
    # Operator console mirrors (Swiftships-UI checklist / ENC selection).
    P 'CHECKLIST_STATUS=done=9,total=11,pending=Weather Conditions|Port Clearance,by=douglas' \
      'ENC_STATUS=selected:US5LA31M'
    sleep 3
    # Operator console region selection (iWebSocket -> BHV_OpRegionV24).
    P 'REGION_UPDATES=core_poly=pts={-3000,-3000:3000,-3000:3000,3000:0,3500:-3000,3000}'
    sleep 2
    # Production iWebSocket comms-loss failsafe: link lost, return to base,
    # arrival hold, return base from the UI.
    P 'BASE_COORDS=29.72282,-91.21539'
    sleep 1
    P COMMS_LOSS=true
    sleep 1
    P RETURN=true
    sleep 2
    P RETURN=false RETURN_HOLD=true
    sleep 2
    # Production iWebSocket COLAV_ALERTS: give-way avoidance of contato_7,
    # then stand-on in extremis, then the avoidance ends (held inactive).
    CA='"mmsi":0,"hdg":90,"spd":5,"lat":29.7,"lon":-91.2,"len":-1,"type":"ship","age":1'
    P "COLAV_ALERTS=[{\"cn\":\"contato_7\",\"active\":true,\"for\":3,\"mode\":\"giveway:stern\",$CA,\"range\":500,\"bng\":36.9,\"relBng\":316.9,\"cpa\":96.2,\"tcpa\":118}]"
    sleep 2
    P "COLAV_ALERTS=[{\"cn\":\"contato_7\",\"active\":true,\"for\":5,\"mode\":\"standon:inextremis\",$CA,\"range\":300,\"bng\":36.9,\"relBng\":316.9,\"cpa\":20,\"tcpa\":30}]"
    sleep 2
    P "COLAV_ALERTS=[{\"cn\":\"contato_7\",\"active\":false,\"for\":7,\"mode\":\"standon:inextremis\",$CA,\"range\":250,\"bng\":36.9,\"relBng\":316.9,\"cpa\":250,\"tcpa\":0}]"
    sleep 2
    # Min CPA: a ship ~39 nm out closing to a 40 km CPA must not outrank a
    # contact opening at 1.5 km (closing-first only within 12 nm).
    P NAV_SPEED=0.1 NAV_HEADING=10 NAV_X=0 NAV_Y=0 \
      'NODE_REPORT=NAME=near_open,TYPE=ship,TIME=1,X=0,Y=1500,SPD=3,HDG=0' \
      'NODE_REPORT=NAME=far_close,TYPE=ship,TIME=1,X=40000,Y=60000,SPD=5,HDG=180'
    sleep 2
    # Swiftships-UI modes: follow an AIS contact, region hold; then outside
    # autonomy the operator drives (DESIRED_THRUST from the console, no
    # REMOTE_CONTROL on the production iWebSocket) -> Remote, lets go while
    # the current carries the boat -> Moving, the boat stops -> Idle.
    P RETURN_HOLD=false WAYPOINT=false TRAILING=true
    sleep 2
    P TRAILING=false REGION=true
    sleep 2
    P REGION=false DESIRED_THRUST=25
    sleep 2
    P DESIRED_THRUST=0 DESIRED_RUDDER=0 NAV_SPEED=1.0
    sleep 4
    P NAV_SPEED=0
    sleep 4
    # Route-corridor missions: the region polygon arrives in REGION_POLY
    # (border + pts) while REGION_UPDATES carries only parameters, which
    # must not wipe the region.
    P 'REGION_POLY=border={-6571.3,-77.1:-5976.3,396.9:-6007.0,437.4:-6603.7,-38.1} # pts={-6574.0,-79.2:-5976.3,396.9:-5975.3,399.9:-6004.0,435.6:-6007.0,437.4:-6603.7,-38.1:-6605.6,-41.1:-6577.0,-76.7}'
    sleep 2
    P 'REGION_UPDATES=reset=true # breach_dist=15 # recover_speed=4'
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
    grep -q 'traffic,vessel=usv-moos contacts=[12]' "$LINES" \
      && ok "traffic summary counts the contact" || bad "traffic summary missing"
    grep -Eq 'contacts,vessel=usv-moos,contact=AIS367045970,source=report range_m=(999\.9|1000(\.0| |$))' "$LINES" \
      && ok "AIS contact positioned from LAT/LON on the mission datum (1000 m)" || bad "AIS contact still uses the poster's X/Y"
    grep -q "X/Y differ from LAT/LON" "$LINES" \
      && ok "datum-mismatch warning event emitted" || bad "datum-mismatch event missing"
    ! grep -q "reported [0-9]* nm away" "$LINES" \
      && ok "no implausible-range warning once LAT/LON is used" || bad "implausible-range warning fired"
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
    grep -q "nav,.* sog=9\.7" "$LINES"                 && ok "NAV_SPEED m/s -> kn by default (nav_speed_units unset)" || bad "NAV_SPEED default conversion changed"
    grep -q "mission,.* base_lat=29\.72282" "$LINES"   && ok "BASE_COORDS -> mission.base_lat / base_lon" || bad "BASE_COORDS not mapped"
    grep -q "Comms failsafe: no Swiftships-UI link, returning to base" "$LINES" && ok "COMMS_LOSS + RETURN -> critical comms failsafe event" || bad "comms failsafe event missing"
    grep -q "autonomy,.* comms_return=1" "$LINES"      && ok "autonomy.comms_return during the failsafe return" || bad "comms_return missing"
    grep -q "autonomy,.* operator_link=5" "$LINES"     && ok "operator_link 5 holding at base after comms loss" || bad "operator_link hold code missing"
    grep -q 'state="HoldingAtBase"' "$LINES"           && ok "RETURN_HOLD -> state HoldingAtBase"      || bad "HoldingAtBase state missing"
    grep -q 'state="Following"' "$LINES"               && ok "TRAILING -> state Following"             || bad "Following state missing"
    grep -q "autonomy,.* region_active=1" "$LINES"     && ok "REGION -> autonomy.region_active (a fence, not a state)" || bad "region_active missing"
    ! grep -q 'state="RegionHold"' "$LINES"            && ok "REGION alone does not change the mission state" || bad "REGION produced a RegionHold state"
    [ "$(grep -o 'Mission state: [A-Za-z]*' "$LINES" | tail -4 | cut -d' ' -f3 | tr '\n' ' ')" = "Idle Remote Moving Idle " ] \
      && ok "outside autonomy (region enforced): throttle -> Remote, drift -> Moving, at rest -> Idle" || bad "Remote / Moving / Idle sequence wrong"
    [ "$(grep -o 'region_vertices=[0-9]*' "$LINES" | tail -1)" = "region_vertices=8" ] \
      && ok "REGION_POLY -> operating region (8 vertices); parameter-only REGION_UPDATES leaves it" || bad "REGION_POLY region missing or wiped by REGION_UPDATES"
    grep -q "Check FAIL: Safety flags: GEOFENCE BREACH" "$LINES" && ok "readiness check turning FAIL -> critical event (popup)" || bad "readiness check FAIL event missing"
    grep -q "autonomy,.* remote_control=1" "$LINES"    && ok "Remote Control tile on while the operator drives" || bad "remote_control not derived"
    grep -q 'colav,vessel=usv-moos,contact=contato_7.*situation="Give-way, crossing astern"' "$LINES" \
      && ok "COLAV_ALERTS -> colav row with the COLREGS situation" || bad "colav situation missing"
    grep -q 'colav,vessel=usv-moos,contact=contato_7.*rule="Rule 16"' "$LINES" \
      && ok "colav row carries the COLREGS rule" || bad "colav rule missing"
    grep -Eq 'colav,vessel=usv-moos,contact=contato_7.* cpa_m=96\.2' "$LINES" \
      && ok "colav CPA from iWebSocket (96.2 m)" || bad "colav cpa missing"
    grep -q "Helm avoiding contato_7: Give-way, crossing astern (Rule 16), CPA 96 m in 118 s" "$LINES" \
      && ok "avoidance start -> warning event" || bad "avoidance start event missing"
    grep -q "IN EXTREMIS with contato_7: Stand-on, in extremis (Rule 17(b))" "$LINES" \
      && ok "in extremis -> critical event" || bad "in-extremis event missing"
    grep -q "traffic,vessel=usv-moos .*colav_extremis=1" "$LINES" \
      && ok "traffic.colav_extremis counts the in-extremis contact" || bad "colav_extremis missing"
    grep -q "traffic,vessel=usv-moos .*avoidance_active=2" "$LINES" \
      && ok "Avoidance tile reads IN EXTREMIS (2)" || bad "avoidance_active=2 missing"
    grep -q 'reason="Avoiding contato_7, Give-way, crossing astern (Rule 16)"' "$LINES" \
      && ok "Reason names the contact and the situation" || bad "colav reason missing"
    grep -q "Clear of contato_7, avoidance ended" "$LINES" \
      && ok "avoidance end -> info event" || bad "avoidance end event missing"
    grep -q 'colav,vessel=usv-moos,contact=contato_7.* active=0' "$LINES" \
      && ok "ended avoidance held as inactive row" || bad "inactive colav row missing"
    grep -Eq 'contacts,vessel=usv-moos,contact=far_close,source=report cpa_m=[34][0-9]{4}(\.| |,|$)' "$LINES" \
      && ok "far closing contact tracked (CPA ~40 km)" || bad "far contact missing"
    grep -Eq 'traffic,vessel=usv-moos .*min_cpa_m=1500(\.0*)?( |,|$)' "$LINES" \
      && ! grep -Eq 'traffic,vessel=usv-moos .*min_cpa_m=[34][0-9]{4}(\.| |,|$)' "$LINES" \
      && ok "Min CPA: opening 1.5 km contact beats a 40 km CPA ship closing from 39 nm" \
      || bad "Min CPA picked the far closing contact"
    grep -q 'arrival_state="decelerating"' "$LINES"    && ok "ARRIVAL_STATE -> mission_state"           || bad "ARRIVAL_STATE not mapped"
    grep -Eq "radar,.* targets=[12]" "$LINES"            && ok "NODE_REPORT -> radar.targets counted"           || bad "contact tracking missing"
    grep -q "radar,.* closest_range_m=500" "$LINES"    && ok "contact range from NAV_X/Y (500 m)"       || bad "contact range missing"
    grep -q "autonomy,.* crash_stop=1" "$LINES"        && ok "CRASH_STOP -> autonomy.crash_stop"        || bad "crash_stop missing"
    grep -q "CRASH STOP commanded" "$LINES"            && ok "crash-stop event emitted"                 || bad "crash-stop event missing"
    grep -q "mission,.* waypoints_remaining=5" "$LINES" && ok "waypoints_remaining derived"             || bad "waypoints_remaining missing"
    grep -q "Survey started" "$LINES" && grep -q "start_ms=" "$LINES" && ok "mission span event carries start_ms" || bad "mission start_ms missing"
    # Console mirrors and the onboard readiness evaluation (FR-12).
    grep -q 'mission_state,vessel=usv-moos checklist_done=9' "$LINES" && ok "CHECKLIST_STATUS -> mission_state.checklist_*" || bad "checklist not mirrored"
    grep -q 'enc_status="selected:US5LA31M"' "$LINES"   && ok "ENC_STATUS -> mission_state.enc_status"     || bad "enc_status missing"
    grep -q 'checklist,vessel=usv-moos,seq=003 item="Console checklist 9 / 11"' "$LINES" && grep -q 'seq=003 description="ticked by douglas"' "$LINES" \
      && ok "checklist table footer row written" || bad "checklist footer row missing"
    grep -q 'checklist,vessel=usv-moos,seq=000 state="TICKED"' "$LINES" && ok "configured item not pending reads TICKED" || bad "ticked item row missing"
    grep -q 'checklist,vessel=usv-moos,seq=001 description="Current and forecast weather acceptable"' "$LINES" \
      && ok "checklist item description from checklist_items" || bad "checklist description missing"
    grep -q 'checklist,vessel=usv-moos,seq=002 state="PENDING"' "$LINES" && ok "pending checklist items written as rows" || bad "checklist item rows missing"
    grep -q 'check=apps evidence="1 / 2 running · missing: pHelmIvP"' "$LINES" && ok "Apps check against DB_CLIENTS" || bad "apps check missing"
    grep -q '_measurement=\\"readiness\\" AND vessel=\\"usv-moos\\"' "$TMP/deletes.txt" 2>/dev/null \
      && ok "stale readiness rows erased at bridge start" || bad "readiness erase at start missing"
    grep -q 'mission,vessel=usv-moos route_length_m=2800' "$LINES" && ok "route length from VIEW_SEGLIST (2800 m)" || bad "route length missing"
    grep -q 'mission,vessel=usv-moos route_in_region=0' "$LINES" && ok "route vs region test (outside)" || bad "route_in_region missing"
    grep -q 'check=endurance evidence="route 1.5 nm needs 0.4 h (x1.5) · no fuel level"' "$LINES" \
      && ok "endurance planned against the route" || bad "endurance evidence wrong"
    grep -q 'mission,vessel=usv-moos region_vertices=4' "$LINES" && ok "OPR_POLY_ACTUAL -> mission.region_vertices" || bad "region_vertices missing"
    grep -q 'mission,vessel=usv-moos region_vertices=5' "$LINES" && grep -q 'geometry,vessel=usv-moos,kind=region,idx=005 ' "$LINES" \
      && ok "console REGION_UPDATES -> operating region (closed outline)" || bad "REGION_UPDATES region missing"
    [ "$(grep -c 'readiness,vessel=usv-moos,check=' "$LINES")" -ge 64 ] \
      && ok "readiness rows written per check (seq/status/name/evidence)" || bad "readiness rows missing"
    grep -q 'readiness_summary,vessel=usv-moos ready=' "$LINES" && ok "readiness summary written" || bad "readiness summary missing"
    grep -q 'readiness_summary,vessel=usv-moos profile="sim"' "$LINES" && ok "readiness_profile=sim applied" || bad "readiness profile not applied"
    grep -q 'check=checklist evidence="9 / 11 ticked · pending: Weather Conditions,Port Clearance"' "$LINES" \
      && ok "console checklist evaluated (pending items listed)" || bad "checklist evaluation missing"
    grep -q 'check=enc status=0' "$LINES"               && ok "ENC selected -> check PASS"                || bad "ENC check not passing"
    grep -q 'check=course evidence="R-17 · 5 waypoints · 1.5 nm · region 4 vertices · ROUTE OUTSIDE REGION"' "$LINES" \
      && ok "course planning check sees route, waypoints, length, region and the outside-region fault" || bad "course check evidence wrong"
    grep -q 'check=comms evidence="Signal K UP · Internet UP · 38 ms"' "$LINES" \
      && ok "InfluxDB poller feeds Signal K / Internet into the comms check" || bad "poller values missing in comms check"
    grep -q 'check=can evidence="interface UP · 212 msg/s"' "$LINES" && ok "CAN bus check from can_bridge data" || bad "CAN check missing"
    grep -q 'check=obc evidence="CPU 4 % · RAM 11 % · disk 63 %"' "$LINES" && ok "host metrics check from Telegraf data" || bad "OBC check missing"
    grep -q 'check=engine status=2' "$LINES"            && ok "sim profile: no engine data reads N/A"    || bad "engine N/A missing"
    grep -q 'check=safety status=1' "$LINES"            && ok "crash stop / breach fails the safety check" || bad "safety check not failing"
    grep -q 'Readiness NOT READY' "$LINES"              && ok "readiness change event emitted"           || bad "readiness event missing"
    grep -q 'r.cpu == "cpu-total"' "$TMP/queries.txt" 2>/dev/null \
      && ok "poller query restricts cpu to cpu-total" || bad "poller query not issued / wrong"
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

hdr "CAN bridge replay of recorded buses (usv-722 unchanged, usv-725 engine from J1939)"
# can_bridge CAN_REPLAY=<candump log> decodes a recorded log through the same
# code as the live bus. Fixtures (tools/fixtures): usv-722 traffic rebuilt
# from the 9 Oct 2026 captures with its v1.2.27 output, and excerpts of the
# usv-725 captures of 15 Oct 2026 (engine off).
FX="$REPO/tools/fixtures"
can_replay() {   # can_replay <env file> <vessel> <gz log> -> $LINES, stderr in $TMP/replay.err
  zcat "$3" > "$TMP/replay.log"
  start_stub
  USV_ENV_FILE="$1" VESSEL_ID="$2" CAN_REPLAY="$TMP/replay.log" \
    timeout 60 "$REPO/ingestion/can-cpp/build/can_bridge" >/dev/null 2>"$TMP/replay.err"
  sleep 0.5; stop_stub
}
lp_has() { grep -E "$1" "$LINES" >/dev/null; }
if [[ -x "$REPO/ingestion/can-cpp/build/can_bridge" && -f "$FX/can-usv722.log.gz" ]]; then
  # 1. usv-722 settings on usv-722 traffic: line-for-line the v1.2.27 output.
  can_replay "$FX/usv722.env.txt" usv-722 "$FX/can-usv722.log.gz"
  if diff <(grep -v '^$' "$LINES") <(zcat "$FX/can-usv722.expected.lp.gz") >/dev/null; then
    ok "usv-722 replay: $(zcat "$FX/can-usv722.expected.lp.gz" | wc -l) lines identical to v1.2.27 (rudder, thrust, gear, trim, key, DM1)"
  else
    bad "usv-722 replay differs from v1.2.27"
    diff <(grep -v '^$' "$LINES") <(zcat "$FX/can-usv722.expected.lp.gz") | head -5
  fi
  grep -q "WARNING" "$TMP/replay.err" && bad "profile warning on the usv-722 bus" || ok "usv-722 bus, HELM_PROFILE=usv722: no profile warning"
  lp_has 'helm_rudder=-50( |$)' && lp_has 'helm_rudder=50( |$)' \
    && ok "usv-722 wheel 0 / 1012 -> -50 / +50 degrees" || bad "usv-722 wheel scale wrong"
  ! lp_has 'engine,vessel=usv-722.* (rpm|coolant_temp|oil_temp|speed_request)=' \
    && ok "usv-722: no J1939 engine fields from can_bridge (ENGINE_SOURCE=signalk)" || bad "usv-722 got engine fields from can_bridge"

  # 2. usv-725 settings on usv-725 traffic.
  can_replay "$FX/usv725.env.txt" usv-725 "$FX/can-usv725.log.gz"
  grep -q "WARNING" "$TMP/replay.err" && bad "profile warning on the usv-725 bus" || ok "usv-725 bus, HELM_PROFILE=usv725: no profile warning"
  lp_has 'engine,vessel=usv-725,instance=0,engine_type=mercury_diesel.* rpm=0' \
    && ok "usv-725 engine rpm from EEC1, tagged mercury_diesel" || bad "usv-725 rpm missing"
  lp_has ' coolant_temp=3[0-9]( |$)' && lp_has ' oil_temp=3[0-9]\.' \
    && ok "usv-725 coolant and oil temperature from ET1 (oil 16-bit, about 33 C)" || bad "usv-725 ET1 temperatures wrong"
  lp_has ' oil_pressure=100( |$)' && lp_has ' intake_manifold_pressure=100( |$)' \
    && ok "usv-725 oil pressure and boost 100 kPa (engine off)" || bad "usv-725 oil pressure / boost missing"
  lp_has ' fuel_level=99\.4' && ok "usv-725 fuel level 99.5 % from the N2K tank sender (127505)" || bad "usv-725 fuel level missing"
  lp_has ' ecu_voltage=11\.' && lp_has ' percent_load=0' && lp_has ' fuel_rate=0' \
    && ok "usv-725 ECU voltage, load and fuel rate" || bad "usv-725 VEP1 / EEC2 / LFE missing"
  lp_has 'dtc_text="SPN 639 FMI 9' && lp_has ' lamp_protect=1' && lp_has ' active_dtcs=1' \
    && ok "usv-725 engine DM1: protect lamp, SPN 639 FMI 9" || bad "usv-725 DM1 missing"
  lp_has 'helm_rudder=-17( |$)' && lp_has 'helm_rudder=17( |$)' \
    && ok "usv-725 wheel 10 / 1022 -> -17 / +17 degrees" || bad "usv-725 wheel scale wrong"
  lp_has 'rudder_actual=-16\.[6-9]' && lp_has 'rudder_actual=16\.[6-9]' \
    && ok "usv-725 actuator position reaches both locks (0x210, byte 5 = 64)" || bad "usv-725 actuator position wrong"
  lp_has ' throttle_pct=100( |$)' && lp_has ' speed_request=4200( |$)' \
    && ok "usv-725 full lever: thrust 100 %, speed request 4200 rpm" || bad "usv-725 thrust / speed request wrong"
  lp_has ' trim_switch=1( |$)' && lp_has ' key_state=1( |$)' && lp_has ' gear=0( |$)' \
    && ok "usv-725 trim up (0CFF08D0), key RUN (00FF0F10), gear neutral" || bad "usv-725 trim / key / gear missing"
  # A fault from the Glendinning device (address 210) must not reach the
  # engine fault fields on usv-725 (the engine's own DM1 owns them).
  zcat "$FX/can-usv725.log.gz" > "$TMP/inj.log"
  LAST=$(grep -o '^([0-9.]*' "$TMP/inj.log" | tail -1 | tr -d '(')
  python3 -c "print('(%.6f) slcan0 0CFECAD2#04FF6E000302FFFF' % ($LAST + 0.5))" >> "$TMP/inj.log"
  gzip -c "$TMP/inj.log" > "$TMP/inj.log.gz"
  can_replay "$FX/usv725.env.txt" usv-725 "$TMP/inj.log.gz"
  ! lp_has 'SPN 110 FMI 3' && lp_has 'dtc_text="SPN 639 FMI 9' \
    && ok "usv-725: a fault from address 210 does not overwrite the engine's DM1" || bad "usv-725 DM1 overwritten by address 210"

  # 3. Wrong profile for the bus: logged, so it is caught on the boat.
  can_replay "$FX/usv722.env.txt" usv-725 "$FX/can-usv725.log.gz"
  grep -q "WARNING: this CAN bus carries usv-725 frames" "$TMP/replay.err" \
    && ok "usv-725 bus with HELM_PROFILE=usv722: warning logged" || bad "no warning for usv-725 bus on usv722 profile"
  can_replay "$FX/usv725.env.txt" usv-722 "$FX/can-usv722.log.gz"
  grep -q "WARNING: this CAN bus carries usv-722 frames" "$TMP/replay.err" \
    && ok "usv-722 bus with HELM_PROFILE=usv725: warning logged" || bad "no warning for usv-722 bus on usv725 profile"
else
  skip "CAN replay (can_bridge or fixtures missing)"
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
[ "$FAIL" -eq 0 ]
