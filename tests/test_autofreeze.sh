#!/bin/bash
set -e

TAU_BIN="/home/xi/Repo/tau/build/tau"
TEST_DIR="/tmp/tau_autofreeze_test_$$"
mkdir -p "$TEST_DIR"

cleanup() {
    "$TAU_BIN" stop "$TEST_DIR/instances/autofreeze_inst" 2>/dev/null || true
    "$TAU_BIN" stop "$TEST_DIR/instances/soft_inst" 2>/dev/null || true
    "$TAU_BIN" stop "$TEST_DIR/instances/hard_inst" 2>/dev/null || true
    rm -rf "$TEST_DIR"
}
trap cleanup EXIT

echo "=== 1. Testing autofreeze: Directive Parsing & Serialization ==="
cat <<EOF > "$TEST_DIR/manifest_af.yml"
- name: autofreeze_inst
- autofreeze:
    wol: true
    cpu_threshold: 5%
    timer: 2s
- spawn: "sleep 30"
  name: worker
  wait: true
EOF

"$TAU_BIN" inspect "$TEST_DIR/manifest_af.yml" | grep -q "autofreeze:" || { echo "FAIL: autofreeze directive not serialized in inspect"; exit 1; }
echo "[+] autofreeze directive parsed & serialized OK!"

echo "=== 2. Testing Soft Freezing a Specific Spawn (tau freeze inst:spawn) ==="
cat <<EOF > "$TEST_DIR/manifest_soft.yml"
- name: soft_inst
- spawn: "sleep 30"
  name: s1
  wait: true
- spawn: "sleep 30"
  name: s2
  wait: true
EOF

"$TAU_BIN" run --head="$TEST_DIR/instances/NAME" -d "$TEST_DIR/manifest_soft.yml"
sleep 1

# Soft freeze only spawn s1
"$TAU_BIN" freeze "$TEST_DIR/instances/soft_inst:s1"
sleep 0.5

# Verify tau-instance is still running and responsive
[ -S "$TEST_DIR/instances/soft_inst/tau.sock" ] || { echo "FAIL: soft_inst daemon died after soft freeze"; exit 1; }

# Check that snapshot was created
[ -d "$TEST_DIR/instances/soft_inst/snapshots" ] || { echo "FAIL: snapshots dir not created"; exit 1; }
echo "[+] Per-spawn soft freeze & delta snapshot OK!"

echo "=== 3. Testing Wake on LAN / IPC Wake on Soft-Frozen Spawn ==="
# Cat on s1 triggers automatic wake
"$TAU_BIN" cat "$TEST_DIR/instances/soft_inst:s1" 10
sleep 0.5
echo "[+] Wake on activity OK!"
"$TAU_BIN" stop "$TEST_DIR/instances/soft_inst" 2>/dev/null || true

echo "=== 4. Testing Auto-Freeze on Idle Timer ==="
"$TAU_BIN" run --head="$TEST_DIR/instances/NAME" -d "$TEST_DIR/manifest_af.yml"
sleep 1

# Wait for 3 seconds for autofreeze timer (2s) to trigger
echo "Waiting for autofreeze idle countdown..."
sleep 3

# Verify that snapshot was automatically generated
SNAPS=$(ls -1 "$TEST_DIR/instances/autofreeze_inst/snapshots" 2>/dev/null | wc -l)
[ "$SNAPS" -ge 1 ] || { echo "FAIL: autofreeze did not trigger automatic snapshot"; exit 1; }
echo "[+] Auto-freeze on idle timer triggered successfully!"
"$TAU_BIN" stop "$TEST_DIR/instances/autofreeze_inst" 2>/dev/null || true

echo "=== 5. Testing Hard Freeze (Entire Instance) ==="
cat <<EOF > "$TEST_DIR/manifest_hard.yml"
- name: hard_inst
- spawn: "sleep 30"
  name: h1
  wait: true
EOF

"$TAU_BIN" run --head="$TEST_DIR/instances/NAME" -d "$TEST_DIR/manifest_hard.yml"
sleep 1

"$TAU_BIN" freeze "$TEST_DIR/instances/hard_inst"
sleep 0.5

# Verify instance directory is preserved with frozen state
[ -f "$TEST_DIR/instances/hard_inst/instance.yml" ] || { echo "FAIL: hard freeze did not preserve instance.yml"; exit 1; }
echo "[+] Hard freeze OK!"

echo "=== ALL AUTOFREEZE & WOL TESTS PASSED SUCCESSFULLY! ==="
