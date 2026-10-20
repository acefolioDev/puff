#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
bash scripts/build-native.sh
compiler="${CXX:-c++}"
packages=(glib-2.0 gio-2.0 libpq sqlite3)
platform_link=()
if [[ "$(uname -s)" == Darwin ]]; then
 platform_link=(-framework Security -framework CoreFoundation)
else
 packages+=(libsecret-1)
fi
read -r -a includes <<< "$(pkg-config --cflags "${packages[@]}")"
read -r -a libraries <<< "$(pkg-config --libs "${packages[@]}")"
"$compiler" -std=c++20 -O2 -g -Wall -Wextra -pthread -Iapp "${includes[@]}" tests/native/core_tests.cpp dist/objects/{core,postgres,sqlite,store,session,files}.o "${libraries[@]}" "${platform_link[@]}" -o dist/puff-tests
./dist/puff-tests

[[ "$(uname -s)" == Darwin ]] && exit 0

secret_test_root="$(mktemp -d /tmp/puff-secret-test.XXXXXX)"
trap 'rm -rf "$secret_test_root"' EXIT
DBUS_SESSION_BUS_ADDRESS="unix:path=$secret_test_root/nonexistent" PUFF_TEST_SECRET_FAILURE=1 ./dist/puff-tests
