#!/bin/bash
set -e

TAU="/home/xi/Repo/tau/build/tau"

rm -rf /tmp/tau_paths_test
mkdir -p /tmp/tau_paths_test
cd /tmp/tau_paths_test

echo "=== 1. Testing Default Path Resolution ==="
# Unset variables
unset TAU_PATH
unset TAU_PATH_TEMP
unset TAU_GLOBAL

# Create a test manifest with a longer sleep
cat << 'YAML' > test_pkg.yml
- name: env_test_pkg
- spawn:
    name: worker
    command: "echo 'DEFAULT_PATHS_OK' > /tmp/tau_paths_test/out.txt; sleep 3"
    wait: true
YAML

$TAU run test_pkg.yml --name default_inst -d
sleep 0.5

EXPECTED_TEMP="/run/user/$USER/tau/instances/default_inst"
EXPECTED_TEMP_UID="/run/user/$(id -u)/tau/instances/default_inst"
if [ ! -d "$EXPECTED_TEMP" ] && [ ! -d "$EXPECTED_TEMP_UID" ]; then
    echo "ERROR: instance not created in default /run/user/<user>/tau/instances"
    exit 1
fi
echo "Instance found in /run/user/.../tau/instances successfully!"
$TAU stop default_inst || true

echo "=== 2. Testing Custom TAU_PATH_TEMP ==="
CUSTOM_TEMP="/tmp/tau_paths_test/custom_temp"
export TAU_PATH_TEMP="$CUSTOM_TEMP"

$TAU run test_pkg.yml --name custom_inst -d
sleep 0.5

if [ ! -d "$CUSTOM_TEMP/instances/custom_inst" ]; then
    echo "ERROR: instance not created in custom TAU_PATH_TEMP"
    exit 1
fi
echo "Custom TAU_PATH_TEMP verified: $CUSTOM_TEMP/instances/custom_inst"
$TAU stop custom_inst || true

echo "=== 3. Testing TAU_GLOBAL Manifest Linking in tau init ==="
export TAU_GLOBAL="/tmp/tau_paths_test/fake_global"
export TAU_PATH="/tmp/tau_paths_test/fake_user_path"
mkdir -p "$TAU_GLOBAL/manifests"
# Place executable fake tau-store in TAU_GLOBAL
cat << 'SH' > "$TAU_GLOBAL/tau-store"
#!/bin/sh
exit 0
SH
chmod +x "$TAU_GLOBAL/tau-store"

mkdir -p /tmp/tau_paths_test/sample_project
cd /tmp/tau_paths_test/sample_project
$TAU init

if [ ! -L "$TAU_GLOBAL/manifests/sample_project.yml" ]; then
    echo "ERROR: tau init did not link to TAU_GLOBAL/manifests when writable & tau-store executable"
    exit 1
fi
echo "tau init successfully linked to TAU_GLOBAL/manifests!"

echo "=== 4. Testing Fallback to TAU_PATH when TAU_GLOBAL is not writable or missing store ==="
# Remove tau-store from TAU_GLOBAL
rm -f "$TAU_GLOBAL/tau-store"
mkdir -p /tmp/tau_paths_test/sample_project2
cd /tmp/tau_paths_test/sample_project2
$TAU init

if [ ! -L "$TAU_PATH/manifests/sample_project2.yml" ]; then
    echo "ERROR: tau init did not fallback to TAU_PATH/manifests"
    exit 1
fi
echo "tau init successfully fell back to TAU_PATH/manifests!"

echo "=== ALL ENV PATH TESTS PASSED! ==="
