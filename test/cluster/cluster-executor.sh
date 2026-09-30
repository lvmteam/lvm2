#!/bin/bash
#
# cluster-executor.sh - Test execution layer for LVM cluster testing
#
# This script provides the test execution infrastructure for running commands
# and tests on cluster nodes. It implements:
#   - Single node command execution
#   - Serial execution across all test nodes
#   - Parallel execution across all test nodes
#   - Test variable system ($node1, $node2, ..., $nodes, $nodep)
#   - Device variable system ($dev1, $dev2, ..., $devN)
#

# Source the core library for logging and utilities
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
source "$SCRIPT_DIR/cluster-test-lib.sh" || exit 1

#
# cluster_translate_devs - Translate node1 device paths to target node paths
#
# Device variables ($dev1, $dev2, etc.) are expanded on the host using node1's
# device paths before commands reach the dispatch functions.  When nodes have
# different device names for the same physical device (common — Linux device
# enumeration order varies), the literal paths are wrong for non-node1 targets.
#
# This function rewrites a command string, replacing every node1 device path
# with the corresponding path on the target node.  Uses a two-phase
# placeholder approach to avoid collisions when paths swap (e.g. node1
# /dev/sda,/dev/sdb but node3 /dev/sdb,/dev/sda).
#
# Requires CLUSTER_DEV_COUNT and CLUSTER_DEV_MAP_<node>_<idx> exported
# variables set by cluster_setup_device_vars().
#
cluster_translate_devs() {
    local node_num="$1"
    local cmd="$2"

    if [ "$node_num" -le 1 ]; then
        echo "$cmd"
        return
    fi

    if [ -z "${CLUSTER_DEV_COUNT:-}" ] || [ "$CLUSTER_DEV_COUNT" -eq 0 ]; then
        echo "$cmd"
        return
    fi

    local result="$cmd"

    # Phase 1: replace node1 paths with placeholders.
    # Sort by path length (longest first) so that longer paths like
    # /dev/mapper/mpathaa are replaced before shorter prefixes like
    # /dev/mapper/mpatha that would otherwise match as substrings.
    local -a _dev_order
    for i in $(seq 0 $((CLUSTER_DEV_COUNT - 1))); do
        local _varname="CLUSTER_DEV_MAP_1_${i}"
        local _p="${!_varname}"
        if [ -n "$_p" ]; then
            _dev_order+=("${#_p} $i")
        fi
    done
    IFS=$'\n' _dev_order=($(printf '%s\n' "${_dev_order[@]}" | sort -t' ' -k1,1 -rn)); unset IFS

    for _entry in "${_dev_order[@]}"; do
        local i="${_entry#* }"
        local _varname="CLUSTER_DEV_MAP_1_${i}"
        local node1_path="${!_varname}"
        result="${result//$node1_path/__CLUSTER_DEV_${i}__}"
    done

    # Phase 2: replace placeholders with target node paths
    for i in $(seq 0 $((CLUSTER_DEV_COUNT - 1))); do
        local _varname="CLUSTER_DEV_MAP_${node_num}_${i}"
        local target_path="${!_varname}"
        if [ -n "$target_path" ]; then
            result="${result//__CLUSTER_DEV_${i}__/$target_path}"
        fi
    done

    echo "$result"
}

#
# cluster_node_exec - Execute command on a specific node
#
# Executes a command on the specified cluster node via SSH and returns
# the output and exit code.
#
# Arguments:
#   $1 - node_num (0 for storage node, 1..N for test nodes)
#   $2 - command to execute
#
# Returns:
#   Exit code from the remote command
#
# Output:
#   stdout/stderr from the remote command
#
cluster_node_exec() {
    local node_num="$1"
    local cmd="$2"

    if [ -z "$node_num" ]; then
        cluster_error "cluster_node_exec: node_num is required"
        return 1
    fi

    if [ -z "$cmd" ]; then
        cluster_error "cluster_node_exec: command is required"
        return 1
    fi

    # Get the VM name for this node
    local vm_name=$(cluster_vm_get_name "$CLUSTER_ID" "$node_num")

    # Get the node IP
    local node_ip
    if ! node_ip=$(cluster_vm_get_ip "$vm_name" 2>/dev/null); then
        cluster_error "Failed to get IP for node $node_num ($vm_name)"
        return 1
    fi

    # Translate node1 device paths to this node's paths
    cmd=$(cluster_translate_devs "$node_num" "$cmd")

    # Execute command via SSH
    cluster_vm_ssh "$node_ip" "export NODE_NUM=$node_num; source /root/cluster-dev-map.sh 2>/dev/null; source /root/cluster-node-helpers.sh 2>/dev/null; source /root/cluster-test-helpers.sh 2>/dev/null; $cmd"
    return $?
}

#
# cluster_all_exec - Execute command serially on all test nodes
#
# Executes a command on all test nodes (1..N) in serial order.
# Stops on first failure.
#
# Arguments:
#   $1 - command to execute
#
# Returns:
#   0 if all executions succeeded
#   Exit code of first failed execution
#
cluster_all_exec() {
    local cmd="$1"

    if [ -z "$cmd" ]; then
        cluster_error "cluster_all_exec: command is required"
        return 1
    fi

    if [ -z "${CLUSTER_NUM_NODES:-}" ]; then
        cluster_error "CLUSTER_NUM_NODES not set"
        return 1
    fi

    local num_nodes="${CLUSTER_NUM_NODES}"

    # Execute on nodes 1..N (skip node 0 which is storage exporter)
    for node_num in $(seq 1 "$num_nodes"); do
        cluster_debug "Executing on node $node_num: $cmd"

        if cluster_node_exec "$node_num" "$cmd"; then
            echo "  node${node_num}: success"
        else
            local exit_code=$?
            echo "  node${node_num}: failed"
            return 1
        fi
    done

    return 0
}

#
# cluster_nodes_exec - Execute command in parallel on all test nodes
#
# Executes a command on all test nodes (1..N) in parallel using background jobs.
# Waits for all jobs to complete and collects results.
#
# Arguments:
#   $1 - command to execute
#
# Returns:
#   0 if all executions succeeded
#   1 if any execution failed
#
cluster_nodes_exec() {
    local cmd="$1"

    if [ -z "$cmd" ]; then
        cluster_error "cluster_nodes_exec: command is required"
        return 1
    fi

    if [ -z "${CLUSTER_NUM_NODES:-}" ]; then
        cluster_error "CLUSTER_NUM_NODES not set"
        return 1
    fi

    local num_nodes="${CLUSTER_NUM_NODES}"
    local pids=()
    local results=()
    local failed=0

    # Create temp directory for output files
    local tmpdir=$(mktemp -d)
    if [ ! -d "$tmpdir" ]; then
        echo "ERROR: Failed to create temp directory" >&2
        return 1
    fi
    trap "rm -rf '$tmpdir'" RETURN

    # Pre-resolve all node IPs to avoid function call issues in subshells
    local node_ips=()
    for node_num in $(seq 1 "$num_nodes"); do
        local vm_name=$(cluster_vm_get_name "$CLUSTER_ID" "$node_num")
        local node_ip
        if ! node_ip=$(cluster_vm_get_ip "$vm_name" 2>/dev/null); then
            echo "ERROR: Failed to get IP for node $node_num ($vm_name)" >&2
            return 1
        fi
        node_ips[$node_num]="$node_ip"
        cluster_debug "Node $node_num IP: $node_ip"
    done

    # Validate SSH key directory and user are set
    if [ -z "${CLUSTER_SSH_KEY_DIR:-}" ]; then
        echo "ERROR: CLUSTER_SSH_KEY_DIR not set" >&2
        return 1
    fi
    if [ -z "${CLUSTER_SSH_USER:-}" ]; then
        echo "ERROR: CLUSTER_SSH_USER not set" >&2
        return 1
    fi

    local ssh_key="${CLUSTER_SSH_KEY_DIR}/cluster_test_rsa"
    if [ ! -f "$ssh_key" ]; then
        echo "ERROR: SSH key not found: $ssh_key" >&2
        return 1
    fi

    cluster_debug "Using SSH key: $ssh_key"
    cluster_debug "SSH user: $CLUSTER_SSH_USER"

    # Launch background jobs for each node
    for node_num in $(seq 1 "$num_nodes"); do
        local output_file="$tmpdir/node${node_num}.out"
        local status_file="$tmpdir/node${node_num}.status"
        local node_ip="${node_ips[$node_num]}"

        cluster_debug "Starting background job for node $node_num (IP: $node_ip)"

        local node_cmd
        node_cmd=$(cluster_translate_devs "$node_num" "$cmd")

        bash -c "
            ssh -o StrictHostKeyChecking=no \
                -o UserKnownHostsFile=/dev/null \
                -o LogLevel=ERROR \
                -o ConnectTimeout=10 \
                -o ServerAliveInterval=5 \
                -o ServerAliveCountMax=3 \
                -i '$ssh_key' \
                '$CLUSTER_SSH_USER@$node_ip' \
                'export NODE_NUM=$node_num; source /root/cluster-dev-map.sh 2>/dev/null; source /root/cluster-node-helpers.sh 2>/dev/null; source /root/cluster-test-helpers.sh 2>/dev/null; $node_cmd' >'$output_file' 2>&1
            echo \$? > '$status_file'
        " &

        pids+=($!)
        cluster_debug "Launched parallel execution on node $node_num (PID: ${pids[-1]})"
    done

    # Wait for all background jobs
    cluster_debug "Waiting for ${#pids[@]} parallel executions to complete"
    for pid in "${pids[@]}"; do
        wait "$pid" 2>/dev/null || true
    done

    # Give background jobs a moment to flush output
    sleep 0.1

    # Collect results - explicitly write to stdout
    for node_num in $(seq 1 "$num_nodes"); do
        local output_file="$tmpdir/node${node_num}.out"
        local status_file="$tmpdir/node${node_num}.status"

        # Debug: Check if files exist
        if [ ! -f "$output_file" ]; then
            echo "ERROR: Output file not created for node $node_num" >&2
            echo "  Expected: $output_file" >&2
            failed=1
            continue
        fi

        if [ ! -f "$status_file" ]; then
            echo "ERROR: Status file not created for node $node_num" >&2
            echo "  Expected: $status_file" >&2
            failed=1
            continue
        fi

        # Get exit status
        local status=-1
        status=$(cat "$status_file" 2>/dev/null || echo "-1")

        # Print header with status to stdout
        if [ "$status" -eq 0 ]; then
            echo "  node${node_num}: success"
        else
            echo "  node${node_num}: failed"
            failed=1
        fi

        # Print output from this node (both stdout and stderr were captured)
        if [ -s "$output_file" ]; then
            cat "$output_file"
        else
            echo "  (no output)"
        fi
    done

    # Count successes and failures, track node numbers
    local success_nodes=()
    local fail_nodes=()

    for node_num in $(seq 1 "$num_nodes"); do
        local status_file="$tmpdir/node${node_num}.status"
        local status=-1

        if [ -f "$status_file" ]; then
            status=$(cat "$status_file" 2>/dev/null || echo "-1")
        fi

        if [ "$status" -eq 0 ]; then
            success_nodes+=($node_num)
        else
            fail_nodes+=($node_num)
        fi
    done

    # Export result variables
    export NODEP_TOTAL_COUNT=$num_nodes
    export NODEP_SUCCESS_COUNT=${#success_nodes[@]}
    export NODEP_FAIL_COUNT=${#fail_nodes[@]}
    export NODEP_SUCCESS_NODES="${success_nodes[*]}"
    export NODEP_FAIL_NODES="${fail_nodes[*]}"

    # Special case: exactly one success
    if [ ${#success_nodes[@]} -eq 1 ]; then
        export NODEP_SINGLE_SUCCESS_NODE="${success_nodes[0]}"
    else
        export NODEP_SINGLE_SUCCESS_NODE=""
    fi

    cluster_debug "nodep results: success=$NODEP_SUCCESS_COUNT fail=$NODEP_FAIL_COUNT"
    if [ -n "$NODEP_SINGLE_SUCCESS_NODE" ]; then
        cluster_debug "  Single success on node: $NODEP_SINGLE_SUCCESS_NODE"
    fi

    return $failed
}

#
# cluster_nodes_exec_per_node - Execute different commands on different nodes in parallel
#
# Similar to cluster_nodes_exec, but allows running different commands on each node.
# Sets the same NODEP_* environment variables for use with assert_* functions.
#
# Arguments:
#   $@ - node:command pairs in format "1:command1" "2:command2" ...
#
# Returns:
#   0 if all commands succeeded, 1 if any failed
#
cluster_nodes_exec_per_node() {
    if [ $# -eq 0 ]; then
        cluster_error "cluster_nodes_exec_per_node: at least one node:command pair required"
        return 1
    fi

    if [ -z "${CLUSTER_NUM_NODES:-}" ]; then
        cluster_error "CLUSTER_NUM_NODES not set"
        return 1
    fi

    # Parse node:command pairs
    declare -A node_commands
    declare -a target_nodes

    for arg in "$@"; do
        # Parse node:command format
        if [[ "$arg" =~ ^([0-9]+):(.+)$ ]]; then
            local node_num="${BASH_REMATCH[1]}"
            local command="${BASH_REMATCH[2]}"

            # Validate node number
            if [ "$node_num" -lt 1 ] || [ "$node_num" -gt "$CLUSTER_NUM_NODES" ]; then
                echo "ERROR: Invalid node number $node_num (valid: 1-$CLUSTER_NUM_NODES)" >&2
                return 1
            fi

            # Check for duplicates
            if [ -n "${node_commands[$node_num]:-}" ]; then
                echo "ERROR: Node $node_num specified multiple times" >&2
                return 1
            fi

            node_commands[$node_num]="$command"
            target_nodes+=($node_num)
        else
            echo "ERROR: Invalid format '$arg' - expected 'node:command'" >&2
            return 1
        fi
    done

    # Validate SSH key directory and user are set
    if [ -z "${CLUSTER_SSH_KEY_DIR:-}" ]; then
        echo "ERROR: CLUSTER_SSH_KEY_DIR not set" >&2
        return 1
    fi
    if [ -z "${CLUSTER_SSH_USER:-}" ]; then
        echo "ERROR: CLUSTER_SSH_USER not set" >&2
        return 1
    fi

    local ssh_key="${CLUSTER_SSH_KEY_DIR}/cluster_test_rsa"
    if [ ! -f "$ssh_key" ]; then
        echo "ERROR: SSH key not found: $ssh_key" >&2
        return 1
    fi

    # Create temp directory for output files
    local tmpdir=$(mktemp -d)
    if [ ! -d "$tmpdir" ]; then
        echo "ERROR: Failed to create temp directory" >&2
        return 1
    fi
    trap "rm -rf '$tmpdir'" RETURN

    # Pre-resolve node IPs for target nodes
    local node_ips=()
    for node_num in "${target_nodes[@]}"; do
        local vm_name=$(cluster_vm_get_name "$CLUSTER_ID" "$node_num")
        local node_ip
        if ! node_ip=$(cluster_vm_get_ip "$vm_name" 2>/dev/null); then
            echo "ERROR: Failed to get IP for node $node_num ($vm_name)" >&2
            return 1
        fi
        node_ips[$node_num]="$node_ip"
        cluster_debug "Node $node_num IP: $node_ip"
    done

    # Launch background jobs for each target node
    local pids=()
    declare -A pid_to_node

    for node_num in "${target_nodes[@]}"; do
        local command="${node_commands[$node_num]}"
        local output_file="$tmpdir/node${node_num}.out"
        local status_file="$tmpdir/node${node_num}.status"
        local node_ip="${node_ips[$node_num]}"

        cluster_debug "Starting node $node_num: $command"

        local node_cmd
        node_cmd=$(cluster_translate_devs "$node_num" "$command")

        bash -c "
            ssh -o StrictHostKeyChecking=no \
                -o UserKnownHostsFile=/dev/null \
                -o LogLevel=ERROR \
                -o ConnectTimeout=10 \
                -o ServerAliveInterval=5 \
                -o ServerAliveCountMax=3 \
                -i '$ssh_key' \
                '$CLUSTER_SSH_USER@$node_ip' \
                'export NODE_NUM=$node_num; source /root/cluster-dev-map.sh 2>/dev/null; source /root/cluster-node-helpers.sh 2>/dev/null; source /root/cluster-test-helpers.sh 2>/dev/null; $node_cmd' >'$output_file' 2>&1
            echo \$? > '$status_file'
        " &

        local pid=$!
        pids+=($pid)
        pid_to_node[$pid]=$node_num
        cluster_debug "Launched on node $node_num (PID: $pid)"
    done

    # Wait for all background jobs
    cluster_debug "Waiting for ${#pids[@]} parallel executions to complete"
    for pid in "${pids[@]}"; do
        wait "$pid" 2>/dev/null || true
    done

    # Give background jobs a moment to flush output
    sleep 0.1

    # Collect results for target nodes only
    local failed=0
    for node_num in "${target_nodes[@]}"; do
        local output_file="$tmpdir/node${node_num}.out"
        local status_file="$tmpdir/node${node_num}.status"

        if [ ! -f "$status_file" ]; then
            echo "ERROR: Status file not created for node $node_num" >&2
            failed=1
            continue
        fi

        # Get exit status
        local status=$(cat "$status_file" 2>/dev/null || echo "-1")

        # Print header with status
        if [ "$status" -eq 0 ]; then
            echo "  node${node_num}: success"
        else
            echo "  node${node_num}: failed"
            failed=1
        fi

        # Print output
        if [ -s "$output_file" ]; then
            cat "$output_file"
        else
            echo "  (no output)"
        fi
    done

    # Count successes and failures across ALL nodes (not just target nodes)
    # Nodes not in target list are counted as "not run" / failed
    local success_nodes=()
    local fail_nodes=()

    for node_num in $(seq 1 "$CLUSTER_NUM_NODES"); do
        local status_file="$tmpdir/node${node_num}.status"

        # Check if this node was targeted
        if [ -n "${node_commands[$node_num]:-}" ]; then
            # Node was targeted - check its status
            local status=-1
            if [ -f "$status_file" ]; then
                status=$(cat "$status_file" 2>/dev/null || echo "-1")
            fi

            if [ "$status" -eq 0 ]; then
                success_nodes+=($node_num)
            else
                fail_nodes+=($node_num)
            fi
        else
            # Node was not targeted - count as failed
            fail_nodes+=($node_num)
        fi
    done

    # Export result variables (same as cluster_nodes_exec for compatibility)
    export NODEP_TOTAL_COUNT=$CLUSTER_NUM_NODES
    export NODEP_SUCCESS_COUNT=${#success_nodes[@]}
    export NODEP_FAIL_COUNT=${#fail_nodes[@]}
    export NODEP_SUCCESS_NODES="${success_nodes[*]}"
    export NODEP_FAIL_NODES="${fail_nodes[*]}"

    # Special case: exactly one success
    if [ ${#success_nodes[@]} -eq 1 ]; then
        export NODEP_SINGLE_SUCCESS_NODE="${success_nodes[0]}"
    else
        export NODEP_SINGLE_SUCCESS_NODE=""
    fi

    cluster_debug "nodepp results: success=$NODEP_SUCCESS_COUNT fail=$NODEP_FAIL_COUNT"
    if [ -n "$NODEP_SINGLE_SUCCESS_NODE" ]; then
        cluster_debug "  Single success on node: $NODEP_SINGLE_SUCCESS_NODE"
    fi

    return $failed
}

#
# cluster_setup_test_vars - Set up test variable system
#
# Creates bash functions for test variables:
#   $node1, $node2, ..., $nodeN - Execute on specific node
#   $nodes - Execute serially on all test nodes
#   $nodep - Execute in parallel on all test nodes (same command)
#   $nodepp - Execute in parallel on different nodes (different commands)
#   $success_node - Execute on the node that succeeded (after nodep/nodepp)
#   $node_rand - Execute on a randomly selected test node
#
# These functions allow natural syntax in test scripts:
#   $node1 vgcreate vg /dev/sdb
#   $nodes vgchange --lock-start vg
#   $nodep lvs vg
#   $nodepp 1:"lvs" 2:"vgs" 3:"pvs"
#   $nodep lvcreate -L 10M -n lv1 testvg || true
#   $success_node lvchange -ay testvg/lv1
#   $node_rand lvchange -ay testvg/lv2
#
# Arguments:
#   None (uses CLUSTER_NUM_NODES from environment)
#
# Returns:
#   0 on success, 1 on failure
#
cluster_setup_test_vars() {
    if [ -z "${CLUSTER_NUM_NODES:-}" ]; then
        cluster_error "CLUSTER_NUM_NODES not set"
        return 1
    fi

    local num_nodes="${CLUSTER_NUM_NODES}"

    cluster_debug "Setting up test variables for $num_nodes node(s)"

    # Create node1, node2, ..., nodeN functions
    for node_num in $(seq 1 "$num_nodes"); do
        local func_name="node${node_num}"

        # Create function dynamically
        eval "
        $func_name() {
            echo \"$func_name \$*\" >&2
            cluster_node_exec $node_num \"\$*\"
        }
        export -f $func_name
        "

        cluster_debug "Created test variable: \$$func_name"
    done

    # Create $nodes function (serial execution)
    nodes() {
        echo "nodes $*" >&2
        cluster_all_exec "$*"
    }
    export -f nodes
    cluster_debug "Created test variable: \$nodes (serial execution)"

    # Create $nodep function (parallel execution)
    # After parallel execution, sets these variables:
    #   NODEP_TOTAL_COUNT - total number of nodes
    #   NODEP_SUCCESS_COUNT - number of nodes where command succeeded (exit 0)
    #   NODEP_FAIL_COUNT - number of nodes where command failed (exit != 0)
    #   NODEP_SUCCESS_NODES - space-separated list of successful node numbers
    #   NODEP_FAIL_NODES - space-separated list of failed node numbers
    #   NODEP_SINGLE_SUCCESS_NODE - node number if exactly one succeeded, else empty
    nodep() {
        echo "nodep $*" >&2
        cluster_nodes_exec "$*"
    }
    export -f nodep
    cluster_debug "Created test variable: \$nodep (parallel execution)"

    # Create $nodepp function (parallel execution with different commands per node)
    # After parallel execution, sets the same NODEP_* variables as nodep:
    #   NODEP_TOTAL_COUNT - total number of nodes
    #   NODEP_SUCCESS_COUNT - number of nodes where command succeeded (exit 0)
    #   NODEP_FAIL_COUNT - number of nodes where command failed (exit != 0)
    #   NODEP_SUCCESS_NODES - space-separated list of successful node numbers
    #   NODEP_FAIL_NODES - space-separated list of failed node numbers
    #   NODEP_SINGLE_SUCCESS_NODE - node number if exactly one succeeded, else empty
    nodepp() {
        echo "nodepp $*" >&2
        cluster_nodes_exec_per_node "$@"
    }
    export -f nodepp
    cluster_debug "Created test variable: \$nodepp (parallel per-node execution)"

    # Create $success_node function - execute on the single success node
    # This is a convenience wrapper for: eval "node${NODEP_SINGLE_SUCCESS_NODE} <command>"
    # Must be called after a nodep or nodepp operation that resulted in exactly one success
    success_node() {
        if [ -z "${NODEP_SINGLE_SUCCESS_NODE:-}" ]; then
            echo "ERROR: success_node requires NODEP_SINGLE_SUCCESS_NODE to be set" >&2
            echo "  This variable is set by nodep/nodepp when exactly one node succeeds" >&2
            echo "  Current NODEP_SUCCESS_COUNT: ${NODEP_SUCCESS_COUNT:-not set}" >&2
            return 1
        fi

        local node_num="$NODEP_SINGLE_SUCCESS_NODE"
        echo "success_node (node${node_num}) $*" >&2
        cluster_node_exec "$node_num" "$*"
    }
    export -f success_node
    cluster_debug "Created test variable: \$success_node (execute on single success node)"

    # Create $node_rand function - execute on a randomly selected test node
    # Useful for testing operations that should work on any node
    node_rand() {
        # Generate random node number from 1 to CLUSTER_NUM_NODES
        local random_node=$((1 + RANDOM % CLUSTER_NUM_NODES))
        echo "node_rand (selected node${random_node}) $*" >&2
        cluster_node_exec "$random_node" "$*"
    }
    export -f node_rand
    cluster_debug "Created test variable: \$node_rand (execute on random node)"

    # Create $noden function - execute on a specified node number
    # Usage: noden <node_num> <command>
    # This is a convenience wrapper for dynamic node selection
    noden() {
        local node_num="$1"
        shift

        if [ -z "$node_num" ]; then
            echo "ERROR: noden requires a node number as first argument" >&2
            echo "  Usage: noden <node_num> <command>" >&2
            return 1
        fi

        if ! [[ "$node_num" =~ ^[0-9]+$ ]]; then
            echo "ERROR: noden node number must be numeric, got: '$node_num'" >&2
            return 1
        fi

        if [ "$node_num" -lt 1 ] || [ "$node_num" -gt "$CLUSTER_NUM_NODES" ]; then
            echo "ERROR: noden node number $node_num out of range (1-$CLUSTER_NUM_NODES)" >&2
            return 1
        fi

        echo "noden (node${node_num}) $*" >&2
        cluster_node_exec "$node_num" "$*"
    }
    export -f noden
    cluster_debug "Created test variable: \$noden (execute on specified node)"

    # Also export node0 for storage node (though tests typically don't use it)
    node0() {
        echo "node0 $*" >&2
        cluster_node_exec 0 "$*"
    }
    export -f node0
    cluster_debug "Created test variable: \$node0 (storage exporter)"

    # Stress test support: arrays to hold registered loop definitions
    STRESS_LOOP_NAMES=()
    STRESS_LOOP_BODIES=()
    STRESS_LOOP_NODES=()

    # stress_loop <name> <body> - register a named loop for stress testing
    # Loop body is a shell fragment that will be run repeatedly on each node.
    # Use single quotes for the body so variables like $(hostname) and $dev1
    # expand on the remote node, not locally.
    stress_loop() {
        local name="$1"
        local body="$2"

        if [ -z "$name" ]; then
            echo "ERROR: stress_loop requires a name" >&2
            return 1
        fi
        if [ -z "$body" ]; then
            echo "ERROR: stress_loop '$name' requires a body" >&2
            return 1
        fi

        STRESS_LOOP_NAMES+=("$name")
        STRESS_LOOP_BODIES+=("$body")
        STRESS_LOOP_NODES+=("all")
        echo "stress_loop $name" >&2
    }
    export -f stress_loop

    # stress_loop_on <nodes> <name> <body> - register a loop for specific nodes only
    # <nodes> is a space-separated list of node numbers, e.g. "1 3"
    stress_loop_on() {
        local nodes="$1"
        local name="$2"
        local body="$3"

        if [ -z "$nodes" ] || [ -z "$name" ] || [ -z "$body" ]; then
            echo "ERROR: stress_loop_on requires: <nodes> <name> <body>" >&2
            return 1
        fi

        STRESS_LOOP_NAMES+=("$name")
        STRESS_LOOP_BODIES+=("$body")
        STRESS_LOOP_NODES+=("$nodes")
        echo "stress_loop_on $nodes $name" >&2
    }
    export -f stress_loop_on

    # stress <duration> - run all registered stress loops on nodes
    # duration format: Nmin (minutes) or Nsec (seconds), e.g. "5min" or "30sec"
    stress() {
        local duration="$1"

        if [ -z "$duration" ]; then
            echo "ERROR: stress requires a duration, e.g. 5min or 30sec" >&2
            return 1
        fi

        local duration_secs
        if [[ "$duration" =~ ^([0-9]+)min$ ]]; then
            duration_secs=$(( ${BASH_REMATCH[1]} * 60 ))
        elif [[ "$duration" =~ ^([0-9]+)sec$ ]]; then
            duration_secs=${BASH_REMATCH[1]}
        else
            echo "ERROR: stress duration must be Nmin or Nsec, got: $duration" >&2
            return 1
        fi

        if [ "$duration_secs" -lt 1 ]; then
            echo "ERROR: stress duration must be at least 1sec" >&2
            return 1
        fi

        if [ ${#STRESS_LOOP_NAMES[@]} -eq 0 ]; then
            echo "ERROR: no stress loops defined (use stress_loop first)" >&2
            return 1
        fi

        echo "stress $duration" >&2
        cluster_stress_run "$duration_secs"
        local rc=$?

        # Clear the registry for potential reuse
        STRESS_LOOP_NAMES=()
        STRESS_LOOP_BODIES=()
        STRESS_LOOP_NODES=()

        return $rc
    }
    export -f stress

    cluster_debug "Created stress test functions: stress_loop, stress_loop_on, stress"

    # Create safety wrappers for commands to prevent local execution
    # These will error if someone forgets to use node1, node2, etc.
    _command_error() {
        local cmd="$1"
        shift
        local caller_line="${BASH_LINENO[0]}"
        local caller_file="${BASH_SOURCE[1]}"
        caller_file="$(basename "$caller_file")"

        echo "" >&2
        echo "ERROR: command '$cmd' called without node prefix" >&2
        echo "  at $caller_file:$caller_line" >&2
        echo "" >&2
        echo "Commands must be executed on test nodes using:" >&2
        echo "  node1 $cmd $*" >&2
        echo "  node2 $cmd $*" >&2
        echo "  noden N $cmd $*    (specific node N)" >&2
        echo "  nodes $cmd $*      (all nodes serially)" >&2
        echo "  nodep $cmd $* || true  (all nodes in parallel)" >&2
        echo "  node_rand $cmd $*  (random node)" >&2
        echo "" >&2
        exit 1
    }

    # Wrapper functions for LVM commands
    vgcreate()      { _command_error "vgcreate" "$@"; }
    vgremove()      { _command_error "vgremove" "$@"; }
    vgchange()      { _command_error "vgchange" "$@"; }
    vgrename()      { _command_error "vgrename" "$@"; }
    vgextend()      { _command_error "vgextend" "$@"; }
    vgreduce()      { _command_error "vgreduce" "$@"; }
    vgexport()      { _command_error "vgexport" "$@"; }
    vgimport()      { _command_error "vgimport" "$@"; }
    vgck()          { _command_error "vgck" "$@"; }
    vgcfgbackup()   { _command_error "vgcfgbackup" "$@"; }
    vgcfgrestore()  { _command_error "vgcfgrestore" "$@"; }
    vgimportdevices() { _command_error "vgimportdevices" "$@"; }
    lvcreate()      { _command_error "lvcreate" "$@"; }
    lvremove()      { _command_error "lvremove" "$@"; }
    lvchange()      { _command_error "lvchange" "$@"; }
    lvrename()      { _command_error "lvrename" "$@"; }
    lvextend()      { _command_error "lvextend" "$@"; }
    lvreduce()      { _command_error "lvreduce" "$@"; }
    lvresize()      { _command_error "lvresize" "$@"; }
    lvconvert()     { _command_error "lvconvert" "$@"; }
    pvcreate()      { _command_error "pvcreate" "$@"; }
    pvremove()      { _command_error "pvremove" "$@"; }
    pvchange()      { _command_error "pvchange" "$@"; }
    pvresize()      { _command_error "pvresize" "$@"; }
    pvmove()        { _command_error "pvmove" "$@"; }
    vgs()           { _command_error "vgs" "$@"; }
    lvs()           { _command_error "lvs" "$@"; }
    pvs()           { _command_error "pvs" "$@"; }
    vgdisplay()     { _command_error "vgdisplay" "$@"; }
    lvdisplay()     { _command_error "lvdisplay" "$@"; }
    pvdisplay()     { _command_error "pvdisplay" "$@"; }
    vgscan()        { _command_error "vgscan" "$@"; }
    lvscan()        { _command_error "lvscan" "$@"; }
    pvscan()        { _command_error "pvscan" "$@"; }
    lvmconfig()     { _command_error "lvmconfig" "$@"; }
    lvmdevices()    { _command_error "lvmdevices" "$@"; }
    lvmlockctl()    { _command_error "lvmlockctl" "$@"; }
    lvmpersist()    { _command_error "lvmpersist" "$@"; }

    # Wrapper functions for non-LVM commands
    wipefs()        { _command_error "wipefs" "$@"; }
    mdadm()         { _command_error "mdadm" "$@"; }
    cryptsetup()    { _command_error "cryptsetup" "$@"; }
    dmsetup()       { _command_error "dmsetup" "$@"; }
    dd()            { _command_error "dd" "$@"; }
    xxd()           { _command_error "xxd" "$@"; }
    systemctl()     { _command_error "systemctl" "$@"; }
    mkfs()          { _command_error "mkfs" "$@"; }
    mkfs.ext2()     { _command_error "mkfs.ext2" "$@"; }
    mkfs.ext3()     { _command_error "mkfs.ext3" "$@"; }
    mkfs.ext4()     { _command_error "mkfs.ext4" "$@"; }
    mkfs.xfs()      { _command_error "mkfs.xfs" "$@"; }
    mkfs.btrfs()    { _command_error "mkfs.btrfs" "$@"; }
    mount()         { _command_error "mount" "$@"; }
    umount()        { _command_error "umount" "$@"; }
    blockdev()      { _command_error "blockdev" "$@"; }
    blkid()         { _command_error "blkid" "$@"; }
    losetup()       { _command_error "losetup" "$@"; }
    fdisk()         { _command_error "fdisk" "$@"; }
    sanlock()       { _command_error "sanlock" "$@"; }
    modprobe()      { _command_error "modprobe" "$@"; }
    reboot()        { _command_error "reboot" "$@"; }
    nvme()          { _command_error "nvme" "$@"; }

    # sg_* SCSI generic commands
    local _sg_cmds="sg_persist sg_inq sg_raw sg_dd sgm_dd sgp_dd sgdisk sg_reset
        sg_format sg_sanitize sg_write_same sg_write_buffer sg_write_x sg_unmap
        sg_start sg_ses sg_ses_microcode sg_senddiag sg_luns sg_logs sg_modes
        sg_opcodes sg_readcap sg_reassign sg_verify sg_vpd sg_turs sg_sync
        sg_compare_and_write sg_xcopy sg_copy_results sg_seek sg_stream_ctl
        sg_get_config sg_get_elem_status sg_get_lba_status sg_zone sg_z_act_query
        sg_rep_zones sg_reset_wp sg_rem_rest_elem sg_prevent sg_scan sg_map
        sg_map26 sg_rbuf sg_read sg_read_attr sg_read_buffer sg_read_long
        sg_write_long sg_write_attr sg_write_verify sg_wr_mode sg_timestamp
        sg_sat_identify sg_sat_set_features sg_sat_read_gplog sg_sat_datetime
        sg_sat_phy_event sg_ident sg_rmsn sg_safte sg_rtpg sg_stpg sg_rdac
        sg_emc_trespass sg_requests sg_decode_sense sg_test_rwbuf sg_bg_ctl
        sg_rep_density sg_rep_pip sg_read_block_limits sginfo sgpio sg"
    for _sg_cmd in $_sg_cmds; do
        eval "${_sg_cmd}() { _command_error \"${_sg_cmd}\" \"\$@\"; }"
        export -f "${_sg_cmd}"
    done

    export -f _command_error
    export -f vgcreate vgremove vgchange vgrename vgextend vgreduce
    export -f vgexport vgimport vgck vgcfgbackup vgcfgrestore vgimportdevices
    export -f lvcreate lvremove lvchange lvrename lvextend lvreduce lvresize lvconvert
    export -f pvcreate pvremove pvchange pvresize pvmove
    export -f vgs lvs pvs vgdisplay lvdisplay pvdisplay
    export -f vgscan lvscan pvscan
    export -f lvmconfig lvmdevices lvmlockctl lvmpersist
    export -f wipefs mdadm cryptsetup dmsetup dd xxd nvme
    export -f systemctl mkfs mkfs.ext2 mkfs.ext3 mkfs.ext4 mkfs.xfs mkfs.btrfs
    export -f mount umount blockdev blkid losetup fdisk sanlock modprobe reboot

    cluster_debug "Created command safety wrappers"

    cluster_log "Test variables set up successfully"
    return 0
}

#
# cluster_discover_devices_on_node - Discover devices on a specific node
#
# Queries a test node to discover imported storage devices.
#
# Arguments:
#   $1 - node_num
#
# Output:
#   List of device paths (one per line)
#
# Returns:
#   0 on success, 1 on failure
#
cluster_discover_devices_on_node() {
    local node_num="$1"
    local skip_rescan="${2:-0}"  # Optional: skip slow rescans if devices already known

    if [ -z "$node_num" ]; then
        cluster_error "cluster_discover_devices_on_node: node_num is required"
        return 1
    fi

    local num_scsi="${CLUSTER_NUM_SCSI:-0}"
    local num_nvme="${CLUSTER_NUM_NVME:-0}"
    local num_multipath="${CLUSTER_NUM_MULTIPATH:-0}"

    # Build discovery script
    local discovery_script="
    set -e

    # Fast discovery without rescanning (devices already imported during cluster setup)
    if [ $skip_rescan -eq 1 ]; then
        # Direct discovery without slow rescan operations
        # Discover devices in order: SCSI, NVMe, then multipath

        # Discover iSCSI SCSI devices (excluding multipath backing devices)
        if [ $num_scsi -gt 0 ]; then
            # Get all LIO-ORG SCSI devices
            scsi_devs=\$(lsblk -d -n -o NAME,VENDOR 2>/dev/null | grep -i 'LIO-ORG' | awk '{print \$1}')

            # If multipath is enabled, filter out devices that are multipath slaves
            if [ $num_multipath -gt 0 ] && [ -n \"\$scsi_devs\" ]; then
                # Get list of multipath slave devices
                mpath_slaves=\$(multipath -ll 2>/dev/null | grep -oP '[0-9]+:[0-9]+:[0-9]+:[0-9]+\\s+\\K\\w+' || true)

                # Filter out slaves from SCSI device list
                for dev in \$scsi_devs; do
                    if ! echo \"\$mpath_slaves\" | grep -qw \"\$dev\"; then
                        echo \"/dev/\$dev\"
                    fi
                done | sort
            else
                # No multipath, output all SCSI devices
                for dev in \$scsi_devs; do
                    echo \"/dev/\$dev\"
                done | sort
            fi
        fi

        # Discover NVMe devices
        if [ $num_nvme -gt 0 ]; then
            nvme list 2>/dev/null | grep -oP '/dev/nvme[0-9]+n[0-9]+' | sort
        fi

        # Discover multipath devices
        if [ $num_multipath -gt 0 ]; then
            multipath -ll 2>/dev/null | grep -oP '^[a-z0-9]+(?= \()' | sort | while read mpath; do
                echo \"/dev/mapper/\$mpath\"
            done
        fi
    # Source the importer library if available (includes rescan - slower but thorough)
    elif [ -f /root/cluster-scripts/cluster-test-lib.sh ]; then
        source /root/cluster-scripts/cluster-test-lib.sh
        source /root/cluster-scripts/cluster-storage-importer.sh

        # Use the built-in discovery function
        # Filter output to only device paths (in case of stdout pollution from rescan)
        cluster_storage_discover_devices $num_scsi $num_nvme $num_multipath 2>/dev/null | grep '^/dev/' || exit 1
    else
        # Fallback: manual discovery
        # Discover devices in order: SCSI, NVMe, then multipath

        # Discover iSCSI SCSI devices (excluding multipath backing devices)
        if [ $num_scsi -gt 0 ]; then
            # Get all LIO-ORG SCSI devices
            scsi_devs=\$(lsblk -d -n -o NAME,VENDOR 2>/dev/null | grep -i 'LIO-ORG' | awk '{print \$1}')

            # If multipath is enabled, filter out devices that are multipath slaves
            if [ $num_multipath -gt 0 ] && [ -n \"\$scsi_devs\" ]; then
                # Get list of multipath slave devices
                mpath_slaves=\$(multipath -ll 2>/dev/null | grep -oP '[0-9]+:[0-9]+:[0-9]+:[0-9]+\\s+\\K\\w+' || true)

                # Filter out slaves from SCSI device list
                for dev in \$scsi_devs; do
                    if ! echo \"\$mpath_slaves\" | grep -qw \"\$dev\"; then
                        echo \"/dev/\$dev\"
                    fi
                done | sort
            else
                # No multipath, output all SCSI devices
                for dev in \$scsi_devs; do
                    echo \"/dev/\$dev\"
                done | sort
            fi
        fi

        # Discover NVMe devices
        if [ $num_nvme -gt 0 ]; then
            nvme list 2>/dev/null | grep -oP '/dev/nvme[0-9]+n[0-9]+' | sort
        fi

        # Discover multipath devices
        if [ $num_multipath -gt 0 ]; then
            multipath -ll 2>/dev/null | grep -oP '^[a-z0-9]+(?= \()' | sort | while read mpath; do
                echo \"/dev/mapper/\$mpath\"
            done
        fi
    fi
    "

    cluster_node_exec "$node_num" "$discovery_script"
    return $?
}

#
# cluster_setup_device_vars - Set up device variable system
#
# Discovers imported storage devices on all test nodes and creates
# device variables: $dev1, $dev2, ..., $devN
#
# These variables point to the same physical devices across all nodes,
# allowing tests to reference devices consistently:
#   $node1 vgcreate vg $dev1 $dev2
#   $node2 vgchange --lock-start vg
#
# Also exports DEVICES array with all device paths.
#
# Arguments:
#   None (uses cluster configuration from environment)
#
# Returns:
#   0 on success, 1 on failure
#
cluster_setup_device_vars() {
    if [ -z "${CLUSTER_NUM_NODES:-}" ]; then
        cluster_error "CLUSTER_NUM_NODES not set"
        return 1
    fi

    cluster_log "Discovering storage devices on test nodes"

    # Discover devices on node 1 (use as reference)
    # Use skip_rescan=1 because devices were already imported during cluster setup
    local devices=()
    while IFS= read -r dev; do
        if [ -n "$dev" ]; then
            devices+=("$dev")
        fi
    done < <(cluster_discover_devices_on_node 1 1)

    if [ ${#devices[@]} -eq 0 ]; then
        cluster_error "No storage devices found on node 1"
        local _diag_vm=$(cluster_vm_get_name "$CLUSTER_ID" 1)
        cluster_error "VM state: $(cluster_virsh domstate "$_diag_vm" 2>&1)"
        cluster_error "VM IPs: $(cluster_virsh domifaddr "$_diag_vm" 2>&1)"
        return 1
    fi

    cluster_log "Found ${#devices[@]} device(s) on node 1"

    # Get WWNs for devices on node 1 (for verification)
    # Batch all WWN lookups in one SSH call for speed
    cluster_log "Verifying device WWNs across all nodes"
    local node1_wwns=()

    # Build script to get all WWNs/WWIDs at once
    # lsblk provides WWN for SCSI/NVMe; multipath -ll provides WWID for dm-multipath
    # (lsblk cannot report WWN for device-mapper devices)
    local wwn_script="{ lsblk -d -n -o NAME,WWN 2>/dev/null; multipath -ll 2>/dev/null | sed -n 's/^\([a-z][a-z0-9]*\) (\([^)]*\)).*/\1 \2/p'; } | grep -E '"
    for i in "${!devices[@]}"; do
        local dev_base=$(basename "${devices[$i]}")
        if [ $i -gt 0 ]; then
            wwn_script+="|"
        fi
        wwn_script+="^${dev_base} "
    done
    wwn_script+="' || true"

    # Get all WWNs in one SSH call
    local wwn_output=$(cluster_node_exec 1 "$wwn_script")

    # Parse WWNs in order
    for dev in "${devices[@]}"; do
        local dev_base=$(basename "$dev")
        local wwn=$(echo "$wwn_output" | grep "^${dev_base} " | awk '{print $2}' || echo "NO_WWN")
        wwn=$(echo "$wwn" | tr -d '[:space:]')  # Remove whitespace
        [ -z "$wwn" ] && wwn="NO_WWN"
        node1_wwns+=("$wwn")
        cluster_debug "  Node 1: $dev → WWN: $wwn"
    done

    # Build WWN-to-device mapping for all nodes
    # Instead of assuming devices match by position, we map by WWN
    declare -A node_device_maps  # Key: "nodeN_devI" → Value: device_path

    # Store node 1 mapping
    for i in "${!devices[@]}"; do
        node_device_maps["1_${i}"]="${devices[$i]}"
    done

    # Map devices on other nodes by WWN
    local num_nodes="${CLUSTER_NUM_NODES}"
    for node_num in $(seq 2 "$num_nodes"); do
        local node_devices=()
        while IFS= read -r dev; do
            if [ -n "$dev" ]; then
                node_devices+=("$dev")
            fi
        done < <(cluster_discover_devices_on_node "$node_num" 1)  # Use fast discovery (skip rescan)

        if [ ${#node_devices[@]} -ne ${#devices[@]} ]; then
            cluster_warn "Node $node_num has ${#node_devices[@]} devices, node 1 has ${#devices[@]}"
            cluster_warn "This may cause issues with device variable consistency"
            continue
        fi

        # Get WWNs for all devices on this node - batch all lookups in one SSH call
        cluster_debug "Mapping devices on node $node_num by WWN"

        # Build script to get all WWNs/WWIDs at once
        local node_wwn_script="{ lsblk -d -n -o NAME,WWN 2>/dev/null; multipath -ll 2>/dev/null | sed -n 's/^\([a-z][a-z0-9]*\) (\([^)]*\)).*/\1 \2/p'; } | grep -E '"
        for i in "${!node_devices[@]}"; do
            local dev_base=$(basename "${node_devices[$i]}")
            if [ $i -gt 0 ]; then
                node_wwn_script+="|"
            fi
            node_wwn_script+="^${dev_base} "
        done
        node_wwn_script+="' || true"

        # Get all WWNs in one SSH call
        local node_wwn_output=$(cluster_node_exec "$node_num" "$node_wwn_script")

        # Parse WWNs for this node
        local node_wwns=()
        for dev in "${node_devices[@]}"; do
            local dev_base=$(basename "$dev")
            local wwn=$(echo "$node_wwn_output" | grep "^${dev_base} " | awk '{print $2}' || echo "NO_WWN")
            wwn=$(echo "$wwn" | tr -d '[:space:]')  # Remove whitespace
            [ -z "$wwn" ] && wwn="NO_WWN"
            node_wwns+=("$wwn")
        done

        # Map by WWN: for each node1 WWN, find matching device on current node
        for i in "${!node1_wwns[@]}"; do
            local target_wwn="${node1_wwns[$i]}"
            local found=0

            for j in "${!node_wwns[@]}"; do
                if [ "${node_wwns[$j]}" = "$target_wwn" ]; then
                    node_device_maps["${node_num}_${i}"]="${node_devices[$j]}"
                    cluster_debug "  dev$((i+1)) (WWN: $target_wwn) → Node $node_num: ${node_devices[$j]}"
                    found=1
                    break
                fi
            done

            if [ $found -eq 0 ]; then
                cluster_error "WWN $target_wwn (dev$((i+1)) on node 1: ${devices[$i]}) not found on node $node_num"
                cluster_error "This means node $node_num is missing a physical device that exists on node 1"
                return 1
            fi
        done
    done

    cluster_log "✓ Device WWN mapping completed - all nodes see the same physical devices"

    # Log device mapping for debugging
    if [ "${CLUSTER_DEBUG:-0}" -eq 1 ]; then
        cluster_debug "Device mapping by WWN:"
        for i in "${!node1_wwns[@]}"; do
            cluster_debug "  dev$((i+1)) (WWN: ${node1_wwns[$i]}):"
            for node_num in $(seq 1 "$num_nodes"); do
                cluster_debug "    Node $node_num: ${node_device_maps["${node_num}_${i}"]}"
            done
        done
    fi

    # Deploy per-node device mapping files
    cluster_log "Deploying device mappings to all nodes"
    for node_num in $(seq 1 "$num_nodes"); do
        # Build device variable script for this node
        local dev_map_script="#!/bin/bash
# Auto-generated device mapping for node $node_num
# Maps device variables to correct physical devices by WWN
"

        # Counters for type-specific device aliases
        local scsi_count=0
        local nvme_count=0
        local mpath_count=0

        # First pass: Create $dev1, $dev2, etc.
        for i in "${!devices[@]}"; do
            local dev_num=$((i + 1))
            local dev_path="${node_device_maps["${node_num}_${i}"]}"
            dev_map_script+="export dev${dev_num}='${dev_path}'
"
        done

        # Second pass: Create type-specific aliases ($scsi1, $nvme1, $mpath1, etc.)
        for i in "${!devices[@]}"; do
            local dev_path="${node_device_maps["${node_num}_${i}"]}"

            # Detect device type and create alias
            if [[ "$dev_path" =~ ^/dev/sd[a-z]+$ ]]; then
                # SCSI device
                scsi_count=$((scsi_count + 1))
                dev_map_script+="export scsi${scsi_count}='${dev_path}'
"
            elif [[ "$dev_path" =~ ^/dev/nvme[0-9]+n[0-9]+$ ]]; then
                # NVMe device
                nvme_count=$((nvme_count + 1))
                dev_map_script+="export nvme${nvme_count}='${dev_path}'
"
            elif [[ "$dev_path" =~ ^/dev/mapper/mpath ]]; then
                # Multipath device
                mpath_count=$((mpath_count + 1))
                dev_map_script+="export mpath${mpath_count}='${dev_path}'
"
            fi
        done

        # Add DEVICES array
        dev_map_script+="export DEVICES=("
        for i in "${!devices[@]}"; do
            dev_map_script+="'${node_device_maps["${node_num}_${i}"]}' "
        done
        dev_map_script+=")
"

        # Deploy to node
        if ! echo "$dev_map_script" | cluster_node_exec "$node_num" "cat > /root/cluster-dev-map.sh && chmod +x /root/cluster-dev-map.sh" > /dev/null; then
            cluster_error "Failed to reach node $node_num during setup (device map deployment)"
            return 1
        fi
        cluster_debug "Deployed device map to node $node_num (scsi:$scsi_count nvme:$nvme_count mpath:$mpath_count)"
    done

    # Persist device mapping as exported variables for command translation.
    # Uses flat CLUSTER_DEV_MAP_<node>_<idx> variables instead of an associative
    # array because bash cannot export associative arrays to child processes.
    export CLUSTER_DEV_COUNT=${#devices[@]}
    for node_num in $(seq 1 "$num_nodes"); do
        for i in "${!devices[@]}"; do
            export "CLUSTER_DEV_MAP_${node_num}_${i}=${node_device_maps["${node_num}_${i}"]}"
        done
    done

    # Export per-device WWNs so refresh_devices can re-map by identity
    for i in "${!node1_wwns[@]}"; do
        export "CLUSTER_DEV_WWN_${i}=${node1_wwns[$i]}"
    done

    # Also export device variables in current shell (for node 1 paths, used in non-remote contexts).
    # First unset any stale variables from a previous test group that had more devices.
    if [ -n "${CLUSTER_PREV_DEV_COUNT:-}" ]; then
        for i in $(seq 1 "$CLUSTER_PREV_DEV_COUNT"); do
            unset "dev${i}" "scsi${i}" "nvme${i}" "mpath${i}" 2>/dev/null || true
        done
    fi
    export CLUSTER_PREV_DEV_COUNT=${#devices[@]}

    local scsi_count=0
    local nvme_count=0
    local mpath_count=0

    for i in "${!devices[@]}"; do
        local dev_num=$((i + 1))
        local dev_path="${devices[$i]}"
        export "dev${dev_num}=${dev_path}"

        # Export type-specific aliases
        if [[ "$dev_path" =~ ^/dev/sd[a-z]+$ ]]; then
            scsi_count=$((scsi_count + 1))
            export "scsi${scsi_count}=${dev_path}"
        elif [[ "$dev_path" =~ ^/dev/nvme[0-9]+n[0-9]+$ ]]; then
            nvme_count=$((nvme_count + 1))
            export "nvme${nvme_count}=${dev_path}"
        elif [[ "$dev_path" =~ ^/dev/mapper/mpath ]]; then
            mpath_count=$((mpath_count + 1))
            export "mpath${mpath_count}=${dev_path}"
        fi
    done
    export DEVICES=("${devices[@]}")

    cluster_log "Device variables set up successfully"
    cluster_log "  Devices: ${devices[*]}"
    [ $scsi_count -gt 0 ] && cluster_log "  SCSI devices: $scsi_count"
    [ $nvme_count -gt 0 ] && cluster_log "  NVMe devices: $nvme_count"
    [ $mpath_count -gt 0 ] && cluster_log "  Multipath devices: $mpath_count"

    # Verify NVMe hostnqn uniqueness across nodes (duplicate hostnqn/hostid
    # breaks persistent reservations because nvmet identifies registrants
    # by hostid).
    if [ $nvme_count -gt 0 ]; then
        cluster_log "Verifying NVMe host identity uniqueness across nodes"
        local -a hostnqns=()
        for node_num in $(seq 1 "$num_nodes"); do
            local nqn
            nqn=$(cluster_node_exec "$node_num" "cat /etc/nvme/hostnqn 2>/dev/null" 2>/dev/null | tr -d '[:space:]')
            hostnqns+=("$nqn")
            cluster_debug "  Node $node_num hostnqn: $nqn"
        done

        local dup_found=0
        for ((i=0; i<${#hostnqns[@]}; i++)); do
            for ((j=i+1; j<${#hostnqns[@]}; j++)); do
                if [ "${hostnqns[$i]}" = "${hostnqns[$j]}" ]; then
                    cluster_error "Nodes $((i+1)) and $((j+1)) share NVMe hostnqn: ${hostnqns[$i]}"
                    dup_found=1
                fi
            done
        done

        if [ $dup_found -eq 1 ]; then
            cluster_error "Duplicate NVMe hostnqn detected — PR operations will fail"
            cluster_error "Fix: remove /etc/nvme/hostnqn and /etc/nvme/hostid from the golden image"
            return 1
        fi
        cluster_log "  All $num_nodes nodes have unique NVMe hostnqn"
    fi

    # Set up debug log directory on each node
    cluster_log "Setting up debug log directory on nodes"
    for node_num in $(seq 1 "$num_nodes"); do
        if ! cluster_node_exec "$node_num" "mkdir -p /var/log/lvm-test" > /dev/null 2>&1; then
            cluster_error "Failed to reach node $node_num during setup (debug log directory)"
            return 1
        fi
        cluster_debug "Created debug log directory on node $node_num: /var/log/lvm-test"
    done

    # Configure LVM debug logging in /etc/lvm/lvm.conf on each node
    cluster_log "Configuring LVM debug logging on nodes"

    # Get test name from environment (set by cluster_run_test)
    local test_name="${CLUSTER_TEST_NAME:-unknown}"

    for node_num in $(seq 1 "$num_nodes"); do
        # Backup existing lvm.conf if not already backed up
        if ! cluster_node_exec "$node_num" \
            "[ -f /etc/lvm/lvm.conf.cluster-test-backup ] || cp /etc/lvm/lvm.conf /etc/lvm/lvm.conf.cluster-test-backup"; then
            cluster_error "Failed to reach node $node_num during setup (lvm.conf backup)"
            return 1
        fi

        # Update lvm.conf with test-specific debug log file
        # Use lvmconfig to merge settings (safest approach)
        if ! cluster_node_exec "$node_num" \
            "lvmconfig --mergedconfig --config 'log { file=\"/var/log/lvm-test/${test_name}-node${node_num}.log\" level=9 indent=1 activation=1 overwrite=0 }' > /etc/lvm/lvm.conf.tmp && mv /etc/lvm/lvm.conf.tmp /etc/lvm/lvm.conf"; then
            cluster_error "Failed to reach node $node_num during setup (lvm.conf logging)"
            return 1
        fi
        cluster_debug "Configured LVM debug logging on node $node_num: ${test_name}-node${node_num}.log"
    done

    # Deploy test helper functions to all nodes
    cluster_log "Deploying test helper functions to all nodes"
    local helper_functions='#!/bin/bash
# Auto-generated test helper functions for cluster tests

# Node number (injected during deployment)
export NODE_NUM=${NODE_NUM:-unknown}

#
# LVM Debug Logging Configuration
#

# Note: LVM debug logging is configured via /etc/lvm/lvm.conf
# We do NOT set LVM_LOG_FILE_EPOCH to avoid file name suffixes
# and to ensure all commands append to the same log file.

# Set expected exit status for LVM commands
export LVM_EXPECTED_EXIT_STATUS=1

#
# Test Helper Functions
#

# not - Execute command expecting failure
# Usage: not <command> [args...]
# Returns 0 if command fails, 1 if command succeeds
not() {
    # Set expected exit status for LVM commands
    export LVM_EXPECTED_EXIT_STATUS=">1"

    # Execute the command
    "$@"
    local exit_code=$?

    # Restore expected exit status
    export LVM_EXPECTED_EXIT_STATUS=1

    # Invert exit code: success if command failed
    if [ $exit_code -eq 0 ]; then
        echo "ERROR: Command succeeded but was expected to fail: $*" >&2
        return 1
    else
        return 0
    fi
}

# Export functions so they'\''re available in subshells
export -f not
'

    # Deploy to all nodes
    for node_num in $(seq 1 "$num_nodes"); do
        # Inject NODE_NUM into helper script for this specific node
        local node_helper_script="${helper_functions//\$\{NODE_NUM:-unknown\}/$node_num}"

        if ! echo "$node_helper_script" | cluster_node_exec "$node_num" "cat > /root/cluster-node-helpers.sh && chmod +x /root/cluster-node-helpers.sh" > /dev/null; then
            cluster_error "Failed to reach node $node_num during setup (helper deployment)"
            return 1
        fi
        cluster_debug "Deployed helper functions with debug logging to node $node_num"
    done

    # Deploy cluster test helpers if they exist
    local script_dir="$(dirname "${BASH_SOURCE[0]}")"
    local test_helpers="${script_dir}/shell/lib/cluster-test-helpers.sh"
    if [ -f "$test_helpers" ]; then
        cluster_log "Deploying cluster test helpers to all nodes"
        for node_num in $(seq 1 "$num_nodes"); do
            if ! cat "$test_helpers" | cluster_node_exec "$node_num" \
                "cat > /root/cluster-test-helpers.sh && chmod +x /root/cluster-test-helpers.sh" > /dev/null; then
                cluster_error "Failed to reach node $node_num during setup (test helper deployment)"
                return 1
            fi
            cluster_debug "Deployed cluster test helpers to node $node_num"
        done
    fi

    return 0
}

#
# cluster_deploy_test_files - Deploy test files to test nodes
#
# Copies test script and dependencies to all test nodes for execution.
#
# Arguments:
#   $1 - test_script (path to test script to deploy)
#
# Returns:
#   0 on success, 1 on failure
#
cluster_deploy_test_files() {
    local test_script="$1"

    if [ -z "$test_script" ]; then
        cluster_error "cluster_deploy_test_files: test_script is required"
        return 1
    fi

    if [ ! -f "$test_script" ]; then
        cluster_error "Test script not found: $test_script"
        return 1
    fi

    if [ -z "${CLUSTER_NUM_NODES:-}" ]; then
        cluster_error "CLUSTER_NUM_NODES not set"
        return 1
    fi

    cluster_log "Deploying test files to cluster nodes"

    local num_nodes="${CLUSTER_NUM_NODES}"
    local test_dir="/root/cluster-tests"
    local script_name=$(basename "$test_script")

    # Deploy to each test node
    for node_num in $(seq 1 "$num_nodes"); do
        cluster_debug "Deploying test files to node $node_num"

        # Get node IP
        local vm_name=$(cluster_vm_get_name "$CLUSTER_ID" "$node_num")
        local node_ip
        if ! node_ip=$(cluster_vm_get_ip "$vm_name" 2>/dev/null); then
            cluster_error "Failed to get IP for node $node_num"
            return 1
        fi

        # Create test directory on node
        cluster_vm_ssh "$node_ip" "mkdir -p $test_dir" || {
            cluster_error "Failed to create test directory on node $node_num"
            return 1
        }

        # Copy test script
        cluster_vm_scp "$test_script" "${CLUSTER_SSH_USER}@${node_ip}:${test_dir}/${script_name}" || {
            cluster_error "Failed to copy test script to node $node_num"
            return 1
        }

        # Make script executable
        cluster_vm_ssh "$node_ip" "chmod +x ${test_dir}/${script_name}" || {
            cluster_warn "Failed to make test script executable on node $node_num"
        }

        # Copy test/lib directory if it exists (test dependencies)
        local lib_dir="test/lib"
        if [ -d "$lib_dir" ]; then
            cluster_debug "Copying test/lib directory to node $node_num"

            # Use rsync via SCP for directory copy
            rsync -az -e "ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -i ${CLUSTER_SSH_KEY_DIR}/cluster_test_rsa" \
                "$lib_dir/" "${CLUSTER_SSH_USER}@${node_ip}:${test_dir}/lib/" 2>/dev/null || {
                cluster_warn "Failed to copy test/lib to node $node_num (may not be needed)"
            }
        fi
    done

    cluster_log "Test files deployed successfully"
    return 0
}

#
# cluster_collect_debug_logs - Collect debug logs from all test nodes
#
# Copies LVM debug logs from all test nodes to local results directory.
# Called automatically on test failure.
#
# Arguments:
#   $1 - test_name (name of the test, without .sh extension)
#
# Returns:
#   0 on success, 1 on failure
#
cluster_restore_lvm_config() {
    if [ -z "${CLUSTER_NUM_NODES:-}" ]; then
        cluster_error "CLUSTER_NUM_NODES not set"
        return 1
    fi

    cluster_log "Restoring original LVM configuration on nodes"
    for node_num in $(seq 1 "$CLUSTER_NUM_NODES"); do
        cluster_node_exec "$node_num" \
            "[ -f /etc/lvm/lvm.conf.cluster-test-backup ] && cp /etc/lvm/lvm.conf.cluster-test-backup /etc/lvm/lvm.conf || true" > /dev/null 2>&1
        cluster_debug "Restored LVM configuration on node $node_num"
    done
}

#
# cluster_stress_run - Run registered stress loops on all nodes concurrently
#
# For each node, generates a self-contained script that runs each registered
# loop body inside a timed while-loop as a background subshell. All loops run
# concurrently on all nodes for the specified duration, then terminate.
#
# Uses STRESS_LOOP_NAMES, STRESS_LOOP_BODIES, STRESS_LOOP_NODES arrays
# populated by stress_loop/stress_loop_on.
#
# Arguments:
#   $1 - duration in minutes
#
# Returns:
#   0 if all nodes completed normally, 1 if any node failed
#
cluster_stress_run() {
    local duration_secs="$1"

    if [ -z "$duration_secs" ]; then
        cluster_error "cluster_stress_run: duration in seconds is required"
        return 1
    fi

    local num_nodes="${CLUSTER_NUM_NODES}"
    local num_loops=${#STRESS_LOOP_NAMES[@]}

    cluster_log "=========================================="
    cluster_log "Stress test: ${num_loops} loops on ${num_nodes} nodes for ${duration_secs} seconds"
    cluster_log "=========================================="

    for i in $(seq 0 $((num_loops - 1))); do
        cluster_log "  loop: ${STRESS_LOOP_NAMES[$i]} (nodes: ${STRESS_LOOP_NODES[$i]})"
    done

    # Pre-resolve all node IPs
    local node_ips=()
    for node_num in $(seq 1 "$num_nodes"); do
        local vm_name=$(cluster_vm_get_name "$CLUSTER_ID" "$node_num")
        local node_ip
        if ! node_ip=$(cluster_vm_get_ip "$vm_name" 2>/dev/null); then
            cluster_error "Failed to get IP for node $node_num ($vm_name)"
            return 1
        fi
        node_ips[$node_num]="$node_ip"
    done

    local ssh_key="${CLUSTER_SSH_KEY_DIR}/cluster_test_rsa"
    if [ ! -f "$ssh_key" ]; then
        cluster_error "SSH key not found: $ssh_key"
        return 1
    fi

    local tmpdir=$(mktemp -d)
    trap "rm -rf '$tmpdir'" RETURN

    # Generate and deploy the stress script for each node
    for node_num in $(seq 1 "$num_nodes"); do
        local script_file="$tmpdir/stress_node${node_num}.sh"

        cat > "$script_file" <<'STRESS_HEADER'
#!/bin/bash
source /root/cluster-dev-map.sh 2>/dev/null
STRESS_HEADER

        cat >> "$script_file" <<STRESS_VARS
DURATION_SECS=$duration_secs
END_TIME=\$((  \$(date +%s) + DURATION_SECS ))
NODE_NUM=$node_num
LOGDIR="/tmp/cluster-stress-logs"
STRESS_VARS

        cat >> "$script_file" <<'STRESS_INIT'
mkdir -p "$LOGDIR"
rm -f "$LOGDIR"/*.log "$LOGDIR"/*.iter
PIDS=()
LOOP_NAMES=()
STRESS_INIT

        # Add each loop that applies to this node
        for i in $(seq 0 $((num_loops - 1))); do
            local loop_name="${STRESS_LOOP_NAMES[$i]}"
            local loop_body="${STRESS_LOOP_BODIES[$i]}"
            local loop_nodes="${STRESS_LOOP_NODES[$i]}"

            # Check if this loop runs on this node
            if [ "$loop_nodes" != "all" ]; then
                local runs_here=0
                for n in $loop_nodes; do
                    if [ "$n" = "$node_num" ]; then
                        runs_here=1
                        break
                    fi
                done
                if [ "$runs_here" -eq 0 ]; then
                    continue
                fi
            fi

            cat >> "$script_file" <<STRESS_LOOP
# Loop: $loop_name
( set +e
  iter=0
  while [ \$(date +%s) -lt \$END_TIME ]; do
      iter=\$((iter + 1))
      echo \$iter > "\$LOGDIR/${loop_name}.iter"
$loop_body
  done
  echo "loop_iterations=\$iter"
) > "\$LOGDIR/${loop_name}.log" 2>&1 &
PIDS+=(\$!)
LOOP_NAMES+=("$loop_name")

STRESS_LOOP
        done

        cat >> "$script_file" <<'STRESS_FOOTER'
if [ ${#PIDS[@]} -eq 0 ]; then
    echo "STRESS_COMPLETE loops=0"
    exit 0
fi

# Wait for loops to finish naturally via END_TIME, plus buffer for
# the final iteration to complete and write its iteration count.
# Kill only as a backstop for stuck loops.
sleep $((DURATION_SECS + 30))

for pid in "${PIDS[@]}"; do
    kill $pid 2>/dev/null
done
wait 2>/dev/null

# Report results
failed=0
for idx in "${!LOOP_NAMES[@]}"; do
    name="${LOOP_NAMES[$idx]}"
    logfile="$LOGDIR/${name}.log"
    iters=$(grep -o 'loop_iterations=[0-9]*' "$logfile" 2>/dev/null | cut -d= -f2)
    if [ -z "$iters" ]; then
        iters=$(cat "$LOGDIR/${name}.iter" 2>/dev/null)
    fi
    if [ -z "$iters" ]; then
        echo "  ${name}: CRASHED"
        failed=1
    else
        echo "  ${name}: completed (${iters} iterations)"
    fi
done

echo "STRESS_COMPLETE loops=${#LOOP_NAMES[@]}"
exit $failed
STRESS_FOOTER

        # Deploy the script to the node
        local node_ip="${node_ips[$node_num]}"
        scp -o StrictHostKeyChecking=no \
            -o UserKnownHostsFile=/dev/null \
            -o LogLevel=ERROR \
            -i "$ssh_key" \
            "$script_file" \
            "${CLUSTER_SSH_USER}@${node_ip}:/tmp/cluster-stress.sh" > /dev/null 2>&1

        if [ $? -ne 0 ]; then
            cluster_error "Failed to deploy stress script to node $node_num"
            return 1
        fi
        cluster_debug "Deployed stress script to node $node_num"
    done

    # Launch the stress script on all nodes in parallel
    local pids=()

    for node_num in $(seq 1 "$num_nodes"); do
        local output_file="$tmpdir/node${node_num}.out"
        local status_file="$tmpdir/node${node_num}.status"
        local node_ip="${node_ips[$node_num]}"

        bash -c "
            ssh -o StrictHostKeyChecking=no \
                -o UserKnownHostsFile=/dev/null \
                -o LogLevel=ERROR \
                -o ServerAliveInterval=30 \
                -o ServerAliveCountMax=10 \
                -i '$ssh_key' \
                '$CLUSTER_SSH_USER@$node_ip' \
                'bash /tmp/cluster-stress.sh' >'$output_file' 2>&1
            echo \$? > '$status_file'
        " &

        pids+=($!)
        cluster_debug "Launched stress on node $node_num (PID: ${pids[-1]})"
    done

    cluster_log "Stress running on all nodes for ${duration_secs} seconds..."

    # Wait for all nodes
    for pid in "${pids[@]}"; do
        wait "$pid" 2>/dev/null || true
    done

    sleep 0.1

    # Collect and display results
    local failed=0

    for node_num in $(seq 1 "$num_nodes"); do
        local output_file="$tmpdir/node${node_num}.out"
        local status_file="$tmpdir/node${node_num}.status"

        if [ ! -f "$status_file" ]; then
            echo "  node${node_num}: ERROR (no status)" >&2
            failed=1
            continue
        fi

        local status=$(cat "$status_file" 2>/dev/null || echo "-1")

        if [ "$status" -eq 0 ]; then
            echo "  node${node_num}: success"
        else
            echo "  node${node_num}: failed"
            failed=1
        fi

        if [ -s "$output_file" ]; then
            cat "$output_file"
        fi
    done

    # Collect stress logs from all nodes
    cluster_stress_collect_logs

    if [ $failed -eq 0 ]; then
        cluster_log "Stress test completed successfully"
        # Remove stress logs on success - only failures need debugging
        local results_dir="${CLUSTER_RESULTS_DIR:-$(pwd)/results}"
        local test_name="${CLUSTER_TEST_NAME:-stress}"
        local timestamp="${CLUSTER_TEST_TIMESTAMP:-$(date +%m%d%H%M%S)}"
        local stress_dir="$results_dir/${test_name}_${timestamp}_stress"
        rm -rf "$stress_dir"
    else
        cluster_log "Stress test FAILED on one or more nodes"
    fi

    return $failed
}

#
# cluster_stress_collect_logs - Collect stress loop logs from all nodes
#
# Copies per-loop log files from /tmp/cluster-stress-logs/ on each node
# to the local results directory.
#
cluster_stress_collect_logs() {
    local results_dir="${CLUSTER_RESULTS_DIR:-$(pwd)/results}"
    local test_name="${CLUSTER_TEST_NAME:-stress}"
    local timestamp="${CLUSTER_TEST_TIMESTAMP:-$(date +%m%d%H%M%S)}"
    local stress_dir="$results_dir/${test_name}_${timestamp}_stress"

    mkdir -p "$stress_dir" 2>/dev/null || true

    for node_num in $(seq 1 "$CLUSTER_NUM_NODES"); do
        local vm_name=$(cluster_vm_get_name "$CLUSTER_ID" "$node_num")
        local node_ip
        node_ip=$(cluster_vm_get_ip "$vm_name" 2>/dev/null) || continue

        local node_dir="$stress_dir/node${node_num}"
        mkdir -p "$node_dir" 2>/dev/null || true

        local ssh_key="${CLUSTER_SSH_KEY_DIR}/cluster_test_rsa"

        # List log files on the node and copy each one
        local log_files
        log_files=$(ssh -o StrictHostKeyChecking=no \
            -o UserKnownHostsFile=/dev/null \
            -o LogLevel=ERROR \
            -i "$ssh_key" \
            "${CLUSTER_SSH_USER}@${node_ip}" \
            "ls /tmp/cluster-stress-logs/*.log 2>/dev/null" 2>/dev/null) || continue

        for remote_log in $log_files; do
            local log_name=$(basename "$remote_log")
            scp -o StrictHostKeyChecking=no \
                -o UserKnownHostsFile=/dev/null \
                -o LogLevel=ERROR \
                -i "$ssh_key" \
                "${CLUSTER_SSH_USER}@${node_ip}:${remote_log}" \
                "${node_dir}/${log_name}" > /dev/null 2>&1 || true
        done

        cluster_debug "Collected stress logs from node $node_num"
    done

    chmod -R a+rw "$stress_dir" 2>/dev/null || true

    cluster_log "Stress logs collected to: $stress_dir"
}

#
# cluster_collect_debug_logs - Collect debug logs from all test nodes
#
# Copies LVM debug logs from all test nodes to local results directory.
# Called automatically on test failure.
#
# Arguments:
#   $1 - test_name (name of the test, without .sh extension)
#
# Returns:
#   0 on success, 1 on failure
#
cluster_collect_debug_logs() {
    local test_name="$1"
    local prefix="${2:-}"
    local timestamp="${CLUSTER_TEST_TIMESTAMP:-unknown}"

    if [ -z "$test_name" ]; then
        cluster_error "cluster_collect_debug_logs: test_name is required"
        return 1
    fi

    if [ -z "${CLUSTER_NUM_NODES:-}" ]; then
        cluster_error "CLUSTER_NUM_NODES not set"
        return 1
    fi

    # Determine script directory for results path with timestamp
    local script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    local failure_logs_dir="$script_dir/results/${prefix}${test_name}_${timestamp}_debug"

    cluster_log "Collecting debug logs from all nodes"

    # Create failure logs directory
    mkdir -p "$failure_logs_dir" || {
        cluster_error "Failed to create failure logs directory: $failure_logs_dir"
        return 1
    }
    chmod 777 "$failure_logs_dir" 2>/dev/null || true

    # Copy config file to debug directory
    if [ -n "${CLUSTER_CONFIG_FILE:-}" ] && [ -f "$CLUSTER_CONFIG_FILE" ]; then
        cp "$CLUSTER_CONFIG_FILE" "$failure_logs_dir/$(basename "$CLUSTER_CONFIG_FILE")" 2>/dev/null || true
        cluster_debug "Copied config file to debug directory"
    fi

    # Copy group file to debug directory (if running from group)
    if [ -n "${CLUSTER_GROUP_FILE:-}" ] && [ -f "$CLUSTER_GROUP_FILE" ]; then
        cp "$CLUSTER_GROUP_FILE" "$failure_logs_dir/$(basename "$CLUSTER_GROUP_FILE")" 2>/dev/null || true
        cluster_debug "Copied group file to debug directory"
    fi

    local num_nodes="${CLUSTER_NUM_NODES}"
    local collected=0

    for node_num in $(seq 1 "$num_nodes"); do
        cluster_debug "Collecting debug logs from node $node_num"

        # Get node IP
        local vm_name=$(cluster_vm_get_name "$CLUSTER_ID" "$node_num")
        local node_ip
        if ! node_ip=$(cluster_vm_get_ip "$vm_name" 2>/dev/null); then
            cluster_warn "Failed to get IP for node $node_num, skipping log collection"
            continue
        fi

        # Check if debug log exists for this test on node
        # LVM may append suffix like _DEBUG_PID_number, so use pattern match
        local log_pattern="${test_name}-node${node_num}.log*"
        local log_files
        if ! log_files=$(cluster_node_exec "$node_num" "ls /var/log/lvm-test/${log_pattern} 2>/dev/null" 2>/dev/null); then
            cluster_debug "No debug log found on node $node_num: /var/log/lvm-test/${log_pattern}"
            continue
        fi

        # Copy all matching log files to failure logs directory
        # If LVM created multiple files (with suffixes), copy them all
        while IFS= read -r remote_log; do
            if [ -z "$remote_log" ]; then
                continue
            fi

            local base_name=$(basename "$remote_log")
            cluster_vm_scp "${CLUSTER_SSH_USER}@${node_ip}:${remote_log}" \
                "$failure_logs_dir/${base_name}" 2>/dev/null || {
                cluster_warn "Failed to copy debug log from node $node_num: $remote_log"
                continue
            }
            chmod 666 "$failure_logs_dir/${base_name}" 2>/dev/null || true
            cluster_debug "Copied debug log: $base_name"
        done <<< "$log_files"

        # Collect dmesg output from node
        local dmesg_file="${test_name}-node${node_num}-dmesg.log"
        if cluster_node_exec "$node_num" "dmesg > /tmp/${dmesg_file} 2>&1" > /dev/null 2>&1; then
            cluster_vm_scp "${CLUSTER_SSH_USER}@${node_ip}:/tmp/${dmesg_file}" \
                "$failure_logs_dir/${dmesg_file}" 2>/dev/null && {
                chmod 666 "$failure_logs_dir/${dmesg_file}" 2>/dev/null || true
                cluster_debug "Collected dmesg from node $node_num"
            } || {
                cluster_debug "Failed to copy dmesg from node $node_num"
            }
            # Clean up remote dmesg file
            cluster_node_exec "$node_num" "rm -f /tmp/${dmesg_file}" > /dev/null 2>&1 || true

            # Check for iSCSI connection failures in dmesg
            if [ -f "$failure_logs_dir/${dmesg_file}" ]; then
                if grep -q "ISCSI_ERR_NOP_TIMEDOUT\|detected conn error\|session recovery timed out" \
                        "$failure_logs_dir/${dmesg_file}" 2>/dev/null; then
                    cluster_warn "Node $node_num: iSCSI connection failure detected."
                    cluster_warn "  NOP timeout often means the target stopped responding while a SCSI"
                    cluster_warn "  command (e.g. sg_persist preempt-abort) was in flight, not initiator"
                    cluster_warn "  CPU scheduling. Check node0 (LIO target) logs in the debug directory."
                    cluster_warn "  Known related LIO PREEMPT-AND-ABORT deadlock:"
                    cluster_warn "  https://lore.kernel.org/target-devel/ajrNEj-B63SDtZ9W@redhat.com/T/#u"
                fi
            fi
        else
            cluster_debug "Failed to collect dmesg from node $node_num"
        fi

        # Collect journalctl output from node since test start
        local journal_file="${test_name}-node${node_num}-journal.log"
        if cluster_node_exec "$node_num" "[ -f /tmp/cluster_test_start_time ]" > /dev/null 2>&1; then
            # Get the saved start time
            local start_time
            if start_time=$(cluster_node_exec "$node_num" "cat /tmp/cluster_test_start_time" 2>/dev/null); then
                # Collect journalctl since that time
                if cluster_node_exec "$node_num" "journalctl --since '${start_time}' > /tmp/${journal_file} 2>&1" > /dev/null 2>&1; then
                    cluster_vm_scp "${CLUSTER_SSH_USER}@${node_ip}:/tmp/${journal_file}" \
                        "$failure_logs_dir/${journal_file}" 2>/dev/null && {
                        chmod 666 "$failure_logs_dir/${journal_file}" 2>/dev/null || true
                        cluster_debug "Collected journalctl from node $node_num"
                    } || {
                        cluster_debug "Failed to copy journalctl from node $node_num"
                    }
                    # Clean up remote journal file
                    cluster_node_exec "$node_num" "rm -f /tmp/${journal_file}" > /dev/null 2>&1 || true
                else
                    cluster_debug "Failed to collect journalctl from node $node_num"
                fi
            else
                cluster_debug "Failed to read start time for journalctl on node $node_num"
            fi
        else
            cluster_debug "No start time saved for journalctl on node $node_num"
        fi

        # Collect sanlock log_dump output from node (if sanlock is running)
        local sanlock_file="${test_name}-node${node_num}-sanlock.log"
        if cluster_node_exec "$node_num" "command -v sanlock >/dev/null 2>&1" > /dev/null 2>&1; then
            # Note: sanlock log_dump doesn't work with > redirect, use tee instead
            if cluster_node_exec "$node_num" "sanlock log_dump 2>&1 | tee /tmp/${sanlock_file} >/dev/null" > /dev/null 2>&1; then
                cluster_vm_scp "${CLUSTER_SSH_USER}@${node_ip}:/tmp/${sanlock_file}" \
                    "$failure_logs_dir/${sanlock_file}" 2>/dev/null && {
                    chmod 666 "$failure_logs_dir/${sanlock_file}" 2>/dev/null || true
                    cluster_debug "Collected sanlock log_dump from node $node_num"
                } || {
                    cluster_debug "Failed to copy sanlock log_dump from node $node_num"
                }
                # Clean up remote sanlock file
                cluster_node_exec "$node_num" "rm -f /tmp/${sanlock_file}" > /dev/null 2>&1 || true
            else
                cluster_debug "Failed to collect sanlock log_dump from node $node_num"
            fi
        else
            cluster_debug "sanlock not available on node $node_num"
        fi

        # Collect lvmlockctl output from node (if lvmlockd is running)
        local lvmlockd_file="${test_name}-node${node_num}-lvmlockd.log"
        if cluster_node_exec "$node_num" "command -v lvmlockctl >/dev/null 2>&1" > /dev/null 2>&1; then
            if cluster_node_exec "$node_num" "lvmlockctl -d > /tmp/${lvmlockd_file} 2>&1" > /dev/null 2>&1; then
                cluster_vm_scp "${CLUSTER_SSH_USER}@${node_ip}:/tmp/${lvmlockd_file}" \
                    "$failure_logs_dir/${lvmlockd_file}" 2>/dev/null && {
                    chmod 666 "$failure_logs_dir/${lvmlockd_file}" 2>/dev/null || true
                    cluster_debug "Collected lvmlockctl -d from node $node_num"
                } || {
                    cluster_debug "Failed to copy lvmlockctl output from node $node_num"
                }
                # Clean up remote lvmlockd file
                cluster_node_exec "$node_num" "rm -f /tmp/${lvmlockd_file}" > /dev/null 2>&1 || true
            else
                cluster_debug "Failed to collect lvmlockctl -d from node $node_num"
            fi
        else
            cluster_debug "lvmlockctl not available on node $node_num"
        fi

        collected=$((collected + 1))
        cluster_debug "Collected debug logs from node $node_num"
    done

    # Collect debug logs from node0 (storage exporter / iSCSI target)
    local vm_name_node0=$(cluster_vm_get_name "$CLUSTER_ID" 0)
    local node0_ip
    if node0_ip=$(cluster_vm_get_ip "$vm_name_node0" 2>/dev/null); then
        cluster_debug "Collecting debug logs from node 0 (storage exporter)"

        local dmesg_file_node0="${test_name}-node0-dmesg.log"
        if cluster_node_exec 0 "dmesg > /tmp/${dmesg_file_node0} 2>&1" > /dev/null 2>&1; then
            cluster_vm_scp "${CLUSTER_SSH_USER}@${node0_ip}:/tmp/${dmesg_file_node0}" \
                "$failure_logs_dir/${dmesg_file_node0}" 2>/dev/null && {
                chmod 666 "$failure_logs_dir/${dmesg_file_node0}" 2>/dev/null || true
                cluster_debug "Collected dmesg from node 0"
            }
            cluster_node_exec 0 "rm -f /tmp/${dmesg_file_node0}" > /dev/null 2>&1 || true
        fi

        local journal_file_node0="${test_name}-node0-journal.log"
        if cluster_node_exec 0 "[ -f /tmp/cluster_test_start_time ]" > /dev/null 2>&1; then
            local start_time_node0
            if start_time_node0=$(cluster_node_exec 0 "cat /tmp/cluster_test_start_time" 2>/dev/null); then
                if cluster_node_exec 0 "journalctl --since '${start_time_node0}' > /tmp/${journal_file_node0} 2>&1" > /dev/null 2>&1; then
                    cluster_vm_scp "${CLUSTER_SSH_USER}@${node0_ip}:/tmp/${journal_file_node0}" \
                        "$failure_logs_dir/${journal_file_node0}" 2>/dev/null && {
                        chmod 666 "$failure_logs_dir/${journal_file_node0}" 2>/dev/null || true
                        cluster_debug "Collected journalctl from node 0"
                    }
                    cluster_node_exec 0 "rm -f /tmp/${journal_file_node0}" > /dev/null 2>&1 || true
                fi
            fi
        else
            if cluster_node_exec 0 "journalctl -b > /tmp/${journal_file_node0} 2>&1" > /dev/null 2>&1; then
                cluster_vm_scp "${CLUSTER_SSH_USER}@${node0_ip}:/tmp/${journal_file_node0}" \
                    "$failure_logs_dir/${journal_file_node0}" 2>/dev/null && {
                    chmod 666 "$failure_logs_dir/${journal_file_node0}" 2>/dev/null || true
                    cluster_debug "Collected journalctl (full boot) from node 0"
                }
                cluster_node_exec 0 "rm -f /tmp/${journal_file_node0}" > /dev/null 2>&1 || true
            fi
        fi

        local lio_file_node0="${test_name}-node0-targetcli.log"
        if cluster_node_exec 0 "command -v targetcli >/dev/null 2>&1" > /dev/null 2>&1; then
            if cluster_node_exec 0 "targetcli ls > /tmp/${lio_file_node0} 2>&1" > /dev/null 2>&1; then
                cluster_vm_scp "${CLUSTER_SSH_USER}@${node0_ip}:/tmp/${lio_file_node0}" \
                    "$failure_logs_dir/${lio_file_node0}" 2>/dev/null && {
                    chmod 666 "$failure_logs_dir/${lio_file_node0}" 2>/dev/null || true
                    cluster_debug "Collected targetcli status from node 0"
                }
                cluster_node_exec 0 "rm -f /tmp/${lio_file_node0}" > /dev/null 2>&1 || true
            fi
        fi
    else
        cluster_debug "Failed to get IP for node 0, skipping log collection"
    fi

    # Report VM steal time delta for all nodes (detects hypervisor scheduling pressure)
    local steal_file="${test_name}-steal-time.log"
    local steal_path="$failure_logs_dir/${steal_file}"
    local steal_warned=0
    for node_num in $(seq 0 "$CLUSTER_NUM_NODES"); do
        local steal_start steal_end steal_delta steal_secs
        steal_start=$(cluster_node_exec "$node_num" "cat /tmp/cluster_steal_start 2>/dev/null" 2>/dev/null) || continue
        steal_end=$(cluster_node_exec "$node_num" "awk '/^cpu / {print \$9}' /proc/stat 2>/dev/null" 2>/dev/null) || continue
        if [ -n "$steal_start" ] && [ -n "$steal_end" ]; then
            steal_delta=$((steal_end - steal_start))
            steal_secs=$((steal_delta / 100))
            echo "node $node_num: steal_start=$steal_start steal_end=$steal_end delta=${steal_delta} (${steal_secs}s)" >> "$steal_path"
            if [ "$steal_secs" -ge 10 ]; then
                cluster_warn "Node $node_num: ${steal_secs}s of VM steal time during test — hypervisor scheduling pressure likely."
                steal_warned=1
            fi
        fi
    done
    if [ -f "$steal_path" ]; then
        chmod 666 "$steal_path" 2>/dev/null || true
        cluster_debug "Collected steal time report"
        if [ "$steal_warned" -eq 0 ]; then
            cluster_debug "No significant VM steal time detected"
        fi
    fi

    if [ $collected -gt 0 ]; then
        cluster_log "Debug logs collected from $collected node(s) to: $failure_logs_dir"
        return 0
    else
        cluster_warn "No debug logs were collected from any nodes"
        return 1
    fi
}

#
# cluster_run_test - Run a test script on the cluster
#
# This is the main entry point for test execution. It:
#   1. Sets up test variables ($node1, $nodes, etc.)
#   2. Sets up device variables ($dev1, $dev2, etc.)
#   3. Deploys test files to nodes
#   4. Executes the test script
#   5. Collects and reports results
#
# Arguments:
#   $1 - test_script (path to test script)
#
# Returns:
#   Exit code from test script
#
cluster_run_test() {
    local test_script="$1"

    if [ -z "$test_script" ]; then
        cluster_error "cluster_run_test: test_script is required"
        return 1
    fi

    if [ ! -f "$test_script" ]; then
        cluster_error "Test script not found: $test_script"
        return 1
    fi

    # Determine log file path with timestamp
    local script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    local results_dir="$script_dir/results"
    local test_name="$(basename "$test_script" .sh)"
    local timestamp="$(date +%m%d%H%M%S)"
    local log_file="$results_dir/log_${test_name}_${timestamp}.txt"

    # Export timestamp, test name, and results dir for use in child process
    export CLUSTER_TEST_TIMESTAMP="$timestamp"
    export CLUSTER_TEST_NAME="$test_name"
    export CLUSTER_RESULTS_DIR="$results_dir"

    # Create results directory
    if [ ! -d "$results_dir" ]; then
        mkdir -p "$results_dir" || {
            cluster_warn "Failed to create results directory: $results_dir"
            cluster_warn "Test output will not be logged to file"
            log_file=""
        }
        chmod 777 "$results_dir" 2>/dev/null || true
    fi

    cluster_log "=========================================="
    cluster_log "Running cluster test: $(basename "$test_script")"
    cluster_log "=========================================="
    cluster_log "Cluster ID: ${CLUSTER_ID}"
    cluster_log "Test nodes: ${CLUSTER_NUM_NODES}"
    cluster_log "=========================================="

    # Initialize log file
    if [ -n "$log_file" ]; then
        {
            echo "=========================================="
            echo "Cluster Test Log"
            echo "=========================================="
            echo "Test: $(basename "$test_script")"
            echo "Cluster ID: ${CLUSTER_ID}"
            echo "Test nodes: ${CLUSTER_NUM_NODES}"
            if [ -n "${CLUSTER_CONFIG_FILE:-}" ]; then
                echo "Config: $(basename "$CLUSTER_CONFIG_FILE")"
            fi
            if [ -n "${CLUSTER_GROUP_FILE:-}" ]; then
                echo "Group: $(basename "$CLUSTER_GROUP_FILE")"
            fi
            echo "Date: $(date)"
            echo "=========================================="
            echo ""
        } > "$log_file"

        chmod 666 "$log_file" 2>/dev/null || true

        if [ -n "${CLUSTER_REVERT_LOG:-}" ] && [ -f "$CLUSTER_REVERT_LOG" ] && [ -s "$CLUSTER_REVERT_LOG" ]; then
            echo "--- Cluster revert (before this test) ---" >> "$log_file"
            cat "$CLUSTER_REVERT_LOG" >> "$log_file"
            echo "--- End revert log ---" >> "$log_file"
            echo "" >> "$log_file"
            rm -f "$CLUSTER_REVERT_LOG"
            unset CLUSTER_REVERT_LOG
        fi

        cluster_log "Test output will be logged to: $log_file"
    fi

    # Start logging all output (setup + execution + results) to the log file.
    # Use a file descriptor with tee so that setup failures are also captured.
    local _log_tee_pid=""
    if [ -n "$log_file" ]; then
        exec 3>&1 4>&2
        exec > >(tee -a "$log_file") 2>&1
        _log_tee_pid=$!
    fi

    # Step 1: Set up test variables
    cluster_log "Setting up test variables"
    if ! cluster_setup_test_vars; then
        cluster_error "Failed to set up test variables"
        if [ -n "$log_file" ]; then
            exec 1>&3 2>&4 3>&- 4>&-
            wait "$_log_tee_pid" 2>/dev/null || true
        fi
        return 1
    fi

    # Step 2: Set up device variables
    cluster_log "Setting up device variables"
    if ! cluster_setup_device_vars; then
        cluster_error "Failed to set up device variables"
        if [ -n "$log_file" ]; then
            exec 1>&3 2>&4 3>&- 4>&-
            wait "$_log_tee_pid" 2>/dev/null || true
        fi
        return 1
    fi

    # Step 3: Clean up all old debug logs from previous tests
    cluster_log "Cleaning up old debug logs on all nodes"
    for node_num in $(seq 1 "$CLUSTER_NUM_NODES"); do
        # Remove all old logs from /var/log/lvm-test/
        cluster_node_exec "$node_num" "rm -rf /var/log/lvm-test/* 2>/dev/null || true" > /dev/null
        cluster_debug "Cleaned all debug logs on node $node_num"
    done

    # Clear kernel message buffer and save journal start time on all nodes
    cluster_log "Clearing kernel message buffer on all nodes"
    for node_num in $(seq 1 "$CLUSTER_NUM_NODES"); do
        # Clear dmesg
        cluster_node_exec "$node_num" "dmesg -C 2>/dev/null || true" > /dev/null
        cluster_debug "Cleared dmesg on node $node_num"

        # Save current timestamp for journalctl collection
        cluster_node_exec "$node_num" "date '+%Y-%m-%d %H:%M:%S' > /tmp/cluster_test_start_time" > /dev/null
        cluster_debug "Saved journal start time on node $node_num"
    done

    # Clear dmesg and save journal start time on node0 (storage exporter)
    cluster_node_exec 0 "dmesg -C 2>/dev/null || true" > /dev/null 2>&1
    cluster_node_exec 0 "date '+%Y-%m-%d %H:%M:%S' > /tmp/cluster_test_start_time" > /dev/null 2>&1
    cluster_debug "Cleared dmesg and saved journal start time on node 0"

    # Snapshot VM steal time on all nodes for detecting hypervisor scheduling pressure
    for node_num in $(seq 0 "$CLUSTER_NUM_NODES"); do
        cluster_node_exec "$node_num" "awk '/^cpu / {print \$9}' /proc/stat > /tmp/cluster_steal_start" > /dev/null 2>&1
    done
    cluster_debug "Saved steal time baseline on all nodes"

    # Step 4: Deploy test files (optional - only if not executing locally)
    # For now, we'll execute the test script locally with SSH commands
    # A future enhancement could deploy and execute remotely

    # Step 5: Execute test script
    cluster_log "=========================================="
    cluster_log "Executing test script: $(basename "$test_script")"
    cluster_log "=========================================="

    # Create a wrapper to track failures with line numbers
    local fail_info_file="/tmp/cluster_test_fail_$$"
    local wrapper_script="/tmp/cluster_test_wrapper_$$"

    cat > "$wrapper_script" <<WRAPPER_EOF
#!/bin/bash
set -eE

# Save test file path for trap
TEST_SCRIPT="\$1"

# ERR trap that captures the line number from the test script, not from helper functions
trap 'failure_handler' ERR

failure_handler() {
    local line_num=""
    local cmd=""

    # Walk the call stack backwards to find the frame from the test script
    # BASH_LINENO[i] contains the line number where frame i+1 was called
    # BASH_SOURCE[i+1] contains the file where frame i+1 was called from
    local i
    for (( i=0; i < \${#BASH_LINENO[@]}; i++ )); do
        # Get the absolute path of the source file at this frame
        local source_file="\${BASH_SOURCE[i+1]}"

        # Check if this frame is from the test script
        if [[ "\$source_file" == "\$TEST_SCRIPT" ]] || [[ "\$(realpath "\$source_file" 2>/dev/null)" == "\$(realpath "\$TEST_SCRIPT" 2>/dev/null)" ]]; then
            line_num=\${BASH_LINENO[i]}
            break
        fi
    done

    # If we didn't find the test script in the call stack, use the deepest frame
    if [[ -z "\$line_num" ]] || [[ "\$line_num" == "0" ]]; then
        # Try to find the last non-zero line number
        for (( i=\${#BASH_LINENO[@]}-1; i >= 0; i-- )); do
            if [[ \${BASH_LINENO[i]} -gt 0 ]]; then
                line_num=\${BASH_LINENO[i]}
                break
            fi
        done
    fi

    # Fallback to immediate caller if still not found
    if [[ -z "\$line_num" ]] || [[ "\$line_num" == "0" ]]; then
        line_num=\${BASH_LINENO[0]}
    fi

    # Extract the actual command from the test script
    if [[ -n "\$line_num" ]] && [[ -f "\$TEST_SCRIPT" ]]; then
        # Read the specific line from the test script and trim whitespace
        cmd=\$(sed -n "\${line_num}p" "\$TEST_SCRIPT" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
    fi

    # Fallback to BASH_COMMAND if we couldn't read from file
    if [[ -z "\$cmd" ]]; then
        cmd="\${BASH_COMMAND}"
    fi

    echo "FAILED_LINE:\$line_num:\$cmd" > $fail_info_file
    exit 1
}

# Source cluster test helpers if they exist (for test scripts to use)
if [ -f "$script_dir/shell/lib/cluster-test-helpers.sh" ]; then
    source "$script_dir/shell/lib/cluster-test-helpers.sh"
fi

source "\$TEST_SCRIPT"
WRAPPER_EOF
    chmod +x "$wrapper_script"

    # Execute test script
    bash "$wrapper_script" "$test_script" 2>&1
    local exit_code=$?

    # Capture failure information
    local failed_line=""
    local failed_command=""
    if [ $exit_code -ne 0 ] && [ -f "$fail_info_file" ]; then
        local fail_info=$(cat "$fail_info_file")
        failed_line=$(echo "$fail_info" | cut -d: -f2)
        failed_command=$(echo "$fail_info" | cut -d: -f3-)
    fi

    # Cleanup wrapper files
    rm -f "$wrapper_script" "$fail_info_file"

    # Step 6: Report results
    local fail_prefix="FAILED_"
    if [ $exit_code -ne 0 ] && cluster_is_known_failure "$test_name"; then
        fail_prefix="KNOWN_"
    fi

    cluster_log "=========================================="
    if [ $exit_code -eq 0 ]; then
        cluster_log "Test PASSED: $(basename "$test_script")"
    else
        if [ -n "$failed_line" ] && [ -n "$failed_command" ]; then
            cluster_error "Test ${fail_prefix%_}: $(basename "$test_script") line $failed_line $failed_command"
        else
            cluster_error "Test ${fail_prefix%_}: $(basename "$test_script") (exit code: $exit_code)"
        fi

        # Collect debug logs on failure
        cluster_log "Collecting debug logs from ${fail_prefix%_} test"
        cluster_collect_debug_logs "$test_name" "$fail_prefix" || {
            cluster_warn "Some debug logs could not be collected"
        }
    fi
    cluster_log "=========================================="

    # Step 7: Restore original LVM configuration
    cluster_restore_lvm_config

    # Stop logging to file and restore original stdout/stderr
    if [ -n "$log_file" ]; then
        {
            echo ""
            echo "=========================================="
            if [ $exit_code -eq 0 ]; then
                echo "Test PASSED: $(basename "$test_script")"
            else
                if [ -n "$failed_line" ] && [ -n "$failed_command" ]; then
                    echo "Test ${fail_prefix%_}: $(basename "$test_script") line $failed_line $failed_command"
                else
                    echo "Test ${fail_prefix%_}: $(basename "$test_script") (exit code: $exit_code)"
                fi
            fi
            echo "Date: $(date)"
            echo "=========================================="
        } >> "$log_file"

        exec 1>&3 2>&4 3>&- 4>&-
        wait "$_log_tee_pid" 2>/dev/null || true

        # Rename log file based on test result
        local final_log_file=""
        if [ $exit_code -eq 0 ]; then
            # Test passed - move to passed subdirectory
            mkdir -p "$results_dir/passed" 2>/dev/null || true
            chmod 777 "$results_dir/passed" 2>/dev/null || true
            final_log_file="$results_dir/passed/log_${test_name}_${timestamp}.txt"
            mv "$log_file" "$final_log_file" 2>/dev/null || final_log_file="$log_file"
        else
            final_log_file="$results_dir/${fail_prefix}${test_name}_${timestamp}.txt"
            mv "$log_file" "$final_log_file" 2>/dev/null || final_log_file="$log_file"
            chmod 666 "$final_log_file" 2>/dev/null || true
        fi

        cluster_log "Test log saved to: $final_log_file"
    fi

    return $exit_code
}

# Export functions for use in other scripts
export -f cluster_translate_devs
export -f cluster_node_exec
export -f cluster_all_exec
export -f cluster_nodes_exec
export -f cluster_setup_test_vars
export -f cluster_setup_device_vars
export -f cluster_discover_devices_on_node
export -f cluster_deploy_test_files
export -f cluster_restore_lvm_config
export -f cluster_stress_run
export -f cluster_stress_collect_logs
export -f cluster_collect_debug_logs
export -f cluster_run_test
