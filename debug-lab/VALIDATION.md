# Validation — 2026-09-17

Source: `719735896c86b56fabec6cf3e825fb2ea640597a` (`v0.3.13.post1`).
Lab changes are on local branch `lab/tcp-debug`.

## Passed

- Ubuntu 24.04 image built natively for Linux AArch64.
- `mooncake_master` and `store_debug_demo` built with Debug symbols and no
  stripping. Final incremental build reports `ninja: no work to do`.
- SSH key authentication works as `debugger` on `127.0.0.1:33333`. Root password
  login was subsequently enabled at the user's request; see the update below.
- Separate-process 4096-byte put/get returns identical data.
- Replica endpoint matches the running memory owner's actual handshake endpoint.
- Missing-key Get returns `OBJECT_NOT_FOUND`; Remove followed by Query confirms
  object cleanup.
- TCP capture contains the payload marker in both directions on the owner's
  data port: `TCP_CAPTURE_OK`. Capture reported 16 packets, zero kernel drops.
- GDB over SSH hit `Client::Put`, `TcpTransport::submitTransferTask`, and
  `TcpTransport::startTransfer` for WRITE and READ; variables and source stacks
  were visible. The GDB verification script exited zero with `GDB_TCP_OK`.
- GDB attached to the already-running owner as the same non-root SSH user and
  resolved `ServerSession::readHeader` at its source line, then detached.
- Stopping services, restarting the Docker container, and starting services
  again produced another successful roundtrip and cleanup.
- A stale-PID test confirmed that service stop does not signal an unrelated
  build-directory process.
- Shell syntax, Python syntax, Compose validation, XML parsing, and
  `git diff --check` passed. Repository pre-commit was not available on this Mac.

## CLion status

Installed the named `Mooncake Docker` remote toolchain/SSH configuration,
`Mooncake Debug` CMake profile, deployment mapping, and `Mooncake roundtrip` /
`Mooncake owner` run configurations. Existing application settings were backed
up before edits. CLion was closed during installation.

**CLion UI detection and breakpoint validation remain unverified:** Computer
Use was denied macOS Accessibility/Screen Recording permissions. The underlying
SSH connection, Linux GDB launch, breakpoint hits, and owner attachment were
verified independently. Open the project and follow README's CLion steps.

## Adjustments justified by source and execution

- Compile with `USE_HTTP=ON` to avoid the pinned release's incomplete-type build
  failure when it is OFF. Runtime still uses `P2PHANDSHAKE` without an HTTP
  metadata service.
- Treat `127.0.0.1:12345` / `:12346` as logical client names. P2P mode allocates
  actual handshake ports dynamically, and TCP payload ports are separate again.
- Break at `TcpTransport::submitTransferTask`; the Store's actual dispatch goes
  through `MultiTransport`, bypassing `TcpTransport::submitTransfer`.

Local evidence is retained in `debug-lab/.local/`: `build.log`, `rebuild.log`,
`verify.log`, `capture.log`, `gdb.log`, `owner-attach.log`, `restart-verify.log`,
`tcp.pcap`, and `tcp.txt`. This ignored directory also holds the local SSH key
and settings backups; it is excluded from the Docker build context.

## SSH port update

Host SSH port changed to `33333` to avoid conflicts with other containers.
Compose recreated `mooncake-debug`; SSH login on the new port and the 4096-byte
roundtrip, missing-key, placement, and cleanup checks all passed. See
`debug-lab/.local/port-33333-verify.log`. The CLion installer uses the new port;
CLion was running during this update, so its active connection was not edited.

## Root SSH update

Enabled root password authentication at the user's request and verified an SSH
password login on port `33333` executes as UID 0. The existing `debugger` SSH key
login also passed. Reloaded sshd in place, leaving the lab processes running.
The development image was rebuilt with the matching SSH policy, and Compose
mounts the ignored local password file for the entrypoint to apply on recreation.

## Ccache — 2026-09-18

- Installed ccache 4.9.1 in the running container and rebuilt
  `mooncake-debug:arm64` with ccache included.
- Configured C/C++ compiler launchers in both `/workspace/build` and CLion's
  existing `/tmp/tmp.uG0XLKRb1t/mooncake/cmake-build-debug-remote-host` cache.
  Reconfiguration succeeded; the generated master compilation recipe invokes
  `/usr/bin/ccache /usr/bin/c++`.
- SSH commands inherit the compiler-launcher defaults through PAM's
  `/etc/environment`. A fresh CMake C/C++ probe over SSH used both launchers;
  recompiling the probe produced direct cache hits and its executable passed.
- Root and debugger successfully shared `/workspace/build/ccache`. Effective
  configuration reports `max_size = 10.0 GB`, set in the persistent cache config
  as well as the image's `/etc/ccache.conf`. Startup reapplies the limit.
- A fresh temporary container from the rebuilt image mounted the existing build
  volume, retained the cache statistics and 10 GB limit, and produced another
  direct cache hit. The running lab container was not recreated.
- Shell/Python syntax, Compose configuration, and whitespace checks passed.
  Pre-commit is not installed on the Mac. The full Mooncake build was not rerun
  for this compiler-launcher configuration change.
