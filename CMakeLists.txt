cmake_minimum_required(VERSION 3.16)
project(AdaptiveCompression CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(-O2 -Wall -Wextra -Wpedantic)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LZ4  REQUIRED liblz4)
pkg_check_modules(ZSTD REQUIRED libzstd)
find_package(ZLIB    REQUIRED)
find_package(Threads REQUIRED)

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

add_custom_target(make_results_dir ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/results
)

add_executable(test_roundtrip tests/test_roundtrip.cpp)
target_link_libraries(test_roundtrip PRIVATE adaptive_core)

add_executable(test_engine tests/test_engine.cpp)
target_link_libraries(test_engine PRIVATE adaptive_core)

add_executable(benchmark_baseline benchmarks/benchmark_baseline.cpp)
target_link_libraries(benchmark_baseline PRIVATE adaptive_core)

add_executable(benchmark_heuristic_single
    benchmarks/benchmark_heuristic_single.cpp)
target_link_libraries(benchmark_heuristic_single PRIVATE adaptive_core)

add_executable(benchmark_routing_validation
    benchmarks/benchmark_routing_validation.cpp)
target_link_libraries(benchmark_routing_validation PRIVATE adaptive_core)

add_executable(benchmark_pipeline_integration
    benchmarks/benchmark_pipeline_integration.cpp)
target_link_libraries(benchmark_pipeline_integration PRIVATE adaptive_core)

add_executable(benchmark_preprocessing
    benchmarks/benchmark_preprocessing.cpp)
target_link_libraries(benchmark_preprocessing PRIVATE adaptive_core)

add_executable(benchmark_streaming_stability
    benchmarks/benchmark_streaming_stability.cpp)
target_link_libraries(benchmark_streaming_stability PRIVATE adaptive_core)

add_executable(benchmark_comparison benchmarks/benchmark_comparison.cpp)
target_link_libraries(benchmark_comparison PRIVATE adaptive_core)

add_executable(benchmark_sensitivity benchmarks/benchmark_sensitivity.cpp)
target_link_libraries(benchmark_sensitivity PRIVATE adaptive_core)

add_executable(benchmark_chunksize benchmarks/benchmark_chunksize.cpp)
target_link_libraries(benchmark_chunksize PRIVATE adaptive_core)

add_executable(benchmark_pareto benchmarks/benchmark_pareto.cpp)
target_link_libraries(benchmark_pareto PRIVATE adaptive_core)

add_executable(benchmark_ablation benchmarks/benchmark_ablation.cpp)
target_link_libraries(benchmark_ablation PRIVATE adaptive_core)

add_executable(benchmark_boundary benchmarks/benchmark_boundary.cpp)
target_link_libraries(benchmark_boundary PRIVATE adaptive_core)

if(NOT WIN32)
    add_executable(network_benchmark benchmarks/network_benchmark.cpp)
    target_link_libraries(network_benchmark PRIVATE adaptive_core)

    add_executable(network_sender benchmarks/network_sender.cpp)
    target_link_libraries(network_sender PRIVATE adaptive_core)

    add_executable(network_receiver benchmarks/network_receiver.cpp)
    target_link_libraries(network_receiver PRIVATE adaptive_core)

    foreach(target network_benchmark network_sender network_receiver)
        add_dependencies(${target} make_results_dir)
    endforeach()
else()
    message(STATUS
        "Skipping network_benchmark, network_sender, network_receiver "
        "— POSIX sockets not available on native Windows. Use WSL2.")
endif()

foreach(target
    test_roundtrip
    test_engine
    benchmark_baseline
    benchmark_heuristic_single
    benchmark_routing_validation
    benchmark_pipeline_integration
    benchmark_preprocessing
    benchmark_streaming_stability
    benchmark_comparison
    benchmark_sensitivity
    benchmark_chunksize
    benchmark_pareto
    benchmark_ablation
    benchmark_boundary)
    add_dependencies(${target} make_results_dir)
endforeach()