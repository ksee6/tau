#!/bin/bash
set -e

TAU_BIN="/home/xi/Repo/tau/build/tau"
TEST_DIR="/tmp/tau_run_start_test_$$"
mkdir -p "$TEST_DIR"

cleanup() {
    "$TAU_BIN" stop "$TEST_DIR/heads/head_inst" 2>/dev/null || true
    "$TAU_BIN" stop "$TEST_DIR/heads/tstart_inst" 2>/dev/null || true
    rm -rf "$TEST_DIR"
}
trap cleanup EXIT

# Create symlink shortcuts for testing argv[0] dispatch
ln -s "$TAU_BIN" "$TEST_DIR/tr"
ln -s "$TAU_BIN" "$TEST_DIR/tstart"
ln -s "$TAU_BIN" "$TEST_DIR/tgh"

echo "=== 1. Testing 'tau start' (Head mode) ==="
cat <<EOF > "$TEST_DIR/app_head.yml"
- name: head_inst
- spawn: "echo HEAD_MODE_OK"
  name: s1
  wait: true
EOF

"$TAU_BIN" start --head="$TEST_DIR/heads/NAME" "$TEST_DIR/app_head.yml"
if [ -d "$TEST_DIR/heads/head_inst" ]; then
    echo "[+] 'tau start' created head instance dir successfully!"
else
    echo "FAIL: 'tau start' did not create head instance directory"
    exit 1
fi

echo "=== 2. Testing 'tau run' (Headless mode) ==="
cat <<EOF > "$TEST_DIR/app_headless.yml"
- name: headless_inst
- spawn: "echo HEADLESS_MODE_OK"
  name: s1
  wait: true
EOF

"$TAU_BIN" run "$TEST_DIR/app_headless.yml"
if [ ! -d "$HOME/.config/tau/instances/headless_inst" ]; then
    echo "[+] 'tau run' executed in headless mode with no persistent instance dir!"
else
    echo "FAIL: 'tau run' persisted an instance directory"
    exit 1
fi

echo "=== 3. Testing 'tstart' Shortcut ==="
cat <<EOF > "$TEST_DIR/app_tstart.yml"
- name: tstart_inst
- spawn: "echo TSTART_OK"
  name: s1
  wait: true
EOF

"$TEST_DIR/tstart" --head="$TEST_DIR/heads/NAME" "$TEST_DIR/app_tstart.yml"
if [ -d "$TEST_DIR/heads/tstart_inst" ]; then
    echo "[+] 'tstart' shortcut worked in head mode!"
else
    echo "FAIL: 'tstart' shortcut failed"
    exit 1
fi

echo "=== 4. Testing 'tr' Shortcut ==="
"$TEST_DIR/tr" "$TEST_DIR/app_headless.yml"
echo "[+] 'tr' shortcut executed headless successfully!"

echo "=== 5. Testing Parent-Directory Binary Name Lookup (tau run <binName>) ==="
PROJECT_DIR="$TEST_DIR/my_project"
SUB_DIR="$PROJECT_DIR/src/deep/nested"
mkdir -p "$PROJECT_DIR/bin" "$SUB_DIR"

# Create a fake binary tool in project bin
cat <<EOF > "$PROJECT_DIR/bin/vite"
#!/bin/sh
echo "VITE_LAUNCHED: \$@"
EOF
chmod +x "$PROJECT_DIR/bin/vite"

# Project manifest
cat <<EOF > "$PROJECT_DIR/tau.yml"
- bindir: ./bin
- bin:
    name: vite
EOF

# Execute from deeply nested sub-directory by simply typing: tau run vite -- --host 0.0.0.0
cd "$SUB_DIR"
OUTPUT=$("$TAU_BIN" run vite -- --host 0.0.0.0)
echo "Output from sub-dir run: $OUTPUT"
if echo "$OUTPUT" | grep -q "VITE_LAUNCHED: --host 0.0.0.0"; then
    echo "[+] Parent-directory binary lookup and argument forwarding OK!"
else
    echo "FAIL: Binary lookup in parent manifest failed"
    exit 1
fi

echo "=== ALL TAU START/RUN & SHORTCUT TESTS PASSED! ==="
