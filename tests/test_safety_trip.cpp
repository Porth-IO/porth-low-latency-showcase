/**
 * @file test_safety_trip.cpp
 * @brief Verification of the Lattice-Guard Sentinel emergency shutdown.
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

TEST_CASE("Sovereign Watchdog: Thermal Emergency Halt", "[safety]") {
    using namespace porth;
    namespace fs = std::filesystem;

    // 1. Ultra-Robust Pathing: Hunt for the config file in common locations
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

    // 2. Setup the Hardware Simulation
    PorthSimDevice sim("safety_test_device");

    if (found) {
        sim.load_newport_profile(config_path);
    } else {
        std::cerr << "[Test] Warning: No PDK profile found. Using hardware defaults.\n";
    }

    auto* regs = sim.view();

    // 3. Initialize the Lattice-Guard Sentinel on Core 1
    porth::PorthSentinel sentinel(regs, 1);
    sentinel.start();

    // 4. Power on and Inject Heat
    regs->control.write(0x1);
    std::cout << "[Test] Injecting 46,000mC thermal spike...\n";
    regs->laser_temp.write(46000);

    // 5. Verification Loop with extended window for CI jitter
    bool tripped = false;
    for (int i = 0; i < 500; ++i) { // 1 second total window
        if (regs->safety_trip.load() == 0xDEADBEEF) {
            tripped = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    // 6. Verify success
    REQUIRE(tripped == true);
    REQUIRE(regs->safety_trip.load() == 0xDEADBEEF);

    std::cout << "[Success] Sentinel triggered 0xDEADBEEF trip.\n";

    sentinel.stop();
}

// NOLINTEND(cppcoreguidelines-avoid-do-while, bugprone-chained-comparison,
// cppcoreguidelines-avoid-goto)