#!/bin/sh
set -eu

# Full release build: never inherit the local incremental-build shortcut.
unset BLUEJAY_SKIP_MESA

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec "$script_dir/build-flash.sh" all
