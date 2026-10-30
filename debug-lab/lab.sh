#!/usr/bin/env bash
set -euo pipefail
lab_dir=$(cd "$(dirname "$0")" && pwd)
compose=(docker compose -f "$lab_dir/compose.yaml")
run=(docker exec -u debugger mooncake-debug)

prepare_key() {
  mkdir -p "$lab_dir/.local"
  chmod 700 "$lab_dir/.local"
  if [[ ! -s "$lab_dir/.local/root_password" ]]; then
    echo "Save a root SSH password in $lab_dir/.local/root_password (mode 600) before starting the lab." >&2
    return 1
  fi
  if [[ ! -f "$lab_dir/.local/id_ed25519" ]]; then
    ssh-keygen -q -t ed25519 -N '' -C mooncake-debug-lab \
      -f "$lab_dir/.local/id_ed25519"
  fi
}

case "${1:-help}" in
  setup)
    prepare_key
    "${compose[@]}" up -d --build
    "${run[@]}" bash /workspace/mooncake/debug-lab/configure.sh
    "${run[@]}" cmake --build /workspace/build --parallel 2 \
      --target mooncake_master store_debug_demo
    "${run[@]}" bash /workspace/mooncake/debug-lab/services.sh start
    "${run[@]}" bash /workspace/mooncake/debug-lab/verify.sh ;;
  up) prepare_key; "${compose[@]}" up -d ;;
  build)
    "${run[@]}" bash /workspace/mooncake/debug-lab/configure.sh
    "${run[@]}" cmake --build /workspace/build --parallel 2 \
      --target mooncake_master store_debug_demo ;;
  start|stop|restart|status)
    "${run[@]}" bash /workspace/mooncake/debug-lab/services.sh "$1" ;;
  verify) "${run[@]}" bash /workspace/mooncake/debug-lab/verify.sh ;;
  capture) docker exec mooncake-debug bash /workspace/mooncake/debug-lab/capture.sh ;;
  shell)
    exec ssh -i "$lab_dir/.local/id_ed25519" -o IdentitiesOnly=yes \
      -o StrictHostKeyChecking=accept-new \
      -o "UserKnownHostsFile=$lab_dir/.local/known_hosts" \
      -p 33333 debugger@127.0.0.1 ;;
  down)
    "${run[@]}" bash /workspace/mooncake/debug-lab/services.sh stop
    "${compose[@]}" down ;;
  *) echo 'Usage: debug-lab/lab.sh setup|up|build|start|stop|restart|status|verify|capture|shell|down' ;;
esac
