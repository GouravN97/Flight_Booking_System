# Flight Booking System

This project is a CLI-based flight booking system written in C. It has three runnable programs:

- `server_app`: owns the shared state, listens for socket requests, and persists data.
- `client_app`: user-facing CLI for signup, login, flight lookup, booking, cancellation, and booking history.
- `admin_app`: admin-facing CLI for creating admins, creating/updating flights, listing data, and reading the admin feed.

The system combines sockets, shared memory, mutexes, semaphores, and file-backed storage to model a concurrent flight booking workflow.

## Build

Build from the project root:

```sh
gcc -Wall -Wextra -pthread -o server_app server_app.c server_main.c server_state.c server_protocol.c auth_service.c flight_service.c booking_service.c admin_ipc.c storage_service.c db_handler.c
gcc -Wall -Wextra -o client_app client_app.c client.c
gcc -Wall -Wextra -o admin_app admin_app.c client.c
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

If the server has no persisted admin user, it creates a default admin:

```text
Admin ID: admin
Password: admin123
```

## User Workflow

Users interact through `client_app`.

Available user actions:

- Signup and login.
- List flights for a source and destination.
- View details for a flight number.
- Book seats for a source, destination, date, and seat count.
- View current bookings.
- Cancel a booking by booking ID.

When a booking succeeds, the server returns a booking ID such as `BK000001`. The booking record stores the user ID, flight number, seat count, and allocated seat numbers.

## Admin Workflow

Admins interact through `admin_app`.

Available admin actions:

- Login as an admin.
- Create another admin user.
- Create a flight with flight number, route, date, seat count, and price.
- Change a flight date.
- Update a flight price.
- List all flights.
- List all admins.
- Write to the shared-memory admin feed.
- Read the shared-memory admin feed.

Admins are responsible for creating flights on demand. Users cannot create flights directly.

## Booking Behavior

When a user books seats, the server:

1. Verifies the user exists.
2. Searches for a matching flight by source, destination, day, month, and year.
3. Checks the flight semaphore to confirm enough seats are available.
4. Locks the selected flight while seats are allocated.
5. Locks individual seat mutexes before marking seats as booked.
6. Stores the booking and flushes updated flight and booking data to disk.

If no matching flight exists, or if all matching flights are full, the server does not automatically create a flight. Instead it:

1. Publishes a flight creation request to the admin feed.
2. Returns a request-sent status to the user client.
3. Leaves flight data unchanged until an admin manually creates a flight.

The admin feed message includes the requesting user, route, date, requested seats, suggested capacity, and the reason for the request.

Example admin feed message:

```text
Flight creation requested by user user1: BLR->DEL on 01/05/2026, seats requested:2, suggested capacity:10, reason:no matching flight exists
```

## Cancellation Behavior

When a user cancels a booking, the server:

1. Finds the booking by booking ID.
2. Clears the booking record.
3. Finds the booked flight.
4. Unlocks each seat from the booking.
5. Posts back to the flight semaphore for every released seat.
6. Flushes the updated flight and booking data.

## Concurrency Model

The server uses several layers of synchronization:

- `state_lock` protects high-level server state during request handling.
- Each flight has a `flight_mutex` to serialize seat allocation and cancellation on that flight.
- Each seat has a `seat_mutex` to protect the booked flag and passenger ID.
- Each flight has a counting semaphore initialized to the number of available seats.
- Database files are protected with `fcntl` file locks during reads and writes.
- A process-wide DB API mutex protects the shared low-level database handler state.

This prevents two clients from booking the same seat at the same time.

## Communication Model

User/admin clients communicate with the server over TCP sockets.

The server understands text commands such as:

- `SIGNUP <user> <password>`
- `LOGIN <user> <password>`
- `LIST <source> <destination>`
- `DETAIL <flight_number>`
- `BOOK <user> <source> <destination> <day> <month> <year> <seats>`
- `CANCEL <booking_id>`
- `MYBOOKINGS <user>`
- `ADMIN_LOGIN <admin> <password>`
- `ADMIN_CREATE <actor_admin> <actor_password> <new_admin> <new_password>`
- `ADMIN_CREATE_FLIGHT <actor_admin> <actor_password> <flight> <source> <destination> <day> <month> <year> <seats> <price>`
- `ADMIN_CHANGE_TIMING <actor_admin> <actor_password> <flight> <day> <month> <year>`
- `ADMIN_UPDATE_PRICE <actor_admin> <actor_password> <flight> <price>`
- `ADMIN_LIST_FLIGHTS <actor_admin> <actor_password>`
- `ADMIN_LIST_ADMINS <actor_admin> <actor_password>`
- `ADMINMSG <message>`
- `READADMIN`

The CLI programs in `client.c` wrap these commands so users and admins can work through menus instead of typing protocol messages manually.

## Shared-Memory Admin Feed

The admin feed is backed by POSIX shared memory named `/flight_admin_bus`.

It is used for:

- Admin-to-admin messages.
- Admin change logs, such as flight creation or price updates.
- User-triggered flight creation requests when booking cannot proceed.

Admins can read this feed from the admin app.

## Persistence

The server persists data in fixed-record database files in the directory where the server is started:

- `flights.db`
- `admins.db`
- `users.db`
- `bookings.db`

On startup, missing database files are created and initialized. Existing files are loaded back into memory. Runtime-only synchronization objects such as mutexes and semaphores are rebuilt after persisted flight records are loaded.

Starting the server from a different directory creates or uses a different set of database files.

## Important Limits

The main limits are defined in `server_state.h`:

- Maximum flights: `1000`
- Maximum seats per flight: `25`
- Maximum users: `10000`
- Maximum bookings: `100000`
- Maximum admins: `128`
- Admin shared-memory feed size: `4096` bytes

## Typical End-to-End Flow

1. Start `server_app`.
2. Start `admin_app` and log in with `admin` / `admin123`.
3. Create a flight from the admin app.
4. Start `client_app`.
5. Signup or login as a user.
6. List flights for a route.
7. Book available seats.
8. View bookings or cancel a booking.
9. If a user requests a missing or full route/date, read the admin feed from `admin_app`.
10. Create the requested flight manually from `admin_app`.

