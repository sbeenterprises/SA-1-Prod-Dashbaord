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
curl -fsSL https://repos.influxdata.com/influxdata-archive.key \
  | gpg --dearmor -o /etc/apt/keyrings/influxdata.gpg
echo "deb [signed-by=/etc/apt/keyrings/influxdata.gpg] https://repos.influxdata.com/debian stable main" \
  > /etc/apt/sources.list.d/influxdata.list
curl -fsSL https://apt.grafana.com/gpg.key \
  | gpg --dearmor -o /etc/apt/keyrings/grafana.gpg
echo "deb [signed-by=/etc/apt/keyrings/grafana.gpg] https://apt.grafana.com stable main" \
  > /etc/apt/sources.list.d/grafana.list

apt-get update -q
say "Installing influxdb2, telegraf, grafana"
apt-get install -y -q influxdb2 telegraf grafana

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
if cmake -S "$REPO/ingestion/moos-cpp" -B "$REPO/ingestion/moos-cpp/build" \
     -DCMAKE_BUILD_TYPE=Release $MOOS_HINT; then
  cmake --build "$REPO/ingestion/moos-cpp/build" -j"$(nproc)"
  install -m 755 "$REPO/ingestion/moos-cpp/build/moos_bridge" /usr/local/bin/
  say "moos_bridge installed — add 'Run = grafana_bridge' to your mission file"
else
  echo "WARNING: MOOS not found. Re-run with MOOS_DIR=<path to MOOSConfig.cmake>"
  echo "         (source tree: ~/moos-ivp/build/MOOS/MOOSCore)"
fi

say "Installing services"
id -u usv &>/dev/null || useradd -r -s /usr/sbin/nologin usv
cp "$REPO"/deploy/systemd/*.service /etc/systemd/system/
systemctl daemon-reload
systemctl enable --now usv-signalk-bridge usv-can-bridge
# usv-simulator.service installed but NOT enabled (bench testing only)

# ---------------------------------------------------------------- grafana
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

grafana-cli admin reset-admin-password "$GRAFANA_ADMIN_PASSWORD" \
  --homepath /usr/share/grafana --config /etc/grafana/grafana.ini 2>/dev/null || true
sed -i 's/^;*default_theme =.*/default_theme = dark/' /etc/grafana/grafana.ini

# Branding: Swiftships logo in the top-left nav + favicon.
# OSS Grafana has no white-label config, so we overwrite the shipped image
# assets; an apt upgrade of grafana restores the originals, so RE-RUN this
# installer after any Grafana upgrade to re-apply branding.
if [[ -f "$REPO/deploy/branding/swiftships_logo.svg" ]]; then
  say "Applying Swiftships branding"
  GPUB=/usr/share/grafana/public
  cp "$REPO/deploy/branding/swiftships_logo.svg" "$GPUB/img/grafana_icon.svg"
  if command -v rsvg-convert &>/dev/null; then
    rsvg-convert -w 32 -h 32 "$REPO/deploy/branding/swiftships_logo.svg" \
      -o "$GPUB/img/fav32.png" 2>/dev/null || true
  fi
fi
systemctl enable --now grafana-server
systemctl restart grafana-server

say "Done. Grafana: http://$(hostname -I | awk '{print $1}'):3000 (folder 'USV Monitoring')"
say "Reminder: merge ingestion/moos-cpp/example.moos into your mission file."
