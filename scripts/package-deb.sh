#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
command -v dpkg-deb >/dev/null
command -v dpkg-shlibdeps >/dev/null
bash scripts/build-native.sh
project_root="$PWD"
stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/root/DEBIAN" "$stage/debian"
cat > "$stage/debian/control" <<'CONTROL'
Source: puff
Section: database
Priority: optional
Maintainer: Puff contributors <puff@localhost>

Package: puff
Architecture: any
Description: Native PostgreSQL and SQLite workspace
CONTROL
# Resolve dependencies on the Debian build host; never repackage a Fedora ELF.
dependencies="$(cd "$stage" && dpkg-shlibdeps -O -e"$project_root/dist/puff")"
dependencies="${dependencies#shlibs:Depends=}"
architecture="$(dpkg --print-architecture)"
cat > "$stage/root/DEBIAN/control" <<CONTROL
Package: puff
Version: 0.2.0-2
Section: database
Priority: optional
Architecture: $architecture
Maintainer: Puff contributors <puff@localhost>
Depends: $dependencies, hicolor-icon-theme
Recommends: postgresql-client
Description: Native PostgreSQL and SQLite database workspace
 SQL editing, bounded results, schema inspection, and safe data editing.
CONTROL
install -Dm755 dist/puff "$stage/root/usr/bin/puff"
strip --strip-unneeded "$stage/root/usr/bin/puff"
install -Dm644 packaging/puff.desktop "$stage/root/usr/share/applications/io.github.puff.Puff.desktop"
install -Dm644 packaging/puff.svg "$stage/root/usr/share/icons/hicolor/scalable/apps/puff.svg"
install -Dm644 packaging/io.github.puff.Puff.metainfo.xml "$stage/root/usr/share/metainfo/io.github.puff.Puff.metainfo.xml"
install -Dm644 LICENSE "$stage/root/usr/share/doc/puff/copyright"
dpkg-deb --root-owner-group --build "$stage/root" "dist/puff_0.2.0-2_${architecture}.deb"
dpkg-deb --info "dist/puff_0.2.0-2_${architecture}.deb"
