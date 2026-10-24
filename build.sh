#!/usr/bin/env bash
# Сборка OrderBook через CMake + Clang.
# Использование: ./build.sh [Debug|Release]
set -euo pipefail

cd "$(dirname "$0")"

BUILD_TYPE="${1:-${BUILD_TYPE:-Debug}}"

cmake -S . -B build \
      -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
      -DCMAKE_CXX_COMPILER=clang++ \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

cmake --build build --parallel

echo
echo "Готово: ./build/orderbook"
echo "Запуск: ./build/orderbook -sym BTCUSDT -depth 10"
