#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
command -v rpmbuild >/dev/null || { echo 'Install build tools: sudo dnf install rpm-build desktop-file-utils'; exit 1; }
bash scripts/build-native.sh
desktop-file-validate packaging/puff.desktop
project_root="$PWD"
rpm_root="$(mktemp -d)"
trap 'rm -rf "$rpm_root"' EXIT
mkdir -p "$rpm_root"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}
# A source archive is also produced for normal source-RPM rebuilding.
tar --transform='s,^,puff-0.2.0/,' -czf "$rpm_root/SOURCES/puff-0.2.0.tar.gz" app scripts tests/native sql packaging Makefile .clang-format LICENSE README.md docs .github
cp "$rpm_root/SOURCES/puff-0.2.0.tar.gz" dist/
cp dist/puff "$rpm_root/SOURCES/puff"
cp packaging/puff.spec "$rpm_root/SPECS/"
rpmbuild --define "_topdir $rpm_root" --define "_tmppath $rpm_root" --define 'puff_prebuilt 1' -bb "$rpm_root/SPECS/puff.spec"
# Refuse a package built with a missing ELF dependency scanner.
while IFS= read -r built_rpm; do
 requirements="$(rpm -qp --requires "$built_rpm")"
 [[ "$requirements" == *"libpq.so.5"* ]] || { echo 'ELF runtime dependency generation failed; use a complete rpm-build installation'; exit 1; }
done < <(find "$rpm_root/RPMS" -name '*.rpm')
find "$rpm_root/RPMS" -name '*.rpm' -exec cp {} "$project_root/dist/" \;
echo 'Native RPM and source archive written to dist/'
