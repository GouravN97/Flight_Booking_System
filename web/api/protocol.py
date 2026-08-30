"""TCP line-protocol client and text-to-JSON parsers for server_app."""

from __future__ import annotations

import re
import socket
from typing import Any

LIST_LINE = re.compile(
    r"^(\S+) date:(\d{2})/(\d{2})/(\d{4}) seats:(\d+)/(\d+) price:([0-9.]+)$"
)
DETAIL_LINE = re.compile(
    r"^(\S+) (\S+)->(\S+) date:(\d{2})/(\d{2})/(\d{4}) seats:(\d+)/(\d+) price:([0-9.]+)$"
)
BOOKING_LINE = re.compile(
    r"^(\S+) flight:(\S+) (\S+)->(\S+) date:(\d{2})/(\d{2})/(\d{4}) seats:(\d+)$"
)


class UpstreamError(Exception):
    def __init__(self, message: str = "cannot reach flight server") -> None:
        super().__init__(message)


def iso_date(day: int, month: int, year: int) -> str:
    return f"{year:04d}-{month:02d}-{day:02d}"


def parse_iso_date(value: str) -> tuple[int, int, int]:
    """Return (day, month, year) from YYYY-MM-DD."""
    parts = value.strip().split("-")
    if len(parts) != 3:
        raise ValueError("date must be YYYY-MM-DD")
    year, month, day = int(parts[0]), int(parts[1]), int(parts[2])
    if not (1 <= month <= 12 and 1 <= day <= 31 and 1970 <= year <= 2100):
        raise ValueError("date out of range")
    return day, month, year


def tcp_request(host: str, port: int, command: str, timeout: float = 10.0) -> str:
    """Send one command; read until the C server closes the connection."""
    if not command.endswith("\n"):
        command = command + "\n"
    try:
        with socket.create_connection((host, port), timeout=timeout) as sock:
            sock.settimeout(timeout)
            sock.sendall(command.encode("utf-8"))
            chunks: list[bytes] = []
            while True:
                data = sock.recv(4096)
                if not data:
                    break
                chunks.append(data)
    except OSError as exc:
        raise UpstreamError() from exc
    if not chunks:
        raise UpstreamError("empty reply from flight server")
    return b"".join(chunks).decode("utf-8", errors="replace")


def first_line(text: str) -> str:
    return text.split("\n", 1)[0].strip()


def tcp_status(text: str) -> str:
    line = first_line(text)
    if line.startswith("OK"):
        return "OK"
    if line.startswith("WAITLISTED"):
        return "WAITLISTED"
    if line.startswith("REQUESTED"):
        return "REQUESTED"
    if line.startswith("ONLINE"):
        return "ONLINE"
    if line.startswith("ERR unauthorized") or line == "ERR unauthorized":
        return "UNAUTHORIZED"
    if "not found" in line.lower():
        return "NOT_FOUND"
    if line.startswith("ERR") or line.startswith("ERROR"):
        return "ERR"
    if line.startswith("UNSUPPORTED"):
        return "ERR"
    return "OK"


def err_message(text: str) -> str:
    line = first_line(text)
    if line.startswith("ERR "):
        return line[4:].strip()
    if line.startswith("ERROR:"):
        return line[6:].strip()
    return line or "request failed"


def parse_auth(text: str, role: str) -> dict[str, Any]:
    lines = text.replace("\r\n", "\n").split("\n")
    first = lines[0].strip()
    if not first.startswith("OK "):
        raise ValueError(err_message(text))
    token = first[3:].strip().split()[0]
    notifications: list[str] = []
    rest = "\n".join(lines[1:]).strip()
    if rest.startswith("NOTIFICATIONS:"):
        body = rest[len("NOTIFICATIONS:") :].lstrip("\n")
        notifications = [ln.strip() for ln in body.split("\n") if ln.strip()]
    elif rest:
        notifications = [ln.strip() for ln in rest.split("\n") if ln.strip()]
    return {"token": token, "role": role, "notifications": notifications}


def parse_list_flights(text: str) -> list[dict[str, Any]]:
    flights: list[dict[str, Any]] = []
    for raw in text.replace("\r\n", "\n").split("\n"):
        line = raw.strip()
        if not line or line.lower().startswith("flights:"):
            continue
        match = LIST_LINE.match(line)
        if not match:
            continue
        day, month, year = int(match.group(2)), int(match.group(3)), int(match.group(4))
        flights.append(
            {
                "flightNumber": match.group(1),
                "date": iso_date(day, month, year),
                "seatsAvailable": int(match.group(5)),
                "seatsTotal": int(match.group(6)),
                "price": float(match.group(7)),
            }
        )
    return flights


def parse_detail(text: str) -> dict[str, Any]:
    line = first_line(text)
    match = DETAIL_LINE.match(line)
    if not match:
        raise ValueError(err_message(text) if tcp_status(text) != "OK" else "unreadable flight detail")
    day, month, year = int(match.group(4)), int(match.group(5)), int(match.group(6))
    return {
        "flightNumber": match.group(1),
        "source": match.group(2),
        "destination": match.group(3),
        "date": iso_date(day, month, year),
        "seatsAvailable": int(match.group(7)),
        "seatsTotal": int(match.group(8)),
        "price": float(match.group(9)),
    }


def parse_admin_flights(text: str) -> list[dict[str, Any]]:
    flights: list[dict[str, Any]] = []
    for raw in text.replace("\r\n", "\n").split("\n"):
        line = raw.strip()
        if not line or line.lower().startswith("all flights:"):
            continue
        match = DETAIL_LINE.match(line)
        if not match:
            continue
        day, month, year = int(match.group(4)), int(match.group(5)), int(match.group(6))
        flights.append(
            {
                "flightNumber": match.group(1),
                "source": match.group(2),
                "destination": match.group(3),
                "date": iso_date(day, month, year),
                "seatsAvailable": int(match.group(7)),
                "seatsTotal": int(match.group(8)),
                "price": float(match.group(9)),
            }
        )
    return flights


def parse_bookings(text: str) -> list[dict[str, Any]]:
    if "No bookings found" in text:
        return []
    bookings: list[dict[str, Any]] = []
    for raw in text.replace("\r\n", "\n").split("\n"):
        line = raw.strip()
        if not line or line.startswith("Bookings for "):
            continue
        match = BOOKING_LINE.match(line)
        if not match:
            continue
        day, month, year = int(match.group(5)), int(match.group(6)), int(match.group(7))
        bookings.append(
            {
                "bookingId": match.group(1),
                "flightNumber": match.group(2),
                "source": match.group(3),
                "destination": match.group(4),
                "date": iso_date(day, month, year),
                "seats": int(match.group(8)),
            }
        )
    return bookings


def parse_admins(text: str) -> list[str]:
    if "No admins found" in text:
        return []
    admins: list[str] = []
    for raw in text.replace("\r\n", "\n").split("\n"):
        line = raw.strip()
        if not line or line.lower() == "admins:":
            continue
        admins.append(line)
    return admins


def parse_feed(text: str) -> list[str]:
    messages = [ln.strip() for ln in text.replace("\r\n", "\n").split("\n") if ln.strip()]
    return messages
