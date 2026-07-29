#!/usr/bin/env bash
# Starts an isolated server instance and runs concurrent booking integration tests.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${TEST_PORT:-19090}"
HOST="127.0.0.1"
TEST_DIR=""
SERVER_PID=""

cleanup() {
    if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
        kill -TERM "${SERVER_PID}" 2>/dev/null || true
        wait "${SERVER_PID}" 2>/dev/null || true
    fi
    if [[ -n "${TEST_DIR}" && -d "${TEST_DIR}" ]]; then
        rm -rf "${TEST_DIR}"
    fi
}
trap cleanup EXIT INT TERM

echo "== Building server and test harness =="
make -C "${ROOT}" server_app concurrent_booking_test

TEST_DIR="$(mktemp -d)"
cp "${ROOT}/server_app" "${TEST_DIR}/"
cp "${ROOT}/concurrent_booking_test" "${TEST_DIR}/"

echo "== Starting isolated server on port ${PORT} in ${TEST_DIR} =="
(
    cd "${TEST_DIR}"
    ./server_app "${PORT}"
) &
SERVER_PID=$!

echo -n "Waiting for server"
for i in $(seq 1 50); do
    if printf 'PING\n' | nc -w 1 "${HOST}" "${PORT}" 2>/dev/null | grep -q ONLINE; then
        echo " ... online"
        break
    fi
    echo -n "."
    sleep 0.1
    if [[ "$i" -eq 50 ]]; then
        echo
        echo "Server failed to start on ${HOST}:${PORT}" >&2
        exit 1
    fi
done

echo "== Running concurrent booking tests =="
(
    cd "${TEST_DIR}"
    ./concurrent_booking_test "${HOST}" "${PORT}"
)
