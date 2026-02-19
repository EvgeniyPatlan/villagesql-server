#!/bin/bash
# Start VillageSQL server

set -e

BASEDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DATADIR="$BASEDIR/data"
SOCKET="$BASEDIR/mysql.sock"
PORT="${MYSQL_PORT:-3307}"
PIDFILE="$BASEDIR/mysql.pid"
LOGFILE="$DATADIR/error.log"

if [[ ! -d "$DATADIR" ]]; then
    echo "Error: Database not initialized. Run './init-db.sh' first."
    exit 1
fi

if [[ -f "$PIDFILE" ]]; then
    if kill -0 $(cat "$PIDFILE") 2>/dev/null; then
        echo "Error: Server is already running (PID: $(cat "$PIDFILE"))"
        exit 1
    else
        echo "Removing stale PID file..."
        rm -f "$PIDFILE"
    fi
fi

echo "Starting VillageSQL server..."
echo "  Data directory: $DATADIR"
echo "  Socket: $SOCKET"
echo "  Port: $PORT"
echo "  Log: $LOGFILE"
echo ""

"$BASEDIR/bin/mysqld" \
    --no-defaults \
    --basedir="$BASEDIR" \
    --datadir="$DATADIR" \
    --socket="$SOCKET" \
    --port="$PORT" \
    --bind-address=127.0.0.1 \
    --pid-file="$PIDFILE" \
    --log-error="$LOGFILE" \
    --log-error-verbosity=3 &

SERVER_PID=$!
echo "Server starting (PID: $SERVER_PID)..."

# Wait for server to be ready
for i in {1..30}; do
    if "$BASEDIR/bin/mysqladmin" --socket="$SOCKET" ping >/dev/null 2>&1; then
        echo ""
        echo "✓ Server is ready!"
        echo ""
        echo "Connect with: ./connect.sh"
        echo "Stop with: ./stop-server.sh"
        echo "View logs: tail -f $LOGFILE"
        exit 0
    fi
    sleep 1
done

echo ""
echo "Warning: Server did not become ready within 30 seconds."
echo "Check the log file: $LOGFILE"
exit 1
