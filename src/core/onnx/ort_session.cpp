#include "core/onnx/ort_session.h"

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <type_traits>
#include <utility>
#include <vector>

#if STUDIOCAST_HAVE_ONNXRUNTIME
#include <dlfcn.h>

#include <onnxruntime_c_api.h>
#include <onnxruntime_cxx_api.h>
#endif

#ifndef STUDIOCAST_ORT_HAS_CUDA_EP_V2
#define STUDIOCAST_ORT_HAS_CUDA_EP_V2 0
#endif

#ifndef STUDIOCAST_ORT_HAS_TENSORRT_EP_V2
#define STUDIOCAST_ORT_HAS_TENSORRT_EP_V2 0
#endif

namespace studiocast::onnx {
namespace {

bool HasProvider(const std::vector<std::string> &providers,
                 const char *provider) {
  return std::find(providers.begin(), providers.end(), std::string(provider)) !=
         providers.end();
}

#if STUDIOCAST_HAVE_ONNXRUNTIME
std::string QueryOrtLibraryPath() {
  Dl_info info{};
  if (dladdr(reinterpret_cast<const void *>(&OrtGetApiBase), &info) != 0 &&
      info.dli_fname && *info.dli_fname) {
    return info.dli_fname;
  }
  return {};
}
#endif

} // namespace

namespace {

std::filesystem::path EnvPath(const char *name) {
  const char *v = std::getenv(name);
  if (!v || !*v)
    return {};
  return std::filesystem::path(v);
}

} // namespace

bool OrtBuildHasTensorRtEpV2() {
  return STUDIOCAST_ORT_HAS_TENSORRT_EP_V2 != 0;
}

std::filesystem::path DefaultTensorRtCachePath(int cuda_device_id) {
  std::filesystem::path base = EnvPath("XDG_CACHE_HOME");
  if (base.empty()) {
    const auto home = EnvPath("HOME");
    if (!home.empty()) {
      base = home / ".cache";
    }
  }
  if (base.empty()) {
    std::error_code ec;
    base = std::filesystem::temp_directory_path(ec);
    if (ec || base.empty()) {
      return {};
    }
  }

  const int id = std::max(0, cuda_device_id);
  return base / "studiocast" / "trt_cache" /
         (std::string("gpu") + std::to_string(id));
}

bool OrtErrorLooksLikeVramOom(const std::string &ort_msg) {
  // Common failure strings seen from ORT CUDA EP when VRAM is exhausted or
  // cuDNN can't find a viable algorithm/workspace due to memory pressure.
  return ort_msg.find("CUDA failure 2") != std::string::npos ||
         ort_msg.find("out of memory") != std::string::npos ||
         ort_msg.find("cudaErrorMemoryAllocation") != std::string::npos ||
         ort_msg.find("CUDNN_STATUS_ALLOC_FAILED") != std::string::npos ||
         // Often appears after OOM / workspace allocation failures when cuDNN
         // can't run the chosen kernel.
         ort_msg.find("CUDNN_STATUS_NOT_SUPPORTED") != std::string::npos;
}

std::string HumanizeOrtError(const std::string &ort_msg,
                             const std::filesystem::path &model_path) {
  if (OrtErrorLooksLikeVramOom(ort_msg)) {
    std::string out = "GPU is likely out of VRAM for this model";
    if (!model_path.empty()) {
      out += " (" + model_path.filename().string() + ")";
    }
    out += ". The model is probably too large for the available GPU memory. "
           "Try a smaller model, lower input resolution, or close other "
           "GPU-heavy apps. "
           "Underlying ONNX Runtime error: ";
    out += ort_msg;
    return out;
  }

  std::string out = "ONNX Runtime error: ";
  out += ort_msg;
  return out;
}

OrtRuntimeInfo OrtSession::QueryRuntimeInfo() {
  OrtRuntimeInfo out;

#if defined(STUDIOCAST_ORT_HAS_CUDA_EP_V2) && STUDIOCAST_ORT_HAS_CUDA_EP_V2
  out.cuda_ep_v2_build = true;
#endif

#if STUDIOCAST_HAVE_ONNXRUNTIME
  const char *v = OrtGetApiBase()->GetVersionString();
  if (v) {
    out.version = v;
  }
  out.library_path = QueryOrtLibraryPath();

  try {
    auto &api = Ort::GetApi();
    char **providers = nullptr;
    int num = 0;
    Ort::ThrowOnError(api.GetAvailableProviders(&providers, &num));
    for (int i = 0; i < num; ++i) {
      if (providers && providers[i]) {
        out.providers.emplace_back(providers[i]);
      }
    }

    // ORT changed this API from `void` to returning `OrtStatus*`
    // (warn_unused_result).
    if constexpr (std::is_void_v<decltype(api.ReleaseAvailableProviders(
                      providers, num))>) {
      api.ReleaseAvailableProviders(providers, num);
    } else {
      Ort::ThrowOnError(api.ReleaseAvailableProviders(providers, num));
    }
  } catch (const Ort::Exception &) {
    // Best-effort only.
  }
#endif

  out.cuda_provider_present =
      HasProvider(out.providers, "CUDAExecutionProvider");
  out.tensorrt_provider_present =
      HasProvider(out.providers, "TensorrtExecutionProvider");
  out.cpu_provider_present = HasProvider(out.providers, "CPUExecutionProvider");

  return out;
}

#if STUDIOCAST_HAVE_ONNXRUNTIME
namespace {

Ort::Env &GlobalEnv() {
  // NOTE: ORT env is process-global and should be long-lived.
  static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "studiocast_onnx");
  return env;
}

const char *ElemTypeToString(ONNXTensorElementDataType t) {
  switch (t) {
  case ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED:
    return "undefined";
  case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:
    return "float32";
  case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8:
    return "uint8";
  case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8:
    return "int8";
  case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT16:
    return "uint16";
  case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT16:
    return "int16";
  case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32:
    return "int32";
  case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64:
    return "int64";
  case ONNX_TENSOR_ELEMENT_DATA_TYPE_STRING:
    return "string";
  case ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL:
    return "bool";
  case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16:
    return "float16";
  case ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE:
    return "float64";
  case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT32:
    return "uint32";
  case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT64:
    return "uint64";
  case ONNX_TENSOR_ELEMENT_DATA_TYPE_COMPLEX64:
    return "complex64";
  case ONNX_TENSOR_ELEMENT_DATA_TYPE_COMPLEX128:
    return "complex128";
  case ONNX_TENSOR_ELEMENT_DATA_TYPE_BFLOAT16:
    return "bfloat16";
  default:
    return "unknown";
  }
}

std::string ShapeToString(const std::vector<int64_t> &shape) {
  std::ostringstream oss;
  oss << "[";
  for (std::size_t i = 0; i < shape.size(); ++i) {
    if (i)
      oss << ", ";
    oss << shape[i];
  }
  oss << "]";
  return oss.str();
}

template <typename TensorTypeAndShapeInfo>
std::string TensorDesc(const TensorTypeAndShapeInfo &ti) {
  std::ostringstream oss;
  const auto type = ti.GetElementType();
  oss << "tensor(" << ElemTypeToString(type) << ")";

  const auto shape = ti.GetShape();
  if (!shape.empty()) {
    oss << " shape=" << ShapeToString(shape);
  }
  return oss.str();
}

struct TensorRtAppendResult {
  bool appended = false;
  bool needs_stream_sync = false;
  std::filesystem::path cache_path;
  std::string status;
  std::vector<std::string> warnings;
};

void AppendTensorRtOption(std::vector<std::string> *keys,
                          std::vector<std::string> *values,
                          std::string key, std::string value) {
  keys->push_back(std::move(key));
  values->push_back(std::move(value));
}

#if STUDIOCAST_ORT_HAS_TENSORRT_EP_V2
OrtTensorRTProviderOptionsV2 *
CreateConfiguredTensorRtOptions(const OrtSessionOptions &opts,
                                const std::filesystem::path &cache_path,
                                bool include_builder_optimization_level) {
  const auto &api = Ort::GetApi();

  OrtTensorRTProviderOptionsV2 *trt_opts_v2 = nullptr;
  Ort::ThrowOnError(api.CreateTensorRTProviderOptions(&trt_opts_v2));

  try {
    std::vector<std::string> keys;
    std::vector<std::string> values;
    keys.reserve(7);
    values.reserve(7);

    AppendTensorRtOption(&keys, &values, "device_id",
                         std::to_string(opts.cuda_device_id));
    AppendTensorRtOption(&keys, &values, "trt_max_workspace_size",
                         std::to_string(opts.tensorrt_max_workspace_size));
    AppendTensorRtOption(&keys, &values, "trt_fp16_enable",
                         opts.tensorrt_fp16_enable ? "1" : "0");
    AppendTensorRtOption(&keys, &values, "trt_engine_cache_enable",
                         opts.tensorrt_engine_cache_enable ? "1" : "0");
    if (opts.tensorrt_engine_cache_enable && !cache_path.empty()) {
      AppendTensorRtOption(&keys, &values, "trt_engine_cache_path",
                           cache_path.string());
    }
    if (include_builder_optimization_level) {
      AppendTensorRtOption(
          &keys, &values, "trt_builder_optimization_level",
          std::to_string(opts.tensorrt_builder_optimization_level));
    }

    std::vector<const char *> key_ptrs;
    std::vector<const char *> value_ptrs;
    key_ptrs.reserve(keys.size());
    value_ptrs.reserve(values.size());
    for (const auto &k : keys)
      key_ptrs.push_back(k.c_str());
    for (const auto &v : values)
      value_ptrs.push_back(v.c_str());

    Ort::ThrowOnError(api.UpdateTensorRTProviderOptions(
        trt_opts_v2, key_ptrs.data(), value_ptrs.data(), key_ptrs.size()));
    return trt_opts_v2;
  } catch (...) {
    api.ReleaseTensorRTProviderOptions(trt_opts_v2);
    throw;
  }
}
#endif

TensorRtAppendResult TryAppendTensorRtEp(Ort::SessionOptions *so,
                                         const OrtSessionOptions &opts) {
  TensorRtAppendResult result;
  result.cache_path =
      opts.tensorrt_engine_cache_path.empty()
          ? DefaultTensorRtCachePath(opts.cuda_device_id)
          : opts.tensorrt_engine_cache_path;

  if (!so) {
    result.status = "invalid_session_options";
    result.warnings.push_back("tensorrt_ep_unavailable: null session options");
    return result;
  }

#if !STUDIOCAST_ORT_HAS_TENSORRT_EP_V2
  result.status = "unsupported_in_build";
  result.warnings.push_back(
      "tensorrt_ep_unavailable: build does not expose TensorRT EP V2 provider "
      "options");
  return result;
#else
  const auto &api = Ort::GetApi();
  try {
    // Some ORT provider APIs may log internally. Ensure the process-global ORT
    // env/logger exists before calling into provider setup.
    (void)GlobalEnv();

    if (opts.tensorrt_engine_cache_enable && !result.cache_path.empty()) {
      std::error_code ec;
      std::filesystem::create_directories(result.cache_path, ec);
      if (ec) {
        result.warnings.push_back(
            std::string("tensorrt_cache_dir_unavailable: ") + ec.message());
      }
    }

    OrtTensorRTProviderOptionsV2 *trt_opts_v2 = nullptr;
    try {
      trt_opts_v2 = CreateConfiguredTensorRtOptions(
          opts, result.cache_path, /*include_builder_optimization_level=*/true);
    } catch (const Ort::Exception &e) {
      const std::string msg = e.what();
      if (msg.find("trt_builder_optimization_level") == std::string::npos) {
        throw;
      }
      result.warnings.push_back(
          std::string("tensorrt_builder_optimization_level_unavailable: ") +
          msg);
      trt_opts_v2 = CreateConfiguredTensorRtOptions(
          opts, result.cache_path,
          /*include_builder_optimization_level=*/false);
    }

    struct Guard {
      const OrtApi *api = nullptr;
      OrtTensorRTProviderOptionsV2 *opts = nullptr;
      ~Guard() {
        if (api && opts) {
          api->ReleaseTensorRTProviderOptions(opts);
        }
      }
    } guard{&api, trt_opts_v2};

    if (opts.user_compute_stream != nullptr) {
      try {
        Ort::ThrowOnError(api.UpdateTensorRTProviderOptionsWithValue(
            trt_opts_v2, "user_compute_stream",
            reinterpret_cast<void *>(opts.user_compute_stream)));
        result.needs_stream_sync = false;
      } catch (const Ort::Exception &e) {
        result.needs_stream_sync = true;
        result.warnings.push_back(
            std::string("tensorrt_user_compute_stream_unavailable: ") +
            e.what());
      }
    } else {
      result.needs_stream_sync = true;
    }

    Ort::ThrowOnError(
        api.SessionOptionsAppendExecutionProvider_TensorRT_V2(*so,
                                                              trt_opts_v2));
    result.appended = true;
    result.status = "appended";
    return result;
  } catch (const Ort::Exception &e) {
    result.status = "unavailable";
    result.warnings.push_back(std::string("tensorrt_ep_unavailable: ") +
                              e.what());
    result.needs_stream_sync = false;
    return result;
  }
#endif
}

bool TryAppendCudaEp(Ort::SessionOptions *so, const OrtSessionOptions &opts,
                     bool *needs_stream_sync, std::string *warn) {
  if (warn)
    warn->clear();
  if (needs_stream_sync)
    *needs_stream_sync = false;

  try {
    // Some ORT CUDA EP APIs may log internally. Ensure the process-global ORT
    // env/logger exists before calling into provider setup.
    (void)GlobalEnv();
#if STUDIOCAST_ORT_HAS_CUDA_EP_V2
    const auto &api = Ort::GetApi();

    OrtCUDAProviderOptionsV2 *cuda_opts_v2 = nullptr;
    Ort::ThrowOnError(api.CreateCUDAProviderOptions(&cuda_opts_v2));
    struct Guard {
      const OrtApi *api = nullptr;
      OrtCUDAProviderOptionsV2 *opts = nullptr;
      ~Guard() {
        if (api && opts) {
          api->ReleaseCUDAProviderOptions(opts);
        }
      }
    } guard{&api, cuda_opts_v2};

    const char *keys[] = {"device_id"};
    const std::string dev = std::to_string(opts.cuda_device_id);
    const char *values[] = {dev.c_str()};
    Ort::ThrowOnError(
        api.UpdateCUDAProviderOptions(cuda_opts_v2, keys, values, 1));

    if (opts.user_compute_stream != nullptr) {
      Ort::ThrowOnError(api.UpdateCUDAProviderOptionsWithValue(
          cuda_opts_v2, "user_compute_stream",
          reinterpret_cast<void *>(opts.user_compute_stream)));
      if (needs_stream_sync)
        *needs_stream_sync = false;
    } else {
      // Without user_compute_stream, ORT may use internal streams.
      if (needs_stream_sync)
        *needs_stream_sync = true;
    }

    Ort::ThrowOnError(
        api.SessionOptionsAppendExecutionProvider_CUDA_V2(*so, cuda_opts_v2));
    return true;
#else
    OrtCUDAProviderOptions cuda_opts{};
    cuda_opts.device_id = opts.cuda_device_id;
    so->AppendExecutionProvider_CUDA(cuda_opts);

    // Legacy CUDA EP does not expose stream interop.
    if (needs_stream_sync)
      *needs_stream_sync = true;
    return true;
#endif
  } catch (const Ort::Exception &e) {
    if (warn)
      *warn = e.what();
    if (needs_stream_sync)
      *needs_stream_sync = false;
    return false;
  }
}

void ConfigureExecutionProviders(Ort::SessionOptions *so,
                                 const OrtSessionOptions &opts,
                                 OrtSessionInfo *info) {
  if (!info)
    return;

  info->using_tensorrt = false;
  info->using_cuda = false;
  info->cuda_needs_stream_sync = false;
  info->active_provider = "cpu";
  info->appended_provider = "cpu";
  info->appended_providers.clear();
  info->tensorrt_status =
      opts.enable_tensorrt ? "requested" : "not_requested";
  info->tensorrt_engine_cache_path =
      opts.tensorrt_engine_cache_path.empty()
          ? DefaultTensorRtCachePath(opts.cuda_device_id)
          : opts.tensorrt_engine_cache_path;

  bool tensor_rt_needs_sync = false;
  if (opts.enable_tensorrt) {
    const TensorRtAppendResult trt = TryAppendTensorRtEp(so, opts);
    info->tensorrt_status = trt.status;
    info->tensorrt_engine_cache_path = trt.cache_path;
    info->warnings.insert(info->warnings.end(), trt.warnings.begin(),
                          trt.warnings.end());
    if (trt.appended) {
      info->using_tensorrt = true;
      info->appended_providers.push_back("tensorrt");
      tensor_rt_needs_sync = trt.needs_stream_sync;
    }
  }

  bool cuda_needs_sync = false;
  std::string cuda_warn;
  const bool should_append_cuda =
      opts.prefer_cuda &&
      (!opts.enable_tensorrt || opts.tensorrt_enable_cuda_fallback ||
       !info->using_tensorrt);
  if (should_append_cuda) {
    info->using_cuda =
        TryAppendCudaEp(so, opts, &cuda_needs_sync, &cuda_warn);
  }
  if (!cuda_warn.empty() && !info->using_cuda && opts.prefer_cuda) {
    info->warnings.push_back(std::string("cuda_ep_unavailable: ") +
                             cuda_warn);
  }
  if (info->using_cuda) {
    info->appended_providers.push_back("cuda");
  }

  if (!info->appended_providers.empty()) {
    info->appended_provider = info->appended_providers.front();
    info->active_provider = info->appended_provider;
  }

  if (info->using_tensorrt) {
    info->tensorrt_status = "active";
  }

  info->cuda_needs_stream_sync =
      (info->using_tensorrt && tensor_rt_needs_sync) ||
      (info->using_cuda && cuda_needs_sync);
}

} // namespace
#endif

struct OrtSession::Impl {
  OrtSessionInfo info;
  OrtSessionOptions opts;
  std::filesystem::path model_path;

  bool latched_failure = false;
  std::string latched_error;

#if STUDIOCAST_HAVE_ONNXRUNTIME
  std::unique_ptr<Ort::Session> session;

  // CUDA-only (lazily created when RunCudaIoBinding is used).
  std::unique_ptr<Ort::IoBinding> binding;
  std::optional<Ort::MemoryInfo> cuda_mem_info;

  // Scratch space to avoid per-frame heap churn in real-time processing.
  std::vector<const char *> scratch_input_names;
  std::vector<const char *> scratch_output_names;
  std::vector<Ort::Value> scratch_inputs;
  std::vector<Ort::Value> scratch_outputs;
#endif

  void LatchFailure(const std::string &err) {
    latched_failure = true;
    latched_error = err;

#if STUDIOCAST_HAVE_ONNXRUNTIME
    // Best-effort cleanup to release allocations held by this session.
    binding.reset();
    session.reset();
    cuda_mem_info.reset();
    scratch_input_names.clear();
    scratch_output_names.clear();
    scratch_inputs.clear();
    scratch_outputs.clear();
#endif
  }
};

OrtSession::OrtSession(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
OrtSession::~OrtSession() = default;

const OrtSessionInfo &OrtSession::info() const { return impl_->info; }

bool OrtSession::HasLatchedFailure() const {
  return impl_ ? impl_->latched_failure : false;
}

const std::string &OrtSession::LatchedError() const {
  static const std::string empty;
  return impl_ ? impl_->latched_error : empty;
}

std::unique_ptr<OrtSession>
OrtSession::Create(const std::filesystem::path &model_path,
                   const OrtSessionOptions &opts, OrtSessionInfo *info_out,
                   std::string *error) {
  if (error)
    error->clear();
  if (info_out)
    *info_out = OrtSessionInfo{};

#if !STUDIOCAST_HAVE_ONNXRUNTIME
  (void)model_path;
  (void)opts;
  if (error) {
    *error = "ONNX Runtime is not available in this build "
             "(STUDIOCAST_HAVE_ONNXRUNTIME=0).";
  }
  return nullptr;
#else
  try {
    const std::string model = model_path.string();
    if (model.empty()) {
      if (error)
        *error = "model_path is empty";
      return nullptr;
    }

    OrtSessionInfo info;
    auto create_session = [&](const OrtSessionOptions &session_opts,
                              OrtSessionInfo *provider_info) {
      Ort::SessionOptions so;
      so.SetIntraOpNumThreads(1);
      so.SetInterOpNumThreads(1);
      so.SetGraphOptimizationLevel(ORT_ENABLE_EXTENDED);
      ConfigureExecutionProviders(&so, session_opts, provider_info);
      return std::make_unique<Ort::Session>(GlobalEnv(), model.c_str(), so);
    };

    std::unique_ptr<Ort::Session> session;
    try {
      session = create_session(opts, &info);
    } catch (const Ort::Exception &trt_create_error) {
      if (!opts.enable_tensorrt || !info.using_tensorrt ||
          !opts.tensorrt_enable_cuda_fallback || !opts.prefer_cuda) {
        throw;
      }

      const std::string trt_error = trt_create_error.what();
      OrtSessionOptions retry_opts = opts;
      retry_opts.enable_tensorrt = false;
      OrtSessionInfo retry_info;
      try {
        session = create_session(retry_opts, &retry_info);
      } catch (const Ort::Exception &retry_error) {
        std::ostringstream oss;
        oss << "TensorRT session creation failed: " << trt_error
            << "; CUDA fallback session creation also failed: "
            << retry_error.what();
        throw Ort::Exception(oss.str(), retry_error.GetOrtErrorCode());
      }

      retry_info.tensorrt_status = "session_create_failed_fell_back_to_cuda";
      retry_info.tensorrt_engine_cache_path = info.tensorrt_engine_cache_path;
      retry_info.warnings.insert(retry_info.warnings.begin(),
                                 info.warnings.begin(), info.warnings.end());
      retry_info.warnings.push_back(
          std::string("tensorrt_session_create_failed_fell_back_to_cuda: ") +
          trt_error);
      info = std::move(retry_info);
    }

    Ort::AllocatorWithDefaultOptions alloc;

    const std::size_t in_count = session->GetInputCount();
    info.input_names.reserve(in_count);
    info.input_descriptions.reserve(in_count);
    info.input_shapes.reserve(in_count);
    info.input_elem_types.reserve(in_count);
    for (std::size_t i = 0; i < in_count; ++i) {
      auto name = session->GetInputNameAllocated(i, alloc);
      info.input_names.emplace_back(name ? name.get() : "");

      try {
        const auto ti = session->GetInputTypeInfo(i);
        const auto onnx_type = ti.GetONNXType();
        if (onnx_type == ONNX_TYPE_TENSOR) {
          const auto tensor = ti.GetTensorTypeAndShapeInfo();
          info.input_descriptions.emplace_back(TensorDesc(tensor));
          info.input_shapes.emplace_back(tensor.GetShape());
          info.input_elem_types.emplace_back(
              static_cast<int>(tensor.GetElementType()));
        } else {
          std::ostringstream oss;
          oss << "onnx_type=" << static_cast<int>(onnx_type);
          info.input_descriptions.emplace_back(oss.str());
          info.input_shapes.emplace_back(std::vector<int64_t>{});
          info.input_elem_types.emplace_back(
              static_cast<int>(ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED));
        }
      } catch (const Ort::Exception &e) {
        info.input_descriptions.emplace_back(std::string("type_info_error: ") +
                                             e.what());
        info.input_shapes.emplace_back(std::vector<int64_t>{});
        info.input_elem_types.emplace_back(
            static_cast<int>(ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED));
      }
    }

    const std::size_t out_count = session->GetOutputCount();
    info.output_names.reserve(out_count);
    info.output_descriptions.reserve(out_count);
    info.output_shapes.reserve(out_count);
    info.output_elem_types.reserve(out_count);
    for (std::size_t i = 0; i < out_count; ++i) {
      auto name = session->GetOutputNameAllocated(i, alloc);
      info.output_names.emplace_back(name ? name.get() : "");

      try {
        const auto ti = session->GetOutputTypeInfo(i);
        const auto onnx_type = ti.GetONNXType();
        if (onnx_type == ONNX_TYPE_TENSOR) {
          const auto tensor = ti.GetTensorTypeAndShapeInfo();
          info.output_descriptions.emplace_back(TensorDesc(tensor));
          info.output_shapes.emplace_back(tensor.GetShape());
          info.output_elem_types.emplace_back(
              static_cast<int>(tensor.GetElementType()));
        } else {
          std::ostringstream oss;
          oss << "onnx_type=" << static_cast<int>(onnx_type);
          info.output_descriptions.emplace_back(oss.str());
          info.output_shapes.emplace_back(std::vector<int64_t>{});
          info.output_elem_types.emplace_back(
              static_cast<int>(ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED));
        }
      } catch (const Ort::Exception &e) {
        info.output_descriptions.emplace_back(std::string("type_info_error: ") +
                                              e.what());
        info.output_shapes.emplace_back(std::vector<int64_t>{});
        info.output_elem_types.emplace_back(
            static_cast<int>(ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED));
      }
    }

    auto impl = std::make_unique<Impl>();
    impl->info = info;
    impl->opts = opts;
    impl->model_path = model_path;
    impl->session = std::move(session);

    if (info_out) {
      *info_out = impl->info;
    }

    return std::unique_ptr<OrtSession>(new OrtSession(std::move(impl)));

  } catch (const Ort::Exception &e) {
    if (error) {
      *error = HumanizeOrtError(e.what(), model_path);
    }
    return nullptr;
  } catch (const std::exception &e) {
    if (error) {
      *error =
          std::string("Failed to create ONNX Runtime session: ") + e.what();
    }
    return nullptr;
  }
#endif
}

bool OrtSession::RunCpu(const RunInput *inputs, std::size_t input_count,
                        const RunOutput *outputs, std::size_t output_count,
                        std::string *error) {
  if (error)
    error->clear();

#if !STUDIOCAST_HAVE_ONNXRUNTIME
  (void)inputs;
  (void)input_count;
  (void)outputs;
  (void)output_count;
  if (error) {
    *error = "ONNX Runtime is not available in this build "
             "(STUDIOCAST_HAVE_ONNXRUNTIME=0).";
  }
  return false;
#else
  if (!impl_ || impl_->latched_failure) {
    if (error)
      *error = impl_ ? impl_->latched_error : "ORT session is not initialized.";
    return false;
  }
  if (!impl_->session) {
    if (error)
      *error = "ORT session is not initialized.";
    return false;
  }

  if (!inputs || !outputs) {
    if (error)
      *error = "null inputs/outputs passed to ORT Run().";
    return false;
  }
  if (input_count == 0 || output_count == 0) {
    if (error)
      *error = "ORT Run() requires at least one input and one output.";
    return false;
  }

  try {
    static Ort::MemoryInfo mem_info =
        Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeDefault);

    impl_->scratch_input_names.clear();
    impl_->scratch_inputs.clear();
    impl_->scratch_input_names.reserve(input_count);
    impl_->scratch_inputs.reserve(input_count);

    for (std::size_t i = 0; i < input_count; ++i) {
      const auto &in = inputs[i];
      if (!in.name || !*in.name) {
        if (error)
          *error = "ORT Run() input has empty name.";
        return false;
      }
      if (!in.data || in.num_floats == 0) {
        if (error)
          *error = std::string("ORT Run() input '") + in.name +
                   "' has empty buffer.";
        return false;
      }
      if (!in.shape || in.shape_rank == 0) {
        if (error)
          *error =
              std::string("ORT Run() input '") + in.name + "' has empty shape.";
        return false;
      }

      impl_->scratch_input_names.push_back(in.name);
      impl_->scratch_inputs.emplace_back(Ort::Value::CreateTensor<float>(
          mem_info, const_cast<float *>(in.data), in.num_floats, in.shape,
          in.shape_rank));
    }

    impl_->scratch_output_names.clear();
    impl_->scratch_outputs.clear();
    impl_->scratch_output_names.reserve(output_count);
    impl_->scratch_outputs.reserve(output_count);

    for (std::size_t i = 0; i < output_count; ++i) {
      const auto &o = outputs[i];
      if (!o.name || !*o.name) {
        if (error)
          *error = "ORT Run() output has empty name.";
        return false;
      }
      if (!o.data || o.num_floats == 0) {
        if (error)
          *error = std::string("ORT Run() output '") + o.name +
                   "' has empty buffer.";
        return false;
      }
      if (!o.shape || o.shape_rank == 0) {
        if (error)
          *error =
              std::string("ORT Run() output '") + o.name + "' has empty shape.";
        return false;
      }

      impl_->scratch_output_names.push_back(o.name);
      impl_->scratch_outputs.emplace_back(Ort::Value::CreateTensor<float>(
          mem_info, o.data, o.num_floats, o.shape, o.shape_rank));
    }

    impl_->session->Run(Ort::RunOptions{nullptr},
                        impl_->scratch_input_names.data(),
                        impl_->scratch_inputs.data(), input_count,
                        impl_->scratch_output_names.data(),
                        impl_->scratch_outputs.data(), output_count);
    return true;

  } catch (const Ort::Exception &e) {
    const std::string msg = e.what();
    const std::string human = HumanizeOrtError(msg, impl_->model_path);

    if (OrtErrorLooksLikeVramOom(msg)) {
      impl_->LatchFailure(human);
    }

    if (error) {
      *error = std::string("ORT Run() failed: ") + human;
    }
    return false;
  } catch (const std::exception &e) {
    if (error) {
      *error = std::string("ORT Run() failed: ") + e.what();
    }
    return false;
  }
#endif
}

bool OrtSession::RunCudaIoBinding(const CudaBindingInput *inputs,
                                  std::size_t input_count,
                                  const CudaBindingOutput *outputs,
                                  std::size_t output_count,
                                  std::string *error) {
  if (error)
    error->clear();

#if !STUDIOCAST_HAVE_ONNXRUNTIME
  (void)inputs;
  (void)input_count;
  (void)outputs;
  (void)output_count;
  if (error) {
    *error = "ONNX Runtime is not available in this build "
             "(STUDIOCAST_HAVE_ONNXRUNTIME=0).";
  }
  return false;
#else
  if (!impl_ || impl_->latched_failure) {
    if (error)
      *error = impl_ ? impl_->latched_error : "ORT session is not initialized.";
    return false;
  }
  if (!impl_->session) {
    if (error)
      *error = "ORT session is not initialized.";
    return false;
  }
  if (!impl_->info.using_cuda && !impl_->info.using_tensorrt) {
    if (error)
      *error = "ORT session is not using a CUDA-capable EP.";
    return false;
  }
  if (!inputs || !outputs) {
    if (error)
      *error = "null inputs/outputs passed to ORT RunCudaIoBinding().";
    return false;
  }
  if (input_count == 0 || output_count == 0) {
    if (error)
      *error =
          "ORT RunCudaIoBinding() requires at least one input and one output.";
    return false;
  }

  try {
    if (!impl_->binding) {
      impl_->binding = std::make_unique<Ort::IoBinding>(*impl_->session);
    }
    if (!impl_->cuda_mem_info.has_value()) {
      impl_->cuda_mem_info.emplace("Cuda", OrtDeviceAllocator,
                                   impl_->opts.cuda_device_id,
                                   OrtMemTypeDefault);
    }

    impl_->scratch_inputs.clear();
    impl_->scratch_outputs.clear();
    impl_->scratch_inputs.reserve(input_count);
    impl_->scratch_outputs.reserve(output_count);

    for (std::size_t i = 0; i < input_count; ++i) {
      const auto &in = inputs[i];
      if (!in.name || !*in.name) {
        if (error)
          *error = "ORT RunCudaIoBinding() input has empty name.";
        return false;
      }
      if (!in.device_ptr || in.num_floats == 0) {
        if (error)
          *error = std::string("ORT RunCudaIoBinding() input '") + in.name +
                   "' has empty buffer.";
        return false;
      }
      if (!in.shape || in.shape_rank == 0) {
        if (error)
          *error = std::string("ORT RunCudaIoBinding() input '") + in.name +
                   "' has empty shape.";
        return false;
      }

      impl_->scratch_inputs.emplace_back(Ort::Value::CreateTensor<float>(
          *impl_->cuda_mem_info, const_cast<float *>(in.device_ptr),
          in.num_floats, in.shape, in.shape_rank));
    }

    for (std::size_t i = 0; i < output_count; ++i) {
      const auto &out = outputs[i];
      if (!out.name || !*out.name) {
        if (error)
          *error = "ORT RunCudaIoBinding() output has empty name.";
        return false;
      }
      if (!out.device_ptr || out.num_floats == 0) {
        if (error)
          *error = std::string("ORT RunCudaIoBinding() output '") + out.name +
                   "' has empty buffer.";
        return false;
      }
      if (!out.shape || out.shape_rank == 0) {
        if (error)
          *error = std::string("ORT RunCudaIoBinding() output '") + out.name +
                   "' has empty shape.";
        return false;
      }

      impl_->scratch_outputs.emplace_back(Ort::Value::CreateTensor<float>(
          *impl_->cuda_mem_info, out.device_ptr, out.num_floats, out.shape,
          out.shape_rank));
    }

    impl_->binding->ClearBoundInputs();
    impl_->binding->ClearBoundOutputs();

    for (std::size_t i = 0; i < input_count; ++i) {
      impl_->binding->BindInput(inputs[i].name, impl_->scratch_inputs[i]);
    }
    for (std::size_t i = 0; i < output_count; ++i) {
      impl_->binding->BindOutput(outputs[i].name, impl_->scratch_outputs[i]);
    }

    impl_->session->Run(Ort::RunOptions{nullptr}, *impl_->binding);

    if (impl_->info.cuda_needs_stream_sync) {
      // Ensure outputs are ready before downstream consumers access the GPU
      // buffers.
      impl_->binding->SynchronizeOutputs();
    }

    return true;

  } catch (const Ort::Exception &e) {
    const std::string msg = e.what();
    const std::string human = HumanizeOrtError(msg, impl_->model_path);

    if (OrtErrorLooksLikeVramOom(msg)) {
      impl_->LatchFailure(human);
    }

    if (error) {
      *error = human;
    }
    return false;
  } catch (const std::exception &e) {
    if (error) {
      *error = std::string("ORT RunCudaIoBinding() failed: ") + e.what();
    }
    return false;
  }
#endif
}

} // namespace studiocast::onnx
