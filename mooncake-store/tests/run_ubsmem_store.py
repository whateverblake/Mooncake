"""Run the UBS pool owner, producer and consumer with a private master."""

import argparse
import os
import pathlib
import socket
import subprocess
import tempfile
import time
import urllib.request


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def wait_until(predicate, process, timeout=30):
    deadline = time.monotonic() + timeout
    while not predicate():
        if process.poll() is not None:
            raise RuntimeError(f"Process exited early: {process.returncode}")
        if time.monotonic() >= deadline:
            raise TimeoutError("Process did not become ready")
        time.sleep(0.05)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("master")
    parser.add_argument("client")
    parser.add_argument("--reject-policy", action="store_true")
    parser.add_argument("--tcp", action="store_true")
    parser.add_argument("--ownership-failure", action="store_true")
    args = parser.parse_args()
    session = pathlib.Path(tempfile.mkdtemp(prefix="mooncake-ubsmem-store-"))
    print(f"Logs and session: {session}", flush=True)
    port = free_port()
    endpoint = f"127.0.0.1:{port}"
    processes = []
    logs = []

    def start(name, command, env=None):
        log = (session / f"{name}.log").open("w")
        logs.append(log)
        process = subprocess.Popen(command, env=env, stdout=log, stderr=log)
        processes.append(process)
        return process

    def master_ready():
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                return True
        except OSError:
            return False

    try:
        metrics_port = free_port()
        master = start(
            "master",
            [
                args.master,
                "--rpc_address=127.0.0.1",
                f"--rpc_port={port}",
                f"--metrics_port={metrics_port}",
                "--rpc_thread_num=2",
                "--client_ttl=3600",
                "--default_kv_lease_ttl=10m",
                "--enable_metric_reporting=false",
                f"--eviction_ratio={0.1 if args.reject_policy else 0}",
                f"--eviction_high_watermark_ratio={0.9 if args.reject_policy else 1}",
            ],
        )
        wait_until(master_ready, master)
        if args.ownership_failure:
            fault = start("fault", [args.client, "fault", endpoint, str(session)])
            if fault.wait(timeout=30) != 0:
                raise RuntimeError("ownership failure test failed")
            print("UBSMEM_OWNERSHIP_FAILURE_OK")
            return
        if not args.tcp:
            role = "reject-master" if args.reject_policy else "boundaries"
            boundary = start(role, [args.client, role, endpoint, str(session)])
            if boundary.wait(timeout=30) != 0:
                raise RuntimeError(f"{role} failed")
            if args.reject_policy:
                print("UBSMEM_MASTER_POLICY_OK")
                return
        prefix = "tcp-" if args.tcp else ""
        provider_port = free_port()
        provider = start(
            "provider",
            [args.client, prefix + "provider", endpoint, str(session)],
            env={**os.environ, "MC_TE_HANDSHAKE_PORT": str(provider_port)},
        )
        wait_until(lambda: (session / "ready").exists(), provider)
        with urllib.request.urlopen(
            f"http://127.0.0.1:{metrics_port}/get_all_segments", timeout=5
        ) as response:
            names = response.read().decode().splitlines()
        expected = f"127.0.0.1:{provider_port}"
        if expected not in names:
            raise RuntimeError(f"Provider segment {expected} missing from {names}")
        for role in ("producer", "consumer"):
            client = start(role, [args.client, prefix + role, endpoint, str(session)])
            if client.wait(timeout=30) != 0:
                raise RuntimeError(f"{role} failed")
        (session / "stop").touch()
        if provider.wait(timeout=30) != 0:
            raise RuntimeError("provider shutdown failed")
        if list(session.glob("*.lease")):
            raise RuntimeError("quiescent pool was not removed")
        print("UBSMEM_STORE_THREE_PROCESS_OK")
    except Exception:
        for log in session.glob("*.log"):
            print(f"{log.name}:\n{log.read_text()[-6000:]}")
        raise
    finally:
        for process in reversed(processes):
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
        for log in logs:
            log.close()


if __name__ == "__main__":
    main()
