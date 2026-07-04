#include "core/open_audio/open_audio_onnx_session.h"

#include <algorithm>
#include <sstream>
#include <utility>
#include <vector>

#include "core/onnx/ort_session.h"

#if STUDIOCAST_HAVE_ONNXRUNTIME
#include <onnxruntime_c_api.h>
#include <onnxruntime_cxx_api.h>
#endif

namespace studiocast::open_audio {

struct OpenAudioOrtSession::Impl {
  OrtSessionInfo info;

#if STUDIOCAST_HAVE_ONNXRUNTIME
  std::unique_ptr<Ort::Session> session;

  // Scratch space to avoid per-frame heap churn in real-time processing.
  std::vector<const char *> scratch_input_names;
  std::vector<const char *> scratch_output_names;
  std::vector<Ort::Value> scratch_inputs;
  std::vector<Ort::Value> scratch_outputs;
#endif
};

OpenAudioOrtSession::OpenAudioOrtSession(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
OpenAudioOrtSession::~OpenAudioOrtSession() = default;

const OrtSessionInfo &OpenAudioOrtSession::info() const { return impl_->info; }

OrtRuntimeInfo OpenAudioOrtSession::QueryRuntimeInfo() {
  OrtRuntimeInfo out;
  const auto shared = studiocast::onnx::OrtSession::QueryRuntimeInfo();
  out.version = shared.version;
  out.providers = shared.providers;
  out.cuda_provider_present = shared.cuda_provider_present;
  out.tensorrt_provider_present = shared.tensorrt_provider_present;
  out.cpu_provider_present = shared.cpu_provider_present;
  out.cuda_ep_v2_build = shared.cuda_ep_v2_build;
  out.library_path = shared.library_path;
  return out;
}

#if STUDIOCAST_HAVE_ONNXRUNTIME
namespace {

Ort::Env &GlobalEnv() {
  // NOTE: ORT env is process-global and should be long-lived.
  static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "studiocast_open_audio");
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

bool TryAppendCudaEp(Ort::SessionOptions *so, int device_id,
                     std::string *warn) {
  if (warn)
    warn->clear();

  try {
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
    const std::string dev = std::to_string(device_id);
    const char *values[] = {dev.c_str()};
    Ort::ThrowOnError(
        api.UpdateCUDAProviderOptions(cuda_opts_v2, keys, values, 1));

    Ort::ThrowOnError(
        api.SessionOptionsAppendExecutionProvider_CUDA_V2(*so, cuda_opts_v2));
    return true;
#else
    OrtCUDAProviderOptions cuda_opts{};
    cuda_opts.device_id = device_id;
    so->AppendExecutionProvider_CUDA(cuda_opts);
    return true;
#endif
  } catch (const Ort::Exception &e) {
    if (warn)
      *warn = e.what();
    return false;
  }
}

} // namespace
#endif

std::unique_ptr<OpenAudioOrtSession>
OpenAudioOrtSession::Create(const std::filesystem::path &model_path,
                            const OrtSessionOptions &opts,
                            OrtSessionInfo *info_out, std::string *error) {
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

    Ort::SessionOptions so;
    so.SetIntraOpNumThreads(1);
    so.SetInterOpNumThreads(1);
    so.SetGraphOptimizationLevel(ORT_ENABLE_EXTENDED);

    bool using_cuda = false;
    std::string cuda_warn;
    if (opts.prefer_cuda) {
      using_cuda = TryAppendCudaEp(&so, opts.cuda_device_id, &cuda_warn);
    }

    auto session =
        std::make_unique<Ort::Session>(GlobalEnv(), model.c_str(), so);

    OrtSessionInfo info;
    info.using_cuda = using_cuda;

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
    if (!cuda_warn.empty() && !using_cuda) {
      info.warnings.push_back(std::string("cuda_ep_unavailable: ") + cuda_warn);
    }

    auto impl = std::make_unique<Impl>();
    impl->session = std::move(session);
    impl->info = info;

    if (info_out) {
      *info_out = impl->info;
    }

    return std::unique_ptr<OpenAudioOrtSession>(
        new OpenAudioOrtSession(std::move(impl)));
  } catch (const Ort::Exception &e) {
    if (error) {
      *error =
          std::string("Failed to create ONNX Runtime session: ") + e.what();
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

bool OpenAudioOrtSession::Run(const OrtRunInput *inputs,
                              std::size_t input_count,
                              const OrtRunOutput *outputs,
                              std::size_t output_count, std::string *error) {
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
  if (!impl_ || !impl_->session) {
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
    if (error) {
      *error = std::string("ORT Run() failed: ") + e.what();
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

bool OpenAudioOrtSession::Run1D(const float *input, std::size_t samples,
                                float *output, std::size_t output_capacity,
                                std::size_t *output_samples,
                                std::string *error) {
  if (error)
    error->clear();
  if (output_samples)
    *output_samples = 0;

#if !STUDIOCAST_HAVE_ONNXRUNTIME
  (void)input;
  (void)samples;
  (void)output;
  (void)output_capacity;
  if (error) {
    *error = "ONNX Runtime is not available in this build "
             "(STUDIOCAST_HAVE_ONNXRUNTIME=0).";
  }
  return false;
#else
  if (!impl_ || !impl_->session) {
    if (error)
      *error = "ORT session is not initialized.";
    return false;
  }
  if (!input || !output) {
    if (error)
      *error = "null buffer passed to Run1D";
    return false;
  }
  if (samples == 0)
    return true;

  if (impl_->info.input_names.empty() || impl_->info.output_names.empty() ||
      impl_->info.input_names[0].empty() ||
      impl_->info.output_names[0].empty()) {
    if (error)
      *error = "ORT session does not expose input/output names for Run1D.";
    return false;
  }

  try {
    static Ort::MemoryInfo mem_info =
        Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeDefault);

    std::vector<int64_t> shape{1, static_cast<int64_t>(samples)};
    Ort::Value input_tensor =
        Ort::Value::CreateTensor<float>(mem_info, const_cast<float *>(input),
                                        samples, shape.data(), shape.size());

    const char *input_name = impl_->info.input_names[0].c_str();
    const char *output_name = impl_->info.output_names[0].c_str();
    const char *input_names[] = {input_name};
    const char *output_names[] = {output_name};

    auto outputs = impl_->session->Run(Ort::RunOptions{nullptr}, input_names,
                                       &input_tensor, 1, output_names, 1);
    if (outputs.empty() || !outputs[0].IsTensor()) {
      if (error)
        *error = "ORT Run() returned no tensor outputs.";
      return false;
    }

    auto &out_val = outputs[0];
    auto ti = out_val.GetTensorTypeAndShapeInfo();
    const auto out_shape = ti.GetShape();
    std::size_t out_len = 1;
    for (auto d : out_shape) {
      if (d > 0)
        out_len *= static_cast<std::size_t>(d);
    }

    float *out_data = out_val.GetTensorMutableData<float>();
    const std::size_t to_copy = std::min(out_len, output_capacity);
    std::copy_n(out_data, to_copy, output);
    if (output_samples)
      *output_samples = to_copy;
    return true;
  } catch (const Ort::Exception &e) {
    if (error) {
      *error = std::string("ORT Run() failed: ") + e.what();
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

} // namespace studiocast::open_audio
