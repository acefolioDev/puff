# Building and installing Puff

Puff remains a C++20 GTK4/libadwaita desktop application. PostgreSQL and existing
SQLite files are supported. A new server engine, SQLite file creation, database
deletion UI, schema diagrams, CSV import and migration planning are not implemented.

## Linux

Build each package on its target distribution. Do not rename or convert the
Fedora executable into a Debian package: GLIBC, C++ and GTK dependencies differ.

Fedora 44 build dependencies:

```sh
sudo dnf install gcc-c++ make pkgconf-pkg-config gtk4-devel libadwaita-devel \
  gtksourceview5-devel libpq-devel sqlite-devel libsecret-devel rpm-build \
  desktop-file-utils clang-tools-extra postgresql-server postgresql
make test integration
make rpm
sudo dnf install ./dist/puff-0.2.0-2*.rpm
```

Debian 13 (trixie) build dependencies:

```sh
sudo apt-get install g++ make pkg-config libgtk-4-dev libadwaita-1-dev \
  libgtksourceview-5-dev libpq-dev libsqlite3-dev libsecret-1-dev dpkg-dev \
  desktop-file-utils xvfb xauth dbus-x11 python3
make test
xvfb-run -a dbus-run-session -- make ui-test
make deb
sudo apt-get install ./dist/puff_0.2.0-2_*.deb
```

The Debian script resolves ELF dependencies with dpkg-shlibdeps. The RPM script
uses RPM's ELF scanner and refuses a package missing the libpq dependency. Both
scripts build before packaging to avoid distributing a stale executable. Desktop
entries, scalable icons and AppStream metadata are installed system-wide.
Runtime packages are declared in each installer; PostgreSQL logical backup and
restore additionally need compatible pg_dump/pg_restore executables. Password
storage on Linux needs a running, unlocked Secret Service provider.

Launch with `puff` or the desktop launcher. Install a newer package using the same
command to upgrade. Uninstall with `sudo dnf remove puff` or `sudo apt remove puff`.
User settings/history remain under the XDG configuration directory, normally
`~/.config/puff`. Removing the package does not erase database contents or keyring
credentials. Remove saved profiles inside Puff to delete their saved credentials.

## CI artifacts

The Fedora and Debian workflows compile, run core/SQLite and GTK lifecycle tests,
and upload their native installers. Fedora additionally tests a temporary real
PostgreSQL cluster. CI requires pushing this source to a GitHub repository with
Actions enabled. No remote is configured in this checkout; no workflow run or
uploaded artifact is implied by the presence of these files.

## macOS preview

GitHub's `macos-15` hosted runner can build the application without a personally
owned Mac. Run the **macOS preview** workflow manually in GitHub Actions. It
installs Homebrew dependencies, compiles, runs core/SQLite tests and creates an
unsigned DMG containing an application bundle and installation instructions.
The credential implementation uses the native Security framework and Keychain;
it never falls back to storing passwords in the configuration database.

This preview deliberately depends on Homebrew at its standard prefix and matching
CPU architecture. On the destination Mac, install Homebrew and run:

```sh
brew install gtk4 libadwaita gtksourceview5 libpq sqlite
```

Then drag My Puff.app from the DMG into Applications. Remove that application to
uninstall. Configuration and Keychain entries persist. The exact configuration
location follows GLib's platform configuration directory.

The workflow has not been executed from this workspace. Native Keychain behavior,
GUI launch, signing, notarization and installation on a second Mac still need
validation. A self-contained signed release additionally needs bundled GTK runtime
resources/dylibs and Apple Developer signing/notarization credentials; the preview
must not be represented as that release.

References: [GitHub macOS runners](https://docs.github.com/en/actions/reference/runners/github-hosted-runners),
[GTK on macOS](https://docs.gtk.org/gtk4/osx.html).
