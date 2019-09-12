// Copyright (c) 2019 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "lite/tests/utils/fill_data.h"
#include "lite/tests/utils/test_funcs.h"
#ifdef LITE_WITH_ARM
#include "lite/backends/arm/math/funcs.h"
#endif
#include "lite/core/context.h"
#include "lite/core/tensor.h"
#include "lite/tests/utils/tensor_utils.h"
#include "lite/tests/utils/test_lite.h"

typedef paddle::lite::Tensor Tensor;
typedef lite::test::TestLite TestLite;

int g_cluster = 0;
int g_threads = 1;

bool g_basic_test = false;

int g_M = 512;
int g_N = 512;
int g_K = 512;
bool g_traA = false;
bool g_traB = false;
bool g_flag_relu = false;
bool g_flag_bias = false;
int g_test_iter = 1;
int g_warmup_iter = 0;
bool g_compare_result = true;

bool test_gemm_int8(bool tra,
                    bool trb,
                    int m,
                    int n,
                    int k,
                    bool has_bias,
                    bool has_relu,
                    int cls,
                    int ths) {
  Tensor ta;
  Tensor tb;
  Tensor tc_int8;
  Tensor tc_fp32;
  Tensor tc_basic_int8;
  Tensor tc_basic_fp32;
  Tensor tbias;

  int lda = tra ? m : k;
  int ldb = trb ? k : n;
  int ldc = n;

  ta.Resize({m, k});
  tb.Resize({k, n});
  tc_int8.Resize({m, n});
  tc_fp32.Resize({m, n});
  tc_basic_int8.Resize({m, n});
  tc_basic_fp32.Resize({m, n});
  tbias.Resize({m});

  ta.set_precision(PRECISION(kInt8));
  tb.set_precision(PRECISION(kInt8));
  tc_int8.set_precision(PRECISION(kInt8));
  tc_fp32.set_precision(PRECISION(kFloat));
  tc_basic_int8.set_precision(PRECISION(kInt8));
  tc_basic_fp32.set_precision(PRECISION(kFloat));
  tbias.set_precision(PRECISION(kFloat));

  fill_tensor_rand(ta, -127, 127);
  fill_tensor_rand(tb, -127, 127);
  fill_tensor_rand(tbias, -1.f, 1.f);

  std::vector<float> scale_a(static_cast<size_t>(m), 1.f / 127);
  std::vector<float> scale_b = {1.f / 127};
  std::vector<float> scale_c = {k / 127.f};
  std::vector<float> scale_merge_fp32(static_cast<size_t>(m));
  std::vector<float> scale_merge_int8(static_cast<size_t>(m));
  for (int j = 0; j < m; ++j) {
    scale_merge_fp32[j] = scale_a[j] * scale_b[0];
    scale_merge_int8[j] = scale_merge_fp32[j] / scale_c[0];
  }

  auto da = ta.mutable_data<int8_t>();
  auto db = tb.mutable_data<int8_t>();
  auto dc_int8 = tc_int8.mutable_data<int8_t>();
  auto dc_fp32 = tc_fp32.mutable_data<float>();
  auto dc_basic_int8 = tc_basic_int8.mutable_data<int8_t>();
  auto dc_basic_fp32 = tc_basic_fp32.mutable_data<float>();
  auto dbias = tbias.mutable_data<float>();

  LOG(INFO) << "gemm_int8 M: " << m << ", N: " << n << ", K: " << k
            << ", transA: " << (tra ? "true" : "false")
            << ", transB: " << (trb ? "true" : "false")
            << ", relu: " << (has_relu ? "true" : "false")
            << ", bias: " << (has_bias ? "true" : "false");
  if (g_compare_result) {
    Tensor ta_fp32;
    Tensor tb_fp32;
    ta_fp32.Resize({m, k});
    ta_fp32.set_precision(PRECISION(kFloat));
    tb_fp32.Resize({k, n});
    tb_fp32.set_precision(PRECISION(kFloat));

    auto da_fp32 = ta_fp32.mutable_data<float>();
    auto db_fp32 = tb_fp32.mutable_data<float>();

    paddle::lite::arm::math::int8_to_fp32(
        da, da_fp32, scale_a.data(), 1, 1, ta.numel());
    paddle::lite::arm::math::int8_to_fp32(
        db, db_fp32, scale_b.data(), 1, 1, tb.numel());
    basic_gemm(tra,
               trb,
               m,
               n,
               k,
               1.f,
               da_fp32,
               lda,
               db_fp32,
               ldb,
               0.f,
               dc_basic_fp32,
               ldc,
               dbias,
               has_bias,
               has_relu);
    paddle::lite::arm::math::fp32_to_int8(dc_basic_fp32,
                                          dc_basic_int8,
                                          scale_c.data(),
                                          1,
                                          1,
                                          tc_basic_fp32.numel());
  }
  lite::test::Timer t0;
#ifdef LITE_WITH_ARM
  //! compute
  double ops = 2.0 * m * n * k;
  std::unique_ptr<paddle::lite::KernelContext> ctx1(
      new paddle::lite::KernelContext);
  auto& ctx = ctx1->As<paddle::lite::ARMContext>();
  ctx.SetRunMode(static_cast<paddle::lite_api::PowerMode>(cls), ths);
  //! prepack
  Tensor tpackedA;
  int hblock = paddle::lite::arm::math::get_hblock_int8(&ctx);
  int round_up_a = ((hblock + m - 1) / hblock) * hblock;
  tpackedA.Resize({round_up_a * k});
  paddle::lite::arm::math::prepackA_int8(
      tpackedA.mutable_data<int8_t>(), da, lda, 0, m, 0, k, tra, &ctx);
  /// warmup
  for (int j = 0; j < g_warmup_iter; ++j) {
    paddle::lite::arm::math::gemm_prepack_int8(tpackedA.data<int8_t>(),
                                               db,
                                               dbias,
                                               dc_fp32,
                                               m,
                                               n,
                                               k,
                                               has_bias,
                                               has_relu,
                                               trb,
                                               scale_merge_fp32.data(),
                                               &ctx);
  }

  /// int8 output compute
  Tensor tbias_int8;
  tbias_int8.Resize(tbias.dims());
  tbias_int8.set_precision(PRECISION(kFloat));
  auto dbias_int8 = tbias_int8.mutable_data<float>();
  for (int l = 0; l < tbias_int8.numel(); ++l) {
    dbias_int8[l] = dbias[l] / scale_c[0];
  }
  for (int i = 0; i < g_test_iter; ++i) {
    t0.start();
    paddle::lite::arm::math::gemm_prepack_int8(tpackedA.data<int8_t>(),
                                               db,
                                               dbias_int8,
                                               dc_int8,
                                               m,
                                               n,
                                               k,
                                               has_bias,
                                               has_relu,
                                               trb,
                                               scale_merge_int8.data(),
                                               &ctx);
    t0.end();
  }
  LOG(INFO) << "gemm_int8_int8 output: M: " << m << ", N: " << n << ", K: " << k
            << ", cluster: " << cls << ", threads: " << ths
            << ", GOPS: " << ops * 1e-9f
            << " GOPS, avg time: " << t0.get_average_ms()
            << " ms, min time: " << t0.get_min_time()
            << " ms, mean GOPs: " << ops * 1e-6f / t0.get_average_ms()
            << " GOPs, max GOPs: " << ops * 1e-6f / t0.get_min_time()
            << " GOPs";

  /// fp32 output compute
  t0.clear();
  for (int i = 0; i < g_test_iter; ++i) {
    t0.start();
    paddle::lite::arm::math::gemm_prepack_int8(tpackedA.data<int8_t>(),
                                               db,
                                               dbias,
                                               dc_fp32,
                                               m,
                                               n,
                                               k,
                                               has_bias,
                                               has_relu,
                                               trb,
                                               scale_merge_fp32.data(),
                                               &ctx);
    t0.end();
  }
  LOG(INFO) << "gemm_int8_fp32 output: M: " << m << ", N: " << n << ", K: " << k
            << ", cluster: " << cls << ", threads: " << ths
            << ", GOPS: " << ops * 1e-9f
            << " GOPS, avg time: " << t0.get_average_ms()
            << " ms, min time: " << t0.get_min_time()
            << " ms, mean GOPs: " << ops * 1e-6f / t0.get_average_ms()
            << " GOPs, max GOPs: " << ops * 1e-6f / t0.get_min_time()
            << " GOPs";

  if (g_compare_result) {
    double max_ratio = 0;
    double max_diff = 0;
    /// fp32 result
    tensor_cmp_host(tc_basic_fp32, tc_fp32, max_ratio, max_diff);
    LOG(INFO) << "fp32 compare result, max diff: " << max_diff
              << ", max ratio: " << max_ratio;
    if (std::abs(max_ratio) > 1e-4f && std::abs(max_diff) > 5e-5f) {
      Tensor tdiff;
      tdiff.set_precision(PRECISION(kFloat));
      tdiff.Resize(tc_fp32.dims());
      tensor_diff(tc_basic_fp32, tc_fp32, tdiff);
      LOG(INFO) << "basic result: ";
      print_tensor(tc_basic_fp32);
      LOG(INFO) << "saber result: ";
      print_tensor(tc_fp32);
      LOG(INFO) << "diff result: ";
      print_tensor(tdiff);
      return false;
    }
    /// int8 result
    max_ratio = 0;
    max_diff = 0;
    tensor_cmp_host(tc_basic_int8, tc_int8, max_ratio, max_diff);
    LOG(INFO) << "int8 compare result, max diff: " << max_diff
              << ", max ratio: " << max_ratio;
    if (fabs(max_ratio) > 1e-4f) {
      Tensor tdiff;
      tdiff.Resize(tc_int8.dims());
      tdiff.set_precision(PRECISION(kInt8));
      tensor_diff(tc_basic_int8, tc_int8, tdiff);
      auto ptr = tdiff.data<int8_t>();
      auto ptr_basic_fp32 = tc_basic_fp32.data<float>();
      float count = 0;
      bool check = true;
      for (int i = 0; i < tdiff.numel(); ++i) {
        if (abs(ptr[i]) > 1) {
          check = false;
          LOG(ERROR) << "basic float data: " << ptr_basic_fp32[i]
                     << ", after scale: " << ptr_basic_fp32[i] / scale_c[0];
          break;
        }
        if (ptr[i] != 0) {
          LOG(ERROR) << "basic float data: " << ptr_basic_fp32[i]
                     << ", after scale: " << ptr_basic_fp32[i] / scale_c[0];
          count += 1;
        }
      }
      check =
          check && count < std::max(10, static_cast<int>(0.01 * tdiff.numel()));
      if (!check) {
        LOG(WARNING) << "int8 basic result";
        print_tensor(tc_basic_int8);
        LOG(WARNING) << "int8 saber result";
        print_tensor(tc_int8);
        LOG(WARNING) << "int8 diff tensor";
        print_tensor(tdiff);
        return false;
      }
    }
  }
#endif
  return true;
}

TEST_ENGINE(TestLite, gemm_prepacked_int8) {
  if (g_basic_test) {
    LOG(INFO) << "run basic sgemm test";
    for (auto& m : {1, 3, 8, 32, 397}) {
      for (auto& n : {1, 3, 13, 141, 512, 789, 1234}) {
        for (auto& k : {1, 3, 8, 59, 234}) {
          for (auto& tra : {false, true}) {
            for (auto& trb : {false, true}) {
              for (auto& has_bias : {false, true}) {
                for (auto& has_relu : {false, true}) {
                  for (auto& th : {1, 2, 4}) {
                    auto flag = test_gemm_int8(
                        tra, trb, m, n, k, has_bias, has_relu, g_cluster, th);
                    if (flag) {
                      LOG(INFO) << "test m = " << m << ", n=" << n
                                << ", k=" << k
                                << ", bias: " << (has_bias ? "true" : "false")
                                << ", relu: " << (has_relu ? "true" : "false")
                                << ", trans A: " << (tra ? "true" : "false")
                                << ", trans B: " << (trb ? "true" : "false")
                                << " passed\n";
                    } else {
                      LOG(FATAL) << "test m = " << m << ", n=" << n
                                 << ", k=" << k
                                 << ", bias: " << (has_bias ? "true" : "false")
                                 << ", relu: " << (has_relu ? "true" : "false")
                                 << ", trans A: " << (tra ? "true" : "false")
                                 << ", trans B: " << (trb ? "true" : "false")
                                 << " failed\n";
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  }
}

TEST_ENGINE(TestLite, gemm_prepacked_int8_custom) {
  auto flag = test_gemm_int8(g_traA,
                             g_traB,
                             g_M,
                             g_N,
                             g_K,
                             g_flag_bias,
                             g_flag_relu,
                             g_cluster,
                             g_threads);
  if (!flag) {
    LOG(FATAL) << "test m = " << g_M << ", n=" << g_N << ", k=" << g_K
               << ", trans A: " << g_traA << ", trans B: " << g_traB
               << ", bias: " << g_flag_bias << ", relu: " << g_flag_relu
               << " failed!!";
  }
  LOG(INFO) << "test m = " << g_M << ", n=" << g_N << ", k=" << g_K
            << ", trans A: " << g_traA << ", trans B: " << g_traB
            << ", bias: " << g_flag_bias << ", relu: " << g_flag_relu
            << " passed!!";
}

int main(int argc, const char** argv) {
#ifdef LITE_WITH_ARM
  paddle::lite::DeviceInfo::Init();
#endif
  LOG(ERROR)
      << "usage: ./" << argv[0]
      << " [do_basic_test] [cluster]  [threads]  [m] [n]  [k] [transA] "
         "[transB] [relu] [bias] [test iter] [compare result] [warm_up iter]";
  if (argc > 1) {
    g_basic_test = atoi(argv[1]) > 0;
  }
  if (argc > 2) {
    g_cluster = atoi(argv[2]);
  }
  if (argc > 3) {
    g_threads = atoi(argv[3]);
  }
  if (argc > 4) {
    if (argc < 10) {
      LOG(ERROR) << "usage: ./" << argv[0]
                 << " [do_basic_test] [cluster]  "
                    "[threads]  [m] [n]  [k] "
                    "[transA] [transB] [bias] [relu] "
                    "[test iter] [compare result] [warm_up iter]";
      return 0;
    }
    g_M = atoi(argv[4]);
    g_N = atoi(argv[5]);
    g_K = atoi(argv[6]);
    g_traA = atoi(argv[7]) > 0;
    g_traB = atoi(argv[8]) > 0;
    g_flag_bias = atoi(argv[9]) > 0;
    g_flag_relu = atoi(argv[10]) > 0;
  }
  if (argc > 11) {
    g_test_iter = atoi(argv[11]);
  }
  if (argc > 12) {
    g_compare_result = atoi(argv[12]) > 0;
  }
  if (argc > 13) {
    g_warmup_iter = atoi(argv[13]);
  }
  InitTest();
  RUN_ALL_TESTS(argv[0]);
  return 0;
}
