#!/usr/bin/env bash
# Copies the Schwung UI's shared modules from a Move into host_ref/ (git-ignored) so the tests can run the host's
# own page planner, contract validator and widget frame against this module's manifest.
#   tools/fetch_host_ref.sh [user@host]      default ableton@move.local
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HOST="${1:-ableton@move.local}"
rm -rf "$ROOT/host_ref/shared"
mkdir -p "$ROOT/host_ref"
scp -q -r -o BatchMode=yes "$HOST:/data/UserData/schwung/shared" "$ROOT/host_ref/shared"
echo "host reference copied to host_ref/shared ($(ls "$ROOT/host_ref/shared/param_pages" | wc -l) param_pages modules)"
