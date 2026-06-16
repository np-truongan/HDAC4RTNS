cmake_minimum_required(VERSION 3.16)
project(AdaptiveCompression CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(-O2 -Wall -Wextra -Wpedantic)

# ---------------------------------------------------------------
#  External dependencies
# ---------------------------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(LZ4  REQUIRED liblz4)
pkg_check_modules(ZSTD REQUIRED libzstd)
find_package(ZLIB    REQUIRED)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------
#  adaptive_core — static library containing all system logic.
#
#  Every benchmark and test links against this target.
#  No compression logic lives outside this library.
# ---------------------------------------------------------------
add_library(adaptive_core STATIC
    src/generators.cpp   # Synthetic workload generators
    src/heuristics.cpp   # Shannon entropy + smoothness probes
    src/strategies.cpp   # LZ4 / ZSTD / Gzip + delta + bit-packing
    src/engine.cpp       # Threshold-based decision engine
    src/pipeline.cpp     # Async producer-consumer pipeline
    src/framing.cpp      # TCP frame send / receive
)

target_include_directories(adaptive_core PUBLIC
    include
    ${LZ4_INCLUDE_DIRS}
    ${ZSTD_INCLUDE_DIRS}
)

target_link_libraries(adaptive_core PUBLIC
    ${LZ4_LIBRARIES}
    ${ZSTD_LIBRARIES}
    ZLIB::ZLIB
    Threads::Threads
)

# ---------------------------------------------------------------
#  Create results/ directory on every build
# ---------------------------------------------------------------
add_custom_target(make_results_dir ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/results
)

# ---------------------------------------------------------------
#  Correctness tests
#  Run this before any benchmark to verify all encode/decode
#  round-trips return bit-identical output.
# ---------------------------------------------------------------
add_executable(test_roundtrip tests/test_roundtrip.cpp)
target_link_libraries(test_roundtrip PRIVATE adaptive_core)

# ---------------------------------------------------------------
#  Static baseline benchmark
#  Measures LZ4, ZSTD, and Gzip across three workloads and four
#  chunk sizes. Establishes ground-truth performance data and
#  motivates adaptive selection.
# ---------------------------------------------------------------
add_executable(benchmark_baseline benchmarks/benchmark_baseline.cpp)
target_link_libraries(benchmark_baseline PRIVATE adaptive_core)

# ---------------------------------------------------------------
#  Heuristic probe validation — single workload
#  Validates entropy and smoothness probes on JSON data.
#  Reports per-chunk feature values and decision log.
# ---------------------------------------------------------------
add_executable(benchmark_heuristic_single
    benchmarks/benchmark_heuristic_single.cpp)
target_link_libraries(benchmark_heuristic_single PRIVATE adaptive_core)

# ---------------------------------------------------------------
#  Decision engine routing validation — all workloads
#  Verifies that each workload type triggers the correct
#  preprocessing and algorithm path. Reports feature
#  distributions used to justify threshold calibration.
# ---------------------------------------------------------------
add_executable(benchmark_routing_validation
    benchmarks/benchmark_routing_validation.cpp)
target_link_libraries(benchmark_routing_validation PRIVATE adaptive_core)

# ---------------------------------------------------------------
#  Pipeline integration benchmark
#  First end-to-end exercise of the async Pipeline class with
#  interleaved heterogeneous workloads. Verifies ordering,
#  strategy switching continuity, and per-workload breakdown.
# ---------------------------------------------------------------
add_executable(benchmark_pipeline_integration
    benchmarks/benchmark_pipeline_integration.cpp)
target_link_libraries(benchmark_pipeline_integration PRIVATE adaptive_core)

# ---------------------------------------------------------------
#  Preprocessing validation benchmark
#  Exercises all four decision paths: Delta+ZSTD, BitPack+ZSTD,
#  None+ZSTD, None+LZ4. Quantifies the compression gain from
#  each preprocessing transformation.
# ---------------------------------------------------------------
add_executable(benchmark_preprocessing
    benchmarks/benchmark_preprocessing.cpp)
target_link_libraries(benchmark_preprocessing PRIVATE adaptive_core)

# ---------------------------------------------------------------
#  Streaming stability benchmark
#  Runs a dedicated producer thread with inter-arrival pacing.
#  Reports latency percentiles (p50/p95/p99), jitter, and
#  strategy switching continuity under continuous execution.
# ---------------------------------------------------------------
add_executable(benchmark_streaming_stability
    benchmarks/benchmark_streaming_stability.cpp)
target_link_libraries(benchmark_streaming_stability PRIVATE adaptive_core)

# ---------------------------------------------------------------
#  Static comparison benchmark
#  Runs LZ4, ZSTD, Gzip, and Adaptive across all four workloads
#  at the default 4 KB chunk size. Produces the main comparison
#  table used in the thesis evaluation chapter.
# ---------------------------------------------------------------
add_executable(benchmark_comparison benchmarks/benchmark_comparison.cpp)
target_link_libraries(benchmark_comparison PRIVATE adaptive_core)

# ---------------------------------------------------------------
#  Threshold sensitivity analysis
#  Sweeps entropy threshold (5.0 to 7.5) and smoothness threshold
#  (0.5 to 0.9) independently, reporting how ratio and latency
#  respond to threshold perturbation.
# ---------------------------------------------------------------
add_executable(benchmark_sensitivity benchmarks/benchmark_sensitivity.cpp)
target_link_libraries(benchmark_sensitivity PRIVATE adaptive_core)

# ---------------------------------------------------------------
#  Chunk size sweep benchmark
#  Evaluates all four systems across eight chunk sizes (512 B to
#  64 KB). Characterises the ratio-latency trade-off and provides
#  empirical justification for the 4 KB default.
# ---------------------------------------------------------------
add_executable(benchmark_chunksize benchmarks/benchmark_chunksize.cpp)
target_link_libraries(benchmark_chunksize PRIVATE adaptive_core)

# ---------------------------------------------------------------
#  Pareto curve benchmark
#  Plots compression ratio vs throughput across all available
#  algorithm levels (ZSTD 1-19, Gzip 1-9, LZ4, Adaptive).
#  Provides a fair speed-matched comparison of all systems.
# ---------------------------------------------------------------
add_executable(benchmark_pareto benchmarks/benchmark_pareto.cpp)
target_link_libraries(benchmark_pareto PRIVATE adaptive_core)

# ---------------------------------------------------------------
#  Network benchmark (Linux / WSL2 only — requires POSIX sockets
#  and tc netem for network condition simulation)
#
#  Run via: sudo ../scripts/run_network_benchmark.sh
#  Measures end-to-end latency under three standardised network
#  conditions with 30 trials and 95% confidence intervals.
# ---------------------------------------------------------------
add_executable(network_benchmark benchmarks/network_benchmark.cpp)
target_link_libraries(network_benchmark PRIVATE adaptive_core)

# ---------------------------------------------------------------
#  Ablation study benchmark
#  Compares all preprocessing × algorithm combinations with
#  30-trial confidence intervals on Telemetry and Nibble data.
# ---------------------------------------------------------------
add_executable(benchmark_ablation benchmarks/benchmark_ablation.cpp)
target_link_libraries(benchmark_ablation PRIVATE adaptive_core)

# ---------------------------------------------------------------
#  Boundary telemetry benchmark
#  Evaluates engine routing and ratio cost at the smooth/noisy
#  boundary across five noise levels. Requires alternativeDecision()
#  in engine.cpp. Run AFTER threshold calibration is complete.
# ---------------------------------------------------------------
add_executable(benchmark_boundary benchmarks/benchmark_boundary.cpp)
target_link_libraries(benchmark_boundary PRIVATE adaptive_core)

foreach(target
    test_roundtrip
    benchmark_baseline benchmark_heuristic_single benchmark_routing_validation
    benchmark_pipeline_integration benchmark_preprocessing
    benchmark_streaming_stability benchmark_comparison benchmark_sensitivity
    benchmark_chunksize benchmark_pareto network_benchmark
    benchmark_ablation benchmark_boundary)
    add_dependencies(${target} make_results_dir)
endforeach()