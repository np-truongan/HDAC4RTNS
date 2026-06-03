cmake_minimum_required(VERSION 3.16)
project(AdaptiveCompression CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(-O2 -Wall -Wextra -Wpedantic)

# ---------------------------------------------------------------
#  Find required libraries
# ---------------------------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(LZ4   REQUIRED liblz4)
pkg_check_modules(ZSTD  REQUIRED libzstd)
find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------
#  Core library
# ---------------------------------------------------------------
add_library(adaptive_core STATIC
    src/generators.cpp
    src/heuristics.cpp
    src/strategies.cpp
    src/engine.cpp
    src/pipeline.cpp
    src/framing.cpp
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
#  Output directory for results
# ---------------------------------------------------------------
add_custom_target(make_results_dir ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory
    ${CMAKE_BINARY_DIR}/results
)

# ---------------------------------------------------------------
#  Week 1: Baseline benchmark
# ---------------------------------------------------------------
add_executable(benchmark_baseline benchmarks/baseline.cpp)
target_link_libraries(benchmark_baseline PRIVATE adaptive_core)

# ---------------------------------------------------------------
#  Week 2
# ---------------------------------------------------------------
add_executable(benchmark_adaptive_week2 benchmarks/adaptive_week2.cpp)
target_link_libraries(benchmark_adaptive_week2 PRIVATE adaptive_core)

add_executable(test_roundtrip tests/test_roundtrip.cpp)
target_link_libraries(test_roundtrip PRIVATE adaptive_core)

# ---------------------------------------------------------------
#  Week 3
# ---------------------------------------------------------------
add_executable(benchmark_adaptive_week3 benchmarks/adaptive_week3.cpp)
target_link_libraries(benchmark_adaptive_week3 PRIVATE adaptive_core)

# ---------------------------------------------------------------
#  Week 4
# ---------------------------------------------------------------
add_executable(benchmark_adaptive_week4 benchmarks/adaptive_week4.cpp)
target_link_libraries(benchmark_adaptive_week4 PRIVATE adaptive_core)

# ---------------------------------------------------------------
#  Week 5
# ---------------------------------------------------------------
add_executable(benchmark_adaptive_week5 benchmarks/adaptive_week5.cpp)
target_link_libraries(benchmark_adaptive_week5 PRIVATE adaptive_core)

# ---------------------------------------------------------------
#  Week 6
# ---------------------------------------------------------------
add_executable(benchmark_adaptive_week6 benchmarks/adaptive_week6.cpp)
target_link_libraries(benchmark_adaptive_week6 PRIVATE adaptive_core)

# ---------------------------------------------------------------
#  Week 7
# ---------------------------------------------------------------
add_executable(benchmark_week7 benchmarks/benchmark_week7.cpp)
target_link_libraries(benchmark_week7 PRIVATE adaptive_core)

add_executable(benchmark_sensitivity benchmarks/benchmark_sensitivity.cpp)
target_link_libraries(benchmark_sensitivity PRIVATE adaptive_core)

# ---------------------------------------------------------------
#  Network benchmark (Linux/WSL2 only — requires POSIX sockets)
# ---------------------------------------------------------------
add_executable(network_benchmark benchmarks/network_benchmark.cpp)
target_link_libraries(network_benchmark PRIVATE adaptive_core)