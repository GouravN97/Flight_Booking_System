#!/usr/bin/env python3
"""HTTP/JSON gateway in front of server_app (TCP line protocol on port 9090)."""

from __future__ import annotations

import json
import os
import re
import sys
import time
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from collections import defaultdict, deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any
from urllib.parse import parse_qs, unquote, urlparse

from protocol import (
    UpstreamError,
    err_message,
    parse_admin_flights,
    parse_admins,
    parse_auth,
    parse_bookings,
    parse_detail,
    parse_feed,
    parse_iso_date,
    parse_list_flights,
    tcp_request,
    tcp_status,
)

TCP_HOST = os.environ.get("FBS_TCP_HOST", "127.0.0.1")
TCP_PORT = int(os.environ.get("FBS_TCP_PORT", "9090"))
HTTP_HOST = os.environ.get("FBS_HTTP_HOST", "127.0.0.1")
HTTP_PORT = int(os.environ.get("FBS_HTTP_PORT", "8080"))

TOKEN_RE = re.compile(r"^[A-Za-z0-9._-]{8,191}$")
ID_RE = re.compile(r"^[A-Za-z0-9._-]{1,63}$")
FLIGHT_RE = re.compile(r"^[A-Za-z0-9._-]{1,23}$")
BOOKING_RE = re.compile(r"^[A-Za-z0-9._-]{1,63}$")
ROUTE_RE = re.compile(r"^[A-Za-z0-9]{1,63}$")

AUTH_WINDOW_SEC = 60.0
AUTH_MAX_PER_WINDOW = 30
_auth_hits: dict[str, deque[float]] = defaultdict(deque)


def _ok(data: Any) -> tuple[int, dict[str, Any]]:
    return 200, {"ok": True, "data": data}


def _created(data: Any) -> tuple[int, dict[str, Any]]:
    return 201, {"ok": True, "data": data}


def _err(status: int, code: str, message: str) -> tuple[int, dict[str, Any]]:
    return status, {"ok": False, "error": {"code": code, "message": message}}


def _map_tcp_error(text: str) -> tuple[int, dict[str, Any]]:
    status = tcp_status(text)
    msg = err_message(text)
    if status == "UNAUTHORIZED":
        return _err(401, "unauthorized", "invalid or expired token")
    if status == "NOT_FOUND":
        return _err(404, "not_found", msg)
    lower = msg.lower()
    if "exists" in lower or "limit reached" in lower:
        return _err(409, "conflict", msg)
    if "failed" in lower and "booking" in lower:
        return _err(409, "conflict", msg)
    return _err(400, "bad_request", msg)


def _safe_id(value: str, pattern: re.Pattern[str], label: str) -> str:
    if not isinstance(value, str) or not pattern.match(value):
        raise ValueError(f"invalid {label}")
    return value


def _json_body(handler: BaseHTTPRequestHandler) -> dict[str, Any]:
    length = int(handler.headers.get("Content-Length") or "0")
    if length <= 0:
        raise ValueError("JSON body required")
    if length > 65536:
        raise ValueError("request body too large")
    raw = handler.rfile.read(length)
    try:
        data = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ValueError("invalid JSON") from exc
    if not isinstance(data, dict):
        raise ValueError("JSON object required")
    return data


def _bearer(handler: BaseHTTPRequestHandler) -> str:
    header = handler.headers.get("Authorization") or ""
    if not header.startswith("Bearer "):
        raise PermissionError("missing bearer token")
    token = header[7:].strip()
    if not TOKEN_RE.match(token):
        raise PermissionError("invalid or expired token")
    return token


def _rate_limit_auth(client: str) -> bool:
    now = time.monotonic()
    q = _auth_hits[client]
    while q and now - q[0] > AUTH_WINDOW_SEC:
        q.popleft()
    if len(q) >= AUTH_MAX_PER_WINDOW:
        return False
    q.append(now)
    return True


def _seats(value: Any, *, max_seats: int = 25) -> int:
    seats = int(value)
    if seats < 1 or seats > max_seats:
        raise ValueError(f"seats must be between 1 and {max_seats}")
    return seats


def _price(value: Any) -> float:
    price = float(value)
    if price < 0:
        raise ValueError("price must be non-negative")
    return price


def _tcp(command: str) -> str:
    return tcp_request(TCP_HOST, TCP_PORT, command)


def handle_health(_: BaseHTTPRequestHandler, __: dict[str, list[str]], ___: str) -> tuple[int, dict[str, Any]]:
    text = _tcp("PING")
    if tcp_status(text) != "ONLINE":
        return _err(502, "upstream", "flight server is not online")
    return _ok({"status": "online"})


def handle_signup(handler: BaseHTTPRequestHandler, __: dict[str, list[str]], ___: str) -> tuple[int, dict[str, Any]]:
    if not _rate_limit_auth(handler.client_address[0]):
        return _err(429, "rate_limited", "too many auth attempts")
    body = _json_body(handler)
    user = _safe_id(str(body.get("username", "")), ID_RE, "username")
    password = str(body.get("password", ""))
    if not password or len(password) > 63 or any(c.isspace() for c in password):
        raise ValueError("invalid password")
    text = _tcp(f"SIGNUP {user} {password}")
    if tcp_status(text) != "OK":
        return _map_tcp_error(text)
    return _created(parse_auth(text, "user"))


def handle_login(handler: BaseHTTPRequestHandler, __: dict[str, list[str]], ___: str) -> tuple[int, dict[str, Any]]:
    if not _rate_limit_auth(handler.client_address[0]):
        return _err(429, "rate_limited", "too many auth attempts")
    body = _json_body(handler)
    user = _safe_id(str(body.get("username", "")), ID_RE, "username")
    password = str(body.get("password", ""))
    if not password or len(password) > 63 or any(c.isspace() for c in password):
        raise ValueError("invalid password")
    text = _tcp(f"LOGIN {user} {password}")
    if tcp_status(text) != "OK":
        mapped = _map_tcp_error(text)
        if mapped[1]["error"]["code"] == "bad_request" and "credential" in mapped[1]["error"]["message"]:
            return _err(401, "unauthorized", mapped[1]["error"]["message"])
        return mapped
    return _ok(parse_auth(text, "user"))


def handle_logout(_: BaseHTTPRequestHandler, __: dict[str, list[str]], ___: str) -> tuple[int, dict[str, Any] | None]:
    return 204, None


def handle_list_flights(handler: BaseHTTPRequestHandler, query: dict[str, list[str]], __: str) -> tuple[int, dict[str, Any]]:
    token = _bearer(handler)
    source = (query.get("source") or [""])[0]
    dest = (query.get("destination") or [""])[0]
    source = _safe_id(source, ROUTE_RE, "source")
    dest = _safe_id(dest, ROUTE_RE, "destination")
    text = _tcp(f"LIST {token} {source} {dest}")
    if tcp_status(text) == "UNAUTHORIZED":
        return _map_tcp_error(text)
    if tcp_status(text) == "ERR":
        return _map_tcp_error(text)
    return _ok({"flights": parse_list_flights(text)})


def handle_flight_detail(handler: BaseHTTPRequestHandler, __: dict[str, list[str]], flight: str) -> tuple[int, dict[str, Any]]:
    token = _bearer(handler)
    flight = _safe_id(unquote(flight), FLIGHT_RE, "flightNumber")
    text = _tcp(f"DETAIL {token} {flight}")
    status = tcp_status(text)
    if status in ("UNAUTHORIZED", "NOT_FOUND", "ERR"):
        return _map_tcp_error(text)
    return _ok(parse_detail(text))


def handle_create_booking(handler: BaseHTTPRequestHandler, __: dict[str, list[str]], ___: str) -> tuple[int, dict[str, Any]]:
    token = _bearer(handler)
    body = _json_body(handler)
    source = _safe_id(str(body.get("source", "")), ROUTE_RE, "source")
    dest = _safe_id(str(body.get("destination", "")), ROUTE_RE, "destination")
    day, month, year = parse_iso_date(str(body.get("date", "")))
    seats = _seats(body.get("seats"))
    text = _tcp(f"BOOK {token} {source} {dest} {day} {month} {year} {seats}")
    status = tcp_status(text)
    if status == "WAITLISTED":
        line = text.strip().split("\n", 1)[0]
        message = line[len("WAITLISTED") :].strip() or "waitlisted"
        return _ok({"status": "waitlisted", "message": message})
    if status == "REQUESTED":
        line = text.strip().split("\n", 1)[0]
        message = line[len("REQUESTED") :].strip() or "admin notified"
        return 409, {"ok": True, "data": {"status": "requested", "message": message}}
    if status != "OK":
        return _map_tcp_error(text)
    booking_id = text.strip().split()[1] if len(text.strip().split()) > 1 else ""
    return _created({"status": "booked", "bookingId": booking_id})


def handle_my_bookings(handler: BaseHTTPRequestHandler, __: dict[str, list[str]], ___: str) -> tuple[int, dict[str, Any]]:
    token = _bearer(handler)
    text = _tcp(f"MYBOOKINGS {token}")
    if tcp_status(text) in ("UNAUTHORIZED", "ERR"):
        return _map_tcp_error(text)
    return _ok({"bookings": parse_bookings(text)})


def handle_cancel(handler: BaseHTTPRequestHandler, __: dict[str, list[str]], booking_id: str) -> tuple[int, dict[str, Any]]:
    token = _bearer(handler)
    booking_id = _safe_id(unquote(booking_id), BOOKING_RE, "bookingId")
    text = _tcp(f"CANCEL {token} {booking_id}")
    status = tcp_status(text)
    if status != "OK":
        return _map_tcp_error(text)
    return _ok({"cancelled": True, "bookingId": booking_id})


def handle_admin_login(handler: BaseHTTPRequestHandler, __: dict[str, list[str]], ___: str) -> tuple[int, dict[str, Any]]:
    if not _rate_limit_auth(handler.client_address[0]):
        return _err(429, "rate_limited", "too many auth attempts")
    body = _json_body(handler)
    user = _safe_id(str(body.get("username", "")), ID_RE, "username")
    password = str(body.get("password", ""))
    if not password or len(password) > 63 or any(c.isspace() for c in password):
        raise ValueError("invalid password")
    text = _tcp(f"ADMIN_LOGIN {user} {password}")
    if tcp_status(text) != "OK":
        mapped = _map_tcp_error(text)
        return _err(401, "unauthorized", mapped[1]["error"]["message"])
    return _ok(parse_auth(text, "admin"))


def handle_admin_create(handler: BaseHTTPRequestHandler, __: dict[str, list[str]], ___: str) -> tuple[int, dict[str, Any]]:
    token = _bearer(handler)
    body = _json_body(handler)
    new_id = _safe_id(str(body.get("username", "")), ID_RE, "username")
    password = str(body.get("password", ""))
    if not password or len(password) > 63 or any(c.isspace() for c in password):
        raise ValueError("invalid password")
    text = _tcp(f"ADMIN_CREATE {token} {new_id} {password}")
    if tcp_status(text) != "OK":
        return _map_tcp_error(text)
    return _created({"username": new_id})


def handle_admin_list(handler: BaseHTTPRequestHandler, __: dict[str, list[str]], ___: str) -> tuple[int, dict[str, Any]]:
    token = _bearer(handler)
    text = _tcp(f"ADMIN_LIST_ADMINS {token}")
    if tcp_status(text) in ("UNAUTHORIZED", "ERR"):
        return _map_tcp_error(text)
    return _ok({"admins": parse_admins(text)})


def handle_admin_create_flight(handler: BaseHTTPRequestHandler, __: dict[str, list[str]], ___: str) -> tuple[int, dict[str, Any]]:
    token = _bearer(handler)
    body = _json_body(handler)
    flight = _safe_id(str(body.get("flightNumber", "")), FLIGHT_RE, "flightNumber")
    source = _safe_id(str(body.get("source", "")), ROUTE_RE, "source")
    dest = _safe_id(str(body.get("destination", "")), ROUTE_RE, "destination")
    day, month, year = parse_iso_date(str(body.get("date", "")))
    seats = _seats(body.get("seats"))
    price = _price(body.get("price", 0))
    text = _tcp(
        f"ADMIN_CREATE_FLIGHT {token} {flight} {source} {dest} {day} {month} {year} {seats} {price}"
    )
    if tcp_status(text) != "OK":
        return _map_tcp_error(text)
    return _created(
        {
            "flightNumber": flight,
            "source": source,
            "destination": dest,
            "date": f"{year:04d}-{month:02d}-{day:02d}",
            "seats": seats,
            "price": price,
        }
    )


def handle_admin_list_flights(handler: BaseHTTPRequestHandler, __: dict[str, list[str]], ___: str) -> tuple[int, dict[str, Any]]:
    token = _bearer(handler)
    text = _tcp(f"ADMIN_LIST_FLIGHTS {token}")
    if tcp_status(text) in ("UNAUTHORIZED", "ERR"):
        return _map_tcp_error(text)
    return _ok({"flights": parse_admin_flights(text)})


def handle_admin_patch_flight(handler: BaseHTTPRequestHandler, __: dict[str, list[str]], flight: str) -> tuple[int, dict[str, Any]]:
    token = _bearer(handler)
    flight = _safe_id(unquote(flight), FLIGHT_RE, "flightNumber")
    body = _json_body(handler)
    has_date = "date" in body
    has_price = "price" in body
    if not has_date and not has_price:
        raise ValueError("provide date and/or price")
    updated: dict[str, Any] = {"flightNumber": flight}
    if has_date:
        day, month, year = parse_iso_date(str(body.get("date", "")))
        text = _tcp(f"ADMIN_CHANGE_TIMING {token} {flight} {day} {month} {year}")
        if tcp_status(text) != "OK":
            return _map_tcp_error(text)
        updated["date"] = f"{year:04d}-{month:02d}-{day:02d}"
    if has_price:
        price = _price(body.get("price"))
        text = _tcp(f"ADMIN_UPDATE_PRICE {token} {flight} {price}")
        if tcp_status(text) != "OK":
            return _map_tcp_error(text)
        updated["price"] = price
    return _ok(updated)


def handle_admin_get_feed(handler: BaseHTTPRequestHandler, __: dict[str, list[str]], ___: str) -> tuple[int, dict[str, Any]]:
    token = _bearer(handler)
    text = _tcp(f"READADMIN {token}")
    if tcp_status(text) in ("UNAUTHORIZED", "ERR"):
        return _map_tcp_error(text)
    return _ok({"messages": parse_feed(text)})


def handle_admin_post_feed(handler: BaseHTTPRequestHandler, __: dict[str, list[str]], ___: str) -> tuple[int, dict[str, Any]]:
    token = _bearer(handler)
    body = _json_body(handler)
    message = str(body.get("message", "")).strip()
    if not message or len(message) > 500:
        raise ValueError("invalid message")
    if "\n" in message or "\r" in message:
        raise ValueError("message cannot contain newlines")
    text = _tcp(f"ADMINMSG {token} {message}")
    if tcp_status(text) != "OK":
        return _map_tcp_error(text)
    return _ok({"published": True})


# (method, path regex) -> handler; groups after the API prefix become the extra path arg.
ROUTES: list[tuple[str, re.Pattern[str], Any]] = [
    ("GET", re.compile(r"^/health$"), handle_health),
    ("POST", re.compile(r"^/auth/signup$"), handle_signup),
    ("POST", re.compile(r"^/auth/login$"), handle_login),
    ("POST", re.compile(r"^/auth/logout$"), handle_logout),
    ("GET", re.compile(r"^/flights$"), handle_list_flights),
    ("GET", re.compile(r"^/flights/([^/]+)$"), handle_flight_detail),
    ("POST", re.compile(r"^/bookings$"), handle_create_booking),
    ("GET", re.compile(r"^/bookings$"), handle_my_bookings),
    ("DELETE", re.compile(r"^/bookings/([^/]+)$"), handle_cancel),
    ("POST", re.compile(r"^/admin/auth/login$"), handle_admin_login),
    ("POST", re.compile(r"^/admin/admins$"), handle_admin_create),
    ("GET", re.compile(r"^/admin/admins$"), handle_admin_list),
    ("POST", re.compile(r"^/admin/flights$"), handle_admin_create_flight),
    ("GET", re.compile(r"^/admin/flights$"), handle_admin_list_flights),
    ("PATCH", re.compile(r"^/admin/flights/([^/]+)$"), handle_admin_patch_flight),
    ("GET", re.compile(r"^/admin/feed$"), handle_admin_get_feed),
    ("POST", re.compile(r"^/admin/feed$"), handle_admin_post_feed),
]


class GatewayHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "FlightBookingGateway/1.0"

    def log_message(self, fmt: str, *args: Any) -> None:
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    def _cors_headers(self) -> None:
        origin = self.headers.get("Origin") or "*"
        self.send_header("Access-Control-Allow-Origin", origin)
        self.send_header("Access-Control-Allow-Headers", "Authorization, Content-Type")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, PATCH, DELETE, OPTIONS")
        self.send_header("Access-Control-Max-Age", "600")
        vary = "Origin" if origin != "*" else None
        if vary:
            self.send_header("Vary", "Origin")

    def _send(self, status: int, payload: dict[str, Any] | None) -> None:
        body = b"" if payload is None else json.dumps(payload, separators=(",", ":")).encode("utf-8")
        self.send_response(status)
        self._cors_headers()
        if payload is None:
            self.send_header("Content-Length", "0")
        else:
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        if body and self.command != "HEAD":
            self.wfile.write(body)

    def do_OPTIONS(self) -> None:  # noqa: N802
        self.send_response(204)
        self._cors_headers()
        self.send_header("Content-Length", "0")
        self.end_headers()

    def _dispatch(self) -> None:
        parsed = urlparse(self.path)
        path = parsed.path
        if not path.startswith("/api/v1"):
            self._send(*_err(404, "not_found", "unknown path"))
            return
        rest = path[len("/api/v1") :] or "/"
        query = parse_qs(parsed.query)
        for method, pattern, fn in ROUTES:
            if method != self.command:
                continue
            match = pattern.match(rest)
            if not match:
                continue
            extra = match.group(1) if match.lastindex else ""
            try:
                status, payload = fn(self, query, extra)
                self._send(status, payload)
            except PermissionError as exc:
                self._send(*_err(401, "unauthorized", str(exc)))
            except ValueError as exc:
                self._send(*_err(400, "bad_request", str(exc)))
            except UpstreamError as exc:
                self._send(*_err(502, "upstream", str(exc)))
            except Exception:
                traceback.print_exc()
                self._send(*_err(500, "internal", "gateway error"))
            return
        self._send(*_err(404, "not_found", "unknown path"))

    def do_GET(self) -> None:  # noqa: N802
        self._dispatch()

    def do_POST(self) -> None:  # noqa: N802
        self._dispatch()

    def do_PATCH(self) -> None:  # noqa: N802
        self._dispatch()

    def do_DELETE(self) -> None:  # noqa: N802
        self._dispatch()


def main() -> None:
    httpd = ThreadingHTTPServer((HTTP_HOST, HTTP_PORT), GatewayHandler)
    print(f"Flight booking HTTP API on http://{HTTP_HOST}:{HTTP_PORT}/api/v1", flush=True)
    print(f"Proxying TCP {TCP_HOST}:{TCP_PORT} (start ./server_app first)", flush=True)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping gateway")
        httpd.server_close()


if __name__ == "__main__":
    main()
