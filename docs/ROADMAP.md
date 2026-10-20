# Puff engineering priorities

## Architecture and scope

`application.cpp` owns windows, connection profiles and tabs. `workspace.cpp`
contains the editor, schema navigation, bounded virtual result grid, SQL review
and async UI callbacks. `Session` serializes each connection's work on a worker
thread and marshals completion to GTK. `Driver` isolates PostgreSQL and SQLite
operations. `Store` maintains profile/history/snippet metadata in SQLite;
passwords use Secret Service on Linux and Keychain on macOS. `files.cpp` handles
exports and PostgreSQL logical backup/restore subprocesses.

Preserve these seams rather than introducing a web runtime or background service.
A later UI refactor should separate connection forms, schema tools and result-grid
controllers as their workflows grow. Engine capabilities should move to an
explicit descriptor when a third adapter or additional object types are added.

## Current implementation cycle

- Expose Create Database alongside Connect existing. Without a writable PostgreSQL
  tab, guide users through connecting to a maintenance database first.
- Review quoted CREATE DATABASE SQL, preserve real permission errors and async
  execution, then prefill a new connection to the created database without copying
  credentials or overwriting the original saved profile.
- Group advanced settings and pair related connection fields. Keep form actions
  and feedback outside scrollable content; lock fields during
  operations. Hide PostgreSQL-only connection fields for SQLite.
- Fix the clipped overflow menu with natural width propagation and explicit width;
  label it Database tools and hide PostgreSQL-only operations for SQLite.
- Reject malformed numeric settings rather than accepting partial numbers.
- Maintain profile Save/Delete functionality even with four workspaces open.
- Extend PostgreSQL creation/permission tests, GTK short-window form validation and
  rendered menu-width checks. Optional snapshots capture only the disposable test UI.
- Add native Debian packaging/CI, protect RPM builds against stale binaries, and
  prepare a manually invoked macOS Homebrew preview build with native Keychain.

## Next complete workflows, in order

1. Verify the creation flow visually on Fedora at multiple window sizes, keyboard
   navigation and both themes. Add real PostgreSQL UI automation for the maintenance
   connection → SQL review → creation → new-tab connection path.
2. Connection management page: saved-profile search, explicit connection status,
   existing PostgreSQL database chooser, profile import/export without secrets,
   reconnect and safe saved-profile editing. Keep SQLite file creation separate.
3. SQL workspace: query tabs within a connection, saved query/history search,
   formatting and context-aware completion. Keep bounded streaming and cancellation.
4. Server-side table filters/sorting and column selection; preserve primary-key
   predicates, optimistic conflict checks and transaction review for data changes.
5. Structured object inspector and relationship exploration, followed by inspectable
   schema change plans. Capability/permission gates precede editing controls.
6. CSV import with preview, column mapping, per-row errors, transactional execution
   and cancellation. Keep page exports distinct from restorable native backups.
7. Diagnostics and role inspection on demand, with engine and privilege explanations.
8. Self-contained macOS bundle, integration testing, signing and
   notarization. Add release checksums and clean-host installation/upgrade tests.

These are outstanding work, not claims of implemented product functionality.
