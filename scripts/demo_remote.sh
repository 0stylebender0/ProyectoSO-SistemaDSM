#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
validate_binary=${DSM_VALIDATE_BINARY:-"$project_dir/build/dsm_validate"}
template="$project_dir/config/remote-node.conf.template"

name=""
bind_address=""
bootstrap=""
node_addresses=""
tcp_port=27400
heartbeat_port=""
nodes=3
peers=2
regions=1
pages=4
threads=2
hold_ms=5000
seed=20261126
config_out=""

usage() {
    cat <<EOF
Uso: bash scripts/demo_remote.sh --name <nombre> --bind <IPv4> [opciones]

Ejecuta un nodo de validacion DSM en esta maquina. Se debe lanzar una
instancia por maquina, cambiando --name y --bind; solo el nodo semilla
omite --bootstrap.

  --name <nombre>          Nombre del nodo (obligatorio)
  --bind <IPv4>            IP de esta maquina alcanzable por los demas (obligatorio)
  --bootstrap <host:puerto> Nodo semilla; vacio en el primer nodo
  --node-addresses <lista>  Participantes host:puerto separados por coma
                            (default: el bootstrap)
  --tcp-port <n>           Puerto TCP (default 27400)
  --heartbeat-port <n>     Puerto UDP; 0 = efimero (default tcp+1)
  --nodes <n>              Participantes totales esperados (default 3)
  --peers <n>              Pares con regiones que se deben verificar (default 2)
  --regions <n>            Regiones propias; 0 para observador (default 1)
  --pages <n>              Paginas por region (default 4)
  --threads <n>            Hilos de escritura y verificacion (default 2)
  --hold-ms <n>            Permanecer activo tras exito (default 5000)
  --seed <n>               Semilla del patron (default 20261126)
  --config-out <ruta>      Guardar la configuracion generada en esa ruta
  --help                   Muestra esta ayuda

Ejemplo en tres maquinas (192.168.1.10, .11 y .12):
  # Maquina .10 (semilla)
  bash scripts/demo_remote.sh --name A --bind 192.168.1.10

  # Maquinas .11 y .12
  bash scripts/demo_remote.sh --name B --bind 192.168.1.11 --bootstrap 192.168.1.10:27400
  bash scripts/demo_remote.sh --name C --bind 192.168.1.12 --bootstrap 192.168.1.10:27400
EOF
}

require_value() {
    if (($# < 2)); then
        echo "Falta el valor de $1" >&2
        exit 1
    fi
}

while (($#)); do
    case $1 in
        --name) require_value "$@"; name=$2; shift 2 ;;
        --bind) require_value "$@"; bind_address=$2; shift 2 ;;
        --bootstrap) require_value "$@"; bootstrap=$2; shift 2 ;;
        --node-addresses) require_value "$@"; node_addresses=$2; shift 2 ;;
        --tcp-port) require_value "$@"; tcp_port=$2; shift 2 ;;
        --heartbeat-port) require_value "$@"; heartbeat_port=$2; shift 2 ;;
        --nodes) require_value "$@"; nodes=$2; shift 2 ;;
        --peers) require_value "$@"; peers=$2; shift 2 ;;
        --regions) require_value "$@"; regions=$2; shift 2 ;;
        --pages) require_value "$@"; pages=$2; shift 2 ;;
        --threads) require_value "$@"; threads=$2; shift 2 ;;
        --hold-ms) require_value "$@"; hold_ms=$2; shift 2 ;;
        --seed) require_value "$@"; seed=$2; shift 2 ;;
        --config-out) require_value "$@"; config_out=$2; shift 2 ;;
        --help|-h) usage; exit 0 ;;
        *) echo "Opcion desconocida: $1" >&2; usage >&2; exit 1 ;;
    esac
done

if [[ -z $name || -z $bind_address ]]; then
    echo "--name y --bind son obligatorios" >&2
    usage >&2
    exit 1
fi
if ! [[ $name =~ ^[A-Za-z0-9_-]+$ ]]; then
    echo "El nombre solo puede contener letras, digitos, '-' y '_'" >&2
    exit 1
fi
if ! [[ $bind_address =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}$ ]]; then
    echo "bind debe ser una direccion IPv4: $bind_address" >&2
    exit 1
fi
if [[ $bind_address == 0.0.0.0 || $bind_address == 255.255.255.255 ]]; then
    echo "En un cluster remoto bind_address debe ser la IP real de esta maquina" >&2
    exit 1
fi
for value in "$tcp_port" "$nodes" "$peers" "$regions" "$pages" "$threads" "$hold_ms"; do
    if ! [[ $value =~ ^[0-9]+$ ]]; then
        echo "Valor numerico invalido: $value" >&2
        exit 1
    fi
done
if ((10#$tcp_port < 1 || 10#$tcp_port > 65535)); then
    echo "tcp-port fuera de rango: $tcp_port" >&2
    exit 1
fi
if [[ -z $heartbeat_port ]]; then
    heartbeat_port=$((10#$tcp_port + 1))
fi
if ! [[ $heartbeat_port =~ ^[0-9]+$ ]] || ((10#$heartbeat_port < 0 || 10#$heartbeat_port > 65535)); then
    echo "Puerto fuera de rango: $heartbeat_port" >&2
    exit 1
fi
if ((10#$nodes < 1 || 10#$peers < 0 || 10#$regions < 0 ||
     10#$pages < 1 || 10#$threads < 1)); then
    echo "Los valores de nodos, pares, regiones, paginas e hilos no son validos" >&2
    exit 1
fi
if [[ -n $bootstrap ]] && ! [[ $bootstrap =~ ^[A-Za-z0-9._-]+:[0-9]+$ ]]; then
    echo "bootstrap debe tener el formato host:puerto: $bootstrap" >&2
    exit 1
fi
if [[ -z $node_addresses ]]; then
    node_addresses=$bootstrap
fi
if [[ ! -x $validate_binary ]]; then
    echo "No se encontro dsm_validate: $validate_binary" >&2
    echo "Compile con: ./build_manual.sh" >&2
    exit 1
fi
if [[ ! -f $template ]]; then
    echo "No se encontro la plantilla: $template" >&2
    exit 1
fi

workdir=""
if [[ -z $config_out ]]; then
    workdir=$(mktemp -d)
    trap 'rm -rf -- "$workdir"' EXIT
    config_out="$workdir/$name.conf"
else
    mkdir -p -- "$(dirname -- "$config_out")"
fi

sed \
    -e "s|@NODE_NAME@|$name|g" \
    -e "s|@BIND_ADDRESS@|$bind_address|g" \
    -e "s|@TCP_PORT@|$tcp_port|g" \
    -e "s|@HEARTBEAT_PORT@|$heartbeat_port|g" \
    -e "s|@BOOTSTRAP_NODES@|$bootstrap|g" \
    -e "s|@NODE_ADDRESSES@|$node_addresses|g" \
    "$template" > "$config_out"

printf 'Nodo %s: tcp=%s heartbeat_udp=%s\n' \
    "$name" "$tcp_port" "$heartbeat_port"
if [[ -n $bootstrap ]]; then
    printf 'Semilla: %s\n' "$bootstrap"
else
    printf 'Semilla: este nodo (sin bootstrap)\n'
fi
printf 'Configuracion: %s\n' "$config_out"
printf 'Apertura requerida en firewall: TCP %s y UDP %s de entrada\n\n' \
    "$tcp_port" "$heartbeat_port"

set +e
"$validate_binary" "$config_out" \
    --regions "$regions" \
    --pages "$pages" \
    --threads "$threads" \
    --expect-nodes "$nodes" \
    --expect-peers "$peers" \
    --membership-timeout-ms 30000 \
    --verify-timeout-ms 30000 \
    --hold-ms "$hold_ms" \
    --seed "$seed"
status=$?
set -e
exit "$status"
