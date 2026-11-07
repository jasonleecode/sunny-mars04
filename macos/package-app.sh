#!/bin/bash
# SPDX-License-Identifier: MIT
set -euo pipefail
project_dir="$(cd "$(dirname "$0")/.." && pwd)"
build_dir="${1:-$project_dir/build-macos}"
destination="${2:-$HOME/Applications/Mars04 Camera Lab.app}"
source_bundle="$build_dir/mars04-viewer.app"
if [[ ! -x "$source_bundle/Contents/MacOS/mars04-viewer" ]]; then
  echo "Build with -DMARS04_BUILD_GUI=ON first: $source_bundle" >&2
  exit 1
fi
if [[ -e "$destination" ]]; then
  echo "Destination already exists; choose a new destination: $destination" >&2
  exit 1
fi
mkdir -p "$(dirname "$destination")"
ditto "$source_bundle" "$destination"
cmake -DMARS04_APP="$destination" \
  -DMARS04_LIBRARY_DIRS="${MARS04_LIBRARY_DIRS:-/opt/homebrew/lib;/usr/local/lib}" \
  -P "$project_dir/macos/FixupBundle.cmake"
resources="$destination/Contents/Resources"
mkdir -p "$resources/docs"
cp "$project_dir/LICENSE" "$resources/LICENSE"
cp "$project_dir/README.md" "$resources/README.md"
cp "$project_dir/docs/"*.md "$resources/docs/"
# Private/local distribution: ad-hoc sign after install-name changes.
while IFS= read -r -d '' library; do
  codesign --force --sign - "$library"
done < <(find "$destination/Contents/Frameworks" -type f -name '*.dylib' -print0)
codesign --force --sign - "$destination"
codesign --verify --deep --strict "$destination"
echo "Packaged: $destination"
