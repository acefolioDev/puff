# Fedora revision 3 — SQL review and database discovery

## Implemented

- Replaced the unbounded message-dialog SQL body with a read-only SQL editor in a
  scroll container. Apply/Cancel remain in the fixed footer. Complete SQL is kept;
  the old 12,000-byte preview truncation is removed.
- Preserved exact database-name confirmation for production operations. A callback
  failure keeps the review open and displays its error; successful callbacks close
  it safely, including callbacks that destroy the parent window.
- Added PostgreSQL database discovery to the driver and a sidebar dropdown with
  current-database display and manual refresh. Only connectable non-template
  databases are offered (the current database remains identifiable).
- Discovery uses a separate short-lived read-only connection, preserving existing
  query streams. Choosing another database opens a prefilled connection form for
  a new tab, without overwriting the old profile or copying its password into UI.
- Added the sample SQL fixture and real PostgreSQL GUI regression script to source
  archives and CI. Debian/macOS workflows remain manual and were not executed.

## Verification

Passed on Fedora 44 x86_64:

- Native C++ build and clang-format checks; shell syntax checks.
- 46 core/SQLite checks and the unavailable Secret Service check.
- 84 cumulative PostgreSQL checks, including database creation/discovery/removal,
  template exclusion and CONNECT privilege filtering. The exact sample seed runs
  through the application's query driver, creates 1,000 customers, and rejects
  repeat execution and the wrong database.
- SQLite and PostgreSQL GTK suites under authenticated Xvfb at 1600×1000, with
  fatal GTK criticals enabled. Checks include actual database selection, preserved
  current-database labeling, empty password prefill, pagination during discovery
  refresh, complete 1,000-line SQL preview, visible actions in a short window,
  typed confirmation, callback error retention and successful Apply.

Headless portal/EGL warnings did not fail the functional suites. The rendered
1,000-line SQL review was inspected; its capture is `ui-validation/sql-review.png`.
Optional snapshots wait for the first painted frame and are not required by the
functional suites.

## Limits

Database metadata is capped at 1,000 rows and reports an error above that limit.
Discovery requires another transient server connection; connection limits or
server failures show an error with a refresh action. SQLite shows its existing
file rather than a PostgreSQL-style server catalog. Selecting a database opens
connection settings/new tab; it does not replace an active session in place.

The RPM is unsigned. Package contents and the packaged executable are validated;
the user's installed application is not automatically upgraded. Close Puff,
install the new RPM, and reopen it. macOS builds remain unexecuted and Debian
builds remain deferred as requested.
