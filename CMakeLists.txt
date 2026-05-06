cmake_minimum_required(VERSION 3.16)
project(AdaptiveCompression CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------
#  Compiler flags
# ---------------------------------------------------------------
add_compile_options(-O2 -Wall -Wextra -Wpedantic)

# ---------------------------------------------------------------
#  Find required libraries
# ---------------------------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(LZ4   REQUIRED liblz4)
pkg_check_modules(ZSTD  REQUIRED libzstd)
find_package(ZLIB REQUIRED)

# ---------------------------------------------------------------
#  Core library — all reusable components
#  Every benchmark and test links against this.
# ---------------------------------------------------------------
add_library(adaptive_core STATIC
    src/generators.cpp
    src/heuristics.cpp
    src/strategies.cpp
    src/engine.cpp
    src/pipeline.cpp
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
)

# ---------------------------------------------------------------
#  Week 1: Baseline benchmark
# ---------------------------------------------------------------
add_executable(benchmark_baseline benchmarks/baseline.cpp)
target_link_libraries(benchmark_baseline PRIVATE adaptive_core)

# ---------------------------------------------------------------
#  Week 2: First adaptive benchmark + correctness tests
# ---------------------------------------------------------------
add_executable(benchmark_adaptive_week2 benchmarks/adaptive_week2.cpp)
target_link_libraries(benchmark_adaptive_week2 PRIVATE adaptive_core)

add_executable(test_roundtrip tests/test_roundtrip.cpp)
target_link_libraries(test_roundtrip PRIVATE adaptive_core)

# ---------------------------------------------------------------
#  Week 3: Multi-dataset adaptive engine evaluation
# ---------------------------------------------------------------
add_executable(benchmark_adaptive_week3 benchmarks/adaptive_week3.cpp)
target_link_libraries(benchmark_adaptive_week3 PRIVATE adaptive_core)

# ---------------------------------------------------------------
#  Week 4: Streaming pipeline integration benchmark
# ---------------------------------------------------------------
add_executable(benchmark_adaptive_week4 benchmarks/adaptive_week4.cpp)
target_link_libraries(benchmark_adaptive_week4 PRIVATE adaptive_core)

# ---------------------------------------------------------------
#  Placeholder targets for subsequent weeks
#  (uncomment as files are added)
# ---------------------------------------------------------------
# add_executable(benchmark_sensitivity benchmarks/sensitivity.cpp)
# target_link_libraries(benchmark_sensitivity PRIVATE adaptive_core)

# ---------------------------------------------------------------
#  Output directory for results
# ---------------------------------------------------------------
add_custom_target(make_results_dir ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory
    ${CMAKE_BINARY_DIR}/results
)

# ---------------------------------------------------------------
#  Week 5: BitPack + preprocessing-aware pipeline
# ---------------------------------------------------------------
add_executable(benchmark_adaptive_week5 benchmarks/adaptive_week5.cpp)
target_link_libraries(benchmark_adaptive_week5 PRIVATE adaptive_core)