#!/bin/bash
# Copyright (c) 2026 VillageSQL Contributors
# Extract debug symbols from built binaries before stripping.
#
# Usage: extract_symbols.sh <build_dir> <output_dir>

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
source "$SCRIPT_DIR/vsql_script_utils.sh"

BUILD_DIR="${1:?Usage: $0 <build_dir> <output_dir>}"
OUTPUT_DIR="${2:?Usage: $0 <build_dir> <output_dir>}"

vsql_parse_version "$SOURCE_DIR"
vsql_platform_info

SYMBOL_PACKAGE_NAME="villagesql-symbols-${VSQL_VERSION}-${PLATFORM}-${ARCH}"
SYMBOL_DIR="$OUTPUT_DIR/$SYMBOL_PACKAGE_NAME"

log_step "VillageSQL Symbol Extraction"
echo ""
log_info "Build Directory:  $BUILD_DIR"
log_info "Output Directory: $OUTPUT_DIR"
log_info "Version:          $VSQL_VERSION"
log_info "Platform:         $PLATFORM-$ARCH"
echo ""

mkdir -p "$SYMBOL_DIR"

RUNTIME_DIR="$BUILD_DIR/runtime_output_directory"
LIBRARY_DIR="$BUILD_DIR/library_output_directory"

extract_symbols_macos() {
    local binary="$1"
    local name
    name="$(basename "$binary")"
    log_info "Extracting symbols: $name"
    dsymutil "$binary" -o "$SYMBOL_DIR/${name}.dSYM"
}

extract_symbols_linux() {
    local binary="$1"
    local name
    name="$(basename "$binary")"
    log_info "Extracting symbols: $name"
    objcopy --only-keep-debug "$binary" "$SYMBOL_DIR/${name}.debug"
    objcopy --add-gnu-debuglink="$SYMBOL_DIR/${name}.debug" "$binary"
}

extract_one() {
    local binary="$1"
    if [[ ! -f "$binary" ]]; then
        log_warn "Binary not found, skipping: $binary"
        return
    fi
    if [[ "$PLATFORM" == "macos" ]]; then
        extract_symbols_macos "$binary"
    else
        extract_symbols_linux "$binary"
    fi
}

# Extract from primary binaries
extract_one "$RUNTIME_DIR/mysqld"
extract_one "$RUNTIME_DIR/mysql"
extract_one "$RUNTIME_DIR/mysqladmin"

# Extract from shared libraries
if [[ "$PLATFORM" == "macos" ]]; then
    for lib in "$LIBRARY_DIR"/libmysqlclient*.dylib; do
        [[ -f "$lib" ]] && extract_one "$lib"
    done
else
    for lib in "$LIBRARY_DIR"/libmysqlclient*.so*; do
        [[ -f "$lib" ]] && extract_one "$lib"
    done
fi

# Write BUILD_ID file
GIT_SHA=$(git -C "$SOURCE_DIR" rev-parse HEAD 2>/dev/null || echo "unknown")
cat > "$SYMBOL_DIR/BUILD_ID" <<EOF
Version: $VSQL_VERSION
Platform: $PLATFORM
Arch: $ARCH
GitSha: $GIT_SHA
BuildDate: $(date -u +"%Y-%m-%d %H:%M:%S UTC")
EOF
log_info "Wrote BUILD_ID: $SYMBOL_DIR/BUILD_ID"

# Create tarball
TARBALL_NAME="${SYMBOL_PACKAGE_NAME}.tar.gz"
log_step "Creating symbol tarball: $TARBALL_NAME"
cd "$OUTPUT_DIR"
tar czf "$TARBALL_NAME" "$SYMBOL_PACKAGE_NAME"

TARBALL_SIZE=$(du -h "$OUTPUT_DIR/$TARBALL_NAME" | cut -f1)
log_step "Symbols tarball created: $OUTPUT_DIR/$TARBALL_NAME ($TARBALL_SIZE)"
