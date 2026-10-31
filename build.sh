#!/usr/bin/env bash
# Build helper:  ./build.sh        -> real build for the board (needs the RKLLM SDK + libsqlite3-dev)
#                ./build.sh mock   -> UI preview build (fake model, any Linux PC)
set -euo pipefail
cd "$(dirname "$0")"

RKLLM_ROOT="${RKLLM_ROOT:-$HOME/llm/rknn-llm/rkllm-runtime/Linux/librkllm_api}"

if [[ "${1:-}" == "mock" ]]; then
  cmake -S . -B build-mock -DCMAKE_BUILD_TYPE=Release -DRKLLM_MOCK=ON
  cmake --build build-mock -j"$(nproc)"
  echo; echo "Built: $(pwd)/build-mock/rkllm_console, rkllm_web, rag_tool"
else
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DRKLLM_ROOT="$RKLLM_ROOT"
  cmake --build build -j"$(nproc)"
  echo; echo "Built: $(pwd)/build/rkllm_console, rkllm_web, rag_tool"
fi
