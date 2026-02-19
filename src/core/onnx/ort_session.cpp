#include "core/onnx/ort_session.h"

#include <algorithm>
#include <sstream>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#if STUDIOCAST_HAVE_ONNXRUNTIME
#include <onnxruntime_c_api.h>
#include <onnxruntime_cxx_api.h>
#endif

namespace studiocast::onnx {

bool OrtErrorLooksLikeVramOom(const std::string& ort_msg) {
  // Common failure strings seen from ORT CUDA EP when VRAM is exhausted or cuDNN can't
  // find a viable algorithm/workspace due to memory pressure.
  return ort_msg.find("CUDA failure 2") != std::string::npos ||
         ort_msg.find("out of memory") != std::string::npos ||
         ort_msg.find("cudaErrorMemoryAllocation") != std::string::npos ||
         ort_msg.find("CUDNN_STATUS_ALLOC_FAILED") != std::string::npos ||
         // Often appears after OOM / workspace allocation failures when cuDNN can't run the chosen kernel.
         ort_msg.find("CUDNN_STATUS_NOT_SUPPORTED") != std::string::npos;
}

std::string HumanizeOrtError(const std::string& ort_msg, const std::filesystem::path& model_path) {
  if (OrtErrorLooksLikeVramOom(ort_msg)) {
    std::string out = "GPU is likely out of VRAM for this model";
    if (!model_path.empty()) {
      out += " (" + model_path.filename().string() + ")";
    }
    out +=
        ". The model is probably too large for the available GPU memory. "
        "Try a smaller model, lower input resolution, or close other GPU-heavy apps. "
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

#if STUDIOCAST_HAVE_ONNXRUNTIME
  const char* v = OrtGetApiBase()->GetVersionString();
  if (v) {
    out.version = v;
  }

  try {
    auto& api = Ort::GetApi();
    char** providers = nullptr;
    int num = 0;
    Ort::ThrowOnError(api.GetAvailableProviders(&providers, &num));
    for (int i = 0; i < num; ++i) {
      if (providers && providers[i]) {
        out.providers.emplace_back(providers[i]);
      }
    }

    // ORT changed this API from `void` to returning `OrtStatus*` (warn_unused_result).
    if constexpr (std::is_void_v<decltype(api.ReleaseAvailableProviders(providers, num))>) {
      api.ReleaseAvailableProviders(providers, num);
    } else {
      Ort::ThrowOnError(api.ReleaseAvailableProviders(providers, num));
    }
  } catch (const Ort::Exception&) {
    // Best-effort only.
  }
#endif

  return out;
}

#if STUDIOCAST_HAVE_ONNXRUNTIME
namespace {

Ort::Env& GlobalEnv() {
  // NOTE: ORT env is process-global and should be long-lived.
  static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "studiocast_onnx");
  return env;
}

const char* ElemTypeToString(ONNXTensorElementDataType t) {
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

std::string ShapeToString(const std::vector<int64_t>& shape) {
  std::ostringstream oss;
  oss << "[";
  for (std::size_t i = 0; i < shape.size(); ++i) {
    if (i) oss << ", ";
    oss << shape[i];
  }
  oss << "]";
  return oss.str();
}

template <typename TensorTypeAndShapeInfo>
std::string TensorDesc(const TensorTypeAndShapeInfo& ti) {
  std::ostringstream oss;
  const auto type = ti.GetElementType();
  oss << "tensor(" << ElemTypeToString(type) << ")";

  const auto shape = ti.GetShape();
  if (!shape.empty()) {
    oss << " shape=" << ShapeToString(shape);
  }
  return oss.str();
}

bool TryAppendCudaEp(Ort::SessionOptions* so,
                     const OrtSessionOptions& opts,
                     bool* needs_stream_sync,
                     std::string* warn) {
  if (warn) warn->clear();
  if (needs_stream_sync) *needs_stream_sync = false;

  try {
#if STUDIOCAST_ORT_HAS_CUDA_EP_V2
    const auto& api = Ort::GetApi();

    OrtCUDAProviderOptionsV2* cuda_opts_v2 = nullptr;
    Ort::ThrowOnError(api.CreateCUDAProviderOptions(&cuda_opts_v2));
    struct Guard {
      const OrtApi* api = nullptr;
      OrtCUDAProviderOptionsV2* opts = nullptr;
      ~Guard() {
        if (api && opts) {
          api->ReleaseCUDAProviderOptions(opts);
        }
      }
    } guard{&api, cuda_opts_v2};

    const char* keys[] = {"device_id"};
    const std::string dev = std::to_string(opts.cuda_device_id);
    const char* values[] = {dev.c_str()};
    Ort::ThrowOnError(api.UpdateCUDAProviderOptions(cuda_opts_v2, keys, values, 1));

    if (opts.user_compute_stream != nullptr) {
      Ort::ThrowOnError(api.UpdateCUDAProviderOptionsWithValue(cuda_opts_v2,
                                                               "user_compute_stream",
                                                               reinterpret_cast<void*>(opts.user_compute_stream)));
      if (needs_stream_sync) *needs_stream_sync = false;
    } else {
      // Without user_compute_stream, ORT may use internal streams.
      if (needs_stream_sync) *needs_stream_sync = true;
    }

    Ort::ThrowOnError(api.SessionOptionsAppendExecutionProvider_CUDA_V2(*so, cuda_opts_v2));
    return true;
#else
    OrtCUDAProviderOptions cuda_opts{};
    cuda_opts.device_id = opts.cuda_device_id;
    so->AppendExecutionProvider_CUDA(cuda_opts);

    // Legacy CUDA EP does not expose stream interop.
    if (needs_stream_sync) *needs_stream_sync = true;
    return true;
#endif
  } catch (const Ort::Exception& e) {
    if (warn) *warn = e.what();
    if (needs_stream_sync) *needs_stream_sync = false;
    return false;
  }
}

}  // namespace
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
  std::vector<const char*> scratch_input_names;
  std::vector<const char*> scratch_output_names;
  std::vector<Ort::Value> scratch_inputs;
  std::vector<Ort::Value> scratch_outputs;
#endif

  void LatchFailure(const std::string& err) {
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

const OrtSessionInfo& OrtSession::info() const { return impl_->info; }

bool OrtSession::HasLatchedFailure() const { return impl_ ? impl_->latched_failure : false; }

const std::string& OrtSession::LatchedError() const {
  static const std::string empty;
  return impl_ ? impl_->latched_error : empty;
}

std::unique_ptr<OrtSession> OrtSession::Create(const std::filesystem::path& model_path,
                                               const OrtSessionOptions& opts,
                                               OrtSessionInfo* info_out,
                                               std::string* error) {
  if (error) error->clear();
  if (info_out) *info_out = OrtSessionInfo{};

#if !STUDIOCAST_HAVE_ONNXRUNTIME
  (void)model_path;
  (void)opts;
  if (error) {
    *error = "ONNX Runtime is not available in this build (STUDIOCAST_HAVE_ONNXRUNTIME=0).";
  }
  return nullptr;
#else
  try {
    const std::string model = model_path.string();
    if (model.empty()) {
      if (error) *error = "model_path is empty";
      return nullptr;
    }

    Ort::SessionOptions so;
    so.SetIntraOpNumThreads(1);
    so.SetInterOpNumThreads(1);
    so.SetGraphOptimizationLevel(ORT_ENABLE_EXTENDED);

    bool using_cuda = false;
    bool cuda_needs_sync = false;
    std::string cuda_warn;
    if (opts.prefer_cuda) {
      using_cuda = TryAppendCudaEp(&so, opts, &cuda_needs_sync, &cuda_warn);
    }

    auto session = std::make_unique<Ort::Session>(GlobalEnv(), model.c_str(), so);

    OrtSessionInfo info;
    info.using_cuda = using_cuda;
    info.cuda_needs_stream_sync = using_cuda && cuda_needs_sync;
    if (!cuda_warn.empty() && !using_cuda && opts.prefer_cuda) {
      info.warnings.push_back(std::string("cuda_ep_unavailable: ") + cuda_warn);
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
          info.input_elem_types.emplace_back(static_cast<int>(tensor.GetElementType()));
        } else {
          std::ostringstream oss;
          oss << "onnx_type=" << static_cast<int>(onnx_type);
          info.input_descriptions.emplace_back(oss.str());
          info.input_shapes.emplace_back(std::vector<int64_t>{});
          info.input_elem_types.emplace_back(static_cast<int>(ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED));
        }
      } catch (const Ort::Exception& e) {
        info.input_descriptions.emplace_back(std::string("type_info_error: ") + e.what());
        info.input_shapes.emplace_back(std::vector<int64_t>{});
        info.input_elem_types.emplace_back(static_cast<int>(ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED));
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
          info.output_elem_types.emplace_back(static_cast<int>(tensor.GetElementType()));
        } else {
          std::ostringstream oss;
          oss << "onnx_type=" << static_cast<int>(onnx_type);
          info.output_descriptions.emplace_back(oss.str());
          info.output_shapes.emplace_back(std::vector<int64_t>{});
          info.output_elem_types.emplace_back(static_cast<int>(ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED));
        }
      } catch (const Ort::Exception& e) {
        info.output_descriptions.emplace_back(std::string("type_info_error: ") + e.what());
        info.output_shapes.emplace_back(std::vector<int64_t>{});
        info.output_elem_types.emplace_back(static_cast<int>(ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED));
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

  } catch (const Ort::Exception& e) {
    if (error) {
      *error = HumanizeOrtError(e.what(), model_path);
    }
    return nullptr;
  } catch (const std::exception& e) {
    if (error) {
      *error = std::string("Failed to create ONNX Runtime session: ") + e.what();
    }
    return nullptr;
  }
#endif
}

bool OrtSession::RunCpu(const RunInput* inputs,
                        std::size_t input_count,
                        const RunOutput* outputs,
                        std::size_t output_count,
                        std::string* error) {
  if (error) error->clear();

#if !STUDIOCAST_HAVE_ONNXRUNTIME
  (void)inputs;
  (void)input_count;
  (void)outputs;
  (void)output_count;
  if (error) {
    *error = "ONNX Runtime is not available in this build (STUDIOCAST_HAVE_ONNXRUNTIME=0).";
  }
  return false;
#else
  if (!impl_ || impl_->latched_failure) {
    if (error) *error = impl_ ? impl_->latched_error : "ORT session is not initialized.";
    return false;
  }
  if (!impl_->session) {
    if (error) *error = "ORT session is not initialized.";
    return false;
  }

  if (!inputs || !outputs) {
    if (error) *error = "null inputs/outputs passed to ORT Run().";
    return false;
  }
  if (input_count == 0 || output_count == 0) {
    if (error) *error = "ORT Run() requires at least one input and one output.";
    return false;
  }

  try {
    static Ort::MemoryInfo mem_info = Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeDefault);

    impl_->scratch_input_names.clear();
    impl_->scratch_inputs.clear();
    impl_->scratch_input_names.reserve(input_count);
    impl_->scratch_inputs.reserve(input_count);

    for (std::size_t i = 0; i < input_count; ++i) {
      const auto& in = inputs[i];
      if (!in.name || !*in.name) {
        if (error) *error = "ORT Run() input has empty name.";
        return false;
      }
      if (!in.data || in.num_floats == 0) {
        if (error) *error = std::string("ORT Run() input '") + in.name + "' has empty buffer.";
        return false;
      }
      if (!in.shape || in.shape_rank == 0) {
        if (error) *error = std::string("ORT Run() input '") + in.name + "' has empty shape.";
        return false;
      }

      impl_->scratch_input_names.push_back(in.name);
      impl_->scratch_inputs.emplace_back(Ort::Value::CreateTensor<float>(
          mem_info, const_cast<float*>(in.data), in.num_floats, in.shape, in.shape_rank));
    }

    impl_->scratch_output_names.clear();
    impl_->scratch_outputs.clear();
    impl_->scratch_output_names.reserve(output_count);
    impl_->scratch_outputs.reserve(output_count);

    for (std::size_t i = 0; i < output_count; ++i) {
      const auto& o = outputs[i];
      if (!o.name || !*o.name) {
        if (error) *error = "ORT Run() output has empty name.";
        return false;
      }
      if (!o.data || o.num_floats == 0) {
        if (error) *error = std::string("ORT Run() output '") + o.name + "' has empty buffer.";
        return false;
      }
      if (!o.shape || o.shape_rank == 0) {
        if (error) *error = std::string("ORT Run() output '") + o.name + "' has empty shape.";
        return false;
      }

      impl_->scratch_output_names.push_back(o.name);
      impl_->scratch_outputs.emplace_back(Ort::Value::CreateTensor<float>(
          mem_info, o.data, o.num_floats, o.shape, o.shape_rank));
    }

    impl_->session->Run(Ort::RunOptions{nullptr},
                        impl_->scratch_input_names.data(),
                        impl_->scratch_inputs.data(),
                        input_count,
                        impl_->scratch_output_names.data(),
                        impl_->scratch_outputs.data(),
                        output_count);
    return true;

  } catch (const Ort::Exception& e) {
    const std::string msg = e.what();
    const std::string human = HumanizeOrtError(msg, impl_->model_path);

    if (OrtErrorLooksLikeVramOom(msg)) {
      impl_->LatchFailure(human);
    }

    if (error) {
      *error = std::string("ORT Run() failed: ") + human;
    }
    return false;
  } catch (const std::exception& e) {
    if (error) {
      *error = std::string("ORT Run() failed: ") + e.what();
    }
    return false;
  }
#endif
}

bool OrtSession::RunCudaIoBinding(const CudaBindingInput* inputs,
                                 std::size_t input_count,
                                 const CudaBindingOutput* outputs,
                                 std::size_t output_count,
                                 std::string* error) {
  if (error) error->clear();

#if !STUDIOCAST_HAVE_ONNXRUNTIME
  (void)inputs;
  (void)input_count;
  (void)outputs;
  (void)output_count;
  if (error) {
    *error = "ONNX Runtime is not available in this build (STUDIOCAST_HAVE_ONNXRUNTIME=0).";
  }
  return false;
#else
  if (!impl_ || impl_->latched_failure) {
    if (error) *error = impl_ ? impl_->latched_error : "ORT session is not initialized.";
    return false;
  }
  if (!impl_->session) {
    if (error) *error = "ORT session is not initialized.";
    return false;
  }
  if (!impl_->info.using_cuda) {
    if (error) *error = "ORT session is not using CUDA EP.";
    return false;
  }
  if (!inputs || !outputs) {
    if (error) *error = "null inputs/outputs passed to ORT RunCudaIoBinding().";
    return false;
  }
  if (input_count == 0 || output_count == 0) {
    if (error) *error = "ORT RunCudaIoBinding() requires at least one input and one output.";
    return false;
  }

  try {
    if (!impl_->binding) {
      impl_->binding = std::make_unique<Ort::IoBinding>(*impl_->session);
    }
    if (!impl_->cuda_mem_info.has_value()) {
      impl_->cuda_mem_info.emplace("Cuda", OrtDeviceAllocator, impl_->opts.cuda_device_id, OrtMemTypeDefault);
    }

    impl_->scratch_inputs.clear();
    impl_->scratch_outputs.clear();
    impl_->scratch_inputs.reserve(input_count);
    impl_->scratch_outputs.reserve(output_count);

    for (std::size_t i = 0; i < input_count; ++i) {
      const auto& in = inputs[i];
      if (!in.name || !*in.name) {
        if (error) *error = "ORT RunCudaIoBinding() input has empty name.";
        return false;
      }
      if (!in.device_ptr || in.num_floats == 0) {
        if (error) *error = std::string("ORT RunCudaIoBinding() input '") + in.name + "' has empty buffer.";
        return false;
      }
      if (!in.shape || in.shape_rank == 0) {
        if (error) *error = std::string("ORT RunCudaIoBinding() input '") + in.name + "' has empty shape.";
        return false;
      }

      impl_->scratch_inputs.emplace_back(Ort::Value::CreateTensor<float>(
          *impl_->cuda_mem_info,
          const_cast<float*>(in.device_ptr),
          in.num_floats,
          in.shape,
          in.shape_rank));
    }

    for (std::size_t i = 0; i < output_count; ++i) {
      const auto& out = outputs[i];
      if (!out.name || !*out.name) {
        if (error) *error = "ORT RunCudaIoBinding() output has empty name.";
        return false;
      }
      if (!out.device_ptr || out.num_floats == 0) {
        if (error) *error = std::string("ORT RunCudaIoBinding() output '") + out.name + "' has empty buffer.";
        return false;
      }
      if (!out.shape || out.shape_rank == 0) {
        if (error) *error = std::string("ORT RunCudaIoBinding() output '") + out.name + "' has empty shape.";
        return false;
      }

      impl_->scratch_outputs.emplace_back(Ort::Value::CreateTensor<float>(
          *impl_->cuda_mem_info, out.device_ptr, out.num_floats, out.shape, out.shape_rank));
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
      // Ensure outputs are ready before downstream consumers access the GPU buffers.
      impl_->binding->SynchronizeOutputs();
    }

    return true;

  } catch (const Ort::Exception& e) {
    const std::string msg = e.what();
    const std::string human = HumanizeOrtError(msg, impl_->model_path);

    if (OrtErrorLooksLikeVramOom(msg)) {
      impl_->LatchFailure(human);
    }

    if (error) {
      *error = human;
    }
    return false;
  } catch (const std::exception& e) {
    if (error) {
      *error = std::string("ORT RunCudaIoBinding() failed: ") + e.what();
    }
    return false;
  }
#endif
}

}  // namespace studiocast::onnx
