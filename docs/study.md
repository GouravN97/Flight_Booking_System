# Study Guide: Flight Booking System + Embedded DBMS

This project is a good interview topic because it combines four classic systems concepts in one codebase:

- a networked client/server application
- concurrency control with threads and locks
- persistence through a custom DBMS engine
- a booking domain with race conditions and capacity management

The most important idea is this: the server owns all shared state, clients are thin front ends, and persistence is handled by a small embedded database engine called my_dbms.

---

## 1. What this project is doing

The repository has three main programs:

- server_app: the core backend. It accepts TCP requests, manages flights/users/admins/bookings, and persists data.
- client_app: the user-facing CLI for signup, login, flight lookup, booking, and cancellation.
- admin_app: the admin CLI for creating flights, managing admins, and reading the admin feed.

The system is built around a simple command protocol over TCP. Clients send textual commands like SIGNUP, LOGIN, LIST, BOOK, CANCEL, and ADMIN_CREATE_FLIGHT. The server parses them, updates shared state, and replies.

---

## 2. High-level architecture

### Runtime layers

1. Network layer
   - server_main.c accepts incoming connections
   - each client gets a detached thread
   - handle_client() parses the request and dispatches to the relevant business logic

2. Business logic layer
   - auth_service.c: authentication for users and admins
   - flight_service.c: flight creation, lookup, timing changes, price changes, and auto-creation logic
   - booking_service.c: booking and cancellation flow
   - admin_ipc.c: shared-memory admin messaging and admin request feed

3. Storage layer
   - storage_service.c: translates server state to/from disk using file locks and a DB API mutex
   - db_handler.c: wraps the custom DBMS with server-friendly helpers
   - my_dbms/: the embedded database engine used for persistence

### Why this matters in interviews

Interviewers usually want to hear that you can explain the separation of concerns:

- server handles coordination
- storage handles persistence
- concurrency controls protect shared state
- clients are not trusted with business logic

---

## 3. Main in-memory data structures

The server state is defined in server_state.h.

### Core state

- Flight array: stores all flights in memory
- User array: stores all registered users
- Admin array: stores all admins
- FlightRequest array: tracks repeated booking requests for route-based auto-creation
- Locks:
  - state_lock: protects general shared server state
  - flights_lock: protects the flight array and flight-related operations
  - requests_lock: protects flight request tracking

### Flight structure

Each Flight contains:

- flight metadata: number, source, destination, date, price
- seat array: current seat booking state
- semaphore: remaining seat capacity
- per-seat mutexes: protect individual seat assignment
- booking array: active bookings on that flight
- next_booking_slot: helps allocate booking slots

This is a very important design point: the booking system is not just a single integer seat counter. It tracks per-seat ownership, per-flight bookings, and booking IDs.

---

## 4. How the server works end-to-end

### Startup sequence

On startup, server_init() does the following:

1. initializes all mutexes
2. loads persisted data from disk into memory
3. re-creates runtime synchronization objects for flights
4. ensures a default admin exists if none was loaded
5. creates the admin shared-memory channel
6. starts listening for TCP connections

### Request lifecycle

A request goes through this path:

1. client sends a command over TCP
2. server_main.c creates a thread for that client
3. handle_client() parses the command
4. the relevant service module updates or reads server state
5. the server responds with an OK/ERR/REQUESTED style message

This is a classic event-driven style implemented with threads and a simple request protocol.

---

## 5. How booking works

The booking logic lives in booking_service.c.

### Booking flow

When a user requests seats:

1. the server validates the user exists
2. it tries to find a flight matching the route/date with enough capacity
3. it reserves seat capacity using a semaphore
4. it marks the specific seats as booked using per-seat mutexes
5. it creates a booking record and a booking ID
6. it persists the flight and booking state to disk

### Important detail: race safety

A naive booking implementation would simply decrement a seat count. That would fail under concurrency because two threads could both read the same available count before either writes back.

This project avoids that with two mechanisms:

- a semaphore for capacity counting
- per-seat mutexes for seat assignment

The semaphore is used first to make sure capacity is not exceeded. The per-seat mutexes make sure two threads do not accidentally assign the same seat.

### Why the code uses sem_trywait

The code does:

- sem_trywait() repeatedly for each requested seat
- if it fails, it stops and returns the failed booking

This means it attempts to atomically reserve the exact number of seats required. If not enough are available, the booking fails and the reserved seats are released.

### Why the code also uses seat_mutexes

Even if the semaphore is correct, you still need to protect the seat array because the array tracks which seat is booked and by whom. Each seat has its own mutex, so the system can safely update seat state without data races.

---

## 6. How concurrency is implemented

This is one of the most interview-relevant parts of the project.

### 6.1 Thread-per-client model

The server uses pthreads. Every accepted client connection gets its own detached worker thread.

That means multiple requests can be processed at once.

### 6.2 Mutexes used in the server

- state_lock: protects auth/admin data and general server state
- flights_lock: protects the flights array and flight-level operations
- requests_lock: protects flight auto-creation request tracking

### 6.3 Per-flight synchronization

Each flight has:

- a semaphore for seat availability
- an array of mutexes, one per seat

This is a strong pattern because it allows both global capacity management and precise seat-level protection.

### 6.4 Why this is not enough by itself

The server also persists data to disk, and disk operations are another shared resource. For that reason, storage_service.c adds:

- a global DB API mutex
- file locks using fcntl and flock

This prevents two threads from writing to the same DB file at the same time.

### 6.5 Concurrency test suite

The tests folder contains a concurrency harness that launches many threads at the same time and checks that:

- no overbooking occurs
- each flight fills only up to capacity
- independent flights do not interfere with each other

This is a very good interview talking point because it shows the project was designed to prove the race-condition fix rather than just assume it works.

---

## 7. How the embedded DBMS works

The embedded engine is under my_dbms/. It is a simplified database engine that teaches the core ideas behind real databases without being production-scale.

### 7.1 The core idea

The DBMS is not using SQL in the full sense. It supports a tiny subset of commands through a shell and a C API.

It stores records in a file-backed B-tree, with a pager in front of it.

### 7.2 Pager

The pager is the lowest-level storage component.

Its job is:

- open a database file
- read pages into memory when needed
- write dirty pages back to disk when flushed
- cache pages so repeated access is cheap

A page is a fixed-size chunk of the database file. The pager uses OS file operations like open, read, write, lseek, and close.

This is important because real databases also use pages, though usually with much more complex paging and buffering logic.

### 7.3 Table and root page

A table object wraps:

- a pager
- a root page number
- the size of the stored value

When a database file is first created, page 0 is initialized as a leaf node. That becomes the root of the initial B-tree.

### 7.4 B-tree structure

The DBMS uses a B-tree to store data sorted by key.

There are two main node types:

- leaf node: stores actual key/value pairs
- internal node: stores keys and child pointers to other nodes

A B-tree is used because it keeps inserts, updates, and searches efficient with a small number of disk accesses.

### 7.5 Why B-tree is used here

A B-tree gives:

- sorted key order
- fast lookup by key
- efficient inserts without rewriting the whole file
- good balance between CPU and disk I/O

### 7.6 Cursor

A cursor is used to move through the table. It knows:

- which page it is on
- which cell within that page
- whether it has reached the end

This is what allows the DBMS to scan rows and find a specific key.

### 7.7 How create/get/update/delete work

The API layer in my_dbms/src/api/db.c implements the basic operations:

- create: insert a new key/value pair
- get: find a value by key
- update: replace an existing value for a key
- delete: remove a key/value pair and rebalance the tree if needed

On insert, if a leaf node becomes full, the engine splits it into two leaf nodes. If the tree grows too much, it creates a new parent internal node.

On delete, the engine rebalances or merges nodes to keep the tree balanced.

This is a very important interview point: the DBMS is not just storing rows in a flat file. It uses a tree structure so it can scale and stay organized.

---

## 8. How the server uses the DBMS for persistence

The flight booking server does not write raw structs directly to disk by hand. Instead, it uses the DBMS through storage/db_handler.c and storage/storage_service.c.

### Mapping from server state to DB files

The server persists four logical tables:

- flights.db: flight records
- users.db: user records
- admins.db: admin records
- bookings.db: booking records

Each file uses a different record layout, but all of them are stored through the same DBMS engine.

### Why this matters

This is an example of a layered design:

- server state is in memory
- DBMS is the disk abstraction
- storage_service.c translates between the two

### Booking records and key scheme

Bookings are stored per flight, not in one giant array. The key is built from the flight index and the booking slot index. This makes it possible to organize bookings by flight while still using a simple key/value store.

That is a strong interview point because it shows the system is designed to avoid collisions and make persistence predictable.

---

## 9. How persistence is made safe

A common interview question is: “How do you avoid corrupted state when multiple threads write to the DB?”

This project answers that in two layers.

### Layer 1: DB API mutex

storage_service.c defines a global mutex called g_db_api_lock.

Every database operation that touches the DBMS goes through this lock. That ensures only one thread uses the DB API at a time.

### Layer 2: file locking

The code uses fcntl and flock to lock the database files while writing. This prevents simultaneous writes from interleaving badly.

So the order is roughly:

1. acquire DB API mutex
2. acquire file lock on the DB file
3. open the DB table
4. write/update records
5. close the DB table
6. release file lock
7. release DB API mutex

This is a realistic strategy for making embedded persistence safe without a full database engine.

---

## 10. How auto-flight creation works

If many users request the same route and no matching flight exists, the server can auto-create a flight.

The flow is:

1. the booking request is tracked in a request structure
2. once the request count passes a threshold, the server may create a flight automatically
3. the new flight is created with a suggested capacity
4. the system publishes an admin notification

This is a nice extension because it shows the project is not only about direct booking; it also handles demand-driven flow.

---

## 11. The most important interview talking points

If you need to speak confidently in an interview, focus on these points:

1. The server is the single authority for shared state.
2. Booking safety is ensured by semaphores and mutexes.
3. The system uses per-flight and per-seat synchronization to avoid race conditions.
4. The DBMS is a minimal B-tree-based storage engine with a pager and cursor abstraction.
5. Persistence is layered: server state -> storage_service -> DBMS -> disk.
6. The project includes a concurrency test suite, which is evidence that race conditions were considered seriously.

---

## 12. Quick interview summary

If you want a 30-second explanation, say this:

“This project is a CLI-based flight booking system implemented in C. The server handles clients over TCP, uses pthreads for concurrency, protects shared state with mutexes and semaphores, and persists data through a custom B-tree-based DBMS with a pager and file locking. The biggest design challenge is preventing overbooking under concurrency, and the solution is a combination of semaphore-based seat counting and per-seat mutexes.”

---

## 13. Suggested revision order

If you want to study it efficiently, follow this order:

1. read server_state.h and understand the in-memory model
2. read server_main.c and server_protocol.c to see request flow
3. read booking_service.c to understand booking and concurrency
4. read storage_service.c and db_handler.c to understand persistence
5. read my_dbms/src/storage/pager.c and my_dbms/src/api/db.c to understand DB internals
6. read tests/TEST_CASES.md to understand the intended correctness guarantees

That order will let you connect the high-level architecture to the low-level implementation.
