#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>

#include "core/onnx/ort_session.h"

// NOTE: Deprecated compatibility shim.
// New code should include "core/onnx/ort_session.h" and use studiocast::onnx::OrtSession directly.

namespace studiocast::open_video {

// These types are aliased to the canonical ORT wrapper in core/onnx.
using OrtRuntimeInfo = studiocast::onnx::OrtRuntimeInfo;
using OrtSessionOptions = studiocast::onnx::OrtSessionOptions;
using OrtSessionInfo = studiocast::onnx::OrtSessionInfo;

// Thin wrapper around an ONNX Runtime session used by the Open Video model packs.
//
// This remains as an API shim so existing Open Video effects can continue to depend
// on `core/open_video/open_video_onnx_session.h` while the canonical implementation
// lives in `core/onnx/ort_session.*`.
class OpenVideoOrtSession {
 public:
  static OrtRuntimeInfo QueryRuntimeInfo();

  // Create an ORT session for the given model.
  // Returns nullptr on failure and fills `error`.
  static std::unique_ptr<OpenVideoOrtSession> Create(const std::filesystem::path& model_path,
                                                     const OrtSessionOptions& opts,
                                                     OrtSessionInfo* info_out,
                                                     std::string* error);

  ~OpenVideoOrtSession();

  OpenVideoOrtSession(const OpenVideoOrtSession&) = delete;
  OpenVideoOrtSession& operator=(const OpenVideoOrtSession&) = delete;

  const OrtSessionInfo& info() const;

  using OrtRunInput = studiocast::onnx::OrtSession::RunInput;
  using OrtRunOutput = studiocast::onnx::OrtSession::RunOutput;

  // Run an ORT session with pre-allocated input/output tensors.
  // All tensors are assumed to be float32 CPU buffers.
  bool Run(const OrtRunInput* inputs,
           std::size_t input_count,
           const OrtRunOutput* outputs,
           std::size_t output_count,
           std::string* error);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  explicit OpenVideoOrtSession(std::unique_ptr<Impl> impl);
};

}  // namespace studiocast::open_video
