#!/bin/bash
# Copyright (c) 2026 VillageSQL Contributors
# Package the VillageSQL development server tarball from an already-built (and
# already-stripped) server build.
#
# Env vars:
#   BUILD_DIR   - path to the cmake build directory (required)
#   OUTPUT_DIR  - directory where the final tarball is written (default: $PWD)
#   VEB_DIR     - directory containing .veb files to include (optional)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
source "$SCRIPT_DIR/vsql_script_utils.sh"

BUILD_DIR="${BUILD_DIR:?BUILD_DIR must be set}"
OUTPUT_DIR="${OUTPUT_DIR:-$PWD}"
VEB_DIR="${VEB_DIR:-}"

vsql_parse_version "$SOURCE_DIR"
vsql_platform_info

PACKAGE_NAME="villagesql-dev-server-${VSQL_VERSION}-${PLATFORM}-${ARCH}"
TARBALL_NAME="${PACKAGE_NAME}.tar.gz"
STAGING_DIR="${TMPDIR:-/tmp}/vsql_dev_staging_$$"

cleanup() {
    [[ -d "$STAGING_DIR" ]] && rm -rf "$STAGING_DIR"
}
trap cleanup EXIT

log_step "VillageSQL Dev Server Packaging"
echo ""
log_info "Version:         $VSQL_VERSION"
log_info "Platform:        $PLATFORM-$ARCH"
log_info "Build Directory: $BUILD_DIR"
log_info "Output Directory:$OUTPUT_DIR"
[[ -n "$VEB_DIR" ]] && log_info "VEB Directory:   $VEB_DIR"
echo ""

mkdir -p "$STAGING_DIR" "$OUTPUT_DIR"

log_step "Step 1: Generating base package with CPack..."
cd "$BUILD_DIR"

CPACK_COMPONENTS="Client;Server;Server_Scripts;SharedLibraries;SupportFiles;Readme;Info;ExampleVebs;Test;TestReadme"
log_info "CPack components: $CPACK_COMPONENTS"

cpack -G TGZ \
    -D CPACK_ARCHIVE_COMPONENT_INSTALL=ON \
    -D CPACK_COMPONENTS_GROUPING=ALL_COMPONENTS_IN_ONE \
    -D "CPACK_COMPONENTS_ALL=${CPACK_COMPONENTS}" \
    -D CPACK_PACKAGE_FILE_NAME="villagesql-base-temp" \
    >/dev/null 2>&1 || die "CPack failed. Check that the build is complete."

BASE_TARBALL="$BUILD_DIR/villagesql-base-temp.tar.gz"
[[ -f "$BASE_TARBALL" ]] || die "CPack did not create expected tarball: $BASE_TARBALL"

ORIGINAL_SIZE=$(du -h "$BASE_TARBALL" | cut -f1)
log_info "Base package size: $ORIGINAL_SIZE"

log_step "Step 2: Extracting base package..."
mkdir -p "$STAGING_DIR/$PACKAGE_NAME"
cd "$STAGING_DIR/$PACKAGE_NAME"
tar xzf "$BASE_TARBALL"

log_step "Step 3: Stripping unnecessary test data..."
if [[ -d "mysql-test" ]]; then
    if [[ -d "mysql-test/std_data" ]]; then
        log_info "Reducing std_data to SSL certificates only..."
        local_ssl_tmp="${TMPDIR:-/tmp}/mtr_ssl_$$"
        mkdir -p "$local_ssl_tmp"
        for f in cacert.pem server-cert.pem server-key.pem client-cert.pem client-key.pem; do
            [[ -f "mysql-test/std_data/$f" ]] && cp "mysql-test/std_data/$f" "$local_ssl_tmp/" || true
        done
        rm -rf mysql-test/std_data
        mkdir -p mysql-test/std_data
        mv "$local_ssl_tmp"/* mysql-test/std_data/ 2>/dev/null || true
        rm -rf "$local_ssl_tmp"
        STD_DATA_SIZE=$(du -sh mysql-test/std_data 2>/dev/null | cut -f1 || echo "unknown")
        log_info "Preserved essential SSL certificates in std_data ($STD_DATA_SIZE)"
    fi

    log_info "Removing non-villagesql test suites..."
    if [[ -d "mysql-test/suite" ]]; then
        find mysql-test/suite -mindepth 1 -maxdepth 1 -type d ! -name "villagesql" -exec rm -rf {} \; 2>/dev/null || true
    fi

    log_info "Removing top-level test files and results..."
    rm -rf mysql-test/r mysql-test/t

    MYSQL_TEST_SIZE=$(du -sh mysql-test 2>/dev/null | cut -f1 || echo "unknown")
    log_info "mysql-test framework size after cleanup: $MYSQL_TEST_SIZE"
fi

log_step "Step 4: Removing unwanted binaries..."
UNWANTED_BINS=(
    mysql_client_test
    mysql_test_event_tracking
    mysql_keyring_encryption_test
    myisamchk
    myisampack
    myisam_ftdump
    myisamlog
    ibd2sdi
    innochecksum
    mysqlxtest
)
for b in "${UNWANTED_BINS[@]}"; do
    rm -f "bin/$b" && log_info "Removed bin/$b" || true
done

log_step "Step 5: Removing non-English share/ language directories..."
if [[ -d "share" ]]; then
    find share -mindepth 1 -maxdepth 1 -type d \
        ! -name "charsets" \
        ! -name "english" \
        ! -name "mysql-test" \
        -exec rm -rf {} \; 2>/dev/null || true
    log_info "Kept share/charsets, share/english, share/mysql-test"
fi

log_step "Step 6: Removing test/example plugins..."
if [[ -d "lib/plugin" ]]; then
    find lib/plugin -maxdepth 1 \( \
        -name "libtest_*" \
        -o -name "component_test_*" \
        -o -name "component_example_*" \
    \) -exec rm -f {} \; 2>/dev/null || true
    log_info "Removed test/example plugins"
fi

if [[ -n "$VEB_DIR" && -d "$VEB_DIR" ]]; then
    log_step "Step 7: Copying VEB files from $VEB_DIR..."
    mkdir -p lib/veb
    VEB_COUNT=0
    for veb in "$VEB_DIR"/*.veb; do
        [[ -f "$veb" ]] || continue
        cp "$veb" lib/veb/
        log_info "$(basename "$veb")"
        VEB_COUNT=$((VEB_COUNT + 1))
    done
    log_info "Copied $VEB_COUNT VEB file(s)"
fi

# Step 8: Add convenience script
log_step "Step 8: Adding convenience scripts..."
TEMPLATE_DIR="$SOURCE_DIR/villagesql/dev_server"
cp "$TEMPLATE_DIR/villagesql.sh" villagesql && chmod +x villagesql
[[ -f "villagesql" ]] || die "Failed to copy villagesql from $TEMPLATE_DIR"
log_info "Convenience scripts added"

# Step 9: Generate documentation
log_step "Step 9: Creating documentation..."

TEST_NOTE="**Note:** This package includes the test framework (mysql-test-run.pl, lib/, include/) with VillageSQL tests. MySQL test suites have been removed to save space. You can create your own test suites for your extensions."
sed "s|@TEST_NOTE@|$TEST_NOTE|g" "$TEMPLATE_DIR/TEST_DOCS.md.template" > test_docs.tmp

sed -e "s|@VSQL_VERSION@|$VSQL_VERSION|g" \
    -e "s|@PLATFORM@|$PLATFORM|g" \
    -e "s|@ARCH@|$ARCH|g" \
    -e "s|@BUILD_DATE@|$(date -u +"%Y-%m-%d")|g" \
    "$TEMPLATE_DIR/QUICKSTART.md.template" > QUICKSTART.md.tmp

awk '/@TEST_DOCS@/ { system("cat test_docs.tmp"); next } 1' QUICKSTART.md.tmp > QUICKSTART.md
rm test_docs.tmp QUICKSTART.md.tmp

cat > VERSION <<EOF
VillageSQL Version: $VSQL_VERSION
Platform: $PLATFORM-$ARCH
Build Date: $(date -u +"%Y-%m-%d %H:%M:%S UTC")
Package Type: Development Server
EOF

# Step 10: Create final tarball
log_step "Step 10: Creating final tarball..."
cd "$STAGING_DIR"
tar czf "$TARBALL_NAME" "$PACKAGE_NAME"

mkdir -p "$OUTPUT_DIR"
mv "$TARBALL_NAME" "$OUTPUT_DIR/"

rm -f "$BASE_TARBALL"

FINAL_SIZE=$(du -h "$OUTPUT_DIR/$TARBALL_NAME" | cut -f1)
FILE_COUNT=$(find "$PACKAGE_NAME" -type f | wc -l | tr -d ' ')

log_step "Package created successfully!"
echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "  Package: $TARBALL_NAME"
echo "  Size: $FINAL_SIZE (original CPack: $ORIGINAL_SIZE)"
echo "  Files: $FILE_COUNT"
echo "  Location: $OUTPUT_DIR/$TARBALL_NAME"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
