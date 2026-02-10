#include "core/open_cuda/onnx_session.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/cuda/kernels/preprocess_to_nchw.h"

#if STUDIOCAST_HAVE_ONNXRUNTIME
#include <onnxruntime_cxx_api.h>
#endif

namespace studiocast::open_cuda {

namespace {

bool FileExists(const std::filesystem::path& p) {
  std::error_code ec;
  return std::filesystem::exists(p, ec) && !ec;
}

bool IsFinite3(const std::array<double, 3>& v) {
  return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

}  // namespace

struct OpenCudaMattingSession::Impl {
  studiocast::maxine::CudaDriverApi* cuda = nullptr;
  ModelPack pack;
  Options opts;

  bool validated_pack = false;
  bool buffers_ready = false;

  int last_frame_w = 0;
  int last_frame_h = 0;

  studiocast::cuda::kernels::ModelPreprocessSpec preprocess{};
  studiocast::cuda::CudaTensor input_tensor;

  // The CUDA stream the ORT session was created for (when using user_compute_stream).
  studiocast::maxine::CUstream ort_stream = nullptr;

  bool ort_needs_stream_sync = false;

#if STUDIOCAST_HAVE_ONNXRUNTIME
  std::optional<Ort::Env> env;
  std::unique_ptr<Ort::Session> session;
  std::unique_ptr<Ort::IoBinding> binding;
  std::string input_name;
  std::string output_name;
  std::vector<int64_t> input_shape;
  std::vector<int64_t> output_shape;
  std::optional<Ort::MemoryInfo> cuda_mem_info;
  std::optional<Ort::Value> input_value;

  bool CreateOrtSession(std::string* error_out) {
    if (error_out) error_out->clear();

    try {
      if (!env.has_value()) {
        env.emplace(ORT_LOGGING_LEVEL_WARNING, "studiocast_open_cuda");
      }

      Ort::SessionOptions so;
      so.SetIntraOpNumThreads(1);
      so.SetInterOpNumThreads(1);
      so.SetGraphOptimizationLevel(ORT_ENABLE_EXTENDED);

      if (opts.enable_tensorrt) {
        // Reserved for future work; keeping API stable for config wiring.
        // TensorRT EP availability is packaging-dependent.
      }

      // Configure CUDA EP (legacy provider options). We intentionally avoid the
      // V2 options API here to keep compatibility with a wider range of ORT
      // builds/headers.
      OrtCUDAProviderOptions cuda_opts{};
      cuda_opts.device_id = opts.device_id;

      // If this throws/returns error, it typically means a CPU-only ORT build.
      so.AppendExecutionProvider_CUDA(cuda_opts);

      // Without user_compute_stream, ensure correctness by synchronizing the
      // caller stream before running inference.
      ort_needs_stream_sync = true;
      ort_stream = nullptr;

      session = std::make_unique<Ort::Session>(*env, pack.onnx_path.c_str(), so);
      binding = std::make_unique<Ort::IoBinding>(*session);

      // Validate the model IO names against manifest (actionable errors when mismatched).
      {
        Ort::AllocatorWithDefaultOptions alloc;
        auto model_in = session->GetInputNameAllocated(0, alloc);
        auto model_out = session->GetOutputNameAllocated(0, alloc);
        input_name = model_in.get();
        output_name = model_out.get();

        if (!pack.input.name.empty() && input_name != pack.input.name) {
          if (error_out) {
            *error_out = "Model input name mismatch: model='" + input_name + "' manifest='" + pack.input.name + "'";
          }
          return false;
        }
        if (!pack.output.name.empty() && output_name != pack.output.name) {
          if (error_out) {
            *error_out =
                "Model output name mismatch: model='" + output_name + "' manifest='" + pack.output.name + "'";
          }
          return false;
        }
      }

      // Cache a CUDA memory info instance for tensor creation.
      cuda_mem_info.emplace("Cuda", OrtDeviceAllocator, opts.device_id, OrtMemTypeDefault);

      // Validate model input/output shapes (best-effort; allow dynamic dims).
      {
        const auto in_info = session->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo();
        const auto out_info = session->GetOutputTypeInfo(0).GetTensorTypeAndShapeInfo();

        if (in_info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
          if (error_out) *error_out = "Model input dtype must be float32.";
          return false;
        }
        if (out_info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
          if (error_out) *error_out = "Model output dtype must be float32.";
          return false;
        }

        input_shape = in_info.GetShape();
        output_shape = out_info.GetShape();
        if (input_shape.size() != 4) {
          if (error_out) *error_out = "Model input must be rank-4 (NCHW).";
          return false;
        }
        if (output_shape.size() != 4) {
          if (error_out) *error_out = "Model output must be rank-4.";
          return false;
        }

        const int64_t want_n = 1;
        const int64_t want_c = 3;
        const int64_t want_h = pack.input.height;
        const int64_t want_w = pack.input.width;

        auto matches_or_dynamic = [](int64_t got, int64_t want) { return got == want || got == -1; };
        if (!matches_or_dynamic(input_shape[0], want_n) || !matches_or_dynamic(input_shape[1], want_c) ||
            !matches_or_dynamic(input_shape[2], want_h) || !matches_or_dynamic(input_shape[3], want_w)) {
          if (error_out) {
            *error_out = "Model input shape does not match manifest (expected 1x3x" + std::to_string(want_h) + "x" +
                         std::to_string(want_w) + ")";
          }
          return false;
        }

        // Normalize dynamic dims to concrete shapes for tensor creation.
        input_shape = {1, 3, want_h, want_w};

        // Expected alpha shape: 1x1xH xW.
        const int64_t want_out_n = 1;
        const int64_t want_out_c = 1;
        const int64_t want_out_h = pack.input.height;
        const int64_t want_out_w = pack.input.width;
        if (!matches_or_dynamic(output_shape[0], want_out_n) || !matches_or_dynamic(output_shape[1], want_out_c) ||
            !matches_or_dynamic(output_shape[2], want_out_h) || !matches_or_dynamic(output_shape[3], want_out_w)) {
          if (error_out) {
            *error_out = "Model output shape does not match expected alpha tensor (expected 1x1x" +
                         std::to_string(want_out_h) + "x" + std::to_string(want_out_w) + ")";
          }
          return false;
        }
        output_shape = {1, 1, want_out_h, want_out_w};
      }

      return true;
    } catch (const Ort::Exception& e) {
      if (error_out) *error_out = std::string("ONNX Runtime error: ") + e.what();
      return false;
    } catch (const std::exception& e) {
      if (error_out) *error_out = std::string("OpenCudaMattingSession error: ") + e.what();
      return false;
    }
  }
#endif
};

OpenCudaMattingSession::OpenCudaMattingSession(studiocast::maxine::CudaDriverApi* cuda, ModelPack pack, Options opts)
    : impl_(std::make_unique<Impl>()) {
  impl_->cuda = cuda;
  impl_->pack = std::move(pack);
  impl_->opts = opts;
}

OpenCudaMattingSession::OpenCudaMattingSession(studiocast::maxine::CudaDriverApi* cuda, ModelPack pack)
    : OpenCudaMattingSession(cuda, std::move(pack), Options{}) {}

OpenCudaMattingSession::~OpenCudaMattingSession() = default;

const ModelPack& OpenCudaMattingSession::pack() const { return impl_->pack; }

bool OpenCudaMattingSession::EnsureInitialized(int frame_w, int frame_h, std::string* error_out) {
  if (error_out) error_out->clear();

  impl_->last_frame_w = frame_w;
  impl_->last_frame_h = frame_h;

  if (!impl_->cuda || !impl_->cuda->IsInitialized()) {
    if (error_out) *error_out = "OpenCudaMattingSession: CUDA driver API not initialized.";
    return false;
  }
  if (frame_w <= 0 || frame_h <= 0) {
    if (error_out) *error_out = "OpenCudaMattingSession: invalid frame size.";
    return false;
  }

  std::string err;
  if (!impl_->cuda->EnsureContext(&err)) {
    if (error_out) *error_out = std::string("OpenCudaMattingSession: failed to ensure CUDA context: ") + err;
    return false;
  }

  if (!impl_->validated_pack) {
    if (impl_->pack.task != "matting") {
      if (error_out) *error_out = "OpenCudaMattingSession: model pack task must be 'matting'.";
      return false;
    }
    if (impl_->pack.input.layout != "nchw") {
      if (error_out) *error_out = "OpenCudaMattingSession: only NCHW models are supported in v1.";
      return false;
    }
    if (impl_->pack.input.dtype != "float32") {
      if (error_out) *error_out = "OpenCudaMattingSession: only float32 input models are supported in v1.";
      return false;
    }
    if (impl_->pack.output.dtype != "float32") {
      if (error_out) *error_out = "OpenCudaMattingSession: only float32 output models are supported in v1.";
      return false;
    }
    if (impl_->pack.input.channels != 3) {
      if (error_out) *error_out = "OpenCudaMattingSession: model input must have 3 channels.";
      return false;
    }
    if (impl_->pack.input.width <= 0 || impl_->pack.input.height <= 0) {
      if (error_out) *error_out = "OpenCudaMattingSession: invalid model input size.";
      return false;
    }
    if (!IsFinite3(impl_->pack.preprocess.mean) || !IsFinite3(impl_->pack.preprocess.std)) {
      if (error_out) *error_out = "OpenCudaMattingSession: preprocess mean/std must be finite.";
      return false;
    }
    if (impl_->pack.preprocess.std[0] == 0.0 || impl_->pack.preprocess.std[1] == 0.0 || impl_->pack.preprocess.std[2] == 0.0) {
      if (error_out) *error_out = "OpenCudaMattingSession: preprocess std must be non-zero.";
      return false;
    }
    if (impl_->pack.preprocess.color != "rgb" || impl_->pack.preprocess.range != "0..1") {
      if (error_out) *error_out = "OpenCudaMattingSession: unsupported preprocess spec (expected rgb + 0..1).";
      return false;
    }
    if (!FileExists(impl_->pack.onnx_path)) {
      if (error_out) *error_out = "OpenCudaMattingSession: missing ONNX file at " + impl_->pack.onnx_path.string();
      return false;
    }

    impl_->preprocess.dst_w = impl_->pack.input.width;
    impl_->preprocess.dst_h = impl_->pack.input.height;
    for (std::size_t i = 0; i < 3; ++i) {
      impl_->preprocess.mean[i] = static_cast<float>(impl_->pack.preprocess.mean[i]);
      impl_->preprocess.std[i] = static_cast<float>(impl_->pack.preprocess.std[i]);
    }
    impl_->preprocess.dst_order = studiocast::cuda::kernels::ChannelOrder::rgb;

    impl_->validated_pack = true;
  }

  // Allocate persistent input tensor.
  {
    std::string alloc_err;
    if (!impl_->input_tensor.ReallocIfNeededNchwF32(impl_->cuda,
                                                    /*n_in=*/1,
                                                    /*c_in=*/3,
                                                    /*h_in=*/impl_->pack.input.height,
                                                    /*w_in=*/impl_->pack.input.width,
                                                    &alloc_err)) {
      if (error_out) *error_out = std::string("OpenCudaMattingSession: failed to allocate input tensor: ") + alloc_err;
      return false;
    }
  }

  impl_->buffers_ready = true;
  return true;
}

bool OpenCudaMattingSession::Run(studiocast::maxine::CUstream stream,
                                 const studiocast::cuda::CudaImage& input_rgb_gpu,
                                 studiocast::cuda::CudaTensor* output_alpha_gpu,
                                 std::string* error_out) {
  if (error_out) error_out->clear();

  if (!output_alpha_gpu) {
    if (error_out) *error_out = "OpenCudaMattingSession::Run: output_alpha_gpu is null.";
    return false;
  }

  if (!EnsureInitialized(input_rgb_gpu.w, input_rgb_gpu.h, error_out)) {
    return false;
  }

  if (!impl_->buffers_ready) {
    if (error_out) *error_out = "OpenCudaMattingSession::Run: internal buffers not initialized.";
    return false;
  }

  if (!input_rgb_gpu.Valid()) {
    if (error_out) *error_out = "OpenCudaMattingSession::Run: invalid input image.";
    return false;
  }

  // Validate output tensor shape.
  if (output_alpha_gpu->ptr == 0 || !output_alpha_gpu->Valid()) {
    if (error_out) *error_out = "OpenCudaMattingSession::Run: output alpha tensor is not allocated/valid.";
    return false;
  }
  if (output_alpha_gpu->n != 1 || output_alpha_gpu->c != 1 || output_alpha_gpu->h != impl_->pack.input.height ||
      output_alpha_gpu->w != impl_->pack.input.width) {
    if (error_out) {
      *error_out = "OpenCudaMattingSession::Run: output alpha tensor shape mismatch (expected 1x1x" +
                   std::to_string(impl_->pack.input.height) + "x" + std::to_string(impl_->pack.input.width) + ")";
    }
    return false;
  }

  // Preprocess RGB/BGR u8 image to NCHW f32 model input.
  {
    std::string pp_err;
    if (!studiocast::cuda::kernels::PreprocessToTensor(input_rgb_gpu, impl_->input_tensor, impl_->preprocess, stream,
                                                       &pp_err)) {
      if (error_out) *error_out = std::string("OpenCudaMattingSession::Run: preprocess failed: ") + pp_err;
      return false;
    }
  }

#if !STUDIOCAST_HAVE_ONNXRUNTIME
  (void)stream;
  if (error_out) *error_out = "OpenCudaMattingSession: built without ONNX Runtime support.";
  return false;
#else
  // Lazily create the ORT session so we can attempt stream interop.
  if (!impl_->session) {
    std::string ort_err;
    if (!impl_->CreateOrtSession(&ort_err)) {
      if (error_out) *error_out = std::move(ort_err);
      return false;
    }
  }

  if (impl_->ort_stream != nullptr && impl_->ort_stream != stream) {
    if (error_out) {
      *error_out =
          "OpenCudaMattingSession::Run: ORT session was created for a different CUDA stream. Create one session per stream.";
    }
    return false;
  }

  // If we could not configure ORT to use the caller stream, synchronize before inference so
  // ORT sees fully-written inputs.
  if (impl_->ort_needs_stream_sync) {
    std::string sync_err;
    if (!impl_->cuda->StreamSynchronize(stream, &sync_err)) {
      if (error_out) *error_out = std::string("OpenCudaMattingSession::Run: StreamSynchronize failed: ") + sync_err;
      return false;
    }
  }

  try {
    // Create (or reuse) the input Ort::Value.
    if (!impl_->input_value.has_value()) {
      auto* in_ptr = reinterpret_cast<float*>(static_cast<std::uintptr_t>(impl_->input_tensor.ptr));
      impl_->input_value.emplace(Ort::Value::CreateTensor<float>(*impl_->cuda_mem_info,
                                                                 in_ptr,
                                                                 impl_->input_tensor.ElementCount(),
                                                                 impl_->input_shape.data(),
                                                                 impl_->input_shape.size()));
    }

    // Create an output Ort::Value over the provided GPU buffer.
    auto* out_ptr = reinterpret_cast<float*>(static_cast<std::uintptr_t>(output_alpha_gpu->ptr));
    Ort::Value out_value = Ort::Value::CreateTensor<float>(*impl_->cuda_mem_info,
                                                           out_ptr,
                                                           output_alpha_gpu->ElementCount(),
                                                           impl_->output_shape.data(),
                                                           impl_->output_shape.size());

    impl_->binding->ClearBoundInputs();
    impl_->binding->ClearBoundOutputs();
    impl_->binding->BindInput(impl_->input_name.c_str(), *impl_->input_value);
    impl_->binding->BindOutput(impl_->output_name.c_str(), out_value);

    Ort::RunOptions ro;
    impl_->session->Run(ro, *impl_->binding);
    return true;
  } catch (const Ort::Exception& e) {
    if (error_out) *error_out = std::string("ONNX Runtime error: ") + e.what();
    return false;
  }
#endif
}

}  // namespace studiocast::open_cuda
