import re
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

array_binary, matrix_binary = [Path(arg) for arg in sys.argv[1:3]]


def free_ports(count):
    sockets = []
    try:
        for _ in range(count):
            probe = socket.socket()
            probe.bind(("127.0.0.1", 0))
            sockets.append(probe)
        return [probe.getsockname()[1] for probe in sockets]
    finally:
        for probe in sockets:
            probe.close()


def write_config(path, name, port, bootstrap="", addresses=""):
    path.write_text("\n".join([
        f"node_name={name}",
        "memory_size=8388608",
        "page_size=4096",
        "bind_address=127.0.0.1",
        f"tcp_port={port}",
        "heartbeat_port=0",
        f"bootstrap_nodes={bootstrap}",
        f"node_addresses={addresses}",
        "interval_ms=200",
        "timeout_ms=1000",
        "worker_count=2",
        "max_connections=32",
        "request_timeout_ms=2000",
        "",
    ]))


def region_line(path, timeout=20):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        match = re.search(r"^\[\w+\] region=(\d+) owner=(\d+)", path.read_text(), re.MULTILINE)
        if match:
            return match.group(1), match.group(2)
        time.sleep(0.05)
    raise AssertionError(f"No aparecio la region en {path}:\n{path.read_text()}")


def run_case(directory, ports, binary, create_args, attach_args, expected, tag):
    configs = [directory / f"{tag}-a.conf", directory / f"{tag}-b.conf"]
    # La primera semilla esta caida: node_addresses debe probar la siguiente.
    write_config(configs[0], "A", ports[0])
    write_config(configs[1], "B", ports[1], "",
                 f"127.0.0.1:1,127.0.0.1:{ports[0]}")
    logs = [directory / f"{tag}-a.log", directory / f"{tag}-b.log"]
    handles = [path.open("w") for path in logs]
    owner = subprocess.Popen(
        [str(binary), str(configs[0])] + create_args,
        stdout=handles[0], stderr=subprocess.STDOUT, text=True)
    try:
        region, owner_id = region_line(logs[0])
        command = [str(binary), str(configs[1]), attach_args[0],
                   region, owner_id] + attach_args[1:]
        attach = subprocess.Popen(
            command, stdout=handles[1], stderr=subprocess.STDOUT, text=True)
        results = []
        for process in (owner, attach):
            try:
                results.append(process.wait(timeout=25))
            except subprocess.TimeoutExpired:
                process.kill()
                raise AssertionError(f"{tag}: proceso no termino\n" + "\n".join(
                    path.read_text() for path in logs))
        assert results == [0, 0], "\n".join(path.read_text() for path in logs)
        owner_text, attach_text = logs[0].read_text(), logs[1].read_text()
        assert f"total={expected}" in owner_text or f"sum={expected}" in owner_text, owner_text
        assert f"total={expected}" in attach_text or f"sum={expected}" in attach_text, attach_text
        assert f"wrote={expected}" in attach_text, attach_text
        assert (f"final m00={expected}" in owner_text or
                f"final value0={expected}" in owner_text), owner_text
    finally:
        if owner.poll() is None:
            owner.kill()
        for handle in handles:
            handle.close()


def main():
    with tempfile.TemporaryDirectory() as temporary:
        directory = Path(temporary)
        ports = free_ports(4)
        run_case(directory, ports[:2], array_binary,
                 ["--create", "256", "4000"], ["--attach", "256"],
                 256 * 257 // 2, "array")
        run_case(directory, ports[2:], matrix_binary,
                 ["--create", "4", "8", "4000"], ["--attach", "4", "8"],
                 4 * 8 * (4 * 8 + 1) // 2, "matrix")


if __name__ == "__main__":
    main()
