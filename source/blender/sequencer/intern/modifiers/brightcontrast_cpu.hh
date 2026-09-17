/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>

/* Keep the containing translation unit at the normal CPU baseline. */
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__)) && !defined(_MSC_VER) && \
    !defined(FALCON_VSE_NO_AVX2)
#  include <immintrin.h>
#  define FALCON_VSE_LOCAL_AVX2 1
#else
#  define FALCON_VSE_LOCAL_AVX2 0
#endif

namespace blender::seq::brightcontrast_cpu {

enum class Mode { Auto, Reference, Portable, AVX2 };

inline Mode mode()
{
  static const Mode selected = [] {
    const char *value = std::getenv("FALCON_VSE_CPU_KERNEL");
    if (value && std::strcmp(value, "reference") == 0) {
      return Mode::Reference;
    }
    if (value && std::strcmp(value, "portable") == 0) {
      return Mode::Portable;
    }
    if (value && std::strcmp(value, "avx2") == 0) {
      return Mode::AVX2;
    }
    return Mode::Auto;
  }();
  return selected;
}

inline void make_byte_table(float mul, float add, unsigned char table[256])
{
  /* Exact evaluation of the existing byte load, affine transform and store.
   * No approximation: each of the 256 possible input values is evaluated once. */
  for (int i = 0; i < 256; i++) {
    const float value = (float(i) * (1.0f / 255.0f)) * mul + add;
    table[i] = static_cast<unsigned char>(
        255.0f * std::min(std::max(value, 0.0f), 1.0f) + 0.5f);
  }
}

inline void apply_bytes(unsigned char *pixels, size_t count, const unsigned char table[256])
{
  for (size_t i = 0; i < count; i++, pixels += 4) {
    pixels[0] = table[pixels[0]];
    pixels[1] = table[pixels[1]];
    pixels[2] = table[pixels[2]];
    /* Alpha is unchanged, including transparent pixels. */
  }
}

inline void apply_float_portable(float *pixels, size_t count, float mul, float add)
{
  for (size_t i = 0; i < count; i++, pixels += 4) {
    pixels[0] = pixels[0] * mul + add;
    pixels[1] = pixels[1] * mul + add;
    pixels[2] = pixels[2] * mul + add;
  }
}

#if FALCON_VSE_LOCAL_AVX2
__attribute__((target("avx2"), noinline)) inline void apply_float_avx2(
    float *pixels, size_t count, float mul, float add)
{
  const __m256 multiplier = _mm256_set1_ps(mul);
  const __m256 offset = _mm256_set1_ps(add);
  size_t i = 0;
  for (; i + 2 <= count; i += 2, pixels += 8) {
    const __m256 input = _mm256_loadu_ps(pixels);
    /* Do not use FMA: preserve the original multiply-then-add rounding. */
    const __m256 result = _mm256_add_ps(_mm256_mul_ps(input, multiplier), offset);
    _mm256_storeu_ps(pixels, _mm256_blend_ps(result, input, 0x88));
  }
  apply_float_portable(pixels, count - i, mul, add);
}
#endif

using FloatKernel = void (*)(float *, size_t, float, float);

inline bool avx2_available()
{
#if FALCON_VSE_LOCAL_AVX2
  return __builtin_cpu_supports("avx2");
#else
  return false;
#endif
}

/* The override never bypasses the CPU/OS capability check. */
inline FloatKernel select_float_kernel(Mode requested, bool available)
{
#if FALCON_VSE_LOCAL_AVX2
  if (available && requested == Mode::AVX2) {
    return apply_float_avx2;
  }
#else
  (void)requested;
  (void)available;
#endif
  return apply_float_portable;
}

inline FloatKernel float_kernel()
{
  static const FloatKernel kernel = select_float_kernel(mode(), avx2_available());
  return kernel;
}

}  // namespace blender::seq::brightcontrast_cpu
