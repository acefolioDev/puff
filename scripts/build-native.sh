#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p dist/objects
compiler="${CXX:-c++}"
packages=(gtk4 libadwaita-1 gtksourceview-5 libpq sqlite3)
platform_compile=(-fstack-protector-strong -D_FORTIFY_SOURCE=3 -fPIE)
platform_link=(-pie -Wl,-z,relro,-z,now)
if [[ "$(uname -s)" == Darwin ]]; then
 platform_compile=(-fstack-protector-strong)
 platform_link=(-framework Security -framework CoreFoundation)
else
 packages+=(libsecret-1)
fi
# Fail before compiling when dependency discovery fails.
include_text="$(pkg-config --cflags "${packages[@]}")"
link_text="$(pkg-config --libs "${packages[@]}")"
read -r -a include_flags <<< "$include_text"
read -r -a link_flags <<< "$link_text"
object_temp=""
binary_temp=""
trap 'rm -f "$object_temp" "$binary_temp"' EXIT
common=(-std=c++20 -O2 -g -Wall -Wextra -Wpedantic -Wno-deprecated-declarations -Wno-missing-field-initializers "${platform_compile[@]}" -pthread -Iapp)
objects=()
for source_file in app/*.cpp; do
 object_file="dist/objects/$(basename "${source_file%.cpp}").o"
 if [[ ! -f "$object_file" || "$source_file" -nt "$object_file" ]] || [[ -n "$(find app -name '*.hpp' -newer "$object_file" -print -quit 2>/dev/null)" ]]; then
  echo "CXX $source_file"
  object_temp="$object_file.tmp.$$"
  "$compiler" "${common[@]}" "${include_flags[@]}" -c "$source_file" -o "$object_temp"
  mv -f "$object_temp" "$object_file"
  object_temp=""
 fi
 objects+=("$object_file")
done
binary_temp="$(mktemp dist/puff.XXXXXX)"
"$compiler" -pthread "${platform_link[@]}" "${objects[@]}" "${link_flags[@]}" -o "$binary_temp"
chmod 755 "$binary_temp"
mv -f "$binary_temp" dist/puff
binary_temp=""
printf 'Built %s\n' "$PWD/dist/puff"
