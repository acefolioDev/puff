# Repository audit and native implementation

## Baseline findings

Inspected 2026-10-18 on Fedora 44 x86_64. The original TypeScript application used Bun 1.4.3, postgres.js 3.4.9, Preact 10.29.8, webview-bun 2.4.0 and Tailwind 4.3.3. Entry points were `src/desktop/main.ts`, `src/backend/server.ts`, and `src/ui/main.tsx`. Only PostgreSQL was implemented. Private XDG JSON profiles excluded passwords.

Baseline verification: TypeScript check and compilation passed; 4 SQL boundary unit tests and 2 PostgreSQL integration scenarios passed. The single executable was approximately 87 MiB. Valuable behavior included profiles, composite-key-safe row CRUD, atomic saves and original-row conflict checks, quoted identifiers, allowlisted DDL types, database/table/column creation, PostgreSQL error details, desktop icon and RPM.

Risks: WebKit and a local HTTP server conflicted with the native-widget requirement. Queries buffered all results; metadata loaded eagerly; cancellation, disconnect, native dialogs, history and keyring storage were absent. JavaScript numeric conversions could lose precision. A global operation gate prevented independent queries.

The complete source is preserved in `archive/puff-0.1.0-webview-source.tar.gz`. No old application code or dependency is loaded by the current build.

## Technology decision

C++20 with system GTK4, libadwaita, GtkSourceView, libpq, SQLite and libsecret is the primary stack. GTK draws actual native widgets, including virtualized ColumnView rows, text editing, menus, file dialogs, theme support and accessibility. All application logic is compiled. There is no Python, Bun, JavaScript engine, HTTP server, Chromium or WebKit dependency in the application.

Rust was evaluated as an equally viable native systems language. C++ uses the installed toolkit's C ABI directly, with standard-library ownership for data/threads and explicit GObject lifetime handling. This avoids an additional GTK binding/toolchain dependency graph; performance is established by measurements and tests, not a claim that C++ is inherently faster than Rust. Qt6 was considered, but there was no Qt code to preserve and GTK fits Fedora Workstation conventions directly. A small Python GTK prototype helped verify behavior before the compiled implementation; its source is archived and is not maintained in parallel.

Development packages initially required a sudo password. For this environment, Fedora compiler/development RPMs were downloaded and unpacked into a private build directory, without modifying system packages or SELinux. The documented normal development flow uses Fedora development packages.

## Boundaries

- `core`: profiles, result values, SQL generation, conservative risk review, redaction, driver interface.
- `postgres` / `sqlite`: engine-specific connection, streaming, metadata, edit transactions and conflict checks.
- `session`: one waiting worker per tab, one accepted operation at a time, explicit cancellation, no polling at idle.
- `store`: short-lived SQLite metadata handles, profile migration, bounded opt-in history and snippets; no password columns.
- `files`: atomic page exports and validated subprocess backup/restore using a private pgpass file.
- `application`: native window, connection forms, multiple workspaces, desktop application lifecycle.
- `workspace`: editor, lazy schema tree, native result model, review dialogs and manual data/schema operations.

At most four tabs/connection operations are active. Each connected tab has one database connection and one worker. A slow connection cannot freeze other tabs or GTK. Workers stop and connections close when tabs disconnect; open transactions roll back on close. No destructive query is retried.

libpq single-row mode and incremental SQLite stepping bound each page to 200 rows, 4 MiB of text, 256 KiB per cell, and 500 returned columns (100 visible grid columns). One oversized PostgreSQL protocol row can still require driver memory before the application rejects it. Sorting/filtering is page-local. Query streams are released or cancelled before reuse. Schema requests are on-demand and capped. Metadata operations replace an unfinished stream in that tab; they do not maintain hidden query connections.

## Order delivered

1. Audited and tested baseline; preserved snapshots.
2. Ported safe SQL, exact PostgreSQL text values, atomic editing and optimistic conflict checks.
3. Added compiled native toolkit UI, independent tabs and drivers, incremental results and cancellation.
4. Added read-only defaults, production target confirmations, keyring, private metadata and restore review.
5. Added native backup/restore, page export, history/snippets, on-demand session and lock views.
6. Tested real PostgreSQL/SQLite behavior and native widget lifecycles; measured resources and optimized batched result-model updates.
7. Added source/binary Fedora RPM specification, desktop metadata, CI, build/test/measurement scripts and explicit limitations.

MySQL/MariaDB, integrated SSH, full schema migration comparison/diagrams, Podman discovery and optional AI remain roadmap work. They have no fake UI screens or claimed implementations.
