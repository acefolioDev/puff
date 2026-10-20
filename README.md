# My Puff

A compiled C++20 database workspace using native GTK4/libadwaita widgets, GtkSourceView, PostgreSQL's libpq, SQLite and desktop Secret Service. No browser engine, embedded web frontend, local HTTP server or interpreter is required.

This is a tested native core release, with remaining roadmap work listed below—not a claim that every database administration feature is complete.

## Build and run on Fedora

```bash
sudo dnf install gcc-c++ make pkgconf-pkg-config gtk4-devel libadwaita-devel \
  gtksourceview5-devel libpq-devel sqlite-devel libsecret-devel
make
./dist/puff
```

Requires C++20, GTK ≥4.12, libadwaita ≥1.4, GtkSourceView 5 and libpq ≥17. Tested on Fedora 44 x86_64 with GCC 16.2.1. System libraries are dynamically linked; `dist/puff` is a native ELF executable. The release RPM strips debug data; the local build retains debugging information. Native source lives in `app/`.

## Install the Fedora package

```bash
sudo dnf install rpm-build desktop-file-utils
make rpm
sudo dnf install ./dist/puff-0.2.0-1.fc44.x86_64.rpm
```

Launch **My Puff** from GNOME/KDE application search, or run `puff`. The RPM replaces the older `puff` package. Runtime libraries are declared automatically from ELF dependencies. PostgreSQL command-line tools are recommended for backup/restore:

```bash
sudo dnf install postgresql
```

Uninstall with `sudo dnf remove puff`. Your private profiles/history and desktop keyring entries are retained; remove saved credentials through the connection dialog before uninstalling if desired. No install script changes PostgreSQL authentication, services, SELinux settings or database contents.

The RPM installs `/usr/bin/puff`, a desktop entry, scalable icon, AppStream metadata, documentation and license. Fedora's desktop/icon triggers perform indexing. `packaging/puff.spec` also supports a normal source build from `dist/puff-0.2.0.tar.gz`; `scripts/package-rpm.sh` packages the already-tested local binary using the same specification. Build on each supported Fedora architecture/release; only Fedora 44 x86_64 is verified here.

## Connect locally

Click **Connect**. Typical PostgreSQL settings:

- Engine `postgres`; host `localhost`; port `5432`; user `postgres` or your own role; database `postgres`.
- Enter that PostgreSQL role's password. Linux login passwords may differ.
- Use TLS `disable` for an intentionally unencrypted local instance, or `verify-full` for authenticated remote TLS. `prefer` can fall back to unencrypted; `require` encrypts without validating server identity.
- **Read-only is enabled by default. Disable it deliberately to create databases/tables or edit rows.** Use a separate restricted database role for production; a privileged user can override PostgreSQL session settings through explicit SQL.
- Select development/staging/production and a project/group. Production SQL and data/schema changes require entering the target database name.
- **Test** checks a connection without saving it. **Save** stores the settings without opening it. **Connect** opens a new tab and saves settings. Nothing connects automatically at startup.

Saved profiles appear in the connection dialog, grouped by project. Ctrl+N opens connection settings; Ctrl+T prefills a new tab from the current profile without copying its in-memory password. Up to four tabs/connection operations may be active. Disconnect a tab with its close button, **Disconnect tab**, or Ctrl+W; reconnect through saved profiles. SQLite opens an existing file path, in read-only mode by default; it does not silently create a mistyped path.

An Ident authentication error comes from PostgreSQL's `pg_hba.conf`, not Puff. Configure a suitable password rule for the intended user/host rather than weakening all authentication. Puff does not edit server configuration automatically.

## Work with data and SQL

Expand a schema on demand and activate a table/view to browse it. Column metadata loads when a table is opened. PostgreSQL constraints and indexes are available through **Inspect constraints / indexes**. The explorer caps at 500 schemas, 1,000 tables/views per schema and 500 columns; use SQL for larger catalogs. Metadata requests replace any unfinished query stream in the same tab.

The editor has SQL highlighting and completion for SQL keywords plus loaded table/column names. Completion does not query the database on keystrokes. Ctrl+Enter executes selected SQL, or the whole editor if there is no selection; Ctrl+. cancels. Execute one statement at a time. Elapsed time covers obtaining the current bounded page, not a promise that a server-side result has fully finished. Some statements can yield rows before ultimately failing; consume all pages to observe completion.

Results use native virtualized rows. Each page retains at most 200 rows and 4 MiB of text, with 256 KiB per cell; 100 of at most 500 returned columns are visible. **Next page replaces the previous page.** Sorting is lexical and page-local; filtering is ASCII case-insensitive and page-local. **Copy rows** copies selected rows as TSV (Ctrl/Shift selection). CSV and JSON exports save the current page only through native file dialogs. JSON uses `columns` plus array records, preserving duplicate column names; non-null database values are strings to preserve exact numeric/text representations, and SQL NULL is JSON null.

Double-click a cell in a keyed table to edit. Text needs no JSON quotes; numbers, JSON, arrays and bytea use the database's text input format. Set SQL NULL with the separate checkbox. Changes are staged and highlighted; **Save edits** reviews and commits them atomically. Full primary keys and original values target the row and detect concurrent changes. **Discard** clears staged edits. Tables without primary keys support browsing and inserts, not updates/deletes. **Add row** lets you supply individual columns or omit them for defaults; generated/identity columns are omitted. Inserts and confirmed deletion commit immediately outside an explicit transaction.

The database-tools menu provides database creation (PostgreSQL/CREATEDB required), table creation with composite primary keys, add/rename/drop column and PostgreSQL type changes. Generated SQL is shown before schema changes. Type conversion may fail on existing data; DDL can lock tables. **BEGIN / Commit / Rollback** operate on the selected tab's connection; the status indicates an open or failed transaction. Closing a tab rolls back uncommitted work, never committed work. Cancelling an unfinished PostgreSQL stream can abort its open transaction; consume results before Commit, or use Rollback after cancellation.

Query history is opt-in per tab because SQL can contain secrets. Search and clear local history, or save reusable snippets through the tools menu. History is capped at 200 statements and snippets at 100. **EXPLAIN** uses a non-ANALYZE PostgreSQL plan or SQLite query plan. Running sessions and lock information is requested explicitly; there are no background monitoring timers.

## Security and storage

Profiles, snippets and optional history live in `$XDG_CONFIG_HOME/puff/workspace.sqlite3`, falling back to `~/.config/puff/`. The directory is 0700 and file 0600. The old `connections.json` is imported once without passwords and left unchanged. Deleted imported profiles are not resurrected on later startup.

Passwords remain in memory unless you explicitly select **Store password in desktop Secret Service**. libsecret talks to GNOME Keyring or a compatible service; a locked/unavailable service reports an error rather than falling back to plaintext. Unchecking remember and saving removes a previously remembered credential; **Remove saved profile and credential** deletes both. Keyring storage requires a working desktop secret service; no keyring daemon is bundled. Opened sessions clear their password storage on disconnect where practical; ordinary C++ string copies cannot provide a guarantee that every historical heap byte is securely erased.

PostgreSQL values use bound parameters; identifiers are quoted and DDL types allowlisted. Read-only settings are an accidental-write safeguard, not a replacement for database permissions. Risk detection is conservative UX, not a complete SQL parser or authorization boundary. Production connections require explicit target confirmation. There is no automatic destructive-query retry, AI integration, outbound analytics or automatic database/service modification. Errors redact the connection password and recognizable password/URI fields. There is no persistent diagnostic log containing queries or credentials.

## Backup and restore

PostgreSQL backup uses `pg_dump --format=custom --compress=6`. A successful process, non-empty output and `pg_restore --list` archive inspection are required before the temporary file atomically replaces the destination. This verifies archive structure, not the correctness of a complete restore; the integration test separately performs a real round trip.

Restoration validates the archive, displays the destination, requires its exact database name and runs `pg_restore --single-transaction --exit-on-error --no-owner`. It does not silently drop/replace existing objects; conflicts fail and roll back. Prefer creating an empty test database and restoring there first. Utilities run without shell strings, elevation or password arguments. Credentials use a private 0600 temporary pgpass file; only its path enters the subprocess environment. Cancel stops the utility and temporary files are removed. The utility workflow is limited to ten minutes.

## Tests and measurements

```bash
make check                 # clang-format --dry-run --Werror (install clang-tools-extra)
make test                  # compiled unit + SQLite checks
sudo dnf install postgresql-server
make integration           # disposable PostgreSQL cluster, socket + local TCP, no existing DB
make ui-test               # native GTK widget lifecycle, requires a display and Python fixture setup
python3 scripts/measure-native.py
```

CI runs on Fedora 44, including a virtual X display and an unprivileged PostgreSQL cluster. Unit/integration tests cover quoting, risk review, private profile migration, redaction, history bounds, exact exports, queue limits, driver cleanup, composite-key CRUD, concurrent edits, atomic rollback, explicit transactions, million-row paging, cell budgets, cancellation, timeout recovery, authentication/TLS failure, connection refusal, schema operations, database creation and backup/restore. A test without the required database environment prints an explicit PostgreSQL skip. GUI lifecycle tests use a disposable SQLite fixture and exercise actual GTK widgets rather than fake query results.

See [resource measurements and conditions](docs/PERFORMANCE.md), [architecture and audit](docs/IMPLEMENTATION.md), and raw JSON results in `docs/`. RSS includes shared toolkit/renderer pages and can remain elevated after closing views because GTK/font/allocator caches retain memory; freeing database handles and result models does not guarantee RSS returns to the initial startup value.

## Remaining work

MariaDB/MySQL drivers, integrated SSH, a database switcher/list, custom CA-file UI, SQL formatting, function-aware completion, parameter-entry UI, column/cell clipboard selection, full-result streaming CSV/JSON/SQL exports, SQLite file creation/backup UI, index/foreign-key creation forms, triggers/routines explorer, ER diagrams, schema comparison/migrations, backup schedules/history/encryption/retention, Podman discovery, systemd/journal UI, Flatpak and optional AI are not implemented. Raw SQL covers many administration operations in the meantime.

SQLite BLOB cells require `hex(column)` for viewing; its explorer currently covers the main database only. Manual identifiers are limited to 63 UTF-8 bytes across both drivers. SQLite query plans, particularly RETURNING statements, may allocate engine memory beyond the bounded UI page. Wide result sets need further rendering benchmarks. Custom PostgreSQL types and unusual encodings need additional round-trip coverage. Browser-style search/filter across an entire table, reverse paging of arbitrary SQL streams and automatic result retention are deliberately absent. Keyring unlock workflows, TLS certificate-validation success, KDE/KWallet, direct Wayland rendering (these tests use X11 within a Wayland session), remote DNS/network partition behavior and multiple physical Fedora/GPU configurations still need release qualification. The RPM is unsigned and has been inspected/extracted/launched, not installed into this user's system.

## Current development and packaging

See [the prioritized engineering roadmap](docs/ROADMAP.md) and
[release/build instructions](docs/RELEASES.md) for the connection-workflow changes,
Debian/Fedora packages and macOS preview workflow. Outstanding capabilities and
platform verification are explicitly listed there.
