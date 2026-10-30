"""Run inside the debug container: python3 test_configured_ports.py /workspace/build.

Uses an isolated master and owners; never stops existing debugging processes.
"""
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest

BUILD = Path(sys.argv.pop(1)) if len(sys.argv) > 1 else Path("/workspace/build")


def reserve_port():
    sock = socket.socket()
    sock.bind(("0.0.0.0", 0))
    return sock


def free_port():
    with reserve_port() as sock:
        return sock.getsockname()[1]


def listening_ports(pid):
    inodes = set()
    for fd in Path(f"/proc/{pid}/fd").iterdir():
        try:
            target = os.readlink(fd)
        except FileNotFoundError:
            continue
        if target.startswith("socket:["):
            inodes.add(target[8:-1])
    ports = set()
    for table in ("tcp", "tcp6"):
        for line in Path(f"/proc/{pid}/net/{table}").read_text().splitlines()[1:]:
            fields = line.split()
            if fields[3] == "0A" and fields[9] in inodes:
                ports.add(int(fields[1].split(":")[1], 16))
    return ports


class ConfiguredPortsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.env = dict(os.environ, GLOG_logtostderr="1", MC_FORCE_TCP="1")
        for key in ("MC_TE_HANDSHAKE_PORT", "MC_TCP_DATA_PORT"):
            cls.env.pop(key, None)
        cls.master_port = free_port()
        cls.master_log = tempfile.TemporaryFile(mode="w+")
        cls.master = subprocess.Popen([
            str(BUILD / "mooncake-store/src/mooncake_master"),
            "--rpc_address=127.0.0.1", f"--rpc_port={cls.master_port}",
            f"--metrics_port={free_port()}", "--rpc_thread_num=2",
            "--enable_ha=false", "--enable_metric_reporting=false",
        ], env=cls.env, stdout=cls.master_log, stderr=subprocess.STDOUT)
        cls.addClassCleanup(cls.stop, cls.master, cls.master_log)
        for _ in range(100):
            if cls.master.poll() is not None:
                raise RuntimeError("Temporary master exited")
            if cls.master_port in listening_ports(cls.master.pid):
                return
            time.sleep(0.1)
        raise RuntimeError("Temporary master did not start")

    @staticmethod
    def stop(process, log):
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        log.close()

    def owner(self, settings, should_start=True):
        service_port = free_port()
        log = tempfile.TemporaryFile(mode="w+")
        process = subprocess.Popen([
            str(BUILD / "mooncake-store/src/mooncake_client"),
            "--host=127.0.0.1:12345", f"--port={service_port}",
            f"--master_server_address=127.0.0.1:{self.master_port}",
            "--metadata_server=P2PHANDSHAKE", "--protocol=tcp",
            "--global_segment_size=16MB", "--local_buffer_size=0",
            "--enable_offload=false", "--logtostderr=true",
        ], env=dict(self.env, **settings), stdout=log, stderr=subprocess.STDOUT)
        self.addCleanup(self.stop, process, log)
        self.last_owner = (process, log)
        for _ in range(100):
            if process.poll() is not None:
                log.seek(0)
                output = log.read()
                if should_start:
                    self.fail(output[-4000:])
                self.assertNotEqual(process.returncode, 0)
                return output
            try:
                ports = listening_ports(process.pid)
            except FileNotFoundError:
                continue  # The process exited between poll() and /proc inspection.
            if service_port in ports:
                self.assertTrue(
                    should_start, "Owner started despite invalid/occupied port"
                )
                log.seek(0)
                return ports - {service_port}, log.read()
            time.sleep(0.1)
        self.fail("Owner did not finish startup within 10 seconds")

    def test_exact_ports(self):
        handshake, data = free_port(), free_port()
        while data == handshake:
            data = free_port()
        ports, output = self.owner({
            "MC_TE_HANDSHAKE_PORT": str(handshake),
            "MC_TCP_DATA_PORT": str(data),
        })
        self.assertEqual(ports, {handshake, data})
        self.assertIn(f"listening on 127.0.0.1:{handshake}", output)
        self.assertIn(f"TcpTransport: listen on port {data}", output)

    def test_unset_ports_are_automatic(self):
        ports, _ = self.owner({"MC_MIN_RPC_PORT": "18000", "MC_MAX_RPC_PORT": "18100"})
        self.assertEqual(len(ports), 2)
        self.assertTrue(all(18000 <= port <= 18100 for port in ports))

    def test_fixed_handshake_with_automatic_data(self):
        handshake = free_port()
        ports, _ = self.owner({
            "MC_TE_HANDSHAKE_PORT": str(handshake),
            "MC_MIN_RPC_PORT": "18000", "MC_MAX_RPC_PORT": "18100",
        })
        self.assertIn(handshake, ports)
        remaining = ports - {handshake}
        self.assertEqual(len(remaining), 1)
        self.assertTrue(18000 <= remaining.pop() <= 18100)

    def test_fixed_data_port_is_excluded_from_auto_handshake(self):
        # A two-port range exposes accidental selection of the reserved data port.
        for _ in range(10):
            ports, _ = self.owner({
                "MC_MIN_RPC_PORT": "18200",
                "MC_MAX_RPC_PORT": "18201",
                "MC_TCP_DATA_PORT": "18200",
            })
            self.assertEqual(ports, {18200, 18201})
            self.stop(*self.last_owner)

    def test_restart_after_metadata_exchange(self):
        handshake, data = free_port(), free_port()
        while handshake == data:
            data = free_port()
        settings = {
            "MC_TE_HANDSHAKE_PORT": str(handshake),
            "MC_TCP_DATA_PORT": str(data),
        }
        self.owner(settings)
        with socket.create_connection(("127.0.0.1", handshake), timeout=2) as peer:
            peer.sendall(struct.pack("=Q", 3) + b"\x01{}")
            response = b""
            while chunk := peer.recv(4096):
                response += chunk
            self.assertIn(b"tcp_data_port", response)
        self.stop(*self.last_owner)
        ports, _ = self.owner(settings)
        self.assertEqual(ports, {handshake, data})

    def test_invalid_ports_fail(self):
        for key in ("MC_TE_HANDSHAKE_PORT", "MC_TCP_DATA_PORT"):
            for value in ("", "0", "-1", "65536", "16001abc", "999999999999999999999"):
                with self.subTest(key=key, value=value):
                    output = self.owner({key: value}, should_start=False)
                    self.assertIn(key, output)

    def test_occupied_ports_fail_without_fallback(self):
        for key in ("MC_TE_HANDSHAKE_PORT", "MC_TCP_DATA_PORT"):
            for listening in (False, True):
                with (
                    self.subTest(key=key, listening=listening),
                    reserve_port() as occupied,
                ):
                    if listening:
                        occupied.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                        occupied.listen()
                    output = self.owner(
                        {key: str(occupied.getsockname()[1])}, should_start=False
                    )
                    self.assertIn(key, output)

    def test_same_port_fails(self):
        port = str(free_port())
        self.owner(
            {"MC_TE_HANDSHAKE_PORT": port, "MC_TCP_DATA_PORT": port},
            should_start=False,
        )


if __name__ == "__main__":
    unittest.main()
