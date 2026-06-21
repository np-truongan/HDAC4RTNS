# Heuristic-Driven Adaptive Compression for Real-Time Network Streams

**Author:** Nguyen Pham Truong An — 23BI14004  
**Supervisor:** Dr. Giang Anh Tuan  
**Institution:** University of Science and Technology of Hanoi  

---

## Overview

Real‑time distributed systems exchange heterogeneous data streams — telemetry, structured messages (JSON), binary logs, and low‑entropy sensor readings — each with fundamentally different statistical properties. Traditional compression applies a single static strategy regardless of data characteristics, resulting in either poor compression ratios or unacceptable latency.

This project implements a heuristic‑driven adaptive compression framework that analyses outgoing byte streams at runtime, estimates their statistical properties (Shannon entropy and sequential smoothness), and dynamically selects an optimal encoding strategy per chunk. The framework balances compression ratio against computational overhead by:

- Applying preprocessing transformations (delta encoding, bit‑packing) when beneficial,
- Choosing the downstream compression algorithm (LZ4, Zstandard, or Gzip) based on the transformed data’s entropy and the preprocessing decision, and
- Running as an asynchronous producer‑consumer pipeline suitable for real‑time network transmission.

The framework is evaluated under both simulated network conditions (loopback TCP with `tc netem`) and real two‑machine conditions, demonstrating that adaptive selection reduces end‑to‑end latency when bandwidth is the dominant cost factor.

---

## Features

- Shannon entropy and smoothness computed in O(n) with a fixed 256‑element working set.
- Couples preprocessing choice with downstream algorithm selection (e.g., Delta + ZSTD achieves 59% better ratio than Delta + LZ4 on telemetry).
- LZ4, Zstandard, Gzip, delta encoding, and bit‑packing, all using real library calls (no mocks).
- Producer‑consumer model with FIFO ordering, thread‑safe queue, and resource snapshotting (CPU time, peak RSS) per chunk.
- Fixed‑header protocol with chunk IDs, timestamps, and decompression‑aware headers for reliable network transmission.
- 15+ benchmarks covering correctness, routing, sensitivity, chunk size, Pareto frontier, preprocessing gains, ablation, and streaming stability.
- Loopback with `tc netem` (three scenarios) and two‑machine TCP (laptop ↔ Pi) with RTT/2 latency measurement.
- Header‑only `stats.h` computes means, standard deviations, 95% confidence intervals, and exports CSV summaries.

---

## Architecture

All compression logic is encapsulated in a static library (`adaptive_core`). Benchmarks and tests link against this library and contain only experiment orchestration.

```
┌─────────────────────────────────────────────┐
│             Data Generators                 │
│   Telemetry | JSON | Binary | Nibble        │
├─────────────────────────────────────────────┤
│           Heuristic Probes                  │
│   Shannon Entropy | Sequential Smoothness   │
├─────────────────────────────────────────────┤
│           Decision Engine                   │
│   EngineConfig thresholds → Decision        │
├─────────────────────────────────────────────┤
│        Compression Strategies               │
│   LZ4 | Zstd | Gzip | Delta | BitPack      │
└─────────────────────────────────────────────┘
          ↑ all layers sit inside ↓
┌─────────────────────────────────────────────┐
│       Async Streaming Pipeline              │
│   Producer/Consumer | Metrics | CSV Export  │
└─────────────────────────────────────────────┘
          ↓ evaluated over ↓
┌─────────────────────────────────────────────┐
│         Network Evaluation                  │
│   Loopback (tc netem) | Two‑Machine TCP     │
└─────────────────────────────────────────────┘
```

---

## Project Structure

```
.
├── include/                     # Public headers
│   ├── types.h                  # Core types: Chunk, Features, Decision, Metrics
│   ├── generators.h             # Workload generator declarations
│   ├── heuristics.h             # Entropy + smoothness probes
│   ├── strategies.h             # Compression + preprocessing
│   ├── engine.h                 # EngineConfig + decide()
│   ├── pipeline.h               # Async Pipeline
│   ├── framing.h                # TCP framing
│   ├── resource_stats.h         # CPU/RSS measurement helpers
│   └── stats.h                  # Statistical analysis (header‑only)
│
├── src/                         # Library implementation
│   ├── generators.cpp
│   ├── heuristics.cpp
│   ├── strategies.cpp
│   ├── engine.cpp
│   ├── pipeline.cpp
│   └── framing.cpp
│
├── benchmarks/                  # All experiment executables
│   ├── benchmark_baseline.cpp
│   ├── benchmark_chunksize.cpp
│   ├── benchmark_comparison.cpp
│   ├── benchmark_ablation.cpp
│   ├── benchmark_boundary.cpp
│   ├── benchmark_sensitivity.cpp
│   ├── benchmark_heuristic_single.cpp
│   ├── benchmark_pareto.cpp
│   ├── benchmark_pipeline_integration.cpp
│   ├── benchmark_preprocessing.cpp
│   ├── benchmark_routing_validation.cpp
│   ├── benchmark_streaming_stability.cpp
│   ├── network_benchmark.cpp
│   ├── network_receiver.cpp
│   └── network_sender.cpp
│
├── tests/                       # Correctness tests
│   ├── test_roundtrip.cpp       # 17 round‑trip tests (run first)
│   └── test_engine.cpp          # Decision engine validation
│
├── scripts/
│   └── run_network_benchmark.sh # Automated loopback scenario runner
│
├── CMakeLists.txt
└── README.md
```

---

## Dependencies

| Library | Version | Purpose |
|---------|---------|---------|
| [LZ4](https://github.com/lz4/lz4) | ≥ 1.9.4 | Low‑latency compression |
| [Zstandard](https://github.com/facebook/zstd) | ≥ 1.5.5 | High‑ratio compression |
| [zlib](https://zlib.net) | ≥ 1.3 | Gzip baseline |
| [iproute2](https://wiki.linuxfoundation.org/networking/iproute2) | — | `tc netem` (Linux/WSL2 only) |
| CMake | ≥ 3.16 | Build system |
| Ninja (optional) | — | Faster builds |

---

## Building

### Linux / WSL2 (recommended for network evaluation)

```bash
sudo apt-get install cmake ninja-build pkg-config build-essential \
                     liblz4-dev libzstd-dev zlib1g-dev iproute2

mkdir build && cd build
cmake .. -G "Ninja" -DCMAKE_BUILD_TYPE=Release
ninja
```

### macOS (benchmarks only; no tc netem)

```bash
brew install cmake ninja lz4 zstd zlib

mkdir build && cd build
cmake .. -G "Ninja" -DCMAKE_BUILD_TYPE=Release
ninja
```

### Windows (MSYS2 MinGW64 – loopback only)

```bash
pacman -S mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja \
          mingw-w64-x86_64-lz4 mingw-w64-x86_64-zstd \
          mingw-w64-x86_64-zlib mingw-w64-x86_64-gcc \
          mingw-w64-x86_64-pkg-config

mkdir build && cd build
cmake .. -G "Ninja" -DCMAKE_BUILD_TYPE=Release
ninja
```

> `network_sender` and `network_receiver` require POSIX sockets and are only built on Linux/macOS/WSL2. Native Windows (Winsock) is not supported.

---

## Running the Tests and Benchmarks

All executables are placed in the `build/` directory. Results are written to `build/results/` as CSV files.

### 1. Correctness Gate (always run first)

```bash
./test_roundtrip    # 17 round‑trip tests
./test_engine       # Decision engine routing validation
```

All tests must pass before any benchmark results are considered valid.

### 2. Static Baselines

```bash
./benchmark_baseline           # LZ4, Zstd, Gzip at 4 KB
./benchmark_chunksize          # Sweep 512 B → 1 MB
./benchmark_comparison         # 4‑system head‑to‑head at 4 KB
```

### 3. Adaptive Framework Analysis

```bash
./benchmark_ablation                  # All preprocessing‑algorithm combinations
./benchmark_boundary                  # Smooth/noisy transition behaviour
./benchmark_sensitivity               # Threshold sweep (entropy + smoothness)
./benchmark_heuristic_single          # Single‑workload heuristic evaluation
./benchmark_pareto                    # Ratio‑speed Pareto frontier
./benchmark_pipeline_integration      # End‑to‑end pipeline ordering
./benchmark_preprocessing_validation  # Preprocessing gain quantification
./benchmark_routing_validation        # Decision routing verification
./benchmark_streaming_stability       # Per‑chunk latency stability
```

### 4. Network Evaluation

#### Loopback (single machine, Linux/WSL2 only, requires sudo)

```bash
cd build
sudo ../scripts/run_network_benchmark.sh   # runs all three scenarios
```

To run a single scenario:

```bash
./network_benchmark baseline
./network_benchmark moderate       # 25 ms delay, 100 Mbps
./network_benchmark constrained    # 100 ms delay, 10 Mbps, 0.5% loss
```

#### Two‑Machine 

Sync clocks on both machines before running:

```bash
sudo ntpdate pool.ntp.org   # run on both
```

On the receiver (e.g., Raspberry Pi):

```bash
sudo ufw allow 54321    # or your custom port
./network_receiver [port] [output_dir]   # port defaults to 54321
```

On the sender (e.g. Linux laptop):

```bash
./network_sender <receiver_ip> [scenario] [output_dir] [port]
# Example:
./network_sender 192.168.1.42 moderate ./csv 54321
# All optional arguments after IP default to "baseline", "./results", 54321.
```

For moderate/constrained scenarios, apply `tc netem` on the sender's network interface before running:

```bash
# Check interface name: ip link
sudo tc qdisc add dev eth0 root netem delay 25ms rate 100mbit
./network_sender 192.168.1.42 moderate
sudo tc qdisc del dev eth0 root
```

---

## Output Files

All benchmarks write CSV files to `build/results/`. Key outputs include:

| File | Description |
|------|-------------|
| `baseline.csv` | Static algorithm performance at multiple chunk sizes. |
| `chunksize_sweep.csv` | Ratio, latency, CPU, RSS across all chunk sizes and systems. |
| `static_comparison.csv` | 4‑system comparison at 4 KB with CPU/RSS. |
| `ablation.csv` | Preprocessing‑algorithm coupling ratios and latencies. |
| `boundary_results.csv` | Engine behaviour across five noise levels. |
| `sensitivity.csv` | Threshold sweep results (ratio, latency, throughput). |
| `pareto_curve.csv` | Ratio‑speed Pareto frontier for all algorithm levels. |
| `routing_validation.csv` | Per‑chunk features and decisions for all workloads. |
| `network_{scenario}.csv` | Per‑chunk network results (e2e latency, ratio, CPU, RSS). |
| `network_stats_{scenario}.csv` | Statistical summaries (mean, CI, min, max). |
| `sender_*.csv` | Two‑machine sender output (RTT/2 latency, CPU, RSS). |
| `receiver_*.csv` | Two‑machine receiver output (decompression CPU, RSS). |