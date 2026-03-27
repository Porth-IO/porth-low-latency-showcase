#!/bin/bash
# @file build.sh
# @brief High-Performance SDK Build Orchestrator.
#
# Porth-IO: Sovereign Logic Layer
# Copyright (c) 2026 Porth-IO Contributors

# Exit on any error
set -e

# ANSI Color Codes
RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

echo -e "${BLUE}${BOLD}--- Porth-IO: Low-Latency Showcase Build Orchestrator ---${NC}"

# 1. Navigate to Project Root
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
cd "$PROJECT_ROOT"

# 2. Prepare Build Environment
BUILD_DIR="build"
if [ ! -d "$BUILD_DIR" ]; then
    echo -e "${CYAN}[1/3] Creating build infrastructure...${NC}"
    mkdir -p "$BUILD_DIR"
fi
cd "$BUILD_DIR"

# 3. Configure CMake (Release Mode)
echo -e "${CYAN}[2/3] Configuring Sovereign SDK (Release/C++23)...${NC}"
cmake -DCMAKE_BUILD_TYPE=Release ..

# 4. Compile Performance Suite and Demos
echo -e "${CYAN}[3/3] Compiling Deterministic Logic and Emulated Hardware...${NC}"
CPU_CORES=$(nproc 2>/dev/null || sysctl -n hw.ncpu || echo 1)
make -j"$CPU_CORES"

# 5. Create Showcase Symlinks
echo -e "${CYAN}Finalizing Root-Level access...${NC}"
cd "$PROJECT_ROOT"

# Symlink the primary demo to the root
ln -sf build/porth_showcase_demo porth_showcase_demo

# 6. Success Summary
echo -e "\n${GREEN}${BOLD}--- Build Immaculate ---${NC}"
echo -e "${BOLD}Showcase Assets Ready.${NC}"
echo -e "${BOLD}Launch Demo via:${NC} ${CYAN}./porth_showcase_demo${NC}"
echo -e "${BOLD}Run Benchmarks via:${NC} ${CYAN}./build/porth_perf_suite${NC}"

# List targets
echo -e "\n${CYAN}Compiled Showcase Targets:${NC}"
ls -F build/ | grep "*" | sort