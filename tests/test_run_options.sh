#!/bin/bash
set -e

TAU_BIN="/home/xi/Repo/tau/build/tau"
TEST_DIR="/tmp/tau_run_opts_test_$$"
mkdir -p "$TEST_DIR"

cleanup() {
    "$TAU_BIN" stop "$TEST_DIR/heads/custom_inst" 2>/dev/null || true
    "$TAU_BIN" stop "$TEST_DIR/heads/symlink_inst" 2>/dev/null || true
    "$TAU_BIN" stop args_inst 2>/dev/null || true
    rm -rf "$TEST_DIR"
}
trap cleanup EXIT

echo "=== Test 1: Testing --head=/path/to/NAME and default copy ==="
cat <<EOF > "$TEST_DIR/manifest1.yml"
- name: custom_inst
- spawn: "echo HELLO_CUSTOM > $TEST_DIR/out1.txt; sleep 10"
  wait: true
EOF

"$TAU_BIN" run --head="$TEST_DIR/heads/NAME" -d "$TEST_DIR/manifest1.yml"
sleep 1

[ -d "$TEST_DIR/heads/custom_inst" ] || { echo "FAIL: custom head dir not created"; exit 1; }
[ -f "$TEST_DIR/heads/custom_inst/manifest.yml" ] || { echo "FAIL: manifest.yml not copied to head dir"; exit 1; }
[ ! -L "$TEST_DIR/heads/custom_inst/manifest.yml" ] || { echo "FAIL: manifest.yml should be regular file with default copy"; exit 1; }
[ -f "$TEST_DIR/out1.txt" ] || { echo "FAIL: out1.txt not created"; exit 1; }
echo "[+] --head and copy OK!"
"$TAU_BIN" stop "$TEST_DIR/heads/custom_inst" 2>/dev/null || true

echo "=== Test 2: Testing --no-copy (symlink in instance dir) ==="
cat <<EOF > "$TEST_DIR/manifest2.yml"
- name: symlink_inst
- spawn: "echo HELLO_SYMLINK > $TEST_DIR/out2.txt; sleep 10"
  wait: true
EOF

"$TAU_BIN" run --head="$TEST_DIR/heads/NAME" --no-copy -d "$TEST_DIR/manifest2.yml"
sleep 1

[ -L "$TEST_DIR/heads/symlink_inst/manifest.yml" ] || { echo "FAIL: manifest.yml should be symlink with --no-copy"; exit 1; }
echo "[+] --no-copy creates symlink OK!"
"$TAU_BIN" stop "$TEST_DIR/heads/symlink_inst" 2>/dev/null || true

echo "=== Test 3: Testing --headless (no instance dir created) ==="
HEADLESS_DIR="$TEST_DIR/headless_heads"
mkdir -p "$HEADLESS_DIR"
cat <<EOF > "$TEST_DIR/manifest3.yml"
- name: headless_inst
- spawn: "echo HELLO_HEADLESS > $TEST_DIR/out3.txt"
  wait: true
EOF

"$TAU_BIN" run --headless "$TEST_DIR/manifest3.yml"
[ -f "$TEST_DIR/out3.txt" ] || { echo "FAIL: out3.txt not created in headless run"; exit 1; }
echo "[+] --headless run OK!"

echo "=== Test 4: Testing -- args forwarded to manifest variables ==="
cat <<EOF > "$TEST_DIR/manifest4.yml"
- name: args_inst
- spawn: "echo ARGS: %0 %1 > $TEST_DIR/out4.txt"
  wait: true
EOF

"$TAU_BIN" run "$TEST_DIR/manifest4.yml" -- foo bar
[ -f "$TEST_DIR/out4.txt" ] || { echo "FAIL: out4.txt not created"; exit 1; }
grep -q "ARGS: foo bar" "$TEST_DIR/out4.txt" || { echo "FAIL: args %0 %1 not interpolated correctly"; cat "$TEST_DIR/out4.txt"; exit 1; }
echo "[+] Forwarding -- args OK!"

echo "=== ALL TAU RUN ENHANCEMENT TESTS PASSED! ==="
