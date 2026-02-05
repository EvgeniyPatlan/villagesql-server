#!/bin/bash
# Edit script for vsql_copyright_allowlisted test case
# Modifies an allowlisted file with non-standard copyright

set -e

FILE="extra/libbacktrace/sha9ae4f4a/internal.h"

echo "Modifying allowlisted file: $FILE"

# Add a comment at the end
echo "/* villint test comment */" >> "$FILE"

echo "Done: $FILE modified (allowlisted, so no copyright violation expected)"
