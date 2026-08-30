# Flight Booking HTTP API

This README describes **why** the HTTP API exists, **how** it was built, and **how to run it**. It is separate from the main project README (CLI / C server) and from [`API_PLAN.md`](../../API_PLAN.md) (the original design contract).

The API is not a second booking engine. It is a thin **HTTP gateway** in front of `server_app`.

---

## Why this API exists

The flight booking system is a C program. Users and admins talk to it over **TCP** with one-line text commands (`LOGIN alice secret`, `BOOK <token> BLR DEL 1 5 2026 2`, and so on). That works for `client_app` and `admin_app`.

A browser **cannot** open a raw TCP socket to port 9090 and speak that protocol. Fetch/XHR only speak HTTP. Shared memory (`/flight_admin_bus`) and `.db` files are also unreachable from HTML.

So the API exists to:

1. Let an **HTML frontend** call the same operations as the CLIs.
2. Keep **all business rules in C** — seats, mutexes, waitlist, auto-create, HMAC tokens, password hashes, persistence.
3. Avoid rewriting the server in another language just to get JSON.

Without a gateway, a web UI would have to reimplement booking, which would race the C server and could overbook seats.

---

## Why a gateway (not HTTP inside C)

Two approaches were considered:

| Approach | Idea | Tradeoff |
| -------- | ---- | -------- |
| **Gateway (what we built)** | New process accepts HTTP, maps each route to one TCP command, returns JSON | Two processes; C and CLIs stay untouched |
| HTTP in `server_app` | Parse HTTP/JSON in `server_protocol.c` | One binary, but a large C change and risk to the existing protocol |

The gateway was chosen because:

- The TCP protocol already covers every user and admin action.
- `handle_client` already **closes the socket after one reply**, which is easy to wrap (open, send line, read until close).
- Concurrency tests and CLIs keep working with zero C edits.
- Python’s stdlib can serve HTTP and JSON without extra packages.

A later option is still to teach C to speak the same `/api/v1` routes and retire this process. The JSON shapes should stay stable so a frontend would not need to change.

---

## How it was made

### Architecture

```text
Browser or curl
    |  HTTP JSON  (port 8080)
    v
web/api/gateway.py     ← routing, CORS, validation, status codes
web/api/protocol.py    ← TCP client + parse text replies into dicts
    |  one command line per request  (port 9090)
    v
server_app             ← unchanged C (auth, flights, bookings, waitlist, disk)
```

Each HTTP request is **one TCP connection**, same pattern as `client/client.c` (`request_server`): connect, write a line ending in `\n`, read until the server closes. The C server already does “one command per connection” in `server_main.c`.

There is **no session table** in the gateway. After signup/login, the C server issues the same HMAC token the CLI uses. The browser (or curl) sends it as `Authorization: Bearer <token>`. The gateway copies that token into the TCP command. Only C verifies the signature, expiry, and role.

### Source files

| File | Role |
| ---- | ---- |
| `gateway.py` | `ThreadingHTTPServer`, `/api/v1` routes, JSON envelope, CORS, Bearer extraction, input checks, light auth rate limit |
| `protocol.py` | `tcp_request()`, regex parsers for `LIST` / `DETAIL` / `MYBOOKINGS` / login notifications / admin lists |

Python 3 stdlib only (`http.server`, `socket`, `json`). No Flask/FastAPI dependency so the C-focused project stays easy to run.

### Request path (example: book seats)

1. Client `POST /api/v1/bookings` with JSON `{ source, destination, date, seats }` and a user token.
2. Gateway rejects usernames/routes/dates that contain spaces or newlines so they cannot break the line protocol.
3. ISO date `2026-05-01` is converted to `1 5 2026` for TCP.
4. Gateway sends `BOOK <token> BLR DEL 1 5 2026 2\n`.
5. C runs `book_seats_for_user` (locks, semaphore, waitlist).
6. Text reply is classified:
   - `OK F101-BK0001` → HTTP 201, `{ "status": "booked", "bookingId": "..." }`
   - `WAITLISTED ...` → HTTP 200, `{ "status": "waitlisted", "message": "..." }` (not treated as an error; the UI must show waitlist)
   - `ERR unauthorized` → 401
   - `ERR booking failed` → 409

The gateway never counts seats or writes `bookings.db`.

### Design choices

**JSON envelope.** Success is `{ "ok": true, "data": ... }`. Failure is `{ "ok": false, "error": { "code", "message" } }`. That is stable for a frontend even when the TCP wording changes slightly.

**`/api/v1` prefix.** Versioning so a later JSON change can live under `/v2` without breaking old pages.

**CORS.** HTML will often be served from another origin (or `file://`). The gateway answers `OPTIONS` and echoes `Origin`, with `Authorization` and `Content-Type` allowed. Tokens are Bearer headers, not cookies, so CSRF is not the main concern.

**Validation before TCP.** Fields interpolated into commands are restricted (`A-Za-z0-9._-` for ids/tokens, no whitespace in passwords). Admin feed messages cannot contain newlines. That is protocol-injection defense, not a replacement for C checks.

**Logout is local.** `POST /auth/logout` returns 204 and does not call C. Tokens are stateless; logout means the client drops the string (same as `client_logout()`).

**PATCH flight.** The C protocol has two commands. If the JSON body has both `date` and `price`, the gateway sends `ADMIN_CHANGE_TIMING` then `ADMIN_UPDATE_PRICE`.

**Rate limit.** Signup/login/admin-login are capped per client IP in memory (30/minute). C has no throttling; this only slows trivial brute force on the HTTP side.

**Dates.** JSON uses `YYYY-MM-DD`. TCP still uses day/month/year integers.

---

## What the API covers

Every CLI action is mapped. Base URL: `http://127.0.0.1:8080/api/v1`.

Bodies: `Content-Type: application/json`. After login: `Authorization: Bearer <token>`.

| Method | Path | TCP command |
| ------ | ---- | ----------- |
| `GET` | `/health` | `PING` |
| `POST` | `/auth/signup` | `SIGNUP` |
| `POST` | `/auth/login` | `LOGIN` |
| `POST` | `/auth/logout` | none |
| `GET` | `/flights?source=&destination=` | `LIST` |
| `GET` | `/flights/{flightNumber}` | `DETAIL` |
| `POST` | `/bookings` | `BOOK` |
| `GET` | `/bookings` | `MYBOOKINGS` |
| `DELETE` | `/bookings/{bookingId}` | `CANCEL` |
| `POST` | `/admin/auth/login` | `ADMIN_LOGIN` |
| `POST` | `/admin/admins` | `ADMIN_CREATE` |
| `GET` | `/admin/admins` | `ADMIN_LIST_ADMINS` |
| `POST` | `/admin/flights` | `ADMIN_CREATE_FLIGHT` |
| `GET` | `/admin/flights` | `ADMIN_LIST_FLIGHTS` |
| `PATCH` | `/admin/flights/{flightNumber}` | timing and/or price |
| `GET` | `/admin/feed` | `READADMIN` |
| `POST` | `/admin/feed` | `ADMINMSG` |

Waitlist **confirmations** still arrive on **login** (`notifications` in the login JSON), matching C. There is no extra notifications TCP command.

Typical TCP → HTTP mapping:

| TCP reply | HTTP |
| --------- | ---- |
| `ONLINE` | 200 health |
| `OK ...` | 200 or 201 |
| `WAITLISTED ...` | 200, `data.status: waitlisted` |
| `REQUESTED ...` | 409, `data.status: requested` |
| `ERR unauthorized` | 401 |
| `... not found` | 404 |
| other `ERR` / `ERROR` | 400 or 409 |
| cannot connect to `server_app` | 502 `upstream` |

---

## How to run

Start the C server first (it owns the databases and the admin feed):

```sh
./server_app
```

Then the gateway, from the project root:

```sh
python3 web/api/gateway.py
```

or `make api`.

| Variable | Default | Meaning |
| -------- | ------- | ------- |
| `FBS_HTTP_HOST` | `127.0.0.1` | HTTP bind address |
| `FBS_HTTP_PORT` | `8080` | HTTP port |
| `FBS_TCP_HOST` | `127.0.0.1` | `server_app` host |
| `FBS_TCP_PORT` | `9090` | `server_app` port |

Python 3.10+; no pip packages. Bind HTTP and TCP to localhost unless you put a TLS reverse proxy in front. Passwords still travel in plaintext on the **TCP** hop; treat that hop as internal.

### Examples

```sh
python3 -c "import urllib.request; print(urllib.request.urlopen('http://127.0.0.1:8080/api/v1/health').read().decode())"
```

```sh
python3 << 'PY'
import json, urllib.request
req = urllib.request.Request(
    "http://127.0.0.1:8080/api/v1/auth/signup",
    data=json.dumps({"username": "alice", "password": "secret"}).encode(),
    headers={"Content-Type": "application/json"},
    method="POST",
)
print(urllib.request.urlopen(req).read().decode())
PY
```

Admin login uses the same default as the C server after a clean start: `admin` / `admin123`.

---

## What this API does not do

- It does not replace `client_app` or `admin_app`.
- It does not open `flights.db` / `users.db` or POSIX shared memory.
- It does not implement waitlist, auto-create, or seat allocation.
- It does not add WebSockets or a seat-picker UI (the protocol has no per-seat user choice).
- It does not change HMAC token format; the token is an opaque string.

The HTML frontend (not included here) should store the token in `sessionStorage` and call these routes. See [`API_PLAN.md`](../../API_PLAN.md) for page-level frontend notes.
