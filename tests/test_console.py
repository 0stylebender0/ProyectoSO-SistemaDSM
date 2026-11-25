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
        return f"[{name}] comandos:" in output

    wait_for(ready, f"El nodo {name} no inicio")
    process.stdin.write("info\n")
    process.stdin.flush()
    wait_for(lambda: "mmap base" in log_path.read_text(), f"El nodo {name} no mostro su informacion")
    return process, log_path, port


def members(port):
    response = subprocess.run(
        [str(status_binary), "127.0.0.1", str(port), "--nodes"],
        capture_output=True, text=True, timeout=3,
    )
    assert response.returncode == 0, response.stderr
    return {int(fields[0]): fields[1]
            for fields in (line.split("\t") for line in response.stdout.splitlines()[1:])}


def history_lines(port):
    response = subprocess.run(
        [str(status_binary), "127.0.0.1", str(port), "--history"],
        capture_output=True, text=True, timeout=3,
    )
    assert response.returncode == 0, response.stderr
    return [line for line in response.stdout.splitlines() if line.startswith("history=")]


def has_event(port, event, name):
    return any(f",{event}," in line and f",{name}," in line
               for line in history_lines(port))


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
        config["network"]["heartbeat_port"] = "0"
        config["discovery"]["bootstrap_nodes"] = ""
        config["heartbeat"]["interval_ms"] = "100"
        config["heartbeat"]["timeout_ms"] = "400"
        config["paging"]["request_timeout_ms"] = "200"
        with (directory / "dsm.conf").open("w") as file:
            config.write(file)

        process, log, port = start_node(directory, "consola")
        process.communicate("\n nodes \r\nmemory\nhistory\nstats\ninvalido\nquit\nnodes\n",
                            timeout=3)
        output = log.read_text()
        assert process.returncode == 0, output
        assert "[consola] nodos conocidos: 1" in output, output
        assert len(re.findall(r"^\s*\d+\s+consola\s+127\.0\.0\.1:" + str(port), output, re.M)) == 1
        assert output.count("10000 bytes /") == 3, output
        assert output.count("12288 bytes /") == 2, output
        assert "3 de 4096 bytes / 4 KB" in output, output
        assert "history_count=1" in output, output
        assert "history_nodes=consola" in output, output
        assert "pages_owned=0" in output, output
        assert "page_faults=0" in output, output
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
        history = subprocess.run([str(status_binary), "127.0.0.1", str(port), "--history"],
                                 capture_output=True, text=True, timeout=3)
        assert history.returncode == 0, history.stderr
        assert "history_count=1" in history.stdout, history.stdout
        assert "history_nodes=eof" in history.stdout, history.stdout
        pages = subprocess.run([str(status_binary), "127.0.0.1", str(port), "--pages"],
                               capture_output=True, text=True, timeout=3)
        assert pages.returncode == 0, pages.stderr
        assert "pages_total=0" in pages.stdout, pages.stdout
        stats = subprocess.run([str(status_binary), "127.0.0.1", str(port), "--stats"],
                               capture_output=True, text=True, timeout=3)
        assert stats.returncode == 0, stats.stderr
        assert "pages_owned=0" in stats.stdout, stats.stdout
        assert "page_faults=0" in stats.stdout, stats.stdout
        process.send_signal(signal.SIGTERM)
        assert process.wait(timeout=3) == 0, log.read_text()

        process, log, _ = start_node(directory, "quit_sin_salto")
        process.communicate("quit", timeout=3)
        assert process.returncode == 0, log.read_text()

        a, a_log, a_port = start_node(directory, "A")
        b, b_log, b_port = start_node(directory, "B", bootstrap=a_port)
        c, c_log, c_port = start_node(directory, "C", bootstrap=a_port)
        wait_for(lambda: members(a_port) == members(b_port) == members(c_port)
                 and len(members(a_port)) == 3, "Los tres nodos no se descubrieron")

        global_status = subprocess.run(
            [str(status_binary), "127.0.0.1", str(a_port), "--global"],
            capture_output=True, text=True, timeout=3,
        )
        assert global_status.returncode == 0, global_status.stdout + global_status.stderr
        for field in ("global_nodes_responded=3", "global_partial=no", "total_memory_configured=30000",
                      "total_memory_reserved=36864", "total_memory_pages_used=0", "total_memory_pages_free=9",
                      "total_pages_total=0", "total_page_faults=0"):
            assert field in global_status.stdout, global_status.stdout
        memory = subprocess.run(
            [str(status_binary), "127.0.0.1", str(a_port), "--memory"],
            capture_output=True, text=True, timeout=3,
        )
        assert memory.returncode == 0 and "memory_configured=10000\n" in memory.stdout, memory.stdout
        assert "memory_reserved=12288\n" in memory.stdout, memory.stdout
        for process, log in ((a, a_log), (b, b_log), (c, c_log)):
            process.stdin.write("nodes --active\nmemory   --global\npages --global\nstats --global\n")
            process.stdin.flush()
            wait_for(lambda: log.read_text().count("Consulta global: COMPLETA (3/3 nodos)") == 3,
                     "Los comandos globales no respondieron desde todos los nodos")
            wait_for(lambda: "nodos activos: 3" in log.read_text(),
                     "nodes --active no incluyo los tres nodos")

        history = subprocess.run([str(status_binary), "127.0.0.1", str(b_port), "--history"],
                                 capture_output=True, text=True, timeout=3)
        assert history.returncode == 0, history.stderr
        sequence = next(line.split("=", 1)[1] for line in history.stdout.splitlines()
                        if line.startswith("history_nodes="))
        assert {"A", "B", "C"} <= set(sequence.split(",")), history.stdout
        stats = subprocess.run([str(status_binary), "127.0.0.1", str(b_port), "--stats"],
                               capture_output=True, text=True, timeout=3)
        assert stats.returncode == 0, stats.stderr
        for field in ("pages_owned=0", "pages_shared=0", "page_faults=0",
                      "page_requests=0", "page_transfers=0", "bytes_sent=0",
                      "bytes_received=0", "invalidations_sent=0",
                      "invalidations_received=0"):
            assert field in stats.stdout, stats.stdout
        pages = subprocess.run([str(status_binary), "127.0.0.1", str(b_port), "--pages"],
                               capture_output=True, text=True, timeout=3)
        assert pages.returncode == 0, pages.stderr
        assert "pages_total=0" in pages.stdout, pages.stdout

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

        c.kill()
        c.wait(timeout=3)
        wait_for(lambda: has_event(a_port, "NODE_UNAVAILABLE", "C"),
                 "A no detecto la caida de C mediante heartbeat", timeout=5)
        offset = a_log.stat().st_size
        a.stdin.write("nodes --active\n")
        a.stdin.flush()
        wait_for(lambda: "nodos activos: 2" in a_log.read_text()[offset:],
                 "nodes --active no excluyo al nodo C no disponible")
        offset = a_log.stat().st_size
        a.stdin.write("nodes\n")
        a.stdin.flush()
        wait_for(lambda: "UNAVAILABLE" in a_log.read_text()[offset:],
                 "nodes no mostro el estado UNAVAILABLE de C")
        partial = subprocess.run(
            [str(status_binary), "127.0.0.1", str(a_port), "--global"],
            capture_output=True, text=True, timeout=3,
        )
        assert partial.returncode == 2, partial.stdout + partial.stderr
        for field in ("global_nodes_known=3", "global_nodes_responded=2", "global_partial=si",
                      "No consultado: C", "total_memory_configured=20000", "total_memory_reserved=24576"):
            assert field in partial.stdout, partial.stdout
        assert str(c_port) in partial.stdout, partial.stdout
        seed_down = subprocess.run(
            [str(status_binary), "127.0.0.1", str(c_port), "--global"],
            capture_output=True, text=True, timeout=3,
        )
        assert seed_down.returncode == 1, seed_down.stdout + seed_down.stderr
        b.send_signal(signal.SIGSTOP)
        try:
            os.waitpid(b.pid, os.WUNTRACED)
            wait_for(lambda: has_event(a_port, "NODE_UNAVAILABLE", "B"),
                     "A no detecto la pausa de B", timeout=5)
            offset = a_log.stat().st_size
            a.stdin.write("memory --global\n")
            a.stdin.flush()
            wait_for(lambda: "global_nodes_responded=1" in a_log.read_text()[offset:],
                     "La consulta global no termino con un participante sin responder")
            assert "No consultado: B" in a_log.read_text()[offset:], a_log.read_text()[offset:]
        finally:
            b.send_signal(signal.SIGCONT)
        wait_for(lambda: has_event(a_port, "NODE_RECOVERED", "B"),
                 "B no volvio a ACTIVE tras reanudarse", timeout=5)
        offset = a_log.stat().st_size
        a.stdin.write("nodes --active\n")
        a.stdin.flush()
        wait_for(lambda: "nodos activos: 2" in a_log.read_text()[offset:],
                 "nodes --active no recupero a B")

    base = demo_port()
    code, output, errors = run_demo(base)
    assert code == 0, output + errors
    assert output.count("app demo: OK") == 4, output
    assert "peers=2" in output, output
    assert "history_nodes=" in output, output
    assert "Demo completada: historial A, B, C, A" in output, output
    for field in ("global_partial=no", "total_pages_total=9", "total_pages_owned=3",
                  "total_memory_pages_used=9", "total_pages_shared=9"):
        assert field in output, output
    for name in ("A", "B", "C"):
        assert f"[{name}] memoria reservada" in output, output
    assert output.count("] detenido") == 4, output
    assert errors == "", errors

    with socket.socket() as occupied:
        occupied.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        occupied.bind(("127.0.0.1", base))
        occupied.listen()
        code, output, errors = run_demo(base)
        assert code != 0 and "Error iniciando red" in errors, output + errors

    assert config_path.read_bytes() == original_config, "La demo modifico dsm.conf"
    print("test_console: OK (comandos locales/globales, consulta parcial, timeout, senales, reincorporacion y demo)")
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
