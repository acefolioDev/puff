#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
[[ "$(uname -s)" == Darwin ]] || { echo 'Run on macOS or the macOS GitHub Actions runner.'; exit 1; }
bash scripts/build-native.sh
brew_prefix="$(brew --prefix)"
stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
app="$stage/My Puff.app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
install -m755 dist/puff "$app/Contents/MacOS/puff-bin"
iconset="$stage/Puff.iconset"
mkdir -p "$iconset"
rsvg-convert -w 1024 -h 1024 packaging/puff.svg -o "$stage/icon.png"
for size in 16 32 128 256 512; do
 sips -z "$size" "$size" "$stage/icon.png" --out "$iconset/icon_${size}x${size}.png" >/dev/null
 retina=$((size * 2))
 sips -z "$retina" "$retina" "$stage/icon.png" --out "$iconset/icon_${size}x${size}@2x.png" >/dev/null
done
iconutil -c icns "$iconset" -o "$app/Contents/Resources/Puff.icns"
rm -rf "$iconset" "$stage/icon.png"
cat > "$app/Contents/MacOS/puff" <<LAUNCHER
#!/bin/bash
export XDG_DATA_DIRS="$brew_prefix/share:\${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
export PATH="$brew_prefix/opt/libpq/bin:$brew_prefix/bin:\$PATH"
exec "\$(dirname "\$0")/puff-bin" "\$@"
LAUNCHER
chmod 755 "$app/Contents/MacOS/puff"
cat > "$app/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>io.github.puff.Puff</string>
<key>CFBundleName</key><string>My Puff</string>
<key>CFBundleExecutable</key><string>puff</string>
<key>CFBundleIconFile</key><string>Puff</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>0.2.0</string>
<key>CFBundleVersion</key><string>3</string>
<key>NSHighResolutionCapable</key><true/>
</dict></plist>
PLIST
cp LICENSE "$app/Contents/Resources/"
cat > "$stage/INSTALL.txt" <<'INSTALL'
My Puff preview for macOS (unsigned, Homebrew runtime required).
Install the same CPU architecture's Homebrew, then run:
  brew install gtk4 libadwaita gtksourceview5 libpq sqlite
Drag My Puff.app to Applications. This preview links Homebrew libraries;
it is not a self-contained distribution. No signing or notarization is claimed.
Remove My Puff.app to uninstall. Local settings remain in ~/.config/puff.
Saved credentials use macOS Keychain under io.github.puff.Connection.
INSTALL
plutil -lint "$app/Contents/Info.plist"
"$app/Contents/MacOS/puff" --version
hdiutil create -volname 'My Puff Preview' -srcfolder "$stage" -ov -format UDZO "dist/puff-0.2.0-$(uname -m)-preview.dmg"
