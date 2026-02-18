#include "core/open_video/open_video_onnx_session.h"

#include <algorithm>
#include <sstream>
#include <type_traits>
#include <utility>
#include <vector>

#if STUDIOCAST_HAVE_ONNXRUNTIME
#include <onnxruntime_c_api.h>
#include <onnxruntime_cxx_api.h>
#endif

namespace studiocast::open_video {

struct OpenVideoOrtSession::Impl {
  OrtSessionInfo info;

#if STUDIOCAST_HAVE_ONNXRUNTIME
  std::unique_ptr<Ort::Session> session;

  // Scratch space to avoid per-frame heap churn.
  std::vector<const char*> scratch_input_names;
  std::vector<const char*> scratch_output_names;
  std::vector<Ort::Value> scratch_inputs;
  std::vector<Ort::Value> scratch_outputs;
#endif
};

OpenVideoOrtSession::OpenVideoOrtSession(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
OpenVideoOrtSession::~OpenVideoOrtSession() = default;

const OrtSessionInfo& OpenVideoOrtSession::info() const { return impl_->info; }

OrtRuntimeInfo OpenVideoOrtSession::QueryRuntimeInfo() {
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
  static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "studiocast_open_video");
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

bool TryAppendCudaEp(Ort::SessionOptions* so, int device_id, std::string* warn) {
  if (warn) warn->clear();

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
    const std::string dev = std::to_string(device_id);
    const char* values[] = {dev.c_str()};
    Ort::ThrowOnError(api.UpdateCUDAProviderOptions(cuda_opts_v2, keys, values, 1));

    Ort::ThrowOnError(api.SessionOptionsAppendExecutionProvider_CUDA_V2(*so, cuda_opts_v2));
    return true;
#else
    OrtCUDAProviderOptions cuda_opts{};
    cuda_opts.device_id = device_id;
    so->AppendExecutionProvider_CUDA(cuda_opts);
    return true;
#endif
  } catch (const Ort::Exception& e) {
    if (warn) *warn = e.what();
    return false;
  }
}

}  // namespace
#endif

std::unique_ptr<OpenVideoOrtSession> OpenVideoOrtSession::Create(const std::filesystem::path& model_path,
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
    std::string cuda_warn;
    if (opts.prefer_cuda) {
      using_cuda = TryAppendCudaEp(&so, opts.cuda_device_id, &cuda_warn);
    }

    auto session = std::make_unique<Ort::Session>(GlobalEnv(), model.c_str(), so);

    OrtSessionInfo info;
    info.using_cuda = using_cuda;
    if (!cuda_warn.empty()) {
      info.warnings.push_back(std::string("CUDA EP unavailable: ") + cuda_warn);
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
    impl->session = std::move(session);

    if (info_out) *info_out = impl->info;
    if (info_out && !impl->info.warnings.empty()) {
      // Expose warnings to caller.
    }

    return std::unique_ptr<OpenVideoOrtSession>(new OpenVideoOrtSession(std::move(impl)));

  } catch (const Ort::Exception& e) {
    if (error) {
      *error = std::string("OpenVideoOrtSession: ") + e.what();
    }
    return nullptr;
  }
#endif
}

bool OpenVideoOrtSession::Run(const OrtRunInput* inputs,
                             std::size_t input_count,
                             const OrtRunOutput* outputs,
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
  if (!impl_ || !impl_->session) {
    if (error) *error = "ORT session is not initialized";
    return false;
  }
  if (!inputs || !outputs) {
    if (error) *error = "inputs/outputs are null";
    return false;
  }

  try {
    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeCPU);

    impl_->scratch_input_names.clear();
    impl_->scratch_output_names.clear();
    impl_->scratch_inputs.clear();
    impl_->scratch_outputs.clear();

    impl_->scratch_input_names.reserve(input_count);
    impl_->scratch_inputs.reserve(input_count);
    for (std::size_t i = 0; i < input_count; ++i) {
      const auto& in = inputs[i];
      if (!in.name || !in.data || !in.shape || in.shape_rank == 0) {
        if (error) *error = "invalid input binding";
        return false;
      }
      impl_->scratch_input_names.push_back(in.name);
      std::vector<int64_t> shape(in.shape, in.shape + in.shape_rank);
      impl_->scratch_inputs.emplace_back(Ort::Value::CreateTensor<float>(mem,
                                                                        const_cast<float*>(in.data),
                                                                        in.num_floats,
                                                                        shape.data(),
                                                                        shape.size()));
    }

    impl_->scratch_output_names.reserve(output_count);
    impl_->scratch_outputs.reserve(output_count);
    for (std::size_t i = 0; i < output_count; ++i) {
      const auto& out = outputs[i];
      if (!out.name || !out.data || !out.shape || out.shape_rank == 0) {
        if (error) *error = "invalid output binding";
        return false;
      }
      impl_->scratch_output_names.push_back(out.name);
      std::vector<int64_t> shape(out.shape, out.shape + out.shape_rank);
      impl_->scratch_outputs.emplace_back(Ort::Value::CreateTensor<float>(mem,
                                                                         out.data,
                                                                         out.num_floats,
                                                                         shape.data(),
                                                                         shape.size()));
    }

    Ort::RunOptions ro;
    impl_->session->Run(ro,
                        impl_->scratch_input_names.data(),
                        impl_->scratch_inputs.data(),
                        impl_->scratch_inputs.size(),
                        impl_->scratch_output_names.data(),
                        impl_->scratch_outputs.data(),
                        impl_->scratch_outputs.size());
    return true;

  } catch (const Ort::Exception& e) {
    if (error) {
      *error = std::string("OpenVideoOrtSession::Run: ") + e.what();
    }
    return false;
  }
#endif
}

}  // namespace studiocast::open_video
