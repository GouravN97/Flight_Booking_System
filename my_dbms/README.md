# my_dbms

A small educational storage engine written in C, inspired by SQLite internals. It stores fixed-size records on disk, uses a pager to cache fixed-size pages, and organizes each table as a B-tree.

This engine is intentionally minimal: it is useful for learning how a pager, cursor, and B-tree fit together. It is a library only (no interactive shell). The parent [Flight Booking System](../README.md) uses this engine through `storage/db_handler.c` for all server persistence.

## Features

- Persistent database file storage
- Fixed-size page-based pager
- One fixed-size value type per table
- Generic `void*` C API for open, close, create, get, update, and delete
- Duplicate primary-key detection
- Leaf-node splitting
- Internal B-tree nodes
- Free-page list for reusing pages after deletion

## Requirements

- A C compiler such as `gcc` or `clang`
- `make`
- A Unix-like environment, because the pager uses POSIX file APIs such as `open`, `read`, `write`, `lseek`, and `close`

## Build

The parent project compiles these sources directly into `server_app`, so you do not need to build anything here to run the Flight Booking System.

To build the engine on its own as a static library:

```bash
make          # produces libmy_dbms.a
make clean    # removes libmy_dbms.a and object files
```

## C API

Include `src/api/db.h` to use the engine from C.
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

The parent project in `../` embeds this engine as its persistence layer.
`storage/db_handler.c` wraps
the C API with create/open/close/get/update/delete helpers and a `foreach_record`
callback for full-table scans on startup.

Six my_dbms tables map to six files in the server working directory:

| File               | Value type         | Key scheme                                  |
| ------------------ | ------------------ | ------------------------------------------- |
| `flights.db`       | `FlightPersist`    | Flight slot index (`0` … `MAX_FLIGHTS - 1`) |
| `users.db`         | `UserEntry`        | User slot index                             |
| `admins.db`        | `AdminPersist`     | Admin slot index                            |
| `bookings.db`      | `BookingRecord`    | Per-flight composite key (see below)        |
| `waitlist.db`      | `WaitlistEntry`    | Waitlist slot index                         |
| `notifications.db` | `UserNotification` | Notification slot index                     |

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

## Data Model

The storage engine stores opaque fixed-size values. A table is opened with one
`value_size`, and every value in that table is copied as exactly that many
bytes. Leaf nodes store key/value cells where the key is a `uint32_t` and the
value is the table's opaque struct payload.

## File Format Overview

The database file is split into fixed-size pages:

- Page size: `4096` bytes
- Maximum cached pages: `400`
- Root page: page `0`

The pager loads pages lazily from disk, keeps them in memory, and writes them back when `db_close` is called.
Pages removed by B-tree deletion are linked into a free-page list and reused by later node splits before the file grows.

## B-tree Overview

The table is stored as a B-tree:

- Leaf nodes contain key/value cells.
- Keys are caller-supplied `uint32_t` values.
- Values are fixed-size opaque struct payloads.
- Internal nodes route searches to child pages.
- Leaf nodes split when full.
- Splitting the root creates a new internal root.

The implementation keeps internal-node capacity deliberately small for easier testing and visualization.

## Project Structure

The code is grouped by layer. The API sits at the top and translates CRUD calls
into B-tree operations; the storage and B-tree layers manage pages, cursors,
keys, and values.

### Top-level files

| File | Description |
| --- | --- |
| `README.md` | Engine overview, API examples, storage notes, and this file map. |
| `Makefile` | Builds `libmy_dbms.a` from all source files. Also provides `make clean`. |

### `src/api/`

Public C API for using the database engine.

| File | Description |
| --- | --- |
| `src/api/db.h` | Defines `Db`, `DbResult`, and the generic CRUD function signatures. Values are passed as `void*`; callers provide a separate `uint32_t` key. |
| `src/api/db.c` | Implements `create`, `get`, `update`, and `delete`. It performs duplicate-key checks, copies opaque value bytes into/out of leaf cells, removes cells, and rebalances leaf/internal nodes after deletion. |

### `src/storage/`

Storage primitives shared by the API and B-tree layers.

| File | Description |
| --- | --- |
| `src/storage/pager.h` | Defines page constants, `Pager`, and pager function declarations. Pages are fixed at `4096` bytes and cached in memory. |
| `src/storage/pager.c` | Opens database files, lazily loads pages, flushes pages to disk, tracks the number of pages, and manages a simple free-page list for page reuse. |
| `src/storage/table.h` | Defines `Table`, which owns a `Pager`, root page number, and `value_size`. Declares `db_open_with_value_size` and `db_close`. |
| `src/storage/table.c` | Opens/closes tables. New files initialize page `0` as the root leaf node; existing files restore free-page-list metadata. |
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

Typical C API flow:

1. Caller opens one table with `db_open_with_value_size(filename, sizeof(MyStruct))`.
2. Caller passes `uint32_t` keys plus `void*` pointers to `create`, `get`, `update`, and `delete`.
3. The API copies exactly `table->value_size` bytes into or out of B-tree leaf cells.
4. `src/api/db.c` uses B-tree search/insert/delete helpers to modify pages.
5. `src/storage/pager.c` loads pages lazily and persists them when `db_close` is called.

## Current Limitations

- A database file represents one table with one fixed-size value type.
- The C API supports generic value-level `create`, `get`, `update`, and `delete`.
- The table value size is provided by the caller when opening the file and must match on reopen.
- No query language, secondary indexes, or transactions.
- The database format is experimental and not compatible with SQLite.
- Pages are flushed only when `db_close` is called.
- Error handling is basic and intended for learning, not production use.

## Notes

This is a learning project, not a production database. The code is meant to make database internals visible and hackable: the pager, B-tree node formats, and cursor traversal are all implemented directly in C.
