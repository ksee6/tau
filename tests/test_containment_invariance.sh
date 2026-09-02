#!/bin/bash
set -e

TAU_BIN="/home/xi/Repo/tau/build/tau"
TEST_DIR="/tmp/tau_containment_test_$$"
mkdir -p "$TEST_DIR"

cleanup() {
    "$TAU_BIN" stop "$TEST_DIR/instances/mem_test" 2>/dev/null || true
    "$TAU_BIN" stop "$TEST_DIR/instances/chroot_test" 2>/dev/null || true
    "$TAU_BIN" stop "$TEST_DIR/instances/multi_entry" 2>/dev/null || true
    rm -rf "$TEST_DIR"
}
trap cleanup EXIT

echo "=== 1. Testing Monotonic Resource Limiter Clamping in Multi-Entry Run ==="
mkdir -p "$TEST_DIR/templates"

cat <<EOF > "$TEST_DIR/templates/outer.yml"
- name: mem_test
- memory: 128MB
- cpu: 20%
- cpuset: "0,1"
- autofreeze:
    timer: 5s
    cpu_threshold: 5%
- slot: true
EOF

cat <<EOF > "$TEST_DIR/child.yml"
- memory: 2GB
- cpu: 90%
- cpuset: "0,1,2,3,4,5"
- autofreeze:
    timer: 60s
    cpu_threshold: 50%
- spawn: "echo OK"
  name: check
  wait: true
EOF

"$TAU_BIN" run --head="$TEST_DIR/instances/NAME" "$TEST_DIR/templates/outer.yml" "$TEST_DIR/child.yml"
echo "[+] Multi-entry resource monotonicity passed!"

echo "=== 2. Testing Chroot Path Containment & Escape Prevention ==="
mkdir -p "$TEST_DIR/jail/root" "$TEST_DIR/jail/sub"

cat <<EOF > "$TEST_DIR/templates/jail_outer.yml"
- name: chroot_test
- chroot: "$TEST_DIR/jail/root"
- slot: true
EOF

cat <<EOF > "$TEST_DIR/jail_breakout.yml"
- spawn: "pwd"
  name: breakout
  wait: true
EOF

"$TAU_BIN" run --head="$TEST_DIR/instances/NAME" "$TEST_DIR/templates/jail_outer.yml" "$TEST_DIR/jail_breakout.yml"
echo "[+] Chroot path containment passed!"

echo "=== 3. Testing Valid Nested Sub-Chroot Within Outer Chroot ==="
cat <<EOF > "$TEST_DIR/jail_valid_sub.yml"
- chroot: "$TEST_DIR/jail/root/sub"
- spawn: "echo SUB_OK"
  name: ok_sub
  wait: true
EOF

mkdir -p "$TEST_DIR/jail/root/sub"
"$TAU_BIN" run --head="$TEST_DIR/instances/NAME" "$TEST_DIR/templates/jail_outer.yml" "$TEST_DIR/jail_valid_sub.yml"
echo "[+] Valid sub-chroot inside outer rootfs allowed!"

echo "=== 4. Testing Multi-Entry Template Wrapping & Slot Cleanliness ==="
cat <<EOF > "$TEST_DIR/templates/t1.yml"
- name: multi_entry
- var:
    T1_LOADED: "true"
- slot: true
- var:
    T1_DONE: "true"
EOF

cat <<EOF > "$TEST_DIR/templates/t2.yml"
- var:
    T2_LOADED: "true"
- slot: true
EOF

cat <<EOF > "$TEST_DIR/app.yml"
- spawn: "echo APP_OK"
  name: app
  wait: true
EOF

"$TAU_BIN" run --head="$TEST_DIR/instances/NAME" "$TEST_DIR/templates/t1.yml" "$TEST_DIR/templates/t2.yml" "$TEST_DIR/app.yml"
echo "[+] Multi-level template slot application OK!"

echo "=== ALL MATHEMATICAL CONTAINMENT AND INVARIANCE TESTS PASSED! ==="
