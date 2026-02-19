#!/bin/bash
# Connect to VillageSQL server

BASEDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOCKET="$BASEDIR/mysql.sock"

if [[ ! -S "$SOCKET" ]]; then
    echo "Error: Server doesn't appear to be running (socket not found: $SOCKET)"
    echo "Start it with: ./start-server.sh"
    exit 1
fi

# Default to root user with no password (initialized with --initialize-insecure)
exec "$BASEDIR/bin/mysql" --socket="$SOCKET" -u root "$@"
