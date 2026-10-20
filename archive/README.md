# Frozen transition snapshots

- `puff-0.1.0-webview-source.tar.gz`: original working Bun/Preact application, tests, build scripts, packaging, lockfile and README. The source is preserved for recovery and comparison. No credentials or database files are included.
- `puff-native-python-prototype.tar.gz`: the intermediate GTK prototype and its tests, retained as a reference for the compiled port. It is not the current runtime or a maintained UI.

The only active application implementation is C++20 in `app/`. Normal builds do not unpack, import, or depend on either snapshot. Old 0.1.0 artifacts in `dist/` are superseded by the 0.2.0 native RPM.
