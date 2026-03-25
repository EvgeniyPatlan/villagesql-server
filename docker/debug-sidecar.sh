#!/usr/bin/env bash
# Copyright (c) 2026 VillageSQL Contributors
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the Free
# Software Foundation; either version 2 of the License, or (at your option)
# any later version.

# Run a privileged debug sidecar against a running VillageSQL server container.
#
# Attaches strace to mysqld and captures filesystem I/O syscalls, including:
#   - Which files are opened (and with what flags — O_DIRECT vs buffered)
#   - Read/write sizes and offsets (alignment)
#   - fsync / fdatasync barriers
#   - POSIX and BSD file locks (flock, fcntl F_SETLK)
#   - Renames (including atomic renameat2 swaps used during DDL)
#   - fallocate, truncate
#
# Usage:
#   docker/debug-sidecar.sh [CONTAINER] [OUTPUT_DIR]
#
#   CONTAINER   name or ID of the vsql server container (default: vsql)
#   OUTPUT_DIR  where to write the trace file (default: ./trace-output)
#
# The trace file is written into OUTPUT_DIR on the host via a bind mount.
# Press Ctrl+C to stop tracing; a quick summary is printed after.
#
# Uses villagesql-dev:latest as the sidecar image (strace is pre-installed).
# Build it first if you haven't: docker/dev/build.sh
#
# Example:
#   # Terminal 1 — start dev container running mysqld
#   docker/dev/run.sh
#   # inside: cmake --build . && bin/mysqld --initialize-insecure --datadir=/build/data
#   #         bin/mysqld --datadir=/build/data --user=root &
#
#   # Terminal 2 — run load test
#   cd bench && go run . --duration 120s
#
#   # Terminal 3 — start sidecar
#   docker/debug-sidecar.sh vsql-dev ./trace-output

set -euo pipefail

CONTAINER="${1:-vsql}"
OUTPUT_DIR="${2:-./trace-output}"

# Syscalls to capture — covers all InnoDB/MySQL I/O paths
SYSCALLS="openat,open,close"
SYSCALLS+=",read,write,pread64,pwrite64,readv,writev,preadv,pwritev,preadv2,pwritev2"
SYSCALLS+=",lseek"
SYSCALLS+=",fsync,fdatasync,sync_file_range"
SYSCALLS+=",flock,fcntl"
SYSCALLS+=",rename,renameat,renameat2"
SYSCALLS+=",truncate,ftruncate,fallocate"
SYSCALLS+=",unlink,unlinkat"

# ---------------------------------------------------------------------------
# Pre-flight checks
# ---------------------------------------------------------------------------

if ! docker inspect --format='{{.State.Running}}' "$CONTAINER" 2>/dev/null | grep -q true; then
    echo "error: container '$CONTAINER' is not running."
    echo ""
    echo "Start the server with:"
    echo "  docker run -d --name vsql -e MYSQL_ALLOW_EMPTY_PASSWORD=1 -p 3306:3306 villagesql/server:latest"
    exit 1
fi

mkdir -p "$OUTPUT_DIR"
OUTPUT_DIR="$(cd "$OUTPUT_DIR" && pwd)"

TRACE_BASENAME="strace-$(date +%Y%m%d-%H%M%S).log"
TRACE_FILE="$OUTPUT_DIR/$TRACE_BASENAME"

echo "==> Target container : $CONTAINER"
echo "==> Trace output     : $TRACE_FILE"
echo "==> Press Ctrl+C to stop."
echo ""

# ---------------------------------------------------------------------------
# Run sidecar
#
# --pid=container:<name>      share the target's PID namespace so we can see
#                             and strace its processes
# --cap-add SYS_PTRACE        required for ptrace(2)
# --security-opt seccomp=unconfined
#                             Docker's default seccomp profile blocks some
#                             ptrace operations; unconfined avoids that
# -v OUTPUT_DIR:/trace        bind-mount so the trace file lands on the host
# ---------------------------------------------------------------------------

docker run --rm -it \
    --name "vsql-debug-$$" \
    --pid="container:$CONTAINER" \
    --cap-add SYS_PTRACE \
    --security-opt seccomp=unconfined \
    -e TRACE_BASENAME="$TRACE_BASENAME" \
    -e SYSCALLS="$SYSCALLS" \
    -v "$OUTPUT_DIR:/trace" \
    villagesql-dev:latest \
    bash -c '
        set -e

        echo "==> Finding mysqld PID..."
        MYSQLD_PID=$(pgrep -x mysqld | head -1 || true)
        if [ -z "$MYSQLD_PID" ]; then
            echo "ERROR: mysqld not found. Processes visible from this namespace:"
            ps -eo pid,comm | head -20
            exit 1
        fi
        echo "==> mysqld PID: $MYSQLD_PID"
        echo ""

        # -f   follow threads (mysqld is highly threaded; InnoDB I/O threads
        #      do the actual writes, not the query thread)
        # -tt  microsecond timestamps
        # -y   annotate fd numbers with file paths, e.g. pwrite64(5</var/lib/mysql/ibdata1>)
        # -o   write to bind-mounted file

        echo "==> Attaching strace (Ctrl+C to stop)..."
        strace -f -tt -y \
            -p "$MYSQLD_PID" \
            -e trace="$SYSCALLS" \
            -o "/trace/$TRACE_BASENAME" \
            || true

        echo ""
        echo "==> Trace complete. Quick summary:"
        echo ""

        TFILE="/trace/$TRACE_BASENAME"

        echo "  Files opened (top 20 by open count):"
        grep -oP "openat\(.*?\"[^\"]+\"" "$TFILE" \
            | grep -oP "\"[^\"]+\"" \
            | sort | uniq -c | sort -rn | head -20 \
            | awk "{printf \"    %6d  %s\n\", \$1, \$2}" || true

        echo ""
        echo "  Direct I/O opens (O_DIRECT):"
        grep "O_DIRECT" "$TFILE" | grep -oP "openat\(.*?\"[^\"]+\"" \
            | grep -oP "\"[^\"]+\"" | sort | uniq -c | sort -rn \
            | awk "{printf \"    %6d  %s\n\", \$1, \$2}" || true

        echo ""
        echo "  Write call count by fd path (top 15):"
        grep -E "pwrite64\([0-9]+<[^>]+>" "$TFILE" \
            | grep -oP "<[^>]+" | tr -d "<" \
            | sort | uniq -c | sort -rn | head -15 \
            | awk "{printf \"    %6d  %s\n\", \$1, \$2}" || true

        echo ""
        echo "  Lock operations:"
        printf "    flock calls : "
        grep -c "flock(" "$TFILE" || echo 0
        printf "    fcntl SETLK : "
        grep -cE "fcntl.*F_SETLK" "$TFILE" || echo 0

        echo ""
        echo "  fsync / fdatasync calls:"
        printf "    fsync      : "
        grep -c "fsync(" "$TFILE" || echo 0
        printf "    fdatasync  : "
        grep -c "fdatasync(" "$TFILE" || echo 0

        echo ""
        echo "  Rename operations:"
        grep -E "rename(at)?(2)?\(" "$TFILE" | head -10 || true

        echo ""
        echo "Full trace: '"'"'$TRACE_BASENAME'"'"' in output directory."
    '
