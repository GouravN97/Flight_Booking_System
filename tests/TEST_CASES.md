# Concurrent Booking Test Suite

This suite verifies that the server handles **simultaneous** booking requests correctly:
no overbooking, correct rejections when capacity is exceeded, independent per-flight
booking namespaces, and parallel progress across different flights.

Run everything from the project root:

```bash
make test-concurrency
# or
./tests/run_concurrency_tests.sh
```

The runner starts `server_app` in a temporary directory (fresh `.db` files), launches
`concurrent_booking_test`, then shuts the server down.

Setup commands still use the documented `ADMIN_CREATE_FLIGHT admin admin123 ...` and
`BOOK userNN ...` forms. The test harness logs in first, caches HMAC tokens, and rewrites
those commands to the token protocol before sending.

Each test uses a **pthread barrier** so all client threads send their `BOOK` command at
the same instant. Responses are classified as:

| Response prefix | Meaning |
| --------------- | ------- |
| `OK ` | Booking succeeded (`OK <flight>-BK####`) |
| `ERR booking failed` | Semaphore/seat allocation lost the race |
| `REQUESTED flight unavailable; ...` | Waitlist enqueue failed; admin creation request sent |
| `WAITLISTED flight full; ...` | Flight full (or no capacity); user queued for the next matching route |

Under heavy contention, losers may receive **`ERR`**, **`REQUESTED`**, or **`WAITLISTED`**. The test harness counts all three as rejections. The critical invariant is the **post-condition seat count** (no overbooking).

---

## Test 01 — Single-seat race (exact capacity)

**Goal:** 10 concurrent 1-seat requests on a 5-seat flight → exactly 5 winners, 5 losers, 0 remaining seats.

**Setup**

```text
ADMIN_CREATE_FLIGHT admin admin123 CF101 BLR DEL 15 8 2026 5 1000.00
SIGNUP user01..user10
```

**Concurrent inputs (10 threads, barrier-synced)**

```text
BOOK user01 BLR DEL 15 8 2026 1
BOOK user02 BLR DEL 15 8 2026 1
...
BOOK user10 BLR DEL 15 8 2026 1
```

**Expected concurrent outputs**

| Count | Pattern |
| ----- | ------- |
| 5 | `OK CF101-BK0001` … `OK CF101-BK0005` (any order, all unique) |
| 5 | `ERR booking failed`, `WAITLISTED ...`, and/or `REQUESTED flight unavailable; ...` |

**Post-condition verify**

```text
DETAIL CF101
→ CF101 BLR->DEL date:15/08/2026 seats:0/5 price:1000.00
```

---

## Test 02 — Parallel bookings on different flights

**Goal:** Bookings on two flights at the same time should not interfere; all succeed.

**Setup**

```text
ADMIN_CREATE_FLIGHT admin admin123 CF102 BLR MUM 16 8 2026 5 2000.00
ADMIN_CREATE_FLIGHT admin admin123 CF103 BLR HYD 16 8 2026 5 3000.00
SIGNUP user11..user20
```

**Concurrent inputs**

```text
BOOK user11 BLR MUM 16 8 2026 1   ┐
BOOK user12 BLR MUM 16 8 2026 1   │ CF102 (5 clients)
BOOK user13 BLR MUM 16 8 2026 1   │
BOOK user14 BLR MUM 16 8 2026 1   │
BOOK user15 BLR MUM 16 8 2026 1   ┘
BOOK user16 BLR HYD 16 8 2026 1   ┐
BOOK user17 BLR HYD 16 8 2026 1   │ CF103 (5 clients)
BOOK user18 BLR HYD 16 8 2026 1   │
BOOK user19 BLR HYD 16 8 2026 1   │
BOOK user20 BLR HYD 16 8 2026 1   ┘
```

**Expected concurrent outputs**

| Count | Pattern |
| ----- | ------- |
| 10 | `OK <flight>-BK0001` (5 per flight) |
| 0 | `ERR booking failed` |

**Post-condition verify**

```text
DETAIL CF102 → seats:0/5
DETAIL CF103 → seats:0/5
```

---

## Test 03 — Multi-seat race fills flight exactly

**Goal:** Five clients each book 2 seats on a 10-seat flight concurrently → all 5 succeed, flight full.

**Setup**

```text
ADMIN_CREATE_FLIGHT admin admin123 CF104 BLR DEL 17 8 2026 10 1500.00
SIGNUP user01..user05
```

**Concurrent inputs**

```text
BOOK user01 BLR DEL 17 8 2026 2
BOOK user02 BLR DEL 17 8 2026 2
BOOK user03 BLR DEL 17 8 2026 2
BOOK user04 BLR DEL 17 8 2026 2
BOOK user05 BLR DEL 17 8 2026 2
```

**Expected concurrent outputs**

| Count | Pattern |
| ----- | ------- |
| 5 | `OK CF104-BK####` (unique IDs) |
| 0 | `ERR booking failed` |

**Post-condition verify**

```text
DETAIL CF104 → seats:0/10
```

---

## Test 04 — Multi-seat race with rejections

**Goal:** Ten clients each request 2 seats (20 total) on a 10-seat flight → 5 OK, 5 ERR.

**Setup**

```text
ADMIN_CREATE_FLIGHT admin admin123 CF105 BLR DEL 18 8 2026 10 1500.00
SIGNUP user06..user15
```

**Concurrent inputs**

```text
BOOK user06..user15 BLR DEL 18 8 2026 2   (10 threads)
```

**Expected concurrent outputs**

| Count | Pattern |
| ----- | ------- |
| 5 | `OK CF105-BK####` |
| 5 | `ERR booking failed` and/or `REQUESTED ...` |

**Post-condition verify**

```text
DETAIL CF105 → seats:0/10
```

---

## Test 05 — Last single seat, many contenders

**Goal:** Eight clients race for one seat → 1 OK, 7 ERR.

**Setup**

```text
ADMIN_CREATE_FLIGHT admin admin123 CF106 BLR DEL 19 8 2026 1 500.00
SIGNUP user01..user08
```

**Concurrent inputs**

```text
BOOK user01..user08 BLR DEL 19 8 2026 1
```

**Expected concurrent outputs**

| Count | Pattern |
| ----- | ------- |
| 1 | `OK CF106-BK0001` |
| 7 | `ERR booking failed` and/or `REQUESTED ...` |

**Post-condition verify**

```text
DETAIL CF106 → seats:0/1
```

---

## Test 06 — Per-flight booking ID namespaces

**Goal:** Two flights booked at the same time each assign `BK0001` independently.

**Setup**

```text
ADMIN_CREATE_FLIGHT admin admin123 CF107 BLR DEL 20 8 2026 3 900.00
ADMIN_CREATE_FLIGHT admin admin123 CF108 BLR MUM 20 8 2026 3 900.00
SIGNUP user01 user02
```

**Concurrent inputs**

```text
BOOK user01 BLR DEL 20 8 2026 1
BOOK user02 BLR MUM 20 8 2026 1
```

**Expected concurrent outputs**

| Response |
| -------- |
| `OK CF107-BK0001` |
| `OK CF108-BK0001` |

**Post-condition verify**

```text
DETAIL CF107 → seats:2/3
DETAIL CF108 → seats:2/3
```

---

## Test 07 — High contention (max seats per flight)

**Goal:** 50 concurrent 1-seat requests on a 25-seat flight → exactly 25 OK and 25 ERR.

**Setup**

```text
ADMIN_CREATE_FLIGHT admin admin123 CF109 BLR DEL 21 8 2026 25 750.00
SIGNUP user01..user20
```

**Concurrent inputs**

50 × `BOOK userXX BLR DEL 21 8 2026 1` (users cycle user01..user20)

**Expected concurrent outputs**

| Count | Pattern |
| ----- | ------- |
| 25 | `OK CF109-BK####` |
| 25 | `ERR booking failed` and/or `REQUESTED ...` |

**Post-condition verify**

```text
DETAIL CF109 → seats:0/25
```

---

## What passing proves

1. **No overbooking** — final `seats:0/N` matches the number of successful bookings.
2. **Correct rejection under contention** — excess concurrent requests get `ERR booking failed`.
3. **Cross-flight parallelism** — Test 02 shows different flights do not serialise each other incorrectly.
4. **Per-flight booking storage** — Test 06 shows independent `BK0001` IDs per flight namespace.
5. **Semaphore + mutex safety at scale** — Test 07 stress-tests 50 simultaneous threads on one flight.

## Manual spot-check (optional)

After `make test-concurrency`, you can replay a single case with `nc`:

```bash
# Terminal 1: ./server_app 19090
# Terminal 2:
printf 'ADMIN_CREATE_FLIGHT admin admin123 CF101 BLR DEL 15 8 2026 5 1000\n' | nc 127.0.0.1 19090
printf 'SIGNUP user01 pass01\n' | nc 127.0.0.1 19090
printf 'BOOK user01 BLR DEL 15 8 2026 1\n' | nc 127.0.0.1 19090
# → OK CF101-BK0001
```
