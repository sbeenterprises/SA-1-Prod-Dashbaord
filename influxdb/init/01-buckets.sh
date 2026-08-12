#!/bin/bash
# Runs once on first InfluxDB startup (docker-entrypoint-initdb.d).
# Creates additional buckets beyond the default "telemetry" bucket, plus
# downsampling tasks for long-term mission analysis.
set -e

# events: mission events + alarms, kept 1 year
influx bucket create --name events --org "${DOCKER_INFLUXDB_INIT_ORG}" --retention 365d || true

# telemetry_1m: downsampled telemetry, kept 1 year (mission replay / analysis)
influx bucket create --name telemetry_1m --org "${DOCKER_INFLUXDB_INIT_ORG}" --retention 365d || true

# Downsample task: 1-minute means of all numeric telemetry
influx task create --org "${DOCKER_INFLUXDB_INIT_ORG}" --file - <<'EOF' || true
option task = {name: "downsample-telemetry-1m", every: 1m, offset: 10s}

from(bucket: "telemetry")
    |> range(start: -task.every)
    |> filter(fn: (r) => r._measurement != "events" and r._measurement != "alarms")
    |> aggregateWindow(every: 1m, fn: mean, createEmpty: false)
    |> to(bucket: "telemetry_1m")
EOF
