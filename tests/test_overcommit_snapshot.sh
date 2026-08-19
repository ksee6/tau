#!/bin/bash
set -e

TAU_BIN="/home/xi/Repo/tau/build/tau"
TEST_DIR="/tmp/tau_overcommit_test_$$"
mkdir -p "$TEST_DIR"

cleanup() {
    sudo "$TAU_BIN" stop test_inst 2>/dev/null || true
    sudo "$TAU_BIN" stop test_freeze 2>/dev/null || true
    rm -rf "$TEST_DIR"
}
trap cleanup EXIT

echo "=== Test 1: Overcommitment Manifest Parsing & Directives ==="
cat <<EOF > "$TEST_DIR/overcommit.yml"
- name: test_inst
- description: Overcommitment test instance

- memory:
    limit: 64M
    max: 256M

- cpu:
    quota: 50
    max: 100

- spawn:
    command: "sleep 100"
    wait: true
    name: worker
EOF

echo "[+] Starting detached overcommit instance..."
sudo "$TAU_BIN" run -d "$TEST_DIR/overcommit.yml"
sleep 1

echo "[+] Verifying listing with memory and cpu limits..."
LS_OUT=$(sudo "$TAU_BIN" ls -m -c test_inst)
echo "$LS_OUT"
echo "$LS_OUT" | grep -q "max_memory: 256M" || { echo "FAIL: max_memory not found"; exit 1; }
echo "$LS_OUT" | grep -q "max_cpu: 100%" || { echo "FAIL: max_cpu not found"; exit 1; }

echo "=== Test 2: Snapshot and Delta Snapshot ==="
echo "[+] Taking first snapshot..."
SNAP1_OUT=$(sudo "$TAU_BIN" snapshot test_inst)
echo "$SNAP1_OUT"

INST_DIR="/run/user/$(id -u)/tau/instances/test_inst"
[ ! -d "$INST_DIR" ] && INST_DIR="/run/tau/instances/test_inst"
[ ! -d "$INST_DIR" ] && INST_DIR="/tmp/tau-instances/test_inst"
if [ ! -d "$INST_DIR" ]; then
    # Try finding instance dir in standard location
    INST_DIR=$(sudo find /run /tmp -type d -name "test_inst" 2>/dev/null | grep instances | head -n 1)
fi

echo "Instance dir: $INST_DIR"
[ -d "$INST_DIR/snapshots" ] || { echo "FAIL: snapshots dir not created"; exit 1; }

SNAP_COUNT=$(sudo ls "$INST_DIR/snapshots" | wc -l)
[ "$SNAP_COUNT" -ge 1 ] || { echo "FAIL: snapshot not found"; exit 1; }

echo "[+] Taking second snapshot (delta)..."
sleep 1
SNAP2_OUT=$(sudo "$TAU_BIN" snapshot test_inst)
echo "$SNAP2_OUT"

LATEST_SNAP=$(sudo ls -t "$INST_DIR/snapshots" | head -n 1)
sudo cat "$INST_DIR/snapshots/$LATEST_SNAP/snapshot.yml" | grep -q "is_delta: true" || { echo "FAIL: delta snapshot not marked"; exit 1; }
echo "[+] Delta snapshot verified!"

echo "=== Test 3: Freezing Instance ==="
echo "[+] Freezing instance..."
FREEZE_OUT=$(sudo "$TAU_BIN" freeze test_inst)
echo "$FREEZE_OUT"

echo "[+] Checking tau ls --frozen..."
FROZEN_LS=$(sudo "$TAU_BIN" ls --frozen)
echo "$FROZEN_LS"
echo "$FROZEN_LS" | grep -q "status: frozen" || { echo "FAIL: frozen status not listed"; exit 1; }

echo "=== Test 4: Running from Instance Folder / Snapshot ==="
echo "[+] Resuming instance from folder $INST_DIR..."
sudo "$TAU_BIN" run -d "$INST_DIR"
sleep 1

RESUME_LS=$(sudo "$TAU_BIN" ls test_inst)
echo "$RESUME_LS"
echo "$RESUME_LS" | grep -q "test_inst" || { echo "FAIL: resumed instance not found"; exit 1; }

echo "[+] Stopping test_inst..."
sudo "$TAU_BIN" stop test_inst

echo "=== ALL OVERCOMMITMENT, SNAPSHOT & FREEZE TESTS PASSED! ==="
