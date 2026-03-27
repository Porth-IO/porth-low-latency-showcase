# Porth-IO: Functional & Physics Tests
# ---------------------------------------------------------

# Helper to avoid repetitive linking logic for standard tests
function(add_porth_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE 
        porth-core 
        Catch2::Catch2WithMain
    )
    
    # Register the test with CTest
    add_test(NAME ${name} COMMAND ${name})
    
    # THE 1% FIX: Label the test so CI can find it with '-L Enterprise'
    set_tests_properties(${name} PROPERTIES LABELS "Enterprise")
endfunction()

# Physics & Digital Twin Tests (Catch2-based)
add_porth_test(test_sim_chaos test_sim_chaos.cpp)
add_porth_test(test_safety_trip test_safety_trip.cpp)