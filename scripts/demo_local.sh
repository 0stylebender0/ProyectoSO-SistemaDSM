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
pids=()
inputs=()
names=(A B C)
ports=()

cleanup() {
    for pid in "${pids[@]}"; do kill -TERM "$pid" 2>/dev/null || true; done
    for pid in "${pids[@]}"; do wait "$pid" 2>/dev/null || true; done
    for input_fd in "${inputs[@]}"; do exec {input_fd}>&-; done
    rm -rf -- "$demo_dir"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

for i in 0 1 2; do
    name=${names[$i]}
    port=$((base_port + i * 10))
    ports+=("$port")
    sed -E \
        -e "s/^[[:space:]]*tcp_port[[:space:]]*=.*/tcp_port=$port/" \
        -e "s/^[[:space:]]*heartbeat_port[[:space:]]*=.*/heartbeat_port=$((port + 1))/" \
        -e "s/^[[:space:]]*control_port[[:space:]]*=.*/control_port=$((port + 2))/" \
        -e 's/^[[:space:]]*bind_address[[:space:]]*=.*/bind_address=127.0.0.1/' \
        -e 's/^[[:space:]]*bootstrap_nodes[[:space:]]*=.*/bootstrap_nodes=/' \
        "$project_dir/config/dsm.conf" > "$demo_dir/$name.conf"
    mkfifo "$demo_dir/$name.in"
    exec {input_fd}<>"$demo_dir/$name.in"
    inputs+=("$input_fd")
    args=(--config "$demo_dir/$name.conf" --name "$name")
    if ((i > 0)); then args+=(--bootstrap "127.0.0.1:$base_port"); fi
    "$node_binary" "${args[@]}" < "$demo_dir/$name.in" > "$demo_dir/$name.log" 2>&1 &
    pids+=("$!")

    ready=false
    for ((attempt = 0; attempt < 100; ++attempt)); do
        if ! kill -0 "${pids[$i]}" 2>/dev/null; then
            cat "$demo_dir/$name.log" >&2
            exit 1
        fi
        if grep -q 'comandos: nodes, memory, quit' "$demo_dir/$name.log"; then
            ready=true
            break
        fi
        sleep 0.05
    done
    if [[ $ready != true ]]; then
        echo "El nodo $name no inicio a tiempo" >&2
        cat "$demo_dir/$name.log" >&2
        exit 1
    fi
done

complete=false
for ((attempt = 0; attempt < 100; ++attempt)); do
    complete=true
    for i in 0 1 2; do
        if ! "$status_binary" 127.0.0.1 "${ports[$i]}" --nodes > "$demo_dir/nodes.$i" 2>/dev/null ||
           ! grep -qx 'NODE_LIST_RESPONSE (3 nodos)' "$demo_dir/nodes.$i"; then
            complete=false
        fi
    done
    if [[ $complete == true ]] && cmp -s "$demo_dir/nodes.0" "$demo_dir/nodes.1" &&
       cmp -s "$demo_dir/nodes.0" "$demo_dir/nodes.2"; then break; fi
    complete=false
    sleep 0.05
done
if [[ $complete != true ]]; then
    echo "Los tres nodos no llegaron a compartir la misma tabla" >&2
    exit 1
fi

for i in 0 1 2; do
    printf 'nodes\nmemory\n' >&"${inputs[$i]}"
done
# Todos consultan la tabla antes de que el primero anuncie su salida.
for i in 0 1 2; do
    ready=false
    for ((attempt = 0; attempt < 100; ++attempt)); do
        if [[ $(grep -c 'mmap base' "$demo_dir/${names[$i]}.log") -ge 2 ]]; then
            ready=true
            break
        fi
        sleep 0.05
    done
    if [[ $ready != true ]]; then
        echo "El nodo ${names[$i]} no respondio a los comandos" >&2
        exit 1
    fi
done
for i in 0 1 2; do
    printf 'quit\n' >&"${inputs[$i]}"
    wait "${pids[$i]}"
done
for i in 0 1 2; do
    printf '\nNodo %s (%s)\n' "${names[$i]}" "${ports[$i]}"
    cat "$demo_dir/${names[$i]}.log"
done
pids=()
echo "Demo completada: los tres nodos se descubrieron y finalizaron con quit."
