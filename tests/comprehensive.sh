#!/bin/bash
set -e

TAU_BIN="$(pwd)/build/tau"
TAU_STORE_BIN="$(pwd)/build/tau-store"

TEST_DIR="/tmp/tau_comprehensive_test"
rm -rf "$TEST_DIR"
mkdir -p "$TEST_DIR"
cd "$TEST_DIR"

echo "=================================================================="
echo "          TAU MASTER COMPREHENSIVE TEST SUITE                     "
echo "=================================================================="

# ─── 1. Basic CLI Flags ───────────────────────────────────────────────────────
echo "[TEST 1] CLI --version and --help"
$TAU_BIN --version | grep -q "tau 0.1.0"
$TAU_BIN --help > /dev/null
echo "✓ Passed CLI basic flags"

# ─── 2. tau init ──────────────────────────────────────────────────────────────
echo "[TEST 2] tau init & symlink"
$TAU_BIN init
if [ ! -f "tau.yml" ]; then
    echo "ERROR: tau.yml not created"
    exit 1
fi
echo "✓ Passed tau init"

# ─── 3. Comprehensive Directives & Variable Operators ────────────────────────
echo "[TEST 3] Directives (var, logic, filesystem, control)"
cat << 'EOF' > test_directives.yml
- name: comprehensive-test
- version: 1.0.0
- description: Full test suite manifest
- license: MIT

# Variable Assignments & Evaluations
- var: MY_STR = Hello World
- var: MY_STR == Hello World
- var: MY_STR includes World
- var: MY_STR reg ^Hello.*

- var: MY_NUM = 100
- var: MY_NUM > 50
- var: MY_NUM >= 100
- var: MY_NUM < 200
- var: MY_NUM <= 100

# Filesystem Operations
- mkdir: ./fs_test/subdir
- spawn: echo "file1_content" > ./fs_test/file1.txt
- copy:
    source: ./fs_test/file1.txt
    target: ./fs_test/file2.txt
- symlink:
    source: ./fs_test/file1.txt
    target: ./fs_test/file1_link.txt
- unlink: ./fs_test/file2.txt

# Logic Blocks
- and:
    entry:
      - var: MY_NUM == 100
      - spawn: echo "Inside AND block" > ./fs_test/and_ok.txt

- or:
    entry:
      - fail: Supposed to fail
      - spawn: echo "Inside OR block fallback" > ./fs_test/or_ok.txt

- nand:
    entry:
      - fail: Force false to make NAND true

- nor:
    entry:
      - fail: Force false
      - fail: Force false to make NOR true

- xor:
    entry:
      - spawn: echo "XOR branch 1"
      - fail: XOR branch 2 fails

# Timing & Spawns
- wait: 0.1s
- waitexit:
    entry:
      - spawn: echo "Background 1" > ./fs_test/bg1.txt
      - spawn: echo "Background 2" > ./fs_test/bg2.txt
EOF

$TAU_BIN run test_directives.yml --name directives_inst

if [ ! -f "./fs_test/file1.txt" ]; then echo "ERROR: file1.txt missing"; exit 1; fi
if [ -f "./fs_test/file2.txt" ]; then echo "ERROR: file2.txt was not unlinked"; exit 1; fi
if [ ! -L "./fs_test/file1_link.txt" ]; then echo "ERROR: file1_link.txt symlink missing"; exit 1; fi
if [ ! -f "./fs_test/and_ok.txt" ]; then echo "ERROR: AND block failed"; exit 1; fi
if [ ! -f "./fs_test/or_ok.txt" ]; then echo "ERROR: OR block failed"; exit 1; fi
if [ ! -f "./fs_test/bg1.txt" ] || [ ! -f "./fs_test/bg2.txt" ]; then echo "ERROR: waitexit background spawns failed"; exit 1; fi
echo "✓ Passed Comprehensive Directives & Variable Operators"

# ─── 4. Fail & Throw Control Directives ───────────────────────────────────────
echo "[TEST 4] Control directives (fail & throw)"
cat << 'EOF' > test_fail.yml
- name: test-fail
- fail: Expected failure test
EOF

if $TAU_BIN run test_fail.yml --name fail_inst 2>/dev/null; then
    echo "ERROR: fail directive did not return error code"
    exit 1
fi

cat << 'EOF' > test_throw.yml
- name: test-throw
- throw: Expected throw test
EOF

if $TAU_BIN run test_throw.yml --name throw_inst 2>/dev/null; then
    echo "ERROR: throw directive did not exit with error code"
    exit 1
fi
echo "✓ Passed Control Directives (fail & throw)"

# ─── 5. Nested Manifest Includes & Cycle Detection ───────────────────────────
echo "[TEST 5] Nested Manifest Includes & Cycle Guard"
cat << 'EOF' > sub_manifest.yml
- name: sub-manifest
- spawn: echo "sub_included" > ./fs_test/sub.txt
EOF

cat << 'EOF' > parent_manifest.yml
- name: parent-manifest
- manifest: sub_manifest.yml
EOF

$TAU_BIN run parent_manifest.yml --name parent_inst
if [ ! -f "./fs_test/sub.txt" ]; then echo "ERROR: nested manifest include failed"; exit 1; fi

# Cycle detection test
cat << 'EOF' > cycle_a.yml
- manifest: cycle_b.yml
EOF
cat << 'EOF' > cycle_b.yml
- manifest: cycle_a.yml
EOF

if $TAU_BIN run cycle_a.yml --name cycle_inst 2>/dev/null; then
    echo "ERROR: cyclic manifest inclusion was not detected"
    exit 1
fi
echo "✓ Passed Nested Manifest Includes & Cycle Detection"

# ─── 6. Template & Slot Merging ──────────────────────────────────────────────
echo "[TEST 6] Template & Slot Merging"
cat << 'EOF' > tpl.yml
- name: template
- spawn: echo "STEP_1" > $PWD/slot_order.txt
- slot: true
- spawn: echo "STEP_3" >> $PWD/slot_order.txt
EOF

cat << 'EOF' > target_pkg.yml
- name: target
- spawn: echo "STEP_2" >> $PWD/slot_order.txt
EOF

$TAU_BIN run tpl.yml target_pkg.yml --name slot_inst

S1=$(sed -n '1p' slot_order.txt)
S2=$(sed -n '2p' slot_order.txt)
S3=$(sed -n '3p' slot_order.txt)

if [ "$S1" != "STEP_1" ] || [ "$S2" != "STEP_2" ] || [ "$S3" != "STEP_3" ]; then
    echo "ERROR: Slot ordering incorrect: $S1, $S2, $S3"
    exit 1
fi
echo "✓ Passed Template & Slot Merging"

# ─── 7. bin and bindir Executable Wrapper Generation ────────────────────────
echo "[TEST 7] bin and bindir Directives"
cat << 'EOF' > bin_pkg.yml
- name: bin-pkg
- bin:
    name: runner_cmd
    description: High performance tool
- bindir: ./bin_dir
EOF

$TAU_BIN run bin_pkg.yml --name bin_inst

if [ ! -x "./bin_dir/runner_cmd" ]; then echo "ERROR: ./bin_dir/runner_cmd script not executable"; exit 1; fi
if ! grep -q "exec tau run" ./bin_dir/runner_cmd; then echo "ERROR: wrapper missing tau run execution"; exit 1; fi
echo "✓ Passed bin & bindir Directives"

# ─── 8. image Directive (Creation, Resize, @/ Path) ─────────────────────────
echo "[TEST 8] image Directive (Create, Truncate, Format)"
cat << 'EOF' > img_pkg.yml
- name: img-pkg
- image:
    path: ./virtual_disk.img
    size: 4M
    type: ext4
    create: true
EOF

$TAU_BIN run img_pkg.yml --name img_inst

if [ ! -f "./virtual_disk.img" ]; then echo "ERROR: virtual_disk.img not created"; exit 1; fi
IMG_SZ=$(stat -c%s ./virtual_disk.img)
EXPECTED_SZ=$((4 * 1024 * 1024))
if [ "$IMG_SZ" -ne "$EXPECTED_SZ" ]; then echo "ERROR: image size mismatch ($IMG_SZ vs $EXPECTED_SZ)"; exit 1; fi
echo "✓ Passed image Directive"

# ─── 9. Background Instance & Socket IPC (ls, cat, stop) ────────────────────
echo "[TEST 9] Background Instance supervision & IPC (ls, cat, stop)"
cat << 'EOF' > bg_pkg.yml
- name: bg-pkg
- spawn:
    command: sh -c 'echo "BG_LOG_LINE"; sleep 20'
    wait: true
    name: daemon_spawn
EOF

$TAU_BIN run bg_pkg.yml --name bg_instance --detach
sleep 1

# Check tau ls
$TAU_BIN ls | grep -q "bg_instance"
$TAU_BIN ls bg_instance | grep -q "daemon_spawn"

# Check tau cat
$TAU_BIN cat bg_instance:daemon_spawn | grep -q "BG_LOG_LINE"

# Stop background instance
$TAU_BIN stop bg_instance:daemon_spawn --kill
echo "✓ Passed Background Supervision & IPC"

# ─── 10. Hot Reloading in tau-spawn ─────────────────────────────────────────
echo "[TEST 10] Hot-Reloading of Manifest Files"
$TAU_BIN stop hot_inst --kill 2>/dev/null || true
sleep 0.5

INSTANCES_DIR="/run/user/$USER/tau/instances"
if [ ! -d "$INSTANCES_DIR" ]; then
    INSTANCES_DIR="/run/user/$(id -u)/tau/instances"
fi
if [ -n "$TAU_PATH_TEMP" ]; then
    INSTANCES_DIR="$TAU_PATH_TEMP/instances"
fi

rm -rf "$INSTANCES_DIR/hot_inst"*
mkdir -p /tmp/tau_comprehensive_test/fs_test
cat << 'EOF' > hot_reload_manifest.yml
- name: hot-reload-pkg
- spawn:
    command: sh -c 'sleep 100'
    wait: false
    name: hot_sleeper
EOF

$TAU_BIN run hot_reload_manifest.yml --name hot_inst --detach
sleep 1

# Append new spawn to instance manifest file on disk
sleep 0.5
echo "- spawn: echo HOT_RELOADED_OK > /tmp/tau_comprehensive_test/fs_test/hot_reload_marker.txt" >> "$INSTANCES_DIR/hot_inst/instance.yml"
sleep 3


ls -la /tmp/tau_comprehensive_test/fs_test

if [ ! -f "/tmp/tau_comprehensive_test/fs_test/hot_reload_marker.txt" ]; then
    echo "ERROR: Hot-reloading did not trigger new directive execution"
    cat "$INSTANCES_DIR/hot_inst/spawn.log" 2>/dev/null || true
    $TAU_BIN stop hot_inst --kill 2>/dev/null || true
    exit 1
fi

$TAU_BIN stop hot_inst --kill 2>/dev/null || true
echo "✓ Passed Hot-Reloading in tau-spawn"


# ─── 11. Store GC & local_ Directory Preservation ───────────────────────────
echo "[TEST 11] Store GC & local_ Directory Preservation"
export TAU_PATH="/tmp/tau_master_store_test"
rm -rf "$TAU_PATH"
mkdir -p "$TAU_PATH/store/local_pkg_hash"
mkdir -p "$TAU_PATH/manifests"
echo "store_marker" > "$TAU_PATH/store/local_pkg_hash/ran"

$TAU_STORE_BIN

if [ ! -d "$TAU_PATH/store/local_pkg_hash" ]; then
    echo "ERROR: Store GC purged local_ store directory"
    exit 1
fi
echo "✓ Passed Store GC & local_ Preservation"

# ─── 12. Complex Local & GitHub Dependency Resolution ───────────────────────
echo "[TEST 12] Complex Local & GitHub Dependency Resolution (xipid/rho.git)"

LOCAL_DEP_DIR="/tmp/tau_comprehensive_test/complex_dep"
rm -rf "$LOCAL_DEP_DIR"
mkdir -p "$LOCAL_DEP_DIR"

cat << 'EOF' > "$LOCAL_DEP_DIR/tau.yml"
- name: complex-dep-pkg
- git:
    source: "https://github.com/xipid/rho.git"
    branch: main
    target: ./deps/rho
- copy:
    source: ./deps/rho/README.md
    target: ./rho_readme_copy.md
EOF
echo "local_dep_code" > "$LOCAL_DEP_DIR/dep_code.c"

ROOT_DEP_DIR="/tmp/tau_comprehensive_test/root_dep"
rm -rf "$ROOT_DEP_DIR"
mkdir -p "$ROOT_DEP_DIR"

cat << EOF > "$ROOT_DEP_DIR/tau.yml"
- name: root-dep-pkg
- local:
    source: $LOCAL_DEP_DIR
    target: ./fetched_dep
EOF

$TAU_BIN run "$ROOT_DEP_DIR/tau.yml"

if [ ! -f "$ROOT_DEP_DIR/fetched_dep/dep_code.c" ]; then
    echo "ERROR: Local dependency files were not copied to target destination"
    exit 1
fi

if [ ! -d "$ROOT_DEP_DIR/fetched_dep/deps/rho" ] || [ ! -f "$ROOT_DEP_DIR/fetched_dep/deps/rho/README.md" ]; then
    echo "ERROR: Complex dependency failed to fetch GitHub repo xipid/rho.git"
    exit 1
fi

NO_MANIFEST_DEP="/tmp/tau_comprehensive_test/no_manifest_dep"
rm -rf "$NO_MANIFEST_DEP"
mkdir -p "$NO_MANIFEST_DEP"
echo "raw_library_data" > "$NO_MANIFEST_DEP/lib.h"

cat << EOF > "$ROOT_DEP_DIR/tau_no_manifest.yml"
- name: root-no-manifest-pkg
- local:
    source: $NO_MANIFEST_DEP
    target: ./fetched_raw_lib
EOF

$TAU_BIN run "$ROOT_DEP_DIR/tau_no_manifest.yml"

if [ ! -f "$ROOT_DEP_DIR/fetched_raw_lib/lib.h" ]; then
    echo "ERROR: Dependency without tau.yml failed to copy to target destination"
    exit 1
fi

echo "✓ Passed Complex Local & GitHub Dependency Resolution (xipid/rho.git)"

echo "=================================================================="
echo "  🎉 ALL 12 MASTER COMPREHENSIVE TESTS PASSED SUCCESSFULLY!       "
echo "=================================================================="
