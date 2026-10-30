#!/usr/bin/env bash
set -euo pipefail
source /workspace/mooncake/debug-lab/env.sh
state=/workspace/build/lab-run
mkdir -p "$state"
master=/workspace/build/mooncake-store/src/mooncake_master
demo=/workspace/build/debug-lab/store_debug_demo

alive() {
  local name=$1 pid expected
  local -a argv
  [[ -f "$state/$name.pid" && -f "$state/$name.started" ]] || return 1
  pid=$(cat "$state/$name.pid")
  [[ "$pid" =~ ^[0-9]+$ && -r "/proc/$pid/cmdline" ]] || return 1
  [[ "$(awk '{print $22}' "/proc/$pid/stat")" == "$(cat "$state/$name.started")" ]] || return 1
  mapfile -d '' -t argv < "/proc/$pid/cmdline"
  expected=$master
  [[ "$name" != owner ]] || expected=$demo
  [[ "${argv[0]:-}" == "$expected" ]] || return 1
  [[ "$name" != owner || "${argv[1]:-}" == owner ]]
}

record_pid() {
  echo "$2" > "$state/$1.pid"
  awk '{print $22}' "/proc/$2/stat" > "$state/$1.started"
}

stop_services() {
  for name in owner master; do
    if alive "$name"; then
      pid=$(cat "$state/$name.pid")
      kill -TERM "$pid"
      for ((i=0; i<100; i++)); do
        alive "$name" || break
        sleep 0.1
      done
      if alive "$name"; then
        echo "$name did not stop; inspect PID $pid before retrying" >&2
        return 1
      fi
    fi
    rm -f "$state/$name.pid" "$state/$name.started"
  done
}

start_services() {
  if ! alive master; then
    nohup "$master" --rpc_address=127.0.0.1 --rpc_port=50051 \
      --rpc_thread_num=2 --client_ttl=3600 \
      --put_start_discard_timeout_sec=3600 --put_start_release_timeout_sec=7200 \
      --default_kv_lease_ttl=10m --enable_metric_reporting=false \
      > "$state/master.log" 2>&1 < /dev/null &
    record_pid master "$!"
  fi
  ready=false
  for ((i=0; i<100; i++)); do
    if (echo > /dev/tcp/127.0.0.1/50051) 2>/dev/null; then
      ready=true
      break
    fi
    alive master || break
    sleep 0.1
  done
  if [[ "$ready" != true ]]; then
    tail -40 "$state/master.log"
    return 1
  fi
  if ! alive owner; then
    nohup "$demo" owner > "$state/owner.log" 2>&1 < /dev/null &
    record_pid owner "$!"
  fi
  for ((i=0; i<200; i++)); do
    if alive owner && grep -q OWNER_READY "$state/owner.log"; then
      echo "Master and owner ready. Logs: $state"
      return 0
    fi
    alive owner || break
    sleep 0.1
  done
  tail -40 "$state/owner.log"
  return 1
}

case "${1:-start}" in
  start) start_services ;;
  stop) stop_services ;;
  restart) stop_services; start_services ;;
  status)
    for name in master owner; do
      if alive "$name"; then
        echo "$name: running (PID $(cat "$state/$name.pid"))"
      else
        echo "$name: stopped"
      fi
    done ;;
  *) echo 'Usage: services.sh start|stop|restart|status' >&2; exit 2 ;;
esac
