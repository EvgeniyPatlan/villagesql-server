#!/bin/bash
# Copyright (c) 2026 VillageSQL Contributors
# Orchestrator script: build, test, and package the VillageSQL development server.
#
# Env vars:
#   BUILD_DIR                  - cmake build directory (default: <source>/../build)
#   OUTPUT_DIR                 - where the final tarballs are written (default: $PWD)
#   CMAKE_EXTRA_FLAGS          - additional cmake flags
#   BUILD_BUNDLED_EXTENSIONS   - set to 1 to build+test bundled extensions (default: 0)
#   RUN_TESTS                  - set to 0 to skip server tests (default: 1)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
source "$SCRIPT_DIR/vsql_script_utils.sh"

BUILD_DIR="${BUILD_DIR:-$(cd "$SOURCE_DIR/.." && pwd)/build}"
OUTPUT_DIR="${OUTPUT_DIR:-$PWD}"
RUN_TESTS="${RUN_TESTS:-1}"
BUILD_BUNDLED_EXTENSIONS="${BUILD_BUNDLED_EXTENSIONS:-0}"

# _EXT_CLONES_DIR is populated in step 2.5 so that the sources survive for
# test_extension_vebs.sh.
_EXT_CLONES_DIR=""

cleanup() {
    [[ -n "$_EXT_CLONES_DIR" ]] && rm -rf "$_EXT_CLONES_DIR"
    return 0
}
trap cleanup EXIT

log_step "VillageSQL Development Server Package Builder"
echo ""

if [[ ! -d "$SOURCE_DIR" ]]; then
    die "Source directory not found: $SOURCE_DIR"
fi
if [[ ! -f "$SOURCE_DIR/CMakeLists.txt" ]]; then
    die "Source directory doesn't appear to be valid (no CMakeLists.txt): $SOURCE_DIR"
fi

mkdir -p "$BUILD_DIR"

vsql_parse_version "$SOURCE_DIR"
vsql_platform_info

log_info "VillageSQL Version: $VSQL_VERSION"
log_info "Platform: $PLATFORM-$ARCH"
log_info "Build Directory: $BUILD_DIR"
log_info "Output Directory: $OUTPUT_DIR"
echo ""

log_step "Step 1: Configure and build..."
BUILD_DIR="$BUILD_DIR" SOURCE_DIR="$SOURCE_DIR" \
    "$SCRIPT_DIR/build-ci.sh" || die "build-ci.sh failed"

# TODO(villagesql): Extract package_dev_server.sh as a separate script so that
# CI can run build → test → package as discrete steps rather than
# embedding the test between two halves of this monolithic script.
if [[ "${BUILD_BUNDLED_EXTENSIONS:-0}" == "1" ]]; then
    log_step "Step 2: Building bundled extensions..."
    SDK_STAGING_DIR="$BUILD_DIR/villagesql-extension-sdk-${VSQL_VERSION}"
    [[ -d "$SDK_STAGING_DIR" ]] || die "SDK staging directory not found: $SDK_STAGING_DIR"

    _EXT_CLONES_DIR="$(mktemp -d)"
    EXTENSION_CLONES_DIR="$_EXT_CLONES_DIR" \
        "$SCRIPT_DIR/build_bundled_extensions.sh" \
            "$SDK_STAGING_DIR" \
            "$BUILD_DIR/veb_output_directory"
    log_info "Bundled extensions built"

    log_step "Step 2.5: Testing bundled extensions..."
    "$SCRIPT_DIR/test_extension_vebs.sh" \
        "$BUILD_DIR" \
        "$_EXT_CLONES_DIR"
    log_info "Bundled extensions tested"
else
    log_info "Skipping bundled extensions (set BUILD_BUNDLED_EXTENSIONS=1 to include)"
fi

log_step "Step 3: Extracting debug symbols..."
"$SCRIPT_DIR/extract_symbols.sh" "$BUILD_DIR" "$OUTPUT_DIR"

log_step "Step 4: Stripping binaries..."
"$SCRIPT_DIR/strip_binaries.sh" "$BUILD_DIR/runtime_output_directory"
"$SCRIPT_DIR/strip_binaries.sh" "$BUILD_DIR/library_output_directory"
"$SCRIPT_DIR/strip_binaries.sh" "$BUILD_DIR/plugin_output_directory"

# Determine VEB directory for packaging
VEB_DIR=""
if [[ "$BUILD_BUNDLED_EXTENSIONS" == "1" && -d "$BUILD_DIR/veb_output_directory" ]]; then
    VEB_DIR="$BUILD_DIR/veb_output_directory"
fi

log_step "Step 5: Packaging dev server..."
BUILD_DIR="$BUILD_DIR" \
OUTPUT_DIR="$OUTPUT_DIR" \
VEB_DIR="$VEB_DIR" \
    "$SCRIPT_DIR/package_dev_server.sh"

log_step "All done!"
echo ""
echo "Output: $OUTPUT_DIR"
