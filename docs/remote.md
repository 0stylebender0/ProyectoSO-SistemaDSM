# Guía de despliegue multimáquina

Esta guía ejecuta la validación del hito 3.4 en computadoras separadas. El
sistema sigue siendo Linux x86-64, sin persistencia y sin recuperación de datos
cuando cae el dueño de una región.

## 1. Topología de ejemplo

| Nodo | IPv4 | TCP | Heartbeat UDP | Rol |
|---|---|---:|---:|---|
| A | 192.168.1.10 | 27400 | 27401 | Semilla y dueño |
| B | 192.168.1.11 | 27400 | 27401 | Dueño |
| C | 192.168.1.12 | 27400 | 27401 | Dueño |
| D | 192.168.1.13 | 27400 | 27401 | Observador para la prueba de caída |

Cada máquina puede usar los mismos números de puerto porque son hosts distintos.
Si dos máquinas comparten host o contenedor de red, use puertos distintos por
instancia.

## 2. Requisitos

- La misma revisión del proyecto y binarios compilados en todas las máquinas:
  ```sh
  ./build_manual.sh
  ```
- El mismo `page_size` en todos los nodos. La plantilla usa 4096.
- La misma versión de protocolo (6). Mezclar binarios de revisiones distintas
  no está soportado.
- IPv4 directa entre las máquinas. No se cubren NAT, túneles ni DNS.
- Conectividad TCP hacia el puerto semilla y UDP entre todos los participantes.

## 3. Puertos y firewall

Puertos de la plantilla:

| Puerto | Protocolo | Uso |
|---|---|---|
| 27400 | TCP | Descubrimiento, páginas, estado y consultas globales. |
| 27401 | UDP | Heartbeat de disponibilidad. |

No hay un puerto de control separado: los mensajes de control (JOIN,
NODE_ANNOUNCE, INVALIDATE, STATUS, etc.) y las transferencias de página
comparten `tcp_port`. Abra solamente los dos puertos de la tabla.

El nodo que recibe conexiones necesita TCP de entrada en `tcp_port`. Todos los
nodos necesitan UDP de entrada en `heartbeat_port` para que los demás detecten
su disponibilidad. Las conexiones salientes TCP y los datagramas UDP salientes
suelen estar permitidos por defecto.

Ejemplos:

```sh
# ufw (Ubuntu/Debian)
sudo ufw allow 27400/tcp
sudo ufw allow 27401/udp

# firewalld (Fedora/RHEL)
sudo firewall-cmd --permanent --add-port=27400/tcp
sudo firewall-cmd --permanent --add-port=27401/udp
sudo firewall-cmd --reload

# iptables
sudo iptables -A INPUT -p tcp --dport 27400 -j ACCEPT
sudo iptables -A INPUT -p udp --dport 27401 -j ACCEPT
```

Si cambia `--tcp-port`, también cambia por defecto `heartbeat_port` (`tcp+1`).
Abra el puerto UDP correspondiente.

Compruebe antes de arrancar:

```sh
ss -lntup | grep -E '27400|27401'
ping -c 1 192.168.1.10
```

## 4. Configuración

La plantilla es `config/remote-node.conf.template`:

```ini
[general]
node_name=@NODE_NAME@
memory_size=33554432
page_size=4096

[network]
bind_address=@BIND_ADDRESS@
tcp_port=@TCP_PORT@
heartbeat_port=@HEARTBEAT_PORT@

[discovery]
bootstrap_nodes=@BOOTSTRAP_NODES@
node_addresses=@NODE_ADDRESSES@

[heartbeat]
interval_ms=1000
timeout_ms=5000
```

Reglas importantes:

- `bind_address` debe ser la IPv4 de esta máquina que los demás pueden
  alcanzar, por ejemplo `192.168.1.10`. No use `0.0.0.0`: el nodo anunciaría
  `127.0.0.1` y los demás no podrían conectarse de vuelta.
- `bootstrap_nodes` lleva `IP:puerto_TCP` del nodo semilla. En el primer nodo
  queda vacío.
- `node_addresses` enumera todos los participantes (`IP:puerto_TCP`, separados
  por coma). Se prueban en orden como semillas junto con `bootstrap_nodes`: si
  la semilla elegida está caída, el nodo puede incorporarse por otra dirección.
  `scripts/demo_remote.sh --node-addresses` rellena este valor; si se omite,
  usa `--bootstrap`.
- `page_size` debe ser igual en todos los nodos.
- `heartbeat_port=0` pide un puerto UDP efímero. Funciona, pero complica el
  firewall; en el cluster fije 27401.
- `timeout_ms` y `interval_ms` controlan la detección de caídas. Con los valores
  de la plantilla, un nodo muerto pasa a `UNAVAILABLE` alrededor de
  `timeout_ms + interval_ms` (6 segundos).

## 5. Ejecución con el script

`scripts/demo_remote.sh` genera la configuración desde la plantilla y ejecuta
`build/dsm_validate`. Se lanza una instancia por máquina.

### Nodo semilla A

```sh
bash scripts/demo_remote.sh --name A --bind 192.168.1.10
```

### Nodos B y C

```sh
bash scripts/demo_remote.sh --name B --bind 192.168.1.11 \
    --bootstrap 192.168.1.10:27400

bash scripts/demo_remote.sh --name C --bind 192.168.1.12 \
    --bootstrap 192.168.1.10:27400
```

Por defecto cada nodo espera tres participantes, verifica dos pares con
regiones, usa cuatro páginas y dos hilos, y permanece cinco segundos activo
después de `RESULT OK`.

Opciones útiles:

```sh
--nodes 3              # participantes totales
--peers 2              # pares con regiones a verificar
--regions 0            # observador, no crea datos
--hold-ms 60000        # mantener el nodo vivo durante una prueba manual
--node-addresses A:27400,B:27400,C:27400
--tcp-port 28000       # cambiar puertos
--heartbeat-port 28001
--config-out /tmp/A.conf
```

La salida esperada en cada dueño incluye:

```text
[checksum] source=local region=... owner=... value=0x...
[checksum] source=remote region=... owner=... value=0x... peer=...
[validate] RESULT OK
```

Los valores de una misma región deben coincidir en todos los nodos. El código
de salida es 0 cuando la validación termina bien.

### Configuración manual

Si prefiere no usar el script:

```sh
sed \
  -e 's|@NODE_NAME@|A|g' \
  -e 's|@BIND_ADDRESS@|192.168.1.10|g' \
  -e 's|@TCP_PORT@|27400|g' \
  -e 's|@HEARTBEAT_PORT@|27401|g' \
  -e 's|@BOOTSTRAP_NODES@||g' \
  -e 's|@NODE_ADDRESSES@|192.168.1.10:27400,192.168.1.11:27400,192.168.1.12:27400|g' \
  config/remote-node.conf.template > /tmp/A.conf

./build/dsm_validate /tmp/A.conf --pages 4 --threads 2 \
    --expect-nodes 3 --expect-peers 2 --seed 424242
```

En B y C cambie el nombre, la IP y `bootstrap_nodes=192.168.1.10:27400`.

## 6. Prueba de caída y reincorporación

Use un observador para no perder datos. En la cuarta máquina:

```sh
bash scripts/demo_remote.sh --name D --bind 192.168.1.13 \
    --bootstrap 192.168.1.10:27400 \
    --regions 0 --peers 3 --hold-ms 60000
```

Mantenga A, B y C activos con `--hold-ms 60000` durante la prueba. Cuando D
haya impreso `RESULT OK`, mate el proceso de forma abrupta:

```sh
pkill -9 -f 'dsm_validate.*D.conf'
```

Espere unos segundos y consulte desde A:

```sh
./build/dsm_status 192.168.1.10 27400 --global --nodes
```

La salida debe ser parcial y parecida a:

```text
global_nodes_known=4
global_nodes_responded=3
global_nodes_unavailable=1
global_partial=si
```

Reincorpore D con el mismo comando. El proceso genera un `NodeId` nuevo,
descubre las regiones de A, B y C y vuelve a imprimir sus checksums. El nodo
caído permanece en la tabla como `UNAVAILABLE`; el nuevo aparece como un
participante distinto.

No mate un dueño si espera conservar su región. Esta entrega no recupera datos
de un dueño caído.

## 7. Diagnóstico

| Síntoma | Causa probable | Acción |
|---|---|---|
| `Configuracion invalida: page_size...` | Tamaños de página distintos o no múltiplos del SO. | Iguale `page_size` a 4096. |
| Todos los nodos se llaman `node1`. | El archivo no define `node_name`. | Añada `node_name=...` o use el script. |
| `NODE_UNAVAILABLE` en todos los pares. | UDP bloqueado o `heartbeat_port` incorrecto. | Abra UDP de entrada y revise el firewall. |
| `Connection refused` al volver a un nodo. | El proceso ya terminó o el puerto es incorrecto. | Mantenga el nodo con `--hold-ms` y revise `ss`. |
| Los pares no descubren la región. | El nodo aún no la creó o usa otro `page_size`. | Espere; `dsm_validate` reintenta hasta `--verify-timeout-ms`. |
| Un nodo remoto anuncia `127.0.0.1`. | `bind_address=0.0.0.0` en un cluster. | Use la IPv4 real de la máquina. |
| Error al hacer `bind`. | Puerto ocupado por otra instancia. | Cambie `--tcp-port` y los puertos asociados. |

Las consultas globales usan cinco segundos por participante y son secuenciales;
varios nodos inaccesibles pueden acumular ese tiempo. El heartbeat evita abrir
TCP contra nodos que ya marcó como `UNAVAILABLE`.

### Informe HTML desde cualquiera de las máquinas

```sh
cd /tmp
./ruta/al/build/dsm_status 192.168.1.10 27400 --global --pages --stats --html
ls dsm-global-192.168.1.10-27400.html
```

`--html` escribe el archivo en el directorio actual. El nombre incluye la IP y
el puerto consultados, por lo que se pueden generar informes de varios nodos en
la misma máquina sin sobrescribirlos. `--open` abre ese archivo con
`xdg-open` (en una estación con escritorio). Para generar varios informes,
repita el comando con la dirección de cada participante.
