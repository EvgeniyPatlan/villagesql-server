#!/bin/bash

# VillageSQL CI Build Script
# Performs a clean build and basic validation for CI environments

set -e

# Determine build type (default to debug)
BUILD_TYPE="${BUILD_TYPE:-debug}"

echo "=== VillageSQL CI Build ==="
echo "Build type: ${BUILD_TYPE}"
echo "Available CPUs: $(nproc)"
echo "Parallel jobs: ${PARALLEL_JOBS}"
echo "Working directory: $(pwd)"

# Use SOURCE_DIR from environment, fallback to current directory
SOURCE_DIR="${SOURCE_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

# Use BUILD_DIR from environment, fallback to ../build relative to source
BUILD_DIR="${BUILD_DIR:-${SOURCE_DIR}/../build}"

# Create build directory
echo "Creating build directory: ${BUILD_DIR}"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Configure CMake
echo "=== CMake Configuration ==="

# Set debug flag based on build type
if [ "$BUILD_TYPE" = "debug" ]; then
  WITH_DEBUG_FLAG="-DWITH_DEBUG=1"
else
  WITH_DEBUG_FLAG=""
fi

cmake "$SOURCE_DIR" \
  -DCMAKE_INSTALL_PREFIX=/tmp/villagesql-install \
  $WITH_DEBUG_FLAG \
  -DWITH_SSL=system \
  -DMYSQL_MAINTAINER_MODE=OFF \
  -DDOWNLOAD_BOOST=1 \
  -DWITH_BOOST=/tmp/boost \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  ${CMAKE_EXTRA_FLAGS}

echo "=== Build Information ==="

# Build the project
echo "=== Building VillageSQL ==="
echo "Starting build with ${PARALLEL_JOBS} parallel jobs..."
make -j"${PARALLEL_JOBS}"

echo "=== Building VillageSQL Unit Tests ==="
make -j"${PARALLEL_JOBS}" villagesql-unit-tests

echo "=== Build Success ==="
echo "VillageSQL built successfully!"
