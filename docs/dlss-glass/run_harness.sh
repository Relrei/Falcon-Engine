#!/bin/bash
# Falcon: ガラス越しガイドの CPU 検証ハーネスを建てて走らせる。
# 使い方: docs/dlss-glass/run_harness.sh   (リポジトリのどこからでも)
# 要るもの: clang++(または CXX=g++)。x86-64 の SSE4.2。Blender 本体のビルドは要らない。
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT/docs/dlss-glass"
OUT="${TMPDIR:-/tmp}/falcon_glass_guides_harness"
cd "$ROOT"
"${CXX:-clang++}" -std=c++20 -O0 -w -g -msse4.2 "$HERE/glass_guides_harness.cpp" -o "$OUT" \
  -DWITH_FALCON_SHARC "-DCCL_NAMESPACE_BEGIN=namespace ccl {" "-DCCL_NAMESPACE_END=}" \
  -iquote intern/cycles -iquote intern/cycles/kernel -iquote intern/atomic -iquote intern \
  -iquote intern/cycles/kernel/device/cpu -isystem "$HERE/stub"
"$OUT"
