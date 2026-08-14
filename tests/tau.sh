#!/bin/bash
set -e

TAU_BIN="$(pwd)/build/tau"
TAU_STORE_BIN="$(pwd)/build/tau-store"

echo "=== 1. Testing tau --version and --help ==="
$TAU_BIN --version
$TAU_BIN --help > /dev/null

echo "=== 2. Creating test workspace ==="
TEST_DIR="/tmp/tau_test_env"
rm -rf "$TEST_DIR"
mkdir -p "$TEST_DIR"
rm -rf ~/.cache/tau/instances/bg_test* ~/.cache/tau/instances/test_inst*
cd "$TEST_DIR"

echo "=== 3. Testing tau init ==="
$TAU_BIN init
if [ ! -f "tau.yml" ]; then
    echo "ERROR: tau.yml not created"
    exit 1
fi
echo "tau init OK"

echo "=== 4. Testing manifest execution (spawn, vars, logic, filesystem) ==="
cat << 'EOF' > test_manifest.yml
- name: test-instance
- var: TEST_VAR = HelloTau
- var: TEST_VAR == HelloTau
- var: TEST_VAR includes Hello
- var: TEST_VAR reg ^Hello.*
- var: NUM_VAR = 42
- var: NUM_VAR > 10
- var: NUM_VAR <= 50

- mkdir: ./test_dir
- spawn: echo "Direct spawn line 1" > ./test_dir/file1.txt
- copy:
    source: ./test_dir/file1.txt
    target: ./test_dir/file2.txt
- symlink:
    source: ./test_dir/file1.txt
    target: ./test_dir/file1_link.txt

- and:
    - spawn: echo "Inside AND block"
    - var: TEST_VAR == HelloTau

- or:
    - fail: Supposed to fail
    - spawn: echo "Inside OR block after failure fallback"

- waitexit:
    - spawn:
        command: echo "Background spawn in waitexit 1"
        wait: false
        name: bg1
    - spawn:
        command: echo "Background spawn in waitexit 2"
        wait: false
        name: bg2

EOF

echo "Running test_manifest.yml..."
$TAU_BIN run test_manifest.yml --name test_inst1

if [ ! -f "./test_dir/file2.txt" ]; then
    echo "ERROR: copy directive failed"
    exit 1
fi

if [ ! -L "./test_dir/file1_link.txt" ]; then
    echo "ERROR: symlink directive failed"
    exit 1
fi

echo "Manifest execution OK!"

echo "=== 5. Testing background detached run & tau ls / cat / stop ==="
cat << 'EOF' > bg_manifest.yml
- name: bg-instance
- spawn:
    command: sleep 30
    wait: true
    name: sleeper
EOF

$TAU_BIN run bg_manifest.yml --name bg_test --detach

sleep 1

echo "Checking tau ls..."
$TAU_BIN ls

echo "Checking tau ls bg_test..."
$TAU_BIN ls bg_test

echo "Stopping bg_test..."
$TAU_BIN stop bg_test:sleeper --kill

echo "=== 6. Testing tau-store ==="
mkdir -p /tmp/tau_store_test/manifests
export TAU_PATH="/tmp/tau_store_test"

cat << 'EOF' > /tmp/tau_store_test/manifests/store_test.yml
- name: store-pkg
- mkdir: ./installed_marker
EOF

$TAU_STORE_BIN
if [ -d "/tmp/tau_store_test/store" ]; then
    echo "tau-store created store dir successfully!"
fi

echo "=== ALL TESTS PASSED SUCCESSFULLY! ==="
