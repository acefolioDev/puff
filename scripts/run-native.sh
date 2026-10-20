#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
[[ -x dist/puff ]] || bash scripts/build-native.sh
exec ./dist/puff "$@"
