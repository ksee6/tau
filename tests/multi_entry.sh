#!/bin/bash
set -e

TAU="/home/xi/Repo/tau/build/tau"
TAU_INST="/home/xi/Repo/tau/build/tau-instance"

rm -rf /tmp/tau_multientry_test
mkdir -p /tmp/tau_multientry_test
cd /tmp/tau_multientry_test

INSTANCES_DIR="/run/user/$USER/tau/instances"
if [ ! -d "$INSTANCES_DIR" ]; then
    INSTANCES_DIR="/run/user/$(id -u)/tau/instances"
fi
if [ -n "$TAU_PATH_TEMP" ]; then
    INSTANCES_DIR="$TAU_PATH_TEMP/instances"
fi

echo "=== 1. Testing Multi-Entry Cascaded Slot Templating & Hot-Reloading ==="
# Template E0: host template with isolation, cgroup memory limit, and slot
cat << 'YAML' > host_template.yml
- name: host_controlled_pkg
- memory: 256M
- cpu: 200
- slot: true
YAML

# User manifest E1: user spawn
cat << 'YAML' > user_entry.yml
- spawn:
    name: user_worker
    command: "echo 'INITIAL_USER_RUN' > /tmp/tau_multientry_test/worker_out.txt; sleep 3"
    wait: true
YAML

# Run in background via tau run with both entries
$TAU run host_template.yml user_entry.yml --name multi_slot_inst -d

sleep 0.5
if [ ! -f "/tmp/tau_multientry_test/worker_out.txt" ]; then
    echo "ERROR: worker_out.txt not created"
    exit 1
fi
echo "Worker initial output: $(cat /tmp/tau_multientry_test/worker_out.txt)"

# Now dynamically modify user_entry.yml mid-run
cat << 'YAML' > user_entry.yml
- spawn:
    name: user_worker
    command: "echo 'INITIAL_USER_RUN' > /tmp/tau_multientry_test/worker_out.txt; sleep 3"
    wait: true
- spawn:
    name: hot_reload_spawn
    command: "echo 'HOT_RELOAD_SUCCESS' > /tmp/tau_multientry_test/hot_reload_out.txt"
    wait: true
YAML

# Give watcher time to detect modification and reload
sleep 0.6

if [ ! -f "/tmp/tau_multientry_test/hot_reload_out.txt" ]; then
    echo "ERROR: hot_reload_out.txt not created on user_entry modification"
    exit 1
fi
echo "Hot reload output: $(cat /tmp/tau_multientry_test/hot_reload_out.txt)"
$TAU stop multi_slot_inst || true
echo "Multi-entry & hot reload test OK!"

echo "=== 2. Testing --headless / -h Flag ==="
cat << 'YAML' > headless_entry.yml
- name: headless_test
- spawn:
    name: h_worker
    command: "echo 'HEADLESS_INIT' > /tmp/tau_multientry_test/h_out.txt; sleep 2"
    wait: true
YAML

$TAU run host_template.yml headless_entry.yml --name headless_inst --headless -d
sleep 0.5

# Modify headless_entry.yml - should NOT trigger hot reload
rm -f /tmp/tau_multientry_test/h_reload.txt
cat << 'YAML' > headless_entry.yml
- name: headless_test
- spawn:
    name: h_worker
    command: "echo 'HEADLESS_INIT' > /tmp/tau_multientry_test/h_out.txt; sleep 2"
    wait: true
- spawn:
    name: h_reload
    command: "echo 'SHOULD_NOT_RUN' > /tmp/tau_multientry_test/h_reload.txt"
    wait: true
YAML

sleep 0.5
if [ -f "/tmp/tau_multientry_test/h_reload.txt" ]; then
    echo "ERROR: headless instance watched and reloaded when --headless was specified"
    exit 1
fi
$TAU stop headless_inst || true
echo "Headless test OK!"

echo "=== 3. Testing --remove / -r Flag ==="
cat << 'YAML' > to_remove_entry.yml
- name: remove_test
- spawn:
    name: r_worker
    command: "echo 'REMOVE_RUN' > /tmp/tau_multientry_test/r_out.txt"
    wait: true
YAML

$TAU run to_remove_entry.yml --name remove_inst --remove -d
sleep 0.5

if [ -f "to_remove_entry.yml" ]; then
    echo "ERROR: to_remove_entry.yml was not deleted after --remove"
    exit 1
fi
if [ ! -f "/tmp/tau_multientry_test/r_out.txt" ]; then
    echo "ERROR: remove_inst did not run"
    exit 1
fi
$TAU stop remove_inst || true
echo "Remove test OK!"

echo "=== 4. Testing --global / -g Flag ==="
cat << 'YAML' > global_entry.yml
- name: global_test
- spawn:
    name: g_worker
    command: "echo 'GLOBAL_RUN' > /tmp/tau_multientry_test/g_out.txt; sleep 3"
    wait: true
YAML

$TAU run global_entry.yml --name global_inst --global -d
sleep 0.5

INST_YML="$INSTANCES_DIR/global_inst/instance.yml"
if [ ! -f "$INST_YML" ]; then
    echo "ERROR: instance.yml missing in $INST_YML"
    exit 1
fi

# Modify the global copy in instances directory
cat << 'YAML' >> "$INST_YML"
- spawn:
    name: g_reload
    command: "echo 'GLOBAL_WATCH_SUCCESS' > /tmp/tau_multientry_test/g_reload.txt"
    wait: true
YAML

sleep 0.6
if [ ! -f "/tmp/tau_multientry_test/g_reload.txt" ]; then
    echo "ERROR: global instance did not watch its instances/ copy"
    exit 1
fi
$TAU stop global_inst || true
echo "Global test OK!"

echo "=== 5. Testing Generated Bin Wrapper with --headless --remove ==="
cat << 'YAML' > bin_pkg.yml
- name: bin_wrapper_pkg
- bin:
    name: my_cli_tool
    description: Sample tool
- bindir: ./generated_bin
- spawn:
    name: worker
    command: "echo 'CLI_TOOL_EXECUTED' > /tmp/tau_multientry_test/cli_res.txt"
    wait: true
YAML

$TAU run bin_pkg.yml --name gen_bin_inst

if [ ! -x "./generated_bin/my_cli_tool" ]; then
    echo "ERROR: generated_bin/my_cli_tool missing or not executable"
    exit 1
fi

echo "Generated wrapper content:"
cat ./generated_bin/my_cli_tool

if ! grep -q -- "--headless" ./generated_bin/my_cli_tool || ! grep -q -- "--remove" ./generated_bin/my_cli_tool; then
    echo "ERROR: wrapper does not use --headless --remove"
    exit 1
fi

export PATH="/home/xi/Repo/tau/build:$PATH"
./generated_bin/my_cli_tool

if [ ! -f "/tmp/tau_multientry_test/cli_res.txt" ]; then
    echo "ERROR: cli_res.txt not produced"
    exit 1
fi

echo "=== ALL MULTI-ENTRY & TEMPLATE TESTS PASSED! ==="
