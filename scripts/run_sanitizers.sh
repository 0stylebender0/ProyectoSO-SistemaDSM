#!/usr/bin/env bash
# Ejecuta la suite DSM con sanitizers en un directorio de build separado.
#
#   scripts/run_sanitizers.sh                 # ASan + UBSan, suite completa
#   scripts/run_sanitizers.sh address,undefined
#   scripts/run_sanitizers.sh thread          # TSan selectivo
#   scripts/run_sanitizers.sh thread build-tsan
#
# TSan no se ejecuta sobre las pruebas que usan el manejador SIGSEGV y la pila
# alternativa del DSM: los interceptores de TSan y la senal compiten por el
# mismo contexto. ASan/UBSan si cubren toda la suite.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mode="${1:-address,undefined}"
build="${2:-$root/build-sanitize-${mode//,/-}}"
jobs="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}"

echo "[sanitizers] modo=${mode} build=${build}"

cmake -S "$root" -B "$build" \
    -DDSM_SANITIZERS="$mode" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build "$build" -j "$jobs"

export ASAN_OPTIONS="detect_leaks=1:abort_on_error=1:strict_string_checks=1"
export LSAN_OPTIONS="report_objects=1"
export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1"
export TSAN_OPTIONS="halt_on_error=1:second_deadlock_stack=1"

if [[ "$mode" == *thread* ]]; then
    # Pruebas sin SignalHandler/SIGSEGV. La cobertura de red, heartbeat,
    # protocolo y consultas globales sigue siendo representativa.
    ctest --test-dir "$build" --output-on-failure \
        -R '^(protocol|heartbeat|global|network|membership)$'
else
    ctest --test-dir "$build" --output-on-failure
fi

echo "[sanitizers] OK (${mode})"
