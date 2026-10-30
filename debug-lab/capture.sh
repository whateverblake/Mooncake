#!/usr/bin/env bash
# Run as container root; only tcpdump needs that privilege.
set -euo pipefail
state=/workspace/build/lab-run
port=$(sed -n 's/.*TcpTransport: listen on port \([0-9]*\).*/\1/p' "$state/owner.log" | tail -1)
[[ "$port" =~ ^[0-9]+$ ]] || { echo 'Cannot find owner TCP data port' >&2; exit 1; }
tcpdump -Z debugger -U -i lo -s 0 -w "$state/tcp.pcap" "tcp port $port" \
  > "$state/tcpdump.log" 2>&1 &
capture_pid=$!
cleanup() {
  kill -INT "$capture_pid" 2>/dev/null || true
  wait "$capture_pid" || true
}
trap cleanup EXIT
ready=false
for ((i=0; i<50; i++)); do
  if grep -q 'listening on' "$state/tcpdump.log"; then ready=true; break; fi
  kill -0 "$capture_pid" || break
  sleep 0.1
done
[[ "$ready" == true ]] || { cat "$state/tcpdump.log"; exit 1; }
runuser -u debugger -- bash /workspace/mooncake/debug-lab/verify.sh
cleanup
trap - EXIT
tcpdump -nn -A -r "$state/tcp.pcap" > "$state/tcp.txt" 2>/dev/null
python3 - "$state/tcp.pcap" "$port" <<'PY'
import pathlib
import struct
import sys

# Linux loopback captures use Ethernet headers. Check both payload directions,
# rather than mistaking master/handshake traffic or retransmits for put + get.
data = pathlib.Path(sys.argv[1]).read_bytes()
endian = "<" if data[:4] == b"\xd4\xc3\xb2\xa1" else ">"
assert struct.unpack_from(endian + "I", data, 20)[0] == 1, "Expected Ethernet pcap"
port = int(sys.argv[2])
offset = 24
directions = set()
while offset + 16 <= len(data):
    length = struct.unpack_from(endian + "I", data, offset + 8)[0]
    packet = data[offset + 16:offset + 16 + length]
    offset += 16 + length
    if len(packet) < 54 or packet[12:14] != b"\x08\x00" or packet[23] != 6:
        continue
    tcp = 14 + (packet[14] & 15) * 4
    source, destination = struct.unpack_from("!HH", packet, tcp)
    payload = packet[tcp + (packet[tcp + 12] >> 4) * 4:]
    if b"MOONCAKE_TCP_DEBUG_PAYLOAD" in payload:
        if destination == port:
            directions.add("WRITE to owner")
        if source == port:
            directions.add("READ from owner")
assert directions == {"WRITE to owner", "READ from owner"}, directions
print(f"TCP_CAPTURE_OK data_port={port}: WRITE to owner + READ from owner")
PY
