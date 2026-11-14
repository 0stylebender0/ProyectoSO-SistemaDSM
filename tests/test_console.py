import configparser
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import sys
import tempfile
import time


node_binary, status_binary, config_path, demo_script = map(Path, sys.argv[1:])
processes = []


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def wait_for(condition, message, timeout=5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if condition():
            return
        time.sleep(0.02)
    raise AssertionError(message)


def start_node(directory, name, port=None, bootstrap=None):
    if port is None:
        port = free_port()
    args = [str(node_binary), "--config", str(directory / "dsm.conf"),
            "--name", name, "--tcp-port", str(port)]
    if bootstrap is not None:
        args += ["--bootstrap", f"127.0.0.1:{bootstrap}"]
    log_path = directory / f"{name}.log"
    with log_path.open("w") as log:
        process = subprocess.Popen(
            args,
            stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT, text=True,
        )
    processes.append(process)

    def ready():
        output = log_path.read_text()
        assert process.poll() is None, output
        return "comandos: nodes, memory, quit" in output

    wait_for(ready, f"El nodo {name} no inicio")
    return process, log_path, port


def members(port):
    response = subprocess.run(
        [str(status_binary), "127.0.0.1", str(port), "--nodes"],
        capture_output=True, text=True, timeout=3,
    )
    assert response.returncode == 0, response.stderr
    return {int(fields[0]): fields[1]
            for fields in (line.split("\t") for line in response.stdout.splitlines()[1:])}


def demo_port():
    for _ in range(50):
        base = free_port()
        if base > 65513:
            continue
        probes = []
        try:
            for offset in (0, 1, 2, 10, 11, 12, 20, 21, 22):
                probe = socket.socket()
                probes.append(probe)
                probe.bind(("127.0.0.1", base + offset))
            return base
        except OSError:
            continue
        finally:
            for probe in probes:
                probe.close()
    raise AssertionError("No se encontraron puertos disponibles para la demo")


def run_demo(port):
    process = subprocess.Popen(
        ["bash", str(demo_script), str(port)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        cwd=tempfile.gettempdir(), start_new_session=True,
    )
    try:
        stdout, stderr = process.communicate(timeout=20)
        return process.returncode, stdout, stderr
    finally:
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.communicate(timeout=3)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.communicate()


try:
    original_config = config_path.read_bytes()
    with tempfile.TemporaryDirectory() as directory:
        directory = Path(directory)
        config = configparser.ConfigParser()
        config.read(config_path)
        config["general"]["memory_size"] = "10000"
        config["general"]["page_size"] = "4096"
        config["network"]["bind_address"] = "127.0.0.1"
        config["discovery"]["bootstrap_nodes"] = ""
        with (directory / "dsm.conf").open("w") as file:
            config.write(file)

        process, log, port = start_node(directory, "consola")
        process.communicate("\n nodes \r\nmemory\ninvalido\nquit\nnodes\n", timeout=3)
        output = log.read_text()
        assert process.returncode == 0, output
        assert "[consola] nodos conocidos: 1" in output, output
        assert len(re.findall(r"^\s*\d+\s+consola\s+127\.0\.0\.1:" + str(port), output, re.M)) == 1
        assert output.count("10000 bytes /") == 3, output
        assert output.count("12288 bytes /") == 2, output
        assert "3 de 4096 bytes / 4 KB" in output, output
        assert "Comando desconocido: invalido" in output, output
        assert "[consola] detenido" in output, output

        for name, stop_signal in (("interrupcion", signal.SIGINT), ("parcial", signal.SIGTERM)):
            process, log, _ = start_node(directory, name)
            if name == "parcial":
                process.stdin.write("mem")
                process.stdin.flush()
                time.sleep(0.05)
            process.send_signal(stop_signal)
            assert process.wait(timeout=3) == 0, log.read_text()

        process, log, port = start_node(directory, "eof")
        process.stdin.write("memory")
        process.stdin.close()
        process.stdin = None
        wait_for(lambda: log.read_text().count("memoria reservada") == 2,
                 "No se proceso el ultimo comando sin salto de linea")
        assert process.poll() is None, log.read_text()
        status = subprocess.run([str(status_binary), "127.0.0.1", str(port)],
                                capture_output=True, text=True, timeout=3)
        assert status.returncode == 0 and "node_name=eof\n" in status.stdout, status.stderr
        process.send_signal(signal.SIGTERM)
        assert process.wait(timeout=3) == 0, log.read_text()

        process, log, _ = start_node(directory, "quit_sin_salto")
        process.communicate("quit", timeout=3)
        assert process.returncode == 0, log.read_text()

        a, a_log, a_port = start_node(directory, "A")
        b, _, b_port = start_node(directory, "B", bootstrap=a_port)
        c, _, c_port = start_node(directory, "C", bootstrap=a_port)
        wait_for(lambda: members(a_port) == members(b_port) == members(c_port)
                 and len(members(a_port)) == 3, "Los tres nodos no se descubrieron")

        for stop_signal in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP, None):
            old_id = next(node_id for node_id, name in members(a_port).items() if name == "A")
            if stop_signal is None:
                a.communicate("quit\n", timeout=3)
            else:
                a.send_signal(stop_signal)
                a.wait(timeout=3)
            assert a.returncode == 0, a_log.read_text()
            assert set(members(b_port).values()) == {"B", "C"}
            assert members(b_port) == members(c_port)
            assert old_id not in members(b_port)

            a, a_log, _ = start_node(directory, "A", port=a_port, bootstrap=b_port)
            wait_for(lambda: members(a_port) == members(b_port) == members(c_port)
                     and len(members(a_port)) == 3, "A no se reincorporo correctamente")
            assert old_id not in members(a_port), "Se recupero el registro del A anterior"

    base = demo_port()
    code, output, errors = run_demo(base)
    assert code == 0, output + errors
    assert output.count("Direccion") == 3, output
    assert output.count("memoria reservada") == 6, output
    assert output.count("] detenido") == 3, output
    assert "Demo completada" in output, output
    assert errors == "", errors

    with socket.socket() as occupied:
        occupied.bind(("127.0.0.1", base))
        occupied.listen()
        code, output, errors = run_demo(base)
        assert code != 0 and "Error iniciando red" in errors, output + errors

    assert config_path.read_bytes() == original_config, "La demo modifico dsm.conf"
    print("test_console: OK (comandos, senales, EOF, salida y reincorporacion de A, demo)")
finally:
    for process in processes:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        if process.stdin is not None and not process.stdin.closed:
            process.stdin.close()
