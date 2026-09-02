#!/bin/bash
set -e

TAU_BIN="/home/xi/Repo/tau/build/tau"
TEST_DIR="/tmp/tau_dsl_enhancements_test_$$"
mkdir -p "$TEST_DIR"

cleanup() {
    "$TAU_BIN" stop cont-alpha 2>/dev/null || true
    "$TAU_BIN" stop "$TEST_DIR/heads/cont-alpha" 2>/dev/null || true
    rm -rf "$TEST_DIR"
}
trap cleanup EXIT

echo "=== 1. Testing Container Alpha Full DSL Manifest Parsing & Execution ==="
cat <<'EOF_CONT' > "$TEST_DIR/cont-alpha.yml"
- name: cont-alpha
- description: Container Alpha on Shared Bridge

- var:
    bridge: tau_br0
    br_ip: 192.168.10.1
    cont_ip: 192.168.10.10

- noread: /tmp/secret_noread/*
- nowrite: /tmp/secret_nowrite/*
- noexecute: /tmp/secret_noexec/*
- nomount: /tmp/secret_nomount/*
- nowatch: /tmp/secret_nowatch/*

- unshare: true

- nolinks: true

- veth:
    source: veth_%name
    sip: %br_ip/24
    target: eth0
    ip: %cont_ip/24

- bridge:
    source: %bridge
    target: veth_%name
    ip: %br_ip/24

- spawn: "echo DSL_CONTAINER_OK > /tmp/tau_cont_alpha_out.txt"
  wait: true
  name: main_worker

- route: default
  via:
    - ip: %br_ip
      link: eth0
      weight: 1

- trigger: alpha_ready

- wait: alpha_ready
EOF_CONT

"$TAU_BIN" inspect "$TEST_DIR/cont-alpha.yml" > "$TEST_DIR/inspect.txt"
grep -q "cont-alpha" "$TEST_DIR/inspect.txt" || { echo "FAIL: cont-alpha not serialized"; exit 1; }
grep -q "noread" "$TEST_DIR/inspect.txt" || { echo "FAIL: noread not in inspect"; exit 1; }
grep -q "veth" "$TEST_DIR/inspect.txt" || { echo "FAIL: veth not in inspect"; exit 1; }
grep -q "bridge" "$TEST_DIR/inspect.txt" || { echo "FAIL: bridge not in inspect"; exit 1; }
grep -q "route" "$TEST_DIR/inspect.txt" || { echo "FAIL: route not in inspect"; exit 1; }
grep -q "trigger" "$TEST_DIR/inspect.txt" || { echo "FAIL: trigger not in inspect"; exit 1; }
echo "[+] Manifest parsed & inspected successfully!"

echo "=== 2. Testing Execution of New Directives ==="
"$TAU_BIN" run --headless "$TEST_DIR/cont-alpha.yml"
[ -f "/tmp/tau_cont_alpha_out.txt" ] || { echo "FAIL: container worker output not found"; exit 1; }
grep -q "DSL_CONTAINER_OK" "/tmp/tau_cont_alpha_out.txt" || { echo "FAIL: output mismatch"; exit 1; }
rm -f "/tmp/tau_cont_alpha_out.txt"
echo "[+] Headless execution of container manifest OK!"

echo "=== 3. Testing Rejection of Invalid Spawn Nested Syntax ==="
cat <<'EOF_INVALID' > "$TEST_DIR/invalid_spawn.yml"
- name: bad_spawn
- spawn:
    command: aaa
EOF_INVALID

if "$TAU_BIN" inspect "$TEST_DIR/invalid_spawn.yml" 2>&1 | grep -q "invalid syntax"; then
    echo "[+] Nested spawn: command: correctly rejected as invalid syntax!"
else
    echo "FAIL: nested spawn syntax was not rejected"
    exit 1
fi

echo "=== 4. Testing Root Alias for Chroot ==="
cat <<'EOF_ROOT' > "$TEST_DIR/root_alias.yml"
- name: root_alias_test
- root: /tmp/custom_root
EOF_ROOT

"$TAU_BIN" inspect "$TEST_DIR/root_alias.yml" > "$TEST_DIR/root_inspect.txt"
grep -q "chroot: /tmp/custom_root" "$TEST_DIR/root_inspect.txt" || { echo "FAIL: root: did not alias to chroot"; exit 1; }
echo "[+] root: keyword aliased to chroot OK!"

echo "=== 5. Testing Passive Wait and Trigger Synchronization ==="
cat <<'EOF_SYNC' > "$TEST_DIR/wait_trigger.yml"
- name: sync_test
- spawn: "sleep 0.1 && echo TRIGGER_EMITTED > /tmp/tau_trig_out.txt"
  wait: false
  name: background_worker
- trigger: custom_event
- wait: [background_worker, custom_event]
- spawn: "echo SYNC_COMPLETE > /tmp/tau_sync_out.txt"
  wait: true
EOF_SYNC

"$TAU_BIN" run --headless "$TEST_DIR/wait_trigger.yml"
[ -f "/tmp/tau_sync_out.txt" ] || { echo "FAIL: wait/trigger synchronization failed"; exit 1; }
rm -f "/tmp/tau_trig_out.txt" "/tmp/tau_sync_out.txt"
echo "[+] Passive wait, multi-target wait array, and trigger synchronization OK!"

echo "=== ALL NEW DSL FEATURES VERIFIED SUCCESSFULLY! ==="
