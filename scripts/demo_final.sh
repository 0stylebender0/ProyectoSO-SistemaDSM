#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
validate_binary="$project_dir/build/dsm_validate"
status_binary="$project_dir/build/dsm_status"
base_port=${1:-28400}

if [[ $# -gt 1 || ! $base_port =~ ^[0-9]{1,5}$ ]]; then
    echo "Uso: bash scripts/demo_final.sh [puerto_base]" >&2
    exit 1
fi
base_port=$((10#$base_port))
if ((base_port < 1 || base_port > 65513)); then
    echo "El puerto base debe estar entre 1 y 65513" >&2
    exit 1
fi
if [[ ! -x $validate_binary || ! -x $status_binary ]]; then
    echo "Primero compila: cmake -B build && cmake --build build -j" >&2
    exit 1
fi

demo_dir=$(mktemp -d)
declare -a all_pids=()
declare -A node_pid=()
declare -A node_port=()
declare -A log_path=()

cleanup() {
    for pid in "${all_pids[@]}"; do kill -TERM "$pid" 2>/dev/null || true; done
    for pid in "${all_pids[@]}"; do wait "$pid" 2>/dev/null || true; done
    rm -rf -- "$demo_dir"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

wait_for_pattern() {
    local label=$1 pattern=$2 timeout=${3:-15}
    local deadline=$((SECONDS + timeout))
    while ((SECONDS < deadline)); do
        if grep -Eq "$pattern" "${log_path[$label]}"; then
            return 0
        fi
        if ! kill -0 "${node_pid[$label]}" 2>/dev/null; then
            echo "El nodo $label termino antes de alcanzar el patron: $pattern" >&2
            cat "${log_path[$label]}" >&2
            return 1
        fi
        sleep 0.05
    done
    echo "El nodo $label no alcanzo el patron: $pattern" >&2
    cat "${log_path[$label]}" >&2
    return 1
}

start_node() {
    local label=$1 name=$2 port=$3 bootstrap=$4 regions=$5 peers=$6 hold_ms=$7
    local config="$demo_dir/$label.conf"
    log_path[$label]="$demo_dir/$label.log"
    node_port[$label]=$port

    sed -E \
        -e "s/^[[:space:]]*node_name[[:space:]]*=.*/node_name=$name/" \
        -e "s/^[[:space:]]*memory_size[[:space:]]*=.*/memory_size=16777216/" \
        -e "s/^[[:space:]]*tcp_port[[:space:]]*=.*/tcp_port=$port/" \
        -e "s/^[[:space:]]*heartbeat_port[[:space:]]*=.*/heartbeat_port=$((port + 1))/" \
        -e "s/^[[:space:]]*control_port[[:space:]]*=.*/control_port=$((port + 2))/" \
        -e 's/^[[:space:]]*bind_address[[:space:]]*=.*/bind_address=127.0.0.1/' \
        -e "s/^[[:space:]]*bootstrap_nodes[[:space:]]*=.*/bootstrap_nodes=$bootstrap/" \
        -e 's/^[[:space:]]*interval_ms[[:space:]]*=.*/interval_ms=300/' \
        -e 's/^[[:space:]]*timeout_ms[[:space:]]*=.*/timeout_ms=1500/' \
        -e 's/^[[:space:]]*worker_count[[:space:]]*=.*/worker_count=2/' \
        -e 's/^[[:space:]]*request_timeout_ms[[:space:]]*=.*/request_timeout_ms=3000/' \
        "$project_dir/config/dsm.conf" > "$config"
    # config/dsm.conf no fija node_name; cada nodo de la demo usa uno distinto.
    printf 'node_name=%s\n' "$name" >> "$config"

    "$validate_binary" "$config" \
        --regions "$regions" \
        --pages 4 \
        --threads 2 \
        --expect-nodes 4 \
        --expect-peers "$peers" \
        --membership-timeout-ms 20000 \
        --verify-timeout-ms 30000 \
        --hold-ms "$hold_ms" \
        --seed 424242 \
        > "${log_path[$label]}" 2>&1 &
    node_pid[$label]=$!
    all_pids+=("$!")
}

# A, B y C son duenos: cada uno crea una region, escribe 4 paginas con dos
# hilos y verifica las regiones de los otros. D solo observa y valida; por eso
# puede matarse sin perder datos. Todos permanecen activos durante la prueba
# de caida y reincorporacion.
start_node A1 A "$base_port" "" 1 2 12000
start_node B B "$((base_port + 10))" "127.0.0.1:$base_port" 1 2 12000
start_node C C "$((base_port + 20))" "127.0.0.1:$base_port" 1 2 12000
start_node D1 D "$((base_port + 30))" "127.0.0.1:$base_port" 0 3 12000

for label in A1 B C D1; do
    wait_for_pattern "$label" '^\[validate\] RESULT OK' 30
done
for label in A1 B C D1; do
    wait_for_pattern "$label" '^\[validate\] remote peers=' 30
done

# Recoleccion y comparacion de checksums de la primera fase.
declare -A expected_value=()
declare -A observations=()
total_initial=0
for label in A1 B C D1; do
    while read -r source region value; do
        if [[ -n ${expected_value[$region]+x} && ${expected_value[$region]} != "$value" ]]; then
            echo "Checksum distinto para la region $region en $label" >&2
            cat "${log_path[$label]}" >&2
            exit 1
        fi
        expected_value[$region]=$value
        observations[$region]=$(( ${observations[$region]:-0} + 1 ))
        total_initial=$((total_initial + 1))
    done < <(sed -nE \
        's/^\[checksum\] source=([a-z]+) region=([0-9]+).* value=(0x[0-9a-f]+).*/\1 \2 \3/p' \
        "${log_path[$label]}")
done

if ((total_initial != 12)); then
    echo "Se esperaban 12 checksums iniciales y se obtuvieron $total_initial" >&2
    for label in A1 B C D1; do cat "${log_path[$label]}" >&2; done
    exit 1
fi
for region in "${!expected_value[@]}"; do
    if (( ${observations[$region]} != 4 )); then
        echo "La region $region fue observada ${observations[$region]} veces, se esperaban 4" >&2
        exit 1
    fi
done

printf '\nFase 1: checksums coincidentes en los cuatro nodos\n'
for region in "${!expected_value[@]}"; do
    printf '  region=%s value=%s observaciones=%s\n' \
        "$region" "${expected_value[$region]}" "${observations[$region]}"
done

old_d_id=$(sed -nE 's/^\[validate\] node=.* id=([0-9]+) .*/\1/p' "${log_path[D1]}" | head -n1)
if [[ -z $old_d_id ]]; then
    echo "No se pudo obtener el NodeId inicial de D" >&2
    exit 1
fi
old_d_checksums=$(grep -c '^\[checksum\]' "${log_path[D1]}" || true)
if ((old_d_checksums != 3)); then
    echo "El observador D debio verificar tres regiones y reporto $old_d_checksums" >&2
    exit 1
fi

# Caida abrupta del observador. Los duenos conservan los datos.
printf '\nFase 2: SIGKILL al observador D (NodeId %s)\n' "$old_d_id"
kill -KILL "${node_pid[D1]}"
wait "${node_pid[D1]}" 2>/dev/null || true

wait_for_pattern A1 'nodo D UNAVAILABLE \(heartbeat\)' 10

deadline=$((SECONDS + 10))
detected=0
while ((SECONDS < deadline)); do
    "$status_binary" 127.0.0.1 "$base_port" --global --nodes \
        > "$demo_dir/global.after-kill" 2>&1 || true
    if grep -q '^global_nodes_unavailable=1$' "$demo_dir/global.after-kill"; then
        detected=1
        break
    fi
    sleep 0.1
done
if ((detected != 1)); then
    echo "Los nodos no marcaron a D como no disponible" >&2
    cat "$demo_dir/global.after-kill" >&2
    exit 1
fi
cat "$demo_dir/global.after-kill"

# Reincorporacion con un NodeId nuevo y los mismos puertos. D2 consulta las
# regiones de A, B y C y comprueba los mismos checksums.
printf '\nFase 3: reincorporacion del observador como D2\n'
start_node D2 D "$((base_port + 30))" "127.0.0.1:$base_port" 0 3 0
wait_for_pattern D2 '^\[validate\] RESULT OK' 30
if ! wait "${node_pid[D2]}"; then
    echo "D2 termino con error" >&2
    cat "${log_path[D2]}" >&2
    exit 1
fi

new_d_id=$(sed -nE 's/^\[validate\] node=.* id=([0-9]+) .*/\1/p' "${log_path[D2]}" | head -n1)
if [[ -z $new_d_id || $new_d_id == "$old_d_id" ]]; then
    echo "D no se reincorporo con un NodeId nuevo (antes $old_d_id, ahora '$new_d_id')" >&2
    cat "${log_path[D2]}" >&2
    exit 1
fi

rejoined=0
for region in "${!expected_value[@]}"; do
    value=$(sed -nE \
        "s/^\\[checksum\\] source=remote region=$region .* value=(0x[0-9a-f]+).*/\\1/p" \
        "${log_path[D2]}" | head -n1)
    if [[ $value != "${expected_value[$region]}" ]]; then
        echo "D2 no reprodujo el checksum de la region $region" >&2
        cat "${log_path[D2]}" >&2
        exit 1
    fi
    rejoined=$((rejoined + 1))
done
if ((rejoined != 3)); then
    echo "D2 no verifico las tres regiones" >&2
    exit 1
fi
printf '  NodeId nuevo=%s checksums verificados=%s\n' "$new_d_id" "$rejoined"

# A, B y C terminan normalmente al vencer su espera.
for label in A1 B C; do
    if ! wait "${node_pid[$label]}"; then
        echo "El nodo $label termino con error" >&2
        cat "${log_path[$label]}" >&2
        exit 1
    fi
done
for label in A1 B C; do
    if [[ $(grep -c '^\[validate\] RESULT OK' "${log_path[$label]}") -ne 1 ]]; then
        echo "El nodo $label no termino con exito" >&2
        cat "${log_path[$label]}" >&2
        exit 1
    fi
done
all_pids=()

printf '\nDemo final completada: 3 duenos, observador caido y reincorporado.\n'
printf 'Nodos iniciales: A, B, C, D. NodeId de D: %s -> %s.\n' "$old_d_id" "$new_d_id"
printf 'Checksums: %s regiones, todos coincidentes.\n' "${#expected_value[@]}"
