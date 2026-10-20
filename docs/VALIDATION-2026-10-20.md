# Validation record — 20 October 2026

## Fedora 44

The host initially lacked a compiler and development headers. System package
installation required a sudo password, so a temporary SDK was downloaded from
Fedora repositories and extracted under `/tmp`; no system packages were installed.
The native build used GCC 16 and the host runtime libraries. All source files
compiled with the existing warning and hardening flags.

Passed:

- clang-format dry-run with Werror across all C++ headers, sources and tests.
- Core and real SQLite tests: 46 checks.
- Unavailable Secret Service test: 1 check, no plaintext fallback.
- Disposable real PostgreSQL cluster: 76 cumulative checks, including database
  creation, duplicate names, reconnect to a newly created quoted name, read-only
  rejection, CREATEDB permission failure/recovery, cancellation and backup/restore.
- GTK lifecycle integration with fatal criticals enabled: schema navigation,
  virtual result grid, table/raw pagination, staging/discard, schema dialog,
  result release, short-window connection action visibility, engine-specific
  credential visibility, malformed-port validation, creation onboarding, and
  natural-width database-tools popovers. Captures were inspected at short and
  normal dialog sizes.
- Native RPM generation with desktop-entry validation and automatic ELF dependency
  verification. The packaged executable was extracted and passed --version plus
  the full GTK integration scenario.
- Build failure recovery: an intentionally failing linker left the prior working
  binary byte-for-byte unchanged. Failed outputs are now written to temporary
  files and published only on success.

The sandbox could not open the desktop display or bind PostgreSQL sockets; those
integration runs used the approved host execution path. No interactive sudo
password was collected. The installed system application was not upgraded.

## Debian and macOS

Debian: native container build was started but stopped at the user's request to
prioritize Fedora delivery. No new Debian installer was produced. Reproducible
packaging and CI scripts remain in the source for later work. Dependency fetching
used verified HTTPS endpoints and the existing host trust bundle; no certificate
or package signature checks were disabled.

macOS: workflow and native Keychain implementation are prepared but unexecuted.
No macOS artifact, signing, notarization or GUI validation is claimed.

## Limits

These checks do not establish that the entire DBMS mission is complete. They do
not substitute for visual QA at multiple resolutions/themes, clean Fedora package
installation/upgrade, macOS testing, or the outstanding workflows in ROADMAP.md.
