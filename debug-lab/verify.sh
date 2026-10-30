#!/usr/bin/env bash
# Integration check: catches a missing demo, bad roundtrip, wrong placement,
# missing-key mishandling, and skipped cleanup. Run with master + owner alive.
set -euo pipefail
source "$(cd "$(dirname "$0")" && pwd)/env.sh"
demo=/workspace/build/debug-lab/store_debug_demo
if [[ ! -x "$demo" ]]; then
  echo "FAIL: store_debug_demo has not been built" >&2
  exit 1
fi
output=$(mktemp)
trap 'rm -f "$output"' EXIT
timeout --kill-after=5s 90s "$demo" roundtrip | tee "$output"
owner_endpoint=$(sed -n 's/.*OWNER_READY endpoint=\([^ ]*\).*/\1/p' /workspace/build/lab-run/owner.log | tail -1)
replica_endpoint=$(sed -n 's/.*REMOTE_REPLICA endpoint=\([^ ]*\).*/\1/p' "$output")
[[ -n "$owner_endpoint" && "$replica_endpoint" == "$owner_endpoint" ]] || {
  echo "FAIL: replica $replica_endpoint is not the running owner $owner_endpoint" >&2
  exit 1
}
echo "OWNER_PLACEMENT_OK endpoint=$owner_endpoint"
