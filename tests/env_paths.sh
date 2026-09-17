#!/bin/bash
set -e

TAU_BIN="/home/xi/Repo/tau/build/tau"

rm -rf /tmp/tau_paths_test
mkdir -p /tmp/tau_paths_test
cd /tmp/tau_paths_test

echo "=== 1. Testing Default Path Resolution ==="
# Unset variables
unset TAU_PATH
unset TAU_PATH_TEMP
unset TAU_GLOBAL
unset TAU
unset TAU_RUNTIME
unset TAU_STORE

# Create a test manifest with a longer sleep
cat << 'YAML' > test_pkg.yml
- name: env_test_pkg
- spawn: "echo 'DEFAULT_PATHS_OK' > /tmp/tau_paths_test/out.txt; sleep 3"
  name: worker
  wait: true
YAML

$TAU_BIN run test_pkg.yml --name default_inst -d
sleep 0.5

EXPECTED_TEMP="$HOME/.run/tau/instances/default_inst"
if [ ! -d "$EXPECTED_TEMP" ]; then
    USER_NAME="$(whoami)"
    EXPECTED_TEMP="/run/${USER_NAME}/tau/instances/default_inst"
fi
if [ ! -d "$EXPECTED_TEMP" ]; then
    echo "ERROR: instance not created in default TAU_RUNTIME instances: $EXPECTED_TEMP"
    exit 1
fi
echo "Instance found in $EXPECTED_TEMP successfully!"
$TAU_BIN stop default_inst || true

echo "=== 2. Testing Custom TAU_PATH_TEMP ==="
CUSTOM_TEMP="/tmp/tau_paths_test/custom_temp"
export TAU_PATH_TEMP="$CUSTOM_TEMP"

$TAU_BIN run test_pkg.yml --name custom_inst -d
sleep 0.5

if [ ! -d "$CUSTOM_TEMP/instances/custom_inst" ]; then
    echo "ERROR: instance not created in custom TAU_PATH_TEMP"
    exit 1
fi
echo "Custom TAU_PATH_TEMP verified: $CUSTOM_TEMP/instances/custom_inst"
$TAU_BIN stop custom_inst || true

echo "=== 3. Testing TAU_GLOBAL Manifest Linking in tau init ==="
export TAU_GLOBAL="/tmp/tau_paths_test/fake_global"
export TAU_PATH="/tmp/tau_paths_test/fake_user_path"
mkdir -p "$TAU_GLOBAL/manifests"
# Place executable fake tau in TAU_GLOBAL
cat << 'SH' > "$TAU_GLOBAL/tau"
#!/bin/sh
exit 0
SH
chmod +x "$TAU_GLOBAL/tau"

mkdir -p /tmp/tau_paths_test/sample_project
cd /tmp/tau_paths_test/sample_project
$TAU_BIN init

if [ -z "$(ls -A "$TAU_GLOBAL/manifests")" ]; then
    echo "ERROR: tau init did not link into TAU_GLOBAL/manifests when writable & global tau executable"
    exit 1
fi
echo "tau init successfully linked to TAU_GLOBAL/manifests!"

echo "=== 4. Testing Fallback to TAU_PATH when TAU_GLOBAL is not writable or missing tau ==="
# Remove tau from TAU_GLOBAL
rm -f "$TAU_GLOBAL/tau"
mkdir -p /tmp/tau_paths_test/sample_project2
cd /tmp/tau_paths_test/sample_project2
$TAU_BIN init

if [ -z "$(ls -A "$TAU_PATH/manifests")" ]; then
    echo "ERROR: tau init did not fallback to TAU_PATH/manifests"
    exit 1
fi
echo "tau init successfully fell back to TAU_PATH/manifests!"

echo "=== ALL ENV PATH TESTS PASSED! ==="
