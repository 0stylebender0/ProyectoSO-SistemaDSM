#!/usr/bin/env bash
set -uo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
root=$PWD
passed=0; failed=0; skipped=0
run() {
    local name=$1 seconds=$2; shift 2
    printf '\n=== %s ===\n' "$name"
    if timeout "$seconds" "$@"; then
        echo "PASS: $name"; ((passed+=1))
    else
        echo "FAIL: $name"; ((failed+=1))
    fi
}
run api 60 "$root/build/test_api" "$root/build/dsm_app"
run global 25 "$root/build/test_global"
run release 30 "$root/build/test_release"
run heartbeat 30 "$root/build/test_heartbeat"
run protocol 60 "$root/build/test_protocol"
run network 20 "$root/build/test_network"
run membership 30 "$root/build/test_membership"
run fault 20 "$root/build/test_fault"
run pager 20 "$root/build/test_pager"
run page_transfer 20 "$root/build/test_page_transfer"
run msi 40 "$root/build/test_msi"
run concurrency 60 "$root/build/test_concurrency"
if command -v python3 >/dev/null; then
    run console 40 python3 "$root/tests/test_console.py" "$root/build/dsm_node" "$root/build/dsm_status" "$root/config/dsm.conf" "$root/scripts/demo_local.sh"
    run validation 60 python3 "$root/tests/test_validate.py" "$root/build/dsm_validate"
    run apps 60 python3 "$root/tests/test_apps.py" "$root/build/dsm_array" "$root/build/dsm_matrix"
else
    echo 'SKIP: console, validation, apps (Python 3 no disponible)'; skipped=3
fi
run final_demo 60 bash "$root/scripts/demo_final.sh"
printf '\nPASS: %s | FAIL: %s | SKIP: %s | Total: 16\n' "$passed" "$failed" "$skipped"
((failed == 0 && skipped == 0))
