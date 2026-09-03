#!/bin/bash
# Native installer for the USV monitoring stack on the onboard computer.
# Debian/Ubuntu (amd64 or arm64). Run as root from the repo root:
#   sudo deploy/install.sh
#
# Installs: InfluxDB 2.x, Grafana OSS, Telegraf (apt), the C++ bridges
# (moos_bridge for pAntler, can_bridge as systemd service), the Python
# Signal K bridge, provisioning, buckets and the downsample task.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
ENV_FILE=/etc/usv/usv.env

say() { echo -e "\033[1;32m==> $*\033[0m"; }

[[ $EUID -eq 0 ]] || { echo "run as root: sudo deploy/install.sh"; exit 1; }

# ---------------------------------------------------------------- config
if [[ ! -f $ENV_FILE ]]; then
  mkdir -p /etc/usv
  cp "$REPO/deploy/usv.env.example" $ENV_FILE
  chmod 600 $ENV_FILE
  echo "Created $ENV_FILE — edit secrets now, then re-run install."
  exit 1
fi
# shellcheck disable=SC1090
source $ENV_FILE
if [[ $INFLUX_TOKEN == CHANGE_ME* || $INFLUX_PASSWORD == CHANGE_ME* ]]; then
  echo "Edit $ENV_FILE first (CHANGE_ME values present)."; exit 1
fi

# ---------------------------------------------------------------- apt repos
say "Adding InfluxData and Grafana apt repositories"
apt-get update -q
apt-get install -y -q curl gnupg ca-certificates apt-transport-https \
  g++ cmake make git libcurl4-openssl-dev nlohmann-json3-dev

install -d /etc/apt/keyrings
# --batch --yes: don't stop the installer with an interactive overwrite
# prompt when the keyring already exists (idempotent re-runs).
# The influxdata-archive-keyring package (pulled in by influxdb2) owns
# /etc/apt/sources.list.d/influxdata.list as a conffile. Writing our own copy
# makes dpkg stop with an interactive "modified configuration file" prompt
# on the very first install, so bootstrap the repo only while the keyring
# package is absent, and never overwrite the file it manages.
if ! dpkg -s influxdata-archive-keyring >/dev/null 2>&1 && \
   [[ ! -f /etc/apt/sources.list.d/influxdata.list ]]; then
  curl -fsSL https://repos.influxdata.com/influxdata-archive.key \
    | gpg --batch --yes --dearmor -o /etc/apt/keyrings/influxdata.gpg
  echo "deb [signed-by=/etc/apt/keyrings/influxdata.gpg] https://repos.influxdata.com/debian stable main" \
    > /etc/apt/sources.list.d/influxdata.list
fi
curl -fsSL https://apt.grafana.com/gpg.key \
  | gpg --batch --yes --dearmor -o /etc/apt/keyrings/grafana.gpg
echo "deb [signed-by=/etc/apt/keyrings/grafana.gpg] https://apt.grafana.com stable main" \
  > /etc/apt/sources.list.d/grafana.list

apt-get update -q
say "Installing influxdb2, telegraf, grafana"
# Non-interactive, keep any locally modified conffiles, retry slow CDNs
# (apt.grafana.com is 375 MB and has timed out in the field).
DEBIAN_FRONTEND=noninteractive apt-get install -y -q \
  -o Dpkg::Options::="--force-confold" \
  -o Acquire::Retries=10 -o Acquire::http::Timeout=120 -o Acquire::https::Timeout=120 \
  influxdb2 telegraf grafana

# ---------------------------------------------------------------- influxdb
say "Starting InfluxDB and creating org/buckets/token"
systemctl enable --now influxdb
for i in $(seq 1 30); do influx ping &>/dev/null && break; sleep 1; done

if ! influx auth list --token "$INFLUX_TOKEN" &>/dev/null; then
  influx setup --skip-verify --force \
    --username "$INFLUX_USERNAME" --password "$INFLUX_PASSWORD" \
    --org "$INFLUX_ORG" --bucket "$INFLUX_BUCKET" \
    --retention "$TELEMETRY_RETENTION" --token "$INFLUX_TOKEN" \
    || echo "influx already set up — continuing with existing token"
fi
influx bucket create --name events --org "$INFLUX_ORG" \
  --retention 365d --token "$INFLUX_TOKEN" 2>/dev/null || true
influx bucket create --name telemetry_1m --org "$INFLUX_ORG" \
  --retention 365d --token "$INFLUX_TOKEN" 2>/dev/null || true
influx task create --org "$INFLUX_ORG" --token "$INFLUX_TOKEN" --file - <<'EOF' 2>/dev/null || true
option task = {name: "downsample-telemetry-1m", every: 1m, offset: 10s}

from(bucket: "telemetry")
    |> range(start: -task.every)
    |> filter(fn: (r) => r._measurement != "events" and r._measurement != "alarms")
    // string fields cannot be averaged: mission_state is all strings, the
    // others are the named fields (nav fix type, charging state, geometry
    // is a vertex set, not a time series)
    |> filter(fn: (r) => r._measurement != "mission_state" and r._measurement != "geometry")
    |> filter(fn: (r) => r._field != "fix_type" and r._field != "charging_status")
    |> aggregateWindow(every: 1m, fn: mean, createEmpty: false)
    |> to(bucket: "telemetry_1m")
EOF

# ---------------------------------------------------------------- telegraf
say "Configuring Telegraf"
sed -e "s|__INFLUX_TOKEN__|$INFLUX_TOKEN|" \
    -e "s|__INFLUX_ORG__|$INFLUX_ORG|" \
    -e "s|__VESSEL_ID__|$VESSEL_ID|" \
    "$REPO/telegraf/telegraf.conf" > /etc/telegraf/telegraf.conf
systemctl enable --now telegraf
systemctl restart telegraf

# ---------------------------------------------------------------- bridges
say "Building C++ bridges"
# IXWebSocket (Signal K bridge dependency) — build once from source
if [[ ! -d /usr/local/lib/cmake/ixwebsocket ]]; then
  say "Building IXWebSocket"
  rm -rf /tmp/IXWebSocket
  git clone --depth 1 https://github.com/machinezone/IXWebSocket.git /tmp/IXWebSocket
  cmake -S /tmp/IXWebSocket -B /tmp/IXWebSocket/build \
    -DCMAKE_BUILD_TYPE=Release -DUSE_TLS=OFF -DBUILD_SHARED_LIBS=OFF
  cmake --build /tmp/IXWebSocket/build -j"$(nproc)"
  cmake --install /tmp/IXWebSocket/build
fi

cmake -S "$REPO/ingestion/can-cpp" -B "$REPO/ingestion/can-cpp/build" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "$REPO/ingestion/can-cpp/build" -j"$(nproc)"
install -m 755 "$REPO/ingestion/can-cpp/build/can_bridge" /usr/local/bin/

cmake -S "$REPO/ingestion/signalk-cpp" -B "$REPO/ingestion/signalk-cpp/build" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "$REPO/ingestion/signalk-cpp/build" -j"$(nproc)"
install -m 755 "$REPO/ingestion/signalk-cpp/build/signalk_bridge" /usr/local/bin/

cmake -S "$REPO/simulator" -B "$REPO/simulator/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$REPO/simulator/build" -j"$(nproc)"
install -m 755 "$REPO/simulator/build/usv_simulator" /usr/local/bin/

if [[ -n "${MOOS_DIR:-}" ]]; then
  MOOS_HINT="-DMOOS_DIR=$MOOS_DIR"
else
  MOOS_HINT=""
fi
# A build dir configured for the unit tests only (-DMISSION_LOGIC_TESTS_ONLY=ON,
# as tools/smoke_test.sh and the docs do) caches that switch; a later
# configure with MOOS_DIR would silently skip moos_bridge. Reset it.
if grep -qs "MISSION_LOGIC_TESTS_ONLY:BOOL=ON" \
     "$REPO/ingestion/moos-cpp/build/CMakeCache.txt"; then
  rm -rf "$REPO/ingestion/moos-cpp/build"
fi
if cmake -S "$REPO/ingestion/moos-cpp" -B "$REPO/ingestion/moos-cpp/build" \
     -DCMAKE_BUILD_TYPE=Release -DMISSION_LOGIC_TESTS_ONLY=OFF $MOOS_HINT; then
  cmake --build "$REPO/ingestion/moos-cpp/build" -j"$(nproc)"
  install -m 755 "$REPO/ingestion/moos-cpp/build/moos_bridge" /usr/local/bin/
  # pAntler launches the app as 'grafana_bridge' (see example.moos) — make
  # sure that name resolves on PATH.
  ln -sf /usr/local/bin/moos_bridge /usr/local/bin/grafana_bridge
  # Commissioning helper (moos_poke VAR=VAL ...), documented in deployment.md.
  [ -x "$REPO/ingestion/moos-cpp/build/moos_poke" ] && \
    install -m 755 "$REPO/ingestion/moos-cpp/build/moos_poke" /usr/local/bin/
  say "moos_bridge installed (as grafana_bridge) — add 'Run = grafana_bridge' to your mission file"
  # If pAntler runs from a systemd unit with a private PATH (as on usv01:
  # moos-antler.service sets PATH to the two MOOS bin dirs only), pAntler
  # cannot exec /usr/local/bin/grafana_bridge ("Failed exec - not good").
  # Extend that unit's PATH with a drop-in instead of editing the unit.
  if [[ -n "${MOOS_ANTLER_UNIT:-}" ]] && systemctl cat "$MOOS_ANTLER_UNIT" &>/dev/null; then
    cur=$(systemctl show "$MOOS_ANTLER_UNIT" -p Environment --value | tr ' ' '\n' | sed -n 's/^PATH=//p')
    if [[ -n "$cur" && "$cur" != *"/usr/local/bin"* ]]; then
      say "Adding /usr/local/bin to PATH of $MOOS_ANTLER_UNIT (drop-in)"
      install -d "/etc/systemd/system/$MOOS_ANTLER_UNIT.d"
      printf '[Service]\nEnvironment=PATH=%s:/usr/local/bin:/usr/bin:/bin\n' "$cur" \
        > "/etc/systemd/system/$MOOS_ANTLER_UNIT.d/10-usv-path.conf"
      systemctl daemon-reload
      echo "         restart it after merging example.moos: systemctl restart $MOOS_ANTLER_UNIT"
    fi
  fi
else
  echo "WARNING: MOOS not found. Re-run with MOOS_DIR=<path to MOOSConfig.cmake>"
  echo "         (source tree: ~/moos-ivp/build/MOOS/MOOSCore)"
fi

say "Installing services"
id -u usv &>/dev/null || useradd -r -s /usr/sbin/nologin usv
cp "$REPO"/deploy/systemd/*.service /etc/systemd/system/
systemctl daemon-reload
systemctl enable --now usv-can-bridge
# usv-simulator.service installed but NOT enabled (bench testing only)

# Signal K bridges: one per vessel when VESSELS is set in usv.env
# ("alpha:3000 bravo:3001 ..."), each an instance of the template unit
# usv-signalk-bridge@<vessel> with its own /etc/usv/vessels/<vessel>.env.
# Without VESSELS the single usv-signalk-bridge (SIGNALK_URL) is used.
if [[ -n "${VESSELS:-}" ]]; then
  say "Configuring per-vessel Signal K bridges: $VESSELS"
  install -d -m 750 -o root -g usv /etc/usv/vessels
  systemctl disable --now usv-signalk-bridge 2>/dev/null || true
  for spec in $VESSELS; do
    name=${spec%%:*}; port=${spec##*:}
    printf 'SIGNALK_URL=ws://localhost:%s/signalk/v1/stream?subscribe=all\nVESSEL_ID=%s\n' \
      "$port" "$name" > "/etc/usv/vessels/$name.env"
    chmod 640 "/etc/usv/vessels/$name.env"; chown root:usv "/etc/usv/vessels/$name.env"
    systemctl enable --now "usv-signalk-bridge@$name"
    systemctl restart "usv-signalk-bridge@$name"
  done
else
  systemctl enable --now usv-signalk-bridge
fi

# ---------------------------------------------------------------- grafana
# grafana-cli needs an explicit homepath when run outside /usr/share/grafana;
# without it every plugin/admin command fails with "Could not find config
# defaults" and the failure is easy to miss.
GCLI="grafana-cli --homepath /usr/share/grafana"

say "Installing Grafana data source plugin (InfluxDB)"
# Grafana >= 13.0.2 ships the InfluxDB data source as an EXTERNAL plugin
# (it was removed from core). Without it, the provisioned datasource shows
# "Data source not found" and every panel errors. On Grafana <= 12 the
# datasource is core and this block is skipped.
if [[ -d /usr/share/grafana/public/app/plugins/datasource/influxdb ]]; then
  say "InfluxDB data source is built-in (Grafana <= 12) — nothing to install"
elif $GCLI plugins ls 2>/dev/null | grep -q '^influxdb'; then
  say "influxdb data source plugin already installed"
elif $GCLI plugins install influxdb 2>/dev/null; then
  chown -R grafana:grafana /var/lib/grafana/plugins 2>/dev/null || true
else
  echo "WARNING: could not install the influxdb data source plugin."
  echo "         On Grafana 13+ ALL dashboards will show 'Data source not"
  echo "         found' until it is installed:"
  echo "           sudo grafana-cli --homepath /usr/share/grafana plugins install influxdb"
fi

say "Installing Grafana panel plugins (analog instruments)"
# Business Charts (Apache ECharts) renders the analog needle gauges,
# compass and rudder indicator. Signed + React-based (safe on Grafana 11+).
# Pin a version via ECHARTS_PANEL_VERSION in /etc/usv/usv.env if desired.
if $GCLI plugins ls 2>/dev/null | grep -q volkovlabs-echarts-panel; then
  say "volkovlabs-echarts-panel already installed"
elif $GCLI plugins install volkovlabs-echarts-panel \
       ${ECHARTS_PANEL_VERSION:-} 2>/dev/null; then
  chown -R grafana:grafana /var/lib/grafana/plugins 2>/dev/null || true
else
  echo "WARNING: could not install volkovlabs-echarts-panel (no internet?)."
  echo "         Analog instrument panels will show 'panel not found'."
  echo "         Either re-run install.sh with internet, or regenerate"
  echo "         plugin-free dashboards on your workstation:"
  echo "           python3 tools/gen_dashboards.py --classic"
  echo "         and re-run install.sh to provision them."
fi

say "Provisioning Grafana"
mkdir -p /var/lib/grafana/dashboards
cp "$REPO"/grafana/dashboards/*.json /var/lib/grafana/dashboards/
cp -r "$REPO"/grafana/provisioning/dashboards /etc/grafana/provisioning/
cp -r "$REPO"/grafana/provisioning/alerting /etc/grafana/provisioning/
sed -e "s|__INFLUX_TOKEN__|$INFLUX_TOKEN|" \
    -e "s|__INFLUX_ORG__|$INFLUX_ORG|" \
    "$REPO/grafana/provisioning/datasources/influxdb.yml" \
    > /etc/grafana/provisioning/datasources/influxdb.yml
chown -R root:grafana /etc/grafana/provisioning
chmod 640 /etc/grafana/provisioning/datasources/influxdb.yml
chown -R grafana:grafana /var/lib/grafana/dashboards

# Set the admin password from usv.env. Do NOT swallow failures silently —
# a failed reset leaves the operator locked out with no hint why.
if ! $GCLI admin reset-admin-password "$GRAFANA_ADMIN_PASSWORD" \
       --config /etc/grafana/grafana.ini; then
  echo "WARNING: could not set the Grafana admin password."
  echo "         Log in with admin/admin (fresh install) or reset manually:"
  echo "           sudo grafana-cli --homepath /usr/share/grafana admin reset-admin-password <password>"
fi
sed -i 's/^;*default_theme =.*/default_theme = dark/' /etc/grafana/grafana.ini
# The vessel's Signal K server owns port 3000, so Grafana moves to
# GRAFANA_PORT (default 3001). Set GRAFANA_PORT in /etc/usv/usv.env to
# change it; re-run this installer to apply.
sed -i "s/^;*http_port =.*/http_port = ${GRAFANA_PORT:-3001}/" /etc/grafana/grafana.ini
# Opening Grafana lands on the pre-departure Readiness dashboard (the
# "home" for every user and org, unless a user picks another in their
# preferences). GRAFANA_HOME_DASHBOARD in usv.env overrides the file name.
HOME_DASH=/var/lib/grafana/dashboards/${GRAFANA_HOME_DASHBOARD:-usv-readiness.json}
if grep -q '^default_home_dashboard_path' /etc/grafana/grafana.ini; then
  sed -i "s|^default_home_dashboard_path =.*|default_home_dashboard_path = $HOME_DASH|" /etc/grafana/grafana.ini
elif grep -q '^\[dashboards\]' /etc/grafana/grafana.ini; then
  sed -i "s|^\[dashboards\]|[dashboards]\ndefault_home_dashboard_path = $HOME_DASH|" /etc/grafana/grafana.ini
else
  printf '\n[dashboards]\ndefault_home_dashboard_path = %s\n' "$HOME_DASH" >> /etc/grafana/grafana.ini
fi

# Branding (logo, favicon, login page, title): deploy/brand.sh. Grafana OSS
# has no white-label config, so it overwrites shipped assets; an apt
# upgrade of grafana restores them until this installer is re-run.
say "Applying Swiftships branding"
bash "$REPO/deploy/brand.sh"

systemctl enable --now grafana-server
systemctl restart grafana-server

say "Done. Grafana: http://$(hostname -I | awk '{print $1}'):${GRAFANA_PORT:-3001} (folder 'Swift Autonomous Dashboard')"
say "Reminder: merge ingestion/moos-cpp/example.moos into your mission file."
