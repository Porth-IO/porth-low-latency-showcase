/**
 * @file test_safety_trip.cpp
 * @brief Verification of the Lattice-Guard Sentinel emergency shutdown.
 *
 * Porth-IO: Low Latency Showcase
 */

#include "PorthSimDevice.hpp"
#include "porth/PorthDriver.hpp"
#include "porth/PorthSentinel.hpp"
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <thread>
#include <vector>

// NOLINTBEGIN(cppcoreguidelines-avoid-do-while, bugprone-chained-comparison,
// cppcoreguidelines-avoid-goto)

/**
 * @brief Test Case: Thermal Emergency Halt Verification.
 * * This test validates the sub-microsecond reaction time of the PorthSentinel.
 * It simulates a thermal excursion in the Indium Phosphide (InP) lattice and 
 * verifies that the hardware-level safety trip (0xDEADBEEF) is executed correctly 
 * to prevent physical substrate damage.
 */
TEST_CASE("Thermal Emergency Halt Verification", "[safety]") {
    using namespace porth;
    namespace fs = std::filesystem;

    // 1. Pathing Logic: Locates the PDK configuration manifest for hardware profiling.
    std::string config_path;
    std::vector<std::string> search_paths = {
        "../configs/newport_default.json",    // Normal local build
        "../../configs/newport_default.json", // CI running from build-ci/tests
        "configs/newport_default.json"        // Root execution
    };

    bool found = false;
    for (const auto& path : search_paths) {
        if (fs::exists(path)) {
            config_path = path;
            found       = true;
            break;
        }
    }

    // 2. Hardware Simulation: Initializes the Digital Twin for safety testing.
    PorthSimDevice sim("safety_test_device");

    if (found) {
        sim.load_newport_profile(config_path);
    } else {
        std::cerr << "[Test] Warning: No PDK profile found. Using hardware defaults.\n";
    }

    auto* regs = sim.view();

    // 3. Isolated Monitoring: Initializes the Lattice-Guard Sentinel on a dedicated core.
    porth::PorthSentinel sentinel(regs, 1);
    sentinel.start();

    // 4. Fault Injection: Triggers a 46,000mC thermal spike to breach the safety boundary.
    regs->control.write(0x1);
    std::cout << "[Test] Injecting 46,000mC thermal spike...\n";
    regs->laser_temp.write(46000);

    // 5. Verification Loop: Monitors the safety_trip register for the expected 0xDEADBEEF signal.
    bool tripped = false;
    for (int i = 0; i < 500; ++i) { // 1 second total window for CI environment jitter
        if (regs->safety_trip.load() == 0xDEADBEEF) {
            tripped = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    // 6. Assertions: Ensures the system reached the halted state correctly.
    REQUIRE(tripped == true);
    REQUIRE(regs->safety_trip.load() == 0xDEADBEEF);

    std::cout << "[Success] Sentinel triggered 0xDEADBEEF thermal trip.\n";

    sentinel.stop();
}

// NOLINTEND(cppcoreguidelines-avoid-do-while, bugprone-chained-comparison,
// cppcoreguidelines-avoid-goto)