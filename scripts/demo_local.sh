#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
node_binary="$project_dir/build/dsm_node"
status_binary="$project_dir/build/dsm_status"
base_port=${1:-27400}

if [[ $# -gt 1 || ! $base_port =~ ^[0-9]{1,5}$ ]]; then
    echo "Uso: bash scripts/demo_local.sh [puerto_base]" >&2
    exit 1
fi
base_port=$((10#$base_port))
if ((base_port < 1 || base_port > 65513)); then
    echo "El puerto base debe estar entre 1 y 65513" >&2
    exit 1
fi
if [[ ! -x $node_binary || ! -x $status_binary ]]; then
    echo "Primero compila: cmake -B build && cmake --build build -j" >&2
    exit 1
fi

demo_dir=$(mktemp -d)
declare -a all_pids=()
declare -A input_fd=()
declare -A log_path=()
declare -A node_pid=()
declare -A node_port=()

cleanup() {
    for pid in "${all_pids[@]}"; do kill -TERM "$pid" 2>/dev/null || true; done
    for pid in "${all_pids[@]}"; do wait "$pid" 2>/dev/null || true; done
    for fd in "${input_fd[@]}"; do exec {fd}>&- 2>/dev/null || true; done
    rm -rf -- "$demo_dir"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

wait_for_pattern() {
    local label=$1 pattern=$2 timeout=${3:-10}
    local deadline=$((SECONDS + timeout))
    while ((SECONDS < deadline)); do
        if ! kill -0 "${node_pid[$label]}" 2>/dev/null; then
            echo "El nodo $label termino antes de tiempo" >&2
            cat "${log_path[$label]}" >&2
            return 1
        fi
        if grep -Eq "$pattern" "${log_path[$label]}"; then
            return 0
        fi
        sleep 0.05
    done
    echo "El nodo $label no alcanzo el patron: $pattern" >&2
    cat "${log_path[$label]}" >&2
    return 1
}

wait_for_members() {
    local port=$1 expected=$2 timeout=${3:-10}
    local deadline=$((SECONDS + timeout))
    while ((SECONDS < deadline)); do
        local output
        output=$("$status_binary" 127.0.0.1 "$port" --nodes 2>/dev/null || true)
        local count
        count=$(grep -c '^[0-9]' <<<"$output" || true)
        if [[ $count -eq $expected ]]; then
            return 0
        fi
        sleep 0.05
    done
    echo "El nodo en el puerto $port no alcanzo $expected participantes" >&2
    return 1
}

start_node() {
    local label=$1 name=$2 port=$3 bootstrap=$4 use_app=$5
    local config="$demo_dir/$label.conf"
    local fifo="$demo_dir/$label.in"
    log_path[$label]="$demo_dir/$label.log"
    node_port[$label]=$port

    sed -E \
        -e "s/^[[:space:]]*tcp_port[[:space:]]*=.*/tcp_port=$port/" \
        -e "s/^[[:space:]]*heartbeat_port[[:space:]]*=.*/heartbeat_port=$((port + 1))/" \
        -e "s/^[[:space:]]*control_port[[:space:]]*=.*/control_port=$((port + 2))/" \
        -e 's/^[[:space:]]*bind_address[[:space:]]*=.*/bind_address=127.0.0.1/' \
        -e 's/^[[:space:]]*bootstrap_nodes[[:space:]]*=.*/bootstrap_nodes=/' \
        "$project_dir/config/dsm.conf" > "$config"

    mkfifo "$fifo"
    local fd
    exec {fd}<>"$fifo"
    input_fd[$label]=$fd

    local args=(--config "$config" --name "$name" --tcp-port "$port")
    if [[ -n $bootstrap ]]; then args+=(--bootstrap "$bootstrap"); fi
    if [[ $use_app == 1 ]]; then args+=(--demo-app); fi

    "$node_binary" "${args[@]}" < "$fifo" > "${log_path[$label]}" 2>&1 &
    node_pid[$label]=$!
    all_pids+=("$!")

    wait_for_pattern "$label" "\[$name\] comandos:" 10
}

assert_status() {
    local label=$1
    local port=${node_port[$label]}
    local output="$demo_dir/status.$label"

    "$status_binary" 127.0.0.1 "$port" --history --pages --stats > "$output"
    grep -q '^history_count=' "$output"
    grep -q '^history_nodes=' "$output"
    grep -q '^pages_total=3$' "$output"
    grep -q '^pages_owned=1$' "$output"
    grep -Eq '^page_transfers=[1-9]' "$output"
    grep -Eq '^bytes_sent=[1-9]' "$output"
    grep -Eq '^bytes_received=[1-9]' "$output"
}

# Tres nodos en localhost. Cada uno crea una region, escribe un patron y
# verifica las regiones de los otros dos.
start_node A1 A "$base_port" "" 1
start_node B B "$((base_port + 10))" "127.0.0.1:$base_port" 1
start_node C C "$((base_port + 20))" "127.0.0.1:$base_port" 1

if ! wait_for_members "$base_port" 3 15; then
    echo "Los tres nodos no se descubrieron" >&2
    exit 1
fi
for label in A1 B C; do
    wait_for_pattern "$label" 'app demo: OK .*peers=2' 25
done
for label in A1 B C; do
    assert_status "$label"
done

# Consultas administrativas desde la consola local de cada nodo.
for label in A1 B C; do
    printf 'info\nnodes\nmemory\npages\nhistory\nstats\n' >&"${input_fd[$label]}"
done
for label in A1 B C; do
    wait_for_pattern "$label" 'estadisticas' 5
done

# A sale y vuelve a incorporarse con un NodeId nuevo. El historial de B debe
# conservar la secuencia de participantes A, B, C y registrar la nueva A.
printf 'quit\n' >&"${input_fd[A1]}"
wait "${node_pid[A1]}"
if ! wait_for_members "${node_port[B]}" 2 10; then
    echo "B y C no retiraron a A" >&2
    exit 1
fi

start_node A2 A "$base_port" "127.0.0.1:${node_port[B]}" 1
wait_for_pattern A2 'app demo: OK .*peers=2' 25
if ! wait_for_members "${node_port[B]}" 3 10; then
    echo "La nueva A no se incorporo" >&2
    exit 1
fi

history_b=$("$status_binary" 127.0.0.1 "${node_port[B]}" --history)
names_line=$(sed -n 's/^history_nodes=//p' <<<"$history_b")
padded=",$names_line,"
for name in A B C; do
    if [[ $padded != *",$name,"* ]]; then
        echo "El historial de B no contiene a $name: $names_line" >&2
        exit 1
    fi
done
count_a=0
IFS=',' read -r -a sequence <<<"$names_line"
for name in "${sequence[@]}"; do
    if [[ $name == A ]]; then count_a=$((count_a + 1)); fi
done
if ((count_a < 2)); then
    echo "El historial de B no registra la reincorporacion de A: $names_line" >&2
    exit 1
fi

for label in A2 B C; do
    printf 'nodes\nhistory\nstats\nquit\n' >&"${input_fd[$label]}"
done
for label in A2 B C; do
    wait "${node_pid[$label]}"
done

for label in A1 A2 B C; do
    printf '\nNodo %s (%s)\n' "$label" "${node_port[$label]}"
    cat "${log_path[$label]}"
done
all_pids=()
printf 'Demo completada: historial A, B, C, A (%s).\n' "$names_line"
