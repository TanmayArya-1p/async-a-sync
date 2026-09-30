#!/bin/sh
# Applies every *.patch in PATCH_DIR to the Fil-C source checkout FILC_SRC,
# once. A patch already applied is left alone, so its files keep their
# timestamps and are not rebuilt. A file that holds something else, such as a
# whole-file copy an older checkout of this repository installed, is first
# restored from upstream and then patched.
#
# Usage: scripts/apply_filc_patches.sh FILC_SRC PATCH_DIR
set -eu

FILC_SRC=$1
PATCH_DIR=$2

for patch in "$PATCH_DIR"/*.patch; do
  name=$(basename "$patch")
  if git -C "$FILC_SRC" apply --reverse --check "$patch" 2>/dev/null; then
    echo "== already installed: $name"
    continue
  fi
  if ! git -C "$FILC_SRC" apply --check "$patch" 2>/dev/null; then
    git -C "$FILC_SRC" apply --numstat "$patch" | cut -f3 |
      while read -r file; do
        echo "== restoring upstream $file"
        git -C "$FILC_SRC" checkout -- "$file"
      done
  fi
  echo "== installing: $name"
  if ! git -C "$FILC_SRC" apply "$patch"; then
    echo "apply_filc_patches.sh: $name does not match $FILC_SRC" >&2
    exit 1
  fi
done
