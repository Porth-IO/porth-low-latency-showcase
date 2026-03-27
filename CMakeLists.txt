cmake_minimum_required(VERSION 3.28)
project(porth-low-latency-showcase VERSION 1.0.0 LANGUAGES C CXX)

# Core Build Configuration
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Sovereign Logic Layer Definitions
add_compile_definitions(PORTH_WITH_ENTERPRISE=1)
add_compile_definitions(PORTH_CONFIG_DIR="${CMAKE_CURRENT_SOURCE_DIR}/configs")

# Performance Tuning: Stabilize Cache Line Size Warnings
add_compile_options(-Wno-interference-size)

# --- Dependency Management (Automated) ---
include(FetchContent)

FetchContent_Declare(
    json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_Declare(
    benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG        v1.8.3
)
FetchContent_Declare(
    catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.5.2
)

# Disable internal tests for dependencies to avoid GTest/Self-test overhead
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(CATCH_INSTALL_HELPERS OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(json benchmark catch2)

# Header Search Paths
include_directories(
    include/
    enterprise/
)

# System Dependency Discovery (Kernel Bypass & Memory Ops)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PCI REQUIRED libpci)
pkg_check_modules(NUMA REQUIRED numa)
pkg_check_modules(XDP REQUIRED libxdp)
pkg_check_modules(BPF REQUIRED libbpf)

find_package(benchmark CONFIG REQUIRED)
find_package(Catch2 CONFIG REQUIRED)

# 1. Primary Showcase Demo
add_executable(porth_showcase_demo examples/main_demo.cpp)
target_link_libraries(porth_showcase_demo 
    PRIVATE 
        ${PCI_LIBRARIES}
        pci
        ${NUMA_LIBRARIES}
        ${XDP_LIBRARIES}
        ${BPF_LIBRARIES}
        nlohmann_json::nlohmann_json
)
target_include_directories(porth_showcase_demo PRIVATE 
    ${PCI_INCLUDE_DIRS} 
    ${NUMA_INCLUDE_DIRS} 
    ${XDP_INCLUDE_DIRS} 
    ${BPF_INCLUDE_DIRS}
)

# 2. Performance Verification Suite
add_executable(porth_perf_suite benchmarks/porth_perf_suite.cpp)
target_link_libraries(porth_perf_suite 
    PRIVATE 
        benchmark::benchmark 
        benchmark::benchmark_main
)

# 3. High-Fidelity Resilience Tests
enable_testing()

# Sentinel Safety Trip Verification
add_executable(test_safety_trip tests/test_safety_trip.cpp)
target_link_libraries(test_safety_trip 
    PRIVATE 
        Catch2::Catch2WithMain
        ${NUMA_LIBRARIES}
        nlohmann_json::nlohmann_json
)
add_test(NAME test_safety_trip COMMAND test_safety_trip)

# Chaos Engineering & Register Corruption Recovery
add_executable(test_sim_chaos tests/test_sim_chaos.cpp)
target_link_libraries(test_sim_chaos 
    PRIVATE 
        Catch2::Catch2WithMain
        ${NUMA_LIBRARIES}
        nlohmann_json::nlohmann_json
)
add_test(NAME test_sim_chaos COMMAND test_sim_chaos)

# Build Status Summary
message(STATUS "----------------------------------------------------")
message(STATUS "Porth-IO: Low-Latency Showcase Configured")
message(STATUS "Platform:       Sovereign Logic Layer (InP/GaN)")
message(STATUS "Precision:      Sub-Nanosecond TSC Timing")
message(STATUS "Dependencies:   nlohmann_json (Auto-Fetched), libnuma, libxdp")
message(STATUS "----------------------------------------------------")