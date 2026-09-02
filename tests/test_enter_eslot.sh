#!/usr/bin/env bash
set -e

TAU_BIN="/home/xi/Repo/tau/build/tau"
TEST_DIR="/tmp/tau_enter_eslot_test_$$"
export TAU_PATH="$TEST_DIR/tau_root"
export TAU_PATH_TEMP="$TEST_DIR/tau_temp"

rm -rf "$TEST_DIR"
mkdir -p "$TEST_DIR"
mkdir -p "$TAU_PATH"
mkdir -p "$TAU_PATH_TEMP"

cleanup() {
    pkill -9 -f "tau" || true
    rm -rf "$TEST_DIR"
}
trap cleanup EXIT

echo "=== 1. Testing --enter with global registered eslot ==="
mkdir -p "$TAU_PATH/slots"
GLOBAL_TOUCH="/tmp/global_eslot_touch_$$.txt"
rm -f "$GLOBAL_TOUCH"

cat << EOF_G > "$TAU_PATH/slots/global_auth.yml"
- eslot: global_auth
  slot: true
  entry:
    - var: 1 == 1
  escape:
    - spawn: touch $GLOBAL_TOUCH
  inject:
    - spawn: touch $GLOBAL_TOUCH
EOF_G

cat << 'EOF_M1' > "$TEST_DIR/manifest_no_eslot.yml"
- name: client_app
- spawn: "echo client_running"
EOF_M1

"$TAU_BIN" run "$TEST_DIR/manifest_no_eslot.yml" --enter global_auth
sleep 0.3
if [ ! -f "$GLOBAL_TOUCH" ]; then
    echo "[-] Error: --enter global_auth did not trigger global eslot!"
    exit 1
fi
rm -f "$GLOBAL_TOUCH"
echo "[+] --enter with global eslot OK!"

echo "=== 2. Testing --enter from deeply nested child process resolving parent eslot ==="
PARENT_TOUCH="/tmp/parent_eslot_touch_$$.txt"
rm -f "$PARENT_TOUCH"

cat << EOF_SUB > "$TEST_DIR/nested_script.sh"
#!/usr/bin/env bash
# Nested process level 1 launches level 2 which runs tau run --enter
bash -c "bash -c 'bash -c \"$TAU_BIN run $TEST_DIR/manifest_no_eslot.yml --enter parent_slot\"'"
EOF_SUB
chmod +x "$TEST_DIR/nested_script.sh"

cat << EOF_P > "$TEST_DIR/parent_manifest.yml"
- name: parent_inst
- eslot: parent_slot
  slot: true
  entry:
    - var: 1 == 1
  inject:
    - spawn: touch $PARENT_TOUCH
- spawn: "$TEST_DIR/nested_script.sh"
  wait: true
EOF_P

"$TAU_BIN" run "$TEST_DIR/parent_manifest.yml"
sleep 0.3

if [ ! -f "$PARENT_TOUCH" ]; then
    echo "[-] Error: --enter parent_slot did not resolve parent eslot through deep process nesting!"
    exit 1
fi
rm -f "$PARENT_TOUCH"
echo "[+] --enter from nested process resolving parent eslot OK!"

echo "=== 3. Testing variable and environment propagation to eslot entry and inject ==="
ENTRY_OUT="/tmp/env_test_entry_$$.txt"
INJECT_OUT="/tmp/env_test_inject_$$.txt"
rm -f "$ENTRY_OUT" "$INJECT_OUT"

cat << EOF_S_AUTH > "$TAU_PATH/slots/auth_slot.yml"
- eslot: auth_slot
  slot: true
  entry:
    - var: AUTH_TOKEN == secret123
    - spawn: echo "ENTRY_VERIFIED AUTH_USER=\$AUTH_USER" > $ENTRY_OUT
      wait: true
  inject:
    - spawn: echo "INJECT_RAN USER_PARAM=\$USER_PARAM" > $INJECT_OUT
      wait: true
EOF_S_AUTH

cat << 'EOF_M_CALL' > "$TEST_DIR/manifest_caller.yml"
- name: caller_test
- var: AUTH_TOKEN = secret123
- var: AUTH_USER = alice
- var: USER_PARAM = hello
- enter: auth_slot
EOF_M_CALL

"$TAU_BIN" run "$TEST_DIR/manifest_caller.yml"
sleep 0.2

if [ ! -f "$ENTRY_OUT" ]; then
    echo "[-] Error: eslot entry verification failed with caller vars!"
    exit 1
fi
if ! grep -q "AUTH_USER=alice" "$ENTRY_OUT"; then
    echo "[-] Error: AUTH_USER did not reach eslot entry!"
    exit 1
fi
echo "[+] Caller vars reached eslot entry successfully!"

if [ ! -f "$INJECT_OUT" ]; then
    echo "[-] Error: eslot inject did not execute!"
    exit 1
fi
if ! grep -q "USER_PARAM=hello" "$INJECT_OUT"; then
    echo "[-] Error: USER_PARAM did not reach eslot inject!"
    exit 1
fi
echo "[+] Caller vars reached eslot inject successfully!"
rm -f "$ENTRY_OUT" "$INJECT_OUT"

# Test CLI exported shell env propagation
export AUTH_TOKEN="secret123"
export AUTH_USER="bob"
export USER_PARAM="world"

"$TAU_BIN" run "$TEST_DIR/manifest_no_eslot.yml" --enter auth_slot
sleep 0.2

if [ ! -f "$ENTRY_OUT" ] || ! grep -q "AUTH_USER=bob" "$ENTRY_OUT"; then
    echo "[-] Error: CLI exported env did not reach eslot entry!"
    exit 1
fi
echo "[+] Shell env reached eslot entry via tau run --enter!"

if [ ! -f "$INJECT_OUT" ] || ! grep -q "USER_PARAM=world" "$INJECT_OUT"; then
    echo "[-] Error: CLI exported env did not reach eslot inject!"
    exit 1
fi
echo "[+] Shell env reached eslot inject via tau run --enter!"
rm -f "$ENTRY_OUT" "$INJECT_OUT"

echo "=== ALL --enter ESLOT TESTS PASSED! ==="
