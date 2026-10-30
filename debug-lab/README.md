# Mooncake TCP source-debugging lab

This lab uses CLion on macOS and Linux GDB in an ARM64 Ubuntu 24.04 container.
It is pinned to Mooncake `v0.3.13.post1`, commit
`719735896c86b56fabec6cf3e825fb2ea640597a`. Changes live on `lab/tcp-debug`.
It exercises CPU memory and the original Transfer Engine (`USE_TENT=OFF`).

## Start here

From `/Users/blake/mooncake` on the Mac:

```bash
./debug-lab/lab.sh status
./debug-lab/lab.sh verify
./debug-lab/lab.sh capture
```

For a new installation, start Docker Desktop, clone this pinned source with
submodules, and save your chosen root password in `debug-lab/.local/root_password`
(create the directory first and set the file permissions to `600`). This local
credential file is ignored by Git and excluded from Docker builds.
Then run `./debug-lab/lab.sh setup`. That creates an SSH key, builds
the image and the two executable targets, starts master and owner, and verifies
the roundtrip. The first build downloads dependencies and takes several minutes.

| Command | Purpose |
| --- | --- |
| `./debug-lab/lab.sh up` | Start the persistent SSH container |
| `./debug-lab/lab.sh build` | Configure and compile with two jobs |
| `./debug-lab/lab.sh start` | Start master and owner if absent |
| `./debug-lab/lab.sh restart` | Restart both services, clearing in-memory data |
| `./debug-lab/lab.sh verify` | Run the 4 KiB roundtrip and error/cleanup checks |
| `./debug-lab/lab.sh capture` | Verify TCP payload traffic in both directions |
| `./debug-lab/lab.sh shell` | Open an SSH terminal as `debugger` |
| `./debug-lab/lab.sh stop` | Stop master and owner; retain SSH access |
| `./debug-lab/lab.sh down` | Stop services and remove the container; retain build volume |

After `down`, run `up` and `start` to resume. The container has an 8 GiB limit.
Build artifacts stay in the `mooncake-debug-lab_build` Docker volume; source
edits are immediately shared through the bind mount.

## What runs where

```text
macOS CLion -- SSH localhost:33333 --> Ubuntu container, Linux GDB

                        master (127.0.0.1:50051)
                       /                       \
                PutStart/PutEnd, Query      MountSegment, heartbeat
                     /                           \
       roundtrip client <--- TCP payload ---> memory owner
       127.0.0.1:12346                         127.0.0.1:12345
       4 KiB source + destination              64 MiB storage segment
```

The master stores metadata and allocates replicas. Object bytes live in the
owner process. The roundtrip client calls `RegisterLocalMemory` for its buffers,
but never `MountSegment`, so it contributes no storage capacity. Its sole
replica must belong to the owner. `MC_STORE_MEMCPY=0` forces the transfer path.

**12345/12346 are logical names passed to the client.** In this release,
`P2PHANDSHAKE` replaces their ports with dynamically allocated handshake ports.
`OWNER_READY endpoint=...` reports the actual owner endpoint, and verification
compares the replica against it. TCP payload ports are allocated separately;
find `TcpTransport: listen on port` in the process logs. `capture.sh` reads the
owner's data port automatically. Filtering only port 50051 or 12345 would not
show the object's data transfer.

All networking between these processes stays in Linux loopback. The only
published port is SSH on macOS `127.0.0.1:33333`. There is no RDMA device or GPU
requirement and no separate etcd/Redis/HTTP metadata service.

Root SSH password login is also enabled at the user's request:

```bash
ssh -p 33333 root@127.0.0.1
```

The entrypoint applies the password from the mounted local credential file on
startup. The existing `debugger` key-based connection remains available for
CLion. Root access does not change the loopback-only SSH port binding.

## CLion configuration

With CLion closed, `python3 debug-lab/install_clion.py` installs settings for
CLion 2026.1 without replacing existing SSH connections or toolchains. Original
application settings are backed up under `debug-lab/.local/clion-backup`.
This installer is specific to this Mac's CLion version; settings can also be
entered manually using the table below.

1. Open `/Users/blake/mooncake` in CLion as a CMake project.
2. In Settings → Build, Execution, Deployment → Toolchains, select
   **Mooncake Docker** and let CLion detect the remote tools.
3. In CMake settings, select **Mooncake Debug**, using that toolchain.
4. Select **Mooncake roundtrip** in the run/debug dropdown.
   Its **Before launch → Build** step compiles `store_debug_demo` and its
   dependencies inside Docker before each Run/Debug.
5. Set a breakpoint at the `client->Put(...)` call in `store_debug_demo.cpp`.
6. Ensure `lab.sh start` succeeded, then press Debug. Step into Put or use the
   function breakpoints below. The demo exits after verifying and cleaning up.

| Setting | Value |
| --- | --- |
| SSH host / port / user | `127.0.0.1` / `33333` / `debugger` |
| Authentication | Key pair; no passphrase |
| Private key | `/Users/blake/mooncake/debug-lab/.local/id_ed25519` |
| CMake / Ninja | `/usr/bin/cmake` / `/usr/bin/ninja` |
| C / C++ compiler | `/usr/bin/gcc` / `/usr/bin/g++` |
| Debugger | `/usr/bin/gdb` inside Linux |
| Environment script | `/workspace/mooncake/debug-lab/env.sh` |
| Local source | `/Users/blake/mooncake` |
| Remote source | `/workspace/mooncake` |
| Deployment type | Local or mounted folder; no source synchronization |
| Build directory | `/workspace/build` |
| Demo executable | `/workspace/build/debug-lab/store_debug_demo` |
| Demo arguments | `roundtrip` |

### Fixed Transfer Engine ports

For the native `mooncake_client` owner with `--metadata_server=P2PHANDSHAKE`
and `--protocol=tcp`, add these values under **Run → Edit Configurations →
Environment variables**:

```text
MC_TE_HANDSHAKE_PORT=16001;MC_TCP_DATA_PORT=16002
```

`MC_TE_HANDSHAKE_PORT` selects the P2P metadata/handshake listener.
`MC_TCP_DATA_PORT` selects the TCP payload listener. These settings apply to the
classic Transfer Engine used by this lab (`USE_TENT=OFF`). The owner's
`--port=50052` still selects its separate client-service RPC listener.

For `store_debug_demo highlevel`, use different ports in that run configuration:

```text
MC_TE_HANDSHAKE_PORT=16003;MC_TCP_DATA_PORT=16004
```

Rebuild and restart each client to apply source changes. Each configured value
must be a decimal number from 1 to 65535. Invalid values or unavailable ports
fail startup; there is no automatic fallback. Do not assign the same port to
two listeners. Set these variables per run configuration, not in the shared
toolchain environment used by every client.

Leaving either variable unset keeps automatic selection for that listener,
using `MC_MIN_RPC_PORT` and `MC_MAX_RPC_PORT` (default: 15000–17000).
Explicit ports are independent of that automatic-selection range.

To run the port regression checks from the Mac terminal:

```bash
docker exec mooncake-debug python3 \
  /workspace/mooncake/debug-lab/test_configured_ports.py /workspace/build
```

The checks start and stop their own temporary master and owner processes.

The toolchain type is **Remote Host (SSH)**; "Mooncake Docker" is its display
name. CLion owns CMake configuration and Ninja builds through this toolchain.
Use **Build → Build Project** or select the `store_debug_demo` / `mooncake_master`
target and build it in CLion. Run/Debug builds the selected executable first.
The terminal `lab.sh build` command is an optional alternative.

The CMake options are maintained in `configure.sh`; the installer reads the
same flags. If the target is missing, reload CMake with `BUILD_DEBUG_LAB=ON`.
Keep the container, master, and owner running while building/debugging the
roundtrip client. After rebuilding a running master or owner, restart those
processes to load the new binaries. Do not launch a
second owner while the managed owner is already running.

For owner-side debugging, use Run → Attach to Process with the remote
toolchain, and select `store_debug_demo owner`. To launch the owner directly
under CLion instead, stop only the managed owner using its PID from `status`,
leave the master running, and launch **Mooncake owner**. Run the roundtrip from
an SSH terminal while that debug session is active.

## Compiler cache

The image includes ccache. Both root and debugger use one shared cache at
`/workspace/build/ccache` in the persistent Compose build volume. The cache has
a **10 GB disk limit**, configured in `ccache.conf` and reapplied by the
entrypoint on every container start. Container recreation preserves the cache;
deleting the build volume removes it.

The SSH environment and `env.sh` supply CMake compiler-launcher defaults.
`configure.sh` and the generated CLion profile also set these explicitly:

```text
-DENABLE_SCCACHE=OFF
-DCMAKE_C_COMPILER_LAUNCHER=/usr/bin/ccache
-DCMAKE_CXX_COMPILER_LAUNCHER=/usr/bin/ccache
```

For an existing CMake profile, add those options and reload CMake if its cache
was created before ccache was installed. Keep Before launch → Build enabled
and use two build jobs. Ccache preserves the Debug compiler flags and symbols.

Check the effective configuration and statistics from the Mac:

```bash
docker exec mooncake-debug ccache --show-config
docker exec mooncake-debug ccache --show-stats
```

The first compilation populates the cache; equivalent later compilations can
hit it. An unchanged incremental build may do no compilation and leave cache
statistics unchanged. Do not clean the whole project just to populate ccache.

## Reading and breakpoint route

Start with the small demo, then follow the real implementation in this order:

| Layer | Source under repository root | Useful functions |
| --- | --- | --- |
| Store API | `mooncake-store/src/client_service.cpp` | `Client::Put`, `Query`, `Get`, `TransferWrite`, `TransferRead` |
| Master RPC client | `mooncake-store/src/master_client.cpp` | `PutStart`, `PutEnd`, `GetReplicaList` |
| Master metadata | `mooncake-store/src/master_service.cpp` | `MasterService::PutStart`, `PutEnd`, `GetReplicaList` |
| Transfer construction | `mooncake-store/src/transfer_task.cpp` | `TransferSubmitter::submit`, `submitTransferEngineOperation`, `submitTransfer` |
| Transfer Engine | `mooncake-transfer-engine/src/transfer_engine.cpp` | `TransferEngine::submitTransfer` |
| Transport routing | `mooncake-transfer-engine/src/multi_transport.cpp` | `MultiTransport::submitTransfer` |
| TCP scheduling | `mooncake-transfer-engine/src/transport/tcp_transport/tcp_transport.cpp` | `TcpTransport::submitTransferTask`, `startTransfer` |
| TCP socket handlers | `mooncake-transfer-engine/src/transport/tcp_transport/tcp_transport_session_impl.h` | `ClientSession::writeHeader`, `ServerSession::readHeader`, body handlers |

Put performs allocation through `PutStart`, writes object bytes to the owner,
then reports completion through `PutEnd`. Get queries replica metadata and
submits a READ from the owner's memory into the destination buffer.
The observed route goes through `MultiTransport::submitTransfer` and
`TcpTransport::submitTransferTask`; the older `TcpTransport::submitTransfer`
entry point is not called by this demo.

Inspect `key`, `slices`, the replica descriptor, `requests`, and `*slice`.
Compare `transport_endpoint_`, `buffer_address_`, `opcode`, `length`, and the
completion status. The remote address is meaningful only in the owner process.
TCP runs asynchronously on worker threads, so stepping only the calling thread
will not enter the socket handlers: place function/source breakpoints there.
Leave GDB scheduler locking off so background workers can run.

To verify function breakpoints without the IDE:

```bash
docker exec -u debugger mooncake-debug gdb -q -batch \
  -x /workspace/mooncake/debug-lab/tcp-breakpoints.gdb \
  /workspace/build/debug-lab/store_debug_demo
```

This prints Store and TCP breakpoint hits, local data, and stacks for both
operations, then continues automatically. For interactive exploration, use
`lab.sh shell`, then `gdb --args /workspace/build/debug-lab/store_debug_demo roundtrip`.

## Logs, capture, and timeouts

Logs are in `/workspace/build/lab-run/` inside the container:
`master.log`, `owner.log`, `tcpdump.log`, `tcp.pcap`, and decoded `tcp.txt`.
For example:

```bash
docker exec mooncake-debug tail -40 /workspace/build/lab-run/owner.log
docker cp mooncake-debug:/workspace/build/lab-run/tcp.pcap debug-lab/.local/tcp.pcap
```

The packet check looks for `MOONCAKE_TCP_DEBUG_PAYLOAD` going to and from the
owner's TCP data port. The demo compares all 4096 bytes and checks missing-key
and deletion behavior; successful completion exits with status zero.

Master client lifetime and TCP status/progress timeouts are extended for the
lab. The Store's `TransferEngineOperationState::wait_for_completion` contains a hard-coded 60-second
deadline. A debugger suspends execution, not wall-clock time. Pausing while a
transfer or RPC is in flight can cause timeout on resume, even if all threads
are stopped. Pause before Put/Get for long reading sessions; rerun after an
in-flight timeout. No upstream timeout logic has been patched.

### Build deviation from the original plan

`USE_HTTP=OFF` fails in this pinned release: `master_service.cpp` calls methods
on `MetadataStoragePlugin` while its defining header is guarded by `USE_HTTP`.
The lab therefore builds with `USE_HTTP=ON`. It still selects `P2PHANDSHAKE` at
runtime and starts no HTTP metadata service. All other planned transport and
Debug settings are retained.

### IDE verification limitation

The toolchain/profile/run configuration files can be installed without UI
access. Confirm their detection and a breakpoint in CLion on first open;
macOS Computer Use permissions are required only if you want the assistant to
perform that UI check. SSH/GDB breakpoint checks exercise the Linux debugger
independently of those macOS permissions.
