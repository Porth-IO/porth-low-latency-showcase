# Porth-IO: Performance Benchmarks
# ---------------------------------------------------------

# 1. Check if we already have benchmark targets (from FetchContent)
# 2. If not, try to find them on the system (vcpkg/apt/etc)
if(NOT TARGET benchmark::benchmark)
    find_package(benchmark CONFIG REQUIRED)
endif()

# Helper function for benchmarks
function(add_porth_benchmark name source)
    add_executable(${name} ${source})
    
    # Link against porth-core and the Google Benchmark libraries
    # benchmark::benchmark_main is often included in the FetchContent build
    target_link_libraries(${name} PRIVATE 
        Porth::porth-core 
        benchmark::benchmark 
        benchmark::benchmark_main
    )
    
    # Apply your project standard warning flags here too
    if(PORTH_WARNING_FLAGS)
        target_compile_options(${name} PRIVATE ${PORTH_WARNING_FLAGS})
    endif()
endfunction()

# Define your benchmark executables
add_porth_benchmark(porth_bench porth_bench.cpp)
add_porth_benchmark(porth_vs_std porth_vs_std.cpp)
add_porth_benchmark(porth_perf_suite porth_perf_suite.cpp)