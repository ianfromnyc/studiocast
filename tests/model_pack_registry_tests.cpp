#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include "core/onnx/ort_session.h"
#include "core/open_audio/open_audio_diagnostics.h"
#include "core/open_audio/open_audio_onnx_session.h"
#include "core/open_audio/model_pack_registry.h"
#include "core/open_video/diagnostics.h"
#include "core/open_video/model_pack_registry.h"

namespace {

bool Expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << message << "\n";
    return false;
  }
  return true;
}

bool ExpectEq(const std::string &name, const std::string &got,
              const std::string &want) {
  if (got == want)
    return true;
  std::cerr << name << "\n  got:  " << got << "\n  want: " << want << "\n";
  return false;
}

bool ExpectContains(const std::string &name, const std::string &haystack,
                    const std::string &needle) {
  if (haystack.find(needle) != std::string::npos)
    return true;
  std::cerr << name << "\n  expected substring: " << needle
            << "\n  in: " << haystack << "\n";
  return false;
}

[[maybe_unused]] bool VectorContainsSubstring(
    const std::vector<std::string> &items, const std::string &needle) {
  for (const auto &item : items) {
    if (item.find(needle) != std::string::npos)
      return true;
  }
  return false;
}

bool ShouldRunTest(const char *name) {
  const char *only = std::getenv("STUDIOCAST_TEST_ONLY");
  if (!only || !*only)
    return true;
  const std::string filter(only);
  std::size_t pos = 0;
  while (pos <= filter.size()) {
    const std::size_t comma = filter.find(',', pos);
    const std::string token =
        filter.substr(pos, comma == std::string::npos ? std::string::npos
                                                      : comma - pos);
    if (token == name)
      return true;
    if (comma == std::string::npos)
      break;
    pos = comma + 1;
  }
  return false;
}

bool RunTestInChild(const char *name, bool (*fn)()) {
  const pid_t pid = fork();
  if (pid < 0) {
    std::cerr << "fork failed for " << name << "; running in-process\n";
    return fn();
  }
  if (pid == 0) {
    const bool ok = fn();
    std::cout.flush();
    std::cerr.flush();
    _exit(ok ? 0 : 1);
  }

  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    std::cerr << "waitpid failed for " << name << "\n";
    return false;
  }
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
    return true;
  if (WIFSIGNALED(status)) {
    std::cerr << name << " terminated by signal " << WTERMSIG(status) << "\n";
  } else {
    std::cerr << name << " failed with status " << status << "\n";
  }
  return false;
}

bool RunNamedTest(const char *name, bool (*fn)(), bool isolate = false) {
  if (!ShouldRunTest(name))
    return true;
  return isolate ? RunTestInChild(name, fn) : fn();
}

template <typename Result>
const Result *FindResult(const std::vector<Result> &results,
                         const std::string &id) {
  for (const auto &r : results) {
    if (r.id == id)
      return &r;
  }
  return nullptr;
}

bool TestOpenVideoIntegrity() {
  namespace ov = studiocast::open_video;
  const auto root = std::filesystem::path("tests") / "data" / "models" /
                    "model_integrity" / "open_video";

  const auto reg = ov::ModelPackRegistry::Scan(root);
  if (!Expect(reg.ResolveModel("video_good_sha").has_value(),
              "video_good_sha should scan as usable"))
    return false;
  if (!Expect(reg.ResolveModel("video_placeholder_pack") == std::nullopt,
              "placeholder Open Video pack should not be usable"))
    return false;
  {
    const auto &problems = reg.Problems();
    auto it = problems.find("video_placeholder_pack");
    if (!Expect(it != problems.end(),
                "placeholder Open Video pack should be reported"))
      return false;
    if (!Expect(it->second.find("placeholder") != std::string::npos,
                "placeholder Open Video problem should be classified"))
      return false;
  }

  const auto results = ov::ModelPackRegistry::Verify(root);
  const auto *good = FindResult(results, "video_good_sha");
  if (!Expect(good && good->ok, "video_good_sha should verify"))
    return false;
  if (!Expect(!good->files.empty(), "video_good_sha should include file result"))
    return false;
  if (!ExpectEq("video_good_sha status", good->status, "ok"))
    return false;
  if (!ExpectEq("video_good_sha file actual", good->files.front().actual_sha256,
                "cbc724d038823203ec77f127b74e6d84341e15f2befee18af5d6f5739f00492b"))
    return false;
  if (!ExpectEq("video_good_sha checksum kind",
                good->files.front().checksum_kind, "installed_sha256"))
    return false;

  const auto *bad = FindResult(results, "video_bad_sha");
  if (!Expect(bad && !bad->ok, "video_bad_sha should fail verification"))
    return false;
  if (!ExpectEq("video_bad_sha status", bad->status, "checksum_mismatch"))
    return false;
  if (!Expect(!bad->files.empty() &&
                  bad->files.front().status == "checksum_mismatch",
              "video_bad_sha file should report checksum_mismatch"))
    return false;

  const auto *missing = FindResult(results, "video_missing_file");
  if (!Expect(missing && !missing->ok,
              "video_missing_file should fail verification"))
    return false;
  if (!ExpectEq("video_missing_file status", missing->status, "missing"))
    return false;

  const auto *invalid = FindResult(results, "video_invalid_manifest");
  if (!Expect(invalid && !invalid->ok,
              "video_invalid_manifest should fail verification"))
    return false;
  if (!ExpectEq("video_invalid_manifest status", invalid->status,
                "invalid_manifest"))
    return false;

  const auto *placeholder = FindResult(results, "video_placeholder_pack");
  if (!Expect(placeholder && !placeholder->ok,
              "video_placeholder_pack should fail verification"))
    return false;
  return ExpectEq("video_placeholder_pack status", placeholder->status,
                  "placeholder");
}

bool TestOpenAudioIntegrity() {
  namespace oa = studiocast::open_audio;
  const auto root = std::filesystem::path("tests") / "data" / "models" /
                    "model_integrity" / "open_audio";

  const auto reg = oa::ModelPackRegistry::Scan(root);
  const auto goodPack = reg.ResolveModel("audio_good_sha");
  if (!Expect(goodPack.has_value(), "audio_good_sha should scan as usable"))
    return false;
  if (!ExpectEq("audio_good_sha origin_sha256", goodPack->origin_sha256,
                "0fb0fbc0fc5b6c4ddff835be62dfaa50e204ad86b993a4c4695eee5fba8e5c2a"))
    return false;
  if (!Expect(reg.ResolveModel("audio_placeholder_pack") == std::nullopt,
              "placeholder Open Audio pack should not be usable"))
    return false;

  const auto results = oa::ModelPackRegistry::Verify(root);
  const auto *good = FindResult(results, "audio_good_sha");
  if (!Expect(good && good->ok, "audio_good_sha should verify"))
    return false;
  if (!Expect(!good->files.empty(), "audio_good_sha should include file result"))
    return false;
  if (!ExpectEq("audio_good_sha expected", good->files.front().expected_sha256,
                "0fb0fbc0fc5b6c4ddff835be62dfaa50e204ad86b993a4c4695eee5fba8e5c2a"))
    return false;
  if (!ExpectEq("audio_good_sha actual", good->files.front().actual_sha256,
                "0fb0fbc0fc5b6c4ddff835be62dfaa50e204ad86b993a4c4695eee5fba8e5c2a"))
    return false;
  if (!ExpectEq("audio_good_sha checksum kind",
                good->files.front().checksum_kind, "installed_sha256"))
    return false;

  const auto *bad = FindResult(results, "audio_bad_sha");
  if (!Expect(bad && !bad->ok, "audio_bad_sha should fail verification"))
    return false;
  if (!ExpectEq("audio_bad_sha status", bad->status, "checksum_mismatch"))
    return false;

  const auto *missing = FindResult(results, "audio_missing_file");
  if (!Expect(missing && !missing->ok,
              "audio_missing_file should fail verification"))
    return false;
  if (!ExpectEq("audio_missing_file status", missing->status, "missing"))
    return false;

  const auto *invalid = FindResult(results, "audio_invalid_manifest");
  if (!Expect(invalid && !invalid->ok,
              "audio_invalid_manifest should fail verification"))
    return false;
  if (!ExpectEq("audio_invalid_manifest status", invalid->status,
                "invalid_manifest"))
    return false;

  const auto *placeholder = FindResult(results, "audio_placeholder_pack");
  if (!Expect(placeholder && !placeholder->ok,
              "audio_placeholder_pack should fail verification"))
    return false;
  return ExpectEq("audio_placeholder_pack status", placeholder->status,
                  "placeholder");
}

studiocast::onnx::OrtRuntimeInfo
MakeRuntime(std::vector<std::string> providers,
            std::vector<std::string> warnings = {}) {
  studiocast::onnx::OrtRuntimeInfo runtime;
  runtime.providers = std::move(providers);
  runtime.warnings = std::move(warnings);
  runtime.cuda_provider_present =
      std::find(runtime.providers.begin(), runtime.providers.end(),
                "CUDAExecutionProvider") != runtime.providers.end();
  runtime.tensorrt_provider_present =
      std::find(runtime.providers.begin(), runtime.providers.end(),
                "TensorrtExecutionProvider") != runtime.providers.end();
  runtime.cpu_provider_present =
      std::find(runtime.providers.begin(), runtime.providers.end(),
                "CPUExecutionProvider") != runtime.providers.end();
  return runtime;
}

struct FakeOnnxProviderBackend {
  studiocast::onnx::OrtRuntimeInfo runtime;
  bool tensorrt_ep_v2_build = false;
  studiocast::onnx::internal::OrtProviderAppendResult tensorrt_append;
  studiocast::onnx::internal::OrtProviderAppendResult cuda_append;
  bool fail_initial_create = false;
  bool fail_tensorrt_disabled_create = false;
  bool fail_cpu_create = false;
  std::string initial_error = "initial session create failed";
  std::string tensorrt_disabled_error = "cuda session create failed";
  std::string cpu_error = "cpu session create failed";
  int tensorrt_append_calls = 0;
  int cuda_append_calls = 0;
  std::vector<studiocast::onnx::internal::OrtSessionCreateAttempt> attempts;
};

studiocast::onnx::internal::OrtProviderAppendResult
FakeAppendTensorRt(void *context,
                   const studiocast::onnx::OrtSessionOptions &) {
  auto *fake = static_cast<FakeOnnxProviderBackend *>(context);
  ++fake->tensorrt_append_calls;
  return fake->tensorrt_append;
}

studiocast::onnx::internal::OrtProviderAppendResult
FakeAppendCuda(void *context, const studiocast::onnx::OrtSessionOptions &) {
  auto *fake = static_cast<FakeOnnxProviderBackend *>(context);
  ++fake->cuda_append_calls;
  return fake->cuda_append;
}

bool FakeShouldFail(
    const FakeOnnxProviderBackend &fake,
    studiocast::onnx::internal::OrtSessionCreateAttempt attempt,
    std::string *error) {
  switch (attempt) {
  case studiocast::onnx::internal::OrtSessionCreateAttempt::Initial:
    if (fake.fail_initial_create) {
      if (error)
        *error = fake.initial_error;
      return true;
    }
    return false;
  case studiocast::onnx::internal::OrtSessionCreateAttempt::TensorRtDisabled:
    if (fake.fail_tensorrt_disabled_create) {
      if (error)
        *error = fake.tensorrt_disabled_error;
      return true;
    }
    return false;
  case studiocast::onnx::internal::OrtSessionCreateAttempt::CpuOnly:
    if (fake.fail_cpu_create) {
      if (error)
        *error = fake.cpu_error;
      return true;
    }
    return false;
  }
  return false;
}

studiocast::onnx::internal::OrtSessionCreateResult
FakeCreateSession(void *context,
                  studiocast::onnx::internal::OrtSessionCreateAttempt attempt,
                  const studiocast::onnx::OrtSessionOptions &opts,
                  studiocast::onnx::OrtSessionInfo *info_out) {
  auto *fake = static_cast<FakeOnnxProviderBackend *>(context);
  fake->attempts.push_back(attempt);

  studiocast::onnx::internal::OrtProviderAppendHooks append_hooks;
  append_hooks.context = fake;
  append_hooks.append_tensorrt = FakeAppendTensorRt;
  append_hooks.append_cuda = FakeAppendCuda;
  if (info_out) {
    *info_out = studiocast::onnx::internal::PlanOrtProviderAttempt(
        fake->runtime, opts, fake->tensorrt_ep_v2_build, append_hooks);
  }

  studiocast::onnx::internal::OrtSessionCreateResult result;
  std::string error;
  if (FakeShouldFail(*fake, attempt, &error)) {
    result.error = error;
    return result;
  }
  result.created = true;
  return result;
}

studiocast::onnx::internal::OrtSessionCreatePlanResult
RunFakeOnnxProviderPlan(studiocast::onnx::OrtSessionOptions opts,
                        FakeOnnxProviderBackend *fake) {
  studiocast::onnx::internal::OrtSessionCreateHooks create_hooks;
  create_hooks.context = fake;
  create_hooks.create_session = FakeCreateSession;
  return studiocast::onnx::internal::CreateOrtSessionWithProviderFallbacks(
      opts, create_hooks);
}

bool TestOnnxCpuOnlySessionDiagnostics() {
  FakeOnnxProviderBackend fake;
  fake.runtime = MakeRuntime({"CPUExecutionProvider"});
  studiocast::onnx::OrtSessionOptions opts;
  opts.prefer_cuda = false;

  const auto plan = RunFakeOnnxProviderPlan(opts, &fake);
  if (!Expect(plan.created, "CPU-only provider plan should create"))
    return false;
  const auto &info = plan.info;
  return Expect(!info.using_cuda, "CPU-only plan must not report CUDA") &&
         Expect(!info.using_tensorrt,
                "CPU-only plan must not report TensorRT") &&
         Expect(info.cpu_provider_advertised,
                "CPU-only plan should preserve advertised CPU provider") &&
         Expect(info.cpu_provider_usable,
                "CPU-only plan should mark CPU provider usable") &&
         ExpectEq("CPU-only active provider", info.active_provider, "cpu") &&
         Expect(fake.cuda_append_calls == 0,
                "CPU-only plan should not append CUDA");
}

bool TestOnnxCudaAppendFailureFallsBackToCpu() {
  FakeOnnxProviderBackend fake;
  fake.runtime =
      MakeRuntime({"CPUExecutionProvider", "CUDAExecutionProvider"});
  fake.cuda_append.status = "unavailable";
  fake.cuda_append.warnings.push_back(
      "cuda_ep_unavailable: fake CUDA runtime missing");

  studiocast::onnx::OrtSessionOptions opts;
  opts.prefer_cuda = true;
  const auto plan = RunFakeOnnxProviderPlan(opts, &fake);
  if (!Expect(plan.created,
              "CUDA append failure should still create CPU provider plan"))
    return false;

  const auto &info = plan.info;
  return Expect(info.cuda_provider_advertised,
                "CUDA append failure should preserve advertised CUDA state") &&
         Expect(!info.cuda_provider_appended,
                "CUDA append failure should not mark CUDA appended") &&
         Expect(!info.using_cuda,
                "CUDA append failure should not report CUDA active") &&
         Expect(info.cpu_provider_usable,
                "CUDA append failure should leave CPU usable") &&
         ExpectEq("CUDA append failure active provider", info.active_provider,
                  "cpu") &&
         Expect(VectorContainsSubstring(info.warnings,
                                        "cuda_ep_unavailable: fake"),
                "CUDA append failure warning should propagate");
}

bool TestOnnxCudaSessionFailureFallsBackToCpu() {
  FakeOnnxProviderBackend fake;
  fake.runtime =
      MakeRuntime({"CPUExecutionProvider", "CUDAExecutionProvider"});
  fake.cuda_append.appended = true;
  fake.cuda_append.status = "appended";
  fake.cuda_append.needs_stream_sync = true;
  fake.fail_initial_create = true;
  fake.initial_error = "fake CUDA session constructor failed";

  studiocast::onnx::OrtSessionOptions opts;
  opts.prefer_cuda = true;
  const auto plan = RunFakeOnnxProviderPlan(opts, &fake);
  if (!Expect(plan.created,
              "CUDA session create failure should fall back to CPU"))
    return false;

  const auto &info = plan.info;
  return Expect(fake.attempts.size() == 2,
                "CUDA session failure should try initial and CPU plans") &&
         Expect(fake.attempts[1] ==
                    studiocast::onnx::internal::OrtSessionCreateAttempt::CpuOnly,
                "CUDA session failure should retry CPU only") &&
         Expect(!info.using_cuda,
                "CPU fallback should not report CUDA active") &&
         Expect(info.cpu_provider_usable,
                "CPU fallback should mark CPU provider usable") &&
         Expect(info.cuda_session_create_failed_fell_back_to_cpu,
                "CPU fallback should record CUDA session create fallback") &&
         Expect(VectorContainsSubstring(
                    info.warnings,
                    "cuda_session_create_failed_fell_back_to_cpu: fake CUDA"),
                "CPU fallback should propagate CUDA create warning");
}

bool TestOnnxTensorRtRequestedFallsBackToCuda() {
  FakeOnnxProviderBackend fake;
  fake.runtime = MakeRuntime({"CPUExecutionProvider", "CUDAExecutionProvider",
                              "TensorrtExecutionProvider"});
  fake.tensorrt_ep_v2_build = true;
  fake.tensorrt_append.appended = true;
  fake.tensorrt_append.status = "appended";
  fake.tensorrt_append.cache_path = "/tmp/studiocast-test-trt-cache";
  fake.tensorrt_append.warnings.push_back(
      "tensorrt_builder_optimization_level_unavailable: fake");
  fake.cuda_append.appended = true;
  fake.cuda_append.status = "appended";
  fake.fail_initial_create = true;
  fake.initial_error = "fake TensorRT engine build failed";

  studiocast::onnx::OrtSessionOptions opts;
  opts.prefer_cuda = true;
  opts.enable_tensorrt = true;
  opts.tensorrt_enable_cuda_fallback = true;
  const auto plan = RunFakeOnnxProviderPlan(opts, &fake);
  if (!Expect(plan.created,
              "TensorRT session create failure should fall back to CUDA"))
    return false;

  const auto &info = plan.info;
  return Expect(fake.attempts.size() == 2,
                "TensorRT failure should try initial and CUDA plans") &&
         Expect(fake.attempts[1] == studiocast::onnx::internal::
                                        OrtSessionCreateAttempt::TensorRtDisabled,
                "TensorRT failure should retry without TensorRT") &&
         Expect(!info.using_tensorrt,
                "TensorRT fallback should not report TensorRT active") &&
         Expect(info.using_cuda,
                "TensorRT fallback should report CUDA active") &&
         Expect(info.cuda_provider_usable,
                "TensorRT fallback should mark CUDA usable") &&
         Expect(info.tensorrt_session_create_failed_fell_back_to_cuda,
                "TensorRT fallback should record CUDA fallback") &&
         ExpectEq("TensorRT fallback status", info.tensorrt_status,
                  "session_create_failed_fell_back_to_cuda") &&
         ExpectEq("TensorRT fallback active provider", info.active_provider,
                  "cuda") &&
         Expect(info.tensorrt_engine_cache_path ==
                    std::filesystem::path("/tmp/studiocast-test-trt-cache"),
                "TensorRT fallback should preserve cache path") &&
         Expect(VectorContainsSubstring(info.warnings,
                                        "tensorrt_builder_optimization"),
                "TensorRT append warning should propagate") &&
         Expect(VectorContainsSubstring(
                    info.warnings,
                    "tensorrt_session_create_failed_fell_back_to_cuda: fake"),
                "TensorRT create warning should propagate");
}

bool TestOnnxProviderWarningPropagation() {
  FakeOnnxProviderBackend fake;
  fake.runtime = MakeRuntime(
      {"CPUExecutionProvider", "CUDAExecutionProvider"},
      {"onnxruntime_provider_query_failed: fake query warning"});
  fake.cuda_append.status = "unavailable";
  fake.cuda_append.warnings.push_back(
      "cuda_ep_unavailable: fake append warning");

  studiocast::onnx::OrtSessionOptions opts;
  opts.prefer_cuda = true;
  const auto plan = RunFakeOnnxProviderPlan(opts, &fake);
  if (!Expect(plan.created, "warning propagation plan should create"))
    return false;
  return Expect(VectorContainsSubstring(
                    plan.info.warnings,
                    "onnxruntime_provider_query_failed: fake"),
                "runtime warning should propagate to session info") &&
         Expect(VectorContainsSubstring(plan.info.warnings,
                                        "cuda_ep_unavailable: fake append"),
                "append warning should propagate to session info");
}

bool TestOpenAudioUsesSharedOnnxProviderInfo() {
#if !STUDIOCAST_HAVE_ONNXRUNTIME
  return true;
#else
  const auto model = std::filesystem::path("tests") / "data" / "models" /
                     "open_cuda" / "mock_model" / "model.onnx";
  studiocast::open_audio::OrtSessionOptions opts;
  opts.prefer_cuda = false;
  studiocast::open_audio::OrtSessionInfo info;
  std::string err;
  auto session =
      studiocast::open_audio::OpenAudioOrtSession::Create(model, opts, &info,
                                                          &err);
  if (!Expect(session != nullptr,
              "Open Audio ORT adapter should create CPU session: " + err))
    return false;
  return Expect(!info.using_cuda,
                "Open Audio CPU session must not report CUDA") &&
         Expect(info.cpu_provider_usable,
                "Open Audio adapter should map shared CPU usable flag") &&
         ExpectEq("Open Audio active provider", info.active_provider, "cpu");
#endif
}

bool TestOnnxWarningsPropagateToDiagnosticsJson() {
  studiocast::open_audio::OpenAudioDiagnostics audio;
  audio.onnxruntime_warnings.push_back(
      "cuda_session_create_failed_fell_back_to_cpu: test");
  const auto audio_json = audio.ToJson();

  studiocast::open_cuda::OpenCudaDiagnostics video;
  video.onnxruntime_warnings.push_back(
      "tensorrt_session_create_failed_fell_back_to_cuda: test");
  const auto video_json = video.ToJson();

  return ExpectContains("Open Audio diagnostics warnings", audio_json,
                        "\"onnxruntime_warnings\"") &&
         ExpectContains("Open Audio diagnostics warning value", audio_json,
                        "cuda_session_create_failed_fell_back_to_cpu") &&
         ExpectContains("Open CUDA diagnostics warnings", video_json,
                        "\"onnxruntime_warnings\"") &&
         ExpectContains("Open CUDA diagnostics warning value", video_json,
                        "tensorrt_session_create_failed_fell_back_to_cuda");
}

} // namespace

int main() {
  bool ok = true;
  ok = RunNamedTest("open_video_integrity", TestOpenVideoIntegrity) && ok;
  ok = RunNamedTest("open_audio_integrity", TestOpenAudioIntegrity) && ok;
  ok = RunNamedTest("onnx_cpu_only", TestOnnxCpuOnlySessionDiagnostics,
                    true) &&
       ok;
  ok = RunNamedTest("onnx_cuda_append_fallback",
                    TestOnnxCudaAppendFailureFallsBackToCpu) &&
       ok;
  ok = RunNamedTest("onnx_cuda_session_fallback",
                    TestOnnxCudaSessionFailureFallsBackToCpu) &&
       ok;
  ok = RunNamedTest("onnx_tensorrt_fallback",
                    TestOnnxTensorRtRequestedFallsBackToCuda) &&
       ok;
  ok = RunNamedTest("onnx_provider_warning_propagation",
                    TestOnnxProviderWarningPropagation) &&
       ok;
  ok = RunNamedTest("open_audio_shared_onnx",
                    TestOpenAudioUsesSharedOnnxProviderInfo, true) &&
       ok;
  ok = RunNamedTest("onnx_diagnostics_warnings",
                    TestOnnxWarningsPropagateToDiagnosticsJson) &&
       ok;
  if (!ok)
    return 1;
  std::cout << "MODEL REGISTRY TESTS OK\n";
  return 0;
}
