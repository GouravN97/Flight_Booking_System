# my_dbms

A small educational DBMS written in C, inspired by SQLite internals. It stores rows on disk, uses a pager to cache fixed-size pages, and organizes table data with a B-tree.

This project is intentionally minimal: it is useful for learning how a database shell, parser, pager, row serializer, cursor, and B-tree fit together. The parent [Flight Booking System](../README.md) uses this engine through `storage/db_handler.c` for all server persistence.

## Features

- Interactive REPL with a `db >` prompt
- Persistent database file storage
- Fixed-size page-based pager
- Row serialization/deserialization for the demo shell
- One fixed-size value type per table
- `insert` and `select` statements
- Generic `void*` C API for open, close, create, get, update, and delete
- Duplicate primary-key detection
- Leaf-node splitting
- Internal B-tree nodes
- Debug meta commands for constants and B-tree structure

## Requirements

- A C compiler such as `gcc` or `clang`
- `make`
- A Unix-like environment, because the pager uses POSIX file APIs such as `open`, `read`, `write`, `lseek`, and `close`

## Build

From the project directory:

```bash
make
```

This creates the executable:

```bash
./my_dbms
```

To remove the executable:

```bash
make clean
```

## Run

The program requires a database filename:

```bash
./my_dbms test.db
```

If `test.db` does not exist, it will be created. If it already exists, the DBMS will reopen it and read data from it.

You can also use:

```bash
make run
```

That runs:

```bash
./my_dbms test.db
```

## Example Session

```text
$ ./my_dbms test.db
db > insert 1 alice alice@example.com
Executed.
db > insert 2 bob bob@example.com
Executed.
db > select
(1, alice, alice@example.com)
(2, bob, bob@example.com)
Executed.
db > .exit
```

Reopen the same file to confirm persistence:

```text
$ ./my_dbms test.db
db > select
(1, alice, alice@example.com)
(2, bob, bob@example.com)
Executed.
db > .exit
```

## Supported SQL

This DBMS supports only a tiny subset of SQL-like commands.

### Insert

```sql
insert <id> <username> <email>
```

Example:

```sql
insert 1 alice alice@example.com
```

Rules:

- `id` must be non-negative.
- `id` acts as the primary key.
- Duplicate `id` values are rejected.
- `username` must be at most 32 characters.
- `email` must be at most 255 characters.
- Values are separated by spaces, so quoted strings with spaces are not supported.

### Select

```sql
select
```

Prints every stored row in key order.

## C API

Include `src/api/db.h` to use the DBMS from C without going through the REPL parser.
Each table stores one fixed-size value type. Open the table with the size of
that struct, then pass pointers to values of that type through the CRUD API.
The B-tree key is a separate `uint32_t`, so stored structs do not need an `id`
field.

```c
#include <stdint.h>
#include <string.h>

#include "db.h"

typedef struct {
  char name[32];
  uint32_t age;
} Person;

Db* people = db_open_with_value_size("people.db", sizeof(Person));

Person alice = {.age = 30};
strcpy(alice.name, "alice");

create(people, 1, &alice);

Person found;
get(people, 1, &found);

found.age = 31;
update(people, 1, &found);

delete(people, 1);
db_close(people);
```

CRUD methods return `DbResult`, so callers can check for `DB_OK`, `DB_NOT_FOUND`, `DB_DUPLICATE_KEY`, `DB_INVALID_ARGUMENT`, or `DB_UNSUPPORTED_OPERATION`.
Use a separate table/database file for each struct shape, and reopen an existing
file with the same value size it was created with.

## Integration with the Flight Booking System

The parent project in `../` embeds this engine as its persistence layer. The
server does not use the interactive REPL; instead, `storage/db_handler.c` wraps
the C API with create/open/close/get/update/delete helpers and a `foreach_record`
callback for full-table scans on startup.

Four my_dbms tables map to four files in the server working directory:

| File          | Value type        | Key scheme                                      |
| ------------- | ----------------- | ----------------------------------------------- |
| `flights.db`  | `FlightPersist`   | Flight slot index (`0` … `MAX_FLIGHTS - 1`)     |
| `users.db`    | `UserEntry`       | User slot index                                 |
| `admins.db`   | `AdminEntry`      | Admin slot index                                |
| `bookings.db` | `BookingRecord`   | Per-flight composite key (see below)            |

Bookings are stored **per flight**, not in one global array. In memory, each
`Flight` owns `BookingRecord bookings[MAX_BOOKINGS_PER_FLIGHT]`. On disk,
`bookings.db` uses a composite key:

```text
key = flight_index * MAX_BOOKINGS_PER_FLIGHT + booking_index
```

With `MAX_FLIGHTS = 1000` and `MAX_BOOKINGS_PER_FLIGHT = 1000`, the table holds
up to one million booking slots partitioned by flight. After a book or cancel,
`storage_flush_flight_bookings()` writes only the affected flight's slots.

Booking IDs exposed to users are flight-scoped, for example `F101-BK0001`
(`<flight_number>-BK<seq>`). On startup, older flat `bookings.db` files (from
when bookings used a single global key space) are detected and migrated into
this per-flight layout automatically.

The server also serializes concurrent access with POSIX file locks on each
`.db` file and a process-wide DB API mutex around my_dbms calls in
`storage/storage_service.c`.

To reset only booking data during development:

```bash
make -C .. reset-bookings-db
```

That removes `bookings.db` from the project root; restart the server to
recreate an empty per-flight bookings table.

## Meta Commands

Meta commands start with `.` and are handled by the shell instead of the SQL parser.

### Exit

```text
.exit
```

Flushes dirty pages, closes the database file, frees memory, and exits.

### Constants

```text
.constants
```

Prints storage-layout constants such as row size, table value size, leaf-node header size, and maximum number of cells per leaf node.

### B-tree Debug View

```text
.btree
```

Prints the current B-tree structure.

Example shape:

```text
Tree:
- leaf (size 2)
  - 1
  - 2
```

## Data Model

The storage engine stores opaque fixed-size values. A table is opened with one
`value_size`, and every value in that table is copied as exactly that many
bytes. Leaf nodes store key/value cells where the key is a `uint32_t` and the
value is the table's opaque struct payload.

The interactive SQL shell still demonstrates the original row shape:

| Column | Type | Max Size |
| --- | --- | --- |
| `id` | `uint32_t` | 4 bytes |
| `username` | fixed char array | 32 characters + null terminator |
| `email` | fixed char array | 255 characters + null terminator |

The demo shell serializes rows into raw page memory using fixed offsets. The C
API stores arbitrary structs directly as opaque bytes.

## File Format Overview

The database file is split into fixed-size pages:

- Page size: `4096` bytes
- Maximum cached pages: `400`
- Root page: page `0`

The pager loads pages lazily from disk, keeps them in memory, and writes them back during `.exit`.
Pages removed by B-tree deletion are linked into a free-page list and reused by later node splits before the file grows.

## B-tree Overview

The table is stored as a B-tree:

- Leaf nodes contain key/value cells.
- Keys are row `id` values.
- Values are fixed-size opaque struct payloads.
- Internal nodes route searches to child pages.
- Leaf nodes split when full.
- Splitting the root creates a new internal root.

The implementation keeps internal-node capacity deliberately small for easier testing and visualization.

## Project Structure

The code is grouped by layer. The REPL sits at the top, SQL parsing and
execution translate commands into storage operations, and the storage/B-tree
layers manage pages, cursors, keys, and values.

### Top-level files

| File | Description |
| --- | --- |
| `README.md` | Project overview, build/run instructions, API examples, storage notes, and this file map. |
| `Makefile` | Builds the `my_dbms` executable from all source files. Also provides `make run` and `make clean`. |
| `my_dbms` | Generated executable created by `make`; it is not source code. |
| `test.db` | Local database file used by the default `make run` target; it is runtime data, not source code. |

### `src/app/`

Application entry point and REPL loop.

| File | Description |
| --- | --- |
| `src/app/main.c` | Parses the database filename argument, opens the table, creates the input buffer, runs the prompt loop, dispatches meta commands, prepares SQL statements, executes them, and prints user-facing success/error messages. |

### `src/api/`

Public C API for using the database engine without the interactive shell.

| File | Description |
| --- | --- |
| `src/api/db.h` | Defines `Db`, `DbResult`, and the generic CRUD function signatures. Values are passed as `void*`; callers provide a separate `uint32_t` key. |
| `src/api/db.c` | Implements `create`, `get`, `update`, and `delete`. It performs duplicate-key checks, copies opaque value bytes into/out of leaf cells, removes cells, and rebalances leaf/internal nodes after deletion. |

### `src/shell/`

Interactive shell helpers.

| File | Description |
| --- | --- |
| `src/shell/input.h` | Declares `InputBuffer` and input helper functions used by the REPL. |
| `src/shell/input.c` | Allocates/frees input buffers, prints the `db >` prompt, and reads a full line from standard input. |
| `src/shell/meta_commands.h` | Declares meta-command result values and `do_meta_command`. |
| `src/shell/meta_commands.c` | Handles commands beginning with `.`, including `.exit`, `.constants`, and `.btree`. It also prints table-specific storage constants and a recursive B-tree debug view. |

### `src/sql/`

Tiny SQL-like parser and executor for the demo shell.

| File | Description |
| --- | --- |
| `src/sql/parser.h` | Declares statement types, prepare result codes, the `Statement` struct, and parser functions. |
| `src/sql/parser.c` | Recognizes `insert` and `select`. It tokenizes insert statements, validates id/string length constraints, and fills a demo `Row`. |
| `src/sql/execution.h` | Declares execution result codes and statement execution functions. |
| `src/sql/execution.c` | Executes prepared statements. Inserts call the generic API with `row.id` as the key and the row struct as the value; selects iterate with a cursor and print rows. |

### `src/storage/`

Storage primitives shared by the API, shell, and B-tree layers.

| File | Description |
| --- | --- |
| `src/storage/row.h` | Defines the demo shell `Row` layout: `id`, `username`, and `email`. Also defines fixed offsets and `ROW_SIZE` for row serialization. |
| `src/storage/row.c` | Serializes/deserializes the demo `Row` to/from raw memory and prints rows for `select`. The generic API stores arbitrary structs directly as opaque bytes. |
| `src/storage/pager.h` | Defines page constants, `Pager`, and pager function declarations. Pages are fixed at `4096` bytes and cached in memory. |
| `src/storage/pager.c` | Opens database files, lazily loads pages, flushes pages to disk, tracks the number of pages, and manages a simple free-page list for page reuse. |
| `src/storage/table.h` | Defines `Table`, which owns a `Pager`, root page number, and `value_size`. Declares `db_open`, `db_open_with_value_size`, and `db_close`. |
| `src/storage/table.c` | Opens/closes tables. New files initialize page `0` as the root leaf node; existing files restore free-page-list metadata. `db_open` keeps the shell on `ROW_SIZE`, while `db_open_with_value_size` opens generic fixed-size tables. |
| `src/storage/cursor.h` | Defines `Cursor`, which tracks a table, page number, cell number, and end-of-table state. |
| `src/storage/cursor.c` | Creates a cursor at the first key, returns the current cell value payload, and advances across linked leaf nodes. |

### `src/btree/`

B-tree page layout and algorithms. Leaf cells store `uint32_t` keys plus
table-sized opaque value payloads. Internal nodes store child page numbers and
separator keys.

| File | Description |
| --- | --- |
| `src/btree/btree_node.h` | Defines common node header layout, node types, and shared node helper declarations. |
| `src/btree/btree_node.c` | Reads/writes common node metadata such as type, root flag, parent pointer, and computes the maximum key under a node. |
| `src/btree/btree_leaf.h` | Defines leaf-node header constants and declares helpers for dynamic cell sizing, key/value access, searching, inserting, and splitting leaf nodes. |
| `src/btree/btree_leaf.c` | Implements leaf-node layout helpers. It computes cell size/max cells from `table->value_size`, binary-searches leaf keys, inserts opaque value bytes, and splits full leaves. |
| `src/btree/btree_internal.h` | Defines internal-node layout constants and declares helpers for child/key access, routing, insertion, and splitting internal nodes. |
| `src/btree/btree_internal.c` | Implements internal-node routing and maintenance. It finds child branches, updates separator keys, inserts child pages, and splits internal nodes when full. |
| `src/btree/btree_algos.h` | Declares high-level B-tree operations shared across storage/API code. |
| `src/btree/btree_algos.c` | Finds the correct leaf for a key starting from the root and creates a new root when splitting the old root. |

### Module flow

Typical interactive command flow:

1. `src/app/main.c` reads a line through `src/shell/input.c`.
2. Meta commands are handled by `src/shell/meta_commands.c`; SQL-like commands go to `src/sql/parser.c`.
3. `src/sql/execution.c` executes inserts/selects using `src/api/db.c` and cursor helpers.
4. `src/api/db.c` uses B-tree search/insert/delete helpers to modify pages.
5. `src/storage/pager.c` loads and flushes fixed-size pages from the database file.

Typical direct C API flow:

1. Caller opens one table with `db_open_with_value_size(filename, sizeof(MyStruct))`.
2. Caller passes `uint32_t` keys plus `void*` pointers to `create`, `get`, `update`, and `delete`.
3. The API copies exactly `table->value_size` bytes into or out of B-tree leaf cells.
4. The pager persists dirty in-memory pages when `db_close` is called.

## Development Workflow

Build after changes:

```bash
make
```

Run a quick manual smoke test:

```bash
./my_dbms /tmp/my_dbms_test.db
```

Then enter:

```sql
insert 1 alice alice@example.com
select
.btree
.constants
.exit
```

For a non-interactive smoke test:

```bash
printf 'insert 1 alice alice@example.com\nselect\n.exit\n' | ./my_dbms /tmp/my_dbms_test.db
```

Expected output includes:

```text
(1, alice, alice@example.com)
```

## Common Errors

### `Must supply a database filename.`

Run the program with a file path:

```bash
./my_dbms test.db
```

### `Syntax error. Could not parse statement.`

The `insert` statement needs exactly:

```sql
insert <id> <username> <email>
```

### `ID must be positive.`

Use a non-negative integer id:

```sql
insert 1 alice alice@example.com
```

### `String is too long.`

Shorten `username` or `email`:

- `username`: max 32 characters
- `email`: max 255 characters

### `Error: Duplicate key.`

The row id already exists. Use a different id.

## Current Limitations

- A database file represents one table with one fixed-size value type.
- The SQL shell supports only `insert` and `select`.
- The SQL shell is still tied to the demo `Row` shape.
- The C API supports generic value-level `create`, `get`, `update`, and `delete`.
- The table value size is provided by the caller when opening the file and must match on reopen.
- No `where`, joins, indexes, transactions, or SQL expressions.
- Input parsing is whitespace-based and does not support quoted strings.
- The database format is experimental and not compatible with SQLite.
- Pages are flushed only when `.exit` is used.
- Error handling is basic and intended for learning, not production use.

## Notes

This is a learning project, not a production database. The code is meant to make database internals visible and hackable: the pager, row layout, B-tree node formats, and cursor traversal are all implemented directly in C.
