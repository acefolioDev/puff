# Measured performance

Measured on 2026-10-18: Fedora Workstation 44 x86_64, Intel Core i5-6300U (2 cores / 4 threads), approximately 12 GiB RAM. The desktop session is Wayland; the tool environment selects GTK's X11 backend. The default GTK renderer is retained. Other desktop applications were running. This is an interactive laptop sample, not a dedicated benchmark machine.

## Method

`scripts/measure-native.py` starts a fresh native process for each scenario. It times process creation to GTK window mapping and records `/proc/PID/status`, proportional set size from `smaps_rollup`, and process CPU ticks. It waits three seconds after mapping, then takes three three-second idle CPU samples. It creates a disposable SQLite database with 100,000 rows (two columns, roughly 80 bytes of text per row), opens one or four real connections/tabs, and exercises table browsing, raw SQL paging, cell editing/discard, scrolling, result release, and disconnection.

Filesystem/library caches are warm. **A true cold-disk startup is unmeasured**; no privileged cache flush was performed. GTK model-update timing measures synchronous row/model synchronization, not the time of final compositor presentation. Backend tests separately stream a million-row PostgreSQL and SQLite result without retaining all rows. These measurements do not establish performance for wide tables, large cells, remote networks or every GPU.

## Current default-renderer sample

| Measurement | Result |
| --- | ---: |
| Fresh process to mapped window | 315 ms |
| Application initialization to mapped window | 261 ms |
| Settled idle RSS | 112.4 MiB |
| Settled idle proportional memory (PSS) | 69.1 MiB |
| Idle CPU samples, percent of one core | 0.00, 0.00, 0.00 |
| Median sampled idle CPU | 0.00% |
| First 200-row / 2-column model update | 100.1–115.5 ms |
| Subsequent page/model updates | 28.7–75.5 ms |

| Connections/tabs | Loaded page RSS (MiB) | After result release (MiB) | After all tabs disconnected (MiB) |
| ---: | ---: | ---: | ---: |
| 1 | 125.8 | 128.1 | 144.4 |
| 4 | 128.7 | 130.7 | 131.1 |

Result release is verified to leave zero result-model rows, and disconnection closes workers and database handles. Repeated metadata operations are checked for file-descriptor retention. **RSS does not return to startup levels**: GTK, fonts, rendering, the source-language manager and the allocator retain reusable caches. This is neither proof of a leak nor proof that all possible leak paths have been eliminated. No ASan/Valgrind run or multi-hour soak was performed.

## Optimization and budgets

The initial port rebuilt columns and row widgets on each page, taking roughly 100–160 ms for a small page on this machine. The current code reuses columns and cached row objects, updates the model in a batch, avoids redundant text/style changes, and bounds result storage. A stronger native test caught stale cached labels after paging/scrolling; synchronization now updates the bounded cached items deterministically and verifies both the first and final displayed rows. The result is faster page updates with correct displayed values, rather than relying on timing-sensitive viewport-only updates.

Provisional regression budgets for **this two-column fixture on this machine**: mapped startup under 1,000 ms; idle RSS under 128 MiB; one loaded tab under 160 MiB; four tabs under 192 MiB; median settled idle CPU under 0.5% of one core; initial model update under 200 ms; subsequent model update under 100 ms. The current default sample meets these budgets. They are engineering targets, not enforced global memory ceilings or universal hardware guarantees. An earlier short idle sample recorded 2.33% CPU during desktop/startup activity; the longer settling period and individual samples are retained in the reporting method rather than treating one zero sample as a guarantee.

The native process has no application idle polling loop or automatically connected databases. GTK/GIO/renderer helper threads are included in the measurements. The worker queue accepts only one operation per tab, at most four connection operations/tabs; each page is at most 200 rows / 4 MiB of text / 256 KiB per cell, with at most 500 returned and 100 visible columns. SQL engine execution and one incoming PostgreSQL protocol row may use memory beyond these application page limits.

## Renderer comparison

An additional trial with `GSK_RENDERER=cairo` measured 94.0 MiB idle RSS and 55.3 MiB PSS, versus 112.4 / 69.1 MiB for the default renderer. This is a renderer-specific laptop result, not a claim that Cairo is always faster. Puff keeps GTK's renderer choice by default. A user prioritizing lower renderer memory can explicitly try:

```sh
GSK_RENDERER=cairo puff
```

Compare scrolling and CPU on the actual target GPU before making that preference permanent. Cairo is still a native GTK renderer; no web engine is introduced.

## Original WebKit baseline

The original 0.1.0 Bun/WebKit process tree sampled 412.5 MiB summed RSS across 8 processes, with 1.67% sampled CPU over three seconds. The executable occupied about 87 MiB. Summed RSS double-counts shared pages; its method differs from the single-process native measurement. This is useful context, not a controlled percentage-saving claim. Cold-start, baseline PSS and baseline large-result rendering were not measured.

## Reproduce and qualification limits

```sh
make
python3 scripts/measure-native.py
GSK_RENDERER=cairo python3 scripts/measure-native.py
```

The script creates private temporary fixtures and never connects to existing user databases. Native self-test timing deliberately uses short test-only timers; normal application operation does not run that test loop. Raw results: [default](native-performance.json), [Cairo trial](native-performance-cairo.json), [original baseline](baseline-performance.json).

The compiled backend suite currently passes 72 PostgreSQL/SQLite/core checks, plus a separate unavailable-Secret-Service check. GTK tests run with fatal criticals and exercise real widgets, displayed labels after paging/scrolling, staging/discard, and result cleanup. Remote positive TLS validation, a locked/unlocked desktop keyring, direct Wayland, KDE, wide result sets, long-lived connection churn and additional physical machines still need release qualification.
