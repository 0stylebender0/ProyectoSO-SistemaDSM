#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
CXX=${CXX:-g++}
flags=(-std=c++17 -Wall -Wextra -Wpedantic -Iinclude -pthread)
mkdir -p build/manual_obj
sources=(dsm config memory_manager fault_queue signal_handler network heartbeat node pager dsm_node protocol status_report)
objects=()
for name in "${sources[@]}"; do
    echo "Compilando src/$name.cpp"
    "$CXX" "${flags[@]}" -c "src/$name.cpp" -o "build/manual_obj/$name.o"
    objects+=("build/manual_obj/$name.o")
done
rm -f build/libdsm.a
ar rcs build/libdsm.a "${objects[@]}"
build_exe() {
    local name=$1; shift
    echo "Generando build/$name"
    "$CXX" "${flags[@]}" "$@" build/libdsm.a -o "build/$name"
}
build_exe dsm_node src/main.cpp src/demo_app.cpp
build_exe dsm_app apps/dsm_app.cpp
build_exe dsm_validate apps/dsm_validate.cpp
build_exe dsm_array apps/dsm_array.cpp
build_exe dsm_matrix apps/dsm_matrix.cpp
build_exe dsm_status apps/status_client.cpp
for name in api global release heartbeat protocol network membership fault pager page_transfer msi concurrency; do
    build_exe "test_$name" "tests/test_$name.cpp"
done
echo 'Compilacion completada.'
