#!/bin/bash
# Copyright (c) 2026 VillageSQL Contributors
# Strip debug symbols from binaries in-place.
#
# Usage: strip_binaries.sh <directory>

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/vsql_script_utils.sh"

TARGET_DIR="${1:?Usage: $0 <directory>}"

if [[ ! -d "$TARGET_DIR" ]]; then
    die "Directory not found: $TARGET_DIR"
fi

PLATFORM="$(uname -s | tr '[:upper:]' '[:lower:]')"
[[ "$PLATFORM" == "darwin" ]] && PLATFORM="macos"

log_step "Stripping binaries in: $TARGET_DIR"
log_info "Platform: $PLATFORM"

STRIPPED=0

strip_file_macos() {
    local f="$1"
    if file "$f" | grep -qE 'Mach-O.*(executable|dynamically linked shared library|bundle)'; then
        strip -S "$f" 2>/dev/null && STRIPPED=$((STRIPPED + 1)) || log_warn "strip -S failed: $f"
    fi
}

strip_file_linux() {
    local f="$1"
    local ftype
    ftype=$(file "$f" 2>/dev/null || true)
    if echo "$ftype" | grep -qE '(ELF.*(executable|shared object))'; then
        strip --strip-debug --strip-unneeded "$f" 2>/dev/null && STRIPPED=$((STRIPPED + 1)) || log_warn "strip failed: $f"
    fi
}

while IFS= read -r -d '' f; do
    if [[ "$PLATFORM" == "macos" ]]; then
        strip_file_macos "$f"
    else
        strip_file_linux "$f"
    fi
done < <(find "$TARGET_DIR" -type f -print0)

log_step "Stripped $STRIPPED file(s)"
