#!/bin/bash
# Initialize VillageSQL database

set -e

BASEDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DATADIR="$BASEDIR/data"

if [[ -d "$DATADIR" ]]; then
    echo "Error: Data directory already exists at $DATADIR"
    echo "Remove it first if you want to reinitialize: rm -rf $DATADIR"
    exit 1
fi

echo "Initializing VillageSQL database..."
echo "Base directory: $BASEDIR"
echo "Data directory: $DATADIR"

"$BASEDIR/bin/mysqld" \
    --no-defaults \
    --initialize-insecure \
    --basedir="$BASEDIR" \
    --datadir="$DATADIR" \
    --log-error-verbosity=3

echo ""
echo "✓ Database initialized successfully!"
echo ""
echo "Next steps:"
echo "  1. Start the server: ./start-server.sh"
echo "  2. Connect to it: ./connect.sh"
