# Porth-IO: Integration Examples
# ---------------------------------------------------------

# Helper function for examples
function(add_porth_example name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE Porth::porth-core)
endfunction()

add_porth_example(std_baseline std_baseline.cpp)
add_porth_example(porth_full_demo porth_full_demo.cpp)
add_porth_example(zero_copy_handshake zero_copy_handshake.cpp)
add_porth_example(calibrate_clock calibrate_clock.cpp)
add_porth_example(mock_handshake mock_handshake.cpp)
add_porth_example(network_portal_demo network_portal_demo.cpp)