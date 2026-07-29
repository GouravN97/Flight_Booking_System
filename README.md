# Flight Booking System

A CLI-based flight booking system written in C. Three programs work together over TCP sockets while the server coordinates concurrent bookings with mutexes, semaphores, shared memory, and file-backed storage.


| Program      | Role                                                                          |
| ------------ | ----------------------------------------------------------------------------- |
| `server_app` | Owns shared state, handles socket requests, persists data                     |
| `client_app` | User CLI for signup, login, flight lookup, booking, cancellation, and history |
| `admin_app`  | Admin CLI for managing flights, admins, and the shared admin feed             |


## Requirements

- A C compiler (`gcc` or `clang`)
- `make`
- A Unix-like environment with POSIX threads, shared memory, and sockets

## Build

From the project root:

```sh
make
```

This produces `server_app`, `client_app`, and `admin_app`.

To remove build artifacts and local database files:

```sh
make clean
```

## Run

Start the server first:

```sh
./server_app
```

The default port is `9090`. A custom port can be supplied:

```sh
./server_app 9091
```

Run user and admin clients from separate terminals:

```sh
./client_app 127.0.0.1 9090
./admin_app 127.0.0.1 9090
```

Host and port can be passed as arguments:

```sh
./client_app <host> <port>
./admin_app <host> <port>
```

If the server has no persisted admin user, it creates a default admin on startup:

```text
Admin ID: admin
Password: admin123
```

## Project Workflow: Start to Finish

This section walks through the full lifecycle of the system: building it, starting it, running admin and user clients, handling bookings (including edge cases), and shutting down with persistence.

### Overview

Three programs cooperate over TCP. The server owns all shared state; clients send one-line text commands and receive text responses. Persistence is handled by the embedded `my_dbms` B-tree engine.

```mermaid
flowchart LR
    subgraph clients [Clients]
        CA[client_app]
        AA[admin_app]
    end

    subgraph server [server_app]
        TCP[TCP listener]
        PROTO[server_protocol]
        AUTH[auth_service]
        FLIGHT[flight_service]
        BOOK[booking_service]
        STORE[storage_service]
        IPC[admin_ipc / shared memory]
    end

    subgraph disk [Persistence]
        FDB[(flights.db)]
        UDB[(users.db)]
        BDB[(bookings.db)]
        ADB[(admins.db)]
    end

    CA -->|TCP commands| TCP
    AA -->|TCP commands| TCP
    TCP --> PROTO
    PROTO --> AUTH
    PROTO --> FLIGHT
    PROTO --> BOOK
    AUTH --> STORE
    FLIGHT --> STORE
    BOOK --> STORE
    FLIGHT --> IPC
    BOOK --> IPC
    STORE --> FDB
    STORE --> UDB
    STORE --> BDB
    STORE --> ADB
    AA -->|READADMIN / ADMINMSG| IPC
```



### Step 1: Build the project

From the project root:

```sh
make
```

This compiles:

- `server_app` — server binary, linked with the `my_dbms` storage engine
- `client_app` — user CLI
- `admin_app` — admin CLI

To reset build artifacts and local database files:

```sh
make clean
```

Run `make clean` before your first run after upgrading, or whenever you want a fresh database. Old `.db` files from a previous storage format are not compatible with the current my_dbms-backed layout.

### Step 2: Start the server

Open a terminal and start the server first. All clients depend on it.

```sh
./server_app          # listens on port 9090
./server_app 9091     # custom port
```

When `server_app` starts, it performs the following initialization sequence:

1. **Allocate server state** — zeroes in-memory arrays for flights (each with its own bookings array), users, admins, and flight requests.
2. **Initialize mutexes** — sets up `state_lock`, `flights_lock`, and `requests_lock`.
3. **Load persistence** — `storage/storage_service.c` opens or creates four database files (`flights.db`, `users.db`, `bookings.db`, `admins.db`) through the my_dbms adapter in `storage/db_handler.c`. Only records that were previously saved are loaded back; empty slots stay empty. Legacy flat `bookings.db` files are migrated automatically into the per-flight layout on load.
4. **Rebuild runtime sync objects** — for each loaded flight, mutexes, seat mutexes, and the seat-counting semaphore are recreated (these are not stored on disk).
5. **Ensure a default admin exists** — if no admin with a password is found in `admins.db`, the server creates `admin` / `admin123`.
6. **Create the admin feed** — opens POSIX shared memory `/flight_admin_bus` (4096 bytes) for admin notifications and cross-admin messaging.
7. **Start the TCP listener** — binds to the chosen port and spawns a detached thread per incoming client connection.

The server prints:

```text
Server listening on port 9090
```

Leave this terminal running for the entire session.

### Step 3: Connect the admin client

Open a second terminal and start the admin CLI:

```sh
./admin_app 127.0.0.1 9090
```

The admin app verifies the server is reachable (via a `PING` command), then shows an authentication menu:

1. **Admin Login** — enter `admin` / `admin123` on first run (or your own credentials after creating admins).
2. After login, the admin menu provides:
  - Create another admin user
  - Create a flight (flight number, source, destination, date, seat count, price)
  - Change a flight date
  - Update a flight price
  - List all flights
  - List all admins
  - Write a message to the shared admin feed
  - Read the shared admin feed

Each menu choice is translated by `client/client.c` into a socket command (for example, `ADMIN_CREATE_FLIGHT admin admin123 F101 BLR DEL 1 5 2026 20 4500`) and sent to the server.

**Typical first-time admin setup:**

1. Log in as `admin` / `admin123`.
2. Choose **Create Flight** and enter route details, for example:
  - Flight number: `F101`
  - Source: `BLR`, Destination: `DEL`
  - Date: `1 5 2026` (day month year)
  - Seats: `20`, Price: `4500`
3. The server stores the flight in memory, initializes seat mutexes and the seat semaphore, and flushes the record to `flights.db`.

Flights must exist before users can book them, unless demand triggers automatic creation (see Step 7).

### Step 4: Connect the user client

Open a third terminal and start the user CLI:

```sh
./client_app 127.0.0.1 9090
```

The user app also pings the server, then shows an authentication menu:

1. **Signup** — register a new user ID and password.
2. **Login** — authenticate with existing credentials.

After login, the user menu provides:

- List flights for a source and destination
- View details for a specific flight number
- Book seats (source, destination, date, seat count)
- View current bookings
- Cancel a booking by booking ID

### Step 5: User signup and login

**Signup flow:**

1. User selects Signup and enters an ID and password.
2. `client_app` sends `SIGNUP <user> <password>` to the server.
3. The server locks `state_lock`, checks for duplicate IDs, allocates a user slot, and writes the record to `users.db`.
4. Server responds `OK`. The client prompts the user to log in.

**Login flow:**

1. User selects Login and enters credentials.
2. `client_app` sends `LOGIN <user> <password>`.
3. The server verifies the ID and password against in-memory state.
4. Server responds `OK` on success. The client unlocks the booking menu.

Authentication is stateless on the server side — the client tracks whether the user is logged in locally. Each subsequent command includes the user ID where required (for example, `BOOK alice BLR DEL 1 5 2026 2`).

### Step 6: Search and book a flight (happy path)

Once logged in, a typical booking proceeds as follows:

1. **List flights** — user enters source and destination (for example, `BLR` and `DEL`). The server returns all matching flights with date, available seats, and price.
2. **Book seats** — user enters source, destination, date (`day month year`), and seat count.
3. The server runs the booking pipeline:

```mermaid
flowchart TD
    A[BOOK command received] --> B{User exists?}
    B -->|No| Z[Return error]
    B -->|Yes| C{Matching flight with capacity?}
    C -->|Yes| D[Acquire seat semaphore slots]
    D --> E[Lock flights_lock and seat mutexes]
    E --> F[Mark seats booked on matching flight]
    F --> G[Create booking on flight e.g. F101-BK0001]
    G --> H[Flush flights.db and flight bookings to bookings.db]
    H --> I[Return OK F101-BK0001]
    C -->|No matching flight| J[Track request / admin feed]
    C -->|Flight full| K[Track request / admin feed]
    J --> L{20+ requests for route/date?}
    K --> L
    L -->|Yes| M[Auto-create flight AUTOYYYYMMDD]
    M --> N[Retry booking on new flight]
    L -->|No| O[Return request-sent to user]
```



1. On success, the server returns a flight-scoped booking ID such as `OK F101-BK0001`.
2. The user can choose **View My Bookings** to confirm the reservation.

Each flight owns its own bookings array (`flight->bookings[]`). The booking record stores the user ID, flight number, seat count, and allocated seat numbers.

### Step 7: When no flight is available

If the user requests a route and date that has no flight, or all matching flights are full, the server does not fail silently. Instead:

1. **Tracks the request** — increments a counter for that source, destination, and date combination.
2. **Publishes to the admin feed** — writes a message to shared memory `/flight_admin_bus`, for example:
  ```text
   Flight creation requested by user alice: BLR->DEL on 01/05/2026, seats requested:2, suggested capacity:12, reason:no matching flight exists
  ```
3. **Returns a request-sent status** to the user client — the booking is not created yet.

**Two ways the situation resolves:**


| Path                  | Trigger                                  | What happens                                                                                                                                                                                   |
| --------------------- | ---------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Manual admin creation | Any number of requests                   | Admin reads the feed (menu option 8), creates the flight manually, and the user retries booking.                                                                                               |
| Automatic creation    | 20 users request the same route and date | Server auto-creates flight `AUTO<YYYYMMDD>`, sets capacity to requested seats + 10 (max 25), price to 0, notifies the admin feed, and the triggering user completes their booking immediately. |


Only one auto-created flight is generated per route/date combination. Admins can update the price later via **Update Flight Price**.

### Step 8: Cancel a booking

From the user client, select **Cancel Booking** and enter the booking ID (for example, `F101-BK0001`).

The server:

1. Finds the booking by ID across all flights.
2. Clears the in-memory booking slot on that flight.
3. Releases each seat under its seat mutex.
4. Posts back to the flight semaphore for every released seat.
5. Deletes the booking from `bookings.db` and flushes updated seat state to `flights.db`.

The seat becomes available for other users immediately.

### Step 9: Admin monitoring and coordination

Admins use the shared-memory feed throughout the session:

- **Read feed** (menu option 8 or `READADMIN` socket command) — see flight creation requests, auto-creation notifications, and messages from other admins.
- **Write message** (menu option 7 or `ADMINMSG`) — broadcast notes to other admin sessions.

Example auto-creation notification in the feed:

```text
Auto-created flight AUTO20260501 for BLR->DEL on 01/05/2026 (capacity:12). Pending users may now book.
```

Admin actions such as creating flights, changing dates, and updating prices are also logged to the feed and persisted to `flights.db` or `admins.db` as appropriate.

### Step 10: Concurrent operation

Multiple user and admin clients can connect at the same time. The server handles each connection in its own thread (`server/server_main.c`).

Synchronization layers prevent data races:

- `flights_lock` protects the flights array, per-flight booking slot allocation, and booking lookups.
- Each seat has its own mutex for seat state.
- A counting semaphore per flight tracks available seats and blocks booking when full.
- File locks and a DB API mutex protect my_dbms reads and writes.

This means two users can book different flights in parallel, while seat conflicts on the same flight are serialized safely.

### Step 11: Shut down the server

Stop the server with `Ctrl+C` or `SIGTERM`. The signal handler calls `server_shutdown`, which:

1. Flushes all in-use flights, admins, users, and bookings to disk through my_dbms.
2. Unmaps and unlinks the shared-memory admin feed.
3. Destroys flight mutexes, seat mutexes, and semaphores.
4. Destroys the global server mutexes.

Database files remain in the directory where the server was started. Starting the server from a different directory creates or uses a separate set of `.db` files.

### Step 12: Restart and recover state

To resume after shutdown:

1. Start `server_app` again in the same directory.
2. The server reloads all persisted records from the four `.db` files.
3. Flight mutexes and semaphores are rebuilt from saved seat occupancy.
4. Start `client_app` and `admin_app` — existing users, flights, and bookings are available immediately without re-entering data.

**Quick verification after restart** (using `nc` or the CLIs):

```sh
printf 'MYBOOKINGS alice\n' | nc 127.0.0.1 9090
printf 'LIST BLR DEL\n' | nc 127.0.0.1 9090
```

### Complete first-session checklist

Use this as a practical run-through for a fresh install:


| Step | Terminal   | Action                                               |
| ---- | ---------- | ---------------------------------------------------- |
| 1    | Any        | `make`                                               |
| 2    | Terminal 1 | `./server_app`                                       |
| 3    | Terminal 2 | `./admin_app 127.0.0.1 9090` → login → create flight |
| 4    | Terminal 3 | `./client_app 127.0.0.1 9090` → signup → login       |
| 5    | Terminal 3 | List flights → book seats → view bookings            |
| 6    | Terminal 2 | Read admin feed to monitor requests                  |
| 7    | Terminal 3 | Cancel booking (optional)                            |
| 8    | Terminal 1 | `Ctrl+C` to shut down and persist                    |
| 9    | Terminal 1 | Restart `./server_app` and confirm data survived     |


### Raw protocol alternative

The CLI menus in `client_app` and `admin_app` are wrappers around the text protocol. You can drive the same workflow directly with tools like `nc`:

```sh
printf 'SIGNUP alice secret\n' | nc 127.0.0.1 9090
printf 'ADMIN_CREATE_FLIGHT admin admin123 F101 BLR DEL 1 5 2026 20 4500\n' | nc 127.0.0.1 9090
printf 'LIST BLR DEL\n' | nc 127.0.0.1 9090
printf 'BOOK alice BLR DEL 1 5 2026 2\n' | nc 127.0.0.1 9090
```

See **Communication Model** below for the full command reference.

## User Workflow

Users interact through `client_app`.

Available actions:

- Signup and login
- List flights for a source and destination
- View details for a flight number
- Book seats for a source, destination, date, and seat count
- View current bookings
- Cancel a booking by booking ID

When a booking succeeds, the server returns a flight-scoped booking ID such as `F101-BK0001` (`<flight_number>-BK<seq>`). The booking record is stored in that flight's `bookings[]` array and includes the user ID, flight number, seat count, and allocated seat numbers.

## Admin Workflow

Admins interact through `admin_app`.

Available actions:

- Login as an admin
- Create another admin user
- Create a flight with flight number, route, date, seat count, and price
- Change a flight date
- Update a flight price
- List all flights
- List all admins
- Write to the shared-memory admin feed
- Read the shared-memory admin feed

Admins are responsible for creating flights on demand. Users cannot create flights directly.

## Booking Data Model

Bookings live on each flight, not in a top-level server array:

- In memory: `Flight.bookings[MAX_BOOKINGS_PER_FLIGHT]` with a per-flight `next_booking_slot` cursor.
- On disk: `bookings.db` keys partition slots by flight index (see **Persistence** below).
- IDs: `<flight_number>-BK<seq>` (for example, `F101-BK0001`).

Listing or cancelling a user's booking scans all flights and their per-flight booking arrays. Legacy flat `bookings.db` files are migrated on server startup.

## Booking Behavior

When a user books seats, the server:

1. Verifies the user exists.
2. Searches for a matching flight by source, destination, day, month, and year.
3. Checks the flight semaphore to confirm enough seats are available.
4. Locks the selected flight while seats are allocated.
5. Locks individual seat mutexes before marking seats as booked.
6. Stores the booking and flushes updated flight and booking data to disk.

If no matching flight exists, or if all matching flights are full, the server tracks the request. When 20 or more users request the same route and date combination, the system automatically creates a flight (see **Automatic Flight Creation** below). Otherwise:

1. Publishes a flight creation request to the admin feed.
2. Returns a request-sent status to the user client.
3. Leaves flight data unchanged until an admin manually creates a flight or the threshold is reached.

The admin feed message includes the requesting user, route, date, requested seats, suggested capacity, and the reason for the request.

Example admin feed message:

```text
Flight creation requested by user user1: BLR->DEL on 01/05/2026, seats requested:2, suggested capacity:12, reason:no matching flight exists
```

## Automatic Flight Creation

To reduce dependency on manual admin intervention, the system automatically creates flights when demand is high enough.

**How it works:**

- The server tracks user requests for flights that do not exist.
- When 20 users have requested the same route and date combination, a flight is automatically created.
- The auto-created flight is assigned a unique flight number: `AUTO<YYYYMMDD>` (e.g., `AUTO20260501`).
- The flight capacity is set to the requested seats plus a 10-seat buffer, capped at 25 seats per flight.
- The flight price is initially set to 0; admins can update it later.
- The user whose request triggered the auto-creation immediately proceeds with booking if the newly created flight has sufficient capacity.
- Other pending users are notified via the admin feed that their requested flight has been created.

**Configuration:**

- Threshold for automatic creation: `20` users (`AUTO_CREATE_FLIGHT_THRESHOLD` in `server/server_state.h`)
- Maximum tracked requests: `10000` (`MAX_FLIGHT_REQUESTS` in `server/server_state.h`)

**Note:** Only one flight is auto-created per route/date combination, even if more than 20 requests accumulate. If a flight already exists for that route and date (even if full), no duplicate is auto-created. Subsequent requests use the existing flight.

## Cancellation Behavior

When a user cancels a booking, the server:

1. Finds the booking by booking ID.
2. Clears the booking record.
3. Finds the booked flight.
4. Unlocks each seat from the booking under its seat mutex.
5. Posts back to the flight semaphore for every released seat.
6. Flushes the updated flight and booking data.

##  Concurrency Model

The server uses several layers of synchronization:

- `state_lock` protects admin/user authentication state.
- `flights_lock` protects the flights array during flight searches, creation, and per-flight booking slot allocation.
- `requests_lock` protects flight-request tracking for auto-creation.
- Each seat has a `seat_mutex` to protect individual seat state.
- Each flight has a counting semaphore initialized to the number of available seats.
- Database files are protected with `fcntl` file locks during reads and writes.
- A process-wide DB API mutex protects the shared my_dbms handler state.

This architecture enables concurrent bookings: multiple clients can book different flights in parallel. Seat conflicts on the same flight are prevented by `flights_lock` during slot allocation and by per-seat mutexes during seat assignment.

## Communication Model

User and admin clients communicate with the server over TCP sockets. Each request is a single line of text; the server returns a text response.

Supported commands:


| Command                                                                                                | Description                         |
| ------------------------------------------------------------------------------------------------------ | ----------------------------------- |
| `SIGNUP <user> <password>`                                                                             | Register a new user                 |
| `LOGIN <user> <password>`                                                                              | Authenticate a user                 |
| `LIST <source> <destination>`                                                                          | List flights on a route             |
| `DETAIL <flight_number>`                                                                               | Show details for one flight         |
| `BOOK <user> <source> <destination> <day> <month> <year> <seats>`                                      | Book seats                          |
| `CANCEL <booking_id>`                                                                                  | Cancel a booking                    |
| `MYBOOKINGS <user>`                                                                                    | List a user's bookings              |
| `ADMIN_LOGIN <admin> <password>`                                                                       | Authenticate an admin               |
| `ADMIN_CREATE <actor> <actor_pw> <new_admin> <new_pw>`                                                 | Create an admin                     |
| `ADMIN_CREATE_FLIGHT <actor> <actor_pw> <flight> <source> <dest> <day> <month> <year> <seats> <price>` | Create a flight                     |
| `ADMIN_CHANGE_TIMING <actor> <actor_pw> <flight> <day> <month> <year>`                                 | Change flight date                  |
| `ADMIN_UPDATE_PRICE <actor> <actor_pw> <flight> <price>`                                               | Update flight price                 |
| `ADMIN_LIST_FLIGHTS <actor> <actor_pw>`                                                                | List all flights                    |
| `ADMIN_LIST_ADMINS <actor> <actor_pw>`                                                                 | List all admins                     |
| `ADMINMSG <message>`                                                                                   | Publish a message to the admin feed |
| `READADMIN`                                                                                            | Read the shared admin feed          |


The CLI programs in `client/client.c` wrap these commands so users and admins can work through menus instead of typing protocol messages manually.

### Example protocol session

```sh
# Start the server, then from another terminal:
printf 'SIGNUP alice secret\n' | nc 127.0.0.1 9090
# OK

printf 'ADMIN_CREATE_FLIGHT admin admin123 F101 BLR DEL 1 5 2026 20 4500\n' | nc 127.0.0.1 9090
# OK

printf 'LIST BLR DEL\n' | nc 127.0.0.1 9090
# Flights:
# F101 date:01/05/2026 seats:20/20 price:4500.00

printf 'BOOK alice BLR DEL 1 5 2026 2\n' | nc 127.0.0.1 9090
# OK F101-BK0001
```

## Shared-Memory Admin Feed

The admin feed is backed by POSIX shared memory named `/flight_admin_bus` (4096 bytes).

It is used for:

- Admin-to-admin messages
- Admin change logs, such as flight creation or price updates
- User-triggered flight creation requests when booking cannot proceed
- Notifications when a flight is auto-created

Admins can read this feed from the admin app (menu option 8) or via the `READADMIN` socket command.

Example auto-creation notification:

```text
Auto-created flight AUTO20260501 for BLR->DEL on 01/05/2026 (capacity:12). Pending users may now book.
```

## Persistence

The server persists data through the B-tree engine in `my_dbms/`. `storage/storage_service.c` maps each domain table to a separate database file and uses the my_dbms C API (`create`, `get`, `update`, `delete`, `db_open_with_value_size`, `db_close`) via a thin adapter in `storage/db_handler.c`.


| File          | Contents                          |
| ------------- | --------------------------------- |
| `flights.db`  | Flight records and seat occupancy |
| `admins.db`   | Admin accounts                    |
| `users.db`    | User accounts                     |
| `bookings.db` | Per-flight booking records        |


Each file is a my_dbms table keyed by slot index (`uint32_t`). Records are fixed-size structs that match the in-memory server state. On startup, missing database files are created and initialized. Existing files are loaded back into memory. Runtime-only synchronization objects such as mutexes and semaphores are rebuilt after persisted flight records are loaded.

**Key layout:**

| File          | Key                         | Record                         |
| ------------- | --------------------------- | ------------------------------ |
| `flights.db`  | Flight slot index           | `FlightPersist` (seats, route) |
| `users.db`    | User slot index             | `UserEntry`                    |
| `admins.db`   | Admin slot index            | `AdminEntry`                   |
| `bookings.db` | `flight_index * MAX_BOOKINGS_PER_FLIGHT + booking_index` | `BookingRecord` |

Bookings are not stored in a single global array. Each flight has up to `MAX_BOOKINGS_PER_FLIGHT` slots in memory and in `bookings.db`. Booking IDs are flight-scoped (`F101-BK0001`), and `storage_flush_flight_bookings()` writes only the bookings for one flight after a book or cancel.

Starting the server from a different directory creates or uses a different set of database files.

**Note:** Database files use the my_dbms page format, not the older fixed-record layout. Run `make clean` before restarting if you have leftover `.db` files from a previous version.

## Concurrency Tests

An integration test suite verifies that simultaneous bookings do not overbook seats and that different flights can be booked in parallel. Each case uses hard-coded protocol commands, expected OK/reject counts, and post-condition `DETAIL` checks.

```sh
make test-concurrency
```

This builds `concurrent_booking_test`, starts an isolated server on port `19090`, runs seven barrier-synchronized race scenarios, and prints per-thread inputs/outputs. Full case documentation with expected responses is in [`tests/TEST_CASES.md`](tests/TEST_CASES.md).

| Test | Scenario | Expected |
| ---- | -------- | -------- |
| 01 | 10 clients, 1 seat each, 5-seat flight | 5 OK, 5 reject, `seats:0/5` |
| 02 | 5 clients × 2 different flights | 10 OK, both flights full |
| 03 | 5 clients × 2 seats on 10-seat flight | 5 OK, `seats:0/10` |
| 04 | 10 clients × 2 seats on 10-seat flight | 5 OK, 5 reject |
| 05 | 8 clients, 1-seat flight | 1 OK, 7 reject |
| 06 | Parallel bookings on two flights | `CF107-BK0001` and `CF108-BK0001` |
| 07 | 50 clients on 25-seat flight | 25 OK, 25 reject, `seats:0/25` |

Under contention, rejected bookings may return either `ERR booking failed` or `REQUESTED flight unavailable`; both count as failures. The seat-count verification is the overbooking guard.

## Important Limits

The main limits are defined in `server/server_state.h`:


| Constant                       | Value      |
| ------------------------------ | ---------- |
| `MAX_FLIGHTS`                  | 1000       |
| `MAX_SEATS_PER_FLIGHT`         | 25         |
| `MAX_USERS`                    | 10000      |
| `MAX_BOOKINGS_PER_FLIGHT`      | 1000       |
| `MAX_ADMINS`                   | 128        |
| `MAX_FLIGHT_REQUESTS`          | 10000      |
| `ADMIN_SHM_SIZE`               | 4096 bytes |
| `AUTO_CREATE_FLIGHT_THRESHOLD` | 20 users   |


## Project Structure

Source code is grouped by role. The Makefile adds `-Iserver`, `-Iclient`, and `-Istorage` so modules can include headers by name (for example, `#include "server_state.h"`).

```text
redux/
├── Makefile
├── README.md
├── apps/                     # Program entry points
│   ├── server_app.c          # Server main
│   ├── client_app.c          # User CLI main
│   └── admin_app.c           # Admin CLI main
├── server/                   # Server core, protocol, and services
│   ├── server.h              # Server umbrella header
│   ├── server_state.c/h      # Shared state and lifecycle
│   ├── server_main.c/h       # TCP listener and thread dispatch
│   ├── server_protocol.c/h   # Socket command handlers
│   ├── auth_service.c/h      # User signup and login
│   ├── flight_service.c/h    # Flight CRUD and request tracking
│   ├── booking_service.c/h   # Booking and cancellation
│   └── admin_ipc.c/h         # Shared-memory admin feed
├── client/                   # Shared client library for both CLIs
│   ├── client.c
│   └── client.h
├── storage/                  # Persistence layer
│   ├── storage_service.c/h   # Domain-to-disk mapping
│   └── db_handler.c/h        # Adapter over my_dbms C API
└── my_dbms/                  # B-tree DBMS engine
    ├── Makefile
    ├── README.md
    └── src/
        ├── api/                # Public CRUD API
        ├── app/                # Standalone DBMS shell
        ├── btree/              # B-tree page layout and algorithms
        ├── shell/              # REPL helpers
        ├── sql/                # Demo SQL parser and executor
        └── storage/            # Pager, table, cursor, row
```

Build artifacts (`server_app`, `client_app`, `admin_app`) and runtime database files (`*.db`) are written to the project root.