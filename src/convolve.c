/*! @file convolve.c
 *  @brief Calculates the linear convolution of two signals.
 *  @author Markovtsev Vadim <v.markovtsev@samsung.com>
 *  @version 1.0
 *
 *  @section Notes
 *  This code partially conforms to <a href="http://google-styleguide.googlecode.com/svn/trunk/cppguide.xml">Google C++ Style Guide</a>.
 *
 *  @section Copyright
 *  Copyright © 2013 Samsung R&D Institute Russia
 *
 *  @section License
 *  Licensed to the Apache Software Foundation (ASF) under one
 *  or more contributor license agreements.  See the NOTICE file
 *  distributed with this work for additional information
 *  regarding copyright ownership.  The ASF licenses this file
 *  to you under the Apache License, Version 2.0 (the
 *  "License"); you may not use this file except in compliance
 *  with the License.  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing,
 *  software distributed under the License is distributed on an
 *  "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 *  KIND, either express or implied.  See the License for the
 *  specific language governing permissions and limitations
 *  under the License.
 */

#ifndef NO_FFTF
#define LIBSIMD_IMPLEMENTATION
#include "inc/simd/convolve.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <fftf/api.h>
#include "inc/simd/arithmetic.h"

void convolve_simd(int simd,
                   const float *__restrict x, size_t xLength,
                   const float *__restrict h, size_t hLength,
                   float *__restrict result) {
  assert(x);
  assert(h);
  assert(result);
  assert(xLength > 0);
  assert(hLength > 0);
  for (int n = 0; n < (int)(xLength + hLength - 1); n++) {
    float sum = 0.f;
    int beg = n < (int)xLength? 0 : n - xLength + 1;
    int end = n + 1;
    if (end > (int)hLength) {
      end = hLength;
    }
    if (simd) {
#ifdef __AVX__
      int simdEnd =  beg + ((end - beg) & ~7);
      __m256 accum = _mm256_setzero_ps();
      for (int m = beg; m < simdEnd; m += 8) {
        __m256 xvec = _mm256_loadu_ps(x + n - m - 7);
        __m256 hvec = _mm256_loadu_ps(h + m);
        xvec = _mm256_permute2f128_ps(xvec, xvec, 1);
        xvec = _mm256_permute_ps(xvec, 27);
        __m256 mulres = _mm256_mul_ps(xvec, hvec);
        accum = _mm256_add_ps(accum, mulres);
      }
      accum = _mm256_hadd_ps(accum, accum);
      accum = _mm256_hadd_ps(accum, accum);
      sum = _mm256_get_ps(accum, 0) + _mm256_get_ps(accum, 4);
      for (int m = simdEnd; m < end; m++) {
        sum += h[m] * x[n - m];
      }
    } else {
#elif defined(__ARM_NEON__)
      int simdEnd = beg + ((end - beg) & ~3);
      float32x4_t accum = vdupq_n_f32(0.f);
      for (int m = beg; m < simdEnd; m += 4) {
        float32x4_t xvec = vld1q_f32(x + n - m - 3);
        float32x4_t hvec = vld1q_f32(h + m);
        xvec = vrev64q_f32(xvec);
        xvec = vcombine_f32(vget_high_f32(xvec), vget_low_f32(xvec));
        accum = vmlaq_f32(accum, xvec, hvec);
      }
      float32x2_t accum2 = vpadd_f32(vget_high_f32(accum),
                                     vget_low_f32(accum));
      sum = vget_lane_f32(accum2, 0) + vget_lane_f32(accum2, 1);
      for (int m = simdEnd; m < end; m++) {
        sum += h[m] * x[n - m];
      }
    } else {
#else
    } {
#endif
      for (int m = beg; m < end; m++) {
        sum += h[m] * x[n - m];
      }
    }
   result[n] = sum;
 }
}

ConvolutionOverlapSaveHandle convolve_overlap_save_initialize(
    size_t xLength, size_t hLength) {
  assert(hLength < xLength / 2);
  assert(xLength > 0);
  assert(hLength > 0);

  ConvolutionOverlapSaveHandle handle;
  size_t M = hLength;  //  usual designation
  handle.x_length = xLength;
  handle.h_length = hLength;
  handle.reverse = 0;

  // Do zero padding of h to the next power of 2 + extra 2 float-s
  int L = M;
  int log = 2;
  while (L >>= 1) {
    log++;
  }
  L = (1 << log);
  handle.H = mallocf(L + 2);
  assert(handle.H);
  memsetf(handle.H + M, 0.f, L - M);
  handle.L = malloc(sizeof(L));
  *handle.L = L;

  handle.fft_boiler_plate = mallocf(L + 2);
  assert(handle.fft_boiler_plate);

  handle.fft_plan = fftf_init(FFTF_TYPE_REAL, FFTF_DIRECTION_FORWARD,
                              FFTF_DIMENSION_1D, handle.L,
                              FFTF_NO_OPTIONS, handle.fft_boiler_plate,
                              handle.fft_boiler_plate);

  assert(handle.fft_plan);

  handle.fft_inverse_plan = fftf_init(
      FFTF_TYPE_REAL, FFTF_DIRECTION_BACKWARD,
      FFTF_DIMENSION_1D, handle.L,
      FFTF_NO_OPTIONS, handle.fft_boiler_plate,
      handle.fft_boiler_plate);
  assert(handle.fft_inverse_plan);

  return handle;
}

void convolve_overlap_save_finalize(ConvolutionOverlapSaveHandle handle) {
  free(handle.fft_boiler_plate);
  fftf_destroy(handle.fft_plan);
  fftf_destroy(handle.fft_inverse_plan);
  free(handle.L);
  free(handle.H);
}

void convolve_overlap_save(ConvolutionOverlapSaveHandle handle,
                           const float *x,
                           const float *h,
                           float *result) {
  assert(x != NULL);
  assert(h != NULL);
  assert(result != NULL);

  size_t M = handle.h_length;  //  usual designation
  int L = *handle.L;

  if (handle.reverse) {
    rmemcpyf(handle.fft_boiler_plate, h, handle.h_length);
  } else {
    memcpy(handle.fft_boiler_plate, h, handle.h_length * sizeof(float));
  }
  memsetf(handle.fft_boiler_plate + handle.h_length, 0.f, L - handle.h_length);

  // H = FFT(paddedH, L)
  fftf_calc(handle.fft_plan);
  memcpy(handle.H, handle.fft_boiler_plate, (L + 2) * sizeof(float));

  int step = L - (M - 1);
  // Note: no "#pragma omp parallel for" here since
  // handle.fft_boiler_plate is shared AND FFTF should utilize all available resources.
  for (size_t i = 0; i < handle.x_length + M - 1; i += step) {
    // X = [zeros(1, M - 1), x, zeros(1, L-1)];
    // we must run FFT on X[i, i + L].
    // No X is really needed, some index arithmetic is used.
    if (i > 0) {
      if (i + step <= handle.x_length) {
        memcpy(handle.fft_boiler_plate, x + i - (M - 1), L * sizeof(float));
      } else {
        int cl = handle.x_length - i + M - 1;
        memcpy(handle.fft_boiler_plate, x + i - (M - 1),
               cl * sizeof(float));
        memsetf(handle.fft_boiler_plate + cl, 0.f, L - cl);
      }
    } else {
      memsetf(handle.fft_boiler_plate, 0.f, M - 1);
      memcpy(handle.fft_boiler_plate + M - 1, x, step * sizeof(float));
    }
    fftf_calc(handle.fft_plan);

    // fftBoilerPlate = fftBoilerPlate * H (complex arithmetic)
    int cciStart = 0;
#ifdef SIMD
    cciStart = L;
    for (int cci = 0; cci < L; cci += FLOAT_STEP) {
      complex_multiply(handle.fft_boiler_plate + cci, handle.H + cci,
                       handle.fft_boiler_plate + cci);
    }
#endif
    for (int cci = cciStart; cci < L + 2; cci += 2) {
      complex_multiply_na(handle.fft_boiler_plate + cci,
                          handle.H + cci,
                          handle.fft_boiler_plate + cci);
    }

    // Return back from the Fourier representation
    fftf_calc(handle.fft_inverse_plan);
    // Normalize
    real_multiply_scalar(handle.fft_boiler_plate + M - 1, step, 1.0f / L,
                         handle.fft_boiler_plate + M - 1);

    if (i + step < handle.x_length + handle.h_length) {
      memcpy(result + i, handle.fft_boiler_plate + M - 1,
             step * sizeof(float));
    } else {
      memcpy(result + i, handle.fft_boiler_plate + M - 1,
             (handle.x_length + handle.h_length - 1 - i) * sizeof(float));
    }
  }
}

ConvolutionFFTHandle convolve_fft_initialize(size_t xLength, size_t hLength) {
  assert(hLength > 0);
  assert(xLength > 0);

  ConvolutionFFTHandle handle;

  int M = xLength + hLength - 1;
  if ((M & (M - 1)) != 0) {
    int log = 1;
    while (M >>= 1) {
      log++;
    }
    M = (1 << log);
  }
  handle.M = malloc(sizeof(M));
  *handle.M = M;
  handle.x_length = xLength;
  handle.h_length = hLength;
  handle.reverse = 0;

  // Now M is the nearest greater than or equal power of 2.
  // Do zero padding of x and h
  // Allocate 2 extra samples for the M/2 complex number.
  float *X = mallocf(M + 2);
  memsetf(X + xLength, 0.f, M + 2 - xLength);
  float *H = mallocf(M + 2);
  memsetf(H + hLength, 0.f, M + 2 - hLength);

  handle.inputs = malloc(2 * sizeof(float *));
  handle.inputs[0] = X;
  handle.inputs[1] = H;

  // Prepare the forward FFT plan
  handle.fft_plan = fftf_init_batch(
      FFTF_TYPE_REAL, FFTF_DIRECTION_FORWARD,
      FFTF_DIMENSION_1D, handle.M,
      FFTF_NO_OPTIONS, 2, (const float *const *)handle.inputs,
      handle.inputs);
  assert(handle.fft_plan);

  // Prepare the inverse FFT plan
  handle.fft_inverse_plan = fftf_init(
    FFTF_TYPE_REAL, FFTF_DIRECTION_BACKWARD,
    FFTF_DIMENSION_1D, handle.M,
    FFTF_NO_OPTIONS, X, X);
  assert(handle.fft_inverse_plan);
  return handle;
}

void convolve_fft_finalize(ConvolutionFFTHandle handle) {
  free(handle.inputs[0]);
  free(handle.inputs[1]);
  free(handle.inputs);
  free(handle.M);
  fftf_destroy(handle.fft_plan);
  fftf_destroy(handle.fft_inverse_plan);
}

void convolve_fft(ConvolutionFFTHandle handle,
                  const float *x, const float *h,
                  float *result) {
  assert(x != NULL);
  assert(h != NULL);
  assert(result != NULL);

  float *X = handle.inputs[0];
  float *H = handle.inputs[1];
  int xLength = handle.x_length;
  int hLength = handle.h_length;
  int M = *handle.M;
  memcpy(X, x, xLength * sizeof(x[0]));
  if (handle.reverse) {
    rmemcpyf(H, h, hLength);
  } else {
    memcpy(H, h, hLength * sizeof(h[0]));
  }

  // fft(X), fft(H)
  fftf_calc(handle.fft_plan);

  int istart = 0;
#ifdef SIMD
  istart = M;
  for (int i = 0; i < M; i += FLOAT_STEP) {
    complex_multiply(X + i, H + i, X + i);
  }
#endif
  for (int i = istart; i < M + 2; i += 2) {
    complex_multiply_na(X + i, H + i, X + i);
  }

  // Return back from the Fourier representation
  fftf_calc(handle.fft_inverse_plan);
  // Normalize
  real_multiply_scalar(X, xLength + hLength - 1, 1.0f / M, result);
}

ConvolutionHandle convolve_initialize(size_t xLength, size_t hLength) {
  ConvolutionHandle handle;
  handle.x_length = xLength;
  handle.h_length = hLength;
#ifdef __ARM_NEON__
  if (xLength > hLength * 2) {
    if (xLength > 200) {
      handle.algorithm = kConvolutionAlgorithmOverlapSave;
      handle.handle.os = convolve_overlap_save_initialize(xLength, hLength);
    } else {
      handle.algorithm = kConvolutionAlgorithmBruteForce;
    }
  } else {
    if (xLength > 50) {
      handle.algorithm = kConvolutionAlgorithmFFT;
      handle.handle.fft = convolve_fft_initialize(xLength, hLength);
    } else {
      handle.algorithm = kConvolutionAlgorithmBruteForce;
    }
  }
#else
  if (xLength > hLength * 2) {
    if (xLength > 200) {
      handle.algorithm = kConvolutionAlgorithmOverlapSave;
      handle.handle.os = convolve_overlap_save_initialize(xLength, hLength);
    } else {
      handle.algorithm = kConvolutionAlgorithmBruteForce;
    }
  } else {
    if (xLength > 350) {
      handle.algorithm = kConvolutionAlgorithmFFT;
      handle.handle.fft = convolve_fft_initialize(xLength, hLength);
    } else {
      handle.algorithm = kConvolutionAlgorithmBruteForce;
    }
  }
#endif
  return handle;
}

void convolve_finalize(ConvolutionHandle handle) {
  switch (handle.algorithm) {
    case kConvolutionAlgorithmFFT:
      convolve_fft_finalize(handle.handle.fft);
      break;
    case kConvolutionAlgorithmOverlapSave:
      convolve_overlap_save_finalize(handle.handle.os);
      break;
    case kConvolutionAlgorithmBruteForce:
      break;
  }
}

void convolve(ConvolutionHandle handle,
              const float *__restrict x, const float *__restrict h,
              float *__restrict result) {
  switch (handle.algorithm) {
    case kConvolutionAlgorithmFFT:
      convolve_fft(handle.handle.fft, x, h, result);
      break;
    case kConvolutionAlgorithmOverlapSave:
      convolve_overlap_save(handle.handle.os, x, h, result);
      break;
    case kConvolutionAlgorithmBruteForce:
      convolve_simd(1, x, handle.x_length, h, handle.h_length, result);
      break;
  }
}

#endif  // #ifndef NO_FFTF
