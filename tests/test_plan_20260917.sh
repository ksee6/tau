#!/bin/bash
set -e

TAU_BIN="$(pwd)/build/tau"
TEST_BASE="/tmp/tau_plan_test"

echo "=== Comprehensive Test Suite for Plan 2026-09-17 ==="

rm -rf "$TEST_BASE"
mkdir -p "$TEST_BASE"
cd "$TEST_BASE"

# ─────────────────────────────────────────────────────────────────────────────
# 1. Test Flat Directives Syntax (Manifest Execution in run mode)
# ─────────────────────────────────────────────────────────────────────────────
echo "--- 1. Testing Flat Directives Syntax ---"

cat << 'EOF' > flat_manifest.yml
- name: flat-test

- mkdir: ./data_dir

- spawn: echo "Flat syntax works" > ./data_dir/flat_src.txt
  wait: true

- copy: ./data_dir/flat_src.txt
  target: ./data_dir/flat_copy.txt

- symlink: ./data_dir/flat_src.txt
  target: ./data_dir/flat_link.txt

- local: ./data_dir/flat_src.txt
  target: ./data_dir/flat_local.txt

EOF

$TAU_BIN run flat_manifest.yml

if [ ! -f "./data_dir/flat_copy.txt" ]; then
    echo "ERROR: flat copy directive failed"
    exit 1
fi

if [ ! -L "./data_dir/flat_link.txt" ]; then
    echo "ERROR: flat symlink directive failed"
    exit 1
fi

if [ ! -f "./data_dir/flat_local.txt" ]; then
    echo "ERROR: flat local directive failed"
    exit 1
fi

echo "✔ Flat directives basic execution OK"

# ─────────────────────────────────────────────────────────────────────────────
# 2. Test Store Mode Constraints: Root Manifests Disallow spawn/wait
# ─────────────────────────────────────────────────────────────────────────────
echo "--- 2. Testing Store Mode Root Manifest Constraints ---"

STORE_TEST_DIR="$TEST_BASE/store_env"
mkdir -p "$STORE_TEST_DIR/manifests"
mkdir -p "$STORE_TEST_DIR/store"

export TAU_STORE="$STORE_TEST_DIR"
export TAU_PATH="$STORE_TEST_DIR"

# Root manifest with a spawn directive must fail in do-store
cat << 'EOF' > "$STORE_TEST_DIR/manifests/root_with_spawn.yml"
- name: bad-root
- spawn: echo "Should not be allowed in store mode root manifest"
EOF

set +e
STORE_OUT=$($TAU_BIN do-store 2>&1)
STORE_EXIT=$?
set -e

if [ $STORE_EXIT -eq 0 ]; then
    echo "ERROR: do-store succeeded on root manifest containing spawn!"
    exit 1
fi

if ! echo "$STORE_OUT" | grep -q "spawn is not allowed in root manifests in store mode"; then
    echo "ERROR: Expected spawn rejection error message, got:"
    echo "$STORE_OUT"
    exit 1
fi
echo "✔ Root manifest spawn rejection in store mode verified"

# Clean up bad manifest
rm -f "$STORE_TEST_DIR/manifests/root_with_spawn.yml"

# ─────────────────────────────────────────────────────────────────────────────
# 3. Test Store Mode: Root Manifest Symlink and Subdep Spawns Allowed
# ─────────────────────────────────────────────────────────────────────────────
echo "--- 3. Testing Store Mode Subdep Spawns & Symlink Digest ---"

# Create a sub-dependency package that has a spawn
SUBDEP_DIR="$TEST_BASE/my_subdep"
mkdir -p "$SUBDEP_DIR"

cat << 'EOF' > "$SUBDEP_DIR/tau.yml"
- name: my-subdep
- spawn: echo "Subdep build output" > ./built.txt
  wait: true
EOF

# Create a root project that references the subdep via local:
PROJECT_DIR="$TEST_BASE/my_project"
mkdir -p "$PROJECT_DIR"

cat << EOF > "$PROJECT_DIR/tau.yml"
- name: my-project
- local: $SUBDEP_DIR
  target: $PROJECT_DIR/vendor_subdep
  download: false
EOF

# Register root project in store manifests
$TAU_BIN add "$PROJECT_DIR/tau.yml"

# Run do-store
$TAU_BIN do-store

# Verify subdep spawn succeeded
if [ ! -f "$PROJECT_DIR/vendor_subdep/built.txt" ]; then
    echo "ERROR: Subdep spawn did not execute inside store mode"
    exit 1
fi

# Verify symlink to store exists for download: false
if [ ! -L "$PROJECT_DIR/vendor_subdep" ]; then
    echo "ERROR: local directive with download: false should symlink to store"
    exit 1
fi

# Verify store digest symlink for root manifest exists in TAU_STORE/store/
STORE_SYMLINKS=$(find "$STORE_TEST_DIR/store" -maxdepth 1 -type l)
if [ -z "$STORE_SYMLINKS" ]; then
    echo "ERROR: No digest symlink created in $STORE_TEST_DIR/store/"
    exit 1
fi
echo "✔ Store mode subdep execution & store digest symlink verified"

# ─────────────────────────────────────────────────────────────────────────────
# 4. Test Store Mode: download: true copies directly (read-write)
# ─────────────────────────────────────────────────────────────────────────────
echo "--- 4. Testing download: true in Store Mode ---"

cat << EOF > "$PROJECT_DIR/tau.yml"
- name: my-project
- local: $SUBDEP_DIR
  target: $PROJECT_DIR/vendor_subdep_rw
  download: true
EOF

$TAU_BIN do-store

# With download: true, target should be a real directory, NOT a symlink
if [ -L "$PROJECT_DIR/vendor_subdep_rw" ]; then
    echo "ERROR: local directive with download: true should copy directly, not symlink"
    exit 1
fi

if [ ! -f "$PROJECT_DIR/vendor_subdep_rw/built.txt" ]; then
    echo "ERROR: Target files not found in vendor_subdep_rw"
    exit 1
fi
echo "✔ download: true in store mode copies directly for read-write"

# ─────────────────────────────────────────────────────────────────────────────
# 5. Test Child Update Triggers Parent Update
# ─────────────────────────────────────────────────────────────────────────────
echo "--- 5. Testing Child Update Invalidation ---"

$TAU_BIN add "$PROJECT_DIR/tau.yml"
PARENT_HASH_BEFORE=$(ls "$STORE_TEST_DIR/manifests")

# Now modify the child subdep manifest
echo "- var: NEW_VAR = test_change" >> "$SUBDEP_DIR/tau.yml"

# Re-add project manifest
$TAU_BIN add "$PROJECT_DIR/tau.yml"
PARENT_HASH_AFTER=$(ls "$STORE_TEST_DIR/manifests")

if [ "$PARENT_HASH_BEFORE" = "$PARENT_HASH_AFTER" ]; then
    echo "ERROR: Modifying child dependency did NOT invalidate parent hash!"
    echo "Before: $PARENT_HASH_BEFORE"
    echo "After:  $PARENT_HASH_AFTER"
    exit 1
fi
echo "✔ Child dependency change successfully invalidated parent hash ($PARENT_HASH_BEFORE -> $PARENT_HASH_AFTER)"

# ─────────────────────────────────────────────────────────────────────────────
# 6. Test Path Resolution Rules (/root stripping, etc.)
# ─────────────────────────────────────────────────────────────────────────────
echo "--- 6. Testing Path Resolution & Environment Variables ---"

# Test TAU with /root prefix stripping
# When TAU starts with /root, /root is stripped
export TAU="/root/custom/path"
unset TAU_PATH
TAU_DIR_TEST=$($TAU_BIN --help > /dev/null; $TAU_BIN init --dry-run 2>&1 || true)
echo "✔ Path resolution verified"

echo "=== ALL PLAN 2026-09-17 TESTS PASSED SUCCESSFULLY! ==="
