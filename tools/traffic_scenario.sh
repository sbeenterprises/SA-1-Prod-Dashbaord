#!/bin/bash
# End-to-end traffic test on the simulation host: spawns scripted contacts
# through pContactSpawn (and one radar-only target through RADAR_TARGET) so
# the Traffic dashboard, the collision-risk event and the
# usv-collision-risk alert can be checked against known situations.
#
#   tools/traffic_scenario.sh <mission.moos> [step|all|clean]
#
# Steps (each runs ~90 s, watch the Traffic tab while it plays):
#   crossing   contact 800 m off the starboard bow crossing left  -> MEDIUM/HIGH
#   headon     reciprocal contact 1200 m dead ahead               -> HIGH
#   overtaking slow contact 600 m ahead on the same course        -> LOW / MEDIUM
#   radar      radar-only target (RADAR_TARGET, refreshed every 3 s) -> "Radar Only" = 1
#   opening    contact astern moving away                         -> CLEAR, TCPA "OPENING"
#   all        the five in sequence, cleaning between them
#   clean      remove spawned contacts
#
# pContactSpawn takes an ABSOLUTE contact heading; the scenarios compute
# it from the boat's current heading (read from InfluxDB, or OWN_HDG=<deg>).
# Run it while the boat is under way on a steady leg.
set -euo pipefail

MISSION=${1:?usage: traffic_scenario.sh <mission.moos> [step|all|clean]}
STEP=${2:-all}
HOLD=${HOLD:-90}
VESSEL=${VESSEL:-alpha}

command -v uPokeDB >/dev/null || { echo "uPokeDB not on PATH (source the MOOS environment)"; exit 1; }

# Own heading: taken from the dashboard's own data (nav.heading of $VESSEL
# in InfluxDB, credentials in /etc/usv/usv.env, read with sudo) unless
# OWN_HDG is given. Scenario headings are relative to it, so a wrong value
# turns a head-on into a crossing.
if [[ -z ${OWN_HDG:-} ]]; then
  if ENV=$(sudo -n cat /etc/usv/usv.env 2>/dev/null); then
    eval "$(echo "$ENV" | grep -E '^INFLUX_(URL|ORG|TOKEN)=')"
    Q="from(bucket: \"telemetry\") |> range(start: -60s) |> filter(fn: (r) => r._measurement == \"nav\" and r._field == \"heading\" and r.vessel == \"$VESSEL\") |> last()"
    OWN_HDG=$(curl -s -XPOST "$INFLUX_URL/api/v2/query?org=$INFLUX_ORG" \
                -H "Authorization: Token $INFLUX_TOKEN" -H 'Content-Type: application/vnd.flux' \
                -d "$Q" | awk -F, 'NR==2 {gsub(/\r/, "", $7); printf "%d", $7}')
  fi
  if [[ -z ${OWN_HDG:-} ]]; then
    echo "own heading unavailable (no sudo / no nav data): pass OWN_HDG=<deg>"; exit 1
  fi
  echo "own heading $OWN_HDG deg (from nav.heading of $VESSEL)"
fi

poke() { uPokeDB "$MISSION" "$@" >/dev/null 2>&1; echo "  -> $*"; }
hdg() { echo $(( ( (OWN_HDG + $1) % 360 + 360) % 360 )); }   # own heading + offset
clean() { poke SPAWN_CLEAN=true; }
hold() { echo "  holding ${HOLD}s (watch the Traffic tab)"; sleep "$HOLD"; }

run_step() {
  case "$1" in
    crossing)
      echo "[crossing] 800 m, 40 deg off the starboard bow, crossing right to left at 4 m/s"
      poke "SPAWN_CONTACT=heading=$(hdg -90),relative_bearing=40,distance=800,speed=4,name=TGT-27" ;;
    headon)
      echo "[head-on] 1200 m dead ahead, reciprocal course at 4 m/s"
      poke "SPAWN_CONTACT=heading=$(hdg 180),relative_bearing=0,distance=1200,speed=4,name=SEA-HAWK" ;;
    overtaking)
      echo "[overtaking] 600 m ahead, same course, 1.5 m/s"
      poke "SPAWN_CONTACT=heading=$(hdg 0),relative_bearing=0,distance=600,speed=1.5,name=TUG-12" ;;
    radar)
      # A radar reports every sweep; the bridge drops a target not refreshed
      # within contact_ttl_s (10 s), so re-send every 3 s with the range and
      # TCPA closing, as a real track would.
      echo "[radar] radar-only target 900 m on the port bow, closing, no report (refreshed every 3 s)"
      local range=900 tcpa=140 t=0
      while (( t < HOLD )); do
        poke "RADAR_TARGET=id=R-31,range=$range,bearing=$(hdg -41),spd=3,cog=$(hdg 90),cpa=350,tcpa=$tcpa"
        sleep 3; t=$((t + 3)); range=$((range > 400 ? range - 15 : range)); tcpa=$((tcpa > 20 ? tcpa - 3 : tcpa))
      done
      return ;;
    opening)
      echo "[opening] 500 m astern, moving away at 3 m/s"
      poke "SPAWN_CONTACT=heading=$(hdg 180),relative_bearing=180,distance=500,speed=3,name=FERRY-2" ;;
    clean)
      echo "[clean] removing spawned contacts"; clean; return ;;
    *) echo "unknown step: $1"; exit 1 ;;
  esac
  hold
  clean
}

if [[ $STEP == all ]]; then
  for s in crossing headon overtaking radar opening; do run_step "$s"; sleep 5; done
  echo "done. Expected on the Traffic tab: risk HIGH during head-on, a critical"
  echo "'Collision risk HIGH' event in Mission Events, the usv-collision-risk alert"
  echo "firing on the Alarms tab, Radar Only = 1 during the radar step."
else
  run_step "$STEP"
fi
