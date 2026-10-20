#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cluster_dir="$(mktemp -d /tmp/puff-pg-ui.XXXXXX)"
trap 'pg_ctl -D "$cluster_dir/data" -m immediate stop >/dev/null 2>&1 || true; rm -rf "$cluster_dir"' EXIT
initdb -D "$cluster_dir/data" -A trust --no-locale --encoding=UTF8 >/dev/null
pg_ctl -D "$cluster_dir/data" -l "$cluster_dir/log" -o "-k $cluster_dir -p 55439 -h ''" -w start >/dev/null
createdb -h "$cluster_dir" -p 55439 test
psql -h "$cluster_dir" -p 55439 -d postgres -v ON_ERROR_STOP=1 -c "CREATE TABLE t(id INTEGER PRIMARY KEY,value TEXT); INSERT INTO t SELECT n, 'row ' || n FROM generate_series(0,999) n" >/dev/null
XDG_CONFIG_HOME="$cluster_dir/config" G_DEBUG=fatal-criticals ./dist/puff --self-test-postgres "$cluster_dir"
