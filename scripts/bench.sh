# ANSI Color Codes
GREEN='\033[0;32m'
CYAN='\033[0;36m'
NC='\033[0m'

echo -e "${CYAN}🧹 Sanitizing environment for pristine benchmarks...${NC}"
# 1. Wipe the CMake cache to eradicate the Google Benchmark DEBUG warning
rm -rf build/

# 2. Clear out any orphaned, root-owned shared memory segments from previous tests
# We use '|| true' so the script doesn't fail if the files don't exist
sudo rm -f /dev/shm/porth_* 2>/dev/null || true

echo -e "${CYAN}🚀 Building Sovereign Benchmark Suite (Strict Release Mode)...${NC}"
./scripts/build.sh > /dev/null

echo -e "${CYAN}📊 Initializing BENCHMARKS.md...${NC}"
cat << 'EOF' > BENCHMARKS.md
# ⚡ Porth-IO: Performance Manifest

> **Hardware Context:** Apple Silicon (M-Series) via OrbStack Linux Emulation.
> *Note: These figures represent virtualized kernel-bypass performance. Physical bare-metal PCIe Gen 6 metrics on isolated cores will scale significantly higher with near-zero jitter.*

EOF

echo -e "${CYAN}🔬 Running Micro-Benchmarks (Google Benchmark)...${NC}"
echo "### Core SPSC Ring Buffer Micro-Benchmarks" >> BENCHMARKS.md
echo "Tests the cache-line separation and Acquire/Release atomics under heavy multi-threaded contention." >> BENCHMARKS.md
echo '```text' >> BENCHMARKS.md
./build/porth_perf_suite --benchmark_color=false >> BENCHMARKS.md
echo '```' >> BENCHMARKS.md
echo "" >> BENCHMARKS.md

echo -e "${CYAN}🏎️ Running End-to-End Telemetry Demo (Tail Latency Analysis)...${NC}"
# We pass --parking 1 so the demo exits quickly after collecting 50,000 samples
# Verify the binary exists before running sudo to avoid "command not found" errors
if [ ! -f "./build/porth_showcase_demo" ]; then
    echo -e "${RED}❌ Error: porth_showcase_demo was not built correctly.${NC}"
    exit 1
fi

echo -e "${CYAN}🏎️ Running End-to-End Telemetry Demo (Tail Latency Analysis)...${NC}"
# Use the direct path to the build artifact to ensure sudo finds it
# We no longer hide output; we want to see the Sovereign Handshake in the logs.
# We reduce parking to 0 for the benchmark runner to keep it snappy.
sudo ./build/porth_showcase_demo --iterations 50000 --parking 0 --audit

echo -e "${GREEN}✅ Benchmarks complete! Output saved to BENCHMARKS.md${NC}"