#pragma once

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
};

// Thin wrapper around an ONNX Runtime session used by the Open Audio backend.
//
// Phase 5: This is primarily used to validate that the selected model can be
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

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  explicit OpenAudioOrtSession(std::unique_ptr<Impl> impl);
};

}  // namespace studiocast::open_audio
