#!/usr/bin/env bash
# Copyright (c) 2026 VillageSQL Contributors
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, see <https://www.gnu.org/licenses/>.

# Release multi-arch VillageSQL Server images to a registry (Docker Hub by
# default), by orchestrating the two scripts that do the work:
#
#   publish_image.sh     one platform -> one arch-tagged image
#   publish_manifest.sh  those arch images -> the shared multi-arch tags
#
# Each platform is released serially under its own arch-specific tag (e.g.
# villagesql/server:0.0.5-arm64), then the shared tags (the primary tag, plus
# "latest" and "stable") are stitched into a manifest list referencing them.
#
# This is the convenience path: every platform, then the manifest, in one go.
# Because the image compiles the server from source, building an arch that
# does not match the host runs under QEMU emulation and is slow, about six
# hours on a 2026 MacBoook Pro M5. For a real release, run publish_image.sh
# --push directly on native hardware for each arch, then publish_manifest.sh
# once.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=docker_release_lib.sh
source "$SCRIPT_DIR/docker_release_lib.sh"

TAG=""
REPO="$DOCKER_REPO"
PLATFORMS="$DEFAULT_PLATFORMS"
SHARED_TAGS="$DOCKER_SHARED_TAGS"

usage() {
    cat <<EOF
Build and publish multi-arch VillageSQL Server images.

Usage:
  publish_docker.sh --tag TAG [options]

Options:
  -t, --tag TAG          Primary tag to publish, e.g. a version (required)
  -r, --repo REPO        Image repository (default: $DOCKER_REPO)
  -p, --platforms LIST   Comma-separated platforms to release
                         (default: $DEFAULT_PLATFORMS)
  -s, --shared-tags LIST Comma-separated shared tags applied to the manifest,
                         in addition to the primary tag
                         (default: $DOCKER_SHARED_TAGS)
  -n, --dry-run          Print the commands without running them
  -h, --help             Show this help and exit

If your goal is building a local image locally without publishing,
publishing one arch at a time, or to redo just the manifest, call
publish_image.sh and publish_manifest.sh directly.

Build args are taken from the environment and forwarded by inheritance:
  VSQL_PRE_RELEASE_VERSION  pre-release suffix (default: empty, a release)
  VSQL_DEV_ABI              expose the development ABI (default: ON)

Examples:
  # Full release of 0.0.5 for both arches, tagged latest + stable
  publish_docker.sh --tag 0.0.5

  # Rehearse the whole release without touching the registry
  publish_docker.sh --tag 0.0.5 --dry-run

  # Publish 0.0.5 without moving latest or stable
  publish_docker.sh --tag 0.0.5 --shared-tags ""
EOF
}

main() {
    while [ $# -gt 0 ]; do
        case "$1" in
            -t|--tag)         TAG="$2"; shift 2 ;;
            -r|--repo)        REPO="$2"; shift 2 ;;
            -p|--platforms)   PLATFORMS="$2"; shift 2 ;;
            -s|--shared-tags) SHARED_TAGS="$2"; shift 2 ;;
            -n|--dry-run)     DRY_RUN=1; shift ;;
            -h|--help)        usage; exit 0 ;;
            *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
        esac
    done

    [ -n "$TAG" ] || { echo "error: --tag is required" >&2; usage >&2; exit 2; }
    require_docker

    echo "Repository : $REPO"
    echo "Tag        : $TAG"
    echo "Platforms  : $PLATFORMS"
    echo "Shared tags: $TAG $(echo "$SHARED_TAGS" | tr ',' ' ')"
    echo "Build args : VSQL_PRE_RELEASE_VERSION='$VSQL_PRE_RELEASE_VERSION' VSQL_DEV_ABI=$VSQL_DEV_ABI"
    [ "$DRY_RUN" -eq 1 ] && echo "(dry run: commands will be printed, not executed)"
    echo ""

    # Flags common to both children. Each child re-derives its own tags from
    # the tag and repo, so passing these explicitly keeps them in step.
    local common=(--tag "$TAG" --repo "$REPO")
    [ "$DRY_RUN" -eq 1 ] && common+=(--dry-run)

    local platform
    split_list "$PLATFORMS"
    [ "${#SPLIT_RESULT[@]}" -gt 0 ] || die "--platforms is empty"
    for platform in "${SPLIT_RESULT[@]}"; do
        "$SCRIPT_DIR/publish_image.sh" "${common[@]}" --push --platform "$platform"
        echo ""
    done

    "$SCRIPT_DIR/publish_manifest.sh" "${common[@]}" \
        --platforms "$PLATFORMS" --shared-tags "$SHARED_TAGS"

    echo ""
    echo "=== Done ==="
}

main "$@"
