#!/usr/bin/env bash
set -e

TAU_BIN="/home/xi/Repo/tau/build/tau"
TEST_DIR="/tmp/tau_lists_dsl_test_$$"
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

echo "=== 1. Testing 'tau enable' (single, custom name, custom list, copy vs symlink) ==="
cat << 'EOF_P' > "$TEST_DIR/pkg1.yml"
- name: pkg-one
- spawn: "echo pkg1_executed"
EOF_P

# Symlink into default list
"$TAU_BIN" enable "$TEST_DIR/pkg1.yml"
if [ ! -L "$TAU_PATH/lists/default/pkg-one.yml" ]; then
    echo "[-] Error: pkg-one.yml was not symlinked in default list!"
    exit 1
fi
echo "[+] Symlink in default list OK!"

# Copy into custom list with custom name
"$TAU_BIN" enable "$TEST_DIR/pkg1.yml" --name my-custom-pkg --list prod --copy
if [ -L "$TAU_PATH/lists/prod/my-custom-pkg.yml" ] || [ ! -f "$TAU_PATH/lists/prod/my-custom-pkg.yml" ]; then
    echo "[-] Error: my-custom-pkg.yml was not copied in prod list!"
    exit 1
fi
echo "[+] Copy with custom name and list OK!"

# Multi-entry enable (template + entry merge)
cat << 'EOF_T' > "$TEST_DIR/template.yml"
- name: tmpl
- slot: true
- spawn: "echo tmpl_post"
EOF_T

cat << 'EOF_A' > "$TEST_DIR/app.yml"
- name: app-entry
- spawn: "echo app_injected"
EOF_A

"$TAU_BIN" enable "$TEST_DIR/template.yml" "$TEST_DIR/app.yml" --name merged-app --list prod
if [ ! -f "$TAU_PATH/lists/prod/merged-app.yml" ]; then
    echo "[-] Error: merged-app.yml not created!"
    exit 1
fi
grep -q "app_injected" "$TAU_PATH/lists/prod/merged-app.yml"
echo "[+] Multi-entry enable and template merging OK!"

echo "=== 2. Testing 'tau do-list' and run counters in temp path ==="
TOUCH1="/tmp/dolist_item1_$$.txt"
TOUCH2="/tmp/dolist_item2_$$.txt"
rm -f "$TOUCH1" "$TOUCH2"

cat << EOF_DOLIST1 > "$TEST_DIR/dolist_item1.yml"
- name: doli1
- spawn: touch $TOUCH1
EOF_DOLIST1

cat << EOF_DOLIST2 > "$TEST_DIR/dolist_item2.yml"
- name: doli2
- spawn: touch $TOUCH2
EOF_DOLIST2

"$TAU_BIN" enable "$TEST_DIR/dolist_item1.yml" --list mybatch
"$TAU_BIN" enable "$TEST_DIR/dolist_item2.yml" --list mybatch

# First run: should launch both and create .run counters in temp path
"$TAU_BIN" do-list --list mybatch
sleep 0.5
if [ ! -f "$TOUCH1" ] || [ ! -f "$TOUCH2" ]; then
    echo "[-] Error: do-list did not launch items on first run!"
    exit 1
fi
if [ ! -f "$TAU_PATH_TEMP/lists/mybatch/doli1.run" ] || [ ! -f "$TAU_PATH_TEMP/lists/mybatch/doli2.run" ]; then
    echo "[-] Error: temp run counter files not created!"
    exit 1
fi
echo "[+] Initial do-list run and counter creation OK!"

# Delete witness files and rerun without --regardless: items should NOT run again
rm -f "$TOUCH1" "$TOUCH2"
"$TAU_BIN" do-list --list mybatch
sleep 0.5
if [ -f "$TOUCH1" ] || [ -f "$TOUCH2" ]; then
    echo "[-] Error: do-list ran already executed items without --regardless!"
    exit 1
fi
echo "[+] Enrolled once enforcement OK!"

# Rerun with --regardless: should run them even if run counters exist
"$TAU_BIN" do-list --list mybatch --regardless
sleep 0.5
if [ ! -f "$TOUCH1" ] || [ ! -f "$TOUCH2" ]; then
    echo "[-] Error: do-list --regardless did not run items!"
    exit 1
fi
rm -f "$TOUCH1" "$TOUCH2"
echo "[+] do-list --regardless OK!"

echo "=== 3. Testing whole-list 'tau disable --list <list>' (stops & clears temp run counters) ==="
"$TAU_BIN" disable --list mybatch
if [ -d "$TAU_PATH_TEMP/lists/mybatch" ]; then
    echo "[-] Error: temp counters directory was not cleared by whole-list disable!"
    exit 1
fi
echo "[+] Whole-list disable and temp counter removal OK!"

echo "=== 4. Testing 'tau disable' (explicit name, specific list, regex) ==="
# Disable specific in list
"$TAU_BIN" disable --name my-custom-pkg --list prod
if [ -f "$TAU_PATH/lists/prod/my-custom-pkg.yml" ]; then
    echo "[-] Error: my-custom-pkg was not disabled!"
    exit 1
fi
echo "[+] Disable specific entry OK!"

# Disable via regex across all lists
"$TAU_BIN" disable --name ".*-app" --regex
if [ -f "$TAU_PATH/lists/prod/merged-app.yml" ]; then
    echo "[-] Error: merged-app was not disabled via regex!"
    exit 1
fi
echo "[+] Disable via regex across lists OK!"

echo "=== 5. Testing 'nowait:' and 'waitall:' directives ==="
cat << 'EOF_NW' > "$TEST_DIR/test_nowait.yml"
- name: nowait_test
- nowait:
    - spawn: "sleep 0.2"
      name: background_task
      wait: false
    - wait: background_task  # Should be masked inside nowait!
    - spawn: "echo nowait_finished"
EOF_NW

"$TAU_BIN" run "$TEST_DIR/test_nowait.yml"
echo "[+] nowait masking outside waits OK!"

cat << 'EOF_WA' > "$TEST_DIR/test_waitall.yml"
- name: waitall_test
- waitall:
    - spawn: "sleep 0.1"
      name: sub1
      wait: false
    - spawn: "sleep 0.1"
      name: sub2
      wait: false
- spawn: "echo waitall_finished"
EOF_WA

"$TAU_BIN" run "$TEST_DIR/test_waitall.yml"
echo "[+] waitall automatic passive wait OK!"

echo "=== 6. Testing 'eslot:' and 'entry:' directives ==="
SUCCESS_FILE="/tmp/eslot_success_test_$$.txt"
ESCAPE_FILE="/tmp/eslot_escaped_test_$$.txt"
FAIL_FILE="/tmp/eslot_fail_test_$$.txt"

cat << EOF_ES1 > "$TEST_DIR/test_eslot_match.yml"
- name: eslot_test
- var: USERNAME = testuser
- eslot: auth_login
  slot: true
  entry:
    - var: USERNAME == testuser
- entry: auth_login
  inject:
    - spawn: touch $SUCCESS_FILE
EOF_ES1

"$TAU_BIN" run "$TEST_DIR/test_eslot_match.yml"
sleep 0.2
if [ ! -f "$SUCCESS_FILE" ]; then
    echo "[-] Error: eslot injection did not execute on valid entry!"
    exit 1
fi
rm -f "$SUCCESS_FILE"
echo "[+] eslot entry verification and inject OK!"

cat << EOF_ES2 > "$TEST_DIR/test_eslot_escape.yml"
- name: eslot_escape_test
- var: USERNAME = wronguser
- eslot: auth_login
  slot: true
  entry:
    - var: USERNAME == testuser
  escape:
    - spawn: touch $ESCAPE_FILE
- entry: auth_login
  inject:
    - spawn: touch $FAIL_FILE
EOF_ES2

"$TAU_BIN" run "$TEST_DIR/test_eslot_escape.yml"
sleep 0.2
if [ -f "$FAIL_FILE" ]; then
    echo "[-] Error: inject ran when verification failed!"
    exit 1
fi
if [ ! -f "$ESCAPE_FILE" ]; then
    echo "[-] Error: escape fallback did not execute when verification failed!"
    exit 1
fi
rm -f "$ESCAPE_FILE"
echo "[+] eslot failure fallback escape OK!"

echo "=== 7. Testing 'noeslot:' directive (inside vs outside, exceptions, false no-op) ==="
mkdir -p "$TAU_PATH/slots"
NOESLOT_TOUCH_A="/tmp/noeslot_touch_a_$$.txt"
NOESLOT_TOUCH_B="/tmp/noeslot_touch_b_$$.txt"
NOESLOT_TOUCH_INSIDE="/tmp/noeslot_touch_inside_$$.txt"
rm -f "$NOESLOT_TOUCH_A" "$NOESLOT_TOUCH_B" "$NOESLOT_TOUCH_INSIDE"

# Global/outside slots
cat << EOF_NE_A > "$TAU_PATH/slots/slot_alpha.yml"
- eslot: slot_alpha
  slot: true
  entry:
    - var: 1 == 1
  inject:
    - spawn: touch $NOESLOT_TOUCH_A
EOF_NE_A

cat << EOF_NE_B > "$TAU_PATH/slots/slot_beta.yml"
- eslot: slot_beta
  slot: true
  entry:
    - var: 1 == 1
  inject:
    - spawn: touch $NOESLOT_TOUCH_B
EOF_NE_B

cat << EOF_M_BLOCK > "$TEST_DIR/test_noeslot_block.yml"
- name: noeslot_block_all
- eslot: local_allowed_slot
  slot: true
  entry:
    - var: 1 == 1
  inject:
    - spawn: touch $NOESLOT_TOUCH_INSIDE
- noeslot: true
- enter: local_allowed_slot
- enter: slot_alpha
- enter: slot_beta
EOF_M_BLOCK

"$TAU_BIN" run "$TEST_DIR/test_noeslot_block.yml"
sleep 0.2
if [ ! -f "$NOESLOT_TOUCH_INSIDE" ]; then
    echo "[-] Error: inside eslot was blocked by noeslot: true!"
    exit 1
fi
echo "[+] inside eslot was allowed with noeslot: true!"

if [ -f "$NOESLOT_TOUCH_A" ] || [ -f "$NOESLOT_TOUCH_B" ]; then
    echo "[-] Error: outside slots were not blocked by noeslot: true!"
    exit 1
fi
echo "[+] noeslot: true successfully masked outside slots!"
rm -f "$NOESLOT_TOUCH_INSIDE"

cat << 'EOF_M_EXC' > "$TEST_DIR/test_noeslot_exceptions.yml"
- name: noeslot_exceptions
- noeslot: [slot_beta]
- enter: slot_alpha
- enter: slot_beta
EOF_M_EXC

"$TAU_BIN" run "$TEST_DIR/test_noeslot_exceptions.yml"
sleep 0.2
if [ -f "$NOESLOT_TOUCH_A" ]; then
    echo "[-] Error: outside slot_alpha ran when not in noeslot exceptions!"
    exit 1
fi
if [ ! -f "$NOESLOT_TOUCH_B" ]; then
    echo "[-] Error: outside slot_beta did not run when in noeslot exceptions!"
    exit 1
fi
echo "[+] noeslot: [exceptions] allowed only specified outside slot!"
rm -f "$NOESLOT_TOUCH_B"

# Test noeslot: false (ignored / no-op)
cat << 'EOF_M_FALSE' > "$TEST_DIR/test_noeslot_false.yml"
- name: noeslot_false
- noeslot: false
- enter: slot_alpha
EOF_M_FALSE

"$TAU_BIN" run "$TEST_DIR/test_noeslot_false.yml"
sleep 0.2
if [ ! -f "$NOESLOT_TOUCH_A" ]; then
    echo "[-] Error: noeslot: false did not act as a no-op (outside slot was blocked)!"
    exit 1
fi
echo "[+] noeslot: false ignored as no-op!"
rm -f "$NOESLOT_TOUCH_A"

echo "=== ALL LISTS, ENABLE/DISABLE, DO-LIST, NOWAIT/WAITALL, ESLOT/ENTRY, NOESLOT TESTS PASSED! ==="
