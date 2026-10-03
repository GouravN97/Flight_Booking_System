# HTTP API Plan (for an HTML frontend)

The HTTP gateway described here is implemented in [`web/api/`](../web/api/README.md) (`python3 web/api/gateway.py`). It does not change the C server, CLI clients, or storage. The goal is a JSON HTTP API that a browser-based HTML/CSS/JS frontend can call, while the existing `server_app` remains the source of truth for bookings, waitlist, auth, and persistence.

Today the system is a **line-oriented TCP protocol** on port `9090`. Browsers cannot speak that protocol. An HTTP API is the missing layer between `server_app` and a web UI.

---

## 1. Goal

Expose every user and admin capability that `client_app` and `admin_app` already provide, as REST-ish JSON over HTTP, so that:

- A **user site** can signup, login, search flights, book, cancel, and see waitlist notices.
- An **admin site** can login, manage flights/admins, and read/write the admin feed.
- The C process keeps concurrency control (mutexes, seat semaphores), HMAC tokens, password hashing, waitlist fulfillment, auto-create, and `my_dbms` persistence.

The HTML frontend should never talk to sockets, shared memory, or `.db` files directly.

---

## 2. What already exists (do not reinvent)

| Layer | Role today |
| ----- | ---------- |
| `server_app` TCP `:9090` | One-line commands, text responses, per-connection thread |
| HMAC tokens | Stateless `role.subject.expiry.mac`, 24h TTL, `auth.secret` |
| Password hashing | Salted SHA-256 in `users.db` / `admins.db` |
| Booking / waitlist / auto-create | Unchanged business rules |
| Admin feed | POSIX shm `/flight_admin_bus` via `READADMIN` / `ADMINMSG` |

The API should be a **translation layer**, not a second booking engine.

---

## 3. Recommended architecture

**Phase 1 (recommended):** a small **HTTP gateway** that:

1. Accepts HTTP from the browser.
2. Maps each route to one existing TCP command.
3. Parses the text reply into JSON.
4. Returns JSON plus CORS headers.

```text
Browser (HTML/JS)
    |  HTTPS or HTTP
    v
HTTP API gateway   (new process, any language)
    |  localhost TCP, existing line protocol
    v
server_app :9090   (unchanged C)
    |
    +-- my_dbms files, auth.secret, admin shm
```

**Why this first:** no C protocol rewrite, CLIs keep working, frontend work can start as soon as JSON shapes are stable.

**Phase 2 (optional later):** teach `server_app` to speak HTTP/JSON on a second port (or replace TCP). Same routes as below. Only worth it if you want one binary and no gateway process.

**Do not** start by putting JSON parsers inside every `server_protocol.c` handler unless you already decided to drop the gateway.

Suggested gateway bind: `127.0.0.1:8080` (dev) with `server_app` still on `9090`. Production: reverse proxy (nginx/caddy) terminates TLS and forwards `/api` to the gateway.

---

## 4. Conventions for the HTTP API

### Base URL

```text
http://127.0.0.1:8080/api/v1
```

Version the prefix (`/v1`) so a later JSON shape change does not break an old frontend.

### Auth

Reuse the existing HMAC token. The browser stores it (see frontend section) and sends:

```http
Authorization: Bearer <token>
```

- User token (`u....`) for user routes.
- Admin token (`a....`) for admin routes.
- Gateway strips `Bearer `, then sends the token as the first argument of the TCP command (same as the CLI).

Do **not** put passwords in later requests. Do **not** invent a second session store in the gateway.

### Content type

- Request bodies: `application/json`
- Responses: `application/json`
- Dates in JSON: ISO `YYYY-MM-DD` (gateway converts to `day month year` for TCP)

### Envelope

Success:

```json
{
  "ok": true,
  "data": { }
}
```

Failure:

```json
{
  "ok": false,
  "error": {
    "code": "unauthorized",
    "message": "invalid or expired token"
  }
}
```

Map TCP prefixes roughly as:

| TCP reply | HTTP status | `error.code` (examples) |
| --------- | ----------- | ----------------------- |
| `OK ...` | 200 (or 201 for create) | — |
| `WAITLISTED ...` | 200 with `data.status: "waitlisted"` | not an error — user must see waitlist UI |
| `ERR unauthorized` / bad token | 401 | `unauthorized` |
| `ERR ...` validation / not found | 400 or 404 | `bad_request`, `not_found` |
| `REQUESTED ...` (legacy/unavailable) | 409 or 200 waitlisted, match current server | `conflict` |
| connection / parse failure | 502 | `upstream` |

`PING` → `ONLINE` maps to health `200`.

### CORS

The HTML files will be served from another origin (or `file://`). The gateway must send:

- `Access-Control-Allow-Origin: <frontend origin>` (not `*` if you use cookies)
- `Access-Control-Allow-Headers: Authorization, Content-Type`
- `Access-Control-Allow-Methods: GET, POST, PATCH, DELETE, OPTIONS`
- Handle `OPTIONS` preflight

### IDs and limits

Keep current limits (`MAX_SEATS_PER_FLIGHT` 25, booking IDs `F101-BK0001`, flight numbers, etc.). Document them in OpenAPI later; the frontend should not assume unlimited seats.

---

## 5. Endpoint map (TCP → HTTP)

Each row is one CLI action. Implement these and the HTML app can replace both CLIs.

### 5.1 Public / health

| Method | Path | TCP | Notes |
| ------ | ---- | --- | ----- |
| `GET` | `/api/v1/health` | `PING` | `{ "ok": true, "data": { "status": "online" } }` |

### 5.2 User auth

| Method | Path | TCP | Body / result |
| ------ | ---- | --- | ------------- |
| `POST` | `/api/v1/auth/signup` | `SIGNUP <user> <password>` | `{ "username", "password" }` → `{ "token", "role": "user", "notifications": [] }` |
| `POST` | `/api/v1/auth/login` | `LOGIN <user> <password>` | Same body. Parse `OK <token>` and optional `NOTIFICATIONS:` lines into `notifications[]`. |
| `POST` | `/api/v1/auth/logout` | none | Client-only: discard token. Gateway may return 204 with no TCP call. |

Passwords still travel in plaintext on TCP today. The **plan** for production is TLS on the HTTP side so the browser never talks to `:9090`.

### 5.3 User flights and bookings

All of these require `Authorization: Bearer <user-token>`.

| Method | Path | TCP |
| ------ | ---- | --- |
| `GET` | `/api/v1/flights?source=BLR&destination=DEL` | `LIST <token> BLR DEL` |
| `GET` | `/api/v1/flights/{flightNumber}` | `DETAIL <token> <flight_number>` |
| `POST` | `/api/v1/bookings` | `BOOK <token> <src> <dst> <d> <m> <y> <seats>` |
| `GET` | `/api/v1/bookings` | `MYBOOKINGS <token>` |
| `DELETE` | `/api/v1/bookings/{bookingId}` | `CANCEL <token> <booking_id>` |

**Suggested JSON**

`GET /flights?source=&destination=`

```json
{
  "ok": true,
  "data": {
    "flights": [
      {
        "flightNumber": "F101",
        "date": "2026-05-01",
        "seatsAvailable": 20,
        "seatsTotal": 20,
        "price": 4500.0
      }
    ]
  }
}
```

Gateway parses lines like `F101 date:01/05/2026 seats:20/20 price:4500.00`.

`POST /bookings`

```json
{
  "source": "BLR",
  "destination": "DEL",
  "date": "2026-05-01",
  "seats": 2
}
```

Success:

```json
{
  "ok": true,
  "data": {
    "status": "booked",
    "bookingId": "F101-BK0001"
  }
}
```

Waitlist (same HTTP 200 — this is a first-class outcome, not a silent error):

```json
{
  "ok": true,
  "data": {
    "status": "waitlisted",
    "message": "flight full; you will be booked when a new flight on this route is created"
  }
}
```

`GET /bookings` — array of `{ bookingId, flightNumber, seats, ... }` parsed from `MYBOOKINGS` text.

Waitlist **confirmations** stay on **login** (as today). Optional later: `GET /api/v1/notifications` if you add a TCP command; until then, show notices after `POST /auth/login`.

### 5.4 Admin auth and resources

`Authorization: Bearer <admin-token>` except login.

| Method | Path | TCP |
| ------ | ---- | --- |
| `POST` | `/api/v1/admin/auth/login` | `ADMIN_LOGIN <admin> <password>` |
| `POST` | `/api/v1/admin/admins` | `ADMIN_CREATE <token> <id> <pw>` |
| `GET` | `/api/v1/admin/admins` | `ADMIN_LIST_ADMINS <token>` |
| `POST` | `/api/v1/admin/flights` | `ADMIN_CREATE_FLIGHT ...` |
| `GET` | `/api/v1/admin/flights` | `ADMIN_LIST_FLIGHTS <token>` |
| `PATCH` | `/api/v1/admin/flights/{flightNumber}` | `ADMIN_CHANGE_TIMING` and/or `ADMIN_UPDATE_PRICE` |
| `GET` | `/api/v1/admin/feed` | `READADMIN <token>` |
| `POST` | `/api/v1/admin/feed` | `ADMINMSG <token> <message>` |

**Create flight body**

```json
{
  "flightNumber": "F101",
  "source": "BLR",
  "destination": "DEL",
  "date": "2026-05-01",
  "seats": 20,
  "price": 4500
}
```

**PATCH flight** — send only fields that change:

```json
{ "date": "2026-05-02" }
```

```json
{ "price": 5200 }
```

If both are present, gateway issues two TCP commands in order (timing then price), or reject mixed patches until the C protocol has a single update command.

**Feed**

```json
{
  "ok": true,
  "data": {
    "messages": [
      "Flight creation requested by user alice: BLR->DEL on 01/05/2026, ..."
    ]
  }
}
```

The feed is a 4096-byte shared-memory blob today. The gateway should split on newlines and return an array. Polling `GET /admin/feed` every few seconds is enough for a first admin UI (no WebSockets required).

---

## 6. Gateway implementation notes (still no C)

Pick one stack and stay with it. Reasonable choices:

| Option | Why |
| ------ | --- |
| Node (Express/Fastify) | Fast to prototype; same language as frontend tooling |
| Python (FastAPI) | OpenAPI generated from type hints |
| Go | Single static binary next to `server_app` |

Responsibilities of the gateway:

1. **Connection pooling or short-lived TCP** — each HTTP request opens (or borrows) a socket to `127.0.0.1:9090`, writes one command line, reads until the response is complete, closes or returns to pool. The C server already uses one thread per TCP connection; pooling reduces handshake cost.
2. **Response framing** — TCP replies are multi-line (`LIST`, `MYBOOKINGS`, `LOGIN` + notifications). Define “end of message” the same way the CLI does (read until the server closes, or a known terminator). If the server currently holds the connection open after a reply, the gateway must know how many bytes/lines to read (inspect `handle_client` when implementing). This is the riskiest part of the gateway; document it in a short “framing” comment in the gateway, not by changing C unless you must.
3. **Input validation** — reject empty usernames, non-integer seats, invalid dates, oversized strings **before** sending TCP, to avoid protocol injection (spaces in user IDs, newlines in admin messages).
4. **No business logic** — do not reimplement waitlist, auto-create, or seat math in the gateway.
5. **Logging** — log method, path, status, latency; never log passwords or full tokens.

Suggested repo layout (when you implement):

```text
web/
  api/                 # HTTP gateway
  frontend/            # static HTML/CSS/JS
docs/API_PLAN.md       # this file
```

Keep the Makefile as-is for C; add a separate `web/api` start script (for example `npm start` or `uvicorn`).

---

## 7. HTML frontend plan

Static files are enough for v1 (no React required). Serve them from the gateway (`/` → `web/frontend`) or any static server with CORS configured.

### Pages (user)

| Page | Uses |
| ---- | ---- |
| `index.html` | Login / signup forms → `POST /auth/login` or `/signup` |
| `search.html` | Source, destination → `GET /flights` |
| `flight.html` | `GET /flights/:id` |
| `book.html` | Date + seats → `POST /bookings`; show booked vs waitlisted |
| `bookings.html` | `GET /bookings`, cancel → `DELETE` |
| After login | Render `notifications[]` as a banner |

### Pages (admin)

| Page | Uses |
| ---- | ---- |
| `admin/login.html` | `POST /admin/auth/login` |
| `admin/flights.html` | list, create, patch date/price |
| `admin/admins.html` | list + create admin |
| `admin/feed.html` | poll `GET /admin/feed`, post messages |

Separate **user token** and **admin token** in `sessionStorage` (or two keys). Do not use one token for both UIs.

### Browser storage

- **Recommended for class project:** `sessionStorage` for `userToken` / `adminToken`.
- **Cookies:** only if you set `HttpOnly` from the gateway on login; then CSRF protection is required. Skip cookies until you need them.

### UX that must match C behavior

1. Booking a full/missing route is **waitlisted**, not a red error.
2. Waitlist booking appears on **next login**, not live (unless you add notifications later).
3. Auto-created flights have price `0` until an admin patches price.
4. Default admin `admin` / `admin123` only after a clean server start.
5. Tokens expire after 24 hours — frontend should send the user back to login on 401.

### No live seat map required

The protocol has no per-seat picker for users (seats are allocated by the server). The UI should ask for **seat count**, not seat numbers.

---

## 8. Security plan (HTTP layer)

The C protocol still sends passwords in the clear on the socket. The API plan should treat that as an **internal** hop:

1. Browser → gateway: HTTPS in any real deployment.
2. Gateway → `server_app`: bind server to `127.0.0.1` only so the line protocol is not on the LAN.
3. Gateway validates tokens only by forwarding them; the C server remains the authority.
4. Rate-limit `POST /auth/login` and `/signup` on the gateway (C has no throttling).
5. Never echo passwords in JSON error bodies.
6. Sanitize admin feed text in the HTML page (`textContent`, not `innerHTML`) — feed content is free-form.

Out of scope for v1 API, but note for later: TLS for TCP, password over hash on the wire, refresh tokens.

---

## 9. Phased delivery

### Phase A — Contract

- Freeze the route table in this file (or generate OpenAPI from it).
- Write example JSON for login, list, book (OK + WAITLISTED), my bookings, admin create flight, feed.

### Phase B — Gateway MVP (user only)

- Health, signup, login, list, detail, book, my bookings, cancel.
- Manual test with `curl` against a running `server_app`.
- CORS + Bearer header.

### Phase C — Static user frontend

- Login/signup, search, book, bookings list/cancel.
- Handle waitlisted and 401.

### Phase D — Admin API + admin UI

- Remaining admin routes.
- Feed poll every 2–5s.

### Phase E — Hardening (optional)

- TLS reverse proxy.
- OpenAPI (`swagger.json`) for the frontend.
- Integration tests: HTTP in → TCP out → JSON assert (reuse scenarios from `tests/TEST_CASES.md`: overbook must not happen; waitlist still counts as non-OK for seat races).
- Only if needed: C change to close the socket after each reply, or a length prefix, so the gateway can parse responses reliably.

### Phase F — Optional merge

- HTTP listener inside `server_app`, retire the gateway. Same `/api/v1` routes so the HTML app does not change.

---

## 10. curl examples (target behavior)

After gateway + `server_app` are running:

```sh
curl -s http://127.0.0.1:8080/api/v1/health

curl -s -X POST http://127.0.0.1:8080/api/v1/auth/signup \
  -H 'Content-Type: application/json' \
  -d '{"username":"alice","password":"secret"}'

# TOKEN from response
curl -s http://127.0.0.1:8080/api/v1/flights?source=BLR\&destination=DEL \
  -H "Authorization: Bearer $TOKEN"

curl -s -X POST http://127.0.0.1:8080/api/v1/bookings \
  -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"source":"BLR","destination":"DEL","date":"2026-05-01","seats":2}'
```

Admin:

```sh
curl -s -X POST http://127.0.0.1:8080/api/v1/admin/auth/login \
  -H 'Content-Type: application/json' \
  -d '{"username":"admin","password":"admin123"}'
```

---

## 11. What this plan explicitly does not do

- Rewrite booking, waitlist, or storage in JavaScript.
- Expose `my_dbms` or `.db` files to the browser.
- Replace `client_app` / `admin_app` (they stay for CLI and concurrency tests).
- Add WebSockets or a seat-selection map unless a new C command exists.
- Change HMAC token format (the frontend treats the token as an opaque string).

---

## 12. Success criteria

The API + HTML work is done when:

1. A user can complete signup → search → book → view bookings → cancel in the browser against the **same** `server_app` instance the CLIs use.
2. A full flight returns a waitlist state in the UI, and a later admin-created flight still fulfills waitlist as today (notice on next login).
3. An admin can create a flight and see the feed without using `admin_app`.
4. Concurrent booking safety remains in C; the gateway does not add a second seat counter.

When you are ready to implement, start with Phase B (gateway + `curl`), then Phase C (HTML). Keep this document as the contract; update JSON examples here if the TCP text format changes.
