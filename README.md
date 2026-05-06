# Heuristic-Driven Adaptive Compression for Real-Time Network Streams

**Author:** Nguyen Pham Truong An — 23BI14004  

---

## Overview

Real-time distributed systems exchange heterogeneous data streams — telemetry, structured messages, binary logs — each with fundamentally different statistical properties. Traditional compression applies a single static strategy regardless of data characteristics, resulting in either poor compression ratios or unacceptable latency.

This project implements a **heuristic-driven adaptive compression framework** that analyzes outgoing byte streams at runtime, estimates their statistical properties, and dynamically selects an optimal encoding strategy per chunk — balancing compression ratio against latency overhead.

---

## Architecture

```
┌─────────────────────────────────────────┐
│           Data Generators               │
│   Telemetry | JSON | Binary             │
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
```

All layers are implemented as a static library (`adaptive_core`). Benchmark executables link against it and never contain logic — only experiment orchestration.

---

## Project Structure

```
adaptive_compression/
├── include/
│   ├── types.h          # Shared types: Chunk, Features, Decision, Metrics
│   ├── generators.h     # Workload generator declarations
│   ├── heuristics.h     # Entropy + smoothness probe declarations
│   ├── strategies.h     # Compression + preprocessing declarations
│   ├── engine.h         # EngineConfig + decide() declaration
│   └── pipeline.h       # Async Pipeline class declaration
│
├── src/
│   ├── generators.cpp   # Telemetry, JSON, Binary data generation
│   ├── heuristics.cpp   # Shannon entropy, smoothness estimation
│   ├── strategies.cpp   # Real LZ4/Zstd/Gzip + delta + bit-packing
│   ├── engine.cpp       # Threshold-based decision logic
│   └── pipeline.cpp     # Async producer/consumer + metrics/CSV
│
├── benchmarks/
│   ├── baseline.cpp     # Week 1: static algorithm baselines
│   ├── adaptive.cpp     # Week 3–4: adaptive framework evaluation
│   └── sensitivity.cpp  # Week 7: threshold sensitivity analysis
│
├── results/             # CSV outputs (git-ignored)
├── CMakeLists.txt
└── README.md
```

---

## Dependencies

| Library | Purpose |
|---------|---------|
| [LZ4](https://github.com/lz4/lz4) | Fast low-latency compression |
| [Zstandard](https://github.com/facebook/zstd) | High-ratio compression |
| [zlib](https://zlib.net) | Gzip baseline comparison |

---

## Building

### Linux / WSL2

```bash
sudo apt-get install cmake liblz4-dev libzstd-dev zlib1g-dev pkg-config build-essential

mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j4
```

### Windows (MSYS2 MinGW64)

```bash
pacman -S mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja \
          mingw-w64-x86_64-lz4 mingw-w64-x86_64-zstd \
          mingw-w64-x86_64-zlib mingw-w64-x86_64-gcc \
          mingw-w64-x86_64-pkg-config

mkdir build && cd build
cmake .. -G "Ninja" -DCMAKE_BUILD_TYPE=Release
ninja
```

---

## Running Benchmarks

All benchmarks write CSV output to `build/results/`.

```bash
# Week 1 — static baselines
./benchmark_baseline        # Linux
./benchmark_baseline.exe    # Windows

# Week 3–4 — adaptive framework  (coming)
./benchmark_adaptive

# Week 7 — sensitivity analysis  (coming)
./benchmark_sensitivity
```

---

## Key Design Decisions

**No mocks, ever.** Every compression call invokes a real library. Simulated compression ratios or stubbed latency values have no place in this codebase.

**Preprocessing-aware decisions.** The decision engine accounts for the fact that delta encoding and bit-packing increase redundancy before compression — so algorithm selection is always made on the transformed data's effective entropy, not the raw value.

**`adaptive_core` is a library, not a script.** All logic lives in `src/`. Benchmarks are thin executables that orchestrate experiments and write CSVs. This makes it straightforward to integrate the framework into external client-server architectures.

**Gzip is baseline-only.** Week 1 results confirm Gzip latency is 10–100x higher than LZ4/Zstd at equivalent chunk sizes. It is included in static baseline comparisons for completeness but excluded from the adaptive framework.

---

## Results Summary (Week 1 Baseline)

All measurements on 1MB datasets, chunk size 4096 bytes.

| Dataset | Entropy | Algorithm | Ratio | Throughput |
|---------|---------|-----------|-------|------------|
| Telemetry | 5.85 | LZ4 | 0.741 | 242 MB/s |
| Telemetry | 5.85 | ZSTD | 0.537 | 132 MB/s |
| Telemetry | 5.85 | GZIP | 0.447 | 36 MB/s |
| JSON | 4.22 | LZ4 | 0.019 | 3475 MB/s |
| JSON | 4.22 | ZSTD | 0.017 | 750 MB/s |
| JSON | 4.22 | GZIP | 0.028 | 272 MB/s |
| Binary | 7.95 | LZ4 | 1.004 | 2738 MB/s |
| Binary | 7.95 | ZSTD | 1.002 | 774 MB/s |
| Binary | 7.95 | GZIP | 1.006 | 45 MB/s |

**Observation:** No single algorithm wins across all workloads — the core motivation for adaptive selection.
