#pragma once

#include <cstdint>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace studiocast::open_audio {

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
};

// Best-effort model I/O description extracted from the session.
struct OrtSessionInfo {
  bool using_cuda = false;

  std::vector<std::string> input_names;
  std::vector<std::string> output_names;

  // Human-friendly strings like: "tensor(float32) shape=[1, -1]".
  std::vector<std::string> input_descriptions;
  std::vector<std::string> output_descriptions;

  // Structured tensor metadata for tool/debugging and engine bindings.
  // Shapes are best-effort (may contain -1 for dynamic dimensions).
  std::vector<std::vector<int64_t>> input_shapes;
  std::vector<int> input_elem_types;
  std::vector<std::vector<int64_t>> output_shapes;
  std::vector<int> output_elem_types;

  // Non-fatal warnings collected during session creation (e.g., CUDA EP unavailable).
  std::vector<std::string> warnings;
};

// Thin wrapper around an ONNX Runtime session used by the Open Audio backend.
//
// This is primarily used to validate that the selected model can be
// loaded by ORT and to expose basic introspection for diagnostics/tools.
class OpenAudioOrtSession {
 public:
  static OrtRuntimeInfo QueryRuntimeInfo();

  // Create an ORT session for the given model.
  //
  // Returns nullptr on failure and fills `error`.
  static std::unique_ptr<OpenAudioOrtSession> Create(const std::filesystem::path& model_path,
                                                     const OrtSessionOptions& opts,
                                                     OrtSessionInfo* info_out,
                                                     std::string* error);

  ~OpenAudioOrtSession();

  OpenAudioOrtSession(const OpenAudioOrtSession&) = delete;
  OpenAudioOrtSession& operator=(const OpenAudioOrtSession&) = delete;

  const OrtSessionInfo& info() const;

  struct OrtRunInput {
    const char* name = nullptr;
    const float* data = nullptr;
    std::size_t num_floats = 0;
    const int64_t* shape = nullptr;
    std::size_t shape_rank = 0;
  };

  struct OrtRunOutput {
    const char* name = nullptr;
    float* data = nullptr;
    std::size_t num_floats = 0;
    const int64_t* shape = nullptr;
    std::size_t shape_rank = 0;
  };

  // Run an ORT session with pre-allocated input/output tensors.
  //
  // All tensors are assumed to be float32 CPU buffers.
  // Returns false on failure and fills `error`.
  bool Run(const OrtRunInput* inputs,
           std::size_t input_count,
           const OrtRunOutput* outputs,
           std::size_t output_count,
           std::string* error);

  // Convenience helper for waveform-style models with a single float tensor
  // input/output. The first input and first output of the underlying ORT
  // session are used.
  //
  // - input: mono samples
  // - samples: number of mono samples
  // - output: caller-provided buffer
  // - output_capacity: maximum number of samples that can be written to output
  // - output_samples: filled with number of valid samples on success
  //
  // Returns false on failure and fills `error`.
  bool Run1D(const float* input,
             std::size_t samples,
             float* output,
             std::size_t output_capacity,
             std::size_t* output_samples,
             std::string* error);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  explicit OpenAudioOrtSession(std::unique_ptr<Impl> impl);
};

}  // namespace studiocast::open_audio
