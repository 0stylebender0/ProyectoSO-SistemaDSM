import re
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path


validate_binary = Path(sys.argv[1])


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


def write_config(path, name, port, bootstrap=""):
    path.write_text(
        "\n".join([
            f"node_name={name}",
            "memory_size=8388608",
            "page_size=4096",
            "bind_address=127.0.0.1",
            f"tcp_port={port}",
            "heartbeat_port=0",
            "control_port=65000",
            f"bootstrap_nodes={bootstrap}",
            "interval_ms=200",
            "timeout_ms=1000",
            "worker_count=2",
            "max_connections=32",
            "max_pending_faults=128",
            "request_timeout_ms=2000",
            "",
        ])
    )


def checksums(text):
    pattern = re.compile(
        r"^\[checksum\] source=(\w+) region=(\d+) owner=(\d+) "
        r"pages=(\d+) bytes=(\d+) value=(0x[0-9a-f]+)",
        re.MULTILINE,
    )
    return [match.groups() for match in pattern.finditer(text)]


def main():
    with tempfile.TemporaryDirectory() as temporary:
        directory = Path(temporary)
        ports = free_ports(3)
        seed_port = ports[0]
        processes = []

        # Tres duenos: cada uno crea una region, escribe con dos hilos y
        # verifica localmente y en los otros dos nodos.
        for index, name in enumerate(["A", "B", "C"]):
            config = directory / f"{name}.conf"
            bootstrap = "" if index == 0 else f"127.0.0.1:{seed_port}"
            write_config(config, name, ports[index], bootstrap)
            log_path = directory / f"{name}.log"
            log = log_path.open("w")
            process = subprocess.Popen(
                [
                    str(validate_binary), str(config),
                    "--regions", "1",
                    "--pages", "3",
                    "--threads", "2",
                    "--expect-nodes", "3",
                    "--expect-peers", "2",
                    "--membership-timeout-ms", "15000",
                    "--verify-timeout-ms", "25000",
                    "--seed", "99",
                ],
                stdout=log,
                stderr=subprocess.STDOUT,
                text=True,
            )
            processes.append((name, process, log, log_path))

        deadline = time.monotonic() + 40
        outputs = {}
        try:
            for name, process, log, log_path in processes:
                remaining = max(0.1, deadline - time.monotonic())
                try:
                    result = process.wait(timeout=remaining)
                except subprocess.TimeoutExpired:
                    process.kill()
                    raise AssertionError(f"El nodo {name} no termino a tiempo\n{log_path.read_text()}")
                log.close()
                text = log_path.read_text()
                output = text
                assert result == 0, f"El nodo {name} fallo con codigo {result}\n{output}"
                assert "[validate] RESULT OK" in output, output
                assert "[validate] remote peers=2" in output, output
                outputs[name] = output
        finally:
            for _, process, log, _ in processes:
                if process.poll() is None:
                    process.kill()
                if not log.closed:
                    log.close()

        values = {}
        local_counts = {}
        remote_counts = {}
        for name, output in outputs.items():
            entries = checksums(output)
            assert len(entries) == 3, f"{name} no reporto tres checksums\n{output}"
            local_counts[name] = sum(1 for entry in entries if entry[0] == "local")
            remote_counts[name] = sum(1 for entry in entries if entry[0] == "remote")
            assert local_counts[name] == 1, f"{name} no reporto su checksum local"
            assert remote_counts[name] == 2, f"{name} no verifico dos pares"
            for _, region, owner, pages, size, value in entries:
                assert pages == "3" and size == str(3 * 4096), output
                values.setdefault(region, set()).add(value)

        assert len(values) == 3, f"Se esperaban tres regiones: {values}"
        for region, region_values in values.items():
            assert len(region_values) == 1, (
                f"Los checksums de la region {region} no coinciden: {region_values}")
        print("Validacion local y remota con checksums coincidentes: OK")


if __name__ == "__main__":
    main()
