# Heuristic-Driven Adaptive Compression for Real-Time Network Streams

**Author:** Nguyen Pham Truong An — 23BI14004
**Supervisor:** Dr. Giang Anh Tuan
**Institution:** University of Science and Technology of Hanoi

---

## Overview

Real-time distributed systems exchange heterogeneous data streams — telemetry, structured messages, binary logs — each with fundamentally different statistical properties. Traditional compression applies a single static strategy regardless of data characteristics, resulting in either poor compression ratios or unacceptable latency.

This project implements a **heuristic-driven adaptive compression framework** that analyses outgoing byte streams at runtime, estimates their statistical properties, and dynamically selects an optimal encoding strategy per chunk — balancing compression ratio against latency overhead.

The framework is evaluated under both **simulated network conditions** (loopback TCP with `tc netem`) and **real two-machine conditions** (laptop ↔ Raspberry Pi), validating that adaptive selection reduces end-to-end latency when bandwidth is the dominant cost factor.

---

## Architecture

```
┌─────────────────────────────────────────┐
│           Data Generators               │
│  Telemetry | JSON | Binary | Nibble     │
├─────────────────────────────────────────┤
│           Heuristic Probes              │
│   Shannon Entropy | Smoothness          │
├─────────────────────────────────────────┤
│           Decision Engine               │
│   EngineConfig thresholds → Decision    │
├─────────────────────────────────────────┤
│        Compression Strategies           │
│   LZ4 | Zstd | Gzip | Delta | BitPack  │
└─────────────────────────────────────────┘
          ↑ all layers sit inside ↓
┌─────────────────────────────────────────┐
│       Async Streaming Pipeline          │
│   Producer/Consumer | Metrics | CSV     │
└─────────────────────────────────────────┘
          ↓ evaluated over ↓
┌─────────────────────────────────────────┐
│         Network Evaluation              │
│  Loopback (tc netem) | Two-Machine TCP  │
└─────────────────────────────────────────┘
```

All compression layers are implemented as a static library (`adaptive_core`). Benchmark executables link against it and contain only experiment orchestration.

---

## Project Structure

```
adaptive_compression/
├── include/
│   ├── types.h                        # Shared types: Chunk, Features, Decision, Metrics
│   ├── generators.h                   # Workload generator declarations
│   ├── heuristics.h                   # Entropy + smoothness probe declarations
│   ├── strategies.h                   # Compression + preprocessing declarations
│   ├── engine.h                       # EngineConfig + decide() declaration
│   ├── pipeline.h                     # Async Pipeline class declaration
│   ├── framing.h                      # TCP frame send/receive declarations
│   └── stats.h                        # Statistical analysis (header-only)
│
├── src/
│   ├── generators.cpp                 # Telemetry, JSON, Binary, Nibble generation
│   ├── heuristics.cpp                 # Shannon entropy + smoothness estimation
│   ├── strategies.cpp                 # LZ4 / Zstd / Gzip + delta + bit-packing
│   ├── engine.cpp                     # Threshold-based decision logic
│   ├── pipeline.cpp                   # Async producer/consumer + metrics/CSV
│   └── framing.cpp                    # TCP frame serialisation and timing
│
├── benchmarks/
│   ├── benchmark_baseline.cpp         # Static algorithm baselines at 4 KB
│   ├── benchmark_chunksize.cpp        # Chunk size sweep: 512 B → 64 KB
│   ├── benchmark_comparison.cpp       # 4-system head-to-head comparison
│   ├── benchmark_ablation.cpp         # Preprocessing-algorithm coupling ablation
│   ├── benchmark_boundary.cpp         # Engine behaviour at smooth/noisy boundary
│   ├── benchmark_sensitivity.cpp      # Entropy + smoothness threshold sweep
│   ├── benchmark_heuristic_single.cpp # Single-probe heuristic evaluation
│   ├── benchmark_pareto.cpp           # Ratio-speed Pareto frontier analysis
│   ├── benchmark_pipeline_integration.cpp  # End-to-end pipeline integration test
│   ├── benchmark_preprocessing.cpp    # Preprocessing transformation evaluation
│   ├── benchmark_routing_validation.cpp    # Decision engine routing verification
│   ├── benchmark_streaming_stability.cpp   # Per-chunk latency stability over time
│   ├── network_benchmark.cpp          # Loopback TCP evaluation (tc netem)
│   ├── network_receiver.cpp           # Two-machine receiver (run on Pi / remote)
│   └── network_sender.cpp             # Two-machine sender (run on laptop)
│
├── tests/
│   └── test_roundtrip.cpp             # 17 correctness tests — run before benchmarks
│
├── scripts/
│   └── run_network_benchmark.sh       # Reproduces all three loopback scenarios
│
├── results/                           # CSV outputs (git-ignored)
├── .gitignore
├── CMakeLists.txt
└── README.md
```

---

## Dependencies

| Library | Version | Purpose |
|---------|---------|---------|
| [LZ4](https://github.com/lz4/lz4) | 1.9.4 | Fast low-latency compression |
| [Zstandard](https://github.com/facebook/zstd) | 1.5.5 | High-ratio compression |
| [zlib](https://zlib.net) | 1.3 | Gzip baseline comparison |
| [iproute2](https://wiki.linuxfoundation.org/networking/iproute2) | — | `tc netem` network simulation (Linux/WSL2 only) |

---

## Building

### Linux / WSL2 (recommended)

```bash
sudo apt-get install cmake ninja-build pkg-config build-essential \
                     liblz4-dev libzstd-dev zlib1g-dev iproute2

mkdir build && cd build
cmake .. -G "Ninja" -DCMAKE_BUILD_TYPE=Release
ninja
```

### Raspberry Pi (receiver for two-machine experiments)

Same as Linux above. The Pi only runs `network_receiver` — no `tc netem` required on the Pi side.

### macOS (optional — no tc netem support)

```bash
brew install cmake ninja lz4 zstd zlib

mkdir build && cd build
cmake .. -G "Ninja" -DCMAKE_BUILD_TYPE=Release
ninja
```

> `tc netem` is Linux-only. On macOS, use `dnctl` + `pfctl` for traffic shaping if needed (see comments in `network_sender.cpp`).

### Windows (MSYS2 MinGW64 — loopback benchmarks only)

```bash
pacman -S mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja \
          mingw-w64-x86_64-lz4 mingw-w64-x86_64-zstd \
          mingw-w64-x86_64-zlib mingw-w64-x86_64-gcc \
          mingw-w64-x86_64-pkg-config

mkdir build && cd build
cmake .. -G "Ninja" -DCMAKE_BUILD_TYPE=Release
ninja
```

> `network_sender` and `network_receiver` require Linux or WSL2. Native Windows (Winsock) is not supported.

---

## Running

### Step 0 — Correctness gate (always run first)

```bash
./test_roundtrip
```

All 17 round-trip tests must pass before any benchmark results are considered valid.

### Step 1 — Static baselines

```bash
./benchmark_baseline          # compression ratio + latency at 4 KB
./benchmark_chunksize         # sweep 512 B → 64 KB across all workloads
./benchmark_comparison        # 4-system comparison at 4 KB
```

### Step 2 — Adaptive framework analysis

```bash
./benchmark_ablation              # all 6 preprocessing-algorithm combinations
./benchmark_boundary              # engine behaviour at smooth/noisy transition
./benchmark_sensitivity           # entropy + smoothness threshold sweep
./benchmark_heuristic_single      # single-probe evaluation
./benchmark_pareto                # ratio-speed Pareto frontier
./benchmark_pipeline_integration  # end-to-end pipeline correctness
./benchmark_preprocessing         # preprocessing transformation evaluation
./benchmark_routing_validation    # decision engine routing verification
./benchmark_streaming_stability   # per-chunk latency stability over time
```

### Step 3 — Loopback network evaluation (Linux/WSL2, requires sudo)

```bash
cd build
sudo ../scripts/run_network_benchmark.sh
```

Runs all three scenarios sequentially. To run individually:

```bash
./network_benchmark baseline
./network_benchmark moderate      # 25 ms delay, 100 Mbps
./network_benchmark constrained   # 100 ms delay, 10 Mbps, 0.5% loss
```

### Step 4 — Two-machine network evaluation (laptop + Raspberry Pi)

**Sync clocks on both machines before running:**

```bash
sudo ntpdate pool.ntp.org   # run on BOTH machines
```

**On the Raspberry Pi (receiver):**

```bash
sudo ufw allow 54321    # if firewall is active
./network_receiver
```

**On your laptop (sender):**

```bash
# Find Pi's IP: run `hostname -I` on the Pi
./network_sender 192.168.1.42 baseline
./network_sender 192.168.1.42 moderate
./network_sender 192.168.1.42 constrained
```

> For `moderate` and `constrained` scenarios in two-machine mode, apply `tc netem` on the **sender's** network interface before running:
> ```bash
> # Check your interface name first: ip link
> sudo tc qdisc add dev eth0 root netem delay 25ms rate 100mbit
> ./network_sender 192.168.1.42 moderate
> sudo tc qdisc del dev eth0 root
> ```

---

## Network Scenarios

| Scenario | One-way Delay | Bandwidth | Packet Loss | Dominant Factor |
|----------|--------------|-----------|-------------|-----------------|
| Baseline | none | unlimited | none | CPU / compression time |
| Moderate | 25 ms (50 ms RTT) | 100 Mbps | none | Network transit |
| Constrained | 100 ms (200 ms RTT) | 10 Mbps | 0.5% | Bandwidth + retransmission |

---

## Results Summary

### Compression Ratio (4 KB chunks, n = 300)

| System | Telemetry | JSON | Binary | Nibble |
|--------|-----------|------|--------|--------|
| LZ4 | 0.741 | 0.019 | 1.004 | 1.000 |
| ZSTD | 0.531 | 0.017 | 1.002 | 0.509 |
| Gzip | 0.443 | 0.028 | 1.006 | 0.575 |
| **Adaptive** | **0.279** | **0.017** | **1.004** | **0.502** |

### End-to-End Latency — Constrained Scenario (ms, n = 300)

| System | Telemetry | JSON | Binary | Nibble |
|--------|-----------|------|--------|--------|
| LZ4 | 193.5 | 193.4 | 378.1 | 421.3 |
| ZSTD | 142.0 | 141.8 | 301.7 | 357.8 |
| Gzip | 141.5 | 141.2 | 280.5 | 354.1 |
| **Adaptive** | **143.2** | **143.1** | **264.2** | **348.4** |

**Key finding:** Under constrained bandwidth the latency ranking inverts relative to baseline — LZ4 is fastest at baseline (lowest CPU overhead) but slowest under constrained bandwidth (most bytes transmitted). The adaptive framework achieves the lowest Binary and Nibble latency under constrained conditions by routing each workload to the strategy that minimises total transmission cost.

---

## Key Design Decisions

**No mocks, ever.** Every compression call invokes a real library. Simulated compression ratios or stubbed latency values have no place in this codebase.

**Preprocessing-aware decisions.** When delta encoding or bit-packing is applied, the downstream algorithm is always Zstandard — which exploits the increased redundancy far more effectively than LZ4's simpler match finder (59% ratio improvement on Telemetry: Delta+ZSTD vs Delta+LZ4).

**`adaptive_core` is a library, not a script.** All logic lives in `src/`. Benchmarks are thin executables that orchestrate experiments and write CSVs. This makes the framework straightforward to integrate into external client-server architectures.

**4 KB default chunk size.** Empirically justified by the chunk size sweep: ratio improves 31% from 512 B to 4 KB for Telemetry, then flattens, while per-chunk latency continues growing proportionally beyond 4 KB.

**Gzip is included for comparison only.** Gzip latency is 10–100× higher than LZ4/Zstd at equivalent chunk sizes. It appears in static baselines and network comparisons for completeness but is not a candidate for the adaptive framework.

---

## Reproducibility

All workload generators use fixed seeds (`std::mt19937`) — results are deterministic across runs on the same machine. `scripts/run_network_benchmark.sh` applies and removes `tc netem` conditions automatically between scenarios to prevent configuration carry-over.