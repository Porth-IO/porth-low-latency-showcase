#!/bin/bash
# 1. Format
find enterprise examples tests tools -name "*.cpp" -o -name "*.hpp" | xargs clang-format -i

# 2. Run Act
echo "🚀 Running Enterprise Audit..."
act -j high-fi-validation --bind --artifact-server-path /tmp/artifacts