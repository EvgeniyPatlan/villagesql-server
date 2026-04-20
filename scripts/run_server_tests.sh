#!/bin/bash
# Copyright (c) 2026 VillageSQL Contributors
# Run VillageSQL server tests (unit tests + MTR villagesql suite).
#
# Usage: run_server_tests.sh <build_dir>

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
source "$SCRIPT_DIR/vsql_script_utils.sh"

BUILD_DIR="${1:?Usage: $0 <build_dir>}"

MYSQLD="$BUILD_DIR/runtime_output_directory/mysqld"
MTR="$SOURCE_DIR/mysql-test/mysql-test-run.pl"

log_step "VillageSQL Server Tests"
echo ""

[[ -x "$MYSQLD" ]] || die "mysqld not found at $MYSQLD"
[[ -f "$MTR" ]]    || die "mysql-test-run.pl not found at $MTR"

log_info "Build Directory:  $BUILD_DIR"
log_info "Source Directory: $SOURCE_DIR"
echo ""

NCORES=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo "4")

UNIT_EXIT=0
MTR_EXIT=0

# Run villagesql unit tests
log_step "Running unit tests..."
(cd "$BUILD_DIR" && ctest -L villagesql) || UNIT_EXIT=$?

if [[ $UNIT_EXIT -ne 0 ]]; then
    log_error "Unit tests failed (exit $UNIT_EXIT)"
fi

# Run MTR villagesql suite
log_step "Running MTR villagesql suite..."
cd "$SOURCE_DIR/mysql-test"
MTR_BINDIR="$BUILD_DIR" perl mysql-test-run.pl \
    --do-suite=village \
    --nounit-tests \
    --parallel=auto \
    --retry=0 \
    || MTR_EXIT=$?

if [[ $MTR_EXIT -ne 0 ]]; then
    log_error "MTR tests failed (exit $MTR_EXIT)"
fi

if [[ $UNIT_EXIT -ne 0 || $MTR_EXIT -ne 0 ]]; then
    die "Server tests failed (unit=$UNIT_EXIT mtr=$MTR_EXIT)"
fi

log_step "All server tests passed"
