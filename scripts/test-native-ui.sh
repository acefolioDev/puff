#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_root="$(mktemp -d /tmp/puff-ui.XXXXXX)"
trap 'rm -rf "$test_root"' EXIT
python3 - "$test_root/sample.sqlite" <<'PY'
import sqlite3,sys
connection=sqlite3.connect(sys.argv[1])
connection.execute('CREATE TABLE t(id INTEGER PRIMARY KEY,value TEXT)')
connection.executemany('INSERT INTO t VALUES (?,?)',((i,'row '+str(i)) for i in range(1000)))
connection.commit()
connection.close()
PY
XDG_CONFIG_HOME="$test_root/config" G_DEBUG=fatal-criticals ./dist/puff --self-test "$test_root/sample.sqlite"
