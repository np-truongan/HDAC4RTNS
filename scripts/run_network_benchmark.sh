#!/bin/bash
#
# scripts/run_network_benchmark.sh
#
# Runs the network benchmark under three standardized network
# conditions using tc netem on the loopback interface.
#
# Must be run as root (or with sudo) for tc commands.
# Run from the build/ directory:
#   cd build && sudo ../scripts/run_network_benchmark.sh

set -e

BINARY="./network_benchmark"
IFACE="lo"

cleanup() {
    echo "[netem] Removing any existing qdisc on $IFACE..."
    tc qdisc del dev "$IFACE" root 2>/dev/null || true
}

apply_netem() {
    local desc="$1"
    shift
    echo "[netem] Applying: $desc"
    tc qdisc add dev "$IFACE" root netem $@
    tc qdisc show dev "$IFACE"
}

# ---------------------------------------------------------------
#  Scenario A: Baseline — no netem, loopback only
# ---------------------------------------------------------------
echo ""
echo "========================================"
echo "  Scenario A: Baseline (no netem)"
echo "========================================"
cleanup
"$BINARY" baseline
echo "[Done] Scenario A"

# ---------------------------------------------------------------
#  Scenario B: Moderate — 50ms RTT, 100Mbps bandwidth
#  netem delay 25ms = 50ms RTT (delay is one-way)
# ---------------------------------------------------------------
echo ""
echo "========================================"
echo "  Scenario B: Moderate (50ms RTT, 100Mbps)"
echo "========================================"
cleanup
apply_netem "delay 25ms, rate 100mbit" delay 25ms rate 100mbit
"$BINARY" moderate
cleanup
echo "[Done] Scenario B"

# ---------------------------------------------------------------
#  Scenario C: Constrained — 200ms RTT, 10Mbps, 0.5% loss
# ---------------------------------------------------------------
echo ""
echo "========================================"
echo "  Scenario C: Constrained (200ms RTT, 10Mbps, 0.5% loss)"
echo "========================================"
cleanup
apply_netem "delay 100ms, rate 10mbit, loss 0.5%" delay 100ms rate 10mbit loss 0.5%
"$BINARY" constrained
cleanup
echo "[Done] Scenario C"

echo ""
echo "All scenarios complete."
echo "Results in: build/results/"