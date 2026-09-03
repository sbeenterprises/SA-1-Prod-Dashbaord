#!/usr/bin/env python3
"""Fake AIS / radar traffic for the Traffic dashboard (simulation host).

pContactSpawn drives one contact at a time; this feeds a whole scene:
several vessels around own ship, each with its own name, type, course
and speed, published as NODE_REPORT every 2 s (exactly what an AIS
receiver or pContactMgr would see), plus radar-only tracks published as
RADAR_TARGET. Own position and heading come from the dashboard's own
data (nav.lat/lon/heading in InfluxDB); positions are converted to the
MOOS local grid with the mission file's datum.

    tools/fake_traffic.py <mission.moos> [--duration 300] [--scene harbour|busy]
                          [--vessel alpha] [--period 2]

Scenes (bearings relative to own heading, ranges in metres):
  harbour  6 contacts: a crossing ferry (MEDIUM/HIGH), a head-on workboat
           (HIGH), a slow tug being overtaken, an anchored barge, a fast
           small craft passing astern, a distant cargo ship, plus one
           radar-only track.
  busy     the harbour scene plus four more AIS ships further out.

Stop with Ctrl-C; contacts expire from the dashboard within 10 s.
"""
import argparse
import math
import os
import re
import subprocess
import sys
import time
import urllib.request

NM = 1852.0


def read_env():
    """INFLUX_URL / ORG / TOKEN: from the environment if set, otherwise from
    /etc/usv/usv.env (root-only, so sudo may prompt for the password)."""
    keys = ("INFLUX_URL", "INFLUX_ORG", "INFLUX_TOKEN")
    if all(k in os.environ for k in keys):
        return {k: os.environ[k] for k in keys}
    try:
        text = subprocess.run(["sudo", "cat", "/etc/usv/usv.env"],
                              stdout=subprocess.PIPE, text=True, check=True).stdout
    except subprocess.CalledProcessError:
        sys.exit("cannot read /etc/usv/usv.env (sudo refused); "
                 "or export INFLUX_URL INFLUX_ORG INFLUX_TOKEN")
    env = dict(re.findall(r'^(INFLUX_(?:URL|ORG|TOKEN))=(.*)$', text, re.M))
    return {k: v.strip().strip('"') for k, v in env.items()}


def own_state(env, vessel):
    """Latest lat, lon, heading (deg), sog (m/s) of the vessel."""
    q = (f'from(bucket: "telemetry") |> range(start: -60s) '
         f'|> filter(fn: (r) => r._measurement == "nav" and r.vessel == "{vessel}" and '
         f'(r._field == "lat" or r._field == "lon" or r._field == "heading" or r._field == "sog")) '
         f'|> last()')
    req = urllib.request.Request(f'{env["INFLUX_URL"]}/api/v2/query?org={env["INFLUX_ORG"]}',
                                 data=q.encode(), method="POST",
                                 headers={"Authorization": f'Token {env["INFLUX_TOKEN"]}',
                                          "Content-Type": "application/vnd.flux"})
    csv = urllib.request.urlopen(req, timeout=5).read().decode()
    out = {}
    header = None
    for line in csv.splitlines():
        cols = line.split(",")
        if len(cols) < 8:
            header = None
            continue
        if cols[1] == "result":
            header = cols
            continue
        if header:
            row = dict(zip(header, cols))
            out[row["_field"]] = float(row["_value"])
    if not {"lat", "lon", "heading"} <= out.keys():
        sys.exit(f"no recent nav data for {vessel} in InfluxDB (is the boat running?)")
    return out["lat"], out["lon"], out["heading"], out.get("sog", 0.0) / 1.94384


def datum(mission):
    text = open(mission).read()
    lat = re.search(r'^\s*LatOrigin\s*=\s*([-\d.]+)', text, re.M)
    lon = re.search(r'^\s*LongOrigin\s*=\s*([-\d.]+)', text, re.M)
    if not (lat and lon):
        sys.exit("LatOrigin / LongOrigin not found in the mission file")
    return float(lat.group(1)), float(lon.group(1))


def to_local(lat, lon, lat0, lon0):
    phi = math.radians(lat0)
    m_lat = 111132.92 - 559.82 * math.cos(2 * phi) + 1.175 * math.cos(4 * phi)
    m_lon = 111412.84 * math.cos(phi) - 93.5 * math.cos(3 * phi)
    return (lon - lon0) * m_lon, (lat - lat0) * m_lat


class Contact:
    def __init__(self, name, ctype, rel_brg, rng, rel_course, spd, radar=False):
        self.name, self.type, self.radar = name, ctype, radar
        self.rel_brg, self.rng, self.rel_course, self.spd = rel_brg, rng, rel_course, spd
        self.x = self.y = 0.0
        self.hdg = 0.0

    def place(self, ox, oy, own_hdg):
        brg = math.radians(own_hdg + self.rel_brg)
        self.x = ox + self.rng * math.sin(brg)
        self.y = oy + self.rng * math.cos(brg)
        self.hdg = (own_hdg + self.rel_course) % 360

    def step(self, dt):
        self.x += self.spd * dt * math.sin(math.radians(self.hdg))
        self.y += self.spd * dt * math.cos(math.radians(self.hdg))


SCENES = {
    "harbour": [
        Contact("FERRY-ACADIANA", "ship", 40, 900, -100, 4.5),   # crossing from starboard
        Contact("WB-CAJUN",       "ship", 2, 1400, 180, 3.5),    # head-on
        Contact("TUG-ATCH",       "tug", -5, 650, 0, 1.2),       # slow, being overtaken
        Contact("BARGE-7",        "barge", -70, 500, 0, 0.0),    # anchored on the port bow
        Contact("RIB-SWIFT",      "kayak", 160, 400, 70, 8.0),   # fast small craft astern
        Contact("MV-DELTA-QUEEN", "ship", 110, 2600, -160, 5.0), # distant cargo
        Contact("R-31",           "radar", -41, 900, 90, 1.5, radar=True),
    ],
}
SCENES["busy"] = SCENES["harbour"] + [
    Contact("CREW-BOAT-4", "ship", -120, 1800, 60, 6.0),
    Contact("SHRIMPER-2",  "ship", 75, 2200, 200, 2.0),
    Contact("PUSH-BOAT-9", "tug", -160, 1500, 0, 1.8),
    Contact("PATROL-1",    "ship", 20, 3000, 190, 7.0),
]


def poke(mission, *args):
    subprocess.run(["uPokeDB", mission, *args], stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mission")
    ap.add_argument("--duration", type=float, default=300)
    ap.add_argument("--scene", choices=sorted(SCENES), default="harbour")
    ap.add_argument("--vessel", default=os.environ.get("VESSEL", "alpha"))
    ap.add_argument("--period", type=float, default=2.0)
    a = ap.parse_args()

    env = read_env()
    lat0, lon0 = datum(a.mission)
    lat, lon, own_hdg, own_spd = own_state(env, a.vessel)
    ox, oy = to_local(lat, lon, lat0, lon0)
    print(f"own ship {a.vessel}: lat {lat:.5f} lon {lon:.5f} hdg {own_hdg:.0f} "
          f"sog {own_spd * 1.94384:.1f} kn -> local x {ox:.0f} y {oy:.0f}")

    scene = SCENES[a.scene]
    for c in scene:
        c.place(ox, oy, own_hdg)
    print(f"scene '{a.scene}': {len(scene)} contacts, publishing every {a.period:.0f} s "
          f"for {a.duration:.0f} s (Ctrl-C to stop)")

    t0 = time.time()
    last = t0
    refreshed = t0
    try:
        while time.time() - t0 < a.duration:
            now = time.time()
            dt = now - last
            last = now
            if now - refreshed > 5:      # follow the boat for the radar geometry
                try:
                    lat, lon, own_hdg, own_spd = own_state(env, a.vessel)
                    ox, oy = to_local(lat, lon, lat0, lon0)
                except Exception:
                    pass
                refreshed = now
            for c in scene:
                c.step(dt)
                if c.radar:
                    # A radar reports polar data plus its own CPA / TCPA
                    # ($RATTM): compute them from the relative motion.
                    dx, dy = c.x - ox, c.y - oy
                    rng = math.hypot(dx, dy)
                    brg = math.degrees(math.atan2(dx, dy)) % 360
                    vx = c.spd * math.sin(math.radians(c.hdg)) - own_spd * math.sin(math.radians(own_hdg))
                    vy = c.spd * math.cos(math.radians(c.hdg)) - own_spd * math.cos(math.radians(own_hdg))
                    v2 = vx * vx + vy * vy
                    tcpa = -(dx * vx + dy * vy) / v2 if v2 > 1e-6 else -1
                    cpa = math.hypot(dx + vx * tcpa, dy + vy * tcpa) if tcpa > 0 else rng
                    poke(a.mission, f"RADAR_TARGET=id={c.name},range={rng:.0f},bearing={brg:.0f},"
                                    f"spd={c.spd * 1.94384:.1f},cog={c.hdg:.0f},cpa={cpa:.0f},"
                                    f"tcpa={tcpa:.0f}")
                else:
                    poke(a.mission, f"NODE_REPORT=NAME={c.name},TYPE={c.type},TIME={now:.2f},"
                                    f"X={c.x:.1f},Y={c.y:.1f},SPD={c.spd:.2f},HDG={c.hdg:.1f},"
                                    f"DEPTH=0,LENGTH=20,MODE=DRIVE")
            time.sleep(max(0.0, a.period - (time.time() - now)))
    except KeyboardInterrupt:
        pass
    print("stopped; contacts expire from the dashboard within 10 s")


if __name__ == "__main__":
    main()
