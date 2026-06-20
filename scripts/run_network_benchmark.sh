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

echo ""
echo "========================================"
echo "  Scenario A: Baseline (no netem)"
echo "========================================"
cleanup
"$BINARY" baseline
echo "[Done] Scenario A"

echo ""
echo "========================================"
echo "  Scenario B: Moderate (50ms RTT, 100Mbps)"
echo "========================================"
cleanup
apply_netem "delay 25ms, rate 100mbit" delay 25ms rate 100mbit
"$BINARY" moderate
cleanup
echo "[Done] Scenario B"

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