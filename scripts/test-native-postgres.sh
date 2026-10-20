#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
[[ -x dist/puff-tests ]] || { echo 'Run make test first'; exit 1; }
cluster_dir="$(mktemp -d /tmp/puff-native-pg.XXXXXX)"
trap 'pg_ctl -D "$cluster_dir/data" -m immediate stop >/dev/null 2>&1 || true; rm -rf "$cluster_dir"' EXIT
initdb -D "$cluster_dir/data" -A trust --no-locale --encoding=UTF8 >/dev/null
cat > "$cluster_dir/data/pg_hba.conf" <<'HBA'
local all all trust
host all all 127.0.0.1/32 scram-sha-256
HBA
pg_ctl -D "$cluster_dir/data" -l "$cluster_dir/log" -o "-k $cluster_dir -p 55439 -h 127.0.0.1" -w start >/dev/null
PUFF_TEST_PG="$cluster_dir" ./dist/puff-tests
