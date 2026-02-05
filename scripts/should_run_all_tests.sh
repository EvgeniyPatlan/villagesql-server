#!/bin/bash

# Check if the commits pointed to by the two most recent nightly tags differ

set -e

readonly RUN_TESTS=0
readonly SKIP_TESTS=1

# Get the two most recent nightly tags
LATEST_TAG=$(git tag -l 'nightly.20*' | sort -r | head -n 1)
PREVIOUS_TAG=$(git tag -l 'nightly.20*' | sort -r | head -n 2 | tail -n 1)

if [ -z "$LATEST_TAG" ]; then
  echo "No nightly tags exist yet, should run all test suites"
  exit $RUN_TESTS
fi

if [ -z "$PREVIOUS_TAG" ] || [ "$LATEST_TAG" = "$PREVIOUS_TAG" ]; then
  echo "Only one nightly tag exists, should run all test suites"
  exit $RUN_TESTS
fi

echo "Most recent nightly tag: $LATEST_TAG"
echo "Previous nightly tag: $PREVIOUS_TAG"

# Get commit SHAs for both tags
LATEST_COMMIT=$(git rev-parse "$LATEST_TAG^{commit}")
PREVIOUS_COMMIT=$(git rev-parse "$PREVIOUS_TAG^{commit}")

echo "Commit for $LATEST_TAG: $LATEST_COMMIT"
echo "Commit for $PREVIOUS_TAG: $PREVIOUS_COMMIT"

if [ "$LATEST_COMMIT" != "$PREVIOUS_COMMIT" ]; then
  echo "Commits differ between the two most recent nightly tags, should run all test suites"
  exit $RUN_TESTS
else
  echo "Commits are the same between the two most recent nightly tags, should skip additional test suites"
  exit $SKIP_TESTS
fi
