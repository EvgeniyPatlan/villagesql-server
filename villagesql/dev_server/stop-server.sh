#!/bin/bash
# Stop VillageSQL server

BASEDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOCKET="$BASEDIR/mysql.sock"
PIDFILE="$BASEDIR/mysql.pid"

if [[ ! -f "$PIDFILE" ]]; then
    echo "Error: PID file not found. Is the server running?"
    exit 1
fi

PID=$(cat "$PIDFILE")

if ! kill -0 "$PID" 2>/dev/null; then
    echo "Error: Server not running (stale PID file)"
    rm -f "$PIDFILE"
    exit 1
fi

echo "Stopping VillageSQL server (PID: $PID)..."
"$BASEDIR/bin/mysqladmin" --socket="$SOCKET" -u root shutdown 2>/dev/null || true

# Wait for process to exit
for i in {1..30}; do
    if ! kill -0 "$PID" 2>/dev/null; then
        echo "✓ Server stopped successfully."
        rm -f "$PIDFILE" "$SOCKET"
        exit 0
    fi
    sleep 1
done

echo "Warning: Server did not stop gracefully. Sending SIGTERM..."
kill "$PID" 2>/dev/null || true
sleep 2

if kill -0 "$PID" 2>/dev/null; then
    echo "Warning: Server still running. Sending SIGKILL..."
    kill -9 "$PID" 2>/dev/null || true
fi

rm -f "$PIDFILE" "$SOCKET"
echo "Server stopped."
