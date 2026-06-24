# CoreDB

A minimal database engine built from scratch in C++ — no external database libraries, just the C++ Standard Library. Built to understand how real databases (Postgres, MySQL, SQLite) work underneath their SQL layer, by implementing the same core mechanisms: disk-backed storage, an in-memory LRU buffer pool, and a B-Tree index, wired into a working REPL.

## What it does

```
db > INSERT 1 piyush piyush@kgp.edu
Inserted.
db > INSERT 2 someone someone@test.com
Inserted.
db > SELECT
(1, piyush, piyush@kgp.edu)
(2, someone, someone@test.com)
db > FIND 1
(1, piyush, piyush@kgp.edu)
db > .exit
```

Data persists across restarts — close the program and reopen it, and everything is still there, read back off disk through the buffer pool and B-Tree.

## Architecture

```
REPL (main.cpp)
    │
    ▼
Table  ── translates row keys into B-Tree operations
    │
    ▼
B-Tree ── leaf nodes (store rows) + internal nodes (route to leaves)
    │
    ▼
Pager  ── LRU buffer pool, caches up to 10 pages in memory
    │
    ▼
Disk (coredb.db, raw 4KB pages)
```

Each layer only knows about the layer directly below it: the REPL doesn't know about pages, the B-Tree doesn't know about disk I/O, and the Pager doesn't know what a "row" is. This separation of concerns mirrors how real database engines are structured.

## Commands

| Command | Description |
|---|---|
| `INSERT <id> <username> <email>` | Insert a row. `id` must be a positive integer; `username` ≤ 32 chars; `email` ≤ 255 chars. |
| `SELECT` | Print every row, in sorted order by id (full B-Tree traversal). |
| `FIND <id>` | Look up a single row by id — uses real B-Tree point-lookup (O(log n)), not a linear scan. |
| `.exit` | Save metadata and close the database cleanly. |

## Building and running

Requires `g++` (C++17) and `CMake` ≥ 3.14.

```bash
mkdir build
cd build
cmake ..
make
./CoreDB
```

### Running tests

```bash
cd build
make
./CoreDBTests
```

GTest is fetched and built automatically from source via CMake's `FetchContent` — no separate GTest installation required (see "Bugs Found and Fixed" below for why this matters).

## Scope — what this is, and what it deliberately is not

This project intentionally implements a **subset** of what a real database does, to allow going deep on storage internals rather than shallow across an entire DBMS feature set.

**Implemented:**
- A REPL with command parsing and input validation
- Disk-backed, fixed-size-page storage with raw binary I/O
- An LRU-evicting buffer pool (hash map + doubly linked list, O(1) operations)
- A B-Tree index: leaf node insert/search, leaf splitting on overflow, and a single level of internal-node routing (root with up to one separator key, pointing to two leaf children)
- Automated tests for the Pager, leaf nodes, internal node routing, and end-to-end Table operations

**Explicitly not implemented (deliberate scope cuts, not oversights):**
- **Full SQL parsing** — only a fixed set of recognized commands (`INSERT`, `SELECT`, `FIND`)
- **Multi-level B-Tree growth (root-splitting)** — if the root's internal node itself overflows, insertion fails with an explicit error rather than recursively splitting upward. Real B-Trees handle this by growing in height; implementing it was judged too time-costly relative to the value of demonstrating the *core* splitting mechanism, which is already proven to work at the leaf level.
- **B-Tree deletion** — only insert and search are supported, since deletion requires node merging/rebalancing, a separate and substantial piece of logic.
- **Duplicate key handling** — inserting an existing key does not currently raise an error or perform an update; it inserts an additional cell with the same key. A real implementation would reject or upsert.
- **Concurrency / multi-user transactions** — single-threaded, single-user only.
- **Crash recovery / write-ahead logging** — data is only guaranteed durable on clean shutdown (`.exit`), not on abrupt crash or power loss mid-write.

## Design Decisions Log

A running record of nontrivial choices made during the build, and why.

| Decision | Reasoning |
|---|---|
| 4KB page size | Matches the standard page size most operating systems and file systems use internally for disk I/O — aligns our reads/writes with how the underlying hardware naturally operates. |
| Fixed-size `Row` struct (`char[]` instead of `std::string`) | Guarantees every row takes up exactly the same number of bytes, which is what allows direct byte-offset calculation (`row_num * ROW_SIZE`) instead of a scan to locate any row. |
| Binary file mode (`ios::binary`) everywhere | Without it, some platforms can silently alter raw bytes during read/write (e.g. line-ending conversion), which would corrupt raw struct data. |
| Separate `.meta` file for row count | File size alone cannot reveal how many rows are *real* data versus zeroed padding inside a fixed-size page — this was discovered as a real bug (see below) and fixed by explicitly persisting row count as metadata. |
| LRU (not FIFO) for buffer pool eviction | Recency of use is a better predictor of near-future access than insertion order — a page inserted long ago might still be accessed constantly, while a recently inserted page might never be touched again. |
| Hash map + doubly linked list for LRU | Gives O(1) for both "find this page's position" (hash map) and "move this page to most-recently-used" (linked list splice) — neither structure alone achieves both. |
| Fixed-size internal node child/key slots (sized for max capacity, not current count) | Keeps slot addresses stable regardless of how many keys currently exist, avoiding the need to recalculate offsets on every insert — at the cost of some wasted space within a node. |
| B-Tree keyed on row `id` | Simplest unique identifier already present in the schema; avoids introducing a separate primary key concept for this scope. |
| Root-splitting explicitly out of scope | A recursive, genuinely complex case; the core splitting mechanism is already demonstrated and tested at the leaf level, which captures the essential DBMS concept being learned. |

## Bugs Found and Fixed During Development

These are documented because finding, diagnosing, and fixing them was as instructive as writing the original code — and they're good material for explaining real engineering process.

**1. Row count miscalculated from file size after restart.**
Initially, the number of existing rows was calculated as `file_length / ROW_SIZE` on startup. Because pages are fixed-size (4KB) but real data rarely fills a page exactly, this caused phantom "empty" rows (all-zero data) to be read back after closing and reopening the database — the leftover zeroed padding inside the last saved page was being miscounted as real rows. **Fixed** by explicitly persisting the true row count in a separate `.meta` file, rather than inferring it from file size.

**2. `file_length` not updated after page eviction writes.**
The buffer pool's `file_length` tracking variable was only set once, at startup. After implementing LRU eviction, writes to new pages during eviction could grow the file on disk without `file_length` reflecting that — meaning a later `get_page()` call could wrongly conclude a page "doesn't exist on disk yet" and return a blank page, silently discarding previously-saved data. This didn't surface in early small-scale tests, since it only triggers once enough pages exist to force an eviction. **Fixed** by updating `file_length` inside `flush_page()` whenever a write extends past the previously known file length.

**3. Windows-native compiler environment was unworkable.**
Significant time was spent attempting to compile this project using MSYS2/MinGW directly on Windows. The compiler (`cc1plus.exe`) consistently failed silently with no error output and exit code 1, across multiple clean reinstalls, antivirus exclusions, and PATH configurations — root cause never conclusively identified. **Resolved** by switching the entire development environment to WSL (Windows Subsystem for Linux), which compiled and ran correctly on the very first attempt, confirming the issue was Windows-environment-specific rather than related to the code or toolchain itself.

**4. CMake's `find_package(GTest)` located an incompatible, Windows-built GTest.**
Because the project lives on a path also visible to an Anaconda installation (mounted into WSL via `/mnt/c/...`), CMake's `find_package(GTest)` located a pre-built GTest intended for MSVC on Windows, not Linux/g++. This caused the test executable to fail at the link stage with dozens of "undefined reference" errors, despite CMake reporting "Found GTest" successfully. **Fixed** by switching to CMake's `FetchContent` to download and build GTest from source as part of the project's own build, guaranteeing toolchain compatibility regardless of what else might be installed on the system.

## Testing

11 automated tests via GTest, covering:
- Pager: file creation, zeroed-page initialization, write/read persistence, buffer pool eviction limit enforcement
- Leaf nodes: sorted insertion, key search (found/not found), split correctness (sorted halves, no overlap, no data loss)
- Internal nodes: correct child-routing decisions based on separator keys
- Table (integration): insert+find round trip, and a 30-row insertion test that forces real leaf splits and verifies every inserted key remains findable afterward

Manual end-to-end verification was also performed: 30 sequential inserts through the actual REPL (triggering real splits), followed by program restart and re-verification via `SELECT` and `FIND`, confirming correct persistence and point-lookup behavior outside of the test suite as well.