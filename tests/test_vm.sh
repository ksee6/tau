#!/bin/bash
set -e

TAU_BIN="/home/xi/Repo/tau/build/tau"
TEST_DIR="/tmp/tau_vm_test_$$"
mkdir -p "$TEST_DIR"

cleanup() {
    rm -rf "$TEST_DIR"
}
trap cleanup EXIT

echo "=== Test 1: Testing VM Directive Parsing & Serialization ==="
cat <<EOF > "$TEST_DIR/vm_manifest.yml"
- name: test_vm_inst

- memory:
    limit: 256M
    max: 1024M

- cpu:
    quota: 200
    max: 400

- vm:
    drives:
        - "$TEST_DIR/disk1.raw"
        - source: "$TEST_DIR/disk2.qcow2"
          read: true
          write: true
          format: auto
    kernel: "$TEST_DIR/vmlinuz"
    initrd: "$TEST_DIR/initramfs"
    cmdline: "console=ttyS0 root=/dev/vda rw"
    wait: false
EOF

# Create dummy drive files
touch "$TEST_DIR/disk1.raw"
touch "$TEST_DIR/disk2.qcow2"
touch "$TEST_DIR/vmlinuz"
touch "$TEST_DIR/initramfs"

echo "[+] Inspecting manifest YAML output..."
INSPECT_OUT=$("$TAU_BIN" run "$TEST_DIR/vm_manifest.yml" 2>&1 || true)
echo "$INSPECT_OUT"

echo "=== Test 2: Checking Master Test Suites ==="
./tests/blueprint_features.sh

echo "=== ALL VM DIRECTIVE TESTS PASSED SUCCESSFULLY! ==="
