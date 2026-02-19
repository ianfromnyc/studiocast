#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace studiocast::onnx {

// Best-effort ONNX Runtime runtime information.
struct OrtRuntimeInfo {
  std::string version;
  std::vector<std::string> providers;
};

// Options for creating an ONNX Runtime session.
struct OrtSessionOptions {
  // If true, attempt to use CUDA EP and fall back to CPU EP if CUDA EP is not available.
  bool prefer_cuda = true;

  // CUDA device id to use when CUDA EP is available.
  int cuda_device_id = 0;

  // Optional compute stream (CUDA stream) to use when supported by the CUDA EP.
  //
  // When non-null and the build exposes CUDA EP V2 provider options
  // (STUDIOCAST_ORT_HAS_CUDA_EP_V2=1), ORT will enqueue its compute on this stream.
  //
  // When null, ORT may use internal streams and the caller may need to synchronize
  // around calls that produce/consume GPU buffers.
  void* user_compute_stream = nullptr;
};

// Best-effort model I/O description extracted from the session.
struct OrtSessionInfo {
  bool using_cuda = false;

  // If true, the session is using CUDA EP but is not guaranteed to run on the
  // caller's stream (e.g., user_compute_stream unavailable). Callers integrating
  // with an explicit stream must synchronize for correctness.
  bool cuda_needs_stream_sync = false;

  std::vector<std::string> input_names;
  std::vector<std::string> output_names;

  // Human-friendly strings like: "tensor(float32) shape=[1, 3, 320, 320]".
  std::vector<std::string> input_descriptions;
  std::vector<std::string> output_descriptions;

  // Structured tensor metadata.
  std::vector<std::vector<int64_t>> input_shapes;
  std::vector<int> input_elem_types;
  std::vector<std::vector<int64_t>> output_shapes;
  std::vector<int> output_elem_types;

  // Non-fatal warnings collected during session creation.
  std::vector<std::string> warnings;
};

// Returns true if the ORT exception text looks like CUDA VRAM exhaustion.
bool OrtErrorLooksLikeVramOom(const std::string& ort_msg);

// Convert raw ORT errors into actionable, user-facing diagnostics.
std::string HumanizeOrtError(const std::string& ort_msg, const std::filesystem::path& model_path);

// Canonical ONNX Runtime session wrapper for StudioCast.
//
// This is shared across "open_video" and "open_cuda" runtimes so the app can:
//   - Prefer Maxine when available
//   - Otherwise run effects on the GPU (one CPU->GPU upload, one GPU->CPU download)
//   - Fall back to CPU if neither Maxine nor CUDA EP is available
class OrtSession {
 public:
  static OrtRuntimeInfo QueryRuntimeInfo();

  // Create an ORT session for the given model.
  // Returns nullptr on failure and fills `error`.
  static std::unique_ptr<OrtSession> Create(const std::filesystem::path& model_path,
                                            const OrtSessionOptions& opts,
                                            OrtSessionInfo* info_out,
                                            std::string* error);

  ~OrtSession();

  OrtSession(const OrtSession&) = delete;
  OrtSession& operator=(const OrtSession&) = delete;

  const OrtSessionInfo& info() const;

  struct RunInput {
    const char* name = nullptr;
    const float* data = nullptr;
    std::size_t num_floats = 0;
    const int64_t* shape = nullptr;
    std::size_t shape_rank = 0;
  };

  struct RunOutput {
    const char* name = nullptr;
    float* data = nullptr;
    std::size_t num_floats = 0;
    const int64_t* shape = nullptr;
    std::size_t shape_rank = 0;
  };

  // Run with pre-allocated float32 CPU tensors.
  bool RunCpu(const RunInput* inputs,
              std::size_t input_count,
              const RunOutput* outputs,
              std::size_t output_count,
              std::string* error);

  struct CudaBindingInput {
    const char* name = nullptr;
    const float* device_ptr = nullptr;
    std::size_t num_floats = 0;
    const int64_t* shape = nullptr;
    std::size_t shape_rank = 0;
  };

  struct CudaBindingOutput {
    const char* name = nullptr;
    float* device_ptr = nullptr;
    std::size_t num_floats = 0;
    const int64_t* shape = nullptr;
    std::size_t shape_rank = 0;
  };

  // Run with CUDA IoBinding (GPU inputs/outputs).
  //
  // If info().cuda_needs_stream_sync is true, ORT may not use the caller's stream.
  // In that case, callers integrating with an explicit stream must synchronize
  // before/after this call so producer/consumer kernels see consistent data.
  bool RunCudaIoBinding(const CudaBindingInput* inputs,
                        std::size_t input_count,
                        const CudaBindingOutput* outputs,
                        std::size_t output_count,
                        std::string* error);

  // Returns true if the session has latched a fatal ORT failure (e.g., VRAM OOM).
  bool HasLatchedFailure() const;

  // Returns the latched fatal error message (empty if none).
  const std::string& LatchedError() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  explicit OrtSession(std::unique_ptr<Impl> impl);
};

}  // namespace studiocast::onnx
