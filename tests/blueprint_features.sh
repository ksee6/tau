#!/bin/bash
set -e

TAU_BIN="$(pwd)/build/tau"
TAU_STORE_BIN="$(pwd)/build/tau-store"

TEST_DIR="/tmp/tau_blueprint_test"
rm -rf "$TEST_DIR"
mkdir -p "$TEST_DIR"
cd "$TEST_DIR"

echo "=== 1. Testing Template & Slot Merging (tau run template.yml manifest.yml) ==="
cat << 'EOF' > template.yml
- name: base-template
- spawn: echo "BEFORE_SLOT" > ./slot_order.txt
- slot: true
- spawn: echo "AFTER_SLOT" >> ./slot_order.txt
EOF

cat << 'EOF' > target.yml
- name: target-pkg
- spawn: echo "INSIDE_SLOT" >> ./slot_order.txt
EOF

$TAU_BIN run template.yml target.yml --name slot_test

if [ ! -f "slot_order.txt" ]; then
    echo "ERROR: slot_order.txt not created"
    exit 1
fi

LINE1=$(sed -n '1p' slot_order.txt)
LINE2=$(sed -n '2p' slot_order.txt)
LINE3=$(sed -n '3p' slot_order.txt)

if [ "$LINE1" != "BEFORE_SLOT" ] || [ "$LINE2" != "INSIDE_SLOT" ] || [ "$LINE3" != "AFTER_SLOT" ]; then
    echo "ERROR: Slot merging failed or incorrect execution order: $LINE1 / $LINE2 / $LINE3"
    exit 1
fi
echo "Template & slot merging OK!"

echo "=== 2. Testing bin & bindir Directives ==="
cat << 'EOF' > bin_manifest.yml
- name: bin-pkg
- bin:
    name: myapp
    description: Sample myapp launcher
- bindir: ./bin_out
EOF

$TAU_BIN run bin_manifest.yml --name bin_test

if [ ! -x "./bin_out/myapp" ]; then
    echo "ERROR: ./bin_out/myapp executable script not generated"
    exit 1
fi

if ! grep -q "tau run" ./bin_out/myapp; then
    echo "ERROR: generated wrapper script does not contain tau run call"
    exit 1
fi
echo "bin & bindir directives OK!"

echo "=== 3. Testing image Directive Creation & Truncation ==="
cat << 'EOF' > image_manifest.yml
- name: image-pkg
- image:
    source: ./test_disk.img
    size: 5M
    type: ext4
    create: true
EOF

$TAU_BIN run image_manifest.yml --name img_test

if [ ! -f "./test_disk.img" ]; then
    echo "ERROR: image file test_disk.img was not created"
    exit 1
fi

IMG_SIZE=$(stat -c%s ./test_disk.img)
EXPECTED_SIZE=$((5 * 1024 * 1024))
if [ "$IMG_SIZE" -ne "$EXPECTED_SIZE" ]; then
    echo "ERROR: image file size is $IMG_SIZE, expected $EXPECTED_SIZE"
    exit 1
fi
echo "image directive creation & truncation OK!"

echo "=== 4. Testing tau add, tau remove & Symlink Hashing ==="
export TAU_PATH="/tmp/tau_store_add_test_$$"
rm -rf "$TAU_PATH"
mkdir -p "$TAU_PATH/manifests"
mkdir -p "$TAU_PATH/store"

cat << 'EOF' > add_test.yml
- name: add-test-pkg
- spawn: echo "ADD_RUN" > ./add_out.txt
EOF

$TAU_BIN add add_test.yml

# Verify symlink created with hash
SYMLINKS=( "$TAU_PATH/manifests/"*.yml )
if [ ! -L "${SYMLINKS[0]}" ]; then
    echo "ERROR: tau add did not create hash symlink in manifests/"
    exit 1
fi

SYMLINK_TARGET=$(readlink -f "${SYMLINKS[0]}")
REAL_TARGET=$(readlink -f add_test.yml)
if [ "$SYMLINK_TARGET" != "$REAL_TARGET" ]; then
    echo "ERROR: Symlink target mismatch: $SYMLINK_TARGET vs $REAL_TARGET"
    exit 1
fi
echo "tau add hash symlink created OK!"

# Test tau remove
$TAU_BIN remove add_test.yml
REMAINING=( "$TAU_PATH/manifests/"*.yml )
if [ -e "${REMAINING[0]}" ]; then
    echo "ERROR: tau remove did not remove symlink"
    exit 1
fi
echo "=== 5. Testing bindir automatic PATH & nested spawn ==="
rm -rf ./bin_test_dir ./out_vite.txt
mkdir -p ./bin_test_dir

cat << 'EOF' > test_vite_manifest.yml
- name: vite-mock
- bin:
    name: vite
    exec: echo "vite v5.0.0"
    description: Mock Vite binary
- bindir: ./bin_test_dir
- or:
    and:
      or:
        and:
          spawn: echo "vite 5.0" > ./out_vite.txt
EOF

$TAU_BIN run test_vite_manifest.yml --name "test_vite_inst_$$"

if [ ! -x "./bin_test_dir/vite" ]; then
    echo "ERROR: ./bin_test_dir/vite executable not generated"
    exit 1
fi
echo "bindir executable wrapper generated OK!"

echo "=== 6. Testing tau activate ==="
ACTIVATE_OUTPUT=$($TAU_BIN activate test_vite_manifest.yml)
if ! echo "$ACTIVATE_OUTPUT" | grep -q "export PATH="; then
    echo "ERROR: tau activate did not output export PATH="
    exit 1
fi
if ! echo "$ACTIVATE_OUTPUT" | grep -q "bin_test_dir"; then
    echo "ERROR: tau activate did not output resolved bindir path"
    exit 1
fi
echo "tau activate OK!"

echo "=== ALL BLUEPRINT FEATURE TESTS PASSED SUCCESSFULLY! ==="
